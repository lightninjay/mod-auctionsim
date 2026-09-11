#include "AuctionSim.h"
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>
#include "ASConfig.h"
#include "AuctionHouseMgr.h"
#include "AuctionHouseSearcher.h"
#include "AuctionPricing.h"
#include "BotPool.h"
#include "Config.h"
#include "DatabaseEnvFwd.h"
#include "Define.h"
#include "Log.h"
#include "Mail.h"
#include "ScriptMgr.h"
#include "WorldConfig.h"

namespace
{
    // Collects every bot-owned auction on `houseId` that `shouldRemove` accepts, then
    // deletes them in a second pass -- so the live house map is never mutated while it
    // is being iterated, and it is never copied. Returns the number removed.
    // "Bot-owned" now means owned by any character in the roster, not a single GUID.
    template <typename Predicate>
    uint32 RemoveBotAuctionsIf(
        AuctionHouseId houseId,
        BotPool const& pool,
        SQLTransaction<CharacterDatabaseConnection>& trans,
        Predicate shouldRemove)
    {
        AuctionHouseObject* house = sAuctionMgr->GetAuctionsMapByHouseId(houseId);

        std::vector<AuctionEntry*> toRemove;
        for (auto const& entry : house->GetAuctions())
        {
            AuctionEntry* auction = entry.second;
            if (pool.Owns(auction->owner) && shouldRemove(auction))
            {
                toRemove.push_back(auction);
            }
        }

        for (AuctionEntry* auction : toRemove)
        {
            auction->DeleteFromDB(trans);
            sAuctionMgr->RemoveAItem(auction->item_guid, true, &trans);
            house->RemoveAuction(auction);
        }
        return static_cast<uint32>(toRemove.size());
    }

    bool IsBotCharacter(uint32 lowGuid)
    {
        // Read the running roster rather than re-parsing config: this hook fires
        // for every mail delivered server-wide.
        AuctionSim* sim = AuctionSim::instance();
        return sim && sim->IsBotOwnedLowGuid(lowGuid);
    }
}

namespace
{
    // Alliance/Horde always scanned; Neutral only when configured on. Centralizes
    // the set so OnStartup/OnUpdate/CleanOverCapAuctions/DeleteAuctions/RunTests
    // stay in sync instead of each hardcoding their own {Alliance, Horde} pair.
    std::vector<AuctionHouseId> ActiveHouses(ASConfig const* config)
    {
        std::vector<AuctionHouseId> houses{AuctionHouseId::Alliance, AuctionHouseId::Horde};
        if (config && config->enableNeutralAH)
        {
            houses.push_back(AuctionHouseId::Neutral);
        }
        return houses;
    }
}

AuctionSim* AuctionSim::_instance = nullptr;

AuctionSim::AuctionSim() : WorldScript("AuctionSim")
{
    _instance = this;
    isEnabled = sConfigMgr->GetOption<bool>("AuctionSim.Enabled", false);
    startupScan = sConfigMgr->GetOption<bool>("AuctionSim.StartupScan", false);
}

bool AuctionSim::EnsureConfigFileExists()
{
    std::filesystem::path dir = std::filesystem::path(sConfigMgr->GetConfigPath()) / "modules";
    std::filesystem::path livePath = dir / "auctionsim.conf";
    std::filesystem::path distPath = dir / "auctionsim.conf.dist";

    std::error_code ec;
    if (std::filesystem::exists(livePath, ec))
    {
        return false;
    }
    if (!std::filesystem::exists(distPath, ec))
    {
        LOG_ERROR("module", "AuctionSim: neither auctionsim.conf nor auctionsim.conf.dist found in {}", dir.string());
        return false;
    }

    std::filesystem::copy_file(distPath, livePath, ec);
    if (ec)
    {
        LOG_ERROR("module", "AuctionSim: couldn't create {}: {}", livePath.string(), ec.message());
        return false;
    }
    LOG_INFO("module", "AuctionSim: created auctionsim.conf from auctionsim.conf.dist");
    return true;
}

void AuctionSim::OnStartup()
{
    if (EnsureConfigFileExists())
    {
        // File didn't exist when ConfigMgr loaded module configs; pull it in now so
        // ASConfig and Bot below read real values instead of defaults.
        sConfigMgr->Reload();
    }

    // Load auctionsim.dat unconditionally: the addon shows/edits the listing table
    // whether or not the module is enabled.
    {
        bool datOk = true;
        config = std::make_unique<ASConfig>(sConfigMgr->GetConfigPath() + "/modules/auctionsim.dat", datOk);
        if (!datOk)
        {
            LOG_ERROR("module", "AuctionSim: auctionsim.dat failed to load");
            config.reset();
        }
    }

    if (!isEnabled)
    {
        // The addon still works while disabled (replies are self-whispers), so a GM
        // can configure everything and enable without a restart.
        LOG_WARN("module", "AuctionSim is disabled!");
        return;
    }

    if (!config)
    {
        LOG_ERROR("module", "AuctionSim: disabling -- auctionsim.dat is required to run");
        isEnabled = false;
        return;
    }

    if (ServerConfigs::CONFIG_ALLOW_TWO_SIDE_INTERACTION_AUCTION == 1)
    {
        LOG_ERROR("module", "AuctionSim: Two sided auction interaction is not allowed");
        isEnabled = false;
        return;
    }

    if (!StartOrReloadBot(false))  // config is fresh at boot; no reload
    {
        isEnabled = false;
        return;
    }

    if (this->startupScan)
    {
        for (AuctionHouseId houseId : ActiveHouses(config.get()))
        {
            ScanAuctions(houseId);
        }
        LOG_INFO("module", "AuctionSim: Startup complete");
    }
}

bool AuctionSim::StartOrReloadBot(bool reloadConfig)
{
    if (!config)
    {
        return false;
    }

    // Reload so BotPool's constructor and the mail hook see any config change --
    // e.g. an admin editing BotCharacterIDs/BotAccountIDs directly in
    // auctionsim.conf. A failed reload leaves any running roster alone.
    if (reloadConfig && !sConfigMgr->Reload())
    {
        return false;
    }

    // Throwaway flag: a roster that fails to resolve must not clear the module's
    // isEnabled or kill a running roster.
    bool built = true;
    auto newPool = std::make_unique<BotPool>(built);
    if (!built || newPool->Empty())
    {
        return false;
    }

    // Retire the old roster rather than destroying it (each headless Player is only
    // ever torn down at shutdown); rebuild the services, which hold a BotPool&.
    if (botPool)
    {
        retiredBotPools.push_back(std::move(botPool));
    }
    botPool = std::move(newPool);
    listingService = std::make_unique<AuctionListingService>(*botPool, *config);
    buyingService = std::make_unique<AuctionBuyingService>(*botPool, *config);

    LOG_INFO("module", "AuctionSim: bot roster active ({} characters)", botPool->Size());
    return true;
}

void AuctionSim::OnUpdate(uint32 diff)
{
    // isEnabled can be set before a bot exists (enabled via the addon), so check both
    if (!this->isEnabled || !buyingService) return;

    scanTimer += diff;

    if (scanTimer >= config->scanIntervalSeconds * 1000)
    {
        for (AuctionHouseId houseId : ActiveHouses(config.get()))
        {
            ScanAuctions(houseId);
        }
        scanTimer = 0;
    }

    buyingService->ProcessDueQueue();
}

void AuctionSim::ScanAuctions(AuctionHouseId _AuctionHouseId)
{
    // const& -- GetAuctions() returns the live map by reference; a by-value `auto`
    // would deep-copy every auction node on the house each scan.
    auto const& auctions = sAuctionMgr->GetAuctionsMapByHouseId(_AuctionHouseId)->GetAuctions();
    int auctionTable[MAX_ITEM_CLASS][MAX_ITEM_QUALITY] = {};
    std::unordered_map<uint32, int> itemAuctionCount;

    buyingService->RollTolerance();

    for (auto it = auctions.begin(); it != auctions.end(); ++it)
    {
        AuctionEntry* auction = it->second;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(auction->item_template);
        if (!proto)
        {
            LOG_WARN(
                "module",
                "AuctionSim: auction {} references item {} not found in item_template, skipping",
                auction->Id,
                auction->item_template);
            continue;
        }

        auctionTable[proto->Class][proto->Quality]++;
        itemAuctionCount[auction->item_template]++;

        if (botPool->Owns(auction->owner))
        {
            continue;
        }

        ScannedItem const* scannedItem =
            config->FindScannedItem(_AuctionHouseId, proto->Class, proto->Quality, auction->item_template);
        if (!scannedItem)
        {
            continue;
        }

        uint32 pricePerItem = auction->buyout / auction->itemCount;

        // Never buy grey items -- no real market for them, and any that turn up
        // are noise, not a legitimate craft/farm alternative worth pricing against.
        if (!AuctionPricing::IsBuyableQuality(proto->Quality))
        {
            continue;
        }

        buyingService->ConsiderForPurchase(
            auction, pricePerItem, scannedItem->GetMarketPrice(), scannedItem->GetBuyCeiling());
    }

    buyingService->SortQueue();

    listingService->ListNewAuctions(_AuctionHouseId, auctionTable, itemAuctionCount);
}

std::vector<AuctionSimTests::TestResult> AuctionSim::RunTests()
{
    std::vector<AuctionSimTests::TestResult> results = AuctionSimTests::RunLogicTests(*botPool, *config);

    for (AuctionHouseId houseId : ActiveHouses(config.get()))
    {
        results.push_back(
            AuctionSimTests::RunLiveListingTest(*botPool, *config, *listingService, houseId));
        results.push_back(
            AuctionSimTests::RunLiveBuyingTest(*botPool, *config, *listingService, houseId));
        results.push_back(
            AuctionSimTests::RunLiveLevelCapTest(*botPool, *config, *listingService, houseId));
        results.push_back(
            AuctionSimTests::RunLiveItemExceptionTest(*botPool, *config, *listingService, houseId));
    }

    return results;
}

AuctionSim::BuyQueueStatus AuctionSim::GetBuyQueueStatus(time_t now) const
{
    auto const& queue = buyingService->GetQueue();
    if (queue.empty())
    {
        return {};
    }

    // SortQueue keeps the soonest-due purchase at the back and the furthest-due at
    // the front.
    return {queue.size(), queue.back().buyTime - now, queue.front().buyTime - now};
}

AuctionBuyingService::ForceBuyResult AuctionSim::ForceNextBuy()
{
    // Same convention as GetBuyQueueStatus above: callers (the ".auctionsim" and
    // addon-bridge command handlers) are expected to have already gated on
    // RequireEnabled, since buyingService only exists while the module is.
    return buyingService->ForceNextBuy();
}

uint32 AuctionSim::CleanOverCapAuctions()
{
    if (!botPool || botPool->Empty() || !config)
    {
        return 0;
    }

    auto trans = CharacterDatabase.BeginTransaction();
    uint32 removedCount = 0;

    for (AuctionHouseId houseId : ActiveHouses(config.get()))
    {
        // Captures houseId (by value, fresh each iteration) so an item excluded
        // from only one house doesn't get swept off every house it's listed on.
        auto isOverCap = [this, houseId](AuctionEntry const* auction) {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(auction->item_template);
            if (!proto)
            {
                return false;  // can't judge it -- leave it alone
            }
            if (!AuctionPricing::IsWithinLevelCap(
                    proto->RequiredLevel, proto->ItemLevel, config->maxRequiredLevel, config->maxItemLevel))
            {
                return true;
            }
            // Catches an item that was listed before a GM added it to
            // AuctionSim.ItemExceptions for this house.
            return config->IsItemExcludedFromHouse(auction->item_template, houseId);
        };

        removedCount += RemoveBotAuctionsIf(houseId, *botPool, trans, isOverCap);
    }

    CharacterDatabase.CommitTransaction(trans);
    LOG_INFO("module", "AuctionSim: cleaned {} over-cap auctions", removedCount);
    return removedCount;
}

void AuctionSim::DeleteAuctions()
{
    if (!botPool || botPool->Empty())
    {
        return;
    }

    auto trans = CharacterDatabase.BeginTransaction();

    for (AuctionHouseId houseId : ActiveHouses(config.get()))
    {
        RemoveBotAuctionsIf(houseId, *botPool, trans, [](AuctionEntry const*) { return true; });
    }

    CharacterDatabase.CommitTransaction(trans);
}

void AuctionSimMailManager::OnBeforeMailDraftSendMailTo(
    MailDraft* /*mailDraft*/,
    MailReceiver const& receiver,
    MailSender const& sender,
    MailCheckMask& /*checked*/,
    uint32& /*deliver_delay*/,
    uint32& /*custom_expiration*/,
    bool& deleteMailItemsFromDB,
    bool& sendMail)
{
    if (IsBotCharacter(receiver.GetPlayerGUIDLow()))
    {
        sendMail = false;
        if (sender.GetMailMessageType() == MAIL_AUCTION)
        {
            deleteMailItemsFromDB = true;
        }
    }
}

void AddAuctionSimScripts()
{
    new AuctionSim();
    new AuctionSimMailManager();
}
