-- Made by MilleniumRat (github.com/MilleniumRat)
-- Experimental playground features. No chat messages are sent or read (removed on request).
--   giant / tiny, big head: cosmetic, only on your screen
--   moon gravity, force crouch, force lean, clear chamber: the host's game changes the player's character
local UE = RT.UEHelpers
local applied = {}         -- [pawn addr .. kind] = restore fn
local holds = {}           -- crouch/lean/gravity holds: key -> { untilT, fn, stop }

local function split(s)
    local t = {}
    for part in (tostring(s or "") .. "|"):gmatch("([^|]*)|") do t[#t + 1] = part end
    return t
end

local function isServer()
    local ok, v = pcall(function() return UE.GetKismetSystemLibrary():IsServer(RT.world()) end)
    return ok and v == true
end

local function players()
    local out, me = {}, RT.getPawn()
    local gs = FindFirstOf("GameStateBase")
    if not RT.valid(gs) then return out end
    pcall(function()
        gs.PlayerArray:ForEach(function(_, e)
            local ps = e:get()
            local pawn, id = nil, 0
            pcall(function() pawn = ps:GetPawn() end)
            pcall(function() id = ps.PlayerId end)
            if RT.valid(pawn) then out[#out + 1] = { pawn = pawn, id = id, me = me ~= nil and RT.addr(pawn) == RT.addr(me) } end
        end)
    end)
    return out
end

local function targets(t)
    local out = {}
    for _, p in ipairs(players()) do
        if t == "all" or (t == "self" and p.me) or tostring(p.id) == tostring(t) then out[#out + 1] = p end
    end
    return out
end

local function restoreKind(pawn, kind)
    local k = RT.addr(pawn) .. kind
    if applied[k] then pcall(applied[k]); applied[k] = nil end
end

local function scalePawn(pawn, s)
    restoreKind(pawn, "scale")
    local mesh = pawn.Mesh
    local o = mesh.RelativeScale3D
    local orig = { X = o.X, Y = o.Y, Z = o.Z }
    mesh:SetRelativeScale3D({ X = orig.X * s, Y = orig.Y * s, Z = orig.Z * s })
    applied[RT.addr(pawn) .. "scale"] = function() if RT.valid(mesh) then mesh:SetRelativeScale3D(orig) end end
end

local HEAD_ITEMS = { "^Helmet", "HelmetMount", "Facewear", "Headwear", "Hat", "Mask", "NVG" }
local function bigHead(pawn, s)
    restoreKind(pawn, "head")
    local list = {}
    pcall(function()
        pawn.InventoryComp.InventoryItems:ForEach(function(_, e)
            local it = e:get()
            local n = RT.className(it)
            for _, pat in ipairs(HEAD_ITEMS) do
                if n:find(pat) then
                    local root = it.RootComponent
                    local o = root.RelativeScale3D
                    local orig = { X = o.X, Y = o.Y, Z = o.Z }
                    list[#list + 1] = { c = root, o = orig }
                    root:SetRelativeScale3D({ X = orig.X * s, Y = orig.Y * s, Z = orig.Z * s })
                    break
                end
            end
        end)
    end)
    applied[RT.addr(pawn) .. "head"] = function() for _, e in ipairs(list) do if RT.valid(e.c) then e.c:SetRelativeScale3D(e.o) end end end
    return #list
end

RT.onTick("pgx_holds", function()
    local now = os.clock()
    for k, h in pairs(holds) do
        if now >= h.untilT then pcall(h.stop or function() end); holds[k] = nil
        elseif h.fn then pcall(h.fn) end
    end
end)

-- giant / tiny (your screen): "<target>|scale"
RT.action{ id = "pgx_scale", tab = "hidden", label = "Giant / tiny", run = function(ctx)
    local a = split(ctx.s.cmd_arg)
    local s = math.max(0.2, math.min(4, tonumber(a[2]) or 1))
    local n = 0
    for _, p in ipairs(targets(a[1])) do
        if s == 1 then restoreKind(p.pawn, "scale") else pcall(scalePawn, p.pawn, s) end
        n = n + 1
    end
    return string.format("size x%.2f on %d players (only on your screen)", s, n)
end }

-- big head (your screen): scales head gear around the head: "<target>|scale"
RT.action{ id = "pgx_head", tab = "hidden", label = "Big head", run = function(ctx)
    local a = split(ctx.s.cmd_arg)
    local s = math.max(0.3, math.min(5, tonumber(a[2]) or 1))
    local n = 0
    for _, p in ipairs(targets(a[1])) do
        if s == 1 then restoreKind(p.pawn, "head") else local ok, c = pcall(bigHead, p.pawn, s); if ok then n = n + c end end
    end
    return s == 1 and "heads back to normal" or (n .. " head items scaled x" .. s .. " (only on your screen)")
end }

-- moon gravity (host): "<target>|scale|seconds"
RT.action{ id = "pgx_grav", tab = "hidden", label = "Moon gravity", run = function(ctx)
    local a = split(ctx.s.cmd_arg)
    local g, secs = math.max(0.05, math.min(2, tonumber(a[2]) or 0.3)), math.max(1, math.min(120, tonumber(a[3]) or 15))
    local host = isServer()
    local n = 0
    for _, p in ipairs(targets(a[1])) do
        if host or p.me then
            pcall(function()
                local mv = p.pawn.CharacterMovement
                local k = "grav" .. RT.addr(p.pawn)
                local old = holds[k] and holds[k].old or mv.GravityScale
                mv.GravityScale = g
                holds[k] = { untilT = os.clock() + secs, old = old, stop = function() if RT.valid(mv) then mv.GravityScale = old end end }
                n = n + 1
            end)
        end
    end
    if n == 0 then return "you are a client: only the host can change other players' gravity" end
    return string.format("gravity x%.2f for %d s on %d players (friends' own screens may snap back)", g, secs, n)
end }

-- forced crouch (host): "<target>|1/0|seconds"
RT.action{ id = "pgx_crouch", tab = "hidden", label = "Force crouch", run = function(ctx)
    local a = split(ctx.s.cmd_arg)
    if not isServer() then return "you are a client: only the host can crouch other players" end
    local on, secs = a[2] == "1", math.max(1, math.min(60, tonumber(a[3]) or 5))
    local n = 0
    for _, p in ipairs(targets(a[1])) do
        local pawn = p.pawn
        if on then
            holds["crouch" .. RT.addr(pawn)] = { untilT = os.clock() + secs, fn = function() pawn:Crouch(false) end,
                                                 stop = function() if RT.valid(pawn) then pawn:UnCrouch(false) end end }
        else
            holds["crouch" .. RT.addr(pawn)] = nil
            pcall(function() pawn:UnCrouch(false) end)
        end
        n = n + 1
    end
    return (on and "holding crouch on " or "released ") .. n .. " players" .. (on and (" for " .. secs .. " s") or "")
end }

-- forced lean (host): "<target>|left/right/none|amount|seconds"
RT.action{ id = "pgx_lean", tab = "hidden", label = "Force lean", run = function(ctx)
    local a = split(ctx.s.cmd_arg)
    if not isServer() then return "you are a client: only the host can lean other players" end
    local dir = a[2] == "left" and -1 or a[2] == "right" and 1 or 0
    local amt = math.max(0, math.min(1, tonumber(a[3]) or 1)) * dir
    local secs = math.max(1, math.min(60, tonumber(a[4]) or 5))
    local n = 0
    for _, p in ipairs(targets(a[1])) do
        local pawn = p.pawn
        local k = "lean" .. RT.addr(pawn)
        if dir == 0 then
            holds[k] = nil
            pcall(function() pawn:Server_UpdateLean(0.0, 0.0, 0.0) end)
        else
            holds[k] = { untilT = os.clock() + secs, fn = function() pawn:Server_UpdateLean(amt, 0.0, 0.0) end,
                         stop = function() if RT.valid(pawn) then pawn:Server_UpdateLean(0.0, 0.0, 0.0) end end }
        end
        n = n + 1
    end
    return dir == 0 and ("released lean on " .. n) or ("leaning " .. n .. " players " .. a[2] .. " for " .. secs .. " s")
end }

-- chamber (host): trims the inserted magazine back to capacity if it is above
RT.action{ id = "pgx_chamber", tab = "hidden", label = "Clear chamber", run = function(ctx)
    local a = split(ctx.s.cmd_arg)
    if not isServer() then return "you are a client: only the host can change ammo" end
    local n, trimmed = 0, 0
    for _, p in ipairs(targets(a[1])) do
        pcall(function()
            local w = p.pawn.InventoryComp:GetEquippedItem()
            local m = w.Magazines[w.MagIndex + 1]
            if m.Ammo > w.Capacity then m.Ammo = w.Capacity; trimmed = trimmed + 1 end
            n = n + 1
        end)
    end
    return string.format("checked %d weapons, %d had a round above capacity (the game normally keeps none)", n, trimmed)
end }

RT.action{ id = "pgx_reset", tab = "hidden", label = "Reset experimental", run = function()
    for k, f in pairs(applied) do pcall(f); applied[k] = nil end
    for k, h in pairs(holds) do pcall(h.stop or function() end); holds[k] = nil end
    return "all experimental effects reset"
end }
