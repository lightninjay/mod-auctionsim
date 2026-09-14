#pragma once
#include <functional>
#include "Define.h"

class ItemTemplate;

// Produces a starting-point listing price/stack suggestion for an item that has
// no auctionsim.dat scan data and no GM override yet -- what the ahsim addon's
// item-pricer shows (all fields editable) when you drag an unpriced item onto it.
// This is a heuristic, not a market simulation: it anchors on vendor price where
// one exists, falls back to an item-level-based curve where it doesn't, scales
// by rarity, and nudges upward for items that are hard to farm (rare drop chance
// off looted creatures). It exists to give the GM a reasonable starting number to
// edit, not a number to trust blindly -- every field the addon shows stays editable.
namespace ItemPriceSuggestion
{
    struct Suggestion
    {
        uint32 marketPrice = 0;
        uint32 listLow = 0;
        uint32 listHigh = 0;
        uint32 typicalStack = 1;
        uint32 stackLow = 1;
        uint32 stackHigh = 1;

        // Human-readable note on what the suggestion was anchored to, sent back to
        // the addon so it can show e.g. "based on vendor price + rare drop chance"
        // instead of presenting the numbers as authoritative.
        char const* basis = "";
    };

    // Synchronous: runs the drop-chance lookup (a query against
    // creature_loot_template/reference_loot_template, WHERE Item = ..., which
    // that pair's schema can't use its composite key for -- Entry leads the
    // key, not Item -- so this is a full table scan) on the calling thread.
    // Only call this where a multi-table-scan blocking DB query is acceptable:
    // one-time bulk work at module load (see
    // ASConfig::SynthesizeMissingNeutralItems), before the world tick loop (and
    // this file's async plumbing below) is live. Do NOT call this from a live,
    // per-request handler on the world thread -- that's exactly what stalls the
    // whole server long enough to trip the watchdog. Use SuggestAsync there.
    Suggestion Suggest(ItemTemplate const* proto, uint32 itemId);

    // Same computation as Suggest(), but for any live per-request caller
    // (addon bridges, chat hooks) -- fires the drop-chance lookup via
    // WorldDatabase.AsyncQuery instead of blocking the calling thread, and
    // invokes `callback` with the finished Suggestion once it completes.
    // Internally hands the pending query to AuctionSim::AddQueryCallback, so
    // it's pumped every world tick regardless of caller.
    //
    // IMPORTANT: `callback` runs on a LATER world tick (possibly after the
    // triggering Player has logged out or otherwise become invalid) --
    // never capture a raw Player* into it. Capture an ObjectGuid and
    // re-resolve via ObjectAccessor::FindPlayer inside the callback instead,
    // and handle a null result (the player's gone) by simply not replying.
    void SuggestAsync(ItemTemplate const* proto, uint32 itemId, std::function<void(Suggestion)> callback);
}

