-- Adds AuctionSim price hints to Auctionator's Sell tab, using the exact same
-- extension point Auctionator already uses for third-party price sources like
-- The Undermine Journal or Wowecon (Atr_BuildHints / Atr_AppendHint, see
-- AuctionatorHints.lua) -- nothing in Auctionator itself is modified.
--
-- This is read-only in both directions: it only ever sends an item id to the
-- server and receives suggested prices back. It cannot change anything in
-- AuctionSim's own pricing data, config, or bot state in any way -- the
-- server-side endpoint it talks to (AuctionSimPublicPriceBridge, addon message
-- prefix "AHSIMPRICE") is deliberately a single-purpose, read-only channel,
-- separate from the GM-only "AHSIM" channel the Bot Manager addon uses.
--
-- Available to any character -- unlike the Bot Manager addon, no GM rights
-- are required, since the server endpoint doesn't check for any.
--
-- Every quote from the server is specific to a single auction house. A player
-- can only ever browse their own faction's house plus the Neutral (goblin)
-- house, so the server always answers with the guaranteed buy floor for the
-- player's own house AND, separately, whether/what Neutral will pay -- never
-- a single faction-ambiguous number. That matters because AuctionSim's bot
-- only ever buys a listing against the ScannedItem row for the house it was
-- actually posted on: an Alliance player pricing off a Horde-derived number
-- (or vice versa) can end up with a listing the bot will simply never buy.
-- Neutral is reported as unavailable outright for the (common) case of an
-- item that isn't on the server's curated Neutral allowlist -- config-limited,
-- not a market fact -- so this hook never quotes a Neutral price the Neutral
-- bot could never actually pay.

local PREFIX = "AHSIMPRICE"
local CACHE_TTL_SECONDS = 300  -- re-request if our cached answer is older than this

-- itemId (number) -> {
--   homeHouse = 2|6,             -- AuctionHouseId of the player's own faction
--   homeFloor = copper,          -- guaranteed buy floor on the player's own house, 0 if none
--   homeHasRealData = bool,      -- true = real scan/GM price, false = rough estimate only
--   homeLow = copper,            -- current observed low end of that house's market (0 if none)
--   neutralAvailable = bool,     -- false = guaranteed no-sale on Neutral, no matter the price
--   neutralFloor = copper,       -- guaranteed buy floor on Neutral, 0 if none/unavailable
--   neutralHasRealData = bool,
--   neutralLow = copper,
--   at = GetTime(),
-- }
local cache = {}
local pending = {}  -- itemId (number) -> true while a request is in flight

local HOUSE_NAMES = { [2] = "Alliance", [6] = "Horde" }

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

local function ToBool01(str)
    return tonumber(str) == 1
end

local eventFrame = CreateFrame("Frame")
eventFrame:RegisterEvent("CHAT_MSG_ADDON")
eventFrame:SetScript("OnEvent", function(self, event, prefix, message)
    if event ~= "CHAT_MSG_ADDON" or prefix ~= PREFIX then
        return
    end

    -- itemId, homeHouse, homeFloor, homeHasRealData, homeLow,
    -- neutralAvailable, neutralFloor, neutralHasRealData, neutralLow
    local itemIdStr, homeHouseStr, homeFloorStr, homeRealStr, homeLowStr,
          neutralAvailStr, neutralFloorStr, neutralRealStr, neutralLowStr = strsplit("\t", message)

    local itemId = tonumber(itemIdStr)
    if not itemId then
        return
    end

    pending[itemId] = nil

    local homeFloor = tonumber(homeFloorStr) or 0
    local neutralFloor = tonumber(neutralFloorStr) or 0
    local neutralAvailable = ToBool01(neutralAvailStr)

    if homeFloor <= 0 and not (neutralAvailable and neutralFloor > 0) then
        return  -- server had nothing at all to suggest (unknown item, module not ready yet)
    end

    cache[itemId] = {
        homeHouse = tonumber(homeHouseStr) or 0,
        homeFloor = homeFloor,
        homeHasRealData = ToBool01(homeRealStr),
        homeLow = tonumber(homeLowStr) or 0,
        neutralAvailable = neutralAvailable,
        neutralFloor = neutralFloor,
        neutralHasRealData = ToBool01(neutralRealStr),
        neutralLow = tonumber(neutralLowStr) or 0,
        at = GetTime(),
    }
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

-- Builds the one-or-two hint lines for a cached quote. Always shows the
-- player's own house first (labelled with the house name, so a player who
-- somehow still ends up looking at the wrong price notices immediately), then
-- Neutral's guaranteed floor as its own separate line when Neutral will
-- actually buy this item at all. When Neutral is allowlist-limited and can
-- never buy it, no Neutral line is added -- silence here is the signal to
-- sell elsewhere, rather than a line quoting a price that would never clear.
local function BuildHintLines(results, cached)
    local houseName = HOUSE_NAMES[cached.homeHouse] or "home AH"

    if cached.homeFloor > 0 then
        local label
        if cached.homeHasRealData then
            label = "AuctionSim (" .. houseName .. " guaranteed floor)"
        else
            label = "AuctionSim (" .. houseName .. " estimate -- no market data yet)"
        end
        AppendHint(results, cached.homeFloor, label)
    end

    if cached.neutralAvailable and cached.neutralFloor > 0 then
        local label = cached.neutralHasRealData
            and "AuctionSim (Neutral guaranteed floor)"
            or "AuctionSim (Neutral estimate -- no market data yet)"
        AppendHint(results, cached.neutralFloor, label)
    end
end

-- The actual hint injection. Atr_BuildHints returning a hint for an item on
-- its FIRST-ever view is not possible -- the request is asynchronous and this
-- function must return synchronously -- so the first call for a never-before-
-- seen item kicks off the request (via RequestPrice) but won't have data yet;
-- a second look (re-opening the Sell tab, or just having hovered the item
-- first, per the tooltip hook above) will show it once the reply arrives.
--
-- IMPORTANT CAVEAT: Atr_BuildHints is only ever called by Auctionator when the
-- Sell tab has NOTHING else to show -- see Atr_OnSearchComplete in
-- Auctionator.lua: it only builds/displays hints when
-- gCurrentPane.activeScan.scanData is empty (no live AH listings found for
-- this item) AND there's no scan history either. The moment an item has any
-- current AH listings or any scan history at all, Auctionator switches to its
-- own "Current"/"History" panel instead and never calls Atr_BuildHints, so
-- this hook alone cannot guarantee the floor is shown -- it's a genuine
-- fallback-only path by Auctionator's own design, not a bug here. That's what
-- the always-on panel below is for.
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
            BuildHintLines(results, cached)
        else
            RequestPrice(itemId)
        end

        return results
    end
end

------------------------------------------------------------------------------
-- Always-on panel: shows the AuctionSim floor(s) unconditionally, regardless
-- of whether Auctionator is currently displaying Current/History/Hints for
-- the item -- Atr_BuildHints above cannot do this by itself (see the caveat
-- just above). Deliberately independent of every one of Auctionator's own
-- display-mode internals (gCurrentPane, activeScan, activeSearch, etc. are
-- all `local` to Auctionator.lua and unreachable from this file anyway): this
-- panel drives itself straight off the same Blizzard API Auctionator's own
-- sell-item button uses --
-- GetAuctionSellItemInfo()/NEW_AUCTION_UPDATE (see Atr_SellControls's
-- $parent_Tex button in Auctionator.xml, which registers exactly that event
-- for exactly this reason). It only anchors to Atr_SellControls for
-- positioning and Show/Hide timing, which is the one piece of Auctionator UI
-- that's a plain global frame (named in XML) rather than a Lua-local.
------------------------------------------------------------------------------

local panel = CreateFrame("Frame", "AuctionSimHintPanel", UIParent)
panel:SetSize(190, 54)
panel:SetBackdrop({
    bgFile = "Interface\\Tooltips\\UI-Tooltip-Background",
    edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
    edgeSize = 16,
    insets = { left = 4, right = 4, top = 4, bottom = 4 },
})
panel:SetBackdropColor(0, 0, 0, 0.8)
panel:Hide()

local panelTitle = panel:CreateFontString(nil, "ARTWORK", "GameFontNormalSmall")
panelTitle:SetPoint("TOPLEFT", 8, -6)
panelTitle:SetText("AuctionSim")

local panelHomeLine = panel:CreateFontString(nil, "ARTWORK", "GameFontHighlightSmall")
panelHomeLine:SetPoint("TOPLEFT", panelTitle, "BOTTOMLEFT", 0, -4)
panelHomeLine:SetJustifyH("LEFT")
panelHomeLine:SetWidth(174)

local panelNeutralLine = panel:CreateFontString(nil, "ARTWORK", "GameFontHighlightSmall")
panelNeutralLine:SetPoint("TOPLEFT", panelHomeLine, "BOTTOMLEFT", 0, -2)
panelNeutralLine:SetJustifyH("LEFT")
panelNeutralLine:SetWidth(174)

-- Make the panel a child of Atr_SellControls. This is important: Auctionator's
-- main window is above UIParent's normal frame strata, so a UIParent child
-- positioned just below SellControls can be painted BEHIND Auctionator's own
-- background. Parenting it to SellControls guarantees it renders above that
-- background while still allowing the panel to extend below the 190x338 parent
-- (Auctionator does not enable child clipping here).
if Atr_SellControls then
    panel:SetParent(Atr_SellControls)
    panel:SetFrameLevel(Atr_SellControls:GetFrameLevel() + 10)
    panel:SetPoint("TOPLEFT", Atr_SellControls, "BOTTOMLEFT", 0, -8)

    -- Parent visibility automatically keeps the panel in sync with the Sell
    -- tab. Keep the explicit Show/Hide hooks as well for Auctionator's existing
    -- behavior and for the initial state.
    hooksecurefunc(Atr_SellControls, "Show", function()
        panel:Show()
    end)
    hooksecurefunc(Atr_SellControls, "Hide", function()
        panel:Hide()
    end)
    if Atr_SellControls:IsShown() then
        panel:Show()
    end
end

local currentItemId = nil

local function FormatFloorLine(label, floor, hasRealData)
    if not floor or floor <= 0 then
        return label .. ": |cff888888no data|r"
    end
    local confidence = hasRealData and "" or " |cff888888(est.)|r"
    return label .. ": " .. GetCoinTextureString(floor) .. confidence
end

-- Renders whatever we currently know about currentItemId. Safe to call any
-- time (item not yet resolved, price not yet back from the server, etc.) --
-- every branch degrades to an explicit "no data yet" line rather than leaving
-- stale text on screen from a previously-viewed item.
local function RefreshPanel()
    if not currentItemId then
        panelHomeLine:SetText("|cff888888No item selected|r")
        panelNeutralLine:SetText("")
        return
    end

    local cached = cache[currentItemId]
    if not cached then
        panelHomeLine:SetText("|cff888888Looking up price...|r")
        panelNeutralLine:SetText("")
        return
    end

    local houseName = HOUSE_NAMES[cached.homeHouse] or "Home"
    panelHomeLine:SetText(FormatFloorLine(houseName, cached.homeFloor, cached.homeHasRealData))

    if cached.neutralAvailable then
        panelNeutralLine:SetText(FormatFloorLine("Neutral", cached.neutralFloor, cached.neutralHasRealData))
    else
        panelNeutralLine:SetText("Neutral: |cff888888no guaranteed sale|r")
    end
end

-- Called whenever the item sitting in the Sell tab's "item to auction" slot
-- might have changed. Use Auctionator's own Atr_GetSellItemInfo() here rather
-- than GetAuctionSellItemInfo()+GetItemInfo(name). The latter can return the
-- item name while the item-link cache is not ready yet, which leaves us with
-- no item ID even though Auctionator itself already has the exact sell link.
-- Atr_GetSellItemInfo() is the same helper Auctionator uses in
-- Atr_OnNewAuctionUpdate(), and obtains the link through its scanning tooltip.
local function UpdateFromSellSlot()
    if type(Atr_GetSellItemInfo) ~= "function" then
        currentItemId = nil
        RefreshPanel()
        return
    end

    local name, count, link = Atr_GetSellItemInfo()
    if not name or name == "" or not link then
        currentItemId = nil
        RefreshPanel()
        return
    end

    local itemId = ItemIdFromLink(link)
    if not itemId then
        currentItemId = nil
        RefreshPanel()
        return
    end

    currentItemId = itemId
    RequestPrice(itemId)
    RefreshPanel()
end

local updateFrame = CreateFrame("Frame")
updateFrame:RegisterEvent("NEW_AUCTION_UPDATE")
updateFrame:SetScript("OnEvent", UpdateFromSellSlot)

-- The CHAT_MSG_ADDON handler above only updates `cache`; it has no reason to
-- know about this panel's existence. Re-render here, once, after that handler
-- runs, so a reply that arrives while the panel is already showing "Looking
-- up price..." for the current item is reflected immediately rather than only
-- on the next NEW_AUCTION_UPDATE.
eventFrame:HookScript("OnEvent", function(self, event, prefix, message)
    if event == "CHAT_MSG_ADDON" and prefix == PREFIX then
        local itemId = tonumber(strsplit("\t", message))
        if itemId and itemId == currentItemId then
            RefreshPanel()
        end
    end
end)

-- Covers opening the Sell tab onto an item that was already sitting in the
-- slot from before the panel existed/loaded (e.g. re-logging mid-auction-
-- creation) -- NEW_AUCTION_UPDATE only fires on a change, not on demand.
if Atr_SellControls then
    hooksecurefunc(Atr_SellControls, "Show", UpdateFromSellSlot)
end
