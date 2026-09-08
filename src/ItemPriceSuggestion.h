#pragma once
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

    // itemId is passed alongside proto so the drop-chance lookup (a DB query
    // against creature_loot_template/reference_loot_template) can run without a
    // second round trip through the caller.
    Suggestion Suggest(ItemTemplate const* proto, uint32 itemId);
}
