#include "ItemPriceSuggestion.h"
#include <algorithm>
#include <cmath>
#include <string>
#include "DatabaseEnv.h"
#include "ItemTemplate.h"

namespace
{
    // Rough AH-vs-vendor markup by quality: how many times a real seller typically
    // prices an item over what a vendor would pay/sell it for. These are starting
    // points for a GM to tune, not derived from live market data -- there's no
    // scan for an item that has never been listed.
    float QualityMultiplier(uint32 quality)
    {
        switch (quality)
        {
            case ITEM_QUALITY_POOR: return 0.5f;
            case ITEM_QUALITY_NORMAL: return 1.5f;
            case ITEM_QUALITY_UNCOMMON: return 4.0f;
            case ITEM_QUALITY_RARE: return 10.0f;
            case ITEM_QUALITY_EPIC: return 25.0f;
            case ITEM_QUALITY_LEGENDARY: return 60.0f;
            case ITEM_QUALITY_ARTIFACT: return 100.0f;
            default: return 2.0f;
        }
    }

    // No vendor price at all (most drop/quest items): fall back to an item-level
    // curve so higher-level gear still suggests a bigger number than a level-1
    // grey. Deliberately soft (sublinear) -- this is only a starting point.
    uint32 ItemLevelBaseline(uint32 itemLevel)
    {
        double level = std::max<uint32>(itemLevel, 1);
        return static_cast<uint32>(std::pow(level, 1.6) * 4.0);
    }

    // Best-effort average drop chance (%) for itemId across creature_loot_template
    // and reference_loot_template. Returns 100 (i.e. "not rare, no adjustment") if
    // the item isn't found in either -- most listable items are BoE/crafted/vendor
    // goods with no loot-table entry at all, and that's not evidence of rarity.
    float AverageDropChancePercent(uint32 itemId)
    {
        QueryResult result = WorldDatabase.Query(
            "SELECT AVG(ABS(Chance)) FROM creature_loot_template WHERE Item = {} "
            "UNION ALL "
            "SELECT AVG(ABS(Chance)) FROM reference_loot_template WHERE Item = {}",
            itemId, itemId);
        if (!result)
        {
            return 100.0f;
        }

        float total = 0.0f;
        uint32 rows = 0;
        do
        {
            Field* fields = result->Fetch();
            if (!fields[0].IsNull())
            {
                total += fields[0].Get<float>();
                rows++;
            }
        } while (result->NextRow());

        if (rows == 0)
        {
            return 100.0f;
        }
        return total / static_cast<float>(rows);
    }

    // Rarer drops warrant a higher suggested price; scale up as chance drops
    // below 100%, capped so a 0.01%-chance item doesn't suggest something absurd.
    float DropRarityFactor(float avgChancePercent)
    {
        float chance = std::clamp(avgChancePercent, 0.01f, 100.0f);
        float factor = std::pow(100.0f / chance, 0.3f);
        return std::clamp(factor, 1.0f, 5.0f);
    }

    bool IsEquipLike(uint32 itemClass)
    {
        return itemClass == ITEM_CLASS_WEAPON || itemClass == ITEM_CLASS_ARMOR ||
               itemClass == ITEM_CLASS_QUIVER || itemClass == ITEM_CLASS_GEM;
    }
}

namespace ItemPriceSuggestion
{
    Suggestion Suggest(ItemTemplate const* proto, uint32 itemId)
    {
        Suggestion s;
        if (!proto)
        {
            return s;
        }

        float qualityMult = QualityMultiplier(proto->Quality);
        float dropFactor = DropRarityFactor(AverageDropChancePercent(itemId));

        uint32 vendorAnchor = 0;
        char const* anchorLabel = "item level";
        if (proto->BuyPrice > 0)
        {
            vendorAnchor = static_cast<uint32>(proto->BuyPrice);
            anchorLabel = "vendor buy price";
        }
        else if (proto->SellPrice > 0)
        {
            // SellPrice is what a vendor pays the player, typically ~4x below
            // BuyPrice for vendor-sold items -- scale back up toward a
            // BuyPrice-equivalent anchor before applying the AH markup.
            vendorAnchor = static_cast<uint32>(proto->SellPrice) * 4;
            anchorLabel = "vendor sell price";
        }
        else
        {
            vendorAnchor = ItemLevelBaseline(proto->ItemLevel);
        }

        uint32 market = static_cast<uint32>(static_cast<float>(vendorAnchor) * qualityMult * dropFactor);
        market = std::max(market, 1u);

        s.marketPrice = market;
        s.listLow = std::max(1u, static_cast<uint32>(market * 0.85f));
        s.listHigh = static_cast<uint32>(market * 1.2f);

        if (IsEquipLike(proto->Class))
        {
            s.typicalStack = s.stackLow = s.stackHigh = 1;
        }
        else
        {
            uint32 maxStack = std::max<uint32>(1, proto->GetMaxStackSize());
            uint32 typical = std::min<uint32>(maxStack, 5);
            s.typicalStack = typical;
            s.stackLow = 1;
            s.stackHigh = std::max(typical, std::min<uint32>(maxStack, typical * 2));
        }

        static thread_local std::string basisStorage;
        basisStorage = std::string("anchored on ") + anchorLabel +
            (dropFactor > 1.05f ? ", adjusted up for rare drop chance" : "");
        s.basis = basisStorage.c_str();
        // basisStorage is thread_local and reused per call -- fine for the bridge's
        // single-threaded "call Suggest, immediately format a reply" use, but the
        // returned Suggestion::basis pointer must not be held past the next call
        // on this thread.

        return s;
    }
}
