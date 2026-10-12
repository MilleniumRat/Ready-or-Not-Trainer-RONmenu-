-- Made by MilleniumRat (github.com/MilleniumRat)
-- RonTrainer (Lua side). Does the actual work inside the game; the in-game menu is RonMenu.dll.
--
--   RonMenu.dll  --menu_state.txt-->  this script   (what is switched on, slider values, one-shot commands)
--   RonMenu.dll  <--status.txt------  this script   (heartbeat + per-feature status)
--   MCP server   <--bridge\*.txt-->   this script   (run Lua live, live mods; see "Live bridge")
--
-- Property names come from the UE4SS class dump (ue4ss/CXXHeaderDump/ReadyOrNot.hpp).
-- Solo use only. Changes to this file load on the next game start (UE4SS hot reload is off: it hung this mod).

local UEHelpers = require("UEHelpers")

local TICK_MS = 50

----------------------------------------------------------------------------
-- Property tables: name -> forced value (float or bool). Restored when switched off.
----------------------------------------------------------------------------

-- ABaseWeapon
local NO_RECOIL = {
    RecoilMultiplierPitch = 0.0, RecoilMultiplierYaw = 0.0, FirstShotRecoil = 0.0,
    ADSRecoilMultiplier = 0.0, VelocityRecoilMultiplier = 0.0,
    RecoilFireStrength = 0.0, RecoilFireStrengthFirst = 0.0, RecoilAngleStrength = 0.0,
    RecoilRandomness = 0.0, RecoilFireADSModifier = 0.0, RecoilAngleADSModifier = 0.0,
    RecoilBuildupADSModifier = 0.0,
}
local NO_SPREAD = {
    ADSSpreadMultiplier = 0.0, FirstShotSpread = 0.0, VelocitySpreadMultiplier = 0.0,
    bIgnoreAmmoTypeSpread = true,
}
-- ABaseMagazineWeapon
local INFINITE_AMMO = { bInfiniteAmmo = true }

----------------------------------------------------------------------------
-- Helpers
----------------------------------------------------------------------------
local function log(fmt, ...) print(string.format("[RonTrainer] " .. fmt .. "\n", ...)) end

local function scriptDir()
    local src = debug.getinfo(1, "S").source:gsub("^@", "")
    return (src:gsub("[/\\]Scripts[/\\]main%.lua$", ""))
end
local DIR = scriptDir()
local STATE_PATH, STATUS_PATH = DIR .. "\\menu_state.txt", DIR .. "\\status.txt"

local function valid(o)
    if o == nil then return false end
    local ok, v = pcall(function() return o:IsValid() end)
    return ok and v
end

local function addr(o) return o:GetAddress() end

local classProps = {}
local function propsOf(obj)
    local cls = obj:GetClass()
    local key = cls:GetFullName()
    if classProps[key] then return classProps[key] end
    local set = {}
    local c = cls
    while c and c:IsValid() do
        pcall(function()
            c:ForEachProperty(function(p)
                set[p:GetFName():ToString()] = p:GetClass():GetFName():ToString()
            end)
        end)
        local ok, super = pcall(function() return c:GetSuperStruct() end)
        if not ok or not super or not super:IsValid() then break end
        c = super
    end
    classProps[key] = set
    return set
end

local function isFloat(t) return t == "FloatProperty" or t == "DoubleProperty" end
local function className(o) return o:GetClass():GetFName():ToString() end

local function getPawn()
    local ok, pc = pcall(UEHelpers.GetPlayerController)
    if not ok or not valid(pc) then return nil, nil end
    local pawn = pc.Pawn
    if not valid(pawn) then return nil, pc end
    return pawn, pc
end

local function getMovement(pawn)
    local ok, mv = pcall(function() return pawn.CharacterMovement end)
    if ok and valid(mv) then return mv end
    return nil
end

-- Items (weapons, gear) owned by the local pawn. Weapons = items with weapon properties.
local itemCache, weaponCache, itemCacheAt = {}, {}, 0
local function getItems(pawn)
    local now = os.clock()
    if now - itemCacheAt < 0.25 then return itemCache, weaponCache end
    itemCacheAt = now
    local items, weapons = {}, {}
    local all = FindAllOf("BaseItem")
    if all then
        local paddr = addr(pawn)
        for _, it in ipairs(all) do
            if valid(it) then
                local ok, owner = pcall(function() return it:GetOwner() end)
                if ok and valid(owner) and addr(owner) == paddr then
                    items[#items + 1] = it
                    if propsOf(it)["RecoilMultiplierPitch"] then weapons[#weapons + 1] = it end
                end
            end
        end
    end
    itemCache, weaponCache = items, weapons
    return items, weapons
end

local function filter(list, propName)
    local out = {}
    for _, o in ipairs(list) do if propsOf(o)[propName] then out[#out + 1] = o end end
    return out
end

----------------------------------------------------------------------------
-- State from the menu, status back to it
----------------------------------------------------------------------------
local function readState()
    local f = io.open(STATE_PATH, "r")
    if not f then return nil end
    local t = {}
    for line in f:lines() do
        local k, v = line:match("^([%w_]+)=(.*)$")
        if k then t[k] = tonumber(v) or v end
    end
    f:close()
    if next(t) == nil then return nil end
    return t
end

local status = {}
local function setStatus(id, text) status[id] = text end

local function flushStatus()
    local f = io.open(STATUS_PATH, "w")
    if not f then return end
    f:write("heartbeat=", tostring(os.time()), "\n")
    for k, v in pairs(status) do f:write(k, "=", v, "\n") end
    f:close()
end

----------------------------------------------------------------------------
-- Forcing properties to fixed values (with restore)
----------------------------------------------------------------------------
local originals = {}  -- [feature][addr:name] = {obj, name, value}

local function forceTable(feature, obj, tbl, matched)
    local props = propsOf(obj)
    originals[feature] = originals[feature] or {}
    local store = originals[feature]
    for name, value in pairs(tbl) do
        local t = props[name]
        local ok_type = (type(value) == "boolean" and t == "BoolProperty") or (type(value) == "number" and isFloat(t))
        if ok_type then
            local key = addr(obj) .. ":" .. name
            local ok, cur = pcall(function() return obj[name] end)
            if ok then
                if not store[key] then store[key] = { obj = obj, name = name, value = cur } end
                if cur ~= value then pcall(function() obj[name] = value end) end
                matched[#matched + 1] = className(obj) .. "." .. name
            end
        end
    end
end

local function restoreFeature(feature)
    local store = originals[feature]
    if not store then return end
    for _, o in pairs(store) do
        if valid(o.obj) then pcall(function() o.obj[o.name] = o.value end) end
    end
    originals[feature] = nil
end

-- groups = { {objs, tbl}, ... }
local function runForce(feature, enabled, groups, emptyMsg)
    if not enabled then
        if originals[feature] then restoreFeature(feature) end
        setStatus(feature, "off")
        return
    end
    local matched = {}
    for _, g in ipairs(groups) do
        for _, obj in ipairs(g[1]) do forceTable(feature, obj, g[2], matched) end
    end
    if #matched > 0 then
        setStatus(feature, "ok: " .. #matched .. " props (" .. matched[1] .. (#matched > 1 and ", ..." or "") .. ")")
    else
        setStatus(feature, emptyMsg or "none: no matching properties (hold a weapon?)")
    end
end

----------------------------------------------------------------------------
-- Scaling a property from its base value without compounding. The game may rewrite
-- the property at any time; whenever it does, the new value becomes the base.
----------------------------------------------------------------------------
local tracked = {}  -- [feature][addr:name] = {obj, name, base, applied}

local function approx(a, b) return math.abs(a - b) <= 0.002 * math.max(1, math.abs(b)) end

-- fn(base) -> wanted value
local function runScale(feature, enabled, objs, names, fn, emptyMsg)
    tracked[feature] = tracked[feature] or {}
    local store = tracked[feature]
    if not enabled then
        for _, tr in pairs(store) do
            if valid(tr.obj) then
                local ok, cur = pcall(function() return tr.obj[tr.name] end)
                if ok and approx(cur, tr.applied) then pcall(function() tr.obj[tr.name] = tr.base end) end
            end
        end
        tracked[feature] = {}
        setStatus(feature, "off")
        return
    end
    local n, resets = 0, 0
    for _, obj in ipairs(objs) do
        local props = propsOf(obj)
        for _, name in ipairs(names) do
            if isFloat(props[name]) then
                local key = addr(obj) .. ":" .. name
                local ok, cur = pcall(function() return obj[name] end)
                if ok and cur then
                    local tr = store[key]
                    if not tr then
                        tr = { obj = obj, name = name, base = cur, applied = cur }
                        store[key] = tr
                    elseif not approx(cur, tr.applied) then
                        tr.base = cur          -- the game changed it: adopt as new base
                        resets = resets + 1
                    end
                    local want = fn(tr.base)
                    if not approx(cur, want) then pcall(function() obj[name] = want end) end
                    tr.applied = want
                    n = n + 1
                end
            end
        end
    end
    if n == 0 then
        setStatus(feature, emptyMsg or "none: no matching properties")
    elseif resets > 0 then
        setStatus(feature, "warn: game keeps resetting " .. resets .. " of " .. n .. " props")
    else
        setStatus(feature, "ok: " .. n .. " props")
    end
end

local function mul(m) return function(b) return b * m end end

----------------------------------------------------------------------------
-- Player features
----------------------------------------------------------------------------
local godActive = false
local function featGod(pawn, on)
    if on then
        local parts = {}
        if propsOf(pawn)["bGodMode"] and pcall(function() pawn.bGodMode = true end) then parts[#parts + 1] = "bGodMode" end
        pcall(function()
            local hc = pawn.CharacterHealth
            if valid(hc) then
                if not hc.bUnlimited then hc:SetUnlimitedResource(true) end
                parts[#parts + 1] = "unlimited health"
            end
        end)
        godActive = true
        setStatus("god", #parts > 0 and ("ok: " .. table.concat(parts, " + ")) or "none: no god-mode properties on pawn")
    else
        if godActive then
            pcall(function() pawn.bGodMode = false end)
            pcall(function() pawn.CharacterHealth:SetUnlimitedResource(false) end)
            godActive = false
        end
        setStatus("god", "off")
    end
end

local function heal(pawn)
    pcall(function() pawn:ResetHealth() end)
    pcall(function()
        local hc = pawn.CharacterHealth
        hc:Server_ResetAllLimbHealth()
        hc:SetCurrentResourceToMax()
    end)
end

-- Fly: the menu DLL sends the held movement keys (the game's own input only moves along the
-- ground plane), and we steer the velocity along the camera direction ourselves.
local KEY_W, KEY_S, KEY_A, KEY_D, KEY_UP, KEY_DOWN = 1, 2, 4, 8, 16, 32
local function bit(v, b) return (v // b) % 2 == 1 end

local flyActive = false
local function featFly(pawn, pc, mv, s)
    local on = s.fly == 1
    if on and mv then
        local speed = s.fly_val or 2500
        local keys = s.keys or 0
        pcall(function()
            if mv.MovementMode ~= 5 then mv:SetMovementMode(5, 0) end
            mv.MaxFlySpeed = speed
            mv.BrakingDecelerationFlying = 0.0
        end)
        if s.fly_noclip == 1 then pcall(function() pawn:SetActorEnableCollision(false) end)
        else pcall(function() pawn:SetActorEnableCollision(true) end) end

        local ok, rot = pcall(function() return pc:GetControlRotation() end)
        if ok and rot then
            local p, y = math.rad(rot.Pitch), math.rad(rot.Yaw)
            local fx, fy, fz = math.cos(p) * math.cos(y), math.cos(p) * math.sin(y), math.sin(p)
            local rx, ry = -math.sin(y), math.cos(y)
            local f = (bit(keys, KEY_W) and 1 or 0) - (bit(keys, KEY_S) and 1 or 0)
            local r = (bit(keys, KEY_D) and 1 or 0) - (bit(keys, KEY_A) and 1 or 0)
            local u = (bit(keys, KEY_UP) and 1 or 0) - (bit(keys, KEY_DOWN) and 1 or 0)
            local vx, vy, vz = fx * f + rx * r, fy * f + ry * r, fz * f + u
            local len = math.sqrt(vx * vx + vy * vy + vz * vz)
            if len > 0 then vx, vy, vz = vx / len * speed, vy / len * speed, vz / len * speed end
            pcall(function() mv.Velocity = { X = vx, Y = vy, Z = vz } end)
            local okv, v = pcall(function() return mv.Velocity end)
            if okv and v and math.abs((v.Z or 0) - vz) > 1 then
                flyActive = true
                setStatus("fly", "warn: velocity write was ignored by the game")
                return
            end
        end
        flyActive = true
        setStatus("fly", "ok: flying" .. (s.fly_noclip == 1 and " (noclip)" or ""))
    else
        if flyActive then
            if mv then pcall(function() mv:SetMovementMode(3, 0); mv.MaxFlySpeed = 600 end) end
            pcall(function() pawn:SetActorEnableCollision(true) end)
            flyActive = false
        end
        setStatus("fly", on and "none: no movement component" or "off")
    end
end

-- Field of view. Methods (fov_mode):
--   0 Match camera FOV   drive APlayerCharacter.BaseFoV so the real camera FOV equals the slider
--   1 Base FOV           BaseFoV = slider (the game scales it, the camera ends up a bit wider)
--   2 Game setting       the options-menu value (BpGameplayHelperLib.SetFoV); may be clamped, is saved
-- Never write the camera itself: the game sets it every frame, so forcing it jitters and it also
-- drives how the weapon is drawn.
local helperLib
local function getHelperLib()
    if valid(helperLib) then return helperLib end
    pcall(function() helperLib = StaticFindObject("/Script/ReadyOrNot.Default__BpGameplayHelperLib") end)
    return valid(helperLib) and helperLib or nil
end

local function cameraFov(pc)
    local ok, v = pcall(function() return pc.PlayerCameraManager:GetFOVAngle() end)
    return ok and v or nil
end

-- aiming and sprinting change the camera FOV on purpose; don't calibrate against those
local function fovBusy(pawn)
    local busy = false
    pcall(function() if pawn.bAiming then busy = true end end)
    pcall(function() if pawn.bHoldingSprint then busy = true end end)
    return busy
end

local fovState = {}
local function fovUndo()
    local st = fovState
    if st.setting and st.originalSetting then
        local lib = getHelperLib()
        if lib then pcall(function() lib:SetFoV(st.originalSetting) end) end
    end
    if st.baseObj and valid(st.baseObj) and st.originalBase then
        pcall(function() st.baseObj.BaseFoV = st.originalBase end)
    end
    fovState = {}
end

local function setBase(pawn, st, value)
    if not st.baseObj then
        st.baseObj = pawn
        pcall(function() st.originalBase = pawn.BaseFoV end)
    end
    pcall(function() if math.abs(pawn.BaseFoV - value) > 0.01 then pawn.BaseFoV = value end end)
end

local function featFov(pawn, pc, s)
    local on, mode, v = s.fov == 1, s.fov_mode or 0, s.fov_val or 100
    if fovState.mode ~= nil and (not on or fovState.mode ~= mode or fovState.pawn ~= addr(pawn)) then fovUndo() end
    if not on then setStatus("fov", "off"); return end
    if not propsOf(pawn)["BaseFoV"] and mode ~= 2 then setStatus("fov", "none: no BaseFoV on the player"); return end
    if fovState.mode == nil then fovState = { mode = mode, pawn = addr(pawn) } end
    local st = fovState
    local cam = cameraFov(pc)

    if mode == 0 then
        st.base = st.base or v
        local busy = fovBusy(pawn)
        if cam and not busy then
            -- correct only once the camera has settled on the last change (the game eases into it)
            if st.prevCam and math.abs(cam - st.prevCam) < 0.3 then
                local err = v - cam
                if math.abs(err) > 0.5 then
                    st.base = math.max(20, math.min(170, st.base + err * 0.6))
                end
            end
            st.prevCam = cam
        else
            st.prevCam = nil
        end
        setBase(pawn, st, st.base)
        if busy then
            setStatus("fov", string.format("ok: target %.0f (aiming / sprinting changes it for now)", v))
        elseif cam and math.abs(cam - v) <= 1.5 then
            setStatus("fov", string.format("ok: camera FOV %.0f (base FOV %.1f)", cam, st.base))
        else
            setStatus("fov", string.format("waiting: adjusting, camera FOV %.0f -> %.0f", cam or -1, v))
        end
    elseif mode == 1 then
        setBase(pawn, st, v)
        setStatus("fov", string.format("ok: base FOV %.0f, camera FOV %.0f", v, cam or -1))
    else
        local lib = getHelperLib()
        if not lib then setStatus("fov", "none: the game's settings helper was not found"); return end
        if st.appliedSetting ~= v then
            if st.originalSetting == nil then pcall(function() st.originalSetting = pawn.BaseFoV end) end
            if pcall(function() lib:SetFoV(v) end) then
                st.setting = true
                st.appliedSetting = v
            end
        end
        setStatus("fov", string.format("ok: options FOV set to %.0f, camera FOV %.0f", v, cam or -1))
    end
end

-- Camera climb: the per-shot kick list on the weapon and the kick still queued on the player.
local savedPatterns = {}  -- [weapon addr] = { weapon, {{Pitch,Yaw,Roll}, ...} }
local function featRecoilPattern(pawn, weapons, on)
    if on then
        for _, w in ipairs(weapons) do
            local key = addr(w)
            if not savedPatterns[key] then
                local saved = {}
                pcall(function()
                    w.RecoilPattern:ForEach(function(_, elem)
                        local r = elem:get()
                        saved[#saved + 1] = { Pitch = r.Pitch, Yaw = r.Yaw, Roll = r.Roll }
                        elem:set({ Pitch = 0.0, Yaw = 0.0, Roll = 0.0 })
                    end)
                end)
                savedPatterns[key] = { w, saved }
            end
        end
        pcall(function() pawn.PendingRecoil = { Pitch = 0.0, Yaw = 0.0, Roll = 0.0 } end)
    else
        for key, entry in pairs(savedPatterns) do
            local w, saved = entry[1], entry[2]
            if valid(w) then
                local n = 0
                pcall(function()
                    w.RecoilPattern:ForEach(function(_, elem)
                        n = n + 1
                        if saved[n] then elem:set(saved[n]) end
                    end)
                end)
            end
            savedPatterns[key] = nil
        end
    end
end

-- The player's own time dilation. Shared by Turbo and the Move speed fallback.
local dilationApplied = false
local function applyDilation(pawn, factor)
    if factor ~= 1.0 then
        pcall(function()
            if math.abs(pawn.CustomTimeDilation - factor) > 0.001 then pawn.CustomTimeDilation = factor end
        end)
        dilationApplied = true
    elseif dilationApplied then
        pcall(function() pawn.CustomTimeDilation = 1.0 end)
        dilationApplied = false
    end
end

----------------------------------------------------------------------------
-- Hooks on the game's own speed functions. They always count calls (diagnostics); depending on
-- the selected methods they also rewrite the speed value the game is about to apply.
----------------------------------------------------------------------------
local hookCfg = { speedMult = nil }   -- refreshed every tick
local speedHooks = {}
local SPEED_FNS = { SetRunSpeed = 1, SetMaxRunSpeed = 1, Server_SetWalkSpeed = 2, Client_SetWalkSpeed = 2,
                    Server_UpdateLastSetRunSpeed = 0 }

for fn, nParams in pairs(SPEED_FNS) do
    local rec = { n = 0, normal = {}, lastOut = {} }
    local ok = pcall(function()
        RegisterHook("/Script/ReadyOrNot.PlayerCharacter:" .. fn, function(_, a, b)
            rec.n = rec.n + 1
            local params = { a, b }
            for i = 1, nParams do
                local p = params[i]
                if p then
                    pcall(function()
                        local v = p:get()
                        if i == 1 then rec.last = v end
                        local out = v
                        if hookCfg.speedMult and hookCfg.speedMult ~= 1 then
                            -- don't scale a value the game is just echoing back from our last change
                            local lo = rec.lastOut[i]
                            if not (lo and math.abs(v - lo) < 0.5) then out = out * hookCfg.speedMult end
                        end
                        if out ~= v then p:set(out) end
                        rec.lastOut[i] = out
                    end)
                end
            end
        end)
    end)
    if ok then speedHooks[fn] = rec end
end

local function hookSummary()
    local parts = {}
    for fn, rec in pairs(speedHooks) do
        if rec.n > 0 then parts[#parts + 1] = string.format("%s x%d (%.0f)", fn, rec.n, rec.last or 0) end
    end
    return #parts > 0 and (" | game calls: " .. table.concat(parts, ", ")) or " | game calls: none seen yet"
end

local function readMws(mv)
    if not mv then return nil end
    local ok, v = pcall(function() return mv.MaxWalkSpeed end)
    return ok and v or nil
end

----------------------------------------------------------------------------
-- Move speed. Methods (speed_mode):
--   0 Auto        scale RunSpeed, check the engine walk speed went up, else fall back to 2
--   1 Run speed   scale APlayerCharacter.RunSpeed (the base the game computes walk speed from)
--   2 Time scale  the player's own time dilation (always works, animations get faster too)
--   3 Hook        multiply the value in the game's SetRunSpeed / SetWalkSpeed calls
--   4 Engine      force CharacterMovement.MaxWalkSpeed every tick (the game may overwrite it)
-- Returns the time-dilation factor this feature wants.
----------------------------------------------------------------------------
local speedState = { phase = "off" }

local function speedCleanup(pawn, mv)
    runScale("speed", false, { pawn }, { "RunSpeed", "MaxAcceleration" }, mul(1))
    runScale("speed_engine", false, mv and { mv } or {}, { "MaxWalkSpeed", "MaxWalkSpeedCrouched" }, mul(1))
    status.speed_engine = nil
    hookCfg.speedMult = nil
    speedState = { phase = "off" }
end

local function featSpeed(pawn, mv, s)
    local on, mode, mult = s.speed == 1, s.speed_mode or 0, s.speed_val or 2
    if not on or (speedState.mode and speedState.mode ~= mode)
       or (speedState.pawn and speedState.pawn ~= addr(pawn)) then
        if speedState.phase ~= "off" then speedCleanup(pawn, mv) end
        if not on then setStatus("speed", "off"); return 1.0 end
    end
    if speedState.phase == "off" then
        speedState = { phase = "start", mode = mode, pawn = addr(pawn), t0 = os.clock(), mws0 = readMws(mv) }
    end
    local mws = readMws(mv)
    local function runSpeed(enabled)
        runScale("speed", enabled, { pawn }, { "RunSpeed", "MaxAcceleration" }, mul(mult), "none: no RunSpeed on pawn")
    end

    if mode == 0 then
        if speedState.phase == "start" or speedState.phase == "probing" then
            speedState.phase = "probing"
            runSpeed(true)
            if os.clock() - speedState.t0 < 0.6 then
                setStatus("speed", "waiting: testing whether RunSpeed works...")
                return 1.0
            end
            local mws0 = speedState.mws0 or 0
            if mult < 1.05 or (mws and mws0 > 0 and mws >= mws0 * (1 + 0.4 * (mult - 1))) then
                speedState.phase = "runspeed"
            else
                runSpeed(false)
                speedState.phase = "dilation"
                speedState.mws1 = mws
            end
        end
        if speedState.phase == "runspeed" then
            runSpeed(true)
            setStatus("speed", string.format("ok: Auto picked RunSpeed, x%.1f (walk speed %.0f -> %.0f)",
                mult, speedState.mws0 or 0, mws or 0))
            return 1.0
        end
        setStatus("speed", string.format("ok: Auto picked time scale, x%.1f (RunSpeed left walk speed at %.0f)",
            mult, speedState.mws1 or 0))
        return mult
    elseif mode == 1 then
        speedState.phase = "on"
        runSpeed(true)
        local st = status.speed or ""
        if st:sub(1, 2) == "ok" then
            setStatus("speed", string.format("ok: RunSpeed x%.1f, engine walk speed now %.0f (was %.0f)",
                mult, mws or 0, speedState.mws0 or 0))
        end
        return 1.0
    elseif mode == 2 then
        speedState.phase = "on"
        setStatus("speed", string.format("ok: time scale x%.1f (animations are faster too)", mult))
        return mult
    elseif mode == 3 then
        speedState.phase = "on"
        if hookCfg.speedMult ~= mult then
            hookCfg.speedMult = mult
            -- re-send the current speed through the hook so the change applies right away
            pcall(function()
                local last = pawn.LastSetRunSpeed
                if last and last > 1 then pawn:SetRunSpeed(last) end
            end)
        end
        local seen = false
        for _, rec in pairs(speedHooks) do if rec.n > 0 then seen = true end end
        setStatus("speed", (seen and "ok: " or "warn: ") .. string.format("hooking x%.1f, walk speed %.0f", mult, mws or 0)
            .. hookSummary())
        return 1.0
    else
        speedState.phase = "on"
        runScale("speed_engine", true, mv and { mv } or {}, { "MaxWalkSpeed", "MaxWalkSpeedCrouched" }, mul(mult),
                 "none: no movement component")
        local st = status.speed_engine or ""
        status.speed_engine = nil
        if st:sub(1, 4) == "warn" then
            setStatus("speed", "warn: the game keeps overwriting MaxWalkSpeed (try another method)")
        else
            setStatus("speed", string.format("ok: MaxWalkSpeed forced to %.0f", mws or 0))
        end
        return 1.0
    end
end


----------------------------------------------------------------------------
-- Animation speed-ups (fast reload, fast restrain). Every tick each feature requests a rate
-- per mesh; meshes nobody asks for any more go back to normal speed.
----------------------------------------------------------------------------
local boostedMeshes, rateRequests = {}, {}

local function requestRate(mesh, rate)
    if not valid(mesh) then return end
    local a = addr(mesh)
    local r = rateRequests[a]
    if not r or rate > r.rate then rateRequests[a] = { mesh = mesh, rate = rate } end
end

local function flushRates()
    for a, m in pairs(boostedMeshes) do
        if not rateRequests[a] then
            if valid(m) then pcall(function() m.GlobalAnimRateScale = 1.0 end) end
            boostedMeshes[a] = nil
        end
    end
    for a, r in pairs(rateRequests) do
        pcall(function()
            if math.abs(r.mesh.GlobalAnimRateScale - r.rate) > 0.001 then r.mesh.GlobalAnimRateScale = r.rate end
        end)
        boostedMeshes[a] = r.mesh
    end
    rateRequests = {}
end

local function requestPlayerMeshes(pawn, rate)
    for _, name in ipairs({ "Mesh1P", "MeshBody1P", "Mesh" }) do
        local ok, m = pcall(function() return pawn[name] end)
        if ok then requestRate(m, rate) end
    end
end

----------------------------------------------------------------------------
-- Fast restrain (zip-cuffing). Methods (fast_arrest_mode):
--   0 Timer + animation   shorter AZipcuffs.ArrestTime and faster paired arrest animations
--   1 Timer only
--   2 Animation only      you, the cuffs and the person being cuffed animate faster
--   3 Instant             complete the arrest the moment it starts
----------------------------------------------------------------------------
local arrestingPrev = false

local function featFastArrest(pawn, items, s)
    local on, mode, mult = s.fast_arrest == 1, s.fast_arrest_mode or 0, s.fast_arrest_val or 6
    local cuffs = filter(items, "ArrestTime")
    local timer = on and (mode == 0 or mode == 1)
    local div = function(b) return b / mult end
    runScale("fast_arrest_timer", timer, cuffs, { "ArrestTime" }, div)
    runScale("fast_arrest_ragdoll", timer, { pawn }, { "MaxRagdollArrestConfirmTime" }, div)
    status.fast_arrest_timer, status.fast_arrest_ragdoll = nil, nil
    if not on then arrestingPrev = false; setStatus("fast_arrest", "off"); return end
    if #cuffs == 0 then setStatus("fast_arrest", "none: no zip-cuffs in your inventory"); return end

    local arresting, active, target = false, nil, nil
    for _, c in ipairs(cuffs) do
        local ok, a = pcall(function() return c.bArresting end)
        if ok and a then
            arresting, active = true, c
            pcall(function() target = c.TargetedCharacter end)
        end
    end
    if not valid(target) then pcall(function() target = pawn.CurrentlyArresting end) end
    if not arresting and valid(target) then
        -- ragdoll / downed arrests go through the character, not the cuffs
        local ok, a = pcall(function() return pawn.CurrentlyArresting end)
        arresting = ok and valid(a)
    end

    if arresting then
        local rate = (mode == 3) and math.max(mult, 20) or mult
        if mode ~= 1 then
            requestPlayerMeshes(pawn, rate)
            for _, c in ipairs(cuffs) do
                local ok, m = pcall(function() return c.ItemMesh end)
                if ok then requestRate(m, rate) end
            end
            if valid(target) then
                local ok, m = pcall(function() return target.Mesh end)
                if ok then requestRate(m, rate) end
            end
        end
        if mode == 3 and not arrestingPrev and active then
            local ok = pcall(function() active:Server_ArrestComplete() end)
            log("instant arrest: Server_ArrestComplete %s", ok and "called" or "failed")
        end
        setStatus("fast_arrest", string.format("ok: restraining at x%.0f", rate))
    else
        local names = { "timer + animation", "timer only", "animation only", "instant" }
        setStatus("fast_arrest", string.format("ok: ready (%s, x%.0f)", names[mode + 1] or "?", mult))
    end
    arrestingPrev = arresting
end

----------------------------------------------------------------------------
-- World features
----------------------------------------------------------------------------
local gameSpeedApplied
local function featGameSpeed(s)
    local on = s.game_speed == 1
    local want = on and (s.game_speed_val or 1.0) or 1.0
    if gameSpeedApplied ~= want then
        local ok = pcall(function()
            UEHelpers.GetGameplayStatics():SetGlobalTimeDilation(UEHelpers.GetWorld(), want)
        end)
        if ok then gameSpeedApplied = want end
    end
    setStatus("game_speed", on and ("ok: x" .. string.format("%.2f", want)) or "off")
end

local function getAI()
    local out = {}
    for _, cls in ipairs({ "SuspectCharacter", "CivilianCharacter" }) do
        for _, ch in ipairs(FindAllOf(cls) or {}) do
            if valid(ch) then out[#out + 1] = ch end
        end
    end
    return out
end

local frozen, freezeCounter = {}, 0
local function featFreezeAI(on)
    freezeCounter = freezeCounter + 1
    if on then
        if freezeCounter % 10 == 1 then   -- ~2x per second picks up newly spawned AI
            local n = 0
            for _, ch in ipairs(getAI()) do
                pcall(function() ch.CustomTimeDilation = 0.0 end)
                frozen[addr(ch)] = ch
                n = n + 1
            end
            setStatus("freeze_ai", n > 0 and ("ok: " .. n .. " characters frozen") or "none: no suspects / civilians found")
        end
    else
        for _, ch in pairs(frozen) do
            if valid(ch) then pcall(function() ch.CustomTimeDilation = 1.0 end) end
        end
        frozen = {}
        setStatus("freeze_ai", "off")
    end
end

----------------------------------------------------------------------------
-- Unlocks (progression only). Every cosmetic asset (UCustomizationDataBase) has two locks:
--   RequiredTags  progression tags you earn by playing (mission grades, achievements...)
--   LockedToDLC   paid content: Supporter Edition, pre-order bonus, DLC packs
-- We clear RequiredTags for the session and never touch anything locked to paid content.
----------------------------------------------------------------------------
local function isPaidLocked(asset)
    local paid = false
    pcall(function()
        asset.LockedToDLC:ForEach(function(_, e)
            local v = e:get()
            if v and v >= 2 then paid = true end   -- 0/1 = no restriction / base game
        end)
    end)
    return paid
end

local cosmetics = { at = 0, cleared = 0 }
local function featCosmetics(s)
    if s.unlock_cosmetics ~= 1 then
        setStatus("unlock_cosmetics", cosmetics.cleared > 0
            and "off: items unlocked earlier stay unlocked until the game restarts" or "off")
        return
    end
    local now = os.clock()
    if now - cosmetics.at < 3.0 then return end   -- new cosmetics load as menus open; re-check now and then
    cosmetics.at = now

    local total, paid = 0, 0
    for _, a in ipairs(FindAllOf("CustomizationDataBase") or {}) do
        if valid(a) then
            total = total + 1
            if isPaidLocked(a) then
                paid = paid + 1
            else
                local n = 0
                pcall(function() n = a.RequiredTags:GetArrayNum() end)
                if n > 0 and pcall(function() a.RequiredTags:Empty() end) then cosmetics.cleared = cosmetics.cleared + 1 end
            end
        end
    end
    -- entries the loadout screen already built keep their own locked flag
    for _, li in ipairs(FindAllOf("LoadoutCustomization") or {}) do
        if valid(li) then
            local ok, asset = pcall(function() return li.Asset end)
            if ok and valid(asset) and not isPaidLocked(asset) then
                pcall(function() if li.bLocked then li.bLocked = false end end)
            end
        end
    end
    if total == 0 then
        setStatus("unlock_cosmetics", "waiting: open the customization screen once so the game loads its cosmetics")
    else
        setStatus("unlock_cosmetics", string.format(
            "ok: %d earnable items unlocked, %d paid / DLC items left as they are (reopen the customization screen)",
            cosmetics.cleared, paid))
    end
end

-- Missions: the mission portal asks IsLevelUnlocked(url, out unlocked, out score, out lockedUrl).
local missions = { on = false, pre = 0, post = 0, hooked = false }
-- Installed only the first time "All missions" is switched on. It used to be registered at every game
-- start, and this native check runs on every level load, which is where UE4SS crashed.
local function ensureMissionHook()
    if missions.hooked then return end
    missions.hooked = true
    pcall(function()
        RegisterHook("/Script/ReadyOrNot.MissionPortal:IsLevelUnlocked",
            function() missions.pre = missions.pre + 1 end,
            function(_, _, outUnlocked, outScore)
                missions.post = missions.post + 1
                -- only while the mission-select screen is open (an answer written while a level loads crashed UE4SS)
                if missions.on and missions.selectAt and os.clock() - missions.selectAt < 1.5 then
                    pcall(function() outUnlocked:set(true) end)
                    pcall(function() outScore:set(0.0) end)
                end
            end)
    end)
end

local function featMissions(s)
    missions.on = s.unlock_missions == 1
    if not missions.on then setStatus("unlock_missions", "off"); return end
    ensureMissionHook()
    for _, w in ipairs(FindAllOf("MissionSelectWidget") or {}) do
        local ok, v = pcall(function() return w:IsInViewport() end)
        if ok and v then missions.selectAt = os.clock() end
    end
    if missions.post > 0 then
        setStatus("unlock_missions", string.format("ok: overridden %d unlock checks", missions.post))
    elseif missions.pre > 0 then
        setStatus("unlock_missions", "warn: the check runs, but this UE4SS build cannot change its answer")
    else
        setStatus("unlock_missions", "waiting: open mission select to apply")
    end
end

----------------------------------------------------------------------------
-- Dump (also reachable from the menu's Debug tab)
----------------------------------------------------------------------------
-- Only read values of plain types. Reading some complex property types (delegates, soft/weak
-- refs, interfaces...) through UE4SS can crash the game natively, which pcall cannot catch.
local SAFE_TYPES = {
    FloatProperty = true, DoubleProperty = true, IntProperty = true, Int64Property = true,
    UInt32Property = true, Int16Property = true, UInt16Property = true, Int8Property = true,
    BoolProperty = true, ByteProperty = true, EnumProperty = true,
}
local function fmtValue(obj, name, typ)
    if not SAFE_TYPES[typ] then return "" end
    local ok, v = pcall(function() return obj[name] end)
    if not ok then return "<err>" end
    return tostring(v)
end

local function dumpObject(f, obj, label)
    f:write(string.format("\n==== %s  [%s]\n", label, obj:GetFullName()))
    local c = obj:GetClass()
    while c and c:IsValid() do
        f:write(string.format("-- %s\n", c:GetFullName()))
        local lines = {}
        pcall(function()
            c:ForEachProperty(function(p)
                local n = p:GetFName():ToString()
                local t = p:GetClass():GetFName():ToString()
                lines[#lines + 1] = string.format("  %-45s %-18s = %s", n, t, fmtValue(obj, n, t))
            end)
        end)
        for _, l in ipairs(lines) do f:write(l, "\n") end
        f:flush()
        local ok, super = pcall(function() return c:GetSuperStruct() end)
        if not ok or not super or not super:IsValid() then break end
        c = super
    end
end

local function doDump()
    local pawn = getPawn()
    if not pawn then log("dump: no local pawn (are you in a mission?)"); return end
    local path = DIR .. "\\dump.txt"
    local f = io.open(path, "w")
    if not f then log("dump: cannot write %s", path); return end
    dumpObject(f, pawn, "PAWN")
    local mv = getMovement(pawn)
    if mv then dumpObject(f, mv, "CHARACTER MOVEMENT") end
    itemCacheAt = 0
    local items = getItems(pawn)
    for i, it in ipairs(items) do dumpObject(f, it, "ITEM " .. i) end
    f:close()
    log("dump written to %s", path)
end

----------------------------------------------------------------------------
-- Commands (one-shot, from the menu buttons)
----------------------------------------------------------------------------
local savedPos

local function teleportTo(pawn, loc)
    local ok, res = pcall(function()
        return pawn:K2_TeleportTo(loc, pawn:K2_GetActorRotation())
    end)
    if ok and res then return true end
    -- no free spot exactly there: try a little higher
    ok, res = pcall(function()
        return pawn:K2_TeleportTo({ X = loc.X, Y = loc.Y, Z = loc.Z + 120 }, pawn:K2_GetActorRotation())
    end)
    return ok and res
end

local function teleportCrosshair(pawn, pc)
    local ksl, kml = UEHelpers.GetKismetSystemLibrary(), UEHelpers.GetKismetMathLibrary()
    local cam = pc.PlayerCameraManager
    local start = cam:GetCameraLocation()
    local fwd = kml:GetForwardVector(cam:GetCameraRotation())
    local finish = { X = start.X + fwd.X * 50000, Y = start.Y + fwd.Y * 50000, Z = start.Z + fwd.Z * 50000 }
    local color = { R = 0, G = 0, B = 0, A = 0 }
    local hit = {}
    local was = ksl:LineTraceSingle(pawn, start, finish, 0, false, {}, 0, hit, true, color, color, 0.0)
    if not was then log("teleport: nothing under the crosshair"); return end
    local p, nrm = hit.Location, hit.ImpactNormal
    -- step back off the surface along its normal so we don't end up inside it
    local dest = { X = p.X + nrm.X * 60, Y = p.Y + nrm.Y * 60, Z = p.Z + nrm.Z * 60 + 100 }
    if teleportTo(pawn, dest) then log("teleported to crosshair") else log("teleport: no free spot there") end
end

-- Shared helpers for the features below ----------------------------------------------------
local function world() return UEHelpers.GetWorld() end
local function cdo(path)
    local ok, o = pcall(StaticFindObject, path)
    return (ok and valid(o)) and o or nil
end

-- Line trace from the camera through the crosshair. Returns the hit table or nil.
local function traceCrosshair(pawn, pc)
    local ksl, kml = UEHelpers.GetKismetSystemLibrary(), UEHelpers.GetKismetMathLibrary()
    local cam = pc.PlayerCameraManager
    local start = cam:GetCameraLocation()
    local fwd = kml:GetForwardVector(cam:GetCameraRotation())
    local finish = { X = start.X + fwd.X * 50000, Y = start.Y + fwd.Y * 50000, Z = start.Z + fwd.Z * 50000 }
    local color = { R = 0, G = 0, B = 0, A = 0 }
    local hit = {}
    if ksl:LineTraceSingle(pawn, start, finish, 0, false, {}, 0, hit, true, color, color, 0.0) then return hit end
    return nil
end

-- Cached FindAllOf for things that rarely appear/disappear.
local listCache = {}
local function cachedAll(cls, maxAge)
    local c = listCache[cls]
    local now = os.clock()
    if c and now - c.at < (maxAge or 1.0) then return c.list end
    local out = {}
    for _, o in ipairs(FindAllOf(cls) or {}) do if valid(o) then out[#out + 1] = o end end
    listCache[cls] = { at = now, list = out }
    return out
end

local function isDown(ch)
    local ok, dead = pcall(function() return ch:IsDeadOrUnconscious() end)
    return ok and dead
end

local function charState(ch)
    local state = "alive"
    pcall(function()
        if ch:IsDeadOrUnconscious() then state = "dead"
        elseif ch.bArrestComplete then state = "arrested"
        elseif ch:IsSurrendered() then state = "surrendered" end
    end)
    return state
end

----------------------------------------------------------------------------
-- Commands from menu buttons (and their key binds)
----------------------------------------------------------------------------
-- The game's own debug functions on AReadyOrNotPlayerController. Some may be empty in the
-- shipping build; the result line in the menu says what was called.
local GAME_CMDS = {
    secure_evidence     = { "SecureAllEvidence" },
    drop_weapons        = { "DropAllSuspectWeapons" },
    arrest_suspects     = { "ArrestAllSuspects" },
    arrest_civilians    = { "ArrestAllCivilians" },
    report_suspects     = { "ReportAllSuspects" },
    report_civilians    = { "ReportAllCivilians" },
    surrender_suspects  = { "SurrenderAllSuspects" },
    surrender_civilians = { "SurrenderAllCivilians" },
    unlock_doors        = { "UnlockAllDoors" },
    tp_next_ai          = { "TeleportToNextRemainingAI" },
    ai_hide             = { "AIHide" },
    ai_unhide           = { "AIStopHide" },
    flush_bodies        = { "FlushDeadBodies" },
    hostages            = { "ForceAllTakeHostages", function() return 5.0 end },
    reset_ai            = { "ResetAI", function() return 100000.0 end },
    spawn_suspects      = { "SpawnRandomAI", function(arg) return math.floor(tonumber(arg) or 3) end },
}

local function countAlive(cls)
    local n = 0
    for _, ch in ipairs(FindAllOf(cls) or {}) do
        if valid(ch) and not isDown(ch) then n = n + 1 end
    end
    return n
end

-- Fallback for evidence: call ISecurable::Secure on every pickup actor in the level.
local function secureEach(pawn)
    local n = 0
    for _, cls in ipairs({ "EvidenceActor", "PickupWeaponActor", "PickupActor" }) do
        for _, a in ipairs(FindAllOf(cls) or {}) do
            if valid(a) and pcall(function() a:Secure(pawn) end) then n = n + 1 end
        end
    end
    return n
end

local function lookedAtDoor(pawn, pc)
    local hit = traceCrosshair(pawn, pc)
    if not hit then return nil end
    local ok, actor = pcall(UEHelpers.GetActorFromHitResult, hit)
    if not ok or not valid(actor) then return nil end
    local doorCls = cdo("/Script/ReadyOrNot.Door")
    if doorCls and actor:IsA(doorCls) then return actor end
    return nil
end

-- forward declaration: the animation player is defined further down
local animCommand

local function runCommand(name, pawn, pc, s)
    local arg = s.cmd_arg
    log("command: %s %s", name, tostring(arg or ""))
    if name == "dump_player" then doDump(); return end
    if name == "dump_classes" then GenerateSDK(); return end
    if name:sub(1, 5) == "anim_" then animCommand(name, arg, pawn, s); return end
    if name:sub(1, 3) == "lv_" then RT.runAction(name:sub(4), pawn, pc, s); return end   -- live mod buttons

    local g = GAME_CMDS[name]
    if g then
        if not valid(pc) then setStatus("cmd_result", name .. ": no player controller"); return end
        local fn, argf = g[1], g[2]
        local ok, err = pcall(function()
            if argf then pc[fn](pc, argf(arg)) else pc[fn](pc) end
        end)
        local extra = ""
        if name == "surrender_suspects" then
            local n = 0
            for _, ch in ipairs(FindAllOf("SuspectCharacter") or {}) do
                if valid(ch) and pcall(function() ch:Surrender() end) then n = n + 1 end
            end
            extra = ", Surrender() on " .. n .. " suspects"
        elseif name == "secure_evidence" and pawn then
            extra = ", Secure() on " .. secureEach(pawn) .. " items"
        end
        setStatus("cmd_result", (ok and ("called " .. fn) or (fn .. " failed: " .. tostring(err))) .. extra)
        return
    end

    if name == "kill_suspects" then
        local n = 0
        for _, ch in ipairs(FindAllOf("SuspectCharacter") or {}) do
            if valid(ch) and pcall(function() ch:Server_Kill() end) then n = n + 1 end
        end
        setStatus("cmd_result", "Server_Kill on " .. n .. " suspects, " .. countAlive("SuspectCharacter") .. " still up")
        return
    end
    if name == "keycards" or name == "elocks_off" then
        local doors = FindAllOf("Door") or {}
        local d = doors[1]
        if not valid(d) then setStatus("cmd_result", "no doors in this level"); return end
        local ok = pcall(function()
            if name == "keycards" then d:SetSWATHasAllKeycards(world()) else d:SetAllElectronicLocks(world(), false) end
        end)
        setStatus("cmd_result", (name == "keycards" and "all keycards" or "electronic locks off") .. (ok and "" or " failed"))
        return
    end
    if name == "end_mission" then
        -- The controller's ForceCompleteMissionGrade is a developer function that does nothing in the
        -- release build. The real end of a mission is ACoopGM.MissionEnd(bSuccess), which only exists
        -- in the host's game; a client falls back to the developer functions.
        local gm
        pcall(function() gm = UEHelpers.GetGameModeBase() end)
        if not valid(gm) then pcall(function() gm = FindFirstOf("CoopGM") end) end
        local done = {}
        if valid(gm) then
            if pcall(function() gm:MissionEnd(true) end) then done[#done + 1] = "MissionEnd(true) on the game mode" end
        end
        if #done == 0 and valid(pc) then
            if pcall(function() pc:ForceCompleteMissionGrade(tostring(arg or "S")) end) then done[#done + 1] = "ForceCompleteMissionGrade" end
            if pcall(function() pc:ForceCompleteMission(100) end) then done[#done + 1] = "ForceCompleteMission" end
        end
        local host = valid(gm)
        setStatus("cmd_result", #done == 0 and "end mission: nothing could be called (are you in a mission?)"
            or ("called " .. table.concat(done, " + ") .. (host and "" or " (not the host: only the developer functions, which may do nothing)")))
        return
    end
    if name == "complete_objectives" then
        local o, cls = cdo("/Script/ReadyOrNot.Default__Objective"), cdo("/Script/ReadyOrNot.Objective")
        local ok = o and cls and pcall(function() o:CompleteObjectivesOfClass(world(), cls) end)
        setStatus("cmd_result", ok and "called CompleteObjectivesOfClass(Objective)" or "could not complete objectives")
        return
    end

    if not pawn then setStatus("cmd_result", name .. " needs you to be in a mission"); return end
    if name == "tp_crosshair" then
        local ok, err = pcall(teleportCrosshair, pawn, pc)
        if not ok then log("teleport error: %s", tostring(err)) end
    elseif name == "save_pos" then
        pcall(function()
            local l = pawn:K2_GetActorLocation()
            savedPos = { X = l.X, Y = l.Y, Z = l.Z }
        end)
        setStatus("cmd_result", "position saved")
    elseif name == "load_pos" then
        if savedPos then teleportTo(pawn, savedPos) else setStatus("cmd_result", "no saved position yet") end
    elseif name == "heal" then
        heal(pawn)
        setStatus("cmd_result", "healed")
    elseif name == "refill_ammo" then
        itemCacheAt = 0
        local _, weapons = getItems(pawn)
        for _, w in ipairs(weapons) do pcall(function() w:ReplenishAmmo() end) end
        setStatus("cmd_result", "ammo refilled on " .. #weapons .. " weapons")
    elseif name == "drop_gun" or name == "drop_gun_evidence" or name == "drop_all_guns" or name == "drop_everything" then
        -- the game's own inventory functions (the same ones a surrender animation triggers)
        local inv
        pcall(function() inv = pawn.InventoryComp end)
        if not valid(inv) then setStatus("cmd_result", "drop: no inventory component found"); return end
        local ok, err = pcall(function()
            if name == "drop_gun" then
                inv:ThrowEquippedItem()
            elseif name == "drop_gun_evidence" then
                local item
                pcall(function() item = pawn:GetEquippedItem() end)
                if not valid(item) then pcall(function() item = pawn.EquippedItem end) end
                if valid(item) then inv:ThrowSpecificItem(item, true) else inv:ThrowEquippedItem() end
            elseif name == "drop_all_guns" then
                inv:ThrowAllWeapons()
            else
                inv:ThrowAllItems()
            end
        end)
        itemCacheAt = 0
        local what = ({ drop_gun = "dropped the item in your hands", drop_gun_evidence = "dropped your gun as evidence",
                        drop_all_guns = "dropped all your weapons", drop_everything = "dropped all your items" })[name]
        setStatus("cmd_result", ok and what or ("drop failed: " .. tostring(err)))
    elseif name == "door_unlock_look" or name == "door_lock_look" then
        local door = lookedAtDoor(pawn, pc)
        if not door then setStatus("cmd_result", "no door under the crosshair"); return end
        local ok = pcall(function()
            if name == "door_unlock_look" then door:UnlockDoor(true) else door:SetLocked(true) end
        end)
        setStatus("cmd_result", (name == "door_unlock_look" and "door unlocked" or "door locked") .. (ok and "" or " (failed)"))
    end
end

----------------------------------------------------------------------------
-- Doors and tools
----------------------------------------------------------------------------
local revealAt = 0
local function featDoors(s, items)
    local doors = cachedAll("Door", 3.0)
    if s.reveal_doors == 1 then
        local now = os.clock()
        if now - revealAt > 1.0 then
            revealAt = now
            local n = 0
            for _, d in ipairs(doors) do
                local ok = pcall(function()
                    d:SetDoorTrapKnowledge(false, true)
                    d:SetDoorLockKnowledge(false, true)
                end)
                if ok then n = n + 1 end
            end
            setStatus("reveal_doors", n > 0 and ("ok: traps and locks known on " .. n .. " doors") or "none: no doors in this level")
        end
    else
        setStatus("reveal_doors", "off")
    end
    runForce("door_kick", s.door_kick == 1, { { doors, { DoorKickSuccessChance = 1.0 } } }, "none: no doors in this level")
    local tools = filter(items or {}, "MaxOperatingTime")
    local m = s.fast_tools_val or 6
    runScale("fast_tools", s.fast_tools == 1, tools, { "MaxOperatingTime" }, function(b) return b / m end,
             "none: no lockpick / multitool in your loadout")
end

----------------------------------------------------------------------------
-- AI behaviour and your squad
----------------------------------------------------------------------------
local moraleAt = 0
local squadAuto = nil
local function featAI(s, pc)
    local suspects = cachedAll("SuspectCharacter", 1.0)

    -- accuracy: the archetype data assets carry an override switch for it
    local archetypes = cachedAll("AIArchetypeData", 5.0)
    runForce("ai_accuracy", s.ai_accuracy == 1, { { archetypes,
        { bAccuracyOverride = true, Accuracy = s.ai_accuracy_val or 0.2 } } }, "none: no AI archetypes loaded")

    -- morale drain: low morale makes suspects give up instead of fighting
    if s.morale_break == 1 then
        local now = os.clock()
        if now - moraleAt >= 1.0 then
            moraleAt = now
            local m = cdo("/Script/ReadyOrNot.Default__MoraleComponent")
            local n = 0
            if m then
                for _, ch in ipairs(suspects) do
                    if not isDown(ch) and pcall(function()
                        m:LowerMoraleOnCharacter(ch, s.morale_break_val or 0.3, FName("RonTrainer"))
                    end) then n = n + 1 end
                end
            end
            setStatus("morale_break", m and ("ok: draining morale on " .. n .. " suspects") or "none: morale system not found")
        end
    else
        setStatus("morale_break", "off")
    end

    if s.no_fake_surrender == 1 then
        local n = 0
        for _, ch in ipairs(suspects) do
            pcall(function() if ch.bIsFakeSurrender then ch.bIsFakeSurrender = false; n = n + 1 end end)
        end
        setStatus("no_fake_surrender", "ok: watching " .. #suspects .. " suspects")
    else
        setStatus("no_fake_surrender", "off")
    end

    runScale("slow_ai", s.slow_ai == 1, suspects,
             { "FocusTurnSpeed", "FocalPointInterpSpeed", "ActorRotationInterpStandingSpeed",
               "ActorRotationInterpMovingSpeed", "AimOffsetInterpSpeed" },
             mul(s.slow_ai_val or 0.4), "none: no suspects")

    -- squad
    local swat = cachedAll("SWATCharacter", 2.0)
    runForce("squad_god", s.squad_god == 1, { { swat, { bGodMode = true } } }, "none: no squad members")
    if s.squad_god == 1 then
        for _, ch in ipairs(swat) do pcall(function() ch.CharacterHealth:SetUnlimitedResource(true) end) end
    end
    local wantAuto = s.squad_auto == 1
    if valid(pc) and squadAuto ~= wantAuto then
        if wantAuto or squadAuto ~= nil then
            pcall(function() if wantAuto then pc:StartSwatAutomation() else pc:StopSwatAutomation() end end)
        end
        squadAuto = wantAuto
    end
    setStatus("squad_auto", wantAuto and "ok: called StartSwatAutomation" or "off")
end


----------------------------------------------------------------------------
-- Mission: bombs, timer, HUD stats (counts / score / bomb countdown)
----------------------------------------------------------------------------
local heldBombs, heldTimer = {}, nil
local function featMission(s)
    local bombs = cachedAll("BombActor", 2.0)
    if s.bombs == 1 then
        for _, b in ipairs(bombs) do
            local k = addr(b)
            pcall(function()
                if not heldBombs[k] then heldBombs[k] = b.TimeUntilExplodes end
                b.TimeUntilExplodes = heldBombs[k]
            end)
        end
    else
        heldBombs = {}
    end
    runForce("bombs", s.bombs == 1, { { bombs, { MultitoolUseTime = 0.1 } } }, "none: no bombs in this level")
    if s.bombs == 1 and #bombs > 0 then setStatus("bombs", "ok: " .. #bombs .. " bombs held, defuse is instant") end

    local gs
    pcall(function() gs = UEHelpers.GetGameStateBase() end)
    if s.freeze_timer == 1 and valid(gs) then
        pcall(function()
            if heldTimer == nil then heldTimer = gs.RoundTimeRemaining end
            gs.RoundTimeRemaining = heldTimer
        end)
        setStatus("freeze_timer", heldTimer and string.format("ok: timer held at %d:%02d", math.floor(heldTimer / 60), math.floor(heldTimer % 60))
                                      or "none: no mission timer")
    else
        heldTimer = nil
        setStatus("freeze_timer", "off")
    end
end

local statsAt = 0
local function hudStats(s)
    local now = os.clock()
    if now - statsAt < 0.5 then return end
    statsAt = now
    if s.hud_counts == 1 then
        local c = { suspect = { alive = 0, surrendered = 0, arrested = 0, dead = 0 },
                    civilian = { alive = 0, surrendered = 0, arrested = 0, dead = 0 } }
        for cls, kind in pairs({ SuspectCharacter = "suspect", CivilianCharacter = "civilian" }) do
            for _, ch in ipairs(cachedAll(cls, 0.5)) do
                local st = charState(ch)
                c[kind][st] = c[kind][st] + 1
            end
        end
        status.hud_counts = string.format("%d,%d,%d,%d,%d,%d,%d,%d",
            c.suspect.alive, c.suspect.surrendered, c.suspect.arrested, c.suspect.dead,
            c.civilian.alive, c.civilian.surrendered, c.civilian.arrested, c.civilian.dead)
    else
        status.hud_counts = nil
    end
    if s.hud_score == 1 then
        local sc = FindFirstOf("ScoringComponent")
        local ok, v = pcall(function() return sc:GetTotalScore(false, false) end)
        status.hud_score = (ok and v) and tostring(math.floor(v)) or nil
    else
        status.hud_score = nil
    end
    if s.hud_bomb == 1 then
        local best
        for _, b in ipairs(cachedAll("BombActor", 2.0)) do
            local ok, t = pcall(function() return b.TimeUntilExplodes end)
            if ok and t and t > 0 and (not best or t < best) then best = t end
        end
        status.hud_bomb = best and string.format("%.0f", best) or nil
    else
        status.hud_bomb = nil
    end
end

----------------------------------------------------------------------------
-- Third person. Methods (third_person_mode, numbers match the menu list):
--   0 Auto            try 5, 1, 6, 2, 3 in that order; each one is checked by measuring how far
--                     the camera is from you, and the first one that really moves it is kept
--   1 Move camera     re-attach the game's first-person camera to the spring arm behind you
--   2 Switch cameras  turn the first-person camera component off and the third-person one on
--   3 Game toggle     the game's developer ToggleThirdPerson (may be stripped)
--   4 Free orbit      the game's developer ToggleFreeThirdPerson (may be stripped)
--   5 Follow camera   spawn a camera actor, attach it behind you and make it the view target
--                     (the same way the game shows spectator cameras)
--   6 Engine free cam the engine's own "camera freecam" mode on the camera manager
-- We also show the body other players see and hide the first-person arms (by flipping the
-- engine's owner-only visibility flags), and restore everything when off.
----------------------------------------------------------------------------
local TP_METHOD_NAMES = { "moved camera", "switched cameras", "game toggle", "free orbit", "follow camera", "engine free cam" }
local TP_AUTO_ORDER = { 5, 1, 6, 2, 3 }
local tp = { active = nil }   -- active = method number currently applied
local tpCam = nil             -- spawned camera actor, kept for reuse

-- The player has no ThirdPersonCameraArm in the release build (it is null), so we add our own
-- spring arm to the capsule: the engine then moves the camera every frame (smooth, follows the mouse).
local ownArm = { pawn = nil, arm = nil }

local function makeArm(pawn)
    if ownArm.pawn == addr(pawn) and valid(ownArm.arm) then return ownArm.arm end
    local arm
    pcall(function()
        local xf = { Rotation = { X = 0.0, Y = 0.0, Z = 0.0, W = 1.0 }, Translation = { X = 0.0, Y = 0.0, Z = 60.0 },
                     Scale3D = { X = 1.0, Y = 1.0, Z = 1.0 } }
        arm = pawn:AddComponentByClass(cdo("/Script/Engine.SpringArmComponent"), false, xf, false)
    end)
    if valid(arm) then
        ownArm = { pawn = addr(pawn), arm = arm }
        log("third person: added our own spring arm")
        return arm
    end
    return nil
end

local function dropOwnArm()
    if valid(ownArm.arm) then pcall(function() ownArm.arm:K2_DestroyComponent(ownArm.arm) end) end
    ownArm = { pawn = nil, arm = nil }
end

local function tpParts(pawn)
    local fp, tpc, arm
    pcall(function() fp = pawn.FirstPersonCameraComponent end)
    pcall(function() tpc = pawn.ThirdPersonCameraComponent end)
    pcall(function() arm = pawn.ThirdPersonCameraArm end)
    if not valid(arm) and ownArm.pawn == addr(pawn) and valid(ownArm.arm) then arm = ownArm.arm end
    return valid(fp) and fp or nil, valid(tpc) and tpc or nil, valid(arm) and arm or nil
end

local function cameraDistance(pawn, pc)
    local ok, d = pcall(function()
        local c = pc.PlayerCameraManager:GetCameraLocation()
        local p = pawn:K2_GetActorLocation()
        return math.sqrt((c.X - p.X) ^ 2 + (c.Y - p.Y) ^ 2 + (c.Z - p.Z) ^ 2)
    end)
    return ok and d or 0
end

-- every primitive component of the pawn and its items
local function primitiveComponents(pawn, items)
    local cls = cdo("/Script/Engine.PrimitiveComponent")
    local out = {}
    if not cls then return out end
    local function collect(actor)
        pcall(function()
            actor:K2_GetComponentsByClass(cls):ForEach(function(_, e)
                local c = e:get()
                if valid(c) then out[#out + 1] = c end
            end)
        end)
    end
    collect(pawn)
    for _, it in ipairs(items or {}) do collect(it) end
    return out
end

local function showBody(pawn, items)
    tp.vis = tp.vis or {}
    for _, c in ipairs(primitiveComponents(pawn, items)) do
        local k = addr(c)
        local ownerNoSee, onlyOwner, visible = false, false, true
        pcall(function() ownerNoSee = c.bOwnerNoSee end)
        pcall(function() onlyOwner = c.bOnlyOwnerSee end)
        pcall(function() visible = c:IsVisible() end)
        if not tp.vis[k] then tp.vis[k] = { c = c, ownerNoSee = ownerNoSee, onlyOwner = onlyOwner, visible = visible } end
        if ownerNoSee then pcall(function() c:SetOwnerNoSee(false) end) end   -- third-person body / gear
        if onlyOwner and visible then pcall(function() c:SetVisibility(false, false) end) end   -- first-person arms
    end
    for _, n in ipairs({ "Mesh1P", "MeshBody1P" }) do
        pcall(function()
            local m = pawn[n]
            if valid(m) and m:IsVisible() then
                local k = addr(m)
                if not tp.vis[k] then tp.vis[k] = { c = m, ownerNoSee = m.bOwnerNoSee, visible = true } end
                m:SetVisibility(false, false)
            end
        end)
    end
    pcall(function() pawn.Mesh:SetHiddenInGame(false, false) end)
end

local function restoreBody()
    for _, v in pairs(tp.vis or {}) do
        if valid(v.c) then
            pcall(function() v.c:SetOwnerNoSee(v.ownerNoSee) end)
            pcall(function() v.c:SetVisibility(v.visible, false) end)
        end
    end
    tp.vis = nil
end

local function armSetup(arm, s)
    if not arm then return end
    pcall(function()
        if not tp.origArm then tp.origArm = arm.TargetArmLength end
        arm:SetActive(true, false)
        arm:SetComponentTickEnabled(true)
        arm.bUsePawnControlRotation = true
        arm.bDoCollisionTest = true
        local d = s.third_person_val or 300
        if math.abs(arm.TargetArmLength - d) > 0.5 then arm.TargetArmLength = d end
    end)
    pcall(function() arm.SocketOffset = { X = 0.0, Y = s.tp_side_val or 50, Z = s.tp_height_val or 30 } end)
end

-- a camera actor of our own (or the controller's spectator camera if the game made one)
local function followCamera(pawn, pc)
    if valid(tpCam) then return tpCam end
    local spec
    pcall(function() spec = pc.SpectateCamera end)
    if valid(spec) then tpCam = spec; tp.camIsSpectate = true; return tpCam end
    local cls = cdo("/Script/Engine.CameraActor")
    if not cls then error("CameraActor class not found") end
    local gs = UEHelpers.GetGameplayStatics()
    local l = pawn:K2_GetActorLocation()
    local xf = { Rotation = { X = 0.0, Y = 0.0, Z = 0.0, W = 1.0 }, Translation = { X = l.X, Y = l.Y, Z = l.Z },
                 Scale3D = { X = 1.0, Y = 1.0, Z = 1.0 } }
    local a = gs:BeginDeferredActorSpawnFromClass(pawn, cls, xf, 1, pawn, 0)   -- 1 = always spawn
    if not valid(a) then error("could not spawn a camera actor") end
    gs:FinishSpawningActor(a, xf, 0)
    tpCam = a
    return tpCam
end

-- position the follow camera; when there is no spring arm we place it ourselves every tick
local function placeFollowCamera(pawn, pc, s)
    if not valid(tpCam) then return end
    pcall(function()
        local fov = pc.PlayerCameraManager:GetFOVAngle()
        if tp.camFov ~= fov and fov and fov > 10 then tpCam.CameraComponent:SetFieldOfView(fov); tp.camFov = fov end
    end)
    if tp.camAttached then return end
    pcall(function()
        local kml = UEHelpers.GetKismetMathLibrary()
        local rot = pc:GetControlRotation()
        local fwd, right = kml:GetForwardVector(rot), kml:GetRightVector(rot)
        local p = pawn:K2_GetActorLocation()
        local d, side, up = s.third_person_val or 300, s.tp_side_val or 50, (s.tp_height_val or 30) + 60
        local loc = { X = p.X - fwd.X * d + right.X * side, Y = p.Y - fwd.Y * d + right.Y * side,
                      Z = p.Z - fwd.Z * d + right.Z * side + up }
        tpCam:K2_SetActorLocationAndRotation(loc, rot, false, {}, true)
    end)
end

-- returns ok, error text
local function applyMethod(m, pawn, pc, s)
    local fp, tpc, arm = tpParts(pawn)
    if m == 1 then
        if not fp then return false, "no first-person camera component" end
        arm = arm or makeArm(pawn)
        if not arm then return false, "no third-person camera arm" end
        return pcall(function()
            tp.fpParent = fp:GetAttachParent()
            tp.fpSocket = fp:GetAttachSocketName()
            local rl, rr = fp.RelativeLocation, fp.RelativeRotation
            tp.fpRelLoc = { X = rl.X, Y = rl.Y, Z = rl.Z }
            tp.fpRelRot = { Pitch = rr.Pitch, Yaw = rr.Yaw, Roll = rr.Roll }
            armSetup(arm, s)
            fp:K2_AttachToComponent(arm, FName("SpringEndpoint"), 2, 2, 1, false)   -- snap loc/rot, keep scale
        end)
    elseif m == 2 then
        if not fp then return false, "no first-person camera component" end
        if not tpc then return false, "no third-person camera component" end
        return pcall(function() armSetup(arm, s); fp:SetActive(false, false); tpc:SetActive(true, false) end)
    elseif m == 5 then
        if not valid(pc) then return false, "no player controller" end
        return pcall(function()
            local cam = followCamera(pawn, pc)
            tp.camAttached = false
            arm = arm or makeArm(pawn)
            if arm then
                armSetup(arm, s)
                tp.camAttached = pcall(function()
                    cam:K2_AttachToComponent(arm, FName("SpringEndpoint"), 2, 2, 1, false)
                end)
            end
            placeFollowCamera(pawn, pc, s)
            pc:SetViewTargetWithBlend(cam, 0.0, 0, 0.0, false)
        end)
    elseif m == 6 then
        if not valid(pc) then return false, "no player controller" end
        return pcall(function()
            local pcm = pc.PlayerCameraManager
            if not tp.origFreeDist then tp.origFreeDist = pcm.FreeCamDistance end
            pcm.FreeCamDistance = s.third_person_val or 300
            pcm.FreeCamOffset = { X = 0.0, Y = s.tp_side_val or 50, Z = s.tp_height_val or 30 }
            pc:Camera(FName("FreeCam"))
        end)
    else
        return pcall(function()
            if m == 4 then pawn:ToggleFreeThirdPerson() else pawn:ToggleThirdPerson() end
        end)
    end
end

local function undoMethod(m, pawn, pc)
    local fp, tpc, arm = tpParts(pawn)
    if m == 1 and fp and tp.fpParent and valid(tp.fpParent) then
        pcall(function()
            fp:K2_AttachToComponent(tp.fpParent, tp.fpSocket or FName("None"), 0, 0, 0, false)
            fp:K2_SetRelativeLocationAndRotation(tp.fpRelLoc, tp.fpRelRot, false, {}, true)
        end)
    elseif m == 2 and fp and tpc then
        pcall(function() tpc:SetActive(false, false); fp:SetActive(true, false) end)
    elseif m == 3 or m == 4 then
        pcall(function() if m == 4 then pawn:ToggleFreeThirdPerson() else pawn:ToggleThirdPerson() end end)
    elseif m == 5 then
        pcall(function() pc:SetViewTargetWithBlend(pawn, 0.0, 0, 0.0, false) end)
        if valid(tpCam) and not tp.camIsSpectate then pcall(function() tpCam:K2_DestroyActor() end) end
        tpCam = nil
    elseif m == 6 then
        pcall(function() pc:Camera(FName("Default")) end)
        pcall(function() if tp.origFreeDist then pc.PlayerCameraManager.FreeCamDistance = tp.origFreeDist end end)
    end
    if arm and tp.origArm and arm ~= ownArm.arm then pcall(function() arm.TargetArmLength = tp.origArm end) end
    tp.origArm = nil
    dropOwnArm()
    restoreBody()
end

-- try methods from the queue until one can be applied; records why the others failed
local function tpTryNext(pawn, pc, s, now)
    while tp.queue and #tp.queue > 0 do
        local m = table.remove(tp.queue, 1)
        local ok, err = applyMethod(m, pawn, pc, s)
        if ok then tp.active, tp.checkAt = m, now + 0.8; return true end
        tp.errors[#tp.errors + 1] = TP_METHOD_NAMES[m] .. ": " .. tostring(err)
        log("third person %s failed: %s", TP_METHOD_NAMES[m], tostring(err))
    end
    return false
end

local function featThirdPerson(pawn, pc, items, s)
    local want = s.third_person == 1
    local mode = s.third_person_mode or 0
    local now = os.clock()
    if tp.pawn ~= addr(pawn) then tp = { pawn = addr(pawn) } end   -- new character: start clean

    -- switched off, or switched to another method: undo what is applied
    if tp.active and (not want or tp.mode ~= mode) then
        undoMethod(tp.active, pawn, pc)
        tp = { pawn = addr(pawn) }
    end
    if tp.dead and tp.mode ~= mode then tp = { pawn = addr(pawn) } end
    if not want then tp = { pawn = addr(pawn) }; setStatus("third_person", "off"); return end
    if tp.dead then return end

    if not tp.active then
        tp.mode = mode
        tp.queue = {}
        if mode == 0 then for _, m in ipairs(TP_AUTO_ORDER) do tp.queue[#tp.queue + 1] = m end else tp.queue = { mode } end
        tp.errors = {}
        if not tpTryNext(pawn, pc, s, now) then
            setStatus("third_person", "none: " .. table.concat(tp.errors, "; "))
            tp.dead = true
            return
        end
    end

    local fp, tpc, arm = tpParts(pawn)
    if tp.active ~= 6 then armSetup(arm, s) end
    if tp.active == 5 then placeFollowCamera(pawn, pc, s) end
    if tp.active == 6 then
        pcall(function()
            local pcm = pc.PlayerCameraManager
            pcm.FreeCamDistance = s.third_person_val or 300
            pcm.FreeCamOffset = { X = 0.0, Y = s.tp_side_val or 50, Z = s.tp_height_val or 30 }
        end)
    end
    if tp.active ~= 3 and tp.active ~= 4 then showBody(pawn, items) end

    -- check it really worked: the camera must have moved away from your head
    if tp.checkAt and now >= tp.checkAt then
        tp.checkAt = nil
        local d = cameraDistance(pawn, pc)
        if d < 110 and tp.active == 5 and tp.camAttached then
            -- the arm did not carry the camera: place it ourselves instead and check again
            pcall(function() tpCam:K2_DetachFromActor(1, 1, 1) end)
            tp.camAttached = false
            placeFollowCamera(pawn, pc, s)
            tp.checkAt = now + 0.5
            return
        end
        if d < 110 then
            local failedMethod = tp.active
            undoMethod(failedMethod, pawn, pc)
            tp.active = nil
            tp.errors[#tp.errors + 1] = string.format("%s: camera stayed at your head (%.0f cm)", TP_METHOD_NAMES[failedMethod], d)
            log("third person %s: camera stayed at %.0f cm", TP_METHOD_NAMES[failedMethod], d)
            if tpTryNext(pawn, pc, s, now) then
                setStatus("third_person", "waiting: " .. TP_METHOD_NAMES[failedMethod] .. " did not move the camera, trying "
                    .. TP_METHOD_NAMES[tp.active] .. "...")
                return
            end
            setStatus("third_person", "none: " .. table.concat(tp.errors, "; "))
            tp.dead = true
            return
        end
    end
    if tp.checkAt then
        setStatus("third_person", "waiting: switching view (" .. TP_METHOD_NAMES[tp.active] .. ")...")
    else
        setStatus("third_person", string.format("ok: third person via %s, camera %.0f cm behind you",
            TP_METHOD_NAMES[tp.active], cameraDistance(pawn, pc)))
    end
end

----------------------------------------------------------------------------
-- Animations / poses. The menu searches the game's full asset list (Manifest_UFSFiles) and
-- sends us a /Game/... path; we load it on demand and play it on your full-body mesh.
-- Methods (anim_method_mode):
--   0 Full-body pose   SkeletalMeshComponent:PlayAnimation (bypasses the animation blueprint,
--                      always visible; walking / aiming animations pause while it plays)
--   1 Blend in slot    montage or slot animation on top of the normal animation blueprint
----------------------------------------------------------------------------
local SLOTS = { "DefaultSlot", "FullBody", "UpperBody", "FullBodySlot" }
local anim = { current = nil, kind = nil, singleNode = false }

local function loadAnim(path)
    local obj = cdo(path)
    if obj then return obj end
    local ok, o = pcall(LoadAsset, path)
    if ok and valid(o) then return o end
    return nil
end

local function stopAnim(pawn)
    if anim.singleNode then
        pcall(function() pawn.Mesh:SetAnimationMode(0, true) end)   -- back to the animation blueprint
    end
    pcall(function()
        local m = pawn:GetCurrentMontage()
        if valid(m) then pawn:StopAnimMontage(m) end
    end)
    anim = { current = nil }
end

local function playAnimObject(pawn, s, obj)
    local cls = obj:GetClass():GetFName():ToString()
    local rate = s.anim_rate_val or 1.0
    local loop = false
    if anim.singleNode then pcall(function() pawn.Mesh:SetAnimationMode(0, true) end) end
    anim = { current = obj, kind = cls }

    if (s.anim_method_mode or 0) == 0 then
        if cls ~= "AnimSequence" and cls ~= "AnimMontage" and cls ~= "AnimComposite" and not cls:find("BlendSpace") then
            return false, "not an animation (" .. cls .. ")"
        end
        local ok = pcall(function()
            local mesh = pawn.Mesh
            mesh:PlayAnimation(obj, loop)
            mesh:SetPlayRate(rate)
        end)
        anim.singleNode = ok
        return ok, ok and "full-body pose" or "PlayAnimation failed"
    end

    if cls == "AnimMontage" then
        local ok, len = pcall(function() return pawn:PlayAnimMontage(obj, rate, FName("None")) end)
        if ok and len and len > 0 then return true, "montage" end
        return false, "the montage would not play on your skeleton (try Full-body pose)"
    end
    if cls ~= "AnimSequence" and cls ~= "AnimComposite" then return false, "this method needs a sequence or montage (" .. cls .. ")" end
    local slot = SLOTS[(s.anim_slot_mode or 0) + 1] or "DefaultSlot"
    local ok, m = pcall(function()
        local ai = pawn.Mesh:GetAnimInstance()
        return ai:PlaySlotAnimationAsDynamicMontage(obj, FName(slot), 0.25, 0.25, rate, loop and 10000 or 1, -1.0, 0.0)
    end)
    if ok and valid(m) then return true, "slot " .. slot end
    return false, "slot animation failed (try Full-body pose)"
end

-- old fallback: search animations the game already loaded (used if the menu has no asset list)
local function findLoadedAnims(word)
    for _, cls in ipairs({ "AnimMontage", "AnimSequence" }) do
        for _, a in ipairs(FindAllOf(cls) or {}) do
            if valid(a) and a:GetFullName():lower():find(word, 1, true) then return a end
        end
    end
    return nil
end

animCommand = function(name, arg, pawn, s)
    if not pawn then setStatus("cmd_result", "animations need you to be in a mission"); return end
    if name == "anim_stop" then
        stopAnim(pawn)
        setStatus("cmd_result", "animation stopped")
        return
    end
    local obj
    if name == "anim_play" then
        local path = tostring(arg or "")
        obj = loadAnim(path)
        if not obj then setStatus("cmd_result", "could not load " .. path); return end
    elseif name == "anim_quick" then
        obj = findLoadedAnims(tostring(arg or ""):lower())
        if not obj then setStatus("cmd_result", "no loaded animation matches '" .. tostring(arg) .. "'"); return end
    else
        return
    end
    local ok, what = playAnimObject(pawn, s, obj)
    local short = obj:GetFName():ToString()
    setStatus("cmd_result", (ok and ("playing " .. short .. " (" .. what .. ")")) or (short .. ": " .. what))
end

----------------------------------------------------------------------------
-- ESP feed. The DLL reads root positions and rotations straight from memory every frame;
-- we tell it where to look, the capsule size, state, health, skeleton (bone offsets relative
-- to the root, refreshed ~10x per second) and static objects (traps, bombs, evidence).
----------------------------------------------------------------------------
local ESP_PATH = DIR .. "\\esp.txt"
local POV_OFFSET = 0x1330   -- APlayerCameraManager::CameraCachePrivate (0x1320) + FCameraCacheEntry::POV (0x10)
local LOC_OFFSET = 0x130    -- USceneComponent::RelativeLocation (world location for an unattached root)
local ROT_OFFSET = 0x148    -- USceneComponent::RelativeRotation
local CTRL_ROT_OFFSET = 0x318   -- AController::ControlRotation (the aimbot writes it every frame)
local espAt, espWritten = 0, false

local shotHooked = false

local function hookShots()
    if shotHooked then return end
    shotHooked = true
    -- Server_OnFire (rotator, host) and Multicast_OnFire (direction vector, everyone) are the ones the
    -- game really calls for a blueprint weapon; the plain OnFire never ran in testing. A shot reaches
    -- more than one of them, so a repeat of the same weapon within 30 ms is ignored.
    for _, fn in ipairs({ "/Script/ReadyOrNot.BaseMagazineWeapon:OnFire", "/Script/ReadyOrNot.BaseMagazineWeapon:Server_OnFire",
                          "/Script/ReadyOrNot.BaseMagazineWeapon:Multicast_OnFire" }) do
        local ok, err = pcall(RegisterHook, fn, function(self, dirP, locP)
            pcall(function()
                local d, l = dirP:get(), locP:get()
                local w = self:get()
                local key, t = addr(w), os.clock()
                RT = RT or {}
                local last = RT.lastShot
                if last and last.k == key and t - last.t < 0.03 then return end
                RT.lastShot = { k = key, t = t }
                local okp, pitch = pcall(function() return d.Pitch end)
                if not (okp and pitch ~= nil) then   -- Multicast_OnFire sends a direction vector
                    local v = { X = d.X, Y = d.Y, Z = d.Z }
                    local len = math.sqrt(v.X ^ 2 + v.Y ^ 2 + v.Z ^ 2)
                    if len < 0.001 then return end
                    d = { Pitch = math.deg(math.asin(v.Z / len)), Yaw = math.deg(math.atan(v.Y, v.X)) }
                end
                if RT and RT.onShot then RT.onShot(self:get(), d, l) end   -- live mods (combat.lua)
            end)
        end)
        log("shot hook %s: %s", fn, ok and "ok" or tostring(err))
    end
end

-- can the camera see this point (nothing in between except the character itself)?
local function canSee(pawn, from, to, ch)
    local visible = true
    pcall(function()
        local hit = {}
        local c = { R = 0, G = 0, B = 0, A = 0 }
        if UEHelpers.GetKismetSystemLibrary():LineTraceSingle(pawn, from, to, 0, true, {}, 0, hit, true, c, c, 0.0) then
            local okh, h = pcall(UEHelpers.GetActorFromHitResult, hit)
            visible = okh and valid(h) and addr(h) == addr(ch)
        end
    end)
    return visible
end

-- skeleton roles, in the order the DLL expects them
local ROLES = { "head", "neck", "chest", "pelvis",
                "ua_l", "la_l", "hand_l", "ua_r", "la_r", "hand_r",
                "th_l", "ca_l", "ft_l", "th_r", "ca_r", "ft_r" }
local ROLE_PATTERNS = {
    head = { "^head$", "head" }, neck = { "^neck_01$", "^neck", "neck" },
    chest = { "spine_05", "spine_04", "spine_03", "spine3", "chest", "spine_02" },
    pelvis = { "^pelvis$", "^hips$", "pelvis", "hips" },
    ua_l = { "upperarm_l", "l_upperarm", "leftarm$" }, la_l = { "lowerarm_l", "forearm_l", "l_forearm", "leftforearm" },
    hand_l = { "^hand_l$", "l_hand$", "lefthand$" },
    ua_r = { "upperarm_r", "r_upperarm", "rightarm$" }, la_r = { "lowerarm_r", "forearm_r", "r_forearm", "rightforearm" },
    hand_r = { "^hand_r$", "r_hand$", "righthand$" },
    th_l = { "thigh_l", "l_thigh", "leftupleg" }, ca_l = { "calf_l", "l_calf", "leftleg$" }, ft_l = { "^foot_l$", "l_foot$", "leftfoot$" },
    th_r = { "thigh_r", "r_thigh", "rightupleg" }, ca_r = { "calf_r", "r_calf", "rightleg$" }, ft_r = { "^foot_r$", "r_foot$", "rightfoot$" },
}
local BAD_BONE = { "twist", "ik_", "_ik", "_end", "ctrl", "attach", "weapon", "prop", "corrective", "roll" }
local boneMaps = {}   -- [mesh addr] = { role = FName }

local function boneMap(mesh)
    local key = addr(mesh)
    if boneMaps[key] then return boneMaps[key] end
    local names = {}
    pcall(function()
        local n = mesh:GetNumBones()
        for i = 0, n - 1 do
            local fname = mesh:GetBoneName(i)
            local s = fname:ToString():lower()
            local bad = false
            for _, b in ipairs(BAD_BONE) do if s:find(b, 1, true) then bad = true; break end end
            if not bad then names[#names + 1] = { s, fname } end
        end
    end)
    local map = {}
    for role, pats in pairs(ROLE_PATTERNS) do
        for _, pat in ipairs(pats) do
            for _, e in ipairs(names) do
                if e[1]:find(pat) then map[role] = e[2]; break end
            end
            if map[role] then break end
        end
    end
    boneMaps[key] = map
    return map
end

local function skeletonLine(ch, root)
    local okm, mesh = pcall(function() return ch.Mesh end)
    if not okm or not valid(mesh) then return nil end
    local map = boneMap(mesh)
    if not map.head or not map.pelvis then return nil end
    local parts = {}
    for _, role in ipairs(ROLES) do
        local fname = map[role]
        local txt = "_"
        if fname then
            local ok, p = pcall(function() return mesh:GetSocketLocation(fname) end)
            if ok and p then txt = string.format("%.1f,%.1f,%.1f", p.X - root.X, p.Y - root.Y, p.Z - root.Z) end
        end
        parts[#parts + 1] = txt
    end
    return "s=" .. table.concat(parts, "|")
end

local espFrame = 0
local function writeEsp(pc, pawn, s)
    local espOn, aimOn = s.esp == 1, s.aimbot == 1
    -- triggerbot, silent aim and the radar need the character list too
    local trigOn, radarOn = s.trigger == 1, s.radar == 1
    local fxView = s.hitmarkers == 1 or s.grenade_arc == 1 or s.sound_markers == 1   -- need the camera only
    if not espOn then setStatus("esp", "off") end
    if not (espOn or aimOn or trigOn or radarOn or fxView) then
        if espWritten then os.remove(ESP_PATH); espWritten = false end
        return
    end
    local style = s.esp_style_mode or 0
    local wantBones = (espOn and (style == 4 or style == 5)) or aimOn
    local now = os.clock()
    local fast = (s.esp_rate_val or 80) / 1000
    if now - espAt < ((wantBones or aimOn or trigOn or espOn) and fast or 0.25) then return end
    espAt = now
    espFrame = (espFrame or 0) + 1
    -- Performance page: skeletons and wall checks only on every Nth refresh
    if wantBones and espFrame % math.max(1, math.floor(s.esp_skel_every_val or 1)) ~= 0 and not aimOn then wantBones = false end
    local cam
    pcall(function() cam = pc.PlayerCameraManager end)
    if not valid(cam) then setStatus("esp", "none: no camera"); return end
    local camLoc
    pcall(function() camLoc = cam:GetCameraLocation() end)
    local maxDist = (s.esp_dist_val or 150) * 100

    local lines = {
        string.format("cam=%X", addr(cam)),
        string.format("pov_off=%X", POV_OFFSET),
        string.format("loc_off=%X", LOC_OFFSET),
        string.format("rot_off=%X", ROT_OFFSET),
        string.format("pc=%X", addr(pc)),
        string.format("ctrl_off=%X", CTRL_ROT_OFFSET),
    }
    if pawn then pcall(function() lines[#lines + 1] = string.format("me=%X", addr(pawn.RootComponent)) end) end
    local aimTargets = s.aim_targets_mode or 0   -- 0 suspects, 1 civilians, 2 both
    local checkVis = pawn and ((aimOn and s.aim_visible == 1) or trigOn
        or (espOn and (s.esp_visonly == 1 or s.esp_vis_color == 1)))
    local visEvery = math.max(1, math.floor(s.esp_vis_every_val or 1))
    if checkVis and not (aimOn or trigOn) and espFrame % visEvery ~= 0 then checkVis = false end
    local n = 0
    local wantWeapon = espOn and s.lbl_weapon == 1
    local myAddr = pawn and addr(pawn) or 0
    local function addChars(cls, kind)
        for _, ch in ipairs(cachedAll(cls, s.esp_discover_val or 0.5)) do
            local ok, root = pcall(function() return ch.RootComponent end)
            if ok and valid(root) and addr(ch) ~= myAddr then
                local hh, rad = 90, 34
                pcall(function() hh = ch.CapsuleComponent.CapsuleHalfHeight; rad = ch.CapsuleComponent.CapsuleRadius end)
                local hp, hpAbs, hpMax = -1, -1, -1
                pcall(function()
                    local h = ch.CharacterHealth
                    hpAbs, hpMax = h.Resource, h.MaxResource
                    if hpMax > 0 then hp = math.max(0, math.min(1, hpAbs / hpMax)) end
                end)
                local vis = -1
                local okl, loc = pcall(function() return ch:K2_GetActorLocation() end)
                if checkVis and okl and loc and camLoc then
                    local head = { X = loc.X, Y = loc.Y, Z = loc.Z + hh * 0.8 }
                    local chest = { X = loc.X, Y = loc.Y, Z = loc.Z + hh * 0.35 }
                    vis = (canSee(pawn, camLoc, head, ch) or canSee(pawn, camLoc, chest, ch)) and 1 or 0
                end
                local weapon, name, pid = "", "", -1
                if wantWeapon then
                    pcall(function()
                        local it = ch:GetEquippedItem()
                        if valid(it) then weapon = className(it):gsub("^BP_", ""):gsub("_C$", ""):gsub("^Primary_", ""):gsub("^Secondary_", ""):gsub(",", " ") end
                    end)
                end
                if kind == "friend" then
                    pcall(function() name = ch.PlayerState:GetPlayerName():ToString():gsub(",", " ") end)
                    pcall(function() pid = ch.PlayerState.PlayerId end)
                end
                lines[#lines + 1] = string.format("c=%X,%s,%s,%.1f,%.1f,%.2f,%d,%.0f,%.0f,%s,%s,%d", addr(root), kind, charState(ch),
                    hh, rad, hp, vis, hpAbs, hpMax, weapon, name, pid)
                n = n + 1
                if wantBones then
                    local near = okl and loc and camLoc and
                        ((loc.X - camLoc.X) ^ 2 + (loc.Y - camLoc.Y) ^ 2 + (loc.Z - camLoc.Z) ^ 2) <= maxDist * maxDist
                    if near then
                        local sl = skeletonLine(ch, loc)
                        if sl then lines[#lines + 1] = sl end
                    end
                end
            end
        end
    end
    local wantSuspects = espOn or (aimOn and aimTargets ~= 1) or trigOn or radarOn
    local wantCivs = (espOn and s.esp_civ == 1) or (aimOn and aimTargets ~= 0) or (radarOn and s.radar_civ == 1)
        or (trigOn and s.trigger_suspects ~= 1)
    if wantSuspects then addChars("SuspectCharacter", "suspect") end
    if wantCivs then addChars("CivilianCharacter", "civilian") end
    if (espOn and s.esp_swat == 1) or (radarOn and s.radar_team == 1) then addChars("SWATCharacter", "swat") end
    if (espOn and s.esp_friends == 1) or (radarOn and s.radar_team == 1)
        or (aimOn and s.aim_friend == 1) then addChars("PlayerCharacter", "friend") end

    local function addObjects(cls, kind, labelFn)
        for _, a in ipairs(cachedAll(cls, 1.0)) do
            local ok, p = pcall(function() return a:K2_GetActorLocation() end)
            if ok and p then
                local label = labelFn and labelFn(a) or ""
                lines[#lines + 1] = string.format("o=%s,%.1f,%.1f,%.1f,%s", kind, p.X, p.Y, p.Z, label)
            end
        end
    end
    if espOn and s.esp_traps == 1 then   -- ATrapActor.TrapStatus: 0 live, 1 triggered, 2 disarmed
        for _, t in ipairs(cachedAll("TrapActor", 1.0)) do
            local ok, p = pcall(function() return t:K2_GetActorLocation() end)
            local st = 0
            pcall(function() st = t.TrapStatus end)
            if ok and p then
                lines[#lines + 1] = string.format("o=%s,%.1f,%.1f,%.1f,%s", st == 0 and "trap" or "trapoff", p.X, p.Y, p.Z,
                    st == 0 and "LIVE" or st == 1 and "triggered" or "disarmed")
            end
        end
    end
    if espOn and s.esp_bombs == 1 then
        addObjects("BombActor", "bomb", function(b)
            local ok, t = pcall(function() return b.TimeUntilExplodes end)
            return (ok and t and t > 0) and string.format("%d:%02d", math.floor(t / 60), math.floor(t % 60)) or ""
        end)
    end
    if espOn and s.esp_evidence == 1 then addObjects("EvidenceActor", "evidence") end
    if espOn and s.esp_weapons == 1 then addObjects("PickupWeaponActor", "weapon") end
    if espOn and s.esp_bodies == 1 then
        -- bodies still waiting to be reported to TOC (bHasBeenReported false)
        for _, cls in ipairs({ "SuspectCharacter", "CivilianCharacter" }) do
            for _, ch in ipairs(cachedAll(cls, 1.0)) do
                pcall(function()
                    if charState(ch) == "dead" and not ch.bHasBeenReported then
                        local p = ch:K2_GetActorLocation()
                        lines[#lines + 1] = string.format("o=body,%.1f,%.1f,%.1f,unreported", p.X, p.Y, p.Z)
                    end
                end)
            end
        end
    end

    -- mission objectives: the game's own objective markers (AObjectivePoint) with the text they show.
    -- No marker in the level: the objective actors themselves, if they have a real position.
    if espOn and s.esp_objectives == 1 then
        local openOnly = s.obj_open == 1
        local markers, open = 0, 0
        for _, p in ipairs(cachedAll("ObjectivePoint", 1.0)) do
            local okl, loc = pcall(function() return p:K2_GetActorLocation() end)
            if okl and loc then
                local enabled, text = false, "Objective"
                pcall(function()
                    local mc = p.ObjectiveMarkerComponent
                    local v = mc.bEnabled
                    enabled = (v == true) or (v == 1)
                    local t = mc.MarkerText:ToString()
                    if t and t ~= "" then text = t end
                end)
                markers = markers + 1
                if enabled then open = open + 1 end
                if enabled or not openOnly then
                    lines[#lines + 1] = string.format("o=%s,%.1f,%.1f,%.1f,%s", enabled and "objective" or "objdone",
                        loc.X, loc.Y, loc.Z, (text:gsub(",", " ")))
                end
            end
        end
        if markers == 0 then
            for _, o in ipairs(cachedAll("Objective", 1.0)) do
                local okl, loc = pcall(function() return o:K2_GetActorLocation() end)
                if okl and loc and (math.abs(loc.X) + math.abs(loc.Y) + math.abs(loc.Z)) > 1 then
                    local st, text = 0, "Objective"   -- ObjectiveStatus: 0 in progress, 1 complete, 2 failed
                    pcall(function()
                        st = o.ObjectiveStatus
                        local t = o.ObjectiveName:ToString()
                        if t and t ~= "" then text = t end
                    end)
                    local done = st ~= 0
                    markers = markers + 1
                    if not done then open = open + 1 end
                    if not done or not openOnly then
                        lines[#lines + 1] = string.format("o=%s,%.1f,%.1f,%.1f,%s", done and "objdone" or "objective",
                            loc.X, loc.Y, loc.Z, (text:gsub(",", " ")))
                    end
                end
            end
        end
        setStatus("esp_objectives", markers > 0 and string.format("ok: %d marker%s, %d open", markers, markers == 1 and "" or "s", open)
                                    or "waiting: no objective in this level")
    end

    local tmp = ESP_PATH .. ".tmp"
    local f = io.open(tmp, "w")
    if not f then return end
    f:write(table.concat(lines, "\n"), "\n")
    f:close()
    os.remove(ESP_PATH)
    os.rename(tmp, ESP_PATH)
    espWritten = true
    if espOn then setStatus("esp", "ok: " .. n .. " characters" .. (wantBones and ", skeletons on" or "")) end
    if aimOn then
        setStatus("aimbot", n > 0 and ("ok: " .. n .. " possible targets tracked") or "waiting: no targets in this level")
    else
        setStatus("aimbot", "off")
    end
end

----------------------------------------------------------------------------
-- Main tick
----------------------------------------------------------------------------
local PAWN_FEATURES = { "god", "speed", "fly", "fov", "infinite_ammo", "no_recoil", "rapid_fire",
                        "fast_arrest", "fast_tools", "third_person" }
-- seq: last command number seen; alive: the menu was running last tick
local menuLink = { seq = nil, alive = false }

local function tick()
    -- The menu writes alive=<unix time> every second. No file, or an old timestamp, means the menu
    -- was unloaded or crashed: then everything is treated as switched off, so every feature
    -- restores the game's own values instead of keeping the last settings forever.
    local s = readState()
    local alive = s and tonumber(s.alive) and math.abs(os.time() - tonumber(s.alive)) <= 10
    if not alive then
        if menuLink.alive then log("menu is not running any more: switching all features off") end
        s = {}
        menuLink.seq = nil
    end
    menuLink.alive = alive and true or false
    if RT then RT.state = s end

    -- world-level features work without a pawn
    featGameSpeed(s)
    featFreezeAI(s.freeze_ai == 1)
    featCosmetics(s)
    featMissions(s)
    featMission(s)
    hudStats(s)

    local pawn, pc = getPawn()
    if valid(pc) then
        pcall(writeEsp, pc, pawn, s)
        local ok, err = pcall(featAI, s, pc)
        if not ok then log("ai error: %s", tostring(err)) end
    end

    if alive then
        if menuLink.seq == nil then menuLink.seq = s.cmd_seq or 0 end   -- ignore commands from before we (re)connected
        if (s.cmd_seq or 0) ~= menuLink.seq then
            menuLink.seq = s.cmd_seq
            runCommand(tostring(s.cmd), pawn, pc, s)
        end
    end

    if not pawn then
        for _, id in ipairs(PAWN_FEATURES) do
            setStatus(id, s[id] == 1 and "waiting: no player (load into a mission)" or "off")
        end
        hookCfg.speedMult = nil
        flushRates()   -- put any sped-up animations back to normal
        return
    end

    local mv = getMovement(pawn)
    local items, weapons = getItems(pawn)
    local magWeapons = filter(weapons, "bInfiniteAmmo")

    featGod(pawn, s.god == 1)
    featFly(pawn, pc, mv, s)
    featFov(pawn, pc, s)

    -- movement: Turbo, Move speed and No slow all feed the player's time dilation
    local dilation = featSpeed(pawn, mv, s)
    applyDilation(pawn, dilation)

    -- animation speed-ups
    featFastArrest(pawn, items, s)
    flushRates()

    featDoors(s, items)
    featThirdPerson(pawn, pc, items, s)

    -- FireRate: rounds per minute if large, seconds between shots if small. Delays get divided.
    local rf = s.rapid_fire_val or 2
    runScale("rapid_fire", s.rapid_fire == 1, weapons, { "FireRate" },
             function(b) if b > 5 then return b * rf else return b / rf end end, "none: no weapon found")
    runScale("rapid_fire_delay", s.rapid_fire == 1, weapons, { "RefireDelay", "BufferFireMinRefireTime" },
             function(b) return b / rf end)
    status.rapid_fire_delay = nil

    runForce("no_recoil", s.no_recoil == 1, { { weapons, NO_RECOIL } })
    featRecoilPattern(pawn, weapons, s.no_recoil == 1)
    runForce("infinite_ammo", s.infinite_ammo == 1, { { magWeapons, INFINITE_AMMO } })
end

----------------------------------------------------------------------------
-- Live bridge: lets the MCP server (ronmenu\mcp\ron_mcp.py) run Lua in the game and add
-- features while the game runs, without a restart.
--   bridge\lua_req.txt          "id=<n>", "op=exec|live_load|live_unload", "name=<mod>", "---", code
--   bridge\lua_resp_<n>.txt     printed output + return values, or the error
--   Scripts\live\<name>.lua     live mods; Scripts\live\_index.txt lists the ones loaded at start
--   live_ui.txt                 features / buttons declared by live mods, shown in the menu's Live tab
-- A live mod uses the global RT table:
--   RT.feature{ id=, label=, tip=, slider={lo, hi, def, fmt},
--               tick=function(ctx, val) return "ok: ..." end, off=function(ctx) end }
--   RT.action{ id=, label=, tip=, danger=, run=function(ctx) return "result text" end }
--   RT.onTick(name, function(ctx) end)
--   ctx = { pawn, pc, items, weapons, s }  (pawn may be nil outside a mission)
----------------------------------------------------------------------------
local bridgeStep   -- defined inside its own scope (Lua allows 200 locals per function)
do
    local BRIDGE = DIR .. "\\bridge"
    local LIVE_DIR = DIR .. "\\Scripts\\live"
    local REQ_PATH = BRIDGE .. "\\lua_req.txt"
    local LIVE_UI_PATH = DIR .. "\\live_ui.txt"

    RT = RT or {}
    RT.log, RT.valid, RT.addr, RT.cdo, RT.getPawn, RT.getItems = log, valid, addr, cdo, getPawn, getItems
    RT.propsOf, RT.className, RT.setStatus, RT.status, RT.world = propsOf, className, setStatus, status, world
    RT.cachedAll, RT.traceCrosshair, RT.teleportTo, RT.UEHelpers = cachedAll, traceCrosshair, teleportTo, UEHelpers
    RT.dir, RT.hookShots, RT.charState, RT.canSee = DIR, hookShots, charState, canSee
    RT.features, RT.actions, RT.ticks, RT.order = {}, {}, {}, {}
    RT.notes = {}
    RT.state = RT.state or {}
    local liveCurrent = nil   -- live mod being loaded (owner of what it registers)

    -- printable form of any value (UObjects by full name, arrays and tables shallow)
    local function show(v, depth)
        depth = depth or 0
        local t = type(v)
        if t == "string" then return string.format("%q", v) end
        if t ~= "table" and t ~= "userdata" then return tostring(v) end
        if t == "userdata" then
            local okv, isv = pcall(function() return v:IsValid() end)
            if okv and isv == false then return "<invalid object>" end
            if okv then
                local ok, name = pcall(function() return v:GetFullName() end)
                if ok and type(name) == "string" then return "<" .. name .. ">" end
            end
            local ok2, s = pcall(function() return v:ToString() end)
            if ok2 and type(s) == "string" then return string.format("%q", s) end
            local ok3, n = pcall(function() return v:GetArrayNum() end)
            if ok3 and type(n) == "number" then
                local parts = {}
                for i = 1, math.min(n, 40) do parts[#parts + 1] = show(v[i], depth + 1) end
                if n > 40 then parts[#parts + 1] = "... (" .. n .. " total)" end
                return "[" .. table.concat(parts, ", ") .. "]"
            end
            local ok4, x = pcall(function() return v:get() end)
            if ok4 and x ~= nil and x ~= v then return show(x, depth + 1) end
            -- structs (FVector, FRotator...) read field by field
            local fields = {}
            for _, f in ipairs({ "X", "Y", "Z", "W", "Pitch", "Yaw", "Roll", "R", "G", "B", "A" }) do
                local okf, fv = pcall(function() return v[f] end)
                if okf and type(fv) == "number" then fields[#fields + 1] = f .. "=" .. string.format("%.3f", fv) end
            end
            if #fields > 0 then return "{" .. table.concat(fields, ", ") .. "}" end
            return tostring(v)
        end
        if depth > 3 then return "{...}" end
        local parts, n = {}, 0
        for k, val in pairs(v) do
            n = n + 1
            if n > 60 then parts[#parts + 1] = "..."; break end
            parts[#parts + 1] = tostring(k) .. "=" .. show(val, depth + 1)
        end
        return "{" .. table.concat(parts, ", ") .. "}"
    end
    RT.show = show

    local SAFE_READ = {
        FloatProperty = true, DoubleProperty = true, IntProperty = true, Int8Property = true, Int16Property = true,
        Int64Property = true, UInt16Property = true, UInt32Property = true, UInt64Property = true,
        BoolProperty = true, ByteProperty = true, EnumProperty = true, NameProperty = true, StrProperty = true,
        ObjectProperty = true, ClassProperty = true,
    }

    -- properties of an object (class chain), values only for plain types; pattern is a Lua pattern, lower-case match
    local function props(obj, pattern)
        local out = {}
        local cls = obj:GetClass()
        local c = cls
        while c and c:IsValid() do
            local cname = c:GetFName():ToString()
            pcall(function()
                c:ForEachProperty(function(p)
                    local name = p:GetFName():ToString()
                    if not pattern or name:lower():find(pattern:lower()) then
                        local ptype = p:GetClass():GetFName():ToString()
                        local val = ""
                        if SAFE_READ[ptype] then
                            local ok, v = pcall(function() return obj[name] end)
                            val = ok and (" = " .. show(v)) or " = <read failed>"
                        end
                        out[#out + 1] = string.format("%s.%s : %s%s", cname, name, ptype, val)
                    end
                end)
            end)
            local ok, super = pcall(function() return c:GetSuperStruct() end)
            if not ok or not super or not super:IsValid() then break end
            c = super
        end
        return table.concat(out, "\n")
    end

    -- UFunctions of an object or class (class chain)
    local function funcs(obj, pattern)
        local out = {}
        local c = obj
        pcall(function() if not obj:IsA(cdo("/Script/CoreUObject.Class")) then c = obj:GetClass() end end)
        while c and c:IsValid() do
            local cname = c:GetFName():ToString()
            pcall(function()
                c:ForEachFunction(function(fn)
                    local name = fn:GetFName():ToString()
                    if not pattern or name:lower():find(pattern:lower()) then out[#out + 1] = cname .. ":" .. name end
                end)
            end)
            local ok, super = pcall(function() return c:GetSuperStruct() end)
            if not ok or not super or not super:IsValid() then break end
            c = super
        end
        return table.concat(out, "\n")
    end

    -- note(): keep a line for later (e.g. from a hook), read with RT.drain()
    local function note(...)
        local p = {}
        for i = 1, select("#", ...) do p[#p + 1] = tostring((select(i, ...))) end
        RT.notes[#RT.notes + 1] = os.date("%H:%M:%S ") .. table.concat(p, " ")
        if #RT.notes > 500 then table.remove(RT.notes, 1) end
    end
    function RT.drain()
        local s = table.concat(RT.notes, "\n")
        RT.notes = {}
        return s
    end

    -- one shared environment for everything the bridge runs, so variables persist between calls
    RT.env = RT.env or setmetatable({}, { __index = _G })
    local env = RT.env
    env.RT, env.show, env.props, env.funcs, env.note = RT, show, props, funcs, note
    env.find = function(cls) return FindAllOf(cls) or {} end
    env.first = function(cls) return FindFirstOf(cls) end
    env.obj = function(path) return StaticFindObject(path) end
    env.load_asset = function(path) return LoadAsset(path) end

    local function context()
        local pawn, pc = getPawn()
        local items, weapons = {}, {}
        if pawn then items, weapons = getItems(pawn) end
        return { pawn = pawn, pc = pc, items = items, weapons = weapons, s = RT.state }
    end

    -- run code; returns the text to send back
    local function runChunk(code, chunkname)
        local out = {}
        env.print = function(...)
            local p = {}
            for i = 1, select("#", ...) do
                local v = select(i, ...)
                p[#p + 1] = type(v) == "string" and v or show(v)
            end
            out[#out + 1] = table.concat(p, "\t")
        end
        local ctx = context()
        env.pawn, env.pc, env.items, env.weapons, env.ctx = ctx.pawn, ctx.pc, ctx.items, ctx.weapons, ctx
        local fn = load("return " .. code, chunkname, "t", env)
        local compileErr
        if not fn then fn, compileErr = load(code, chunkname, "t", env) end
        if not fn then return false, "compile error: " .. tostring(compileErr) end
        local res = table.pack(xpcall(fn, debug.traceback))
        if not res[1] then
            out[#out + 1] = "error: " .. tostring(res[2])
            return false, table.concat(out, "\n")
        end
        for i = 2, res.n do out[#out + 1] = "=> " .. show(res[i]) end
        return true, table.concat(out, "\n")
    end

    local function esc(s) return (tostring(s or ""):gsub("[|\r\n]", " ")) end

    local function writeLiveUi()
        local lines = {}
        for _, key in ipairs(RT.order) do
            local kind, id = key:match("^(%a)_(.+)$")
            if kind == "f" and RT.features[id] then
                local d = RT.features[id]
                local sl = d.slider
                lines[#lines + 1] = table.concat({ "f", id, esc(d.label or id), esc(d.tip), sl and "1" or "0",
                    tostring(sl and sl[1] or 0), tostring(sl and sl[2] or 1), tostring(sl and (sl[3] or sl[1]) or 0),
                    esc(sl and sl[4] or "%.2f"), esc(d.tab), d.valueOnly and "1" or "0", esc(d.parent) }, "|")
            elseif kind == "a" and RT.actions[id] then
                local d = RT.actions[id]
                lines[#lines + 1] = table.concat({ "a", id, esc(d.label or id), esc(d.tip), d.danger and "1" or "0", esc(d.tab) }, "|")
            end
        end
        local f = io.open(LIVE_UI_PATH, "w")
        if f then f:write(table.concat(lines, "\n"), "\n"); f:close() end
    end

    local function addOrder(key)
        for _, k in ipairs(RT.order) do if k == key then return end end
        RT.order[#RT.order + 1] = key
    end

    function RT.feature(def)
        assert(type(def) == "table" and def.id, "RT.feature needs an id")
        assert(tostring(def.id):match("^[%w_]+$"), "feature id: letters, digits and _ only")
        def.owner, def.wasOn, def.errors = liveCurrent, false, 0
        RT.features[def.id] = def
        addOrder("f_" .. def.id)
        writeLiveUi()
    end

    function RT.action(def)
        assert(type(def) == "table" and def.id and def.run, "RT.action needs an id and run")
        assert(tostring(def.id):match("^[%w_]+$"), "action id: letters, digits and _ only")
        def.owner = liveCurrent
        RT.actions[def.id] = def
        addOrder("a_" .. def.id)
        writeLiveUi()
    end

    function RT.onTick(name, fn)
        RT.ticks[name] = { fn = fn, owner = liveCurrent, errors = 0 }
    end

    function RT.runAction(id, pawn, pc, s)
        local d = RT.actions[id]
        if not d then setStatus("cmd_result", "live action " .. id .. " is not loaded"); return end
        local ctx = context()
        local ok, res = pcall(d.run, ctx)
        setStatus("cmd_result", ok and tostring(res or ("ran " .. (d.label or id))) or ("error: " .. tostring(res)))
    end

    -- remove everything a live mod registered (switching its features off first)
    local function unloadLive(name)
        local ctx = context()
        local n = 0
        for id, d in pairs(RT.features) do
            if d.owner == name then
                if d.wasOn and d.off then pcall(d.off, ctx) end
                RT.features[id] = nil
                status["lv_" .. id] = nil
                n = n + 1
            end
        end
        for id, d in pairs(RT.actions) do if d.owner == name then RT.actions[id] = nil; n = n + 1 end end
        for k, t in pairs(RT.ticks) do if t.owner == name then RT.ticks[k] = nil; n = n + 1 end end
        local keep = {}
        for _, key in ipairs(RT.order) do
            local kind, id = key:match("^(%a)_(.+)$")
            if (kind == "f" and RT.features[id]) or (kind == "a" and RT.actions[id]) then keep[#keep + 1] = key end
        end
        RT.order = keep
        writeLiveUi()
        return n
    end

    local function loadLive(name)
        local f = io.open(LIVE_DIR .. "\\" .. name .. ".lua", "r")
        if not f then return false, "no file live\\" .. name .. ".lua" end
        local code = f:read("a")
        f:close()
        unloadLive(name)
        liveCurrent = name
        local fn, err = load(code, "@live/" .. name .. ".lua", "t", env)
        local ok, out
        if not fn then ok, out = false, "compile error: " .. tostring(err)
        else
            local outLines = {}
            env.print = function(...)
                local p = {}
                for i = 1, select("#", ...) do p[#p + 1] = tostring((select(i, ...))) end
                outLines[#outLines + 1] = table.concat(p, "\t")
            end
            local r, e = xpcall(fn, debug.traceback)
            ok = r
            if not r then outLines[#outLines + 1] = "error: " .. tostring(e) end
            out = table.concat(outLines, "\n")
        end
        liveCurrent = nil
        if not ok then unloadLive(name) end
        log("live mod %s: %s", name, ok and "loaded" or out)
        return ok, out
    end

    local function loadLiveIndex()
        local f = io.open(LIVE_DIR .. "\\_index.txt", "r")
        if not f then writeLiveUi(); return end
        for line in f:lines() do
            local name = line:match("^%s*([%w_%-]+)%s*$")
            if name then loadLive(name) end
        end
        f:close()
        writeLiveUi()
    end

    local function respond(id, ok, text)
        local tmp = BRIDGE .. "\\lua_resp_" .. id .. ".tmp"
        local f = io.open(tmp, "w")
        if not f then return end
        f:write(ok and "ok\n" or "error\n", text or "")
        f:close()
        os.rename(tmp, BRIDGE .. "\\lua_resp_" .. id .. ".txt")
    end

    local function pollBridge()
        local f = io.open(REQ_PATH, "r")
        if not f then return end
        local data = f:read("a")
        f:close()
        os.remove(REQ_PATH)
        local header, code = data:match("^(.-)\n%-%-%-\n(.*)$")
        header = header or data
        code = code or ""
        local h = {}
        for k, v in header:gmatch("([%w_]+)=([^\n]*)") do h[k] = v end
        local id = h.id or "0"
        local op = h.op or "exec"
        local ok, text
        if op == "exec" then
            ok, text = runChunk(code, "=mcp")
        elseif op == "live_load" then
            ok, text = loadLive(h.name or "")
            text = (text ~= "" and text or "") .. (ok and "\nloaded live mod " .. tostring(h.name) or "")
        elseif op == "live_unload" then
            ok, text = true, "removed " .. unloadLive(h.name or "") .. " registrations of " .. tostring(h.name)
        else
            ok, text = false, "unknown op " .. op
        end
        respond(id, ok, text)
    end

    -- features / ticks of live mods, every game tick
    local function liveTick()
        local s = RT.state or {}
        local ctx
        for id, d in pairs(RT.features) do
            local on = s["lv_" .. id] == 1
            if on or d.wasOn then
                ctx = ctx or context()
                if on then
                    if d.errors < 50 then
                        local ok, res = pcall(d.tick or function() end, ctx, s["lv_" .. id .. "_val"])
                        if ok then
                            status["lv_" .. id] = res and (tostring(res):gsub("[\r\n]", " ")) or "ok: on"
                        else
                            d.errors = d.errors + 1
                            status["lv_" .. id] = "none: error: " .. tostring(res):gsub("\n", " ")
                            if d.errors == 1 then log("live feature %s: %s", id, tostring(res)) end
                        end
                    end
                    d.wasOn = true
                else
                    if d.off then pcall(d.off, ctx) end
                    d.wasOn, d.errors = false, 0
                    status["lv_" .. id] = "off"
                end
            elseif status["lv_" .. id] == nil then
                status["lv_" .. id] = "off"
            end
        end
        for name, t in pairs(RT.ticks) do
            if t.errors < 50 then
                ctx = ctx or context()
                local ok, err = pcall(t.fn, ctx)
                if not ok then
                    t.errors = t.errors + 1
                    if t.errors == 1 then log("live tick %s: %s", name, tostring(err)) end
                end
            end
        end
    end

    local liveLoaded = false
    bridgeStep = function()
        if not liveLoaded then   -- first game-thread tick: load the saved live mods
            liveLoaded = true
            local ok, err = pcall(loadLiveIndex)
            if not ok then log("live mods: %s", tostring(err)) end
        end
        local ok, err = pcall(pollBridge)
        if not ok then log("bridge error: %s", tostring(err)) end
        ok, err = pcall(liveTick)
        if not ok then log("live tick error: %s", tostring(err)) end
    end
end

local lastFlush = 0
local function step()
    local ok, err = pcall(tick)
    if not ok then log("tick error: %s", tostring(err)) end
    bridgeStep()
    local now = os.clock()
    if now - lastFlush >= 1.0 then lastFlush = now; flushStatus() end
end

-- Run directly on the game thread. The older LoopAsync + ExecuteInGameThread pattern made
-- UE4SS hang when it stopped this mod (CTRL+R reload); a game-thread delayed action is
-- owned and cancelled by UE4SS itself.
if LoopInGameThreadWithDelay then
    LoopInGameThreadWithDelay(TICK_MS, step)
else
    local busy = false
    LoopAsync(TICK_MS, function()
        if not busy then
            busy = true
            ExecuteInGameThread(function() step(); busy = false end)
        end
        return false
    end)
end

RegisterKeyBind(Key.F9, function() ExecuteInGameThread(function() pcall(doDump) end) end)
RegisterKeyBind(Key.F10, function() log("generating class dump..."); GenerateSDK() end)

flushStatus()
log("loaded. Waiting for RonMenu (inject RonMenu.dll, press INSERT in game).")
