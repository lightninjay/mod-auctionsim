#pragma once
#include "Define.h"

class ItemTemplate;

// Single gate for "should this item be visible through AuctionSim's addon
// channels at all" -- the Price Search tab, the Item Pricer, and the public
// player-facing price endpoint all route through it, so an item can never be
// reachable via one path but filtered on another.
//
// Two separate reasons this exists:
//
// 1. Correctness. item_template is full of rows that are not real, obtainable,
//    auctionable goods: Blizzard's internal QA items ("AHNQIRAJ TEST ITEM A
//    CLOTH BELT"), placeholders, deprecated rows, internal money/permanent/key
//    classes, and Bind-on-Pickup items (which can only ever reach a vendor,
//    never the auction house). Offering any of these as things to price is
//    meaningless -- no player can ever put one on the auction house.
//
// 2. Stability. Those rows also carry unvetted junk data (zero or absurd item
//    levels, prices, stack sizes) that the rest of the pricing path was never
//    written to expect, and hovering one drives a client tooltip query plus our
//    own lookup for an item the normal item pipeline never sees. Keeping them
//    out of results at the source is cheaper and safer than hardening every
//    downstream consumer against data that should never have been offered.
//
// Deliberately conservative: it is much worse to hide a real tradable item from
// a GM than to leave one junk row visible, so every rule here keys on something
// unambiguous rather than a general "looks odd" heuristic.
namespace ItemEligibility
{
    // True if this item is something a player could plausibly hold and auction.
    bool IsAuctionableItem(ItemTemplate const& proto);
}
