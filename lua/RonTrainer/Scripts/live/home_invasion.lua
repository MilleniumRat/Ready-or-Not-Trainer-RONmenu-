-- Made by MilleniumRat (github.com/MilleniumRat)
-- The game shows "thanks for buying" but the mission board still locks the DLC campaigns.
-- While on: the DLC requirement on the campaign entries (RequiredDlc / VersionRestriction 4, 5, 8)
-- is cleared, and IsGameVersionEnabled answers true for those three. Restored when off.
HI_ON = HI_ON or false
local DLCS = { [4] = true, [5] = true, [8] = true }
-- The hook is only installed the first time the feature is switched on: a hook on this native
-- check is suspected of changing what content (e.g. civilians) the game allows even when idle.
local function ensureHook()
    if RON_HI_HOOK then return end
    RON_HI_HOOK = true
    -- Crash 2026-10-10: this hook is also called while a level loads, where UE4SS wrote the answer
    -- to a bad address. It now only answers while the mission-select screen is open (HI_SELECT),
    -- which the feature's tick keeps current; during travel it passes straight through.
    RegisterHook("/Script/ReadyOrNot.GameFeatureLibrary:IsGameVersionEnabled", function(ctx, feature)
        if HI_ON and HI_SELECT and os.clock() - HI_SELECT < 1.5 then
            local ok, v = pcall(function() return feature:get() end)
            if ok and DLCS[v] then return true end
        end
    end)
end
local saved = {}
local function clear(cls, prop)
    local n = 0
    for _, o in ipairs(FindAllOf(cls) or {}) do
        pcall(function()
            local v = o[prop]
            local k = RT.addr(o) .. prop
            if DLCS[v] then saved[k] = { o = o, p = prop, v = v }; o[prop] = 0; n = n + 1
            elseif saved[k] then n = n + 1 end
        end)
    end
    return n
end

RT.feature{ id = "home_invasion", tab = "Mission", label = "DLC missions available",
    tip = "Your DLC campaigns (Home Invasion, Dark Waters, Boiling Point) show in mission select. Reopen mission select after switching on.",
    tick = function()
        ensureHook()
        HI_ON = true
        local open = false
        for _, w in ipairs(FindAllOf("MissionSelectWidget") or {}) do
            local ok, v = pcall(function() return w:IsInViewport() end)
            if ok and v then open = true end
        end
        if open then HI_SELECT = os.clock() end
        local n = clear("MissionSelectCampaign", "RequiredDlc") + clear("CampaignData", "VersionRestriction")
        return "ok: " .. n .. " DLC campaign entries unlocked"
    end,
    off = function()
        HI_ON = false
        for _, e in pairs(saved) do if RT.valid(e.o) then pcall(function() e.o[e.p] = e.v end) end end
        saved = {}
    end }
