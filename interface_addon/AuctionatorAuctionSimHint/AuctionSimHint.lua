-- Adds an "AuctionSim (guaranteed buy floor)" price hint to Auctionator's Sell
-- tab, using the exact same extension point Auctionator already uses for
-- third-party price sources like The Undermine Journal or Wowecon
-- (Atr_BuildHints / Atr_AppendHint, see AuctionatorHints.lua) -- nothing in
-- Auctionator itself is modified.
--
-- This is read-only in both directions: it only ever sends an item id to the
-- server and receives a suggested price back. It cannot change anything
-- AuctionSim's own pricing data, config, or bot state in any way -- the
-- server-side endpoint it talks to (AuctionSimPublicPriceBridge, addon message
-- prefix "AHSIMPRICE") is deliberately a single-purpose, read-only channel,
-- separate from the GM-only "AHSIM" channel the Bot Manager addon uses.
--
-- Available to any character -- unlike the Bot Manager addon, no GM rights
-- are required, since the server endpoint doesn't check for any.

local PREFIX = "AHSIMPRICE"
local CACHE_TTL_SECONDS = 300  -- re-request if our cached answer is older than this

local cache = {}    -- itemId (number) -> { price = copper, hasRealData = bool, at = GetTime() }
local pending = {}  -- itemId (number) -> true while a request is in flight

local function RequestPrice(itemId)
    if pending[itemId] then
        return
    end
    local cached = cache[itemId]
    if cached and (GetTime() - cached.at) < CACHE_TTL_SECONDS then
        return
    end
    pending[itemId] = true
    SendAddonMessage(PREFIX, tostring(itemId), "WHISPER", UnitName("player"))
end

-- itemLink looks like "|cffffffff|Hitem:2589:0:0:0:0:0:0:0:0|h[Linen Cloth]|h|r"
-- -- pull the numeric id out of the Hitem: segment without needing any of
-- Auctionator's own (private, inaccessible from here) link-parsing helpers.
local function ItemIdFromLink(itemLink)
    if not itemLink then
        return nil
    end
    local id = itemLink:match("item:(%d+)")
    return id and tonumber(id) or nil
end

local eventFrame = CreateFrame("Frame")
eventFrame:RegisterEvent("CHAT_MSG_ADDON")
eventFrame:SetScript("OnEvent", function(self, event, prefix, message)
    if event ~= "CHAT_MSG_ADDON" or prefix ~= PREFIX then
        return
    end

    local itemIdStr, priceStr, hasRealDataStr = strsplit("\t", message)
    local itemId = tonumber(itemIdStr)
    if not itemId then
        return
    end

    pending[itemId] = nil

    local price = tonumber(priceStr) or 0
    if price <= 0 then
        return  -- server had nothing to suggest (unknown item, module not ready yet)
    end

    cache[itemId] = { price = price, hasRealData = tonumber(hasRealDataStr) == 1, at = GetTime() }
end)

-- Warms the cache as soon as an item's tooltip is shown anywhere (bags, bank,
-- AH, etc.) -- broad and cheap, and means the hint usually already has data by
-- the time a player actually opens the Sell tab for that item, rather than
-- always being one query behind. Every item tooltip triggers this, not just
-- ones Auctionator itself is currently looking at.
GameTooltip:HookScript("OnTooltipSetItem", function(tooltip)
    local _, itemLink = tooltip:GetItem()
    local itemId = ItemIdFromLink(itemLink)
    if itemId then
        RequestPrice(itemId)
    end
end)

-- Auctionator's own Atr_AppendHint (AuctionatorHints.lua) is declared local to
-- that file, so it isn't reachable from here -- this replicates its exact
-- behavior (same three fields, same price > 0 guard) rather than calling it.
local function AppendHint(results, price, text)
    if price and price > 0 then
        table.insert(results, { price = price, text = text })
    end
end

-- The actual hint injection. Atr_BuildHints returning a hint for an item on
-- its FIRST-ever view is not possible -- the request is asynchronous and this
-- function must return synchronously -- so the first call for a never-before-
-- seen item kicks off the request (via RequestPrice) but won't have data yet;
-- a second look (re-opening the Sell tab, or just having hovered the item
-- first, per the tooltip hook above) will show it once the reply arrives.
if type(Atr_BuildHints) == "function" then
    local originalBuildHints = Atr_BuildHints
    Atr_BuildHints = function(itemName, itemLink)
        local results = originalBuildHints(itemName, itemLink)

        local itemId = ItemIdFromLink(itemLink)
        if not itemId then
            return results
        end

        local cached = cache[itemId]
        if cached then
            local label = cached.hasRealData
                and "AuctionSim (guaranteed buy floor)"
                or "AuctionSim (estimated -- no market data yet)"
            AppendHint(results, cached.price, label)
        else
            RequestPrice(itemId)
        end

        return results
    end
end
