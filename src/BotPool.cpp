#include "BotPool.h"
#include <charconv>
#include <sstream>
#include <string>
#include "Config.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Tokenize.h"
#include "World.h"

namespace
{
    std::unordered_set<uint32> ParseIdList(std::string const& csv)
    {
        std::unordered_set<uint32> out;
        for (std::string_view tok : Acore::Tokenize(csv, ',', false))
        {
            uint32 id = 0;
            auto [ptr, ec] = std::from_chars(tok.data(), tok.data() + tok.size(), id);
            (void)ptr;
            if (ec == std::errc() && id != 0)
            {
                out.insert(id);
            }
        }
        return out;
    }
}

std::vector<std::pair<uint32, uint32>> BotPool::ResolveRosterIds()
{
    std::vector<std::pair<uint32, uint32>> result;
    std::unordered_set<uint32> seenCharacterIDs;

    std::unordered_set<uint32> explicitCharIDs =
        ParseIdList(sConfigMgr->GetOption<std::string>("AuctionSim.BotCharacterIDs", ""));
    std::unordered_set<uint32> accountIDs =
        ParseIdList(sConfigMgr->GetOption<std::string>("AuctionSim.BotAccountIDs", ""));

    // Backward compatibility: an old auctionsim.conf may still only have the
    // deprecated single-value keys. Fold them in if the new list keys are unset.
    if (explicitCharIDs.empty() && accountIDs.empty())
    {
        uint32 legacyCharID = sConfigMgr->GetOption<uint32>("AuctionSim.BotCharacterID", 0);
        uint32 legacyAccountID = sConfigMgr->GetOption<uint32>("AuctionSim.BotAccountID", 0);
        if (legacyCharID != 0)
        {
            explicitCharIDs.insert(legacyCharID);
        }
        if (legacyAccountID != 0)
        {
            accountIDs.insert(legacyAccountID);
        }
    }

    // Explicit character ids: resolve each to its owning account so every
    // roster entry has a valid account for the WorldSession ctor.
    for (uint32 charID : explicitCharIDs)
    {
        QueryResult qr = CharacterDatabase.Query(
            "SELECT account FROM characters WHERE guid = {}", charID);
        if (!qr)
        {
            LOG_ERROR("module", "AuctionSim: BotCharacterIDs entry {} not found in characters table, skipping", charID);
            continue;
        }
        uint32 account = qr->Fetch()[0].Get<uint32>();
        if (seenCharacterIDs.insert(charID).second)
        {
            result.emplace_back(account, charID);
        }
    }

    // Account ids: expand to every character on the account.
    for (uint32 accountID : accountIDs)
    {
        QueryResult qr = CharacterDatabase.Query(
            "SELECT guid FROM characters WHERE account = {}", accountID);
        if (!qr)
        {
            LOG_ERROR("module", "AuctionSim: BotAccountIDs entry {} has no characters, skipping", accountID);
            continue;
        }
        do
        {
            uint32 charID = qr->Fetch()[0].Get<uint32>();
            if (seenCharacterIDs.insert(charID).second)
            {
                result.emplace_back(accountID, charID);
            }
        } while (qr->NextRow());
    }

    return result;
}

BotPool::BotPool(bool& outBuilt)
{
    std::vector<std::pair<uint32, uint32>> ids = ResolveRosterIds();
    if (ids.empty())
    {
        LOG_ERROR("module", "AuctionSim: no bot characters resolved from BotCharacterIDs/BotAccountIDs");
        outBuilt = false;
        return;
    }

    for (auto const& [accountID, characterID] : ids)
    {
        LoginDatabasePreparedStatement* stmt = LoginDatabase.GetPreparedStatement(LOGIN_GET_USERNAME_BY_ID);
        stmt->SetData(0, accountID);
        PreparedQueryResult result = LoginDatabase.Query(stmt);
        if (!result)
        {
            LOG_ERROR("module", "AuctionSim: BotPool: no account {} in login db, skipping character {}", accountID, characterID);
            continue;
        }

        std::string accountName = result->Fetch()[0].Get<std::string>();

        Entry entry;
        entry.accountID = accountID;
        entry.characterID = characterID;
        entry.session = std::make_unique<WorldSession>(
            accountID,
            std::move(accountName),
            0,
            nullptr,
            SEC_PLAYER,
            sWorld->getIntConfig(CONFIG_EXPANSION),
            0,
            LOCALE_enUS,
            0,
            false,
            false,
            0,
            true);  // is_bot: this fork's WorldSession (mod-playerbots/Grimfeather branch) adds a
                    // trailing is_bot flag for exactly this case -- a headless, socket-less session
                    // driving a real character. Marking it true keeps AuctionSim's bots consistent
                    // with how the core's own playerbots identify themselves, in case any playerbots-
                    // side logic (chat routing, bot-vs-real-player bookkeeping) branches on IsBot().
        entry.player = std::make_unique<Player>(entry.session.get());
        entry.player->Initialize(characterID);

        ownedGuids.insert(entry.player->GetGUID());
        ownedLowGuids.insert(characterID);
        entries.push_back(std::move(entry));
    }

    if (entries.empty())
    {
        LOG_ERROR("module", "AuctionSim: BotPool: every configured id failed to resolve");
        outBuilt = false;
        return;
    }

    LOG_INFO("module", "AuctionSim: bot roster active ({} characters)", entries.size());
    outBuilt = true;
}

Player& BotPool::NextPlayer()
{
    Player& p = *entries[nextIndex % entries.size()].player;
    ++nextIndex;
    return p;
}
