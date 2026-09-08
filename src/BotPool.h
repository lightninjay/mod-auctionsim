#pragma once
#include <memory>
#include <unordered_set>
#include <vector>
#include "Define.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "WorldSession.h"

// A roster of headless characters the module drives to list and buy auctions.
// Replaces the single-Bot design: AuctionSim.BotCharacterIDs (explicit GUIDs)
// and AuctionSim.BotAccountIDs (every character on the account, expanded via
// SQL) are unioned into one roster. Each entry's Player is only ever torn
// down at shutdown -- same lifetime contract the old Bot had -- so a reload
// retires the whole old roster into AuctionSim::retiredBots rather than
// destroying it.
class BotPool
{
public:
    struct Entry
    {
        uint32 accountID = 0;
        uint32 characterID = 0;
        std::unique_ptr<WorldSession> session;
        std::unique_ptr<Player> player;
    };

    // outBuilt is false if no configured id resolved to a real character --
    // a construction-success flag, not the module's enabled state. Mirrors
    // the old Bot ctor's contract.
    explicit BotPool(bool& outBuilt);

    size_t Size() const { return entries.size(); }
    bool Empty() const { return entries.empty(); }

    // Non-owning reference to one roster member's Player, chosen round-robin
    // across the pool so listings/buys spread across characters instead of
    // piling onto one. Only call once Empty() is known false.
    Player& NextPlayer();

    // O(1) "is this GUID one of ours" check -- replaces the old single-GUID
    // equality check at every site that used to compare against
    // bot->GetPlayer()->GetGUID() (mail hook, ScanAuctions self-skip,
    // CleanOverCapAuctions/DeleteAuctions ownership filter, addon bridge).
    bool Owns(ObjectGuid guid) const { return ownedGuids.count(guid) > 0; }
    bool OwnsLowGuid(uint32 lowGuid) const { return ownedLowGuids.count(lowGuid) > 0; }

    std::vector<Entry> const& GetEntries() const { return entries; }

private:
    std::vector<Entry> entries;
    std::unordered_set<ObjectGuid> ownedGuids;
    std::unordered_set<uint32> ownedLowGuids;
    size_t nextIndex = 0;

    // Reads AuctionSim.BotCharacterIDs + AuctionSim.BotAccountIDs, expands
    // account ids into their characters, and returns the de-duplicated
    // (accountID, characterID) pairs to build. Empty on any config/DB failure.
    static std::vector<std::pair<uint32, uint32>> ResolveRosterIds();
};
