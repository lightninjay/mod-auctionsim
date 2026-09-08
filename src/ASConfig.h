#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "AuctionHouseMgr.h"
#include "ItemTemplate.h"
#include "ScannedItem.h"

class ASConfig
{
public:
    // Sized to allow indexing directly by an AuctionHouseId's raw numeric value
    // (Alliance=2, Horde=6, Neutral=7) without remapping to a dense range; must be
    // at least Neutral+1=8. auctionsim.dat itself only ever contains faction values
    // 2 and 6 -- the Neutral(7) bucket is populated separately by
    // BuildNeutralSelectionTable from items marked neutral-eligible (see
    // IsNeutralEligible / AuctionSim.NeutralItems), not from scanned rows.
    static constexpr size_t kAuctionHouseIndexBound = 8;

    // A category-depth header row is faction:class:quality:snapshotCount followed
    // by one 12-value StatBlock -- 16 fields.
    static constexpr size_t kCategoryRowFields = ScannedItem::kIdentityFields + ScannedItem::kStatsPerBlock;

    ASConfig(std::string const& filepath, bool& outLoaded);

    // Level caps for newly-listed items; 0 disables the respective check. Read once
    // from AuctionSim.MaxRequiredLevel / AuctionSim.MaxItemLevel at construction.
    uint32 maxRequiredLevel = 0;
    uint32 maxItemLevel = 0;

    // Every item id stocked by at least one vendor (from npc_vendor). The buy-side
    // vendor-buy-price guard only applies to items in this set: an
    // ItemTemplate::BuyPrice left on an item that no vendor actually sells is stale
    // DB data and must not block an otherwise-good purchase.
    std::unordered_set<uint32> vendorSoldItems;
    bool IsVendorSold(uint32 itemId) const { return vendorSoldItems.count(itemId) > 0; }

    // Whether the Neutral (goblin) auction houses are populated at all. From
    // AuctionSim.EnableNeutralAH. When false, kAuctionHouseIndexBound's Neutral
    // slot is simply never filled or scanned.
    bool enableNeutralAH = false;

    // How often the module rescans the auction houses, in seconds. From
    // AuctionSim.ScanIntervalMinutes (minutes in the conf file, seconds here --
    // matches AuctionPricing::CalculateRemainingScans' unit). Floored at 5 minutes:
    // shorter than that risks the bot fighting itself (buy/list decisions racing a
    // scan pass still being processed) and hammering the AH tables unnecessarily.
    uint32 scanIntervalSeconds = 3600;

    // Explicit item-id allowlist for the Neutral bucket, from
    // AuctionSim.NeutralItems (comma-separated item template ids in
    // auctionsim.conf) -- curated by the server owner rather than inferred, since
    // ItemTemplate's faction/race masks are not a reliable signal of "only
    // obtainable via the opposing faction" in 3.3.5a data. An item on this list
    // still lists normally on its native Alliance/Horde house AND is additionally
    // listed on the Neutral house, so the opposing faction has a legitimate way to
    // buy it too -- it is never removed from its native bucket.
    std::unordered_set<uint32> neutralEligibleItems;
    bool IsNeutralEligible(uint32 itemId) const { return neutralEligibleItems.count(itemId) > 0; }

    // Explicit per-item, per-house auction house exceptions, from
    // AuctionSim.ItemExceptions (comma-separated "itemId:bitmask" pairs in
    // auctionsim.conf, e.g. "6661:7" for Savory Deviate Delight Recipe banned
    // everywhere). Bit 1 = Alliance, bit 2 = Horde, bit 4 = Neutral; bits combine
    // by addition (3 = Alliance+Horde, 7 = all three). This is a manual override
    // list for items the ItemLevel/RequiredLevel caps don't catch -- e.g. a
    // low-level recipe or quest reward that's still undesirable to auto-list --
    // rather than a replacement for those caps. Checked at the same point a
    // level-cap failure is (ListOneItem / CleanOverCapAuctions), so it behaves
    // identically: a banned item is simply never listed on that house, and any
    // of the bot's existing auctions for it there are treated as over-cap.
    std::unordered_map<uint32, uint8> itemHouseExceptions;
    bool IsItemExcludedFromHouse(uint32 itemId, AuctionHouseId houseId) const;

    // ScannedItem storage. A std::deque, not a vector: the ScannedItem* kept in
    // ItemSelectionTable / ItemIndex must stay valid as rows are appended, and a
    // deque never relocates existing elements on growth (a vector would).
    std::deque<ScannedItem> ScanData;

    // [house][class][quality] -> the pool the listing service draws from.
    std::vector<ScannedItem*> ItemSelectionTable[kAuctionHouseIndexBound][MAX_ITEM_CLASS][MAX_ITEM_QUALITY];

    // (house, class, quality, itemID) -> that item's row, so FindScannedItem is
    // O(1) during a scan instead of a linear bucket walk. First row wins on the
    // rare duplicate key (suffix is not part of the key, matching the old search).
    std::unordered_map<uint64_t, ScannedItem const*> ItemIndex;

    // itemID -> its row, independent of house/class/quality -- for the addon's
    // item pricer, which only knows an item id (from a drag-drop) and needs to
    // answer "does this item already have pricing data at all". First row wins
    // on a duplicate itemID scanned into multiple houses.
    std::unordered_map<uint32, ScannedItem const*> ByItemId;
    ScannedItem const* FindAnyScan(uint32 itemId) const;

    // Adds or replaces a GM-entered price for one item (see ScannedItem::FromOverride),
    // filing it into ScanData/ItemSelectionTable/ItemIndex/ByItemId exactly like a
    // real scanned row, and persists it to auctionsim_overrides.dat so it survives a
    // restart. neutralEligible mirrors AuctionSim.NeutralItems -- true also files the
    // item into the Neutral bucket (in addition to its native house). Returns false
    // only if the item's template can't be resolved or the overrides file can't be
    // written.
    bool UpsertOverride(
        uint32 itemId,
        uint32 marketPrice,
        uint32 listLow,
        uint32 listHigh,
        uint32 typicalStack,
        uint32 stackLow,
        uint32 stackHigh,
        bool neutralEligible);

    // Per-(itemClass, quality) listing multiplier vs. the real market, from the
    // AuctionSim.<Class>Percent config lines. Plain decimals (1, 1.5, 0.25, 0).
    float ItemSelectionMask[MAX_ITEM_CLASS][MAX_ITEM_QUALITY];

    // Observed per-(faction, class, quality) auction-count distribution, from the
    // category header rows of auctionsim.dat. Drives how full the bot keeps each
    // category: it only tops a category up while its live auction count is below
    // q1, and never past a random point in [q1, median].
    struct CategoryDepth
    {
        bool has = false;
        uint32 q1 = 0;
        uint32 median = 0;
        uint32 adjLow = 0;
        uint32 adjHigh = 0;
    };
    CategoryDepth categoryDepth[kAuctionHouseIndexBound][MAX_ITEM_CLASS][MAX_ITEM_QUALITY];

    CategoryDepth const& GetCategoryDepth(AuctionHouseId houseId, uint32 itemClass, uint32 quality) const;

    std::vector<ScannedItem*> const& ItemsFor(AuctionHouseId houseId, uint32 itemClass, uint32 quality) const;

    // O(1) lookup of a specific item's row within one (houseId, itemClass, quality)
    // bucket. Returns nullptr if the coordinate is out of range or the item is not
    // in that bucket.
    ScannedItem const* FindScannedItem(AuctionHouseId houseId, uint32 itemClass, uint32 quality, uint32 itemID) const;

    // One (itemClass, quality) mask cell, addressed the same way the addon bridge's wire
    // protocol addresses it: percentConfigKey matches the config key suffix (e.g.
    // "ConsumablePercent"), qualityLabel matches the quality token (e.g. "GREY").
    struct MaskKeyEntry
    {
        std::string_view percentConfigKey;
        std::string_view qualityLabel;
        uint32 itemClass;
        uint32 quality;
    };

    // Resolves a wire key formatted "<percentConfigKey>.<qualityLabel>" (e.g.
    // "ConsumablePercent.GREY") to its ItemSelectionMask indices. False if unrecognized.
    static bool ResolveMaskKey(std::string_view key, uint32& outItemClass, uint32& outQuality);

    // Every (percentConfigKey, qualityLabel) pair -- MAX_ITEM_CLASS * 7 entries -- for
    // enumerating the full mask grid (e.g. to answer a GETCONFIG request).
    static std::vector<MaskKeyEntry> const& AllMaskKeys();

private:
    // Packs a bucket coordinate + itemID into an ItemIndex key.
    static uint64_t IndexKey(size_t house, uint32 itemClass, uint32 quality, uint32 itemID);

    // Constructor helpers, in call order. The line loaders skip an individual bad
    // row (logging it) and carry on.
    static bool ParseHeaderLine(std::string const& line, size_t& outItemRows, size_t& outCategoryRows);
    void LoadCategoryRow(std::string const& line, std::string const& filepath);
    void LoadItemRow(std::string const& line, std::string const& filepath);
    void BuildSelectionTables(std::string const& filepath);
    void LoadMasks();
    void LoadVendorItems();
    void LoadNeutralConfig();
    void SynthesizeNeutralDepth();
    void LoadOverrides();
    void LoadItemExceptions();

    // Overrides persist next to auctionsim.dat as auctionsim_overrides.dat, one
    // "itemId:faction:marketPrice:listLow:listHigh:typicalStack:stackLow:stackHigh:neutralEligible"
    // line per item -- kept separate from auctionsim.dat since that file is
    // regenerated wholesale by data/compile-data.cpp and a GM's manual entries
    // would otherwise be silently lost on the next scan-data refresh.
    std::string overridesFilePath;
    bool WriteOverridesFile() const;

    void UnpackQualityString(std::string_view qualityString, int itemClass);
};
