-- Made by MilleniumRat (github.com/MilleniumRat)
-- Player playground backend for the menu's FUN > players tab.
-- Every function used here was checked in the class dump (ReadyOrNot.hpp / Engine.hpp) and, where
-- marked "live", tested in game:
--   roster      AGameStateBase.PlayerArray -> APlayerState:GetPlayerName/GetPawn, PlayerId, bIsABot   (live)
--   ammo        ABaseMagazineWeapon.Magazines (FMagazine Ammo/AmmoType), MagIndex, Capacity,
--               SetMagazineCount(Count, AmmoTypes), ReplenishAmmo, Server_AddMagazine            (live)
--   items       UInventoryComponent.InventoryItems, AddInventoryItem, DestroyInventoryItem        (live: add)
--   heal        ResetHealth, CharacterHealth:Server_ResetAllLimbHealth, SetCurrentResourceToMax
--   ragdoll     AReadyOrNotCharacter EnableRagdoll/DisableRagdoll, Multicast_EnableRagdoll/DisableRagdoll
--   move        AActor K2_TeleportTo, ACharacter LaunchCharacter
--   world       UExponentialHeightFogComponent setters, ULightComponent SetIntensity/SetLightColor (local)
-- Authority: only the server (host or single player) may change players. As a client the host's game
-- owns your state, so those actions are refused with a reason instead of desyncing you.
local UE = RT.UEHelpers
local undo = {}          -- stack of { label, fn }
local gifts = {}         -- items we spawned
local ammoSnap = {}      -- [weapon addr] = { w, mags = {{a, t}}, idx }
local posSnap = {}       -- [pawn addr] = { pawn, loc, rot }
local flops = {}         -- [pawn addr] = { pawn, untilT, host }

local function split(s)
    local t = {}
    for part in (tostring(s or "") .. "|"):gmatch("([^|]*)|") do t[#t + 1] = part end
    return t
end

local function isServer()
    local ok, v = pcall(function() return UE.GetKismetSystemLibrary():IsServer(RT.world()) end)
    return ok and v == true
end

local function localPawn() return (RT.getPawn()) end

-- all players: { ps, pawn, name, id, me }
local function players()
    local out, me = {}, localPawn()
    local gs = FindFirstOf("GameStateBase")
    if not RT.valid(gs) then return out end
    pcall(function()
        gs.PlayerArray:ForEach(function(_, e)
            local ps = e:get()
            if RT.valid(ps) then
                local pawn, name, id = nil, "?", 0
                pcall(function() pawn = ps:GetPawn() end)
                pcall(function() name = ps:GetPlayerName():ToString() end)
                pcall(function() id = ps.PlayerId end)
                if not RT.valid(pawn) then pawn = nil end
                out[#out + 1] = { ps = ps, pawn = pawn, name = name, id = id,
                                  me = pawn ~= nil and me ~= nil and RT.addr(pawn) == RT.addr(me) }
            end
        end)
    end)
    return out
end

-- target "self" / "all" / "<PlayerId>"  ->  list of players with a pawn
local function targets(t)
    local out = {}
    for _, p in ipairs(players()) do
        if p.pawn and (t == "all" or (t == "self" and p.me) or tostring(p.id) == t) then out[#out + 1] = p end
    end
    return out
end

-- guard: returns an error text, or nil if allowed
local function needHost(list)
    if #list == 0 then return "no target with a character (load into the station or a mission)" end
    if not isServer() then
        return "you are a client in someone else's game: only the host can change players (host the lobby to use this)"
    end
    return nil
end

local function pushUndo(label, fn)
    undo[#undo + 1] = { label = label, fn = fn }
    if #undo > 30 then table.remove(undo, 1) end
end

-- ---------------------------------------------------------------- roster / item list for the menu
local rosterAt, itemsAt = 0, 0
local function esc(s) return (tostring(s):gsub("[|;\r\n]", " ")) end
RT.onTick("pg_roster", function()
    local now = os.clock()
    if now - rosterAt >= 0.5 then
        rosterAt = now
        local parts = {}
        for _, p in ipairs(players()) do
            local bot = false
            pcall(function() bot = p.ps.bIsABot end)
            parts[#parts + 1] = table.concat({ p.id, esc(p.name), p.pawn and 1 or 0, p.me and 1 or 0, bot and 1 or 0 }, "|")
        end
        RT.status.pg_roster = table.concat(parts, ";")
        local w = RT.world()
        if not RT.valid(w) then RT.status.pg_auth = "none"
        else
            local ok, sa = pcall(function() return UE.GetKismetSystemLibrary():IsStandalone(w) end)
            RT.status.pg_auth = (ok and sa) and "solo" or (isServer() and "host" or "client")
        end
        RT.status.pg_undo = #undo > 0 and esc(undo[#undo].label) or ""
    end
    if now - itemsAt >= 3.0 then
        itemsAt = now
        local seen, list = {}, {}
        local function add(it)
            local ok, path = pcall(function() return it:GetClass():GetFullName() end)
            if not ok or not path then return end
            path = path:gsub("^%S+%s+", "")
            if seen[path] then return end
            seen[path] = true
            local label = path:match("%.([^.]+)$") or path
            list[#list + 1] = esc(path) .. "|" .. esc((label:gsub("_C$", "")))
        end
        for _, p in ipairs(players()) do
            pcall(function() p.pawn.InventoryComp.InventoryItems:ForEach(function(_, e) local it = e:get(); if RT.valid(it) then add(it) end end) end)
        end
        RT.status.pg_items = table.concat(list, ";")
    end
end)

-- ---------------------------------------------------------------- movement
local function remember(pawn)
    local ok, l = pcall(function() return pawn:K2_GetActorLocation() end)
    local ok2, r = pcall(function() return pawn:K2_GetActorRotation() end)
    if ok and ok2 then posSnap[RT.addr(pawn)] = { pawn = pawn, loc = { X = l.X, Y = l.Y, Z = l.Z }, rot = { Pitch = 0.0, Yaw = r.Yaw, Roll = 0.0 } } end
end

local function teleport(pawn, loc, yaw)
    remember(pawn)
    return pcall(function() pawn:K2_TeleportTo(loc, { Pitch = 0.0, Yaw = yaw, Roll = 0.0 }) end)
end

local function offsetFrom(pawn, side, dist)
    local l = pawn:K2_GetActorLocation()
    local yaw = pawn:K2_GetActorRotation().Yaw
    local a = math.rad(yaw + ({ behind = 180, left = -90, right = 90, front = 0 })[side] or 180)
    return { X = l.X + math.cos(a) * dist, Y = l.Y + math.sin(a) * dist, Z = l.Z + 5 }, yaw
end

-- pg_tp_to: "<target>|side|dist"   you go to the target
RT.action{ id = "pg_tp_to", tab = "hidden", label = "Teleport to player", run = function(ctx)
    local a = split(ctx.s.cmd_arg)
    local t = targets(a[1])[1]
    local me = localPawn()
    if not t or not me then return "pick one player with a character" end
    if t.me then return "that is you" end
    local loc, yaw = offsetFrom(t.pawn, a[2] ~= "" and a[2] or "behind", tonumber(a[3]) or 150)
    local ok = teleport(me, loc, yaw)
    pushUndo("teleport to " .. t.name, function() local s = posSnap[RT.addr(me)]; if s then me:K2_TeleportTo(s.loc, s.rot) end end)
    return ok and ("teleported to " .. t.name .. (isServer() and "" or " (client: the host may move you back)")) or "teleport failed"
end }

-- pg_bring: "<target>|spacing"   targets come to you, side by side in front of you
RT.action{ id = "pg_bring", tab = "hidden", label = "Bring players", run = function(ctx)
    local a = split(ctx.s.cmd_arg)
    local list = {}
    for _, p in ipairs(targets(a[1])) do if not p.me then list[#list + 1] = p end end
    local err = needHost(list)
    if err then return err end
    local me = localPawn()
    local l, yaw = me:K2_GetActorLocation(), me:K2_GetActorRotation().Yaw
    local sp = tonumber(a[2]) or 120
    local f, r = math.rad(yaw), math.rad(yaw + 90)
    local n = 0
    for i, p in ipairs(list) do
        local side = (i - (#list + 1) / 2) * sp
        local loc = { X = l.X + math.cos(f) * 200 + math.cos(r) * side, Y = l.Y + math.sin(f) * 200 + math.sin(r) * side, Z = l.Z + 5 }
        if teleport(p.pawn, loc, yaw + 180) then n = n + 1 end
    end
    pushUndo("bring", function() for _, p in ipairs(list) do local s = posSnap[RT.addr(p.pawn)]; if s and RT.valid(p.pawn) then p.pawn:K2_TeleportTo(s.loc, s.rot) end end end)
    return "brought " .. n .. " players"
end }

-- pg_form: "<target>|line/circle/stairs|spacing"   formation in front of you, facing you
RT.action{ id = "pg_form", tab = "hidden", label = "Formation", run = function(ctx)
    local a = split(ctx.s.cmd_arg)
    local list = targets(a[1])
    local err = needHost(list)
    if err then return err end
    local me = localPawn()
    local l, yaw = me:K2_GetActorLocation(), me:K2_GetActorRotation().Yaw
    local sp = tonumber(a[3]) or 120
    local f, r = math.rad(yaw), math.rad(yaw + 90)
    local cx, cy = l.X + math.cos(f) * 400, l.Y + math.sin(f) * 400
    local n = 0
    for i, p in ipairs(list) do
        local loc, face
        if a[2] == "circle" then
            local ang = (i - 1) / #list * math.pi * 2
            local rad = math.max(sp, sp * #list / (2 * math.pi))
            loc = { X = cx + math.cos(ang) * rad, Y = cy + math.sin(ang) * rad, Z = l.Z + 5 }
            face = math.deg(math.atan(cy - loc.Y, cx - loc.X))
        else
            local side = (i - (#list + 1) / 2) * sp
            local back = a[2] == "stairs" and (i - 1) * 60 or 0
            loc = { X = cx + math.cos(r) * side + math.cos(f) * back, Y = cy + math.sin(r) * side + math.sin(f) * back, Z = l.Z + 5 }
            face = yaw + 180
        end
        if teleport(p.pawn, loc, face) then n = n + 1 end
    end
    pushUndo(a[2] .. " formation", function() for _, p in ipairs(list) do local s = posSnap[RT.addr(p.pawn)]; if s and RT.valid(p.pawn) then p.pawn:K2_TeleportTo(s.loc, s.rot) end end end)
    return n .. " players in a " .. a[2]
end }

RT.action{ id = "pg_return", tab = "hidden", label = "Return players", run = function(ctx)
    local list = targets(split(ctx.s.cmd_arg)[1])
    local err = needHost(list)
    if err and not (#list == 1 and list[1].me) then return err end
    local n = 0
    for _, p in ipairs(list) do
        local s = posSnap[RT.addr(p.pawn)]
        if s and pcall(function() p.pawn:K2_TeleportTo(s.loc, s.rot) end) then n = n + 1 end
    end
    return "returned " .. n .. " players to where they were before"
end }

-- ---------------------------------------------------------------- health / ragdoll / launch
RT.action{ id = "pg_heal", tab = "hidden", label = "Heal", run = function(ctx)
    local list = targets(split(ctx.s.cmd_arg)[1])
    local err = needHost(list)
    if err then return err end
    for _, p in ipairs(list) do
        pcall(function() p.pawn:ResetHealth() end)
        pcall(function() local hc = p.pawn.CharacterHealth; hc:Server_ResetAllLimbHealth(); hc:SetCurrentResourceToMax() end)
    end
    return "healed " .. #list
end }

-- pg_flop: "<target>|seconds"   native timed ragdoll, multicast so everyone sees it, auto get-up
RT.action{ id = "pg_flop", tab = "hidden", label = "Flop (ragdoll)", run = function(ctx)
    local a = split(ctx.s.cmd_arg)
    local list = targets(a[1])
    local err = needHost(list)
    if err then return err end
    local dur = math.max(0.5, math.min(10, tonumber(a[2]) or 3))
    local n = 0
    for _, p in ipairs(list) do
        local ok = pcall(function() p.pawn:Multicast_EnableRagdoll(dur, true) end)
        if not ok then ok = pcall(function() p.pawn:EnableRagdoll(dur, true) end) end
        if ok then flops[RT.addr(p.pawn)] = { pawn = p.pawn, untilT = os.clock() + dur + 0.5 }; n = n + 1 end
    end
    return "flopped " .. n .. " for " .. dur .. " s"
end }

local function unflop(pawn)
    pcall(function() pawn:Multicast_DisableRagdoll() end)
    pcall(function() pawn:DisableRagdoll() end)
end
RT.action{ id = "pg_getup", tab = "hidden", label = "Get up", run = function(ctx)
    local list = targets(split(ctx.s.cmd_arg)[1])
    local err = needHost(list)
    if err then return err end
    for _, p in ipairs(list) do unflop(p.pawn); flops[RT.addr(p.pawn)] = nil end
    return "got " .. #list .. " players up"
end }
RT.onTick("pg_flops", function()   -- safety net: make sure everyone gets up
    local now = os.clock()
    for k, f in pairs(flops) do
        if now >= f.untilT then
            if RT.valid(f.pawn) then
                local ok, still = pcall(function() return f.pawn:IsInRagdoll() end)
                if ok and still then unflop(f.pawn) end
            end
            flops[k] = nil
        end
    end
end)

-- pg_launch: "<target>|up|forward"
RT.action{ id = "pg_launch", tab = "hidden", label = "Launch", run = function(ctx)
    local a = split(ctx.s.cmd_arg)
    local list = targets(a[1])
    local err = needHost(list)
    if err then return err end
    local up, fw = tonumber(a[2]) or 900, tonumber(a[3]) or 0
    local n = 0
    for _, p in ipairs(list) do
        remember(p.pawn)
        local yaw = math.rad(p.pawn:K2_GetActorRotation().Yaw)
        if pcall(function() p.pawn:LaunchCharacter({ X = math.cos(yaw) * fw, Y = math.sin(yaw) * fw, Z = up + 0.0 }, true, true) end) then n = n + 1 end
    end
    pushUndo("launch", function() for _, p in ipairs(list) do local s = posSnap[RT.addr(p.pawn)]; if s and RT.valid(p.pawn) then p.pawn:K2_TeleportTo(s.loc, s.rot) end end end)
    return "launched " .. n
end }

-- ---------------------------------------------------------------- ammo
local function hasMags(it) return RT.propsOf(it).Magazines ~= nil end

local function weaponOf(pawn, which)
    local inv = pawn.InventoryComp
    if which == "held" then
        local ok, it = pcall(function() return inv:GetEquippedItem() end)
        if ok and RT.valid(it) and hasMags(it) then return it end
    end
    local found, any
    inv.InventoryItems:ForEach(function(_, e)
        local it = e:get()
        if RT.valid(it) and hasMags(it) then
            local n = RT.className(it)
            any = any or it
            if not found and ((which == "secondary" and n:find("^Secondary")) or (which ~= "secondary" and n:find("^Primary"))) then found = it end
        end
    end)
    return found or any
end

local function ammoNames(pawn, w, count)
    local slot = RT.className(w):find("^Secondary") and "SecondaryAmmoSlots" or "PrimaryAmmoSlots"
    local name = "None"
    pcall(function() name = pawn.InventoryComp.LastEquippedLoadout[slot][1]:ToString() end)
    local t = {}
    for i = 1, count do t[i] = FName(name) end
    return t
end

local function snapshot(w)
    local k = RT.addr(w)
    if ammoSnap[k] then return end
    local mags = {}
    w.Magazines:ForEach(function(_, e) local m = e:get(); mags[#mags + 1] = { m.Ammo, m.AmmoType } end)
    ammoSnap[k] = { w = w, mags = mags, idx = w.MagIndex }
end

local function setMags(pawn, w, list)   -- list = {{ammo, type}, ...}; first entry = the inserted magazine
    w:SetMagazineCount(#list, ammoNames(pawn, w, #list))
    local have = 0
    w.Magazines:ForEach(function() have = have + 1 end)
    for i = have + 1, #list do pcall(function() w:Server_AddMagazine({ Ammo = list[i][1], AmmoType = list[i][2] }) end) end
    for i = 1, #list do pcall(function() w.Magazines[i].Ammo = list[i][1]; w.Magazines[i].AmmoType = list[i][2] end) end
    pcall(function() w.MagIndex = 0 end)
end

-- pg_ammo: "<target>|weapon|mode|n"   modes: set / empty / refill / nospares / lastbullet / onemag / restore
RT.action{ id = "pg_ammo", tab = "hidden", label = "Ammo", run = function(ctx)
    local a = split(ctx.s.cmd_arg)
    local list = targets(a[1])
    local err = needHost(list)
    if err then return err end
    local mode, n, done = a[3], tonumber(a[4]) or 1, 0
    for _, p in ipairs(list) do
        local w = weaponOf(p.pawn, a[2])
        if w then
            local ok = pcall(function()
                local idx = w.MagIndex + 1
                local cur = w.Magazines[idx]
                local cap = w.Capacity
                if mode == "restore" then
                    local s = ammoSnap[RT.addr(w)]
                    if s then setMags(p.pawn, w, s.mags); w.MagIndex = s.idx; ammoSnap[RT.addr(w)] = nil end
                    return
                end
                snapshot(w)
                if mode == "set" then cur.Ammo = math.max(0, math.min(cap, n))
                elseif mode == "empty" then cur.Ammo = 0
                elseif mode == "refill" then w:ReplenishAmmo()
                elseif mode == "nospares" then setMags(p.pawn, w, { { cur.Ammo, cur.AmmoType } })
                elseif mode == "lastbullet" then setMags(p.pawn, w, { { 1, cur.AmmoType } })
                elseif mode == "onemag" then setMags(p.pawn, w, { { cap, cur.AmmoType } }) end
            end)
            if ok then done = done + 1 end
        end
    end
    return string.format("%s on %d weapons", mode, done)
end }

-- ---------------------------------------------------------------- items
local function spawnItem(pawn, cls)
    local gs = UE.GetGameplayStatics()
    local l = pawn:K2_GetActorLocation()
    local xf = { Rotation = { X = 0.0, Y = 0.0, Z = 0.0, W = 1.0 }, Translation = { X = l.X, Y = l.Y, Z = l.Z },
                 Scale3D = { X = 1.0, Y = 1.0, Z = 1.0 } }
    local it = gs:BeginDeferredActorSpawnFromClass(pawn, cls, xf, 1, pawn, 0)
    gs:FinishSpawningActor(it, xf, 0)
    pawn.InventoryComp:AddInventoryItem(it)
    return it
end

-- pg_give: "<target>|classpath|count"
RT.action{ id = "pg_give", tab = "hidden", label = "Give item", run = function(ctx)
    local a = split(ctx.s.cmd_arg)
    local list = targets(a[1])
    local err = needHost(list)
    if err then return err end
    local cls = RT.cdo(a[2])
    if not cls then return "item class not loaded: " .. tostring(a[2]) end
    local count, n = math.max(1, math.min(10, tonumber(a[3]) or 1)), 0
    for _, p in ipairs(list) do
        for _ = 1, count do
            local ok, it = pcall(spawnItem, p.pawn, cls)
            if ok and RT.valid(it) then gifts[#gifts + 1] = { it = it, pawn = p.pawn }; n = n + 1 end
        end
    end
    pushUndo("give items", function()
        for _, g in ipairs(gifts) do if RT.valid(g.it) then pcall(function() g.pawn.InventoryComp:DestroyInventoryItem(g.it) end) end end
        gifts = {}
    end)
    return "gave " .. n .. " items"
end }

-- pg_take: "<target>|classpath|count"   removes items of that class (never the one in hand first)
RT.action{ id = "pg_take", tab = "hidden", label = "Take item", run = function(ctx)
    local a = split(ctx.s.cmd_arg)
    local list = targets(a[1])
    local err = needHost(list)
    if err then return err end
    local count, n = math.max(1, math.min(20, tonumber(a[3]) or 1)), 0
    for _, p in ipairs(list) do
        local inv = p.pawn.InventoryComp
        local held
        pcall(function() held = inv:GetEquippedItem() end)
        local victims = {}
        inv.InventoryItems:ForEach(function(_, e)
            local it = e:get()
            local ok, path = pcall(function() return it:GetClass():GetFullName():gsub("^%S+%s+", "") end)
            if ok and path == a[2] then
                if held and RT.valid(held) and RT.addr(held) == RT.addr(it) then table.insert(victims, it) else table.insert(victims, 1, it) end
            end
        end)
        for i = 1, math.min(count, #victims) do
            if pcall(function() inv:DestroyInventoryItem(victims[i]) end) then n = n + 1 end
        end
    end
    return "took " .. n .. " items"
end }

-- ---------------------------------------------------------------- world look (only on your screen)
local envSaved = { fog = {}, sun = {} }
local PRESETS = {
    clear   = { fog = 0.0005, fall = 0.2, col = { 0.6, 0.7, 0.8 }, sun = 1.0, sunCol = { 1, 1, 1 } },
    haze    = { fog = 0.02, fall = 0.2, col = { 0.8, 0.75, 0.65 }, sun = 0.8, sunCol = { 1, 0.95, 0.85 } },
    mist    = { fog = 0.08, fall = 0.05, col = { 0.7, 0.75, 0.8 }, sun = 0.5, sunCol = { 0.9, 0.95, 1 } },
    horror  = { fog = 0.06, fall = 0.1, col = { 0.35, 0.05, 0.05 }, sun = 0.25, sunCol = { 1, 0.3, 0.3 } },
    golden  = { fog = 0.01, fall = 0.2, col = { 1, 0.6, 0.3 }, sun = 1.6, sunCol = { 1, 0.7, 0.4 } },
    moon    = { fog = 0.015, fall = 0.2, col = { 0.2, 0.3, 0.6 }, sun = 0.3, sunCol = { 0.5, 0.6, 1 } },
    redalert= { fog = 0.03, fall = 0.15, col = { 1, 0.1, 0.1 }, sun = 0.6, sunCol = { 1, 0.15, 0.1 } },
}
local function envRestore()
    for _, e in pairs(envSaved.fog) do if RT.valid(e.c) then pcall(function() e.c:SetFogDensity(e.d); e.c:SetFogHeightFalloff(e.f); e.c:SetFogInscatteringColor(e.col) end) end end
    for _, e in pairs(envSaved.sun) do if RT.valid(e.c) then pcall(function() e.c:SetIntensity(e.i); e.c:SetLightColor(e.col, true) end) end end
    envSaved = { fog = {}, sun = {} }
end
RT.action{ id = "pg_env", tab = "hidden", label = "World look", run = function(ctx)
    local name = split(ctx.s.cmd_arg)[1]
    if name == "reset" then envRestore(); return "world look restored" end
    local P = PRESETS[name]
    if not P then return "unknown preset" end
    local nf, ns = 0, 0
    for _, c in ipairs(FindAllOf("ExponentialHeightFogComponent") or {}) do
        pcall(function()
            local k = RT.addr(c)
            if not envSaved.fog[k] then
                local col = c.FogInscatteringLuminance
                envSaved.fog[k] = { c = c, d = c.FogDensity, f = c.FogHeightFalloff, col = { R = col.R, G = col.G, B = col.B, A = col.A } }
            end
            c:SetFogDensity(P.fog); c:SetFogHeightFalloff(P.fall)
            c:SetFogInscatteringColor({ R = P.col[1], G = P.col[2], B = P.col[3], A = 1.0 })
            nf = nf + 1
        end)
    end
    for _, c in ipairs(FindAllOf("DirectionalLightComponent") or {}) do
        pcall(function()
            local k = RT.addr(c)
            if not envSaved.sun[k] then
                local col = c.LightColor
                envSaved.sun[k] = { c = c, i = c.Intensity, col = { R = col.R / 255, G = col.G / 255, B = col.B / 255, A = 1.0 } }
            end
            c:SetIntensity(envSaved.sun[k].i * P.sun)
            c:SetLightColor({ R = P.sunCol[1], G = P.sunCol[2], B = P.sunCol[3], A = 1.0 }, true)
            ns = ns + 1
        end)
    end
    return string.format("%s: %d fog, %d sun lights changed (only on your screen)", name, nf, ns)
end }

RT.action{ id = "pg_undo", tab = "hidden", label = "Undo last", run = function()
    local u = table.remove(undo)
    if not u then return "nothing to undo" end
    local ok, err = pcall(u.fn)
    return ok and ("undid: " .. u.label) or ("undo failed: " .. tostring(err))
end }
