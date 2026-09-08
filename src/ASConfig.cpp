#include "ASConfig.h"
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include "ASParse.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "ObjectMgr.h"
#include "QueryResult.h"
#include "ScannedItem.h"
#include "Tokenize.h"

namespace
{
    struct ItemClassConfigKey
    {
        uint32 itemClass;
        char const* configKey;
    };

    constexpr std::array<ItemClassConfigKey, MAX_ITEM_CLASS> kItemClassConfigKeys = {{
        {ITEM_CLASS_CONSUMABLE, "AuctionSim.ConsumablePercent"},
        {ITEM_CLASS_CONTAINER, "AuctionSim.ContainerPercent"},
        {ITEM_CLASS_WEAPON, "AuctionSim.WeaponPercent"},
        {ITEM_CLASS_GEM, "AuctionSim.GemPercent"},
        {ITEM_CLASS_ARMOR, "AuctionSim.ArmorPercent"},
        {ITEM_CLASS_REAGENT, "AuctionSim.ReagentPercent"},
        {ITEM_CLASS_PROJECTILE, "AuctionSim.ProjectilePercent"},
        {ITEM_CLASS_TRADE_GOODS, "AuctionSim.TradeGoodsPercent"},
        {ITEM_CLASS_GENERIC, "AuctionSim.GenericPercent"},
        {ITEM_CLASS_RECIPE, "AuctionSim.RecipePercent"},
        {ITEM_CLASS_MONEY, "AuctionSim.MoneyPercent"},
        {ITEM_CLASS_QUIVER, "AuctionSim.QuiverPercent"},
        {ITEM_CLASS_QUEST, "AuctionSim.QuestPercent"},
        {ITEM_CLASS_KEY, "AuctionSim.KeyPercent"},
        {ITEM_CLASS_PERMANENT, "AuctionSim.PermanentPercent"},
        {ITEM_CLASS_MISC, "AuctionSim.MiscPercent"},
        {ITEM_CLASS_GLYPH, "AuctionSim.GlyphPercent"},
    }};

    struct QualityToken
    {
        uint32 quality;
        std::string_view label;
    };

    constexpr std::array<QualityToken, 7> kQualityTokens = {{
        {ITEM_QUALITY_POOR, "GREY"},
        {ITEM_QUALITY_NORMAL, "WHITE"},
        {ITEM_QUALITY_UNCOMMON, "GREEN"},
        {ITEM_QUALITY_RARE, "BLUE"},
        {ITEM_QUALITY_EPIC, "PURPLE"},
        {ITEM_QUALITY_LEGENDARY, "ORANGE"},
        {ITEM_QUALITY_ARTIFACT, "YELLOW"},
    }};
}

ASConfig::ASConfig(std::string const& filepath, bool& outLoaded)
{
    this->maxRequiredLevel = sConfigMgr->GetOption<uint32>("AuctionSim.MaxRequiredLevel", 0);
    this->maxItemLevel = sConfigMgr->GetOption<uint32>("AuctionSim.MaxItemLevel", 0);

    // Independent of auctionsim.dat -- load these even on the early-return paths
    // below so the buy-side guard and the Neutral gate always have their data.
    LoadVendorItems();
    LoadNeutralConfig();
    LoadItemExceptions();

    if (!std::filesystem::exists(filepath))
    {
        LOG_ERROR("module", "AuctionSim: {} not found", filepath);
        outLoaded = false;
        return;
    }

    std::ifstream stream(filepath, std::ios::in);
    if (!stream.is_open())
    {
        LOG_ERROR("module", "AuctionSim: Couldn't open {}", filepath);
        outLoaded = false;
        return;
    }

    std::string line;
    if (!std::getline(stream, line))
    {
        LOG_ERROR("module", "AuctionSim: {} is empty", filepath);
        outLoaded = false;
        return;
    }

    // Line 1 is "N M": N item rows, preceded by M category-depth rows.
    size_t declaredItemRows = 0;
    size_t categoryRows = 0;
    if (!ParseHeaderLine(line, declaredItemRows, categoryRows))
    {
        LOG_ERROR("module", "AuctionSim: {} has a malformed header line '{}'", filepath, line);
        outLoaded = false;
        return;
    }

    for (size_t read = 0; read < categoryRows && std::getline(stream, line); ++read)
    {
        LoadCategoryRow(line, filepath);
    }

    while (std::getline(stream, line))
    {
        LoadItemRow(line, filepath);
    }

    if (this->ScanData.size() > declaredItemRows)
    {
        LOG_ERROR(
            "module",
            "AuctionSim: {} contains more item rows ({}) than its header declared ({}); refusing to load -- "
            "the file looks corrupt",
            filepath,
            this->ScanData.size(),
            declaredItemRows);
        outLoaded = false;
        return;
    }

    BuildSelectionTables(filepath);

    // Derive auctionsim_overrides.dat from auctionsim.dat's own path so it lives
    // alongside it without a separate config key.
    std::filesystem::path datPath(filepath);
    overridesFilePath = (datPath.parent_path() / "auctionsim_overrides.dat").string();
    LoadOverrides();

    SynthesizeNeutralDepth();

    size_t depthProfiles = 0;
    for (auto const& byFaction : this->categoryDepth)
    {
        for (auto const& byClass : byFaction)
        {
            for (CategoryDepth const& depth : byClass)
            {
                if (depth.has)
                {
                    depthProfiles++;
                }
            }
        }
    }

    LOG_INFO(
        "module",
        "AuctionSim: loaded prices for {} items and {} category depth profiles",
        this->ScanData.size(),
        depthProfiles);

    LoadMasks();
}

bool ASConfig::ParseHeaderLine(std::string const& line, size_t& outItemRows, size_t& outCategoryRows)
{
    std::istringstream header(line);
    header >> outItemRows >> outCategoryRows;
    return static_cast<bool>(header) && outItemRows > 0;
}

// One category-depth row: faction:class:quality:snapshotCount followed by a
// 12-value StatBlock. Only q1 / median / adjLow / adjHigh are kept.
void ASConfig::LoadCategoryRow(std::string const& line, std::string const& filepath)
{
    std::vector<std::string_view> f = Acore::Tokenize(line, ':', false);
    if (f.size() != kCategoryRowFields)
    {
        LOG_ERROR("module", "AuctionSim: skipping malformed category row in {}: '{}'", filepath, line);
        return;
    }

    uint32 faction = 0;
    uint32 itemClass = 0;
    uint32 quality = 0;
    StatBlock stats;
    if (!ASParse::Integer(f[0], faction) || !ASParse::Integer(f[1], itemClass) ||
        !ASParse::Integer(f[2], quality) || !ParseStatBlock(f, ScannedItem::kIdentityFields, stats))
    {
        LOG_ERROR("module", "AuctionSim: skipping unparseable category row in {}: '{}'", filepath, line);
        return;
    }
    if (faction >= kAuctionHouseIndexBound || itemClass >= MAX_ITEM_CLASS || quality >= MAX_ITEM_QUALITY)
    {
        LOG_ERROR("module", "AuctionSim: category row out of range in {}: '{}'", filepath, line);
        return;
    }

    CategoryDepth& depth = this->categoryDepth[faction][itemClass][quality];
    depth.has = true;
    depth.q1 = stats.q1;
    depth.median = stats.median;
    depth.adjLow = stats.adjLow;
    depth.adjHigh = stats.adjHigh;
}

void ASConfig::LoadItemRow(std::string const& line, std::string const& filepath)
{
    if (auto item = ScannedItem::TryParse(line))
    {
        this->ScanData.push_back(*item);
    }
    else
    {
        LOG_ERROR("module", "AuctionSim: skipping malformed line in {}: '{}'", filepath, line);
    }
}

// Resolves each row's item_template once and files it into ItemSelectionTable and
// ItemIndex. ScanData is a deque, so the pointers taken here stay valid.
void ASConfig::BuildSelectionTables(std::string const& filepath)
{
    for (ScannedItem& item : this->ScanData)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item.GetItemID());
        if (!proto)
        {
            LOG_WARN(
                "module",
                "AuctionSim: item {} in {} not found in item_template, skipping",
                item.GetItemID(),
                filepath);
            continue;
        }

        size_t house = item.GetFactionNum();
        if (house >= kAuctionHouseIndexBound || proto->Class >= MAX_ITEM_CLASS || proto->Quality >= MAX_ITEM_QUALITY)
        {
            continue;
        }

        this->ItemSelectionTable[house][proto->Class][proto->Quality].push_back(&item);
        this->ItemIndex.try_emplace(IndexKey(house, proto->Class, proto->Quality, item.GetItemID()), &item);
        this->ByItemId.try_emplace(item.GetItemID(), &item);

        // Neutral-eligible items list on the Neutral bucket IN ADDITION TO their
        // native faction bucket (not instead of) -- they're still buyable on their
        // own faction's AH; the neutral listing exists so the opposing faction has
        // a legitimate way to get them too.
        if (enableNeutralAH && IsNeutralEligible(item.GetItemID()))
        {
            size_t neutralHouse = static_cast<size_t>(AuctionHouseId::Neutral);
            this->ItemSelectionTable[neutralHouse][proto->Class][proto->Quality].push_back(&item);
            this->ItemIndex.try_emplace(
                IndexKey(neutralHouse, proto->Class, proto->Quality, item.GetItemID()), &item);
        }
    }
}

// AuctionSim.EnableNeutralAH toggles whether the Neutral bucket is used at all.
// AuctionSim.NeutralItems is a comma-separated item_template id list (curated by
// the operator -- see header comment on neutralEligibleItems for why this isn't
// auto-detected from item flags).
void ASConfig::LoadNeutralConfig()
{
    enableNeutralAH = sConfigMgr->GetOption<bool>("AuctionSim.EnableNeutralAH", false);

    uint32 minutes = sConfigMgr->GetOption<uint32>("AuctionSim.ScanIntervalMinutes", 60);
    if (minutes < 5)
    {
        LOG_ERROR(
            "module",
            "AuctionSim: ScanIntervalMinutes {} is below the 5-minute floor, clamping to 5",
            minutes);
        minutes = 5;
    }
    scanIntervalSeconds = minutes * 60;

    neutralEligibleItems.clear();
    std::string raw = sConfigMgr->GetOption<std::string>("AuctionSim.NeutralItems", "");
    for (std::string_view tok : Acore::Tokenize(raw, ',', false))
    {
        uint32 itemId = 0;
        if (ASParse::Integer(tok, itemId) && itemId != 0)
        {
            neutralEligibleItems.insert(itemId);
        }
    }

    LOG_INFO(
        "module",
        "AuctionSim: Neutral AH {} ({} item(s) configured)",
        enableNeutralAH ? "enabled" : "disabled",
        neutralEligibleItems.size());
}

// AuctionSim.ItemExceptions is a comma-separated "itemId:bitmask" list, e.g.
// "6661:7,12345:2". Bit 1/2/4 = Alliance/Horde/Neutral (add bits together to
// exclude from more than one house); a bad or zero bitmask, a bad item id, or a
// malformed pair is logged and skipped rather than aborting the whole line, same
// as every other loader here.
void ASConfig::LoadItemExceptions()
{
    itemHouseExceptions.clear();

    std::string raw = sConfigMgr->GetOption<std::string>("AuctionSim.ItemExceptions", "");
    for (std::string_view pair : Acore::Tokenize(raw, ',', false))
    {
        std::vector<std::string_view> parts = Acore::Tokenize(pair, ':', false);
        uint32 itemId = 0;
        uint32 mask = 0;
        if (parts.size() != 2 || !ASParse::Integer(parts[0], itemId) || itemId == 0 ||
            !ASParse::Integer(parts[1], mask) || mask == 0 || mask > 7)
        {
            LOG_ERROR("module", "AuctionSim: skipping malformed AuctionSim.ItemExceptions entry '{}'", pair);
            continue;
        }
        itemHouseExceptions[itemId] = static_cast<uint8>(mask);
    }

    LOG_INFO("module", "AuctionSim: {} item auction-house exception(s) configured", itemHouseExceptions.size());
}

// Maps houseId to its bit (Alliance=1, Horde=2, Neutral=4) and checks it against
// the item's configured mask, if any. Any houseId this module doesn't otherwise
// use (there are none today, but AuctionHouseId is the core's enum, not ours)
// simply never matches and the item is never excluded.
bool ASConfig::IsItemExcludedFromHouse(uint32 itemId, AuctionHouseId houseId) const
{
    auto it = itemHouseExceptions.find(itemId);
    if (it == itemHouseExceptions.end())
    {
        return false;
    }

    uint8 bit = 0;
    switch (houseId)
    {
        case AuctionHouseId::Alliance: bit = 1; break;
        case AuctionHouseId::Horde: bit = 2; break;
        case AuctionHouseId::Neutral: bit = 4; break;
        default: return false;
    }
    return (it->second & bit) != 0;
}

// auctionsim.dat only ever scans the Alliance/Horde houses, so the Neutral
// bucket never gets a real CategoryDepth profile from LoadCategoryRow -- and
// ListNewAuctions skips any category where depth.has is false. Since neutral
// items still list normally on their native house (see BuildSelectionTables),
// the Neutral house's target depth is a scaled-down blend of whatever depth
// its native house(s) observed for that category, so it fills in proportion
// to real demand rather than not filling at all. AuctionSim.NeutralDepthScale
// (default 0.5) controls how aggressively -- keep it below 1 so the neutral
// listings stay a supplemental channel, not a full duplicate market.
void ASConfig::SynthesizeNeutralDepth()
{
    if (!enableNeutralAH)
    {
        return;
    }

    float scale = sConfigMgr->GetOption<float>("AuctionSim.NeutralDepthScale", 0.5f);
    size_t neutralHouse = static_cast<size_t>(AuctionHouseId::Neutral);
    size_t allianceHouse = static_cast<size_t>(AuctionHouseId::Alliance);
    size_t hordeHouse = static_cast<size_t>(AuctionHouseId::Horde);

    for (uint32 itemClass = 0; itemClass < MAX_ITEM_CLASS; ++itemClass)
    {
        for (uint32 quality = 0; quality < MAX_ITEM_QUALITY; ++quality)
        {
            if (this->ItemSelectionTable[neutralHouse][itemClass][quality].empty())
            {
                continue;  // nothing neutral-eligible in this category -- no profile needed
            }

            CategoryDepth const& a = this->categoryDepth[allianceHouse][itemClass][quality];
            CategoryDepth const& h = this->categoryDepth[hordeHouse][itemClass][quality];
            if (!a.has && !h.has)
            {
                continue;  // neither native house has ever seen this category either
            }

            // Blend whichever side(s) have data; average when both do.
            uint32 divisor = (a.has ? 1u : 0u) + (h.has ? 1u : 0u);
            auto blend = [&](uint32 CategoryDepth::* field) -> uint32 {
                uint32 sum = (a.has ? a.*field : 0) + (h.has ? h.*field : 0);
                return static_cast<uint32>((static_cast<float>(sum) / divisor) * scale + 0.5f);
            };

            CategoryDepth& depth = this->categoryDepth[neutralHouse][itemClass][quality];
            depth.has = true;
            depth.q1 = blend(&CategoryDepth::q1);
            depth.median = blend(&CategoryDepth::median);
            depth.adjLow = blend(&CategoryDepth::adjLow);
            depth.adjHigh = blend(&CategoryDepth::adjHigh);
        }
    }
}

void ASConfig::LoadMasks()
{
    for (auto const& entry : kItemClassConfigKeys)
    {
        UnpackQualityString(sConfigMgr->GetOption<std::string>(entry.configKey, ""), entry.itemClass);
    }
}

// Every item id stocked by a vendor. Negative npc_vendor.item rows are references
// to other npc_vendor rows, whose own positive item ids this DISTINCT scan already
// covers, so one query is enough.
void ASConfig::LoadVendorItems()
{
    QueryResult result = WorldDatabase.Query("SELECT DISTINCT item FROM npc_vendor WHERE item > 0");
    if (!result)
    {
        return;
    }

    do
    {
        vendorSoldItems.insert(result->Fetch()[0].Get<uint32>());
    } while (result->NextRow());

    LOG_INFO("module", "AuctionSim: {} distinct vendor-sold items loaded", vendorSoldItems.size());
}

uint64_t ASConfig::IndexKey(size_t house, uint32 itemClass, uint32 quality, uint32 itemID)
{
    return (static_cast<uint64_t>(house) << 56) | (static_cast<uint64_t>(itemClass) << 48) |
           (static_cast<uint64_t>(quality) << 40) | static_cast<uint64_t>(itemID);
}

std::vector<ScannedItem*> const& ASConfig::ItemsFor(AuctionHouseId houseId, uint32 itemClass, uint32 quality) const
{
    return ItemSelectionTable[static_cast<size_t>(houseId)][itemClass][quality];
}

ASConfig::CategoryDepth const& ASConfig::GetCategoryDepth(
    AuctionHouseId houseId, uint32 itemClass, uint32 quality) const
{
    static CategoryDepth const kEmpty{};

    size_t house = static_cast<size_t>(houseId);
    if (house >= kAuctionHouseIndexBound || itemClass >= MAX_ITEM_CLASS || quality >= MAX_ITEM_QUALITY)
    {
        return kEmpty;
    }
    return categoryDepth[house][itemClass][quality];
}

ScannedItem const* ASConfig::FindScannedItem(
    AuctionHouseId houseId, uint32 itemClass, uint32 quality, uint32 itemID) const
{
    size_t house = static_cast<size_t>(houseId);
    if (house >= kAuctionHouseIndexBound || itemClass >= MAX_ITEM_CLASS || quality >= MAX_ITEM_QUALITY)
    {
        return nullptr;
    }

    auto it = ItemIndex.find(IndexKey(house, itemClass, quality, itemID));
    return it != ItemIndex.end() ? it->second : nullptr;
}

namespace
{
    // "AuctionSim.ConsumablePercent" -> "ConsumablePercent"
    std::string_view StripConfigPrefix(std::string_view configKey)
    {
        auto dotPos = configKey.find('.');
        return dotPos == std::string_view::npos ? configKey : configKey.substr(dotPos + 1);
    }
}

bool ASConfig::ResolveMaskKey(std::string_view key, uint32& outItemClass, uint32& outQuality)
{
    auto dotPos = key.find('.');
    if (dotPos == std::string_view::npos)
    {
        return false;
    }

    std::string_view percentConfigKey = key.substr(0, dotPos);
    std::string_view qualityLabel = key.substr(dotPos + 1);

    for (auto const& entry : kItemClassConfigKeys)
    {
        if (StripConfigPrefix(entry.configKey) != percentConfigKey)
        {
            continue;
        }
        for (auto const& token : kQualityTokens)
        {
            if (token.label == qualityLabel)
            {
                outItemClass = entry.itemClass;
                outQuality = token.quality;
                return true;
            }
        }
        return false;
    }
    return false;
}

std::vector<ASConfig::MaskKeyEntry> const& ASConfig::AllMaskKeys()
{
    static std::vector<MaskKeyEntry> keys = [] {
        std::vector<MaskKeyEntry> result;
        result.reserve(kItemClassConfigKeys.size() * kQualityTokens.size());
        for (auto const& entry : kItemClassConfigKeys)
        {
            for (auto const& token : kQualityTokens)
            {
                result.push_back({StripConfigPrefix(entry.configKey), token.label, entry.itemClass, token.quality});
            }
        }
        return result;
    }();
    return keys;
}

void ASConfig::UnpackQualityString(std::string_view qualityString, int itemClass)
{
    for (auto const& token : kQualityTokens)
    {
        std::string prefix = std::string(token.label) + ": ";
        auto pos = qualityString.find(prefix);
        if (pos == std::string_view::npos)
        {
            LOG_ERROR(
                "module",
                "AuctionSim: missing '{}' entry in multiplier string for item class {}, defaulting to 0",
                token.label,
                itemClass);
            this->ItemSelectionMask[itemClass][token.quality] = 0.0f;
            continue;
        }

        // The value runs from just after "<LABEL>: " to the next comma (or the end
        // of the string for the last entry); trim any surrounding whitespace.
        size_t valueStart = pos + prefix.size();
        size_t commaPos = qualityString.find(',', valueStart);
        std::string_view valueStr = qualityString.substr(
            valueStart, commaPos == std::string_view::npos ? std::string_view::npos : commaPos - valueStart);
        while (!valueStr.empty() && (valueStr.front() == ' ' || valueStr.front() == '\t'))
        {
            valueStr.remove_prefix(1);
        }
        while (!valueStr.empty() &&
               (valueStr.back() == ' ' || valueStr.back() == '\t' || valueStr.back() == '\r'))
        {
            valueStr.remove_suffix(1);
        }

        float value = 0.0f;
        if (!ASParse::Float(valueStr, value) || value < 0.0f)
        {
            LOG_ERROR(
                "module",
                "AuctionSim: malformed multiplier '{}' for '{}' in item class {}, defaulting to 0",
                valueStr,
                token.label,
                itemClass);
            value = 0.0f;
        }

        this->ItemSelectionMask[itemClass][token.quality] = value;
    }

    this->ItemSelectionMask[itemClass][ITEM_QUALITY_HEIRLOOM] = 0.0f;
}

ScannedItem const* ASConfig::FindAnyScan(uint32 itemId) const
{
    auto it = ByItemId.find(itemId);
    return it != ByItemId.end() ? it->second : nullptr;
}

// Reads auctionsim_overrides.dat (if present -- fine if it isn't, no GM has
// priced anything manually yet) and files every row exactly like a real scan.
void ASConfig::LoadOverrides()
{
    std::error_code ec;
    if (!std::filesystem::exists(overridesFilePath, ec))
    {
        return;
    }

    std::ifstream stream(overridesFilePath, std::ios::in);
    if (!stream.is_open())
    {
        LOG_ERROR("module", "AuctionSim: couldn't open {}", overridesFilePath);
        return;
    }

    std::string line;
    size_t loaded = 0;
    while (std::getline(stream, line))
    {
        if (line.empty() || line[0] == '#')
        {
            continue;
        }

        std::vector<std::string_view> f = Acore::Tokenize(line, ':', false);
        // itemId:faction:marketPrice:listLow:listHigh:typicalStack:stackLow:stackHigh:neutralEligible
        if (f.size() != 9)
        {
            LOG_ERROR("module", "AuctionSim: skipping malformed override row: '{}'", line);
            continue;
        }

        uint32 itemId = 0, faction = 0, marketPrice = 0, listLow = 0, listHigh = 0;
        uint32 typicalStack = 0, stackLow = 0, stackHigh = 0, neutralFlag = 0;
        if (!ASParse::Integer(f[0], itemId) || !ASParse::Integer(f[1], faction) ||
            !ASParse::Integer(f[2], marketPrice) || !ASParse::Integer(f[3], listLow) ||
            !ASParse::Integer(f[4], listHigh) || !ASParse::Integer(f[5], typicalStack) ||
            !ASParse::Integer(f[6], stackLow) || !ASParse::Integer(f[7], stackHigh) ||
            !ASParse::Integer(f[8], neutralFlag) || faction > UINT8_MAX)
        {
            LOG_ERROR("module", "AuctionSim: skipping unparseable override row: '{}'", line);
            continue;
        }

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        if (!proto)
        {
            LOG_WARN("module", "AuctionSim: override for item {} has no item_template, skipping", itemId);
            continue;
        }

        ScanData.push_back(ScannedItem::FromOverride(
            static_cast<uint8>(faction), itemId, marketPrice, listLow, listHigh, typicalStack, stackLow, stackHigh));
        ScannedItem& row = ScanData.back();

        size_t house = faction;
        if (house < kAuctionHouseIndexBound)
        {
            ItemSelectionTable[house][proto->Class][proto->Quality].push_back(&row);
            ItemIndex.try_emplace(IndexKey(house, proto->Class, proto->Quality, itemId), &row);
        }
        ByItemId.insert_or_assign(itemId, &row);

        if (neutralFlag != 0)
        {
            neutralEligibleItems.insert(itemId);
            size_t neutralHouse = static_cast<size_t>(AuctionHouseId::Neutral);
            ItemSelectionTable[neutralHouse][proto->Class][proto->Quality].push_back(&row);
            ItemIndex.try_emplace(IndexKey(neutralHouse, proto->Class, proto->Quality, itemId), &row);
        }

        loaded++;
    }

    if (loaded > 0)
    {
        LOG_INFO("module", "AuctionSim: loaded {} price override(s) from {}", loaded, overridesFilePath);
    }
}

bool ASConfig::WriteOverridesFile() const
{
    std::string tempPath = overridesFilePath + ".tmp";
    {
        std::ofstream stream(tempPath, std::ios::out | std::ios::trunc);
        if (!stream.is_open())
        {
            LOG_ERROR("module", "AuctionSim: couldn't open {} for writing", tempPath);
            return false;
        }
        stream << "# GM-entered item prices from the ahsim addon's item pricer.\n";
        stream << "# itemId:faction:marketPrice:listLow:listHigh:typicalStack:stackLow:stackHigh:neutralEligible\n";
        for (ScannedItem const& item : ScanData)
        {
            if (!item.IsOverride())
            {
                continue;
            }
            stream << item.GetItemID() << ":" << static_cast<uint32>(item.GetFactionNum()) << ":"
                   << item.GetMarketPrice() << ":" << item.GetListLow() << ":" << item.GetListHigh() << ":"
                   << item.GetTypicalStackSize() << ":" << item.GetStackLow() << ":" << item.GetStackHigh() << ":"
                   << (neutralEligibleItems.count(item.GetItemID()) ? 1 : 0) << "\n";
        }
    }

    std::error_code ec;
    std::filesystem::rename(tempPath, overridesFilePath, ec);
    if (ec)
    {
        LOG_ERROR("module", "AuctionSim: couldn't replace {}: {}", overridesFilePath, ec.message());
        return false;
    }
    return true;
}

// Guesses this item's native faction bucket from its allowable-race mask so a
// class/race-locked item (e.g. a Horde-only recipe) doesn't get listed on the
// wrong faction's house. An item usable by both/neither (the common case --
// most craftables, consumables, trade goods) lists on both, same as a real
// item with no race restriction would naturally show up on either scan.
// This fork (mod-playerbots/Grimfeather branch) does NOT define RACEMASK_ALLIANCE/
// RACEMASK_HORDE macros in SharedDefines.h (those only exist in mainline azerothcore --
// confirmed by checking this exact fork's source), so the masks are built here from
// the RACE_* enum values instead. Bit convention matches ItemTemplate::AllowableRace
// itself: bit (raceId - 1) per race.
namespace
{
    constexpr uint32 kAllianceRaceMask =
        (1 << (RACE_HUMAN - 1)) | (1 << (RACE_DWARF - 1)) | (1 << (RACE_NIGHTELF - 1)) |
        (1 << (RACE_GNOME - 1)) | (1 << (RACE_DRAENEI - 1));
    constexpr uint32 kHordeRaceMask =
        (1 << (RACE_ORC - 1)) | (1 << (RACE_UNDEAD_PLAYER - 1)) | (1 << (RACE_TAUREN - 1)) |
        (1 << (RACE_TROLL - 1)) | (1 << (RACE_BLOODELF - 1));
}

bool ASConfig::UpsertOverride(
    uint32 itemId,
    uint32 marketPrice,
    uint32 listLow,
    uint32 listHigh,
    uint32 typicalStack,
    uint32 stackLow,
    uint32 stackHigh,
    bool neutralEligible)
{
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
    if (!proto)
    {
        return false;
    }

    bool allianceUsable = proto->AllowableRace == 0 || (proto->AllowableRace & kAllianceRaceMask) != 0;
    bool hordeUsable = proto->AllowableRace == 0 || (proto->AllowableRace & kHordeRaceMask) != 0;
    // Race-locked to exactly one side -- list only there; everyone else lists both,
    // same rationale as the comment above.
    if ((proto->AllowableRace & kAllianceRaceMask) != 0 && (proto->AllowableRace & kHordeRaceMask) == 0)
    {
        hordeUsable = false;
    }
    else if ((proto->AllowableRace & kHordeRaceMask) != 0 && (proto->AllowableRace & kAllianceRaceMask) == 0)
    {
        allianceUsable = false;
    }

    // Remove any prior override row(s) for this item before re-filing -- an edit
    // replaces, it doesn't stack duplicate rows into the selection tables.
    for (auto& row : ScanData)
    {
        if (row.IsOverride() && row.GetItemID() == itemId)
        {
            // Row can't be erased from the deque without invalidating pointers
            // other rows depend on; instead its bucket entries are removed below
            // and it's left as an orphaned, never-selected row (harmless: nothing
            // still points at it once ItemSelectionTable/ItemIndex are cleaned).
            for (auto& classBuckets : ItemSelectionTable)
            {
                for (auto& qualityBuckets : classBuckets)
                {
                    for (auto& bucket : qualityBuckets)
                    {
                        bucket.erase(std::remove(bucket.begin(), bucket.end(), &row), bucket.end());
                    }
                }
            }
        }
    }

    std::vector<std::pair<uint8, bool>> housesToFile;
    if (allianceUsable)
    {
        housesToFile.emplace_back(static_cast<uint8>(AuctionHouseId::Alliance), false);
    }
    if (hordeUsable)
    {
        housesToFile.emplace_back(static_cast<uint8>(AuctionHouseId::Horde), false);
    }
    if (housesToFile.empty())
    {
        // Shouldn't happen given the logic above, but never silently drop the item.
        housesToFile.emplace_back(static_cast<uint8>(AuctionHouseId::Alliance), false);
    }

    for (auto const& [faction, unused] : housesToFile)
    {
        (void)unused;
        ScanData.push_back(ScannedItem::FromOverride(
            faction, itemId, marketPrice, listLow, listHigh, typicalStack, stackLow, stackHigh));
        ScannedItem& row = ScanData.back();
        size_t house = faction;
        ItemSelectionTable[house][proto->Class][proto->Quality].push_back(&row);
        ItemIndex.insert_or_assign(IndexKey(house, proto->Class, proto->Quality, itemId), &row);
        ByItemId.insert_or_assign(itemId, &row);

        if (neutralEligible)
        {
            size_t neutralHouse = static_cast<size_t>(AuctionHouseId::Neutral);
            ItemSelectionTable[neutralHouse][proto->Class][proto->Quality].push_back(&row);
            ItemIndex.insert_or_assign(IndexKey(neutralHouse, proto->Class, proto->Quality, itemId), &row);
        }
    }

    if (neutralEligible)
    {
        neutralEligibleItems.insert(itemId);
    }
    else
    {
        neutralEligibleItems.erase(itemId);
    }

    return WriteOverridesFile();
}
