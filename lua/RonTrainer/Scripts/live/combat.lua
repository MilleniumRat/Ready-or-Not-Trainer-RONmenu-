-- Made by MilleniumRat (github.com/MilleniumRat)
-- Combat helpers. Every game call below is in the class dump (ReadyOrNot.hpp / Engine.hpp):
--   melee reach        AReadyOrNotCharacter.MeleeRange
--   stun after a hit   UGameplayStatics.ApplyDamage with the game's UBeanbagDamageType (a UStunDamage)
--   auto reload        APlayerCharacter.Reload(), ABaseMagazineWeapon.Magazines / MagIndex / bReloading
--   sidearm switch     APlayerCharacter.EquipSecondaryItem() / EquipPrimaryItem()
--   penetration        ABaseMagazineWeapon.PenetrationDistance
--   friendly fire      AReadyOrNotCharacter.bNoTeamDamage
--   physics grab       UPrimitiveComponent.SetPhysicsLinearVelocity / AddImpulse
--   confirmed hits     health drop (UCharacterHealthComponent.Resource, Head.Health) right after
--                      your own shot (the OnFire hook in main.lua) along that shot's line
-- It also writes fx.txt for the overlay: confirmed hits, grenades in flight, the predicted
-- grenade arc, other weapons' gunshots and whether you are aiming down sights.
-- Untested in game when written: every status line says what really happened.
local UE = RT.UEHelpers
local FX_PATH = (RT.dir or ".") .. "\\fx.txt"
local CLIENT = "you are a client in someone else's game: only the host's game can do this"
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

local function now() return os.clock() end
local function v3(x, y, z) return { X = x, Y = y, Z = z } end
local function sub(a, b) return v3(a.X - b.X, a.Y - b.Y, a.Z - b.Z) end
local function len(a) return math.sqrt(a.X * a.X + a.Y * a.Y + a.Z * a.Z) end
local function dot(a, b) return a.X * b.X + a.Y * b.Y + a.Z * b.Z end
local function locOf(a)
    local ok, l = pcall(function() return a:K2_GetActorLocation() end)
    if ok and l then return v3(l.X, l.Y, l.Z) end
end
local function fwdOf(rot)   -- {Pitch, Yaw} in degrees -> unit vector
    local p, y = math.rad(rot.Pitch or 0), math.rad(rot.Yaw or 0)
    return v3(math.cos(p) * math.cos(y), math.cos(p) * math.sin(y), math.sin(p))
end
local function camera(pc)
    local ok, l, f = pcall(function()
        local cam = pc.PlayerCameraManager
        local r = cam:GetCameraRotation()
        local c = cam:GetCameraLocation()
        return v3(c.X, c.Y, c.Z), fwdOf({ Pitch = r.Pitch, Yaw = r.Yaw })
    end)
    if ok then return l, f end
end

local function held(pawn)
    if not pawn then return nil end
    local ok, it = pcall(function() return pawn.InventoryComp:GetEquippedItem() end)
    return ok and RT.valid(it) and it or nil
end
local function isMagWeapon(it)
    local ok, v = pcall(function() return it.MagIndex end)
    return ok and type(v) == "number"
end
local function aiming(it)
    local ok, v = pcall(function() return it.bIsAimingDownSights end)
    return ok and v == true
end
-- rounds in the inserted magazine, rounds in all the others
local function magInfo(w)
    local cur, spare = nil, 0
    pcall(function()
        local idx, i = w.MagIndex, 0
        w.Magazines:ForEach(function(_, e)
            local m = e:get()
            if i == idx then cur = m.Ammo else spare = spare + m.Ammo end
            i = i + 1
        end)
    end)
    return cur, spare
end
local function primaryOf(pawn)
    local found
    pcall(function()
        pawn.InventoryComp.InventoryItems:ForEach(function(_, e)
            local it = e:get()
            if not found and RT.valid(it) and isMagWeapon(it) and RT.className(it):find("^Primary") then found = it end
        end)
    end)
    return found
end
local function damageClass(path)
    local c
    pcall(function() c = StaticFindObject(path) end)
    return RT.valid(c) and c or nil
end

-- =========================================================================== confirmed hits
local lastShot = { t = -10 }      -- your own last shot: time, start, direction
local diag = { shotsSeen = 0, ownShots = 0, healthDrops = 0, notYours = 0 }   -- where the hit chain stops
local gunshots = {}               -- other weapons firing: { pos, t }
local boomQueue = {}              -- your shots waiting to explode where they land
local track = {}                  -- character addr -> last health reading
local hits, hitSeq = {}, 0        -- recent confirmed hits (for the overlay and the counters)
local handlers = {}               -- on-hit effects: function(ctx, hit)

RT.onShot = function(weapon, rot, l)
    diag.shotsSeen = diag.shotsSeen + 1
    local pawn = RT.getPawn()
    local own = false
    pcall(function() own = RT.addr(weapon:GetOwner()) == RT.addr(pawn) end)
    if own then
        diag.ownShots = diag.ownShots + 1
        lastShot = { t = now(), from = v3(l.X, l.Y, l.Z), dir = fwdOf(rot) }
        if RT.state and RT.state.lv_boom_hit == 1 then
            boomQueue[#boomQueue + 1] = lastShot
            if #boomQueue > 8 then table.remove(boomQueue, 1) end
        end
    else
        gunshots[#gunshots + 1] = { pos = locOf(weapon) or v3(l.X, l.Y, l.Z), t = now() }
        if #gunshots > 64 then table.remove(gunshots, 1) end
    end
end

-- was this health drop caused by you? "shot" when it lies on your last shot's line, "melee"
-- when nothing was fired and the character is in front of you within melee reach
local function attribute(ctx, pos, t)
    if lastShot.from and t - lastShot.t < 0.6 then
        local to = sub(pos, lastShot.from)
        local along = dot(to, lastShot.dir)
        if along > 0 then
            local perp = math.sqrt(math.max(0, dot(to, to) - along * along))
            if perp < 110 then return "shot" end
        end
        return nil
    end
    local me = locOf(ctx.pawn)
    local camLoc, camFwd = camera(ctx.pc)
    if me and camFwd then
        local to = sub(pos, me)
        local d = len(to)
        if d < 150 * 1.6 + 60 and d > 1 and dot(to, camFwd) / d > 0.6 then return "melee" end
    end
    return nil
end

local function scanHits(ctx)
    local t = now()
    local fresh = {}
    for _, cls in ipairs({ "SuspectCharacter", "CivilianCharacter" }) do
        for _, ch in ipairs(RT.cachedAll(cls, 0.5)) do
            local k = RT.addr(ch)
            local hp, head, dead
            pcall(function() local h = ch.CharacterHealth; hp = h.Resource; head = h.Head.Health end)
            pcall(function() dead = ch:IsDeadOrUnconscious() end)
            if hp then
                local p = track[k]
                if p and hp < p.hp - 0.01 then
                    diag.healthDrops = diag.healthDrops + 1
                    local pos = locOf(ch)
                    local kind = pos and attribute(ctx, pos, t)
                    if not kind then diag.notYours = diag.notYours + 1 end
                    if kind then
                        hitSeq = hitSeq + 1
                        local h = { id = hitSeq, ch = ch, pos = pos, dmg = p.hp - hp, kind = kind, t = t,
                                    head = head ~= nil and p.head ~= nil and head < p.head - 0.01,
                                    kill = dead == true and not p.dead, suspect = cls == "SuspectCharacter" }
                        hits[#hits + 1] = h
                        if #hits > 40 then table.remove(hits, 1) end
                        for _, fn in ipairs(handlers) do pcall(fn, ctx, h) end
                    end
                end
                fresh[k] = { hp = hp, head = head, dead = dead }
            end
        end
    end
    track = fresh
end

local function eligible(ctx, h, civKey)
    if h.kill then return false end
    if not h.suspect and ctx.s[civKey] ~= 1 then return false end
    return RT.valid(h.ch)
end

-- =========================================================================== WEAPON


RT.feature{ id = "auto_reload", tab = "Weapon", label = "Automatic reload",
    tip = "Calls the game's own Reload() when the inserted magazine is at or below this many rounds and you have a spare.",
    slider = { 0, 30, 0, "at %.0f rounds" },
    tick = (function()
        local last, count = -10, 0
        return function(ctx, val)
            local w = held(ctx.pawn)
            if not w or not isMagWeapon(w) then return "waiting: hold a gun" end
            local cur, spare = magInfo(w)
            if not cur then return "warn: could not read the magazine" end
            local thr = ctx.s.lv_auto_reload_empty == 1 and 0 or math.floor(val + 0.5)
            local reloading = false
            pcall(function() reloading = w.bReloading end)
            if reloading then return "ok: reloading" end
            if cur > thr then return string.format("ok: %d rounds, reloads at %d (%d reloads so far)", cur, thr, count) end
            if spare <= 0 then return "warn: no spare ammo left" end
            if ctx.s.lv_auto_reload_ads ~= 1 and aiming(w) then return "ok: waiting until you stop aiming" end
            if now() - last < 2 then return "ok: reload requested" end
            last = now()
            local ok = pcall(function() ctx.pawn:Reload() end)
            if not ok then return "none: the game's Reload() call failed" end
            count = count + 1
            return string.format("ok: reload started at %d rounds", cur)
        end
    end)() }
RT.feature{ id = "auto_reload_empty", tab = "Weapon", parent = "auto_reload", label = "Only when empty", tip = "", tick = function() return "ok" end }
RT.feature{ id = "auto_reload_ads", tab = "Weapon", parent = "auto_reload", label = "Also while aiming", tip = "Off: waits until you stop aiming down sights.", tick = function() return "ok" end }



-- on-hit effects (host / solo): the hit must be confirmed by the target's health dropping
local boomCount, boomAt = 0, -10
local boomImpacts, boomMiss = 0, 0
local boomFx, boomSnd




RT.feature{ id = "boom_hit", tab = "Weapon", label = "Explosive bullets",
    tip = "Every shot explodes where it lands (walls, floor, props and people): real radial damage around the impact (the game's ApplyRadialDamage). Hurts anyone in range except you.",
    slider = { 20, 500, 150, "%.0f damage" },
    tick = function()
        if not isServer() then return "none: " .. CLIENT end
        if diag.shotsSeen == 0 then return "warn: no shot seen yet: fire once (if this stays 0, the fire hook is not reporting)" end
        if diag.ownShots == 0 then return "warn: shots seen " .. diag.shotsSeen .. " but none recognised as yours" end
        local st = string.format("%d shots (yours %d), %d landed, %d explosions", diag.shotsSeen, diag.ownShots, boomImpacts, boomCount)
        if boomCount == 0 then return "warn: " .. st .. (boomMiss > 0 and ", " .. boomMiss .. " hit nothing" or "") end
        return "ok: " .. st
    end }
RT.feature{ id = "boom_fx", tab = "Weapon", parent = "boom_hit", label = "Blast effect (experimental)",
    tip = "Shows the game's explosion and sound at each impact. Experimental: this is the part suspected of crashing the game (several crashes followed it), so it is off by default.",
    tick = function() return "ok: blast effect on (crash risk)" end }
RT.feature{ id = "boom_radius", tab = "Weapon", parent = "boom_hit", label = "Radius", valueOnly = true, slider = { 100, 800, 300, "%.0f cm" } }
RT.feature{ id = "boom_cooldown", tab = "Weapon", parent = "boom_hit", label = "Cooldown", valueOnly = true, slider = { 0.2, 5, 1, "%.1f s" } }

-- Explosive bullets: every shot of yours explodes where it lands (wall, floor, prop or character),
-- found by tracing the shot's own line. The cooldown limits how often.
local function processBoom(ctx)
    local q = boomQueue
    boomQueue = {}
    local shot = q[#q]
    if not shot or not isServer() then return end
    local s = ctx.s
    if shot.t - boomAt < (s.lv_boom_cooldown_val or 1) then return end
    local ok, was, loc, nrm = pcall(function()
        local color, hit = { R = 0, G = 0, B = 0, A = 0 }, {}
        local fin = v3(shot.from.X + shot.dir.X * 100000, shot.from.Y + shot.dir.Y * 100000, shot.from.Z + shot.dir.Z * 100000)
        local w = UE.GetKismetSystemLibrary():LineTraceSingle(ctx.pawn, shot.from, fin, 0, false, {}, 0, hit, true, color, color, 0.0)
        if not w then return false end
        return true, v3(hit.Location.X, hit.Location.Y, hit.Location.Z), v3(hit.ImpactNormal.X, hit.ImpactNormal.Y, hit.ImpactNormal.Z)
    end)
    if not ok or not was then boomMiss = boomMiss + 1; return end
    boomAt = shot.t
    boomImpacts = boomImpacts + 1
    local at = v3(loc.X + nrm.X * 15, loc.Y + nrm.Y * 15, loc.Z + nrm.Z * 15)
    local fired = pcall(function()
        UE.GetGameplayStatics():ApplyRadialDamage(RT.world(), s.lv_boom_hit_val or 150, at, s.lv_boom_radius_val or 300,
            damageClass("/Script/Engine.DamageType"), { ctx.pawn }, ctx.pawn, ctx.pc, false, 0)
    end)
    if fired then boomCount = boomCount + 1 end
    -- the visible blast and its sound (the same assets as the Chaos "Explosion at crosshair").
    -- Off by default: loading these assets and spawning them on every shot is the suspected crash.
    if s.lv_boom_fx ~= 1 then return end
    pcall(function()
        boomFx = boomFx or LoadAsset("/Game/ReadyOrNot/VFX/VFXDetonation/P_Detonation_Trap_Explosive.P_Detonation_Trap_Explosive")
        if RT.valid(boomFx) then
            local k = (s.lv_boom_radius_val or 300) / 200
            local sc, rot = { X = k, Y = k, Z = k }, { Pitch = 0.0, Yaw = 0.0, Roll = 0.0 }
            if boomFx:GetClass():GetFName():ToString() == "NiagaraSystem" then
                RT.cdo("/Script/Niagara.Default__NiagaraFunctionLibrary"):SpawnSystemAtLocation(ctx.pawn, boomFx, at, rot, sc, true, true, 0, true)
            else
                UE.GetGameplayStatics():SpawnEmitterAtLocation(ctx.pawn, boomFx, at, rot, sc, true, 0, true)
            end
        end
    end)
    pcall(function()
        boomSnd = boomSnd or LoadAsset("/Game/FMOD/Events/Character/Gore/Gore_BombvestExplode.Gore_BombvestExplode")
        if RT.valid(boomSnd) then
            RT.cdo("/Script/FMODStudio.Default__FMODBlueprintStatics"):PlayEventAtLocation(ctx.pawn, boomSnd,
                { Rotation = { X = 0.0, Y = 0.0, Z = 0.0, W = 1.0 }, Translation = at, Scale3D = { X = 1.0, Y = 1.0, Z = 1.0 } }, true)
        end
    end)
end

RT.feature{ id = "full_auto", tab = "Weapon", label = "Full auto on every gun",
    tip = "Puts every gun you carry (pistols and shotguns too) in the game's own automatic fire mode (CurrentFireMode = Auto). Combine with Rapid fire for speed.",
    tick = function(ctx)
        local n = 0
        for _, w in ipairs(ctx.weapons or {}) do
            local ok = pcall(function()
                local has = false
                w.AvailableFireModes:ForEach(function(_, e) if e:get() == 1 then has = true end end)
                if not has then pcall(function() w.AvailableFireModes[#w.AvailableFireModes + 1] = 1 end) end
            end)
            if force("full_auto", w, "CurrentFireMode", 1) then n = n + 1 end
        end
        if n == 0 then return "waiting: no gun found" end
        return "ok: " .. n .. " guns set to automatic (untested whether single-shot guns keep firing while held)"
    end,
    off = function() restore("full_auto") end }

-- =========================================================================== PLAYER
-- The game fixes its lean at 15 degrees (about 13 cm of sideways view): QuickLeanAmount is rewritten
-- every frame, the animation values do not move the camera, and the intensity is ignored. So the extra
-- lean really moves your character sideways by the extra distance while you lean, with collision (a wall
-- stops it) and moves it back when you stop. View, weapon and aim move together.
local leanApplied = 0.0
local function leanShift(pawn, delta)
    local moved = 0.0
    pcall(function()
        local yaw = math.rad(pawn:K2_GetActorRotation().Yaw)
        local before = pawn:K2_GetActorLocation()
        pawn:K2_AddActorWorldOffset({ X = -math.sin(yaw) * delta, Y = math.cos(yaw) * delta, Z = 0.0 }, true, {}, false)
        local after = pawn:K2_GetActorLocation()
        moved = (after.X - before.X) * -math.sin(yaw) + (after.Y - before.Y) * math.cos(yaw)
    end)
    leanApplied = leanApplied + moved
end

-- the game's own settings object: what the options menu writes to GameUserSettings.ini

RT.feature{ id = "ff_protect", tab = "Health", label = "Friendly-fire protection",
    tip = "Players can't hurt each other: sets the game's own bNoTeamDamage on every player. Damage from suspects still hits.",
    tick = function()
        if not isServer() then return "none: " .. CLIENT end
        local n = 0
        for _, p in ipairs(RT.cachedAll("PlayerCharacter", 1.0)) do if force("ff_protect", p, "bNoTeamDamage", true) then n = n + 1 end end
        return string.format("ok: on %d players (untested whether it also covers your AI squad)", n)
    end,
    off = function() restore("ff_protect") end }

-- physics grab: hold a loose prop in front of you
local grab = {}
local function hitComponent(hit)
    local c
    pcall(function() c = hit.Component:Get() end)
    if not RT.valid(c) then pcall(function() c = hit.Component end) end
    return RT.valid(c) and c or nil
end
local function simulating(c)
    local ok, v = pcall(function() return c:IsSimulatingPhysics(FName("None")) end)
    return ok and v == true
end

-- =========================================================================== overlay feed (fx.txt)
local arcAt, arcLine, landLine = -10, nil, nil
local function grenadeArc(ctx)
    local it = held(ctx.pawn)
    local imp, up
    pcall(function() imp = it.ThrowImpulse; up = it.UpImpulse end)
    if not (imp and up) then arcLine, landLine = nil, nil; return "waiting: hold a grenade" end
    if now() - arcAt < 0.1 then return nil end
    arcAt = now()
    local start, f = camera(ctx.pc)
    if not start then return "warn: no camera" end
    -- launch model: velocity = forward * ThrowImpulse + up * UpImpulse (the grenade's own values)
    local v = v3(f.X * imp, f.Y * imp, f.Z * imp + up)
    local p = v3(start.X + f.X * 40, start.Y + f.Y * 40, start.Z + f.Z * 40)
    local g = -980
    local pts, land = { string.format("%.0f,%.0f,%.0f", p.X, p.Y, p.Z) }, nil
    local ksl = UE.GetKismetSystemLibrary()
    local c = { R = 0, G = 0, B = 0, A = 0 }
    local dt = 0.05
    for _ = 1, 60 do
        local q = v3(p.X + v.X * dt, p.Y + v.Y * dt, p.Z + v.Z * dt + 0.5 * g * dt * dt)
        v.Z = v.Z + g * dt
        local hit = {}
        local okT, blocked = pcall(function() return ksl:LineTraceSingle(ctx.pawn, p, q, 0, true, {}, 0, hit, true, c, c, 0.0) end)
        if okT and blocked then
            local ip
            pcall(function() ip = hit.ImpactPoint end)
            if not ip then pcall(function() ip = hit.Location end) end
            if ip then land = v3(ip.X, ip.Y, ip.Z); pts[#pts + 1] = string.format("%.0f,%.0f,%.0f", ip.X, ip.Y, ip.Z) end
            break
        end
        pts[#pts + 1] = string.format("%.0f,%.0f,%.0f", q.X, q.Y, q.Z)
        p = q
    end
    arcLine = "arc=" .. table.concat(pts, ";")
    landLine = land and string.format("land=%.0f,%.0f,%.0f", land.X, land.Y, land.Z) or nil
    return nil
end

local fxWritten = false
RT.onTick("combat", function(ctx)
    local s = ctx.s
    local t = now()
    local lines = {}
    local it = held(ctx.pawn)
    lines[#lines + 1] = "ads=" .. ((it and aiming(it)) and "1" or "0")
    local rel = false
    pcall(function() rel = it.bReloading == true end)
    lines[#lines + 1] = "rel=" .. (rel and "1" or "0")
    if wantHits then
        for _, h in ipairs(hits) do
            if t - h.t < 3 then
                lines[#lines + 1] = string.format("h=%d,%.0f,%.0f,%.0f,%.1f,%d,%d,%s,%.2f", h.id, h.pos.X, h.pos.Y, h.pos.Z,
                    h.dmg, h.head and 1 or 0, h.kill and 1 or 0, h.kind, t - h.t)
            end
        end
        RT.setStatus("hitmarkers", hitSeq > 0 and ("ok: " .. hitSeq .. " confirmed hits") or "waiting: hit a suspect (confirmed by its health dropping)")
    end
    if wantSounds then
        local keep = s.sound_markers_val or 3
        local out = {}
        for _, g in ipairs(gunshots) do
            if t - g.t < keep then
                out[#out + 1] = g
                lines[#lines + 1] = string.format("snd=%.0f,%.0f,%.0f,%.2f", g.pos.X, g.pos.Y, g.pos.Z, t - g.t)
            end
        end
        gunshots = out
        RT.setStatus("sound_markers", "ok: listening to other weapons firing (" .. #out .. " shown)")
    end
    if s.grenade_arc == 1 then
        local st = grenadeArc(ctx)
        if arcLine then lines[#lines + 1] = arcLine end
        if landLine and s.arc_landing == 1 then lines[#lines + 1] = landLine end
        RT.setStatus("grenade_arc", st or (arcLine and "ok: arc from the grenade's ThrowImpulse / UpImpulse (no bounces)" or "waiting: hold a grenade"))
    end
    if s.esp == 1 and s.esp_projectiles == 1 then
        local n = 0
        for _, g in ipairs(RT.cachedAll("BaseGrenade", 0.25)) do
            local parent
            pcall(function() parent = g:GetAttachParentActor() end)
            local hidden = false
            pcall(function() hidden = g.bHidden end)
            local p = locOf(g)
            if p and not RT.valid(parent) and not hidden then
                n = n + 1
                lines[#lines + 1] = string.format("g=%.0f,%.0f,%.0f,%s", p.X, p.Y, p.Z, (RT.className(g):gsub("^BP_", ""):gsub("_C$", "")))
            end
        end
        RT.setStatus("esp_projectiles", "ok: " .. n .. " grenades in the world (fuse time is not readable)")
    end
    local f = io.open(FX_PATH .. ".tmp", "w")
    if f then
        f:write(table.concat(lines, "\n"), "\n")
        f:close()
        os.remove(FX_PATH)
        os.rename(FX_PATH .. ".tmp", FX_PATH)
        fxWritten = true
    end
end)

-- =========================================================================== cosmetic body yaw
-- Turns only the third-person body (APlayerCharacter.Mesh, hidden from your own first-person
-- view). Your camera, first-person arms, aim and shots are untouched. Local only: other players'
-- games draw your body from their own copy, so they do not see it.
local body = { base = nil, mesh = nil, angle = 0, jitter = 0, nextJitter = 0, lastT = nil }
local function bodyMesh(pawn)
    local ok, m = pcall(function() return pawn.Mesh end)
    return ok and RT.valid(m) and m or nil
end
local function setBodyYaw(m, yaw)
    pcall(function() m:K2_SetRelativeRotation({ Pitch = 0.0, Yaw = yaw, Roll = 0.0 }, false, {}, true) end)
end
local function bodyRestore()
    if body.mesh and RT.valid(body.mesh) and body.base then setBodyYaw(body.mesh, body.base) end
    body.mesh, body.meshAddr, body.base, body.lastT = nil, nil, nil, nil
end
local function bodyTick(ctx, extra)
    local m = bodyMesh(ctx.pawn)
    if not m then return "waiting: no character" end
    if body.meshAddr ~= RT.addr(m) then   -- wrappers differ per call: compare the real object
        bodyRestore()
        body.mesh, body.meshAddr = m, RT.addr(m)
        local ok, r = pcall(function() return m.RelativeRotation.Yaw end)
        body.base = ok and r or -90.0
    end
    setBodyYaw(m, body.base + extra)
    return string.format("ok: body turned %.0f deg (local only; see it in third person)", extra % 360)
end

RT.feature{ id = "spinbot", tab = "Camera", label = "Spinbot (body only)",
    tip = "Spins your third-person body. Your view, arms, aim and shots stay normal. Only you see it (in third person).",
    slider = { 30, 1440, 360, "%.0f deg/s" },
    tick = function(ctx, val)
        local t = now()
        local dt = body.lastT and math.min(0.2, t - body.lastT) or 0
        body.lastT = t
        local dir = ctx.s.lv_spin_ccw == 1 and -1 or 1
        body.angle = (body.angle + dir * val * dt) % 360
        local a = body.angle
        if ctx.s.lv_spin_stepped == 1 then
            local step = math.max(5, ctx.s.lv_spin_step_val or 45)
            a = math.floor(a / step) * step
        end
        return bodyTick(ctx, a)
    end,
    off = function() bodyRestore() end }
RT.feature{ id = "spin_ccw", tab = "Camera", parent = "spinbot", label = "Counter-clockwise", tip = "", tick = function() return "ok" end }
RT.feature{ id = "spin_stepped", tab = "Camera", parent = "spinbot", label = "Stepped", tip = "", tick = function() return "ok" end }
RT.feature{ id = "spin_step", tab = "Camera", parent = "spinbot", label = "Step size", valueOnly = true, slider = { 5, 180, 45, "%.0f deg" } }

