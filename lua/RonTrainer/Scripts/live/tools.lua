-- Made by MilleniumRat (github.com/MilleniumRat)
-- Player / weapon / AI / door / lobby tools. Every call below exists in the class dump
-- (ReadyOrNot.hpp); see ronmenu\docs\feature_matrix.md for evidence and what is NOT implemented.
-- Server-side ones refuse with a reason when you are a client in someone else's game.
local UE = RT.UEHelpers
local saved = {}   -- [feature] = { [key] = {obj, name, value} }

local function isServer()
    local ok, v = pcall(function() return UE.GetKismetSystemLibrary():IsServer(RT.world()) end)
    return ok and v == true
end
local CLIENT = "you are a client in someone else's game: only the host's game can do this"

local function force(feature, obj, name, value)
    saved[feature] = saved[feature] or {}
    local key = RT.addr(obj) .. name
    if not saved[feature][key] then
        local ok, v = pcall(function() return obj[name] end)
        if not ok or v == nil then return false end
        saved[feature][key] = { obj = obj, name = name, value = v }
    end
    pcall(function() if obj[name] ~= value then obj[name] = value end end)
    return true
end
local function base(feature, obj, name)
    local e = saved[feature] and saved[feature][RT.addr(obj) .. name]
    if e then return e.value end
    local ok, v = pcall(function() return obj[name] end)
    return ok and v or nil
end
local function restore(feature)
    for _, e in pairs(saved[feature] or {}) do if RT.valid(e.obj) then pcall(function() e.obj[e.name] = e.value end) end end
    saved[feature] = nil
end

local function debugSub() local d = FindFirstOf("ReadyOrNotDebugSubsystem"); return RT.valid(d) and d or nil end

-- one debug-subsystem switch as a feature (restores the previous value when off)
local function debugSwitch(id, tab, label, tip, prop, needHost)
    RT.feature{ id = id, tab = tab, label = label, tip = tip,
        tick = function()
            if needHost and not isServer() then return "none: " .. CLIENT end
            local d = debugSub()
            if not d then return "none: the game's debug subsystem is missing" end
            force(id, d, prop, true)
            return "ok: the game's own " .. prop .. " switch is on"
        end,
        off = function() restore(id) end }
end

local function lookedAt(ctx)
    if not (ctx.pawn and ctx.pc) then return nil end
    local hit = RT.traceCrosshair(ctx.pawn, ctx.pc)
    if not hit then return nil end
    local ok, a = pcall(UE.GetActorFromHitResult, hit)
    return ok and RT.valid(a) and a or nil
end
local function isA(obj, path) local c = RT.cdo(path); return c and obj and pcall(function() return obj:IsA(c) end) and obj:IsA(c) end
local function dist(a, b)
    local ok, la = pcall(function() return a:K2_GetActorLocation() end)
    local ok2, lb = pcall(function() return b:K2_GetActorLocation() end)
    if not (ok and ok2) then return 1e9 end
    return math.sqrt((la.X - lb.X) ^ 2 + (la.Y - lb.Y) ^ 2 + (la.Z - lb.Z) ^ 2)
end
-- "look" = what the crosshair is on; "near" = everything of that class within the radius slider
local function pick(ctx, cls, path, radiusId, mode)
    if mode == "look" then
        local a = lookedAt(ctx)
        if a and isA(a, path) then return { a } end
        return {}
    end
    local r = (ctx.s["lv_" .. radiusId .. "_val"] or 10) * 100
    local out = {}
    for _, o in ipairs(FindAllOf(cls) or {}) do if RT.valid(o) and dist(o, ctx.pawn) <= r then out[#out + 1] = o end end
    return out
end

-- ================================================================ PLAYER

local healAt = 0

local fxStopped = 0

local function speedMod(id, label, tip, prop)
    RT.feature{ id = id, tab = "Movement", label = label, tip = tip, slider = { 0.5, 3.0, 1.5, "x%.2f" },
        tick = function(ctx, val)
            local p = ctx.pawn
            if not p then return "waiting: no player" end
            local b = base(id, p, prop)
            if not b then return "none: no " .. prop end
            force(id, p, prop, b * (val or 1.5))
            return string.format("ok: %s %.2f -> %.2f", prop, b, b * (val or 1.5))
        end,
        off = function() restore(id) end }
end
speedMod("crouch_speed", "Crouched speed", "Multiplies how fast you move while crouched (SpeedModifier_Crouch).", "SpeedModifier_Crouch")
speedMod("aim_speed", "Aiming speed", "Multiplies how fast you move while aiming down sights (SpeedModifier_Aim).", "SpeedModifier_Aim")

-- ================================================================ WEAPONS
local function weaponScale(id, label, tip, props, lo, hi, def)
    RT.feature{ id = id, tab = "Weapon", label = label, tip = tip, slider = { lo, hi, def, "x%.2f" },
        tick = function(ctx, val)
            local n = 0
            for _, w in ipairs(ctx.weapons or {}) do
                for _, pr in ipairs(props) do
                    local b = base(id, w, pr)
                    if type(b) == "number" then force(id, w, pr, b * (val or def)); n = n + 1 end
                end
            end
            return n > 0 and string.format("ok: %d values x%.2f on %d weapons", n, val or def, #(ctx.weapons or {})) or "waiting: no weapons"
        end,
        off = function() restore(id) end }
end
weaponScale("recoil_v", "Vertical recoil", "Scales how far the muzzle climbs (RecoilMultiplierPitch). Changes real aim.", { "RecoilMultiplierPitch" }, 0, 2, 0.5)
weaponScale("recoil_h", "Horizontal recoil", "Scales sideways kick (RecoilMultiplierYaw). Changes real aim.", { "RecoilMultiplierYaw" }, 0, 2, 0.5)
weaponScale("cam_kick", "Camera kick", "Scales the procedural recoil push on the camera (RecoilFireStrength, RecoilFireStrengthFirst, RecoilAngleStrength).",
    { "RecoilFireStrength", "RecoilFireStrengthFirst", "RecoilAngleStrength" }, 0, 2, 0.5)
weaponScale("spread_ads", "Aimed spread", "Scales spread while aiming down sights (ADSSpreadMultiplier). Changes where bullets go.", { "ADSSpreadMultiplier" }, 0, 2, 0.5)
weaponScale("spread_hip", "Hip / movement spread", "Scales first-shot and moving spread (FirstShotSpread, VelocitySpreadMultiplier).",
    { "FirstShotSpread", "VelocitySpreadMultiplier" }, 0, 2, 0.5)
local function itemRate(id, label, tip, prop)
    RT.feature{ id = id, tab = "Weapon", label = label, tip = tip, slider = { 1, 6, 2.5, "x%.1f" },
        tick = function(ctx, val)
            local n = 0
            for _, it in ipairs(ctx.items or {}) do
                local b = base(id, it, prop)
                if type(b) == "number" then force(id, it, prop, b * (val or 2.5)); n = n + 1 end
            end
            return n > 0 and string.format("ok: %s x%.1f on %d items", prop, val or 2.5, n) or "waiting: no items"
        end,
        off = function() restore(id) end }
end
itemRate("draw_speed", "Draw speed", "How fast items come out (DrawPlayRate).", "DrawPlayRate")
itemRate("holster_speed", "Holster speed", "How fast items are put away (HolsterPlayRate).", "HolsterPlayRate")

debugSwitch("inf_items", "Grenades", "Unlimited grenades and tools",
    "The game's own 'infinite SWAT items' switch: flashbangs, stingers, gas, wedges and C2 are not used up. Host / solo only.", "bInfiniteSWATItems", true)

-- ================================================================ AI
debugSwitch("ai_stop_move", "AI", "Stop suspect and civilian movement", "The game's own 'disable AI movement' switch. Host / solo only.", "bDisableAIMovement", true)
debugSwitch("swat_stop_move", "AI", "Stop SWAT team movement", "The game's own 'disable SWAT movement' switch. Host / solo only.", "bDisableSwatMovement", true)
debugSwitch("ai_no_target", "AI", "Suspects don't target anyone", "The game's own 'disable AI targeting' switch: suspects stop aiming and shooting. Host / solo only.", "bDisableAITargeting", true)
RT.feature{ id = "ai_radius", tab = "AI", label = "Nearby radius", tip = "Used by the 'nearby' AI buttons.", valueOnly = true, slider = { 2, 50, 10, "%.0f m" } }

local function aiAction(id, label, tip, fn, danger, includeSwat)
    for _, mode in ipairs({ "look", "near" }) do
        RT.action{ id = id .. "_" .. mode, tab = "AI", danger = danger, tip = tip,
            label = label .. (mode == "look" and " (crosshair)" or " (nearby)"),
            run = function(ctx)
                if not isServer() then return CLIENT end
                local all = pick(ctx, "CyberneticCharacter", "/Script/ReadyOrNot.CyberneticCharacter", "ai_radius", mode)
                local list = {}
                for _, ch in ipairs(all) do   -- your own SWAT squad is never a target unless asked
                    if includeSwat or not isA(ch, "/Script/ReadyOrNot.SWATCharacter") then list[#list + 1] = ch end
                end
                if #list == 0 then return mode == "look" and "aim at a suspect or civilian" or "nobody nearby" end
                local n = 0
                for _, ch in ipairs(list) do if pcall(fn, ch, ctx) then n = n + 1 end end
                return label .. ": " .. n .. " of " .. #list
            end }
    end
end
aiAction("ai_surrender", "Surrender", "The character's own Surrender() compliance transition.", function(ch) ch:Surrender() end)
aiAction("ai_cuff", "Cuff", "Native arrest completion with your zipcuffs. Only for characters that already surrendered.", function(ch, ctx)
    if not ch:IsSurrendered() then error("not surrendered") end
    local cuffs
    ctx.pawn.InventoryComp.InventoryItems:ForEach(function(_, e) local it = e:get(); if RT.className(it):find("Zipcuffs") then cuffs = it end end)
    ch:ArrestComplete(ctx.pawn, cuffs)
end)
aiAction("ai_kill", "Eliminate", "The character's own Kill(): normal death processing, counts on your score.", function(ch) ch:Kill() end, true)
aiAction("ai_bring", "Bring here", "Teleports them in front of you.", function(ch, ctx)
    local l, yaw = ctx.pawn:K2_GetActorLocation(), math.rad(ctx.pawn:K2_GetActorRotation().Yaw)
    ch:K2_TeleportTo({ X = l.X + math.cos(yaw) * 200, Y = l.Y + math.sin(yaw) * 200, Z = l.Z + 5 }, { Pitch = 0.0, Yaw = math.deg(yaw) + 180, Roll = 0.0 })
end, false, true)

-- ================================================================ DOORS
RT.feature{ id = "door_radius", tab = "Doors", label = "Nearby radius", tip = "Used by the 'nearby' door, trap, evidence and report buttons.", valueOnly = true, slider = { 2, 50, 10, "%.0f m" } }

local function doorAction(id, label, tip, fn)
    for _, mode in ipairs({ "look", "near" }) do
        RT.action{ id = id .. "_" .. mode, tab = "Doors", tip = tip, label = label .. (mode == "look" and " (crosshair)" or " (nearby)"),
            run = function(ctx)
                if not isServer() then return CLIENT end
                local list = pick(ctx, "Door", "/Script/ReadyOrNot.Door", "door_radius", mode)
                if #list == 0 then return mode == "look" and "aim at a door" or "no doors nearby" end
                local n = 0
                for _, d in ipairs(list) do if pcall(fn, d, ctx) then n = n + 1 end end
                return label .. ": " .. n .. " doors"
            end }
    end
end
doorAction("d_unlock", "Unlock", "The door's own UnlockDoor().", function(d) d:UnlockDoor(true) end)
doorAction("d_lock", "Lock", "The door's own LockDoor().", function(d) d:LockDoor(true) end)
doorAction("d_open", "Open", "The door's own OpenDoor() (traps on the door still trigger as normal).", function(d, ctx) d:OpenDoor(ctx.pawn, false, true, false) end)
doorAction("d_close", "Close", "The door's own CloseDoor().", function(d, ctx) d:CloseDoor(ctx.pawn, false, true) end)
doorAction("d_kick", "Kick open", "The door's own forced KickDoor(). Broken doors can't be repaired.", function(d, ctx) d:KickDoor(ctx.pawn, false, true) end)
doorAction("d_c2", "Blow open (C2)", "The door's own ExplodeDoor(). Destroyed doors can't be repaired.", function(d, ctx) d:ExplodeDoor(ctx.pawn, ctx.pawn, false) end)

RT.action{ id = "d_open_all", tab = "Doors", label = "Open all doors", tip = "The game's own OpenAllDoors() debug function.",
    run = function() if not isServer() then return CLIENT end; local d = debugSub(); if not d then return "no debug subsystem" end; d:OpenAllDoors(); return "opened all doors" end }
RT.action{ id = "d_close_all", tab = "Doors", label = "Close all doors", tip = "The game's own CloseAllDoors() debug function.",
    run = function() if not isServer() then return CLIENT end; local d = debugSub(); if not d then return "no debug subsystem" end; d:CloseAllDoors(); return "closed all doors" end }

RT.action{ id = "trap_disarm", tab = "Doors", label = "Disarm traps (nearby)", tip = "Native trap disarm on live traps within the radius.",
    run = function(ctx)
        if not isServer() then return CLIENT end
        local n = 0
        for _, t in ipairs(pick(ctx, "TrapActor", "/Script/ReadyOrNot.TrapActor", "door_radius", "near")) do
            local ok, st = pcall(function() return t.TrapStatus end)
            if ok and st == 0 then
                if pcall(function() t:Server_OnTrapDisarmed() end) or pcall(function() t:Multicast_OnTrapDisarmed() end) then n = n + 1 end
            end
        end
        return "disarmed " .. n .. " live traps"
    end }

RT.action{ id = "ev_secure", tab = "Doors", label = "Collect evidence (nearby)", tip = "Secure() on evidence and dropped weapons within the radius: the same call as picking them up.",
    run = function(ctx)
        if not isServer() then return CLIENT end
        local n = 0
        for _, cls in ipairs({ "EvidenceActor", "PickupWeaponActor" }) do
            for _, a in ipairs(pick(ctx, cls, "/Script/ReadyOrNot." .. cls, "door_radius", "near")) do
                if pcall(function() a:Secure(ctx.pawn) end) then n = n + 1 end
            end
        end
        return "secured " .. n .. " items"
    end }

RT.action{ id = "report_near", tab = "Doors", label = "Report to TOC (nearby)", tip = "Normal report processing for down, dead or arrested characters within the radius that haven't been reported.",
    run = function(ctx)
        if not isServer() then return CLIENT end
        local n = 0
        for _, ch in ipairs(pick(ctx, "ReadyOrNotCharacter", "/Script/ReadyOrNot.ReadyOrNotCharacter", "door_radius", "near")) do
            pcall(function()
                if ch.bHasBeenReported then return end
                if not (ch:IsDeadOrUnconscious() or ch.bArrestComplete) then return end
                if RT.addr(ch) == RT.addr(ctx.pawn) then return end
                ctx.pawn:Server_ReportToTOC(ch, false, true)
                n = n + 1
            end)
        end
        return "reported " .. n
    end }

-- ================================================================ LOBBY
RT.feature{ id = "no_retaliation", tab = "Mission", label = "No SWAT retaliation",
    tip = "Raises the team-kill limit before your AI squad turns on you (CoopGM.MaxTeamKillsBeforeAIRetaliates). Does not stop an attack already underway. Missions only, host / solo.",
    tick = function()
        if not isServer() then return "none: " .. CLIENT end
        local gm = FindFirstOf("CoopGM")
        if not RT.valid(gm) then return "waiting: only in missions (co-op game mode)" end
        force("no_retaliation", gm, "MaxTeamKillsBeforeAIRetaliates", 9999)
        return "ok: retaliation limit 9999"
    end,
    off = function() restore("no_retaliation") end }

