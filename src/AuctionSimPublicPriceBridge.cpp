#include "AuctionSimPublicPriceBridge.h"
#include <algorithm>
#include <charconv>
#include <string>
#include <string_view>
#include "ASConfig.h"
#include "AuctionSim.h"
#include "Common.h"
#include "Config.h"
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

    void HandlePriceRequest(Player* player, std::string_view payload)
    {
        uint32 itemId = 0;
        if (!ParseUint32(payload, itemId) || itemId == 0)
        {
            SendMessage(player, "0\t0\t0");  // malformed request -- reply with an
                                              // obviously-invalid result rather
                                              // than nothing, so the client isn't
                                              // left waiting forever.
            return;
        }

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        if (!proto)
        {
            SendMessage(player, Acore::StringFormat("{}\t0\t0", itemId));
            return;
        }

        AuctionSim* sim = AuctionSim::instance();
        ASConfig const* config = sim ? sim->GetConfig() : nullptr;
        if (!config)
        {
            // Module hasn't finished loading its pricing data yet.
            SendMessage(player, Acore::StringFormat("{}\t0\t0", itemId));
            return;
        }

        // hasRealData (the third field) distinguishes real scan/GM-priced data
        // from a pure ItemPriceSuggestion guess, so the client can show an
        // appropriate confidence level -- this endpoint never says which it
        // used beyond that one flag, and never exposes anything else about the
        // item's pricing (no stack sizes, no per-house breakdown, no listing
        // counts).
        if (ScannedItem const* row = config->FindAnyScan(itemId))
        {
            // Already in memory -- no DB round trip needed, reply immediately.
            SendMessage(player, Acore::StringFormat("{}\t{}\t{}", itemId, ApplyMargin(row->GetMarketPrice()), 1));
            return;
        }

        // No scan/override data -- ItemPriceSuggestion::Suggest would run a
        // full-table-scan loot-table query (see its doc comment: neither
        // creature_loot_template nor reference_loot_template can use their
        // composite key for a plain "WHERE Item = ?"). This endpoint is open to
        // every connected player with no rate limiting, so running that
        // synchronously here is exactly the kind of thing that stalls the whole
        // server long enough to trip the watchdog -- go through the async path
        // instead. player may log out or move on before the query resolves, so
        // capture its GUID and re-resolve inside the callback rather than the
        // raw pointer.
        ObjectGuid playerGuid = player->GetGUID();
        ItemPriceSuggestion::SuggestAsync(
            proto, itemId,
            [playerGuid, itemId](ItemPriceSuggestion::Suggestion s)
            {
                Player* player = ObjectAccessor::FindPlayer(playerGuid);
                if (!player)
                {
                    return;  // player logged out or moved on before the lookup finished
                }
                SendMessage(player, Acore::StringFormat("{}\t{}\t{}", itemId, ApplyMargin(s.marketPrice), 0));
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
