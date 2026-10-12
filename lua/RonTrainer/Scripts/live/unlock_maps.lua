-- Made by MilleniumRat (github.com/MilleniumRat)
-- Unlock all maps: lifts the "finish the previous mission first" progression lock in mission select.
--   UMissionSelectCampaign.bIsLocked / LockReason (1 = LevelProgression) and
--   UMissionSelectLevel.bIsLocked, only (no hook).
-- Campaigns locked because they are not owned (LockReason 3) or not available in Commander (2)
-- stay locked. Nothing is written to your save: it lasts until the game restarts.
-- No native hook is installed: a hook on IsLevelUnlocked crashed UE4SS while a level was loading.
local function unlockNow()
    local camps, levels, kept = 0, 0, 0
    for _, c in ipairs(FindAllOf("MissionSelectCampaign") or {}) do
        pcall(function()
            local reason = c.LockReason
            if reason == 2 or reason == 3 then kept = kept + 1; return end
            if c.bIsLocked then c.bIsLocked = false; c.LockReason = 0; camps = camps + 1 end
            c.Levels:ForEach(function(_, e)
                local l = e:get()
                if RT.valid(l) and l.bIsLocked then l.bIsLocked = false; levels = levels + 1 end
            end)
        end)
    end
    return camps, levels, kept
end

RT.feature{ id = "unlock_maps", tab = "Mission", label = "Unlock all maps",
    tip = "Every mission can be picked without finishing the ones before it. DLC you do not own stays locked. Nothing is saved: it lasts until the game restarts. Open (or reopen) mission select after switching on.",
    tick = (function()
        local totC, totL = 0, 0
        return function()
            local c, l, k = unlockNow()
            totC, totL = totC + c, totL + l
            local kept = k > 0 and (", " .. k .. " stay locked (not owned / not in Commander)") or ""
            if totC + totL == 0 then return "ok: on; open mission select to apply" .. kept end
            return string.format("ok: %d campaigns and %d missions unlocked%s", totC, totL, kept)
        end
    end)(),
    off = function() end }
