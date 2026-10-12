-- Made by MilleniumRat (github.com/MilleniumRat)
-- Backend for the menu's FUN window. Commands come in as hidden live buttons with an argument
-- (ctx.s.cmd_arg); toggles are live features in the hidden "Fun" section.
local UE = RT.UEHelpers
local spawned = {}      -- effect components we started (for "clear")
local animated = {}     -- meshes we put into single-animation mode
local lights = {}       -- party lights

local function split(s)
    local t = {}
    for part in (tostring(s or "") .. "|"):gmatch("([^|]*)|") do t[#t + 1] = part end
    return t
end

local function crosshairPoint(ctx)
    local hit = ctx.pawn and ctx.pc and RT.traceCrosshair(ctx.pawn, ctx.pc)
    local p = hit and (hit.ImpactPoint or hit.Location)
    if p then return { X = p.X, Y = p.Y, Z = p.Z } end
    return nil
end

local function characters(which)
    local out = {}
    local cls = which == "civilians" and { "CivilianCharacter" } or which == "suspects" and { "SuspectCharacter" }
        or { "SuspectCharacter", "CivilianCharacter" }
    for _, c in ipairs(cls) do
        for _, ch in ipairs(FindAllOf(c) or {}) do if RT.valid(ch) then out[#out + 1] = ch end end
    end
    return out
end

local function spawnFx(ctx, asset, loc, scale)
    local cls = asset:GetClass():GetFName():ToString()
    local s = { X = scale, Y = scale, Z = scale }
    local r = { Pitch = 0.0, Yaw = 0.0, Roll = 0.0 }
    local comp
    if cls == "NiagaraSystem" then
        local lib = RT.cdo("/Script/Niagara.Default__NiagaraFunctionLibrary")
        comp = lib:SpawnSystemAtLocation(ctx.pawn, asset, loc, r, s, true, true, 0, true)
    else
        comp = UE.GetGameplayStatics():SpawnEmitterAtLocation(ctx.pawn, asset, loc, r, s, true, 0, true)
    end
    if RT.valid(comp) then spawned[#spawned + 1] = comp end
    return comp
end

-- fun_fx: "path|where|scale"   where = crosshair / me / suspects / everyone
RT.action{ id = "fun_fx", tab = "hidden", label = "Spawn effect", run = function(ctx)
    local a = split(ctx.s.cmd_arg)
    local asset = LoadAsset(a[1])
    if not RT.valid(asset) then return "could not load " .. tostring(a[1]) end
    local scale = tonumber(a[3]) or 1.0
    local where = a[2] ~= "" and a[2] or "crosshair"
    local spots = {}
    if where == "crosshair" then spots[1] = crosshairPoint(ctx)
    elseif where == "me" then local l = ctx.pawn:K2_GetActorLocation(); spots[1] = { X = l.X, Y = l.Y, Z = l.Z }
    else for _, ch in ipairs(characters(where)) do local l = ch:K2_GetActorLocation(); spots[#spots + 1] = { X = l.X, Y = l.Y, Z = l.Z } end end
    local n = 0
    for _, p in ipairs(spots) do if p and RT.valid(spawnFx(ctx, asset, p, scale)) then n = n + 1 end end
    return n > 0 and ("spawned " .. a[1]:match("([^/.]+)$") .. " x" .. n) or "nothing to spawn it on"
end }

RT.action{ id = "fun_fx_clear", tab = "hidden", label = "Clear effects", run = function()
    local n = 0
    for _, c in ipairs(spawned) do if RT.valid(c) then pcall(function() c:K2_DestroyComponent(c) end); n = n + 1 end end
    spawned = {}
    return "removed " .. n .. " effects"
end }

-- fun_snd: "path|where|volume"   where = me / crosshair, volume 0..1. Played through an audio component so it can be stopped.
local sounds = {}
RT.action{ id = "fun_snd", tab = "hidden", label = "Play sound", run = function(ctx)
    local a = split(ctx.s.cmd_arg)
    local ev = LoadAsset(a[1])
    if not RT.valid(ev) then return "could not load " .. tostring(a[1]) end
    if not ctx.pawn then return "load into a level first" end
    local st = RT.cdo("/Script/FMODStudio.Default__FMODBlueprintStatics")
    local p = a[2] == "crosshair" and crosshairPoint(ctx)
    local c
    if p then c = st:PlayEventAttached(ev, ctx.pawn.RootComponent, FName("None"), p, 1, true, true, true)   -- keep world position
    else c = st:PlayEventAttached(ev, ctx.pawn.RootComponent, FName("None"), { X = 0.0, Y = 0.0, Z = 0.0 }, 0, true, true, true) end
    if RT.valid(c) then
        sounds[#sounds + 1] = c
        local vol = tonumber(a[3])
        if vol then pcall(function() c:SetVolume(vol) end) end   -- UFMODAudioComponent.SetVolume
    end
    return "playing " .. a[1]:match("([^/.]+)$")
end }

RT.action{ id = "fun_snd_stop", tab = "hidden", label = "Stop sounds", run = function()
    local n = 0
    for _, c in ipairs(sounds) do if RT.valid(c) then pcall(function() c:Stop(); c:K2_DestroyComponent(c) end); n = n + 1 end end
    sounds = {}
    return "stopped " .. n .. " sounds"
end }

-- fun_anim: "path|who"   who = suspects / civilians / everyone
RT.action{ id = "fun_anim", tab = "hidden", label = "Animate NPCs", run = function(ctx)
    local a = split(ctx.s.cmd_arg)
    local anim = LoadAsset(a[1])
    if not RT.valid(anim) then return "could not load " .. tostring(a[1]) end
    local n = 0
    for _, ch in ipairs(characters(a[2])) do
        pcall(function()
            local m = ch.Mesh
            m:PlayAnimation(anim, true)
            animated[RT.addr(m)] = m
            n = n + 1
        end)
    end
    return "animated " .. n .. " characters"
end }

RT.action{ id = "fun_anim_stop", tab = "hidden", label = "Stop NPC animations", run = function()
    local n = 0
    for _, m in pairs(animated) do if RT.valid(m) then pcall(function() m:SetAnimationMode(0, true) end); n = n + 1 end end
    animated = {}
    return "restored " .. n .. " characters"
end }

RT.action{ id = "fun_launch", tab = "Fun", label = "Launch everyone", tip = "Throws every suspect and civilian into the air.",
    run = function()
        local n = 0
        for _, ch in ipairs(characters("everyone")) do
            if pcall(function() ch:LaunchCharacter({ X = math.random(-300, 300) + 0.0, Y = math.random(-300, 300) + 0.0, Z = 1400.0 }, true, true) end) then n = n + 1 end
        end
        return "launched " .. n
    end }

RT.action{ id = "fun_boom", tab = "Fun", label = "Explosion at crosshair", tip = "Explosion effect and sound where you aim; pushes nearby characters away (no damage).",
    run = function(ctx)
        local p = crosshairPoint(ctx)
        if not p then return "aim at something" end
        local fx = LoadAsset("/Game/ReadyOrNot/VFX/VFXDetonation/P_Detonation_Trap_Explosive.P_Detonation_Trap_Explosive")
        if RT.valid(fx) then spawnFx(ctx, fx, p, 1.5) end
        local ev = LoadAsset("/Game/FMOD/Events/Character/Gore/Gore_BombvestExplode.Gore_BombvestExplode")
        if RT.valid(ev) then
            RT.cdo("/Script/FMODStudio.Default__FMODBlueprintStatics"):PlayEventAtLocation(ctx.pawn, ev,
                { Rotation = { X = 0.0, Y = 0.0, Z = 0.0, W = 1.0 }, Translation = p, Scale3D = { X = 1.0, Y = 1.0, Z = 1.0 } }, true)
        end
        local n = 0
        for _, ch in ipairs(characters("everyone")) do
            pcall(function()
                local l = ch:K2_GetActorLocation()
                local dx, dy = l.X - p.X, l.Y - p.Y
                local d = math.sqrt(dx * dx + dy * dy)
                if d < 600 then
                    local k = (600 - d) / 600 * 1500 / math.max(d, 1)
                    ch:LaunchCharacter({ X = dx * k, Y = dy * k, Z = 900.0 }, true, true)
                    n = n + 1
                end
            end)
        end
        return "boom (" .. n .. " characters thrown)"
    end }

local gravSaved = nil

local aiGrav = {}

local hue = 0
local function hsv(h)
    local i = math.floor(h * 6) % 6
    local f = h * 6 - math.floor(h * 6)
    local q, t = 1 - f, f
    local r, g, b = ({ { 1, t, 0 }, { q, 1, 0 }, { 0, 1, t }, { 0, q, 1 }, { t, 0, 1 }, { 1, 0, q } })[i + 1][1],
        ({ { 1, t, 0 }, { q, 1, 0 }, { 0, 1, t }, { 0, q, 1 }, { t, 0, 1 }, { 1, 0, q } })[i + 1][2],
        ({ { 1, t, 0 }, { q, 1, 0 }, { 0, 1, t }, { 0, q, 1 }, { t, 0, 1 }, { 1, 0, q } })[i + 1][3]
    return { R = r, G = g, B = b, A = 1.0 }
end
RT.feature{ id = "fun_party", tab = "Fun", label = "Party light", tip = "A colour-cycling light follows you. The amount is the speed.",
    slider = { 0.1, 3.0, 1.0, "x%.1f" },
    tick = function(ctx, val)
        if not ctx.pawn then return "waiting: no player" end
        local L = lights[1]
        if not RT.valid(L) then
            local gs = UE.GetGameplayStatics()
            local l = ctx.pawn:K2_GetActorLocation()
            local xf = { Rotation = { X = 0.0, Y = 0.0, Z = 0.0, W = 1.0 }, Translation = { X = l.X, Y = l.Y, Z = l.Z + 120 },
                         Scale3D = { X = 1.0, Y = 1.0, Z = 1.0 } }
            L = gs:BeginDeferredActorSpawnFromClass(ctx.pawn, RT.cdo("/Script/Engine.PointLight"), xf, 1, ctx.pawn, 0)
            gs:FinishSpawningActor(L, xf, 0)
            pcall(function() L:K2_AttachToActor(ctx.pawn, FName("None"), 1, 1, 1, false) end)
            pcall(function() L:K2_SetActorRelativeLocation({ X = 0.0, Y = 0.0, Z = 120.0 }, false, {}, true) end)
            pcall(function() L.PointLightComponent:SetIntensity(8000.0); L.PointLightComponent:SetAttenuationRadius(1500.0) end)
            lights[1] = L
        end
        hue = (hue + 0.01 * (val or 1.0)) % 1.0
        pcall(function() L.PointLightComponent:SetLightColor(hsv(hue), true) end)
        return "ok: party on"
    end,
    off = function()
        for _, L in ipairs(lights) do if RT.valid(L) then pcall(function() L:K2_DestroyActor() end) end end
        lights = {}
    end }
