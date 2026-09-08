-- Drag an item onto the pricer window to see (and edit) what AuctionSim would
-- list it at. If the item has no scan/override data yet, the server suggests
-- starting numbers from its rarity, vendor price, and drop chance -- every
-- field shown here is editable regardless of where the numbers came from.

local sformat = string.format
local tonumber = tonumber
local OP = AHSim.OP

local WINDOW_WIDTH = 340
local WINDOW_HEIGHT = 420

local BACKDROP = {
    bgFile = "Interface\\Buttons\\WHITE8X8",
    edgeFile = "Interface\\DialogFrame\\UI-DialogBox-Border",
    tile = true, tileSize = 32, edgeSize = 32,
    insets = { left = 6, right = 6, top = 6, bottom = 6 },
}

local DROP_BACKDROP = {
    bgFile = "Interface\\Tooltips\\UI-Tooltip-Background",
    edgeFile = "Interface\\Buttons\\WHITE8X8",
    tile = false, edgeSize = 2,
    insets = { left = 1, right = 1, top = 1, bottom = 1 },
}

local EDITBOX_BACKDROP = {
    bgFile = "Interface\\Tooltips\\UI-Tooltip-Background",
    edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
    tile = true, tileSize = 16, edgeSize = 12,
    insets = { left = 3, right = 3, top = 3, bottom = 3 },
}

local SOURCE_LABEL = {
    override = "|cff40ff40Saved GM price|r",
    scan = "|cff40a0ffFrom real AH scan data|r",
    suggested = "|cffffd100Suggested (edit before saving)|r",
}

local function MakeEditBox(parent, width, height)
    local box = CreateFrame("EditBox", nil, parent)
    box:SetSize(width, height)
    box:SetAutoFocus(false)
    box:SetFontObject(GameFontHighlightSmall)
    box:SetTextInsets(4, 4, 0, 0)
    box:SetJustifyH("CENTER")
    box:SetNumeric(true)
    box:SetBackdrop(EDITBOX_BACKDROP)
    box:SetBackdropColor(0, 0, 0, 0.6)
    box:SetScript("OnEscapePressed", box.ClearFocus)
    box:SetScript("OnEnterPressed", box.ClearFocus)
    return box
end

local function MakeFieldRow(parent, yOffset, labelText)
    local label = parent:CreateFontString(nil, "ARTWORK", "GameFontNormalSmall")
    label:SetPoint("TOPLEFT", parent, "TOPLEFT", 12, yOffset)
    label:SetText(labelText)

    local box = MakeEditBox(parent, 90, 20)
    box:SetPoint("TOPRIGHT", parent, "TOPRIGHT", -12, yOffset + 3)
    return box
end

function AHSim.BuildItemPricer()
    if AHSimItemPricerFrame then
        return
    end

    local f = CreateFrame("Frame", "AHSimItemPricerFrame", UIParent)
    f:SetSize(WINDOW_WIDTH, WINDOW_HEIGHT)
    f:SetPoint("CENTER", 200, 0)
    f:SetFrameStrata("DIALOG")
    f:SetToplevel(true)
    f:SetBackdrop(BACKDROP)
    f:SetBackdropColor(0, 0, 0, 1)
    f:SetBackdropBorderColor(1, 1, 1, 1)
    f:EnableMouse(true)
    f:SetMovable(true)
    f:RegisterForDrag("LeftButton")
    f:SetScript("OnDragStart", f.StartMoving)
    f:SetScript("OnDragStop", f.StopMovingOrSizing)
    f:SetClampedToScreen(true)
    f:Hide()
    tinsert(UISpecialFrames, "AHSimItemPricerFrame")

    local title = f:CreateFontString(nil, "ARTWORK", "GameFontNormal")
    title:SetPoint("TOP", f, "TOP", 0, -16)
    title:SetText("AuctionSim \226\128\148 Item Pricer")

    local close = CreateFrame("Button", nil, f, "UIPanelCloseButton")
    close:SetPoint("TOPRIGHT", f, "TOPRIGHT", -5, -5)

    -- Drop target: an icon-sized frame the GM drags an item onto (or clicks
    -- while an item is on the cursor -- both OnReceiveDrag and OnMouseUp cover
    -- that, matching how bag/bank slots accept items in 3.3.5a).
    local dropTarget = CreateFrame("Button", nil, f)
    dropTarget:SetSize(48, 48)
    dropTarget:SetPoint("TOP", f, "TOP", 0, -44)
    dropTarget:SetBackdrop(DROP_BACKDROP)
    dropTarget:SetBackdropColor(0, 0, 0, 0.4)
    dropTarget:SetBackdropBorderColor(0.6, 0.6, 0.6, 1)

    local icon = dropTarget:CreateTexture(nil, "ARTWORK")
    icon:SetPoint("TOPLEFT", 2, -2)
    icon:SetPoint("BOTTOMRIGHT", -2, 2)
    icon:SetTexture("Interface\\Icons\\INV_Misc_QuestionMark")

    local hint = f:CreateFontString(nil, "ARTWORK", "GameFontDisableSmall")
    hint:SetPoint("TOP", dropTarget, "BOTTOM", 0, -4)
    hint:SetText("Drag an item here")

    local itemNameText = f:CreateFontString(nil, "ARTWORK", "GameFontNormalSmall")
    itemNameText:SetPoint("TOP", hint, "BOTTOM", 0, -6)
    itemNameText:SetWidth(WINDOW_WIDTH - 24)

    local sourceText = f:CreateFontString(nil, "ARTWORK", "GameFontHighlightSmall")
    sourceText:SetPoint("TOP", itemNameText, "BOTTOM", 0, -4)

    local basisText = f:CreateFontString(nil, "ARTWORK", "GameFontDisableSmall")
    basisText:SetPoint("TOP", sourceText, "BOTTOM", 0, -2)
    basisText:SetWidth(WINDOW_WIDTH - 24)

    local fieldsFrame = CreateFrame("Frame", nil, f)
    fieldsFrame:SetPoint("TOPLEFT", basisText, "BOTTOMLEFT", -12, -14)
    fieldsFrame:SetSize(WINDOW_WIDTH - 24, 190)

    local row = 0
    local function nextY()
        local y = -row * 26
        row = row + 1
        return y
    end

    f.marketBox = MakeFieldRow(fieldsFrame, nextY(), "Market price (copper)")
    f.listLowBox = MakeFieldRow(fieldsFrame, nextY(), "List low")
    f.listHighBox = MakeFieldRow(fieldsFrame, nextY(), "List high")
    f.stackTypicalBox = MakeFieldRow(fieldsFrame, nextY(), "Typical stack")
    f.stackLowBox = MakeFieldRow(fieldsFrame, nextY(), "Stack low")
    f.stackHighBox = MakeFieldRow(fieldsFrame, nextY(), "Stack high")

    local neutralCheck = CreateFrame("CheckButton", nil, fieldsFrame, "UICheckButtonTemplate")
    neutralCheck:SetPoint("TOPLEFT", fieldsFrame, "TOPLEFT", 8, nextY() - 2)
    neutralCheck:SetSize(22, 22)
    local neutralLabel = fieldsFrame:CreateFontString(nil, "ARTWORK", "GameFontNormalSmall")
    neutralLabel:SetPoint("LEFT", neutralCheck, "RIGHT", 2, 0)
    neutralLabel:SetText("Also list on Neutral AH")
    f.neutralCheck = neutralCheck

    local saveButton = CreateFrame("Button", nil, f, "UIPanelButtonTemplate")
    saveButton:SetSize(100, 22)
    saveButton:SetPoint("BOTTOM", f, "BOTTOM", 0, 14)
    saveButton:SetText("Save price")
    f.saveButton = saveButton

    local statusText = f:CreateFontString(nil, "ARTWORK", "GameFontHighlightSmall")
    statusText:SetPoint("BOTTOM", saveButton, "TOP", 0, 6)
    f.statusText = statusText

    f.currentItemId = nil

    local function RequestPriceFor(itemId, itemLink)
        f.currentItemId = itemId
        icon:SetTexture(GetItemIcon(itemId) or "Interface\\Icons\\INV_Misc_QuestionMark")
        itemNameText:SetText(itemLink or ("item:" .. itemId))
        sourceText:SetText("")
        basisText:SetText("querying server...")
        statusText:SetText("")
        AHSim:Send(OP.ITEMQUERY, itemId)
    end

    local function TryAcceptCursorItem()
        if not CursorHasItem() then
            return
        end
        local infoType, itemId, itemLink = GetCursorInfo()
        if infoType == "item" and itemId then
            RequestPriceFor(itemId, itemLink)
        end
        ClearCursor()
    end

    dropTarget:SetScript("OnReceiveDrag", TryAcceptCursorItem)
    dropTarget:SetScript("OnMouseUp", TryAcceptCursorItem)
    dropTarget:SetScript("OnEnter", function(self)
        if f.currentItemId then
            GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
            GameTooltip:SetItemByID(f.currentItemId)
            GameTooltip:Show()
        end
    end)
    dropTarget:SetScript("OnLeave", function() GameTooltip:Hide() end)

    saveButton:SetScript("OnClick", function()
        if not f.currentItemId then
            statusText:SetText("|cffff4040Drag an item in first.|r")
            return
        end
        local market = tonumber(f.marketBox:GetText()) or 0
        if market <= 0 then
            statusText:SetText("|cffff4040Market price must be > 0.|r")
            return
        end
        AHSim:Send(
            OP.ITEMPRICESET,
            f.currentItemId,
            market,
            tonumber(f.listLowBox:GetText()) or market,
            tonumber(f.listHighBox:GetText()) or market,
            tonumber(f.stackTypicalBox:GetText()) or 1,
            tonumber(f.stackLowBox:GetText()) or 1,
            tonumber(f.stackHighBox:GetText()) or 1,
            neutralCheck:GetChecked() and 1 or 0)
        statusText:SetText("saving...")
    end)

    AHSim:RegisterHandler(OP.ITEMPRICE, function(itemId, source, market, listLow, listHigh, typicalStack, stackLow, stackHigh, neutralEligible, basis)
        if tonumber(itemId) ~= f.currentItemId then
            return  -- stale reply for a since-replaced drop
        end
        f.marketBox:SetText(market or 0)
        f.listLowBox:SetText(listLow or 0)
        f.listHighBox:SetText(listHigh or 0)
        f.stackTypicalBox:SetText(typicalStack or 1)
        f.stackLowBox:SetText(stackLow or 1)
        f.stackHighBox:SetText(stackHigh or 1)
        neutralCheck:SetChecked(tonumber(neutralEligible) == 1)
        sourceText:SetText(SOURCE_LABEL[source] or source or "")
        basisText:SetText(basis or "")
    end)

    AHSim:RegisterHandler(OP.ITEMPRICESETRESULT, function(status, detail)
        if status == "ok" then
            statusText:SetText("|cff40ff40Saved.|r")
        else
            statusText:SetText("|cffff4040Save failed: " .. (detail or "unknown error") .. "|r")
        end
    end)
end

SLASH_AUCTIONSIMPRICE1 = "/ahsimprice"
SlashCmdList["AUCTIONSIMPRICE"] = function()
    if not AHSim.authorized then
        DEFAULT_CHAT_FRAME:AddMessage("|cffffd100AuctionSim:|r you are not authorized to use the Item Pricer.")
        return
    end
    if not AHSimItemPricerFrame and AHSim.BuildItemPricer then
        AHSim.BuildItemPricer()
    end
    if AHSimItemPricerFrame then
        AHSimItemPricerFrame:Show()
        AHSimItemPricerFrame:Raise()
    end
end

-- Adds an "Item Pricer" button to the main Bot Manager window rather than
-- leaving /ahsimprice as the only entry point. Wraps (doesn't edit)
-- AHSim.BuildWindow from AHSimPanel.lua so this file stays self-contained --
-- the button is attached the first time the main window is actually built.
local originalBuildWindow = AHSim.BuildWindow
AHSim.BuildWindow = function()
    if originalBuildWindow then
        originalBuildWindow()
    end
    if AHSimFrame and not AHSimFrame.itemPricerButton then
        local btn = CreateFrame("Button", nil, AHSimFrame, "UIPanelButtonTemplate")
        btn:SetSize(110, 22)
        btn:SetPoint("TOPLEFT", AHSimFrame, "TOPLEFT", 14, -14)
        btn:SetText("Item Pricer")
        btn:SetScript("OnClick", function() SlashCmdList["AUCTIONSIMPRICE"]() end)
        AHSimFrame.itemPricerButton = btn
    end
end

