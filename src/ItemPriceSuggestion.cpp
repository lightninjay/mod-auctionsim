#include "ItemPriceSuggestion.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "AuctionSim.h"
#include "DatabaseEnv.h"
#include "QueryResult.h"
#include "StringFormat.h"
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
    //
    // WHERE Item = ? can't use either table's primary key (both are keyed
    // (Entry, Item, ...) -- Entry leads, not Item), so this is a full table scan
    // every call. Shared by both the sync and async paths below; the SQL text
    // itself doesn't change, only how it's run.
    //
    // The "AS chance_avg" alias is load-bearing, not cosmetic -- do not remove
    // it. MySQL's default alias for AVG(ABS(Chance)) contains the literal
    // substring "avg", and this fork's Field::GetData<T>() special-cases any
    // column whose alias equals (case-insensitively) "avg"/"sum"/"min"/"max"/
    // "count": on a type mismatch it logs a warning and retries via
    // GetData<double>() to "self-correct". That retry can itself fail the same
    // check against the same metadata with nothing having changed, recursing
    // into itself without bound -- a stack overflow, confirmed via gdb backtrace
    // (Field::GetData<double> at Field.cpp:255 calling itself several thousand
    // times). This is a core bug, not an application-level one, and not
    // specific to any particular item -- it was only ever gated behind which
    // items missed the price cache. Aliasing the column to something that
    // doesn't match any of those five reserved names sidesteps the special
    // case entirely; nothing else needs to change.
    constexpr char const* kDropChanceSql =
        "SELECT AVG(ABS(Chance)) AS chance_avg FROM creature_loot_template WHERE Item = {} "
        "UNION ALL "
        "SELECT AVG(ABS(Chance)) AS chance_avg FROM reference_loot_template WHERE Item = {}";

    float AverageDropChanceFromResult(QueryResult const& result)
    {
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
                // Get<double>(), matching AVG()'s real SQL return type -- not
                // just belt-and-suspenders alongside the alias fix above, but
                // the technically correct type regardless of it.
                total += static_cast<float>(fields[0].Get<double>());
                rows++;
            }
        } while (result->NextRow());

        if (rows == 0)
        {
            return 100.0f;
        }
        return total / static_cast<float>(rows);
    }

    // Blocking: only used by the synchronous Suggest() path (see its doc comment
    // in the header for exactly which callers that's still appropriate for).
    float AverageDropChancePercentSync(uint32 itemId)
    {
        QueryResult result = WorldDatabase.Query(kDropChanceSql, itemId, itemId);
        return AverageDropChanceFromResult(result);
    }

    // ---- Drop-chance memoisation for the async path -----------------------
    //
    // kDropChanceSql is a full scan of two large tables (see its comment), and
    // SuggestAsync is reachable from AuctionSimPublicPriceBridge, which any
    // connected player can hit with no GM check. Without this, N requests for
    // the same unpriced item meant N identical full-table scans -- moving that
    // off the world thread stopped it stalling the tick, but it still saturates
    // the WorldDatabase worker pool, which starves every other async query on
    // the server.
    //
    // What's cached is the raw query answer (a float), NOT the finished
    // Suggestion. Two reasons: the loot tables don't change while the world is
    // running, so this answer never goes stale; and Suggestion::basis points at
    // BuildSuggestion's reused thread_local buffer, so a cached Suggestion would
    // hand out a pointer whose contents change under the holder. BuildSuggestion
    // itself is pure arithmetic -- re-running it per request costs nothing.
    std::unordered_map<uint32, float> dropChanceCache;

    // Requests for an itemId whose query is already in flight. They wait on that
    // one query rather than firing duplicates -- without this, a burst of
    // requests that arrives before the first query returns still produces one
    // full table scan per request, which is the exact burst pattern that matters.
    std::unordered_map<uint32, std::vector<std::function<void(ItemPriceSuggestion::Suggestion)>>>
        inFlightWaiters;

    // item_template is ~45k rows, so this is the practical ceiling anyway; the
    // cap only exists so a server with a vastly larger item set can't grow this
    // without bound. Clearing wholesale is fine -- entries are pure cache and
    // cost one query each to rebuild.
    constexpr size_t kMaxCacheEntries = 60000;

    // ---- Hard ceiling on concurrent drop-chance queries --------------------
    //
    // The cache above and the in-flight dedup below both key on itemId, so they
    // only collapse load for REPEATED items. Under genuinely random item access
    // -- a player alt-right-clicking item after item into Auctionator's sell
    // window, which is the exact reported repro -- every request is a distinct
    // cache miss, so each one enqueues its own full-table scan and
    // inFlightWaiters.size() grows without bound.
    //
    // The per-player token bucket in AuctionSimPublicPriceBridge caps the arrival
    // RATE, but a rate limit is not a concurrency limit: if the arrival rate
    // (10/sec sustained, per player, multiplied by however many players are
    // online) exceeds the rate the database can retire these scans, the queue
    // still grows without bound. It just takes slightly longer to get there.
    // That unbounded queue is what eventually starves the world tick.
    //
    // So bound the thing that actually matters: how many of these scans can be
    // outstanding at once, server-wide. Past this ceiling, callers get an
    // immediate answer built with a neutral drop factor instead of waiting --
    // see the kNeutralDropChance note in SuggestAsync for why that degradation
    // is the right trade.
    constexpr size_t kMaxInFlightQueries = 4;

    // "No rarity adjustment" -- the same value AverageDropChanceFromResult
    // returns for an item with no loot-table rows at all, so an over-ceiling
    // answer is identical to the answer any vendor/crafted/BoE item gets
    // legitimately. It is never written to dropChanceCache, so the real value is
    // still looked up normally once the burst subsides.
    constexpr float kNeutralDropChance = 100.0f;

    // Both maps are touched only from the world thread: SuggestAsync is called
    // from world-thread hooks, and the query callbacks are pumped by
    // AuctionSim::queryProcessor.ProcessReadyCallbacks() on the world tick. Same
    // single-threaded justification the rest of this module relies on.

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

    // The actual suggestion math, shared by both Suggest() and SuggestAsync() --
    // identical regardless of how avgDropChancePercent was obtained (blocking
    // query vs. async callback). proto is assumed non-null; both public
    // entry points check that before calling this.
    ItemPriceSuggestion::Suggestion BuildSuggestion(ItemTemplate const* proto, float avgDropChancePercent)
    {
        ItemPriceSuggestion::Suggestion s;

        float qualityMult = QualityMultiplier(proto->Quality);
        float dropFactor = DropRarityFactor(avgDropChancePercent);

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
        // basisStorage is thread_local and reused per call -- fine as long as
        // every caller (Suggest, and SuggestAsync's query callback) consumes
        // Suggestion::basis immediately and doesn't hold the pointer past the
        // next call on this thread.

        return s;
    }
}

namespace ItemPriceSuggestion
{
    Suggestion Suggest(ItemTemplate const* proto, uint32 itemId)
    {
        if (!proto)
        {
            return {};
        }
        return BuildSuggestion(proto, AverageDropChancePercentSync(itemId));
    }

    void SuggestAsync(ItemTemplate const* proto, uint32 itemId, std::function<void(Suggestion)> callback)
    {
        if (!proto)
        {
            callback(Suggestion{});
            return;
        }

        // Already known -- answer inline without touching the database at all.
        // Note this makes the callback run on THIS tick rather than a later one;
        // that's still within the contract documented in the header (callers must
        // not assume a raw Player* stays valid), and the existing callers all
        // re-resolve from an ObjectGuid, so a same-tick reply is safe for them.
        if (auto cached = dropChanceCache.find(itemId); cached != dropChanceCache.end())
        {
            callback(BuildSuggestion(proto, cached->second));
            return;
        }

        // A query for this item is already outstanding -- ride on it.
        if (auto pending = inFlightWaiters.find(itemId); pending != inFlightWaiters.end())
        {
            pending->second.push_back(std::move(callback));
            return;
        }

        // At the concurrency ceiling: answer now with a neutral drop factor
        // rather than enqueueing another full-table scan. Degrading the
        // suggestion slightly (this item just doesn't get its rare-drop markup
        // for this one request) is strictly better than letting the query queue
        // grow without bound -- and because nothing is cached here, the very
        // next request for this item once the burst subsides gets the real
        // value. inFlightWaiters.size() is exactly the outstanding query count:
        // an entry is created below immediately before the query is issued and
        // erased when it resolves.
        if (inFlightWaiters.size() >= kMaxInFlightQueries)
        {
            callback(BuildSuggestion(proto, kNeutralDropChance));
            return;
        }

        inFlightWaiters[itemId].push_back(std::move(callback));

        QueryCallback queryCallback = WorldDatabase.AsyncQuery(
            Acore::StringFormat(kDropChanceSql, itemId, itemId));

        queryCallback.WithCallback(
            [proto, itemId](QueryResult result)
            {
                float chance = AverageDropChanceFromResult(result);

                if (dropChanceCache.size() >= kMaxCacheEntries)
                {
                    dropChanceCache.clear();
                }
                dropChanceCache[itemId] = chance;

                auto waiting = inFlightWaiters.find(itemId);
                if (waiting == inFlightWaiters.end())
                {
                    return;
                }
                // Move the list out and erase BEFORE invoking: a callback is free
                // to call SuggestAsync again (the cache is populated above, so a
                // re-entrant call for this same item returns inline), and that
                // would otherwise mutate the container being iterated.
                std::vector<std::function<void(Suggestion)>> waiters = std::move(waiting->second);
                inFlightWaiters.erase(waiting);

                for (auto& waiter : waiters)
                {
                    // Each waiter consumes Suggestion::basis immediately; see the
                    // thread_local note in BuildSuggestion.
                    waiter(BuildSuggestion(proto, chance));
                }
            });

        AuctionSim* sim = AuctionSim::instance();
        if (!sim)
        {
            // Shutting down or not yet constructed -- nothing to pump this into;
            // the query is simply dropped (matches how a since-logged-out Player
            // would also never get a reply). Drop the waiters entry too: leaving
            // it behind would mark this itemId permanently "in flight", so every
            // later request for it would queue against a query that will never
            // complete and never get a reply.
            inFlightWaiters.erase(itemId);
            return;
        }
        sim->AddQueryCallback(std::move(queryCallback));
    }
}
