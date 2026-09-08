-- "Price Search" tab: an Auctionator-style item name search against the pool
-- of items AuctionSim actually knows about server-side (item_template plus
-- any GM overrides / real scan data), rather than requiring the GM to have the
-- item in hand to drag onto the window (that's still available separately via
-- AHSimItemPricer.lua's /ahsimprice drag target).
--
-- Searching sends OP.ITEMSEARCH; the server replies with up to twenty
-- OP.ITEMSEARCHRESULT rows (itemId, name, quality) followed by one
-- OP.ITEMSEARCHDONE (shown, total). Selecting a result reuses the same
-- OP.ITEMQUERY / OP.ITEMPRICE / OP.ITEMPRICESET protocol as the drag-and-drop
-- pricer, so a GM can look up what AuctionSim would list an item at and tune
-- it the same way. The min/max fields it shows (per-unit price and stack size)
-- are literally the low/high band the bot rolls a new listing from -- see
-- ScannedItem::GetListLow/GetListHigh and GetStackLow/GetStackHigh -- shown
-- here as two side-by-side "Minimum listing" / "Maximum listing" columns so
-- that band can be tuned as a whole for one item.

local sformat = string.format
local tonumber = tonumber
local ipairs = ipairs
local OP = AHSim.OP

local MAX_RESULT_ROWS = 20
local ROW_HEIGHT = 20
local LIST_WIDTH = 240
local FIELD_WIDTH = 140

local EDITBOX_BACKDROP = {
    bgFile = "Interface\\Tooltips\\UI-Tooltip-Background",
    edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
    tile = true, tileSize = 16, edgeSize = 12,
    insets = { left = 3, right = 3, top = 3, bottom = 3 },
}

local LIST_BACKDROP = {
    bgFile = "Interface\\Tooltips\\UI-Tooltip-Background",
    edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
    tile = true, tileSize = 16, edgeSize = 16,
    insets = { left = 4, right = 4, top = 4, bottom = 4 },
}

local SOURCE_LABEL = {
    override = "|cff40ff40Saved GM price|r",
    scan = "|cff40a0ffFrom real AH scan data|r",
    suggested = "|cffffd100Suggested (edit before saving)|r",
}

-- Standard client-side item quality colors (poor..artifact); index = Quality.
local QUALITY_COLORS = {
    [0] = { 0.62, 0.62, 0.62 },
    [1] = { 1, 1, 1 },
    [2] = { 0.12, 1, 0 },
    [3] = { 0, 0.44, 0.87 },
    [4] = { 0.64, 0.21, 0.93 },
    [5] = { 1, 0.5, 0 },
    [6] = { 0.9, 0.8, 0.5 },
}

local function MakeEditBox(parent, width, height)
    local box = CreateFrame("EditBox", nil, parent)
    box:SetSize(width, height)
    box:SetAutoFocus(false)
    box:SetNumeric(true)
    box:SetFontObject(GameFontHighlightSmall)
    box:SetTextInsets(4, 4, 0, 0)
    box:SetJustifyH("CENTER")
    box:SetBackdrop(EDITBOX_BACKDROP)
    box:SetBackdropColor(0, 0, 0, 0.6)
    box:SetScript("OnEscapePressed", box.ClearFocus)
    box:SetScript("OnEnterPressed", box.ClearFocus)
    return box
end

-- Label above an edit box, both anchored TOPLEFT of `parent` at `y`.
local function MakeFieldRow(parent, y, labelText, width)
    local label = parent:CreateFontString(nil, "ARTWORK", "GameFontNormalSmall")
    label:SetPoint("TOPLEFT", parent, "TOPLEFT", 0, y)
    label:SetText(labelText)

    local box = MakeEditBox(parent, width or FIELD_WIDTH, 20)
    box:SetPoint("TOPLEFT", parent, "TOPLEFT", 0, y - 16)
    return box
end

function AHSim.BuildPriceSearchTab(panel)
    -- ===== Search bar =====
    local searchLabel = panel:CreateFontString(nil, "ARTWORK", "GameFontNormalSmall")
    searchLabel:SetPoint("TOPLEFT", panel, "TOPLEFT", 2, 0)
    searchLabel:SetText("Item name:")

    local searchBox = CreateFrame("EditBox", "AHSimPriceSearchBox", panel)
    searchBox:SetSize(LIST_WIDTH - 70, 22)
    searchBox:SetPoint("TOPLEFT", searchLabel, "BOTTOMLEFT", 0, -4)
    searchBox:SetAutoFocus(false)
    searchBox:SetFontObject(GameFontHighlightSmall)
    searchBox:SetTextInsets(6, 6, 0, 0)
    searchBox:SetMaxLetters(50)
    searchBox:SetBackdrop(EDITBOX_BACKDROP)
    searchBox:SetBackdropColor(0, 0, 0, 0.6)
    searchBox:SetScript("OnEscapePressed", searchBox.ClearFocus)

    local searchButton = CreateFrame("Button", nil, panel, "UIPanelButtonTemplate")
    searchButton:SetSize(60, 22)
    searchButton:SetPoint("LEFT", searchBox, "RIGHT", 6, 0)
    searchButton:SetText("Search")

    local searchStatus = panel:CreateFontString(nil, "ARTWORK", "GameFontDisableSmall")
    searchStatus:SetPoint("TOPLEFT", searchBox, "BOTTOMLEFT", 0, -4)
    searchStatus:SetWidth(LIST_WIDTH)
    searchStatus:SetJustifyH("LEFT")

    -- ===== Results list =====
    local listBg = CreateFrame("Frame", nil, panel)
    listBg:SetPoint("TOPLEFT", searchStatus, "BOTTOMLEFT", 0, -8)
    listBg:SetPoint("BOTTOMLEFT", panel, "BOTTOMLEFT", 0, 0)
    listBg:SetWidth(LIST_WIDTH)
    listBg:SetBackdrop(LIST_BACKDROP)
    listBg:SetBackdropColor(0, 0, 0, 0.5)
    listBg:SetBackdropBorderColor(0.5, 0.5, 0.5, 1)

    local listScroll = CreateFrame("ScrollFrame", "AHSimPriceSearchListScroll", listBg, "UIPanelScrollFrameTemplate")
    listScroll:SetPoint("TOPLEFT", listBg, "TOPLEFT", 6, -6)
    listScroll:SetPoint("BOTTOMRIGHT", listBg, "BOTTOMRIGHT", -26, 6)

    local listContent = CreateFrame("Frame", "AHSimPriceSearchListContent", listScroll)
    listContent:SetSize(LIST_WIDTH - 34, MAX_RESULT_ROWS * ROW_HEIGHT)
    listScroll:SetScrollChild(listContent)

    local rows = {}
    for i = 1, MAX_RESULT_ROWS do
        local row = CreateFrame("Button", nil, listContent)
        row:SetSize(LIST_WIDTH - 34, ROW_HEIGHT)
        row:SetPoint("TOPLEFT", listContent, "TOPLEFT", 0, -(i - 1) * ROW_HEIGHT)

        local hl = row:CreateTexture(nil, "HIGHLIGHT")
        hl:SetAllPoints()
        hl:SetTexture(1, 1, 1, 0.15)

        local text = row:CreateFontString(nil, "ARTWORK", "GameFontHighlightSmall")
        text:SetPoint("LEFT", row, "LEFT", 2, 0)
        text:SetPoint("RIGHT", row, "RIGHT", -2, 0)
        text:SetJustifyH("LEFT")
        row.text = text
        row:Hide()
        rows[i] = row
    end

    -- ===== Detail / edit panel (right of the results list) =====
    local detail = CreateFrame("Frame", nil, panel)
    detail:SetPoint("TOPLEFT", listBg, "TOPRIGHT", 16, 0)
    detail:SetPoint("BOTTOMRIGHT", panel, "BOTTOMRIGHT", 0, 0)

    local icon = detail:CreateTexture(nil, "ARTWORK")
    icon:SetSize(32, 32)
    icon:SetPoint("TOPLEFT", detail, "TOPLEFT", 0, 0)
    icon:SetTexture("Interface\\Icons\\INV_Misc_QuestionMark")

    local nameText = detail:CreateFontString(nil, "ARTWORK", "GameFontNormal")
    nameText:SetPoint("TOPLEFT", icon, "TOPRIGHT", 8, -2)
    nameText:SetPoint("RIGHT", detail, "RIGHT", 0, 0)
    nameText:SetJustifyH("LEFT")
    nameText:SetText("Search for an item and pick a result.")

    local sourceText = detail:CreateFontString(nil, "ARTWORK", "GameFontHighlightSmall")
    sourceText:SetPoint("TOPLEFT", icon, "BOTTOMLEFT", 0, -6)
    sourceText:SetPoint("RIGHT", detail, "RIGHT", 0, 0)
    sourceText:SetJustifyH("LEFT")

    local basisText = detail:CreateFontString(nil, "ARTWORK", "GameFontDisableSmall")
    basisText:SetPoint("TOPLEFT", sourceText, "BOTTOMLEFT", 0, -2)
    basisText:SetPoint("RIGHT", detail, "RIGHT", 0, 0)
    basisText:SetJustifyH("LEFT")
    basisText:SetWordWrap(true)

    local fields = CreateFrame("Frame", nil, detail)
    fields:SetPoint("TOPLEFT", basisText, "BOTTOMLEFT", 0, -20)
    fields:SetPoint("RIGHT", detail, "RIGHT", 0, 0)

    local marketBox = MakeFieldRow(fields, 0, "Market price (copper)", FIELD_WIDTH)

    -- Two columns: the min/max per-unit price and stack size AuctionSim will
    -- roll a new listing between -- tune the whole band for this item at once.
    local colGap = 24

    local minHeader = fields:CreateFontString(nil, "ARTWORK", "GameFontNormal")
    minHeader:SetPoint("TOPLEFT", fields, "TOPLEFT", 0, -50)
    minHeader:SetText("|cff8080ffMinimum listing|r")

    local maxHeader = fields:CreateFontString(nil, "ARTWORK", "GameFontNormal")
    maxHeader:SetPoint("TOPLEFT", fields, "TOPLEFT", FIELD_WIDTH + colGap, -50)
    maxHeader:SetText("|cffff8080Maximum listing|r")

    local minCol = CreateFrame("Frame", nil, fields)
    minCol:SetPoint("TOPLEFT", minHeader, "BOTTOMLEFT", 0, -8)
    minCol:SetSize(FIELD_WIDTH, 130)

    local maxCol = CreateFrame("Frame", nil, fields)
    maxCol:SetPoint("TOPLEFT", maxHeader, "BOTTOMLEFT", 0, -8)
    maxCol:SetSize(FIELD_WIDTH, 130)

    local listLowBox = MakeFieldRow(minCol, 0, "Unit price (min)", FIELD_WIDTH)
    local listHighBox = MakeFieldRow(maxCol, 0, "Unit price (max)", FIELD_WIDTH)

    local stackLowBox = MakeFieldRow(minCol, -46, "Stack size (min)", FIELD_WIDTH)
    local stackHighBox = MakeFieldRow(maxCol, -46, "Stack size (max)", FIELD_WIDTH)

    local stackTypicalBox = MakeFieldRow(minCol, -92, "Typical stack", FIELD_WIDTH)

    local neutralCheck = CreateFrame("CheckButton", nil, maxCol, "UICheckButtonTemplate")
    neutralCheck:SetSize(22, 22)
    neutralCheck:SetPoint("TOPLEFT", maxCol, "TOPLEFT", -2, -92 - 16)
    local neutralLabel = fields:CreateFontString(nil, "ARTWORK", "GameFontNormalSmall")
    neutralLabel:SetPoint("LEFT", neutralCheck, "RIGHT", 2, 0)
    neutralLabel:SetText("Also list on Neutral AH")

    local saveButton = CreateFrame("Button", nil, detail, "UIPanelButtonTemplate")
    saveButton:SetSize(120, 22)
    saveButton:SetPoint("TOPLEFT", minCol, "BOTTOMLEFT", 0, -92 - 30)
    saveButton:SetText("Save price")

    local statusText = detail:CreateFontString(nil, "ARTWORK", "GameFontHighlightSmall")
    statusText:SetPoint("LEFT", saveButton, "RIGHT", 10, 0)

    detail.currentItemId = nil

    local function RequestPriceFor(itemId, displayName)
        detail.currentItemId = itemId
        icon:SetTexture(GetItemIcon(itemId) or "Interface\\Icons\\INV_Misc_QuestionMark")
        nameText:SetText(displayName or ("item:" .. itemId))
        sourceText:SetText("")
        basisText:SetText("querying server...")
        statusText:SetText("")
        AHSim:Send(OP.ITEMQUERY, itemId)
    end

    -- ===== Search wiring =====
    local currentMatches = {}

    local function ClearResults()
        for i = 1, MAX_RESULT_ROWS do
            rows[i]:Hide()
        end
        currentMatches = {}
    end

    local function DoSearch()
        local text = strtrim(searchBox:GetText() or "")
        if text == "" then
            searchStatus:SetText("|cffff4040Type part of an item name.|r")
            return
        end
        ClearResults()
        searchStatus:SetText("searching...")
        AHSim:Send(OP.ITEMSEARCH, text)
    end
    searchButton:SetScript("OnClick", DoSearch)
    searchBox:SetScript("OnEnterPressed", function(self)
        DoSearch()
        self:ClearFocus()
    end)

    for i, row in ipairs(rows) do
        row:SetScript("OnClick", function()
            local match = currentMatches[i]
            if match then
                RequestPriceFor(match.id, match.name)
            end
        end)
        row:SetScript("OnEnter", function(self)
            local match = currentMatches[i]
            if match then
                GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
                GameTooltip:SetHyperlink("item:" .. match.id)
                GameTooltip:Show()
            end
        end)
        row:SetScript("OnLeave", function() GameTooltip:Hide() end)
    end

    AHSim:RegisterHandler(OP.ITEMSEARCHRESULT, function(itemId, name, quality)
        local idx = #currentMatches + 1
        if idx > MAX_RESULT_ROWS then
            return
        end
        currentMatches[idx] = { id = tonumber(itemId), name = name }
        local row = rows[idx]
        local c = QUALITY_COLORS[tonumber(quality) or 1] or QUALITY_COLORS[1]
        row.text:SetText(name)
        row.text:SetTextColor(c[1], c[2], c[3])
        row:Show()
    end)

    AHSim:RegisterHandler(OP.ITEMSEARCHDONE, function(shown, total)
        shown, total = tonumber(shown) or 0, tonumber(total) or 0
        if shown == 0 then
            searchStatus:SetText("|cffff4040No matches.|r")
        elseif total > shown then
            searchStatus:SetText(sformat("|cffffd100%d shown of %d matches -- refine your search.|r", shown, total))
        else
            searchStatus:SetText(sformat("%d match(es).", shown))
        end
    end)

    -- ===== Price get/set (shared protocol with AHSimItemPricer.lua's drag target) =====
    AHSim:RegisterHandler(
        OP.ITEMPRICE,
        function(itemId, source, market, listLow, listHigh, typicalStack, stackLow, stackHigh, neutralEligible, basis)
            if tonumber(itemId) ~= detail.currentItemId then
                return -- reply meant for the drag-drop pricer, or a stale/replaced selection
            end
            marketBox:SetText(market or 0)
            listLowBox:SetText(listLow or 0)
            listHighBox:SetText(listHigh or 0)
            stackTypicalBox:SetText(typicalStack or 1)
            stackLowBox:SetText(stackLow or 1)
            stackHighBox:SetText(stackHigh or 1)
            neutralCheck:SetChecked(tonumber(neutralEligible) == 1)
            sourceText:SetText(SOURCE_LABEL[source] or source or "")
            basisText:SetText(basis or "")
        end)

    saveButton:SetScript("OnClick", function()
        if not detail.currentItemId then
            statusText:SetText("|cffff4040Pick a result first.|r")
            return
        end
        local market = tonumber(marketBox:GetText()) or 0
        if market <= 0 then
            statusText:SetText("|cffff4040Market price must be > 0.|r")
            return
        end
        AHSim:Send(
            OP.ITEMPRICESET,
            detail.currentItemId,
            market,
            tonumber(listLowBox:GetText()) or market,
            tonumber(listHighBox:GetText()) or market,
            tonumber(stackTypicalBox:GetText()) or 1,
            tonumber(stackLowBox:GetText()) or 1,
            tonumber(stackHighBox:GetText()) or 1,
            neutralCheck:GetChecked() and 1 or 0)
        statusText:SetText("saving...")
    end)

    AHSim:RegisterHandler(OP.ITEMPRICESETRESULT, function(status, detailMsg)
        -- Both tabs' Save buttons trigger the same reply; only speak up if this
        -- tab actually has a selection (the drag-drop pricer shows its own status).
        if not detail.currentItemId then
            return
        end
        if status == "ok" then
            statusText:SetText("|cff40ff40Saved.|r")
        else
            statusText:SetText("|cffff4040Save failed: " .. (detailMsg or "unknown error") .. "|r")
        end
    end)
end

AHSim.AddTab("Price Search", AHSim.BuildPriceSearchTab)
