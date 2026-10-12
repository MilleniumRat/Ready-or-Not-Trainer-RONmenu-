-- Made by MilleniumRat (github.com/MilleniumRat)
-- Arsenal: god mode through the limb health data, no sway, magazine control, teleport methods,
-- cosmetic unlocks. Every game call below is in the class dump (ReadyOrNot.hpp):
--   god (limb health)   UCharacterHealthComponent: FLimbHealthData Head/RightLeg/LeftLeg/RightArm/LeftArm
--                       (Health, MaxHealth, Tickets), Server_SetCurrentLimbHealthToMax, Server_ResetLimbTickets,
--                       Server_SetCurrentResourceToMax, Server_SetHealthStatus, bEnableIncapacitation
--   no sway             ABaseItem: WeaponBob*ADSModifier, InertiaDragAim*, FreeAimLimitADS
--   magazines           ABaseMagazineWeapon.Magazines (FMagazine Ammo/AmmoType), MagIndex, Capacity,
--                       Server_AddMagazine (a server call, so it also reaches the host from a client)
--   vest slots          ASWATArmour.TotalSlots
--   teleports           APlayerCharacter.Server_TeleportPlayerToLocation (server call) / K2_TeleportTo (local)
--   cosmetic unlocks    UCustomizationDataBase.RequiredTags / bHideIfLocked (data in memory, not your save)
-- Written without the game running: every status line says what really happened. As a client the
-- server calls go to the host, which decides whether to accept them.
local UE = RT.UEHelpers
local saved = {}

local function isServer()
    local ok, v = pcall(function() return UE.GetKismetSystemLibrary():IsServer(RT.world()) end)
    return ok and v == true
end
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
local function restore(feature)
    for _, e in pairs(saved[feature] or {}) do if RT.valid(e.obj) then pcall(function() e.obj[e.name] = e.value end) end end
    saved[feature] = nil
end

local function v3(x, y, z) return { X = x, Y = y, Z = z } end
local function locOf(a)
    local ok, l = pcall(function() return a:K2_GetActorLocation() end)
    if ok and l then return v3(l.X, l.Y, l.Z) end
end
local function norm(a)
    local l = math.sqrt(a.X * a.X + a.Y * a.Y + a.Z * a.Z)
    if l < 1e-6 then return v3(1, 0, 0) end
    return v3(a.X / l, a.Y / l, a.Z / l)
end
local function sval(ctx, key, default)
    local v = ctx.s and ctx.s[key]
    return type(v) == "number" and v or default
end

-- ===================================================================== god mode: limb health
local LIMBS = { { "Head", 5 }, { "RightLeg", 1 }, { "LeftLeg", 2 }, { "RightArm", 3 }, { "LeftArm", 4 } }   -- ELimbType
local limbFixes = 0

RT.feature{ id = "god_limb", tab = "Health", label = "God mode (limb health)",
    tip = "A second way to be unhurtable: keeps the head, arm and leg health, the limb tickets and your overall health at maximum "
       .. "through the game's own health calls (the Server_ ones, so a client can use them too), and stops incapacitation. "
       .. "Works next to or instead of the normal God mode.",
    tick = function(ctx)
        local pawn = ctx.pawn
        if not pawn then return "waiting: no character (load into a mission)" end
        local hc
        pcall(function() hc = pawn.CharacterHealth end)
        if not RT.valid(hc) then return "none: no health component on your character" end
        local fixed = 0
        for _, L in ipairs(LIMBS) do
            pcall(function()
                local d = hc[L[1]]
                local mx, cur = d.MaxHealth, d.Health
                if cur < mx - 0.01 then
                    if not pcall(function() hc:Server_SetCurrentLimbHealthToMax(L[2]) end) then
                        pcall(function() hc:SetCurrentLimbHealthToMax(L[2]) end)
                    end
                    pcall(function() d.Health = mx end)
                    fixed = fixed + 1
                end
                if d.MaxTickets > 0 and d.Tickets < d.MaxTickets then
                    pcall(function() hc:Server_ResetLimbTickets(L[2]) end)
                    pcall(function() d.Tickets = d.MaxTickets end)
                end
            end)
        end
        pcall(function()
            if hc.Resource < hc.MaxResource - 0.01 then
                pcall(function() hc:Server_SetCurrentResourceToMax() end)
                pcall(function() hc.Resource = hc.MaxResource end)
                fixed = fixed + 1
            end
        end)
        pcall(function()
            local st = hc.HealthStatus   -- 0 healthy, 1 injured, 2 downed, 3 incapacitated
            if st == 1 or st == 2 or st == 3 then
                pcall(function() hc:Server_SetHealthStatus(0) end)
                fixed = fixed + 1
            end
        end)
        force("god_limb", hc, "bEnableIncapacitation", false)
        force("god_limb", hc, "bUnlimitedRevives", true)
        limbFixes = limbFixes + fixed
        local how = isServer() and "on your own game" or "through server calls to the host"
        return string.format("ok: limb health pinned %s (%d repairs so far)", how, limbFixes)
    end,
    off = function() restore("god_limb") end }

-- ===================================================================== no sway
-- The weapon's walk bob while aiming, the lag of the weapon behind your view and the free-aim
-- room while aiming down sights.
local NO_SWAY = { WeaponBobADSModifier = 0.0, WeaponBobRotADSModifier = 0.0, InertiaDragAimRotation = 0.0,
                  InertiaDragAimLocation = 0.0, FreeAimLimitADS = 0.0 }
RT.feature{ id = "no_sway", tab = "Weapon", label = "No sway",
    tip = "Stops the weapon from moving while you aim: no walk bob, no lag behind your view, no free-aim drift on your items.",
    tick = function(ctx)
        local n, items = 0, 0
        for _, it in ipairs(ctx.items or {}) do
            local hit = false
            for name, v in pairs(NO_SWAY) do
                if force("no_sway", it, name, v) then n = n + 1; hit = true end
            end
            if hit then items = items + 1 end
        end
        if n == 0 then return "none: no item with sway values (hold a weapon in a mission)" end
        return string.format("ok: %d sway values zeroed on %d items", n, items)
    end,
    off = function() restore("no_sway") end }

-- ===================================================================== magazines
local function magWeapons(ctx)
    local out = {}
    for _, w in ipairs(ctx.weapons or {}) do
        local ok, v = pcall(function() return w.MagIndex end)
        if ok and type(v) == "number" then out[#out + 1] = w end
    end
    return out
end
local function readMags(w)
    local list = {}
    local ok = pcall(function()
        w.Magazines:ForEach(function(_, e)
            local m = e:get()
            list[#list + 1] = { ammo = m.Ammo, typ = m.AmmoType }
        end)
    end)
    return ok and list or nil
end
local function setAmmo(w, i, n) pcall(function() w.Magazines[i].Ammo = n end) end

local sizeNow            -- rounds per magazine while "Custom magazine size" is on
local maxSeen = {}       -- weapon -> biggest magazine seen
local function capOf(w, list)
    if sizeNow then return sizeNow end
    local k = RT.addr(w)
    local c = maxSeen[k] or 0
    pcall(function() c = math.max(c, w.Capacity or 0) end)
    for _, m in ipairs(list) do if m.ammo > c then c = m.ammo end end
    maxSeen[k] = c
    return c
end

local lastAdd, totalAdded = {}, 0
RT.feature{ id = "ammo_mags", tab = "Weapon", label = "Infinite ammo (keep magazines)",
    tip = "A different way to never run out: keeps this many magazines on every weapon, all spares full. "
       .. "Whenever one is used up the game is asked to add a full one (Server_AddMagazine, so it also works as a client). "
       .. "It is not limited by your vest. Added magazines stay when you switch it off.",
    slider = { 2, 99, 12, "%.0f magazines" },
    tick = function(ctx, val)
        local want = math.max(2, math.floor(val or 12))
        local ws = magWeapons(ctx)
        if #ws == 0 then return "waiting: no weapon with magazines (load into a mission)" end
        local server = isServer()
        local t = os.clock()
        local added, topped = 0, 0
        for _, w in ipairs(ws) do
            local list = readMags(w)
            if list and #list > 0 then
                local k = RT.addr(w)
                local idx = 0
                pcall(function() idx = w.MagIndex end)
                local cap = capOf(w, list)
                local typ = (list[idx + 1] or list[1]).typ
                local fullSpare, partial = 0, {}
                for i, m in ipairs(list) do
                    if i - 1 ~= idx then
                        if m.ammo >= cap then fullSpare = fullSpare + 1 else partial[#partial + 1] = i end
                    end
                end
                if server then   -- the host can fill the half-used ones directly instead of adding more
                    for _, i in ipairs(partial) do setAmmo(w, i, cap); fullSpare = fullSpare + 1; topped = topped + 1 end
                end
                local need = want - 1 - fullSpare
                local gap = server and 0.25 or 0.5
                if cap > 0 and need > 0 and #list < 99 and (not lastAdd[k] or t - lastAdd[k] > gap) then
                    lastAdd[k] = t
                    for _ = 1, math.min(need, 4) do
                        if pcall(function() w:Server_AddMagazine({ Ammo = cap, AmmoType = typ }) end) then added = added + 1 end
                    end
                end
            end
        end
        totalAdded = totalAdded + added
        return string.format("ok: %d weapons kept at %d magazines (%d added so far%s)", #ws, want, totalAdded,
            server and "" or "; through server calls")
    end }

local sizeApplied = {}
RT.feature{ id = "ammo_size", tab = "Weapon", label = "Custom magazine size",
    tip = "Sets how many rounds every magazine of your weapons holds. Magazines you get later are set too. "
       .. "On a client only your own screen changes unless the host accepts it.",
    slider = { 1, 999, 60, "%.0f rounds" },
    tick = function(ctx, val)
        local n = math.max(1, math.floor(val or 60))
        sizeNow = n
        local ws = magWeapons(ctx)
        if #ws == 0 then return "waiting: no weapon with magazines (load into a mission)" end
        for _, w in ipairs(ws) do
            local list = readMags(w)
            if list then
                local k = RT.addr(w)
                local st = sizeApplied[k]
                force("ammo_size", w, "Capacity", n)
                if not st or st.n ~= n then
                    for i = 1, #list do setAmmo(w, i, n) end
                    sizeApplied[k] = { n = n, count = #list }
                elseif #list > st.count then
                    for i = st.count + 1, #list do setAmmo(w, i, n) end
                    st.count = #list
                end
            end
        end
        local server = isServer()
        return string.format("%s: %d weapons set to %d rounds per magazine%s", server and "ok" or "warn", #ws, n,
            server and "" or " (client: the host decides what counts)")
    end,
    off = function() sizeNow = nil; sizeApplied = {}; restore("ammo_size") end }

RT.feature{ id = "vest_slots", tab = "Weapon", label = "Vest slot limit",
    tip = "Raises the slot total of your armour (some vests allow only 15) on the vests that are loaded, and on the vest classes "
       .. "so new ones get it too. Open the loadout screen after switching it on.",
    slider = { 15, 60, 30, "%.0f slots" },
    tick = function(ctx, val)
        local n = math.floor(val or 30)
        local c = 0
        for _, a in ipairs(RT.cachedAll("SWATArmour", 1.0)) do
            if force("vest_slots", a, "TotalSlots", n) then c = c + 1 end
            pcall(function() force("vest_slots", a:GetClass():GetCDO(), "TotalSlots", n) end)
        end
        if c == 0 then return "waiting: no vest loaded yet (open the loadout screen or start a mission)" end
        return string.format("ok: slot total %d on %d vests", n, c)
    end,
    off = function() restore("vest_slots") end }

-- ===================================================================== teleport methods
local function camera(pc)
    local ok, l, f = pcall(function()
        local cam = pc.PlayerCameraManager
        local c = cam:GetCameraLocation()
        local fwd = UE.GetKismetMathLibrary():GetForwardVector(cam:GetCameraRotation())
        return v3(c.X, c.Y, c.Z), v3(fwd.X, fwd.Y, fwd.Z)
    end)
    if ok then return l, f end
end

-- Moves your character. A client asks the server (Server_TeleportPlayerToLocation); on the host,
-- and when that call is not accepted, the local teleport is used.
local function tpGo(ctx, dest, dir)
    local pawn = ctx.pawn
    if not isServer() then
        if pcall(function() pawn:Server_TeleportPlayerToLocation(dest, dir or v3(1, 0, 0)) end) then return true, "server call" end
    end
    return RT.teleportTo(pawn, dest) and true or false, "local"
end

local function traceFrom(ctx, start, fwd, dist)
    local ok, was, loc, nrm = pcall(function()
        local ksl = UE.GetKismetSystemLibrary()
        local fin = v3(start.X + fwd.X * dist, start.Y + fwd.Y * dist, start.Z + fwd.Z * dist)
        local color, hit = { R = 0, G = 0, B = 0, A = 0 }, {}
        local w = ksl:LineTraceSingle(ctx.pawn, start, fin, 0, false, {}, 0, hit, true, color, color, 0.0)
        if w then return true, v3(hit.Location.X, hit.Location.Y, hit.Location.Z), v3(hit.ImpactNormal.X, hit.ImpactNormal.Y, hit.ImpactNormal.Z) end
        return false, fin
    end)
    if not ok then return nil end
    return was, loc, nrm
end

-- where the crosshair points (same placement as the Teleport button)
local function crosshairDest(ctx)
    local cl, cf = camera(ctx.pc)
    if not cl then return nil, "no camera" end
    local was, p, n = traceFrom(ctx, cl, cf, 50000)
    if not was then return nil, "nothing under the crosshair" end
    return v3(p.X + n.X * 60, p.Y + n.Y * 60, p.Z + n.Z * 60 + 100), nil, cf
end

RT.feature{ id = "tp_blink_dist", tab = "Movement", label = "Blink distance", valueOnly = true, slider = { 2, 150, 15, "%.0f m" },
    tip = "How far 'Blink forward' jumps along your view." }
RT.action{ id = "tp_blink", tab = "Movement", label = "Blink forward",
    tip = "Jumps forward along your view by the blink distance, and stops in front of a wall. Bind a key to it for a quick dash.",
    run = function(ctx)
        if not ctx.pawn then return "needs you to be in a mission" end
        local cl, cf = camera(ctx.pc)
        local me = locOf(ctx.pawn)
        if not cl or not me then return "blink: no camera or position" end
        local dist = sval(ctx, "lv_tp_blink_dist_val", 15) * 100
        local was, p, n = traceFrom(ctx, cl, cf, dist)
        local point = p
        if was and n then point = v3(p.X + n.X * 60, p.Y + n.Y * 60, p.Z + n.Z * 60) end
        if not point then return "blink: the view trace failed" end
        -- keep the height of your body relative to the camera
        local dest = v3(me.X + (point.X - cl.X), me.Y + (point.Y - cl.Y), me.Z + (point.Z - cl.Z))
        local ok, how = tpGo(ctx, dest, cf)
        return ok and string.format("blinked %.0f m forward (%s)", dist / 100, how) or "blink: no free spot there"
    end }

-- glide: the crosshair teleport, spread over some time
local glide
RT.feature{ id = "tp_glide_time", tab = "Movement", label = "Glide time", valueOnly = true, slider = { 0.2, 4, 0.8, "%.1f s" },
    tip = "How long 'Glide to crosshair' takes." }
RT.action{ id = "tp_glide", tab = "Movement", label = "Glide to crosshair",
    tip = "Like Teleport to crosshair, but you fly there smoothly instead of jumping.",
    run = function(ctx)
        if not ctx.pawn then return "needs you to be in a mission" end
        local dest, err, cf = crosshairDest(ctx)
        if not dest then return "glide: " .. err end
        local me = locOf(ctx.pawn)
        if not me then return "glide: no position" end
        glide = { from = me, to = dest, t0 = os.clock(), dur = sval(ctx, "lv_tp_glide_time_val", 0.8), dir = cf }
        return string.format("gliding %.0f m", math.sqrt((dest.X - me.X) ^ 2 + (dest.Y - me.Y) ^ 2 + (dest.Z - me.Z) ^ 2) / 100)
    end }
RT.onTick("arsenal_glide", function(ctx)
    if not glide then return end
    if not ctx.pawn then glide = nil; return end
    local t = (os.clock() - glide.t0) / math.max(0.05, glide.dur)
    if t >= 1 then
        tpGo(ctx, glide.to, glide.dir)
        glide = nil
        return
    end
    local e = t * t * (3 - 2 * t)   -- ease in and out
    local g = glide
    tpGo(ctx, v3(g.from.X + (g.to.X - g.from.X) * e, g.from.Y + (g.to.Y - g.from.Y) * e, g.from.Z + (g.to.Z - g.from.Z) * e), g.dir)
end)

-- jump to the next one of something (press again for the next)
local cycle = {}
local function targets(kind)
    local out = {}
    if kind == "suspect" or kind == "civilian" then
        local cls = kind == "suspect" and "SuspectCharacter" or "CivilianCharacter"
        for _, ch in ipairs(RT.cachedAll(cls, 0.5)) do
            local st = RT.charState and RT.charState(ch) or ""
            if st ~= "dead" then out[#out + 1] = ch end
        end
    elseif kind == "evidence" then
        for _, a in ipairs(RT.cachedAll("EvidenceActor", 0.5)) do out[#out + 1] = a end
    elseif kind == "objective" then
        for _, p in ipairs(RT.cachedAll("ObjectivePoint", 0.5)) do
            local on = true
            pcall(function() local v = p.ObjectiveMarkerComponent.bEnabled; on = (v == true) or (v == 1) end)
            if on then out[#out + 1] = p end
        end
    end
    table.sort(out, function(a, b) return RT.addr(a) < RT.addr(b) end)
    return out
end
local function tpNext(kind, label)
    return function(ctx)
        if not ctx.pawn then return "needs you to be in a mission" end
        local list = targets(kind)
        if #list == 0 then return "no " .. label .. " found in this level" end
        cycle[kind] = ((cycle[kind] or 0) % #list) + 1
        local tgt = locOf(list[cycle[kind]])
        local me = locOf(ctx.pawn)
        if not tgt then return "teleport: no position for that " .. label end
        -- stand a little in front of it, on your side
        local away = me and norm(v3(me.X - tgt.X, me.Y - tgt.Y, 0)) or v3(1, 0, 0)
        local dest = v3(tgt.X + away.X * 180, tgt.Y + away.Y * 180, tgt.Z + 60)
        local ok, how = tpGo(ctx, dest, v3(-away.X, -away.Y, 0))
        return ok and string.format("teleported to %s %d of %d (%s)", label, cycle[kind], #list, how) or "teleport: no free spot there"
    end
end
RT.action{ id = "tp_next_suspect", tab = "Movement", label = "Teleport to next suspect",
    tip = "Cycles through the suspects that are still up.", run = tpNext("suspect", "suspect") }
RT.action{ id = "tp_next_civilian", tab = "Movement", label = "Teleport to next civilian",
    tip = "Cycles through the civilians that are still up.", run = tpNext("civilian", "civilian") }
RT.action{ id = "tp_next_evidence", tab = "Movement", label = "Teleport to next evidence",
    tip = "Cycles through the evidence in the level.", run = tpNext("evidence", "evidence") }
RT.action{ id = "tp_next_objective", tab = "Movement", label = "Teleport to next objective",
    tip = "Cycles through the game's objective markers (the ones Objective ESP shows).", run = tpNext("objective", "objective") }
RT.action{ id = "tp_up_game", tab = "Movement", label = "Teleport up (game)",
    tip = "The game's own TeleportUp developer function. May do nothing in the release build.",
    run = function(ctx)
        if not ctx.pc then return "needs you to be in a mission" end
        local ok, err = pcall(function() ctx.pc:TeleportUp() end)
        return ok and "called TeleportUp" or ("TeleportUp failed: " .. tostring(err))
    end }

-- ===================================================================== unlock cosmetic items
-- Clothing, helmets and other customisation items are locked by progression tags
-- (UCustomizationDataBase.RequiredTags). Clearing them lifts the lock in memory only: your save
-- is not touched and it lasts until the game restarts. Items locked to a DLC you do not own
-- (LockedToDLC) are left alone. Weapons and gear show no lock of this kind in the class dump.
local unlockAt, unlocked, scanned = 0, 0, 0
RT.feature{ id = "unlock_items", tab = "Unlocks", label = "Unlock all cosmetic items",
    tip = "Lifts the progression lock on clothing, helmets and other customisation items. Nothing is saved: it lasts until the "
       .. "game restarts. DLC you do not own stays locked. Leave it on while you browse, items load as the menus open.",
    tick = function()
        local t = os.clock()
        if t - unlockAt > 2.0 then
            unlockAt = t
            scanned = 0
            for _, o in ipairs(FindAllOf("CustomizationDataBase") or {}) do
                if RT.valid(o) then
                    scanned = scanned + 1
                    pcall(function()
                        if o.RequiredTags:GetArrayNum() > 0 then
                            o.RequiredTags:Empty()
                            unlocked = unlocked + 1
                        end
                        if o.bHideIfLocked then o.bHideIfLocked = false end
                    end)
                end
            end
        end
        if scanned == 0 then return "waiting: no customisation data loaded yet (open the loadout / customise screen)" end
        return string.format("ok: %d items unlocked so far (%d loaded)", unlocked, scanned)
    end }

RT.action{ id = "items_diag", tab = "Unlocks", label = "Check item locks",
    tip = "Writes how many customisation items are locked by progression tags and how many tags your Commander profile has.",
    run = function()
        local total, locked, dlc = 0, 0, 0
        for _, o in ipairs(FindAllOf("CustomizationDataBase") or {}) do
            if RT.valid(o) then
                total = total + 1
                pcall(function() if o.RequiredTags:GetArrayNum() > 0 then locked = locked + 1 end end)
                pcall(function() if o.LockedToDLC:GetArrayNum() > 0 then dlc = dlc + 1 end end)
            end
        end
        local tags = "?"
        pcall(function()
            local p = FindFirstOf("CommanderProfile")
            if RT.valid(p) then
                local n = 0
                p.ProgressionTags:ForEach(function() n = n + 1 end)
                tags = tostring(n)
            end
        end)
        return string.format("cosmetics loaded: %d, locked by tags: %d, DLC-locked: %d, profile tags: %s", total, locked, dlc, tags)
    end }
