-- Made by MilleniumRat (github.com/MilleniumRat)
-- Extra player / visual features (verified against BasePlayer_C properties in the station).
-- Every forced value is remembered and put back when the feature is switched off.
local saved = {}   -- [feature] = { {obj, name, value}, ... }

local function force(feature, obj, name, value)
    saved[feature] = saved[feature] or {}
    local key = RT.addr(obj) .. name
    local list = saved[feature]
    if not list[key] then
        local ok, v = pcall(function() return obj[name] end)
        if not ok or v == nil then return false end
        list[key] = { obj = obj, name = name, value = v }
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
    for _, e in pairs(saved[feature] or {}) do
        if RT.valid(e.obj) then pcall(function() e.obj[e.name] = e.value end) end
    end
    saved[feature] = nil
end


local stunsRemoved = 0

local bleedsStopped = 0




print("extras: 6 features")
