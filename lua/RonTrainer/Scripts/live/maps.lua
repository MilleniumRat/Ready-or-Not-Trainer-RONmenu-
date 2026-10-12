-- Made by MilleniumRat (github.com/MilleniumRat)
-- Start a map directly with the game's own ServerTravel (Barricaded Suspects mode).
local MAPS = {
    { id = "map_dorms",    label = "Start Dorms",    path = "/Game/ReadyOrNot/Level/DLC_01_Home/Dorms/Level/RoN_Dorms_Core" },
    { id = "map_lawmaker", label = "Start Lawmaker", path = "/Game/ReadyOrNot/Level/DLC_01_Home/Lawmaker/Levels/RoN_Lawmaker_Core" },
    { id = "map_narcos",   label = "Start Narcos",   path = "/Game/ReadyOrNot/Level/DLC_01_Home/Narcos/Levels/RoN_Narcos_Core" },
}
for _, m in ipairs(MAPS) do
    RT.action{ id = m.id, tab = "Mission", label = m.label, tip = "Loads " .. m.label:sub(7) .. " (Barricaded Suspects).",
        run = function()
            local lib = RT.cdo("/Script/ReadyOrNot.Default__ReadyOrNotFunctionLibrary")
            if not lib then return "travel function not found" end
            lib:ServerTravel(m.path .. "?Mode=CM_BarricadedSuspects")
            return "loading " .. m.label:sub(7) .. "..."
        end }
end
