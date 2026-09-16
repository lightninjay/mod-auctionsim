#include "AuctionSimPublicPriceBridge.h"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <iterator>
#include <string>
#include <string_view>
#include <unordered_map>
#include "ASConfig.h"
#include "AuctionHouseMgr.h"
#include "AuctionSim.h"
#include "Common.h"
#include "Config.h"
#include "ItemEligibility.h"
#include "ItemPriceSuggestion.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SharedDefines.h"

namespace
{
    constexpr std::string_view kPrefix = "AHSIMPRICE";
    constexpr std::string_view kFullPrefixWithTab = "AHSIMPRICE\t";

    // ---- Per-player rate limit --------------------------------------------
    //
    // This endpoint has no GM check by design, so anyone connected can drive it.
    // ItemPriceSuggestion now memoises the expensive drop-chance query, which
    // removes the database load, but each request still costs a hash lookup,
    // a string format and an outbound whisper packet -- so the call rate itself
    // still needs a ceiling.
    //
    // A token bucket rather than a flat cooldown, because the legitimate client
    // pattern is bursty: an Auctionator-style hook asks about every item on an
    // AH page or in a bag at once, then goes quiet. A flat cooldown would break
    // that; a bucket absorbs the burst and only bites on sustained spam.
    constexpr double kBurstTokens = 30.0;       // one AH page / full bag at once
    constexpr double kRefillPerSecond = 10.0;   // sustained ceiling

    struct RateBucket
    {
        double tokens = kBurstTokens;
        std::chrono::steady_clock::time_point lastRefill;
    };

    // World-thread only, same as the rest of this module's hook state. Entries
    // are pruned once a player's bucket has refilled to full, so this stays
    // proportional to currently-active users rather than to everyone ever seen.
    std::unordered_map<ObjectGuid, RateBucket> rateBuckets;

    // Sweeping the whole map on every request is O(players) per request, i.e.
    // O(players^2) per second of traffic -- measured at ~250ms of world-thread
    // time per second at 3k population, which would reintroduce exactly the tick
    // starvation this change exists to prevent. Sweep on a timer instead: the
    // map only needs to not grow without bound, and it can never exceed the
    // number of players who have sent a request since the last sweep.
    constexpr auto kPruneInterval = std::chrono::seconds(60);
    std::chrono::steady_clock::time_point lastPrune{};

    bool ConsumeRequestToken(ObjectGuid guid)
    {
        auto now = std::chrono::steady_clock::now();

        if (now - lastPrune >= kPruneInterval)
        {
            lastPrune = now;
            for (auto it = rateBuckets.begin(); it != rateBuckets.end();)
            {
                double idleSeconds = std::chrono::duration<double>(now - it->second.lastRefill).count();
                bool refilledToFull = it->second.tokens + idleSeconds * kRefillPerSecond >= kBurstTokens;
                it = (refilledToFull && !(it->first == guid)) ? rateBuckets.erase(it) : std::next(it);
            }
        }

        auto [entry, inserted] = rateBuckets.try_emplace(guid);
        RateBucket& bucket = entry->second;
        if (inserted)
        {
            bucket.lastRefill = now;
        }
        else
        {
            double elapsed = std::chrono::duration<double>(now - bucket.lastRefill).count();
            bucket.tokens = std::min(kBurstTokens, bucket.tokens + elapsed * kRefillPerSecond);
            bucket.lastRefill = now;
        }

        if (bucket.tokens < 1.0)
        {
            return false;
        }
        bucket.tokens -= 1.0;
        return true;
    }

    // Same self-whisper transport AuctionSimAddonBridge uses (see its header) --
    // reaches the client's CHAT_MSG_ADDON handler with no live bot character
    // needed, so this works even while AuctionSim's bot roster is disabled or
    // has never been configured; only the pricing data (ASConfig) needs to exist,
    // which is loaded at module startup regardless of AuctionSim.Enabled.
    void SendMessage(Player* target, std::string const& body)
    {
        if (!target)
        {
            return;
        }
        target->Whisper(Acore::StringFormat("{}\t{}", kPrefix, body), LANG_ADDON, target);
    }

    // Parses a plain unsigned decimal token. No ASParse dependency here --
    // deliberately: this file doesn't include anything from the GM bridge's
    // dependency chain, to keep its own dependency surface minimal and obvious.
    bool ParseUint32(std::string_view token, uint32& out)
    {
        auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), out);
        return ec == std::errc() && ptr == token.data() + token.size();
    }

    // The number this whole file exists to compute: a price any player can list
    // an item at that AuctionSim's buy queue is guaranteed to pick up, with some
    // headroom below the true guaranteed-buy ceiling (ScannedItem::GetMarketPrice
    // / ItemPriceSuggestion::Suggest's marketPrice -- AuctionBuyingService always
    // queues a purchase at or under that value, see ConsiderForPurchase) so the
    // player isn't pricing at the exact knife's edge. Percent is clamped to a
    // sane 50-100 range so a typo in the config can't suggest 0 or something
    // above the guaranteed-buy price.
    uint32 ApplyMargin(uint32 guaranteedBuyPrice)
    {
        uint32 marginPercent = sConfigMgr->GetOption<uint32>("AuctionSim.PlayerPriceSuggestMarginPercent", 90);
        marginPercent = std::clamp<uint32>(marginPercent, 50, 100);

        uint64 suggested = (static_cast<uint64>(guaranteedBuyPrice) * marginPercent) / 100;
        return static_cast<uint32>(std::max<uint64>(suggested, 1));
    }

    // A player can only ever browse the Alliance or Horde auction house that
    // matches their own team (plus Neutral, which both factions can reach) --
    // there is no client-side way for them to accidentally look at the other
    // faction's house. That means "the house AuctionBuyingService will actually
    // evaluate this player's listing against" is fully determined by team, with
    // no ambiguity to ask the client about. AuctionSim.cpp's live buy pass keys
    // its ScannedItem lookup off the auction's own house
    // (config->FindScannedItem(_AuctionHouseId, ...)) -- never off some
    // item-wide "best guess" -- so replying with anything other than this
    // player's own house's row would describe a price the bot might never
    // actually honor for them.
    AuctionHouseId ResolveHomeHouse(Player* player)
    {
        return player->GetTeamId() == TEAM_ALLIANCE ? AuctionHouseId::Alliance : AuctionHouseId::Horde;
    }

    // What's actually sitting on the auction house right now for this item, on
    // this house -- as opposed to ScannedItem::GetListLow(), which is a
    // historical/statistical reference the bot uses to decide what price to
    // roll for its OWN new listings, not a live read of the shelf. The two can
    // legitimately disagree: GetListLow reflects the broader reference economy
    // this item was scanned from, while this reflects only what this specific,
    // possibly much smaller or larger, live house happens to have posted at
    // this exact moment. Reporting the static number as "current low" was the
    // actual bug being fixed here -- a player comparing their own listing
    // against "the low end of the market" needs to know what a buyer would
    // actually see sitting on the house today, not a number derived from
    // reference data that may never have existed on this house at all.
    //
    // Pure in-memory scan (GetAuctions() is the live AuctionHouseObject map,
    // no DB access), bounded by how many auctions are currently on this one
    // house, and gated behind the same per-player rate limit as everything
    // else this file does -- there is no unbounded-cost path here.
    uint32 FindLowestCurrentListing(AuctionHouseId houseId, uint32 itemTemplateId)
    {
        AuctionHouseObject* house = sAuctionMgr->GetAuctionsMapByHouseId(houseId);
        if (!house)
        {
            return 0;
        }

        uint32 lowest = 0;
        for (auto const& entry : house->GetAuctions())
        {
            AuctionEntry const* auction = entry.second;
            if (auction->item_template != itemTemplateId || auction->itemCount == 0)
            {
                continue;
            }
            uint32 pricePerItem = auction->buyout / auction->itemCount;
            if (pricePerItem == 0)
            {
                continue;  // no valid buyout on this listing (bid-only or similar) -- not a comparable "price"
            }
            if (lowest == 0 || pricePerItem < lowest)
            {
                lowest = pricePerItem;
            }
        }
        return lowest;
    }

    // Every "no data" / "nothing to say about this item" reply, in one place,
    // so every early-out below produces the exact same nine-field shape rather
    // than each bailout hand-rolling its own field count -- a mismatch there
    // would desync the addon's strsplit on a path that's easy to not exercise
    // in testing (unknown item, config not loaded, etc.).
    std::string NoDataReply(uint32 itemId)
    {
        return Acore::StringFormat("{}\t0\t0\t0\t0\t0\t0\t0\t0", itemId);
    }

    // Looks up the Neutral-house row for `itemId`, but only if the item is
    // actually reachable there at all. AuctionSim.NeutralItems is a curated
    // allowlist (see ASConfig::neutralEligibleItems's doc comment) -- most
    // items are never filed into the Neutral bucket no matter what real AH
    // data exists for them, so an item can have a perfectly good Alliance or
    // Horde price and still be a guaranteed no-sale on Neutral. Reporting
    // "not available here" for those (rather than silently reusing the home
    // house's number) is the whole point of this helper: it lets the client
    // tell a player "list this on your own AH instead" instead of quoting a
    // price the Neutral bot will never actually pay.
    struct NeutralQuote
    {
        bool available = false;  // item can ever sell on Neutral at all
        uint32 floor = 0;
        uint32 low = 0;
        bool hasRealData = false;
    };

    NeutralQuote ResolveNeutralQuote(ASConfig const* config, uint32 itemId)
    {
        NeutralQuote quote;
        if (!config->enableNeutralAH || !config->IsNeutralEligible(itemId))
        {
            return quote;  // guaranteed no-sale here regardless of any scan data
        }

        quote.available = true;
        quote.low = FindLowestCurrentListing(AuctionHouseId::Neutral, itemId);
        if (ScannedItem const* row = config->FindHouseScan(itemId, AuctionHouseId::Neutral))
        {
            quote.floor = ApplyMargin(row->GetMarketPrice());
            quote.hasRealData = true;
        }
        // else: on the allowlist but SynthesizeMissingNeutralItems hasn't filed
        // a row yet (e.g. mid-startup) -- available stays true (it WILL sell
        // there), but with no floor to show yet; hasRealData/floor stay 0.
        // quote.low is independent of this and reports real listings either way.
        return quote;

    }

    void HandlePriceRequest(Player* player, std::string_view payload)
    {
        // Checked before parsing, so a flood of malformed payloads costs the same
        // as a flood of well-formed ones.
        if (!ConsumeRequestToken(player->GetGUID()))
        {
            // Deliberately the same "no data" shape this endpoint already returns
            // for an unknown item or not-yet-loaded config, rather than a new
            // status code: it keeps the wire format simple, and a client that is
            // over its budget should behave exactly as it does when the server
            // has nothing for that item -- show no suggestion and move on. The
            // client is never left waiting on a reply that doesn't come.
            uint32 requestedId = 0;
            ParseUint32(payload, requestedId);
            SendMessage(player, NoDataReply(requestedId));
            return;
        }

        uint32 itemId = 0;
        if (!ParseUint32(payload, itemId) || itemId == 0)
        {
            SendMessage(player, NoDataReply(0));  // malformed request -- reply with an
                                                   // obviously-invalid result rather
                                                   // than nothing, so the client isn't
                                                   // left waiting forever.
            return;
        }

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        if (!proto)
        {
            SendMessage(player, NoDataReply(itemId));
            return;
        }

        // Internal/QA/placeholder rows are never auctionable, so there is
        // nothing meaningful to suggest -- and this endpoint is driven by a
        // client tooltip hook, so it fires for whatever the player happens to
        // hover, including rows the normal item pipeline never handles. Answer
        // with the standard "no data" shape and do no further work: no scan
        // lookup, and above all no async loot-table query for a row that should
        // never have been priced in the first place.
        if (!ItemEligibility::IsAuctionableItem(*proto))
        {
            SendMessage(player, NoDataReply(itemId));
            return;
        }

        AuctionSim* sim = AuctionSim::instance();
        ASConfig const* config = sim ? sim->GetConfig() : nullptr;
        if (!config)
        {
            // Module hasn't finished loading its pricing data yet.
            SendMessage(player, NoDataReply(itemId));
            return;
        }

        AuctionHouseId homeHouse = ResolveHomeHouse(player);
        uint32 homeHouseNum = static_cast<uint32>(homeHouse);

        // hasRealData distinguishes real scan/GM-priced data from a pure
        // ItemPriceSuggestion guess, so the client can show an appropriate
        // confidence level for each house independently -- this endpoint never
        // exposes anything else about the item's pricing (no stack sizes, no
        // listing counts).
        if (ScannedItem const* homeRow = config->FindHouseScan(itemId, homeHouse))
        {
            // Already in memory -- no DB round trip needed, reply immediately.
            NeutralQuote neutral = ResolveNeutralQuote(config, itemId);
            SendMessage(
                player,
                Acore::StringFormat(
                    "{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}",
                    itemId,
                    homeHouseNum,
                    ApplyMargin(homeRow->GetMarketPrice()),
                    1,
                    FindLowestCurrentListing(homeHouse, itemId),
                    neutral.available ? 1 : 0,
                    neutral.floor,
                    neutral.hasRealData ? 1 : 0,
                    neutral.low));
            return;
        }

        // No scan/override data for this player's own house -- ItemPriceSuggestion
        // ::Suggest would run a full-table-scan loot-table query (see its doc
        // comment: neither creature_loot_template nor reference_loot_template can
        // use their composite key for a plain "WHERE Item = ?"). This endpoint is
        // open to every connected player with no GM check, so running that
        // synchronously here is exactly the kind of thing that stalls the whole
        // server long enough to trip the watchdog -- go through the async path
        // instead. player may log out or move on before the query resolves, so
        // capture its GUID and re-resolve inside the callback rather than the raw
        // pointer -- and re-fetch ASConfig too, rather than capturing this
        // request's `config`, since a GM ".auctionsim reload" can replace it
        // (AuctionSim::StartOrReloadBot resets the unique_ptr) while the async
        // loot-table query is still in flight.
        ObjectGuid playerGuid = player->GetGUID();
        ItemPriceSuggestion::SuggestAsync(
            proto, itemId,
            [playerGuid, itemId, homeHouseNum](ItemPriceSuggestion::Suggestion s)
            {
                Player* player = ObjectAccessor::FindPlayer(playerGuid);
                if (!player)
                {
                    return;  // player logged out or moved on before the lookup finished
                }

                AuctionSim* sim = AuctionSim::instance();
                ASConfig const* config = sim ? sim->GetConfig() : nullptr;
                NeutralQuote neutral = config ? ResolveNeutralQuote(config, itemId) : NeutralQuote{};

                SendMessage(
                    player,
                    Acore::StringFormat(
                        "{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}",
                        itemId,
                        homeHouseNum,
                        ApplyMargin(s.marketPrice),
                        0,
                        FindLowestCurrentListing(static_cast<AuctionHouseId>(homeHouseNum), itemId),
                        neutral.available ? 1 : 0,
                        neutral.floor,
                        neutral.hasRealData ? 1 : 0,
                        neutral.low));
            });
    }
}

void AuctionSimPublicPriceBridge::OnPlayerBeforeSendChatMessage(
    Player* player, uint32& /*type*/, uint32& lang, std::string& msg)
{
    if (lang != LANG_ADDON)
    {
        return;
    }

    std::string_view view = msg;
    if (view.substr(0, kFullPrefixWithTab.size()) != kFullPrefixWithTab)
    {
        return;
    }

    std::string_view payload = view.substr(kFullPrefixWithTab.size());
    msg.clear();  // swallow -- never let this reach the client as a visible whisper

    // No auth check, deliberately: this endpoint is open to any connected
    // player and only ever does one thing -- see the header comment.
    HandlePriceRequest(player, payload);
}

void AddAuctionSimPublicPriceBridgeScript() { new AuctionSimPublicPriceBridge(); }
