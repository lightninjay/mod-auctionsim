#pragma once
#include "ScriptMgr.h"

class Player;

// Public, read-only companion to AuctionSimAddonBridge -- deliberately its own
// small file rather than a message type bolted onto the GM bridge, so the
// entire "any player, no GM check" surface area is one file you can audit at a
// glance. It exists so any player's client (e.g. a small hook added to the
// Auctionator addon) can ask "what would AuctionSim's bot always buy this item
// for" and get a number back, without needing GM rights and without touching
// AuctionSim's config, pricing data, or bot state in any way.
//
// This class NEVER calls anything that mutates ASConfig (no UpsertOverride, no
// SetHouseOverride/ClearHouseOverride, no config writes, no bot/roster calls) --
// it only reads via ASConfig::FindAnyScan and ItemPriceSuggestion's suggestion
// functions (Suggest / SuggestAsync -- the latter used here: a per-item
// drop-chance DB query has to go through AuctionSim::AddQueryCallback rather
// than block this hook, or a burst of requests could stall the whole server).
// If you're adding something here later, that read-only invariant is the whole
// point of this file being separate: keep it that way.
//
// Because there is no GM check, every request is metered by a per-player token
// bucket (see ConsumeRequestToken in the .cpp) and the underlying drop-chance
// query is memoised and de-duplicated inside ItemPriceSuggestion. Anything new
// added here must stay behind that same bucket -- an unmetered path on this
// endpoint is reachable by any connected player.
//
// Same whisper-based transport as AuctionSimAddonBridge (see that header's
// comment for why OnPlayerBeforeSendChatMessage is the right hook), but under
// its own addon message prefix ("AHSIMPRICE") so the two channels can never be
// confused with each other client-side, and a GM-focused addon update can
// never accidentally widen what this one exposes.
class AuctionSimPublicPriceBridge : public PlayerScript
{
public:
    AuctionSimPublicPriceBridge() :
        PlayerScript("AuctionSimPublicPriceBridge", {PLAYERHOOK_ON_BEFORE_SEND_CHAT_MESSAGE})
    {
    }

    void OnPlayerBeforeSendChatMessage(Player* player, uint32& type, uint32& lang, std::string& msg) override;
};

void AddAuctionSimPublicPriceBridgeScript();
