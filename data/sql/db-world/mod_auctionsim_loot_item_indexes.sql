-- Adds a secondary index on `Item` to creature_loot_template and
-- reference_loot_template.
--
-- ItemPriceSuggestion::Suggest / SuggestAsync query both tables with
-- "WHERE Item = ?" (see ItemPriceSuggestion.cpp), but neither table's primary
-- key leads with Item:
--   creature_loot_template  PRIMARY KEY (Entry, Item, Reference, GroupId)
--   reference_loot_template PRIMARY KEY (Entry, Item)
-- so that filter can't use the primary key and falls back to a full table
-- scan every call. Moving the lookup onto AuctionSim's async query plumbing
-- (see AuctionSim::AddQueryCallback) stopped that scan from freezing the main
-- thread; this index is what makes the lookup itself fast, cutting per-call
-- latency from a full scan down to an index seek. Both together are the fix --
-- async alone just moves the same slow scan onto a DB worker thread.
--
-- Guarded with an INFORMATION_SCHEMA existence check rather than a bare
-- `CREATE INDEX IF NOT EXISTS` for portability: IF NOT EXISTS on indexes isn't
-- supported by every MySQL/MariaDB version AzerothCore installs run on.
-- AzerothCore's DB updater applies this file automatically on next server
-- start (tracked like any other module SQL file); no manual step needed
-- unless Updates.EnableDatabases has world-DB updates turned off, in which
-- case run this file directly against your world database.

DELIMITER $$
CREATE PROCEDURE `mod_auctionsim_add_loot_item_indexes`()
BEGIN
    IF NOT EXISTS (
        SELECT 1 FROM INFORMATION_SCHEMA.STATISTICS
        WHERE TABLE_SCHEMA = DATABASE()
          AND TABLE_NAME = 'creature_loot_template'
          AND INDEX_NAME = 'idx_mod_auctionsim_item'
    ) THEN
        ALTER TABLE `creature_loot_template` ADD INDEX `idx_mod_auctionsim_item` (`Item`);
    END IF;

    IF NOT EXISTS (
        SELECT 1 FROM INFORMATION_SCHEMA.STATISTICS
        WHERE TABLE_SCHEMA = DATABASE()
          AND TABLE_NAME = 'reference_loot_template'
          AND INDEX_NAME = 'idx_mod_auctionsim_item'
    ) THEN
        ALTER TABLE `reference_loot_template` ADD INDEX `idx_mod_auctionsim_item` (`Item`);
    END IF;
END$$
DELIMITER ;

CALL `mod_auctionsim_add_loot_item_indexes`();
DROP PROCEDURE `mod_auctionsim_add_loot_item_indexes`;
