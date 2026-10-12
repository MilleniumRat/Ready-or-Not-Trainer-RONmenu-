// Made by MilleniumRat (github.com/MilleniumRat)
// Menu UI, key binds, saved config, and the state file that talks to the Lua side (UE4SS mod).
//
//   menu_state.txt  (DLL -> Lua)   key=value lines, rewritten whenever something changes
//   status.txt      (Lua -> DLL)   per-feature status + heartbeat + last command result, ~1/s
//   anims.txt       (Lua -> DLL)   animation search results
//   ronmenu.cfg     (DLL)          everything you set, restored on the next inject
#include "common.h"
#include "imgui.h"
#include "stretch.h"

#include <algorithm>
#include <atomic>
#include <thread>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <deque>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------------------------
// Features
// ---------------------------------------------------------------------------------------------
struct Entry {
    const char* id;
    const char* label;
    const char* tip;
    unsigned    bind;        // VK code, 0 = none
    bool        noCheck;     // value only, no on/off toggle
    bool        hasSlider;
    float       lo, hi, def;
    const char* fmt;
    bool        on = false;
    float       val = 0.f;
    bool        noStatus = false;  // plain option, the Lua side reports nothing for it
    const char* combo = nullptr;   // method choices, "\0"-separated (written as <id>_mode)
    int         mode = 0;
    const char* parent = nullptr;  // shown inside this feature's set-up (id, "@anim" or "@actions")
};

struct ModeInfo { const char* id; const char* items; std::vector<const char*> help; };

// Selectable methods / options. Indices must match the *_mode handling in the Lua mod.
const std::vector<ModeInfo> kModes = {
    { "speed",
      "Auto\0Run speed\0Time scale\0Hook game speed calls\0Engine walk speed\0",
      { "Tries Run speed first and checks it worked; otherwise switches to Time scale.",
        "Scales the base speed the game calculates your walk speed from. Animations stay normal.",
        "Speeds up your whole character. Always works, but animations (reload, aim) get faster too.",
        "Rewrites the speed value whenever the game sets it. Only works if the game calls its speed functions (see dot).",
        "Forces the engine's walk speed every tick. The game may keep overwriting it." } },
    { "fov",
      "Match camera FOV (auto)\0Base FOV\0Game setting\0",
      { "Adjusts the game's base FOV until your real camera FOV equals the slider. Aim zoom and sprint still work.",
        "Sets the game's base FOV to the slider. The game scales it, so the camera ends up a bit wider than the number.",
        "Sets the FOV value of the game's options menu. It may be clamped, and it is saved in your settings." } },
    { "fast_arrest",
      "Timer + animation\0Timer only\0Animation only\0Instant\0",
      { "Shorter restrain timer and faster arrest animations (you, the cuffs and the person cuffed).",
        "Only shortens the zip-cuff timer.",
        "Only speeds up the arrest animations.",
        "Completes the arrest the moment it starts (may look abrupt)." } },
    { "end_grade", "S\0A\0B\0C\0D\0F\0", {} },
    { "third_person",
      "Auto\0Move camera\0Switch cameras\0Game toggle (dev)\0Free orbit (dev)\0Follow camera\0Engine free cam\0",
      { "Tries Follow camera, Move camera, Engine free cam, Switch cameras and the game's toggle, and keeps the first one that really moves the view. If none works, the dot lists why each one failed.",
        "Moves the game's own first-person camera onto the arm behind your character. Works without any developer function.",
        "Turns the first-person camera off and the game's third-person camera on.",
        "The game's developer third-person toggle (may be removed in the release build).",
        "The game's developer free-orbit camera (may be removed in the release build).",
        "Puts a separate camera behind you and looks through it, the way spectator cameras work. Does not touch the game's own cameras.",
        "The engine's built-in free camera mode (orbits behind you, follows where you look)." } },
    { "anim_method",
      "Full-body pose\0Blend in slot\0",
      { "Plays the animation directly on your body, bypassing the game's animation system. Always shows; walking and aiming animations pause until you stop it.",
        "Plays it on top of the normal animations (montage or slot). Needs the right slot below and only works if the game's animation setup supports it." } },
    { "aim_key", "While aiming (right mouse)\0While shooting (left mouse)\0Mouse 4\0Mouse 5\0Shift\0Alt\0Always\0",
      { "Turns onto the target only while you hold right mouse (aim down sights).",
        "Turns onto the target only while you hold the fire button.",
        "Only while you hold mouse button 4.",
        "Only while you hold mouse button 5.",
        "Only while you hold Shift.",
        "Only while you hold Alt.",
        "Whenever a target is inside the circle. Strong; you lose control of the view near targets." } },
    { "aim_bone", "Head\0Neck\0Chest\0Pelvis\0Nearest to crosshair\0",
      { "Aims at the head.", "Aims at the neck.", "Aims at the chest (largest target, most armour).",
        "Aims at the pelvis (below most armour).", "Whichever of head, neck, chest and pelvis is closest to your crosshair." } },
    { "aim_targets", "Suspects\0Civilians\0Suspects and civilians\0",
      { "Only suspects.", "Only civilians.", "Anyone." } },
    { "aim_priority", "Closest to crosshair\0Closest to you\0Lowest health\0",
      { "Picks the target nearest the middle of your screen.", "Picks the nearest target in the circle.",
        "Picks the most injured target in the circle." } },
    { "esp_style", "2D box\0Corner box\0Rounded box\0" "3D box\0Skeleton\0Skeleton + box\0",
      { "Flat box around each character.", "Only the four corners.", "Box with rounded corners.", "Box that turns with the character.",
        "Bones only (falls back to a box when bones are missing).", "Bones with a faint box." } },
    { "head_marker", "None\0Circle\0Dot\0Square\0", { "", "", "", "" } },
    { "hp_pos", "Left\0Right\0Above\0Below\0", { "", "", "", "" } },
    { "lbl_pos", "Above\0Below\0", { "", "" } },
    { "lbl_units", "Metres\0Feet\0", { "", "" } },
    { "line_origin", "Bottom\0Crosshair\0Top\0", { "", "", "" } },
    { "radar_shape", "Circle\0Square\0", { "", "" } },
    { "trigger_hold", "Always\0Hold right mouse\0Hold Shift\0Hold Alt\0Hold mouse 4\0", { "", "", "", "", "" } },
    { "hit_style", "Cross\0Circle\0Cross + circle\0", { "", "", "" } },
    { "hit_color", "White\0Red\0Yellow\0Blue\0", { "", "", "", "" } },
    { "radar_pos", "Top left\0Top right\0Bottom left\0Bottom right\0Custom\0", { "", "", "", "", "Set with the two position sliders." } },
    { "anim_slot", "DefaultSlot\0FullBody\0UpperBody\0FullBodySlot\0",
      { "Only used by 'Blend in slot': plain animations play in this slot. Try another one if nothing moves." } },
};

// Sub-categories (the order of g_tabs). TAB_PLAYER is "Health", TAB_VISUALS is "View".
enum Tab { TAB_WEAPON, TAB_AIM, TAB_GRENADES, TAB_PLAYER, TAB_MOVEMENT, TAB_AI, TAB_MISSION, TAB_DOORS,
           TAB_ESP, TAB_VISUALS, TAB_CAMERA, TAB_UNLOCKS, TAB_LIVE, TAB_FUN, TAB_BINDS, TAB_DEBUG,
           TAB_ESP_LOOK, TAB_ESP_INFO, TAB_ESP_OBJ, TAB_ESP_RADAR, TAB_ESP_PERF };   // appended: keeps saved tab numbers

// Main categories, each holding sub-categories.
struct MainCat { const char* name; std::vector<int> subs; };
const std::vector<MainCat> kMain = {
    { "Player",   { TAB_PLAYER, TAB_MOVEMENT, TAB_CAMERA } },
    { "Weapons",  { TAB_WEAPON, TAB_AIM, TAB_GRENADES } },
    { "ESP",      { TAB_ESP, TAB_VISUALS } },
    { "AI",       { TAB_AI } },
    { "Doors",    { TAB_DOORS } },
    { "Lobby",    { TAB_MISSION, TAB_UNLOCKS, TAB_LIVE } },
    { "Settings", { TAB_BINDS, TAB_DEBUG } },
};

// ---------------------------------------------------------------------------------------------
// Pages: a fixed sidebar, every group and setting of the chosen page expanded on it.
// ---------------------------------------------------------------------------------------------
enum PageKind { PK_FEATURES, PK_PLAYERS, PK_ANIMS, PK_EFFECTS, PK_BINDS, PK_SETTINGS, PK_ACTIVE, PK_FAVS };
struct Page { const char* name; const char* desc; PageKind kind; std::vector<int> tabs; };
const std::vector<Page> kPages = {
    { "Active",           "Everything that is switched on right now, in one place",       PK_ACTIVE,   {} },
    { "Favourites",       "Features you pinned (right-click a feature's name to pin it)",  PK_FAVS,     {} },
    { "Player",           "God mode, health, status effects, movement and saved positions", PK_FEATURES, { TAB_PLAYER, TAB_MOVEMENT } },
    { "Weapons",          "Ammunition, recoil, spread, handling, damage and grenades",      PK_FEATURES, { TAB_WEAPON, TAB_GRENADES } },
    { "Aim",              "Aimbot",                                 PK_FEATURES, { TAB_AIM } },
    { "Players",          "Everyone in the session: position, health, ammunition, equipment", PK_PLAYERS, {} },
    { "AI",               "Suspects, civilians and your squad",                             PK_FEATURES, { TAB_AI } },
    { "Doors & Mission",  "Doors, traps, evidence, reports, time and objectives",           PK_FEATURES, { TAB_DOORS, TAB_MISSION } },
    { "ESP",              "Who is shown, how they look, what is written next to them, objects and radar", PK_FEATURES,
      { TAB_ESP, TAB_ESP_LOOK, TAB_ESP_INFO, TAB_ESP_OBJ, TAB_ESP_RADAR, TAB_ESP_PERF } },
    { "View & Camera",    "Field of view, stretched resolution and third person",           PK_FEATURES, { TAB_VISUALS, TAB_CAMERA } },
    { "Animations",       "Poses and every body animation in the game",                     PK_ANIMS,    {} },
    { "Effects & Sounds", "Particles, sounds and chaos toggles",                            PK_EFFECTS,  { TAB_FUN } },
    { "Unlocks & Live",   "Session unlocks and features added while the game runs",         PK_FEATURES, { TAB_UNLOCKS, TAB_LIVE } },
    { "Keybinds",         "A key for every feature and button",                             PK_BINDS,    {} },
    { "Settings",         "Interface size, connection and reset",                           PK_SETTINGS, {} },
};
int g_page = 0;
float g_uiScale = 1.f;
std::set<std::string> g_favs;     // pinned feature ids (Favourites page), saved in ui.cfg
char g_bindFilter[48] = "";
bool g_bindOnlyBound = false;   // Settings: show a switched-off feature's settings too

void TurnAllOff();

int MainOf(int tab) {
    for (int m = 0; m < (int)kMain.size(); ++m)
        for (int t : kMain[m].subs) if (t == tab) return m;
    return 0;
}

struct TabDef { const char* name; const char* subtitle; std::vector<Entry> entries; };

// Buttons. Bindable like features.
struct Action { const char* cmd; const char* label; const char* tip; bool danger; unsigned bind; };

std::vector<TabDef> g_tabs;
std::vector<Action> g_actions;
int g_tab = TAB_PLAYER;
#ifdef RONMENU_PREVIEW
bool g_previewBottom = false;
#endif
std::wstring g_modDir, g_stateFile, g_statusFile, g_cfgFile, g_animsFile;
bool g_open = false;
bool g_dirty = true;
bool g_cfgDirty = false;
DWORD g_cfgChangedAt = 0;
bool g_unload = false;
bool g_showHud = true;
unsigned g_menuKey = VK_INSERT, g_unloadKey = VK_END;
int  g_cmdSeq = 0;
std::string g_cmd, g_cmdArg;
unsigned g_keys = 0;
DWORD g_lastWrite = 0, g_lastRead = 0;
std::map<std::string, std::string> g_status;
long long g_heartbeat = 0;

// key capture for binds
unsigned* g_capture = nullptr;
std::string g_captureWhat;

// animation browser: every body animation in the game, read from Manifest_UFSFiles_Win64.txt
// in the background (the Lua side loads the chosen one on demand)
char g_animFilter[64] = "";
struct AnimAsset { std::string path, name, folder, lower; };
std::vector<AnimAsset> g_catalog;
std::vector<AnimAsset> g_fx;       // particle / Niagara effects
std::vector<AnimAsset> g_sounds;   // FMOD events
std::atomic<int> g_catalogState{ 0 };   // 0 not started, 1 loading, 2 ready, 3 failed
std::thread g_catalogThread;
std::vector<int> g_results;
size_t g_resultTotal = 0;
std::string g_lastQuery = "\x01";
bool g_tpOnly = true, g_lastTpOnly = true;
int g_animSel = -1;
std::string g_pendingPreset;
extern bool g_funOpen;
extern std::map<std::string, int> g_liveActionTab;

// selection in the left-to-right layout
std::string g_sel;          // feature id, "@anim" or "@actions"
int g_step = 0;             // current step in the set-up pane
char g_search[64] = "";

// Live tab: features and buttons the Lua side declares at runtime (live mods added through the
// MCP bridge). Lua writes live_ui.txt; ids are prefixed "lv_" in the state file and config.
std::deque<std::string> g_strPool;               // stable storage for live labels (Entry holds const char*)
std::map<std::string, std::string> g_liveCfg;    // saved e.lv_* / a.lv_* settings, applied when the entry appears
std::string g_liveUiRaw;
DWORD g_liveReadAt = 0;

// ---------------------------------------------------------------------------------------------
// Look
// ---------------------------------------------------------------------------------------------
ImFont* g_fontRegular = nullptr;
ImFont* g_fontSmall = nullptr;
ImFont* g_fontBold = nullptr;
ImFont* g_fontTitle = nullptr;
ImFont* g_fontBig = nullptr;
ImGuiStyle g_baseStyle;
float g_styleScale = 0.f;

// Palette: black surfaces, white and grey text. Colour only where it carries a meaning (on, status).
const ImVec4 kAccent(0.80f, 0.82f, 0.86f, 1.f);        // selection, sliders (neutral)
constexpr float kR = 0.f;                               // square corners
const ImVec4 kGreen(0.47f, 0.90f, 0.59f, 1.f);
const ImVec4 kRed(0.95f, 0.42f, 0.42f, 1.f);
const ImVec4 kYellow(0.96f, 0.82f, 0.40f, 1.f);
const ImVec4 kGray(0.56f, 0.56f, 0.59f, 1.f);          // secondary text
const ImVec4 kText(0.94f, 0.94f, 0.95f, 1.f);          // primary text
const ImVec4 kBg(0.f, 0.f, 0.f, 0.95f);                // window: black, 95% opaque
const ImVec4 kSidebar(0.f, 0.f, 0.f, 0.f);             // left column: no fill
const ImVec4 kListBg(0.f, 0.f, 0.f, 0.f);              // middle column: no fill
const ImVec4 kCard(0.13f, 0.13f, 0.14f, 1.f);          // input boxes and lists
const ImVec4 kCardOn(0.13f, 0.13f, 0.14f, 1.f);
const ImU32 kLine = IM_COL32(255, 255, 255, 22);       // separators

ImU32 Col(const ImVec4& c, float alphaMul = 1.f) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, c.w * alphaMul));
}

// UI size unit: 1.0 at the base font size, grows with resolution.
float U() { return ImGui::GetFontSize() / 15.f; }

void BuildEntries() {
    g_tabs = {
        { "Weapon", "Reload, recoil, ammo, fire rate and handling", {
            { "infinite_ammo", "Infinite ammo",   "Magazines never run dry (the weapon's own infinite-ammo flag).", 0, false, false, 0, 0, 0, "" },
            { "no_recoil",     "No recoil",       "No kick, no muzzle climb.", 0, false, false, 0, 0, 0, "" },
            { "rapid_fire",    "Rapid fire",      "Multiplies fire rate.", 0, false, true, 1.f, 5.f, 2.f, "x%.1f" },
        }},
        { "Aim", "Aimbot", {
            { "aimbot",        "Aimbot",               "Turns your view onto a target inside the circle. The amount is the circle size in degrees.", 0, false, true, 1.f, 60.f, 8.f, "%.0f\xC2\xB0" },
            { "aim_key",       "When it aims",         "", 0, true, false, 0, 0, 0, "" },
            { "aim_bone",      "Aim at",               "", 0, true, false, 0, 0, 0, "" },
            { "aim_targets",   "Targets",              "", 0, true, false, 0, 0, 0, "" },
            { "aim_priority",  "Which target first",   "", 0, true, false, 0, 0, 0, "" },
            { "aim_smooth",    "Smoothing",            "1 = snaps instantly, higher = turns slower and looks more natural.", 0, true, true, 1.f, 30.f, 6.f, "%.0f" },
            { "aim_visible",   "Only visible targets", "Skip targets behind walls (checked by the Lua mod about 12x per second).", 0, false, false, 0, 0, 0, "" },
            { "aim_skip_down", "Skip surrendered / cuffed", "Never aim at someone who gave up or is cuffed.", 0, false, false, 0, 0, 0, "" },
            { "aim_sticky",    "Stay on target",       "Keeps the same target while it stays near the circle.", 0, false, false, 0, 0, 0, "" },
            { "aim_fov_circle","Show circle",          "Draws the aim circle.", 0, false, false, 0, 0, 0, "" },
            { "aim_line",      "Show aim line in 3D",  "Dashed line from your gun to the aim point, in the world.", 0, false, false, 0, 0, 0, "" },
            { "aim_ads_only",  "Only while aiming down sights", "", 0, false, false, 0, 0, 0, "" },
            { "aim_friend",    "Aim at the selected player (pranks)", "Also aims at the player picked on the Players page. Only for friends who agreed.", 0, false, false, 0, 0, 0, "" },
            { "aim_strength",  "Strength",             "Share of the way to the target per step (1 = full).", 0, true, true, 0.1f, 1.f, 1.f, "%.2f" },
            { "aim_maxdist",   "Max distance",         "", 0, true, true, 5.f, 300.f, 150.f, "%.0f m" },
            { "trigger",       "Triggerbot",           "Clicks fire for you while a target is under your crosshair; the game's own firing decides whether a shot comes out. The amount is the delay before the first click.", 0, false, true, 0.f, 500.f, 90.f, "%.0f ms" },
            { "trigger_hold",  "Active",               "", 0, true, false, 0, 0, 0, "" },
            { "trigger_dist",  "Max distance",         "", 0, true, true, 5.f, 150.f, 50.f, "%.0f m" },
            { "trigger_ads",   "Only while aiming down sights", "", 0, false, false, 0, 0, 0, "" },
            { "trigger_suspects","Suspects only",      "", 0, false, false, 0, 0, 0, "" },
            { "trigger_skip_down","Skip surrendered / cuffed", "", 0, false, false, 0, 0, 0, "" },
        }},
        { "Grenades", "Fuse time and throw distance", {
        }},
        { "Health", "Damage, armour and status effects", {
            { "god",   "God mode",       "No damage: the game's own god-mode flag plus unlimited health.", VK_F3, false, false, 0, 0, 0, "" },
        }},
        { "Movement", "Speed, flying and positioning", {
            { "speed", "Move speed",     "Makes you move faster.", 0, false, true, 1.f, 5.f, 1.8f, "x%.1f" },
            { "fly",   "Fly",            "Fly where you look. WASD moves, SPACE up, CTRL down. Turn off to shoot.", VK_F4, false, true, 300.f, 6000.f, 1500.f, "%.0f" },
            { "fly_noclip", "Noclip while flying", "Ignore all collision while flying.", 0, false, false, 0, 0, 0, "" },
        }},
        { "AI", "Suspects, civilians and your squad", {
            { "freeze_ai",   "Freeze AI",          "Suspects and civilians stop moving and acting.", VK_F5, false, false, 0, 0, 0, "" },
            { "ai_accuracy", "Suspect accuracy",   "How well suspects shoot. 0 = they miss almost everything.", 0, false, true, 0.f, 1.f, 0.15f, "%.2f" },
            { "no_fake_surrender", "No fake surrenders", "Suspects that surrender mean it.", 0, false, false, 0, 0, 0, "" },
            { "morale_break","Break morale",       "Drains suspects' morale so they give up instead of fighting.", 0, false, true, 0.05f, 1.f, 0.3f, "%.2f / s" },
            { "slow_ai",     "Slow AI reactions",  "Suspects turn and aim slower (x1 = normal).", 0, false, true, 0.1f, 1.f, 0.4f, "x%.2f" },
            { "fast_arrest", "Fast restrain", "Restrain (zip-cuff) suspects and civilians faster.", 0, false, true, 2.f, 20.f, 6.f, "x%.0f" },
            { "squad_god",   "Squad god mode",     "Your AI teammates cannot be hurt.", 0, false, false, 0, 0, 0, "" },
            { "squad_auto",  "Squad auto-clear",   "Your squad clears the level on its own (the game's own automation).", 0, false, false, 0, 0, 0, "" },
            { "spawn_count", "Spawn amount",       "How many suspects 'Spawn suspects' adds.", 0, true, true, 1.f, 10.f, 3.f, "%.0f" },
        }},
        { "Mission", "Time, bombs, objectives and score", {
            { "game_speed", "Game speed",  "Global time. Below 1 is slow motion.", 0, false, true, 0.05f, 3.f, 0.5f, "x%.2f" },
            { "freeze_timer","Freeze mission timer", "The mission clock stops.", 0, false, false, 0, 0, 0, "" },
            { "bombs",      "Bombs: freeze and instant defuse", "Bomb timers stop counting down and defusing takes no time.", 0, false, false, 0, 0, 0, "" },
            { "end_grade",  "Grade for 'End mission'", "", 0, true, false, 0, 0, 0, "" },
        }},
        { "Doors", "Breaching, locks, traps and tools", {
            { "reveal_doors", "Reveal traps and locks", "Your team knows which doors are trapped or locked, as if you had checked them.", 0, false, false, 0, 0, 0, "" },
            { "door_kick",    "Kicks always open",      "Every door kick succeeds.", 0, false, false, 0, 0, 0, "" },
            { "fast_tools",   "Fast lockpick / multitool", "Lockpicking, picking with the multitool and disarming take this many times less time.", 0, false, true, 2.f, 20.f, 6.f, "x%.0f" },
        }},
        { "Targets", "Who is shown, and how far away", {
            { "esp",          "ESP",                  "Marks characters (and chosen objects) through walls. Everything below sets who and what.", VK_F8, false, false, 0, 0, 0, "" },
            { "esp_visonly",  "Only visible characters", "Hide characters behind walls (checked by the Lua mod about 12x per second).", 0, false, false, 0, 0, 0, "" },
            { "esp_vis_color","Colour hidden characters differently", "", 0, false, false, 0, 0, 0, "#" },
            { "col_hidden",   "Hidden colour",        "", 0, true, false, 0, 0, 0, "#color" },
            { "esp_aimhl",    "Highlight the aim target", "", 0, false, false, 0, 0, 0, "" },
            { "col_aim",      "Aim target colour",    "", 0, true, false, 0, 0, 0, "#color" },
            { "esp_selhl",    "Highlight the selected player", "The player picked on the Players page.", 0, false, false, 0, 0, 0, "" },
            { "col_sel",      "Selected player colour", "", 0, true, false, 0, 0, 0, "#color" },
            { "esp_suspects", "Suspects",             "", 0, false, false, 0, 0, 0, "" },
            { "esp_dist",     "Max distance",         "", 0, true, true, 10.f, 300.f, 150.f, "%.0f m" },
            { "col_suspect",  "Colour",               "", 0, true, false, 0, 0, 0, "#color" },
            { "esp_civ",      "Civilians",            "", 0, false, false, 0, 0, 0, "" },
            { "esp_dist_civ", "Max distance",         "", 0, true, true, 10.f, 300.f, 150.f, "%.0f m" },
            { "col_civ",      "Colour",               "", 0, true, false, 0, 0, 0, "#color" },
            { "esp_swat",     "AI SWAT team",         "", 0, false, false, 0, 0, 0, "" },
            { "col_swat",     "Colour",               "", 0, true, false, 0, 0, 0, "#color" },
            { "esp_friends",  "Other players",        "Your friends in the session (never you).", 0, false, false, 0, 0, 0, "" },
            { "esp_dist_team","Max distance (SWAT and players)", "", 0, true, true, 10.f, 300.f, 150.f, "%.0f m" },
            { "col_friend",   "Colour",               "", 0, true, false, 0, 0, 0, "#color" },
            { "esp_surr",     "Surrendered / complying", "", 0, false, false, 0, 0, 0, "" },
            { "col_surr",     "Colour",               "", 0, true, false, 0, 0, 0, "#color" },
            { "esp_cuffed",   "Cuffed",               "", 0, false, false, 0, 0, 0, "" },
            { "col_cuffed",   "Colour",               "", 0, true, false, 0, 0, 0, "#color" },
            { "esp_down",     "Down (dead or unconscious)", "The game reports dead and unconscious the same way, so they share this switch.", 0, false, false, 0, 0, 0, "" },
            { "col_dead",     "Colour",               "", 0, true, false, 0, 0, 0, "#color" },
        }},
        { "View", "Field of view, stretched resolution and camera effects", {
            { "fov",     "Field of view",        "Wider (or narrower) view.", 0, false, true, 60.f, 130.f, 100.f, "%.0f" },
            { "stretch", "Stretched resolution", "Shows the middle of the screen stretched to full width, like 4:3 stretched in CS. Everything looks wider.", 0, false, true, 1.f, 1.6f, 1.333f, "x%.2f" },
        }},
        { "Camera", "Third person and its camera", {
            { "third_person", "Third person",  "See your own character. The slider is the camera distance.", 0, false, true, 100.f, 1000.f, 300.f, "%.0f cm" },
            { "tp_side",      "Shoulder offset", "Moves the third-person camera sideways (negative = left shoulder).", 0, true, true, -150.f, 150.f, 50.f, "%.0f cm" },
            { "tp_height",    "Camera height",   "Moves the third-person camera up or down.", 0, true, true, -60.f, 150.f, 30.f, "%.0f cm" },
            { "anim_method",  "How animations play", "", 0, true, false, 0, 0, 0, "" },
            { "anim_rate",    "Animation speed", "", 0, true, true, 0.25f, 3.f, 1.f, "x%.2f" },
            { "anim_slot",    "Animation slot",  "", 0, true, false, 0, 0, 0, "" },
        }},
        { "Unlocks", "Progression unlocks for this session", {
            { "unlock_cosmetics", "Earned cosmetics", "Unlocks the customization items you would normally earn by playing (mission grades, achievements). Stays unlocked until the game restarts.", 0, false, false, 0, 0, 0, "" },
            { "unlock_missions",  "All missions",     "Every mission is selectable, without the score needed on the previous one.", 0, false, false, 0, 0, 0, "" },
        }},
        { "Live", "Features added while the game runs", {} },
        { "Chaos", "Toggles and buttons from the fun mod", {} },
        { "Binds", "Keys for every feature and action", {} },
        { "Debug", "Connection, dumps and settings", {} },
        { "Appearance", "Boxes, skeletons, heads and colours. Presets below.", {
            { "esp_style",    "Style",                "", 0, true, false, 0, 0, 0, "" },
            { "box_thick",    "Box thickness",        "", 0, true, true, 0.5f, 5.f, 1.5f, "%.1f px" },
            { "box_outline",  "Dark outline",         "A dark edge around boxes, skeletons and markers so they read on any background.", 0, false, false, 0, 0, 0, "" },
            { "col_outline",  "Outline colour",       "", 0, true, false, 0, 0, 0, "#color" },
            { "corner_len",   "Corner length",        "Share of the box side (corner box style).", 0, true, true, 0.1f, 0.5f, 0.25f, "%.2f" },
            { "box_fill",     "Box fill",             "0 = no fill.", 0, true, true, 0.f, 1.f, 0.f, "%.2f" },
            { "skel_thick",   "Skeleton thickness",   "", 0, true, true, 0.5f, 5.f, 1.6f, "%.1f px" },
            { "skel_alpha",   "Skeleton opacity",     "", 0, true, true, 0.1f, 1.f, 1.f, "%.2f" },
            { "skel_joints",  "Joint dots",           "", 0, false, false, 0, 0, 0, "" },
            { "joint_size",   "Dot size",             "", 0, true, true, 1.f, 5.f, 2.f, "%.1f px" },
            { "head_marker",  "Head marker",          "", 0, true, false, 0, 0, 0, "" },
            { "head_size",    "Head marker size",     "", 0, true, true, 0.5f, 3.f, 1.f, "x%.1f" },
            { "esp_fade",     "Fade with distance",   "", 0, false, false, 0, 0, 0, "" },
            { "esp_minsize",  "Smallest drawn size",  "Characters smaller than this on screen are skipped.", 0, true, true, 0.f, 60.f, 6.f, "%.0f px" },
        }},
        { "Information", "Health, labels, tracers and off-screen arrows", {
            { "esp_health",   "Health bar",           "Unreadable health is not drawn (never shown as zero).", 0, false, false, 0, 0, 0, "" },
            { "hp_pos",       "Position",             "", 0, true, false, 0, 0, 0, "" },
            { "hp_thick",     "Thickness",            "", 0, true, true, 2.f, 8.f, 3.f, "%.0f px" },
            { "hp_gradient",  "Colour by health",     "Green to red as health drops.", 0, false, false, 0, 0, 0, "" },
            { "col_hp",       "Bar colour",           "Used when 'Colour by health' is off.", 0, true, false, 0, 0, 0, "#color" },
            { "hp_recent",    "Show recent damage",   "The part lost in the last second, in white.", 0, false, false, 0, 0, 0, "" },
            { "lbl_name",     "Label: name / type",   "Players show their name, everyone else their type.", 0, false, false, 0, 0, 0, "" },
            { "lbl_dist",     "Label: distance",      "", 0, false, false, 0, 0, 0, "" },
            { "lbl_health",   "Label: health",        "Shown as 'health ?' when the game does not send it.", 0, false, false, 0, 0, 0, "" },
            { "lbl_state",    "Label: state",         "active / surrendered / cuffed / down.", 0, false, false, 0, 0, 0, "" },
            { "lbl_weapon",   "Label: weapon in hand", "", 0, false, false, 0, 0, 0, "" },
            { "lbl_size",     "Label size",           "", 0, true, true, 0.6f, 1.6f, 1.f, "x%.2f" },
            { "lbl_pos",      "Placement",            "", 0, true, false, 0, 0, 0, "" },
            { "lbl_spacing",  "Line spacing",         "", 0, true, true, 0.f, 8.f, 1.f, "%.0f px" },
            { "lbl_units",    "Distance in",          "", 0, true, false, 0, 0, 0, "" },
            { "lbl_compact",  "One line",             "", 0, false, false, 0, 0, 0, "" },
            { "lbl_shadow",   "Text shadow",          "", 0, false, false, 0, 0, 0, "" },
            { "lbl_catcolor", "Text in the character's colour", "", 0, false, false, 0, 0, 0, "" },
            { "col_text",     "Text colour",          "", 0, true, false, 0, 0, 0, "#color" },
            { "esp_lines",    "Tracer lines",         "", 0, false, false, 0, 0, 0, "" },
            { "line_origin",  "From",                 "", 0, true, false, 0, 0, 0, "" },
            { "line_thick",   "Thickness",            "", 0, true, true, 0.5f, 4.f, 1.f, "%.1f px" },
            { "line_alpha",   "Opacity",              "", 0, true, true, 0.1f, 1.f, 0.7f, "%.2f" },
            { "line_aimonly", "Only aim target / selected player", "", 0, false, false, 0, 0, 0, "" },
            { "esp_arrows",   "Off-screen arrows",    "Arrows around the crosshair pointing at characters outside the view.", 0, false, false, 0, 0, 0, "" },
            { "arrow_size",   "Arrow size",           "", 0, true, true, 6.f, 30.f, 12.f, "%.0f px" },
            { "arrow_radius", "Distance from centre", "", 0, true, true, 0.2f, 0.95f, 0.55f, "%.2f" },
            { "arrow_alpha",  "Opacity",              "", 0, true, true, 0.1f, 1.f, 0.8f, "%.2f" },
            { "arrow_dist",   "Show distance",        "", 0, false, false, 0, 0, 0, "" },
        }},
        { "Objects", "Each kind separately; collected, reported and disarmed ones drop out", {
            { "esp_evidence", "Evidence",             "", 0, false, false, 0, 0, 0, "" },
            { "col_evidence", "Colour",               "", 0, true, false, 0, 0, 0, "#color" },
            { "esp_weapons",  "Dropped weapons",      "", 0, false, false, 0, 0, 0, "" },
            { "col_weapon",   "Colour",               "", 0, true, false, 0, 0, 0, "#color" },
            { "esp_bodies",   "Bodies not yet reported", "", 0, false, false, 0, 0, 0, "" },
            { "col_body",     "Colour",               "", 0, true, false, 0, 0, 0, "#color" },
            { "esp_traps",    "Traps",                "", 0, false, false, 0, 0, 0, "" },
            { "trap_activeonly","Only live traps",    "", 0, false, false, 0, 0, 0, "" },
            { "col_trap",     "Colour",               "", 0, true, false, 0, 0, 0, "#color" },
            { "esp_bombs",    "Bombs (with countdown)", "", 0, false, false, 0, 0, 0, "" },
            { "col_bomb",     "Colour",               "", 0, true, false, 0, 0, 0, "#color" },
            { "esp_projectiles","Grenades in the world", "Fuse time is not readable, so none is shown.", 0, false, false, 0, 0, 0, "" },
            { "esp_objectives","Objectives",          "The game's own objective markers in the world, with their text and distance. Objects farther than 'Object distance' are not drawn.", 0, false, false, 0, 0, 0, "" },
            { "col_objective","Colour",               "", 0, true, false, 0, 0, 0, "#color" },
            { "obj_open",     "Only open objectives", "Hides finished and failed objectives. Turn it off to see them greyed out.", 0, false, false, 0, 0, 0, "" },
            { "obj_dist",     "Object distance",      "", 0, true, true, 5.f, 300.f, 80.f, "%.0f m" },
            { "obj_size",     "Object label size",    "", 0, true, true, 0.6f, 1.6f, 1.f, "x%.2f" },
        }},
        { "Radar & HUD", "Radar, hit markers, grenade arc, gunshots and counters", {
            { "radar",        "Radar",                "Small top-down map of the characters your game knows about. The amount is its range.", 0, false, true, 10.f, 100.f, 40.f, "%.0f m" },
            { "radar_size",   "Size",                 "", 0, true, true, 120.f, 400.f, 200.f, "%.0f px" },
            { "radar_pos",    "Position",             "", 0, true, false, 0, 0, 0, "" },
            { "radar_x",      "Custom position: across", "Used with Position = Custom.", 0, true, true, 0.f, 1.f, 0.02f, "%.2f" },
            { "radar_y",      "Custom position: down", "", 0, true, true, 0.f, 1.f, 0.1f, "%.2f" },
            { "radar_shape",  "Shape",                "", 0, true, false, 0, 0, 0, "" },
            { "radar_bg",     "Background opacity",   "", 0, true, true, 0.f, 1.f, 0.65f, "%.2f" },
            { "col_radar_border","Border colour",     "", 0, true, false, 0, 0, 0, "#color" },
            { "radar_marker", "Marker size",          "", 0, true, true, 2.f, 8.f, 4.f, "%.1f px" },
            { "radar_rings",  "Distance rings",       "", 0, false, false, 0, 0, 0, "" },
            { "radar_clamp",  "Pin far ones to the edge", "", 0, false, false, 0, 0, 0, "" },
            { "radar_rotate", "Turn with your view",  "", 0, false, false, 0, 0, 0, "" },
            { "radar_civ",    "Civilians",            "", 0, false, false, 0, 0, 0, "" },
            { "radar_team",   "SWAT and players",     "", 0, false, false, 0, 0, 0, "" },
            { "radar_height", "Above / below arrows", "", 0, false, false, 0, 0, 0, "" },
            { "hitmarkers",   "Hit markers",          "Marks only hits that really landed: the target's health dropped right after your shot. The amount is how long a marker stays.", 0, false, true, 0.2f, 2.f, 0.6f, "%.1f s" },
            { "hit_style",    "Marker",               "", 0, true, false, 0, 0, 0, "" },
            { "hit_color",    "Colour",               "", 0, true, false, 0, 0, 0, "" },
            { "hit_head",     "Gold for headshots",   "", 0, false, false, 0, 0, 0, "" },
            { "hit_numbers",  "Damage numbers",       "", 0, false, false, 0, 0, 0, "" },
            { "grenade_arc",  "Grenade arc",          "While you hold a grenade: its predicted path from the grenade's own throw values, and where it lands. No bounces.", 0, false, false, 0, 0, 0, "" },
            { "arc_landing",  "Landing marker",       "", 0, false, false, 0, 0, 0, "" },
            { "sound_markers","Gunshot markers",      "Briefly marks where other weapons fired (the game's own fire event). The amount is how long a marker stays.", 0, false, true, 1.f, 8.f, 3.f, "%.0f s" },
            { "snd_range",    "Range",                "", 0, true, true, 10.f, 150.f, 60.f, "%.0f m" },
            { "hud_counts",   "Counter on HUD",       "Suspects / civilians: up, surrendered, cuffed, down.", 0, false, false, 0, 0, 0, "" },
            { "hud_score",    "Score on HUD",         "Your current mission score.", 0, false, false, 0, 0, 0, "" },
            { "hud_bomb",     "Bomb countdown on HUD", "Time left on the closest bomb.", 0, false, false, 0, 0, 0, "" },
        }},
        { "Performance", "How often data is refreshed and how much is drawn", {
            { "esp_rate",     "Position / state refresh", "How often the Lua mod sends characters. Lower = smoother, more work.", 0, true, true, 40.f, 500.f, 80.f, "every %.0f ms" },
            { "esp_discover", "Find new characters",  "How often the level is searched for characters that appeared.", 0, true, true, 0.2f, 5.f, 0.5f, "every %.1f s" },
            { "esp_skel_every","Skeleton refresh",    "Bones are re-read on every Nth refresh.", 0, true, true, 1.f, 6.f, 1.f, "every %.0f" },
            { "esp_vis_every","Visibility checks",    "Wall checks on every Nth refresh.", 0, true, true, 1.f, 6.f, 1.f, "every %.0f" },
            { "esp_maxdraw",  "Most characters drawn", "The nearest ones win.", 0, true, true, 5.f, 100.f, 60.f, "%.0f" },
        }},
    };
    const char* plain[] = { "fly_noclip", "esp_style", "esp_civ", "esp_down", "esp_health", "esp_lines", "esp_traps",
                            "esp_bombs", "esp_evidence", "esp_dist", "hud_counts", "hud_score", "hud_bomb",
                            "anim_rate", "anim_slot", "spawn_count", "end_grade",
                            "tp_side", "tp_height", "anim_method",
                            "aim_key", "aim_bone", "aim_targets", "aim_priority", "aim_smooth", "aim_visible", "aim_skip_down",
                            "aim_sticky", "aim_fov_circle", "aim_line",
                            "aim_ads_only", "aim_strength", "aim_maxdist",
                            "trigger_hold", "trigger_dist", "trigger_ads", "trigger_suspects", "trigger_skip_down",
                            "esp_projectiles",
                            "hit_style", "hit_color", "hit_head", "hit_numbers",
                            "radar_size", "radar_pos", "radar_rotate", "radar_civ", "radar_height", "arc_landing", "snd_range" };
    for (auto& t : g_tabs)
        for (auto& e : t.entries) {
            e.val = e.def;
            for (const char* id : plain) if (!strcmp(e.id, id)) e.noStatus = true;
            for (const ModeInfo& m : kModes) if (!strcmp(e.id, m.id)) e.combo = m.items;
            for (const char* id : { "fly_noclip", "esp_down", "esp_health", "aim_visible",
                                    "aim_skip_down", "aim_sticky", "aim_fov_circle", "aim_line",
                                    "trigger_suspects", "trigger_skip_down",
                                    "hit_head", "hit_numbers", "radar_rotate", "radar_civ", "radar_height", "arc_landing",
                                    "esp_suspects", "esp_surr", "esp_cuffed", "esp_aimhl", "esp_selhl", "box_outline", "lbl_name",
                                    "lbl_dist", "lbl_shadow", "hp_gradient", "arrow_dist", "radar_rings", "trap_activeonly",
                                    "obj_open" })
                if (!strcmp(e.id, id)) e.on = true;   // sensible defaults
        }
    // ESP groups: only the Lua-reported features keep a status line
    for (int t : { (int)TAB_ESP, (int)TAB_ESP_LOOK, (int)TAB_ESP_INFO, (int)TAB_ESP_OBJ, (int)TAB_ESP_RADAR, (int)TAB_ESP_PERF })
        for (auto& e : g_tabs[t].entries) {
            bool reported = false;
            for (const char* id : { "esp", "hitmarkers", "sound_markers", "grenade_arc", "esp_projectiles", "esp_objectives" }) if (!strcmp(e.id, id)) reported = true;
            if (!reported) e.noStatus = true;
        }
    for (auto& t : g_tabs)
        for (auto& e : t.entries)
            if (!strcmp(e.id, "aim_friend")) e.noStatus = true;
    struct Parent { const char* child; const char* parent; };
    const Parent parents[] = {
        { "fly_noclip", "fly" },
        { "esp_visonly", "esp" }, { "esp_vis_color", "esp" }, { "col_hidden", "esp" }, { "esp_aimhl", "esp" }, { "col_aim", "esp" },
        { "esp_selhl", "esp" }, { "col_sel", "esp" },
        { "esp_dist", "esp_suspects" }, { "col_suspect", "esp_suspects" }, { "esp_dist_civ", "esp_civ" }, { "col_civ", "esp_civ" },
        { "col_swat", "esp_swat" }, { "esp_dist_team", "esp_friends" }, { "col_friend", "esp_friends" },
        { "col_surr", "esp_surr" }, { "col_cuffed", "esp_cuffed" }, { "col_dead", "esp_down" },
        { "col_outline", "box_outline" }, { "joint_size", "skel_joints" }, { "head_size", "head_marker" },
        { "hp_pos", "esp_health" }, { "hp_thick", "esp_health" }, { "hp_gradient", "esp_health" }, { "col_hp", "esp_health" }, { "hp_recent", "esp_health" },
        { "lbl_pos", "lbl_size" }, { "lbl_spacing", "lbl_size" }, { "lbl_units", "lbl_size" }, { "lbl_compact", "lbl_size" },
        { "lbl_shadow", "lbl_size" }, { "lbl_catcolor", "lbl_size" }, { "col_text", "lbl_size" },
        { "line_origin", "esp_lines" }, { "line_thick", "esp_lines" }, { "line_alpha", "esp_lines" }, { "line_aimonly", "esp_lines" },
        { "arrow_size", "esp_arrows" }, { "arrow_radius", "esp_arrows" }, { "arrow_alpha", "esp_arrows" }, { "arrow_dist", "esp_arrows" },
        { "col_evidence", "esp_evidence" }, { "col_weapon", "esp_weapons" }, { "col_body", "esp_bodies" },
        { "col_objective", "esp_objectives" }, { "obj_open", "esp_objectives" },
        { "trap_activeonly", "esp_traps" }, { "col_trap", "esp_traps" }, { "col_bomb", "esp_bombs" },
        { "radar_x", "radar" }, { "radar_y", "radar" }, { "radar_shape", "radar" }, { "radar_bg", "radar" }, { "col_radar_border", "radar" },
        { "radar_marker", "radar" }, { "radar_rings", "radar" }, { "radar_clamp", "radar" }, { "radar_team", "radar" },
        { "aim_friend", "aimbot" },
        { "tp_side", "third_person" }, { "tp_height", "third_person" },
        { "anim_rate", "@anim" }, { "anim_slot", "@anim" },
        { "spawn_count", "@actions" }, { "end_grade", "@actions" },
        { "aim_key", "aimbot" }, { "aim_bone", "aimbot" }, { "aim_targets", "aimbot" }, { "aim_priority", "aimbot" },
        { "aim_smooth", "aimbot" }, { "aim_visible", "aimbot" }, { "aim_skip_down", "aimbot" }, { "aim_sticky", "aimbot" },
        { "aim_fov_circle", "aimbot" }, { "aim_line", "aimbot" },
        { "aim_ads_only", "aimbot" }, { "aim_strength", "aimbot" }, { "aim_maxdist", "aimbot" },
        { "trigger_hold", "trigger" }, { "trigger_dist", "trigger" }, { "trigger_ads", "trigger" },
        { "trigger_suspects", "trigger" }, { "trigger_skip_down", "trigger" },
        { "hit_style", "hitmarkers" }, { "hit_color", "hitmarkers" }, { "hit_head", "hitmarkers" }, { "hit_numbers", "hitmarkers" },
        { "radar_size", "radar" }, { "radar_pos", "radar" }, { "radar_rotate", "radar" }, { "radar_civ", "radar" }, { "radar_height", "radar" },
        { "arc_landing", "grenade_arc" }, { "snd_range", "sound_markers" },
    };
    for (auto& t : g_tabs)
        for (auto& e : t.entries)
            for (const Parent& p : parents) if (!strcmp(e.id, p.child)) e.parent = p.parent;

    g_actions = {
        { "heal",               "Heal",                       nullptr, false, 0 },
        { "tp_crosshair",       "Teleport to crosshair",      nullptr, false, VK_F7 },
        { "esp_preset_clean",   "Clean",                      "Corner boxes, name and distance only.", false, 0 },
        { "esp_preset_detailed","Detailed",                   "Skeleton + box, health bar and every label.", false, 0 },
        { "esp_preset_friends", "Friends",                    "Only your squad and the other players, with name, health and weapon.", false, 0 },
        { "esp_preset_save",    "Save as my preset",          "Remembers the current look (appearance, information, colours).", false, 0 },
        { "esp_preset_load",    "Load my preset",             nullptr, false, 0 },
        { "esp_preset_reset",   "Reset look",                 "Appearance, information and colours back to default. Who is shown stays.", false, 0 },
        { "save_pos",           "Save position",              nullptr, false, 0 },
        { "load_pos",           "Load position",              nullptr, false, 0 },
        { "refill_ammo",        "Refill ammo",                nullptr, false, 0 },
        { "drop_gun",           "Drop held item",             "Throws whatever is in your hands on the floor. Pick it up again with the interact key.", false, 0 },
        { "drop_gun_evidence",  "Drop gun as evidence",       "Drops it the way surrendering suspects do (shows as evidence to secure).", false, 0 },
        { "drop_all_guns",      "Drop all weapons",           nullptr, false, 0 },
        { "drop_everything",    "Drop all items",             "Weapons, grenades, tools: everything in your inventory.", true, 0 },
        { "door_unlock_look",   "Unlock door I'm looking at", nullptr, false, 0 },
        { "door_lock_look",     "Lock door I'm looking at",   nullptr, false, 0 },
        { "unlock_doors",       "Unlock all doors",           nullptr, false, 0 },
        { "keycards",           "Give all keycards",          nullptr, false, 0 },
        { "elocks_off",         "Disable electronic locks",   nullptr, false, 0 },
        { "surrender_suspects", "Make suspects surrender",    nullptr, false, 0 },
        { "arrest_suspects",    "Arrest all suspects",        nullptr, false, 0 },
        { "report_suspects",    "Report suspects to TOC",     "Reports every suspect to TOC (needed for score).", false, 0 },
        { "kill_suspects",      "Kill all suspects",          "Counts as unauthorized force on your score.", true, 0 },
        { "drop_weapons",       "Drop all suspect weapons",   nullptr, false, 0 },
        { "surrender_civilians","Make civilians comply",      nullptr, false, 0 },
        { "arrest_civilians",   "Arrest all civilians",       nullptr, false, 0 },
        { "report_civilians",   "Report civilians to TOC",    nullptr, false, 0 },
        { "spawn_suspects",     "Spawn suspects",             "Adds random suspects (amount set above).", false, 0 },
        { "ai_hide",            "Make AI hide",               nullptr, false, 0 },
        { "ai_unhide",          "Stop AI hiding",             nullptr, false, 0 },
        { "hostages",           "Suspects take hostages",     nullptr, true, 0 },
        { "flush_bodies",       "Remove dead bodies",         nullptr, false, 0 },
        { "reset_ai",           "Reset AI",                   nullptr, false, 0 },
        { "tp_next_ai",         "Teleport to next AI",        nullptr, false, 0 },
        { "secure_evidence",    "Secure all evidence",        "Secures every piece of evidence and dropped weapon in the level.", false, 0 },
        { "complete_objectives","Complete objectives",        nullptr, false, 0 },
        { "end_mission",        "End mission with grade",     "Ends the mission now with the grade chosen above.", true, 0 },
        { "anim_crouch",        "Crouch",                     "Crouched pose while you are really standing.", false, 0 },
        { "anim_surrender",     "Kneel, hands on head",       nullptr, false, 0 },
        { "anim_handsup",       "Hands up",                   nullptr, false, 0 },
        { "anim_cuffed",        "Cuffed",                     nullptr, false, 0 },
        { "anim_sit",           "Sitting",                    nullptr, false, 0 },
        { "anim_relaxed",       "Relaxed stand",              nullptr, false, 0 },
        { "anim_play",          "Play selected animation",    nullptr, false, 0 },
        { "anim_stop",          "Stop animation",             nullptr, false, 0 },
    };
    g_menuKey = VK_INSERT;
    g_unloadKey = VK_END;
    g_showHud = true;
    g_liveUiRaw.clear();   // re-read the live tab on the next tick
}

// Colour settings (entries whose fmt is "#color"): RGBA, saved as c.<id>=r,g,b,a
struct ColorDef { const char* id; ImVec4 def; };
const ColorDef kColorDefs[] = {
    { "col_suspect", ImVec4(1.f, 0.24f, 0.24f, 1.f) },  { "col_civ", ImVec4(0.27f, 0.90f, 0.43f, 1.f) },
    { "col_swat", ImVec4(0.35f, 0.62f, 1.f, 1.f) },     { "col_friend", ImVec4(0.30f, 0.90f, 0.95f, 1.f) },
    { "col_surr", ImVec4(1.f, 0.82f, 0.24f, 1.f) },     { "col_cuffed", ImVec4(0.35f, 0.67f, 1.f, 1.f) },
    { "col_dead", ImVec4(0.59f, 0.59f, 0.59f, 0.86f) }, { "col_hidden", ImVec4(0.80f, 0.45f, 0.95f, 1.f) },
    { "col_aim", ImVec4(1.f, 0.35f, 0.85f, 1.f) },      { "col_sel", ImVec4(1.f, 1.f, 1.f, 1.f) },
    { "col_outline", ImVec4(0.f, 0.f, 0.f, 0.6f) },     { "col_text", ImVec4(0.95f, 0.95f, 0.97f, 1.f) },
    { "col_hp", ImVec4(0.31f, 0.86f, 0.43f, 1.f) },
    { "col_evidence", ImVec4(0.31f, 0.86f, 1.f, 1.f) }, { "col_weapon", ImVec4(0.75f, 0.75f, 0.80f, 0.9f) },
    { "col_body", ImVec4(0.75f, 0.43f, 1.f, 1.f) },     { "col_trap", ImVec4(1.f, 0.59f, 0.16f, 1.f) },
    { "col_bomb", ImVec4(1.f, 0.24f, 0.24f, 1.f) },     { "col_radar_border", ImVec4(1.f, 1.f, 1.f, 0.12f) },
    { "col_objective", ImVec4(1.f, 0.85f, 0.20f, 1.f) },
};
std::map<std::string, ImVec4> g_colors;
std::string g_espCustom;   // saved custom ESP look: id=on/val/mode;... and colours

ImVec4 ColorDefault(const std::string& id) {
    for (const ColorDef& d : kColorDefs) if (id == d.id) return d.def;
    return ImVec4(1, 1, 1, 1);
}
ImVec4& ColorRef(const std::string& id) {
    auto it = g_colors.find(id);
    if (it == g_colors.end()) it = g_colors.emplace(id, ColorDefault(id)).first;
    return it->second;
}
bool IsColorEntry(const Entry& e) { return e.fmt && !strcmp(e.fmt, "#color"); }

Entry* FindEntry(const std::string& id) {
    for (auto& t : g_tabs)
        for (auto& e : t.entries)
            if (id == e.id) return &e;
    return nullptr;
}

Action* FindAction(const std::string& cmd) {
    for (auto& a : g_actions) if (cmd == a.cmd) return &a;
    return nullptr;
}

// ---------------------------------------------------------------------------------------------
// Keys
// ---------------------------------------------------------------------------------------------
std::string KeyName(unsigned vk) {
    if (!vk) return "None";
    switch (vk) {
        case VK_MBUTTON: return "Mouse 3";
        case VK_XBUTTON1: return "Mouse 4";
        case VK_XBUTTON2: return "Mouse 5";
    }
    if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    switch (vk) {   // keys that need the "extended" bit for the right name
        case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_DIVIDE: case VK_RCONTROL: case VK_RMENU:
            sc |= 0x100;
    }
    wchar_t buf[64] = {};
    if (GetKeyNameTextW((LONG)(sc << 16), buf, 64) <= 0) return "Key " + std::to_string(vk);
    char out[128] = {};
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, out, sizeof out, nullptr, nullptr);
    return out;
}

void StartCapture(unsigned* target, const std::string& what) {
    g_capture = target;
    g_captureWhat = what;
}

void MarkCfg() {
    g_cfgDirty = true;
    g_cfgChangedAt = GetTickCount();
}

// ---------------------------------------------------------------------------------------------
// Config file
// ---------------------------------------------------------------------------------------------
void SaveConfig() {
    std::ostringstream o;
    o << "# RonMenu settings, written automatically\n";
    o << "menu_key=" << g_menuKey << "\n" << "unload_key=" << g_unloadKey << "\n";
    // interface preferences live in ui.cfg (WriteUiConfig), gameplay settings here
    for (auto& t : g_tabs)
        for (auto& e : t.entries) {
            o << "e." << e.id << ".on=" << (e.on ? 1 : 0) << "\n";
            char b[64]; snprintf(b, sizeof b, "%.4f", e.val);
            o << "e." << e.id << ".val=" << b << "\n";
            o << "e." << e.id << ".mode=" << e.mode << "\n";
            o << "e." << e.id << ".bind=" << e.bind << "\n";
        }
    for (auto& a : g_actions) o << "a." << a.cmd << ".bind=" << a.bind << "\n";
    for (auto& kv : g_colors) {
        char b[96];
        snprintf(b, sizeof b, "%.3f,%.3f,%.3f,%.3f", kv.second.x, kv.second.y, kv.second.z, kv.second.w);
        o << "c." << kv.first << "=" << b << "\n";
    }
    if (!g_espCustom.empty()) o << "esp_custom=" << g_espCustom << "\n";
    // live entries that are not loaded right now keep their saved settings
    for (auto& kv : g_liveCfg) {
        const std::string& k = kv.first;
        size_t dot = k.rfind('.');
        std::string id = k.substr(2, dot - 2);
        bool present = k[0] == 'e' ? FindEntry(id) != nullptr : FindAction(id) != nullptr;
        if (!present) o << k << "=" << kv.second << "\n";
    }
    std::wstring tmp = g_cfgFile + L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        f << o.str();
    }
    MoveFileExW(tmp.c_str(), g_cfgFile.c_str(), MOVEFILE_REPLACE_EXISTING);

    // interface preferences: page, size, overlay HUD, animation filter
    std::wstring ui = g_modDir + L"\\ui.cfg", uiTmp = ui + L".tmp";
    {
        std::ofstream f(uiTmp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        char us[16];
        snprintf(us, sizeof us, "%.2f", g_uiScale);
        f << "# RonMenu interface preferences, written automatically\n"
          << "page=" << g_page << "\n" << "ui_scale=" << us << "\n" << "hud=" << (g_showHud ? 1 : 0) << "\n"
          << "anim_filter=" << g_animFilter << "\n";
        std::string favs;
        for (auto& id : g_favs) { if (!favs.empty()) favs += ","; favs += id; }
        f << "favs=" << favs << "\n";
    }
    MoveFileExW(uiTmp.c_str(), ui.c_str(), MOVEFILE_REPLACE_EXISTING);
}

void LoadConfigFile(const std::wstring& path, int& n);

void LoadConfig() {
    int n = 0;
    LoadConfigFile(g_cfgFile, n);              // gameplay (older versions also kept interface keys here)
    LoadConfigFile(g_modDir + L"\\ui.cfg", n);  // interface preferences win
    Log("config loaded (%d settings)", n);
}

void LoadConfigFile(const std::wstring& path, int& n) {
    std::ifstream f(path);
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        ++n;
        if (k.rfind("e.lv_", 0) == 0 || k.rfind("a.lv_", 0) == 0) { g_liveCfg[k] = v; continue; }   // live entries come later
        if (k == "menu_key") { unsigned x = (unsigned)strtoul(v.c_str(), nullptr, 10); if (x) g_menuKey = x; }
        else if (k == "unload_key") g_unloadKey = (unsigned)strtoul(v.c_str(), nullptr, 10);
        else if (k == "hud") g_showHud = v == "1";
        else if (k == "tab") { int t = atoi(v.c_str()); if (t >= 0 && t < (int)g_tabs.size()) g_tab = t; }
        else if (k == "anim_filter") snprintf(g_animFilter, sizeof g_animFilter, "%s", v.c_str());
        else if (k == "page") { int pg = atoi(v.c_str()); if (pg >= 0 && pg < (int)kPages.size()) g_page = pg; }
        else if (k == "favs") {
            g_favs.clear();
            std::istringstream in(v);
            for (std::string id; std::getline(in, id, ',');) if (!id.empty()) g_favs.insert(id);
        }
        else if (k == "ui_scale") { float f2 = (float)atof(v.c_str()); if (f2 >= 0.7f && f2 <= 1.6f) g_uiScale = f2; }
        else if (k.rfind("c.", 0) == 0) {
            ImVec4 c;
            if (sscanf_s(v.c_str(), "%f,%f,%f,%f", &c.x, &c.y, &c.z, &c.w) == 4) g_colors[k.substr(2)] = c;
        }
        else if (k == "esp_custom") g_espCustom = v;
        else if (k.rfind("e.", 0) == 0) {
            size_t dot = k.rfind('.');
            Entry* e = FindEntry(k.substr(2, dot - 2));
            if (!e) continue;
            std::string field = k.substr(dot + 1);
            if (field == "on") e->on = v == "1";
            else if (field == "val") e->val = (std::min)(e->hi, (std::max)(e->lo, (float)atof(v.c_str())));
            else if (field == "mode") e->mode = atoi(v.c_str());
            else if (field == "bind") e->bind = (unsigned)strtoul(v.c_str(), nullptr, 10);
        } else if (k.rfind("a.", 0) == 0) {
            size_t dot = k.rfind('.');
            if (Action* a = FindAction(k.substr(2, dot - 2))) a->bind = (unsigned)strtoul(v.c_str(), nullptr, 10);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// State file I/O
// ---------------------------------------------------------------------------------------------
void WriteState() {
    std::ostringstream o;
    o << "v=1\n";
    o << "alive=" << (long long)time(nullptr) << "\n";   // the Lua side switches everything off when this goes stale
    o << "cmd_seq=" << g_cmdSeq << "\n";
    o << "cmd=" << (g_cmd.empty() ? "none" : g_cmd) << "\n";
    o << "cmd_arg=" << g_cmdArg << "\n";
    o << "keys=" << g_keys << "\n";
    for (auto& t : g_tabs) {
        for (auto& e : t.entries) {
            if (!e.noCheck) o << e.id << "=" << (e.on ? 1 : 0) << "\n";
            if (e.hasSlider) {
                char b[64]; snprintf(b, sizeof b, "%.4f", e.val);
                o << e.id << "_val=" << b << "\n";
            }
            if (e.combo) o << e.id << "_mode=" << e.mode << "\n";
        }
    }
    std::wstring tmp = g_stateFile + L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        f << o.str();
    }
    MoveFileExW(tmp.c_str(), g_stateFile.c_str(), MOVEFILE_REPLACE_EXISTING);
}

void ReadStatus() {
    std::ifstream f(g_statusFile);
    if (!f) return;
    std::map<std::string, std::string> m;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        m[line.substr(0, eq)] = line.substr(eq + 1);
    }
    if (m.empty()) return;
    auto hb = m.find("heartbeat");
    if (hb != m.end()) g_heartbeat = atoll(hb->second.c_str());
    g_status = std::move(m);
}

std::string Lower(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// <game>\ReadyOrNot\Binaries\Win64\<exe>  ->  <game>
std::wstring GameRoot() {
    wchar_t env[MAX_PATH];
    if (GetEnvironmentVariableW(L"RONMENU_GAME_ROOT", env, MAX_PATH)) return env;   // used by the offline preview
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring p = exe;
    for (int i = 0; i < 4; ++i) {
        size_t cut = p.find_last_of(L"\\/");
        if (cut == std::wstring::npos) break;
        p.resize(cut);
    }
    return p;
}

void LoadCatalog() {
    std::ifstream f(GameRoot() + L"\\Manifest_UFSFiles_Win64.txt");
    if (!f) { g_catalogState = 3; Log("animation list: manifest not found"); return; }
    const std::string prefix = "ReadyOrNot/Content/";
    std::vector<AnimAsset> out, fx, snd;
    std::string line;
    while (std::getline(f, line)) {
        size_t tab = line.find('\t');
        std::string p = line.substr(0, tab);
        if (p.size() < 8 || p.compare(p.size() - 7, 7, ".uasset") != 0) continue;
        if (p.rfind(prefix, 0) != 0) continue;
        std::string rel = p.substr(prefix.size(), p.size() - prefix.size() - 7);
        std::string low = Lower(rel);
        size_t slash0 = rel.rfind('/');
        std::string nm = rel.substr(slash0 + 1);
        if (low.rfind("fmod/events/", 0) == 0) {   // sound
            snd.push_back({ "/Game/" + rel + "." + nm, nm, rel.substr(12, slash0 > 12 ? slash0 - 12 : 0), low });
            continue;
        }
        if ((nm.rfind("P_", 0) == 0 || nm.rfind("PS_", 0) == 0 || nm.rfind("NS_", 0) == 0) &&
            (low.find("vfx") != std::string::npos || low.find("particle") != std::string::npos || low.find("/fx/") != std::string::npos)) {
            fx.push_back({ "/Game/" + rel + "." + nm, nm, rel.substr(0, slash0), low });
            continue;
        }
        if (p.find("/Animations/") == std::string::npos) continue;
        if (low.find("lipsync") != std::string::npos || low.find("facial") != std::string::npos ||
            low.find("/face/") != std::string::npos) continue;
        size_t slash = rel.rfind('/');
        size_t ai = rel.find("Animations/");
        AnimAsset a;
        a.name = rel.substr(slash + 1);
        a.path = "/Game/" + rel + "." + a.name;
        a.folder = (ai != std::string::npos && slash > ai + 11) ? rel.substr(ai + 11, slash - (ai + 11)) : "";
        a.lower = low;
        out.push_back(std::move(a));
    }
    g_catalog = std::move(out);
    g_fx = std::move(fx);
    g_sounds = std::move(snd);
    g_catalogState = 2;
    Log("asset lists: %zu animations, %zu effects, %zu sounds", g_catalog.size(), g_fx.size(), g_sounds.size());
}

void StartCatalog() {
    int expected = 0;
    if (g_catalogState.compare_exchange_strong(expected, 1)) g_catalogThread = std::thread(LoadCatalog);
}

bool IsThirdPersonAnim(const AnimAsset& a) {
    return a.lower.find("thirdperson") != std::string::npos || a.lower.find("animations/tp/") != std::string::npos;
}

void RunSearch() {
    std::string q = Lower(g_animFilter);
    if (q == g_lastQuery && g_tpOnly == g_lastTpOnly) return;
    g_lastQuery = q;
    g_lastTpOnly = g_tpOnly;
    std::vector<std::string> words;
    std::istringstream in(q);
    for (std::string w; in >> w;) words.push_back(w);
    g_results.clear();
    g_resultTotal = 0;
    g_animSel = -1;
    for (int i = 0; i < (int)g_catalog.size(); ++i) {
        const AnimAsset& a = g_catalog[i];
        if (g_tpOnly && !IsThirdPersonAnim(a)) continue;
        bool all = true;
        for (const std::string& w : words) if (a.lower.find(w) == std::string::npos) { all = false; break; }
        if (!all) continue;
        ++g_resultTotal;
        if (g_results.size() < 500) g_results.push_back(i);
    }
}

// Pose presets: exact animation names first, then a keyword search over the whole list.
struct Preset { const char* cmd; std::vector<const char*> names; std::vector<const char*> words; const char* loadedWord; };
const std::vector<Preset> kPresets = {
    { "anim_crouch",    { "MOB1_Crouch_Idle", "W2_Crouch_Idle_v2" }, { "crouch", "idle" }, "crouch" },
    { "anim_surrender", { "SCR_Gunpoint_On_Knees_Hands_On_Head_Loop" }, { "knees", "loop" }, "surrender" },
    { "anim_handsup",   { "SCR_Hands_Up_Hostage_Loop" }, { "hands_up", "loop" }, "hands" },
    { "anim_cuffed",    { "tp_npc_arrested_wiggle_loop" }, { "arrested", "loop" }, "arrested" },
    { "anim_sit",       { "POHQ_Doc_Sitting_Idle_A_Morrison", "tp_npc_story_gas_sit_middle" }, { "sitting", "idle" }, "sit" },
    { "anim_relaxed",   { "MIL2_M3_W2_Stand_Relaxed_Idle" }, { "relaxed", "idle" }, "relaxed" },
};

const Preset* FindPreset(const char* cmd) {
    for (const Preset& p : kPresets) if (!strcmp(p.cmd, cmd)) return &p;
    return nullptr;
}

std::string ResolvePreset(const Preset& p) {
    for (const char* n : p.names) {
        std::string ln = Lower(n);
        for (const AnimAsset& a : g_catalog) if (Lower(a.name) == ln) return a.path;
    }
    for (const AnimAsset& a : g_catalog) {
        bool all = true;
        for (const char* w : p.words) if (a.lower.find(w) == std::string::npos) { all = false; break; }
        if (all && a.lower.find("montage") == std::string::npos) return a.path;
    }
    return "";
}

bool LuaAlive() {
    return g_heartbeat != 0 && llabs((long long)time(nullptr) - g_heartbeat) <= 4;
}

void Command(const char* name, const std::string& arg = "") {
    g_cmd = name;
    g_cmdArg = arg;
    for (char& c : g_cmdArg) if (c == '\n' || c == '\r') c = ' ';
    ++g_cmdSeq;
    g_dirty = true;
}

// Runs a button / bound action, filling in its argument from the related setting.
void ApplyEspPreset(const std::string& which);

void RunAction(const char* cmd) {
    if (!strncmp(cmd, "esp_preset_", 11)) { ApplyEspPreset(cmd + 11); return; }   // handled here, not by Lua
    std::string arg;
    if (!strcmp(cmd, "spawn_suspects")) {
        Entry* e = FindEntry("spawn_count");
        arg = std::to_string((int)((e ? e->val : 3.f) + 0.5f));
    } else if (!strcmp(cmd, "end_mission")) {
        static const char* grades[] = { "S", "A", "B", "C", "D", "F" };
        Entry* e = FindEntry("end_grade");
        arg = grades[(e && e->mode >= 0 && e->mode < 6) ? e->mode : 0];
    } else if (!strcmp(cmd, "anim_play")) {
        if (g_catalogState != 2 || g_animSel < 0 || g_animSel >= (int)g_results.size()) return;
        arg = g_catalog[g_results[g_animSel]].path;
    } else if (const Preset* p = FindPreset(cmd)) {
        if (g_catalogState != 2) {
            // list still loading: remember the request, Tick() sends it when ready
            StartCatalog();
            g_pendingPreset = cmd;
            return;
        }
        arg = ResolvePreset(*p);
        if (arg.empty()) { Command("anim_quick", p->loadedWord); return; }
        cmd = "anim_play";
    }
    Command(cmd, arg);
}

bool GameFocused() {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

// Movement keys for flying, only while the game has focus and the menu is closed.
void PollFlyKeys() {
    Entry* fly = FindEntry("fly");
    unsigned keys = 0;
    if (fly && fly->on && !g_open && GameFocused()) {
        auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
        if (down('W')) keys |= 1;
        if (down('S')) keys |= 2;
        if (down('A')) keys |= 4;
        if (down('D')) keys |= 8;
        if (down(VK_SPACE)) keys |= 16;
        if (down(VK_CONTROL)) keys |= 32;
    }
    if (keys != g_keys) { g_keys = keys; g_dirty = true; }
}

// ---------------------------------------------------------------------------------------------
// Widgets
// ---------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------
// Widgets: plain text, flat buttons, no cards. Feature rows are drawn in Feature() below.
// ---------------------------------------------------------------------------------------------
float ToggleWidth() { return ImGui::GetFrameHeight(); }

// Plain checkbox, used on the settings pages. Feature rows show ON / OFF text instead.
bool Toggle(const char* id, bool* v) { return ImGui::Checkbox(id, v); }

// A key as a plain button. Left click starts capturing a new key, right click clears it.
// Without a bind target it is only text.
void KeyBadge(const char* id, unsigned* bind, const std::string& what, const char* fixedText = nullptr) {
    bool capturing = (bind && g_capture == bind);
    std::string text = fixedText ? fixedText : capturing ? "press a key..." : (bind && *bind ? KeyName(*bind) : "bind");
    ImGui::PushID(id);
    if (!bind) {
        ImGui::TextUnformatted(text.c_str());
    } else {
        if (ImGui::Button(text.c_str())) StartCapture(bind, what);
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { *bind = 0; MarkCfg(); if (g_capture == bind) g_capture = nullptr; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click to bind a key (Esc cancels, Backspace clears). Right click clears.");
    }
    ImGui::PopID();
}

struct StatusInfo { ImVec4 col; const char* label; std::string detail; };

StatusInfo Classify(const std::string& d) {
    StatusInfo s{ kGray, "off", d };
    if (d.rfind("ok", 0) == 0)           { s.col = kGreen;  s.label = "working"; }
    else if (d.rfind("none", 0) == 0)    { s.col = kRed;    s.label = "not available"; }
    else if (d.rfind("warn", 0) == 0)    { s.col = kYellow; s.label = "partly working"; }
    else if (d.rfind("waiting", 0) == 0) { s.col = kYellow; s.label = "waiting"; }
    return s;
}

StatusInfo GetStatus(const Entry& e) {
    if (!strcmp(e.id, "stretch")) return Classify(stretch::Status());   // done by the DLL itself
    StatusInfo s{ kGray, "waiting for the Lua mod", "" };
    if (!LuaAlive()) return s;
    s.label = "no report yet";
    auto it = g_status.find(e.id);
    if (it == g_status.end()) return s;
    StatusInfo c = Classify(it->second);
    if (c.col.x == kGray.x && it->second.rfind("off", 0) != 0) { c.col = kYellow; c.label = "see details"; }
    return c;
}

// Selectable button: lighter while it is the chosen one.
bool Chip(const char* label, bool sel, const ImVec2& size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Button, sel ? ImVec4(0.32f, 0.33f, 0.36f, 1.f) : ImVec4(0.16f, 0.16f, 0.17f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.26f, 0.26f, 0.27f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.34f, 0.34f, 0.36f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Text, sel ? ImVec4(1.f, 1.f, 1.f, 1.f) : ImVec4(0.78f, 0.78f, 0.80f, 1.f));
    bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return r;
}

// Section title: bold, normal capitals, no rule under it.
void Heading(const char* text, bool = false) {
    ImGui::Dummy(ImVec2(0, 6.f * U()));
    ImGui::PushFont(g_fontBold);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
}

void Note(const char* text) {
    ImGui::PushFont(g_fontSmall);
    ImGui::PushTextWrapPos(0.f);
    ImGui::TextColored(kGray, "%s", text);
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
}

void Wrapped(const char* text, const ImVec4& col, ImFont* f = nullptr) {
    ImGui::PushFont(f ? f : g_fontSmall);
    ImGui::PushTextWrapPos(0.f);
    ImGui::TextColored(col, "%s", text);
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
}

// Button; danger = red label. The tip shows on hover.
bool StyledButton(const char* label, const ImVec2& size, bool danger, const char* tip = nullptr) {
    if (danger) ImGui::PushStyleColor(ImGuiCol_Text, kRed);
    bool r = ImGui::Button(label, size);
    if (danger) ImGui::PopStyleColor();
    if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    return r;
}

// Search jumps to a page and highlights the row it found for a moment.
std::string g_flash;
DWORD g_flashAt = 0;
bool g_flashScroll = false;

float FlashAlpha(const char* id) {
    if (g_flash.empty() || g_flash != id) return 0.f;
    float a = 1.f - (float)(int)(GetTickCount() - g_flashAt) / 2600.f;
    if (a <= 0.f) { g_flash.clear(); return 0.f; }
    return (std::min)(a, 1.f);
}

void FlashScroll(const char* id) {
    if (g_flashScroll && g_flash == id) { ImGui::SetScrollHereY(0.25f); g_flashScroll = false; }
}

void FlashRect(const char* id, const ImVec2& a, const ImVec2& b) {
    float al = FlashAlpha(id);
    if (al <= 0.f) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, b, Col(kAccent, 0.10f * al), kR * U());
    dl->AddRect(a, b, Col(kAccent, 0.85f * al), kR * U(), 0, 1.5f * U());
}

// Action buttons, two per row; a bound key shows in the label.
void Actions(const std::vector<const char*>& cmds) {
    std::vector<Action*> list;
    for (const char* c : cmds) if (Action* a = FindAction(c)) list.push_back(a);
    float h = ImGui::GetFrameHeight() + 2.f * U();
    for (size_t i = 0; i < list.size(); ++i) {
        Action* a = list[i];
        bool last = (i + 1 == list.size());
        bool left = (i % 2 == 0);
        float w = (left && !last) ? (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f : -FLT_MIN;
        char label[160];
        if (a->bind) snprintf(label, sizeof label, "%s  [%s]###%s", a->label, KeyName(a->bind).c_str(), a->cmd);
        else snprintf(label, sizeof label, "%s###%s", a->label, a->cmd);
        FlashScroll(a->cmd);
        if (StyledButton(label, ImVec2(w, h), a->danger)) RunAction(a->cmd);
        FlashRect(a->cmd, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) StartCapture(&a->bind, a->label);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s%sRight click: bind a key%s", a->tip ? a->tip : "", a->tip ? "\n" : "",
                              a->bind ? " (Backspace in capture clears it)" : "");
        if (left && !last) ImGui::SameLine();
    }
}

int g_animTarget = 0;   // 0 me, 1 suspects, 2 civilians, 3 everyone
const char* kTargets[] = { "me", "suspects", "civilians", "everyone" };

void PlayAnimPath(const std::string& path) {
    if (path.empty()) return;
    if (g_animTarget == 0) { Command("anim_play", path); return; }
    Command("lv_fun_anim", path + "|" + kTargets[g_animTarget]);
}

void StopAnims() {
    if (g_animTarget == 0) Command("anim_stop");
    else Command("lv_fun_anim_stop");
}

// Next small button on the same line, or a new line if it would not fit.
void FlowNext(const char* label, bool first) {
    if (first) return;
    float w = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.f;
    ImGui::SameLine(0, 4.f * U());
    if (ImGui::GetContentRegionAvail().x < w) ImGui::NewLine();   // respects table columns
}

bool Chips(const char* id, const char* const* items, int n, int* cur) {
    bool changed = false;
    ImGui::PushID(id);
    for (int i = 0; i < n; ++i) {
        FlowNext(items[i], i == 0);
        ImGui::PushID(i);
        if (Chip(items[i], *cur == i)) { *cur = i; changed = true; }
        ImGui::PopID();
    }
    ImGui::PopID();
    return changed;
}

void AnimBrowser() {
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kGray, "play on");
    ImGui::SameLine();
    Chips("target", kTargets, 4, &g_animTarget);
    Heading("POSES");
    {
        static const char* poseCmd[] = { "anim_crouch", "anim_surrender", "anim_handsup", "anim_cuffed", "anim_sit", "anim_relaxed" };
        static const char* poseName[] = { "crouch", "kneel", "hands up", "cuffed", "sit", "relaxed" };
        for (int i = 0; i < 6; ++i) {
            FlowNext(poseName[i], i == 0);
            if (ImGui::Button(poseName[i])) {
                if (g_animTarget == 0) RunAction(poseCmd[i]);
                else if (g_catalogState == 2) if (const Preset* pr = FindPreset(poseCmd[i])) PlayAnimPath(ResolvePreset(*pr));
            }
        }
        FlowNext("stop", false);
        if (ImGui::Button("stop")) StopAnims();
    }

    Heading("ALL ANIMATIONS");
    StartCatalog();
    int state = g_catalogState;
    if (state == 1) { Note("Reading the game's animation list..."); return; }
    if (state == 3) { Note("Could not read Manifest_UFSFiles_Win64.txt from the game folder."); return; }

    float avail = ImGui::GetContentRegionAvail().x;
    float tw = ToggleWidth();
    float lw = ImGui::CalcTextSize("Third-person only").x;
    ImGui::SetNextItemWidth(avail - tw - lw - ImGui::GetStyle().ItemSpacing.x * 3);
    if (ImGui::InputTextWithHint("##filter", "search: crouch idle, surrender loop, sit, wave...", g_animFilter, sizeof g_animFilter))
        MarkCfg();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Third-person only");
    ImGui::SameLine();
    Toggle("##tponly", &g_tpOnly);
    RunSearch();

    ImGui::PushFont(g_fontSmall);
    ImGui::TextColored(kGray, "%zu matches of %zu animations%s", g_resultTotal, g_catalog.size(),
                       g_resultTotal > g_results.size() ? " (first 500 shown)" : "");
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_FrameBg, kCard);
    if (ImGui::BeginListBox("##anims", ImVec2(-FLT_MIN, ImGui::GetTextLineHeightWithSpacing() * 9.f))) {
        ImGuiListClipper clip;
        clip.Begin((int)g_results.size());
        while (clip.Step()) {
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                const AnimAsset& a = g_catalog[g_results[i]];
                ImGui::PushID(i);
                if (ImGui::Selectable(a.name.c_str(), g_animSel == i, ImGuiSelectableFlags_AllowDoubleClick)) {
                    g_animSel = i;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        if (g_animTarget == 0) RunAction("anim_play"); else PlayAnimPath(a.path);
                    }
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", a.folder.c_str());
                ImGui::PopID();
            }
        }
        ImGui::EndListBox();
    }
    ImGui::PopStyleColor();
    Note("Double-click to play. Animations made for other body types may look stretched or not play at all.");
    if (ImGui::Button("play selected") && g_animSel >= 0 && g_animSel < (int)g_results.size()) {
        if (g_animTarget == 0) RunAction("anim_play"); else PlayAnimPath(g_catalog[g_results[g_animSel]].path);
    }
    ImGui::SameLine();
    if (ImGui::Button("stop")) StopAnims();
}

// Buttons of each section, in groups. Buttons from live mods join the section they name.
struct ActionGroup { const char* title; std::vector<const char*> cmds; };
std::vector<ActionGroup> TabActionGroupsBase(int tab);

std::map<std::string, int> g_liveActionTab;   // live button id -> tab it belongs to (-1 = hidden)

std::vector<ActionGroup> TabActionGroups(int tab) {
    std::vector<ActionGroup> out = TabActionGroupsBase(tab);
    ActionGroup extra{ tab == TAB_LIVE ? "LIVE BUTTONS" : "EXTRA", {} };
    for (auto& a : g_actions) {
        if (strncmp(a.cmd, "lv_", 3)) continue;
        auto it = g_liveActionTab.find(a.cmd);
        if ((it != g_liveActionTab.end() ? it->second : TAB_LIVE) == tab) extra.cmds.push_back(a.cmd);
    }
    if (!extra.cmds.empty()) out.push_back(extra);
    return out;
}

std::vector<ActionGroup> TabActionGroupsBase(int tab) {
    switch (tab) {
        case TAB_PLAYER:  return {};   // the Heal button was removed from the source
        case TAB_MOVEMENT: return { { "POSITION", { "tp_crosshair", "save_pos", "load_pos" } } };
        case TAB_WEAPON:  return { { "AMMO", { "refill_ammo" } },
                                   { "DROP", { "drop_gun", "drop_gun_evidence", "drop_all_guns", "drop_everything" } } };
        case TAB_DOORS:   return { { "DOORS", { "door_unlock_look", "door_lock_look", "unlock_doors", "keycards", "elocks_off" } } };
        case TAB_AI:      return { { "SUSPECTS", { "surrender_suspects", "arrest_suspects", "report_suspects", "kill_suspects", "drop_weapons" } },
                                   { "CIVILIANS", { "surrender_civilians", "arrest_civilians", "report_civilians" } },
                                   { "SANDBOX", { "spawn_suspects", "tp_next_ai", "ai_hide", "ai_unhide", "hostages", "flush_bodies", "reset_ai" } } };
        case TAB_MISSION: return { { "MISSION", { "secure_evidence", "complete_objectives", "end_mission" } } };
        case TAB_ESP_LOOK: return { { "PRESETS", { "esp_preset_clean", "esp_preset_detailed", "esp_preset_friends",
                                                   "esp_preset_save", "esp_preset_load", "esp_preset_reset" } } };
    }
    return {};
}

int TabOfAction(const char* cmd) {
    if (!strncmp(cmd, "anim_", 5)) return TAB_CAMERA;
    for (int t = 0; t < (int)g_tabs.size(); ++t)
        for (auto& g : TabActionGroups(t))
            for (const char* c : g.cmds) if (!strcmp(c, cmd)) return t;
    return -1;
}

void BindRow(const char* label, unsigned* bind) {
    ImGui::PushID(bind);
    float x = ImGui::GetCursorScreenPos().x;   // screen space: SameLine(x) is column-relative in tables
    float avail = ImGui::GetContentRegionAvail().x;
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(*bind ? kText : ImVec4(kText.x, kText.y, kText.z, 0.75f), "%s", label);
    ImGui::SameLine();
    ImGui::SetCursorScreenPos(ImVec2(x + avail * 0.68f, ImGui::GetCursorScreenPos().y));
    KeyBadge("##b", bind, label);
    ImGui::PopID();
}

void DrawBindsTab() {
    {   // keys bound to more than one thing
        std::map<unsigned, std::vector<std::string>> by;
        if (g_menuKey) by[g_menuKey].push_back("Open / close menu");
        if (g_unloadKey) by[g_unloadKey].push_back("Unload RonMenu");
        for (auto& t : g_tabs) for (auto& e : t.entries) if (!e.noCheck && e.bind) by[e.bind].push_back(e.label);
        for (auto& a : g_actions) if (a.bind) by[a.bind].push_back(a.label);
        bool any = false;
        for (auto& kv : by) {
            if (kv.second.size() < 2) continue;
            if (!any) { Heading("Keys bound twice", true); any = true; }
            std::string line = KeyName(kv.first) + ":  ";
            for (size_t i = 0; i < kv.second.size(); ++i) line += (i ? ", " : "") + kv.second[i];
            Note(line.c_str());
        }
        if (any) Note("Pressing such a key triggers all of them. Click a key below to change it.");
    }
    Note("Click a key to change it, then press the new key (keyboard or mouse 3/4/5). "
         "Esc cancels, Backspace clears, right click clears. Everything is saved automatically.");
    ImGui::Dummy(ImVec2(0, 4.f * U()));
    ImGui::SetNextItemWidth(260.f * U());
    ImGui::InputTextWithHint("##bindq", "Filter keys...", g_bindFilter, sizeof g_bindFilter);
    ImGui::SameLine(0, 18.f * U());
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Only bound keys");
    ImGui::SameLine();
    Toggle("##onlybound", &g_bindOnlyBound);
    std::string q = Lower(g_bindFilter);
    auto shown = [&](const char* label, unsigned bind) {
        if (g_bindOnlyBound && !bind) return false;
        return q.empty() || Lower(label).find(q) != std::string::npos;
    };
    ImGui::Dummy(ImVec2(0, 4.f * U()));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.f * U(), 2.f * U()));
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(14.f * U(), 0));
    if (ImGui::BeginTable("binds", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableNextColumn();
        Heading("Menu", true);
        if (shown("Open / close menu", g_menuKey)) BindRow("Open / close menu", &g_menuKey);
        if (shown("Unload RonMenu", g_unloadKey)) BindRow("Unload RonMenu", &g_unloadKey);
        // features by page, then buttons by page; the column switch happens halfway
        int col = 0, rows = 0, total = 0;
        for (auto& pg : kPages) for (int t : pg.tabs) for (auto& e : g_tabs[t].entries)
            if (!e.noCheck && !e.noStatus && !e.parent && shown(e.label, e.bind)) ++total;
        for (auto& pg : kPages) {
            bool head = false;
            for (int t : pg.tabs)
                for (auto& e : g_tabs[t].entries) {
                    if (e.noCheck || e.noStatus || e.parent || !shown(e.label, e.bind)) continue;
                    if (col == 0 && rows > total / 2) { ImGui::TableNextColumn(); col = 1; head = false; }
                    if (!head) { Heading(pg.name, true); head = true; }
                    BindRow(e.label, &e.bind);
                    ++rows;
                }
        }
        if (col == 0) ImGui::TableNextColumn();
        bool head = false;
        for (auto& a : g_actions) {
            auto it = g_liveActionTab.find(a.cmd);
            if (it != g_liveActionTab.end() && it->second < 0) continue;   // internal commands
            if (!shown(a.label, a.bind)) continue;
            if (!head) { Heading("Buttons", true); head = true; }
            BindRow(a.label, &a.bind);
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar(2);
}

void DrawDebugTab() {
    Heading("Interface", true);
    {
        static const char* names[] = { "85%", "100%", "115%", "130%", "150%" };
        static const float vals[] = { 0.85f, 1.f, 1.15f, 1.3f, 1.5f };
        int cur = 1;
        for (int i = 0; i < 5; ++i) if (fabsf(g_uiScale - vals[i]) < 0.01f) cur = i;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Interface size");
        ImGui::SameLine(170.f * U());
        if (Chips("uiscale", names, 5, &cur)) { g_uiScale = vals[cur]; MarkCfg(); }
        Note("Text and controls are rebuilt at the new size right away. The base size follows your game resolution.");
    }
    ImGui::Dummy(ImVec2(0, 4.f * U()));
    Heading("Connection", true);
    ImGui::PushFont(g_fontBold);
    ImGui::TextUnformatted("Lua mod");
    ImGui::PopFont();
    ImGui::SameLine();
    if (LuaAlive()) ImGui::TextColored(kGreen, "connected");
    else ImGui::TextColored(kRed, "not connected");
    Note(LuaAlive()
        ? "A feature that is on shows the Lua mod's status under its name. Green = applied, yellow = partly / waiting, red = nothing to apply it to."
        : "The Lua mod is not responding. Restart the game: UE4SS only starts it at launch (and its CTRL+R reload can hang it).");
    ImGui::Dummy(ImVec2(0, 2.f * U()));
    float rowX = ImGui::GetCursorPosX(), avail = ImGui::GetContentRegionAvail().x;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Show active features on screen");
    ImGui::SameLine(rowX + avail - ToggleWidth());
    if (Toggle("##hud", &g_showHud)) MarkCfg();

    float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    float h = ImGui::GetFrameHeight();
    Heading("Dumps", true);
    if (StyledButton("Dump player + items", ImVec2(half, h), false)) Command("dump_player");
    ImGui::SameLine();
    if (StyledButton("Full class dump (slow)", ImVec2(-FLT_MIN, h), false)) Command("dump_classes");

    Heading("Saving and reset", true);
    Note("Everything you change (features, sliders, methods, binds) is saved to ronmenu.cfg and restored on the next inject.");
    if (StyledButton("Turn all features off", ImVec2(half, h), false)) TurnAllOff();
    ImGui::SameLine();
    if (StyledButton("Reset everything to defaults", ImVec2(-FLT_MIN, h), true)) {
        int tab = g_tab;
        BuildEntries();
        g_tab = tab;
        g_dirty = true;
        MarkCfg();
    }
    if (StyledButton("Unload RonMenu", ImVec2(-FLT_MIN, h), true)) g_unload = true;
}

int ActiveCount(const TabDef& t) {
    int n = 0;
    for (auto& e : t.entries) if (!e.noCheck && !e.noStatus && e.on) ++n;
    return n;
}

void TurnAllOff() {
    for (auto& t : g_tabs) for (auto& e : t.entries) if (!e.noCheck && !e.noStatus) e.on = false;
    g_dirty = true;
    MarkCfg();
}

// ------------------------------- header: name, Lua status, search, two buttons -----------------
void DrawHeader() {
    ImGui::PushFont(g_fontBold);
    ImGui::TextUnformatted("RonMenu");
    ImGui::PopFont();
    float right = ImGui::GetWindowContentRegionMax().x;
    float sw = (std::min)(240.f * U(), (std::max)(120.f * U(), right * 0.3f));
    ImGui::SameLine();
    ImGui::SetCursorPosX(right - sw);
    ImGui::SetNextItemWidth(sw);
    ImGui::InputTextWithHint("##search", "Search", g_search, sizeof g_search);
    ImGui::Separator();
}

int PageOfTab(int tab) {
    for (int p = 0; p < (int)kPages.size(); ++p)
        for (int t : kPages[p].tabs) if (t == tab) return p;
    return 0;
}

int PageOfKind(PageKind k) {
    for (int p = 0; p < (int)kPages.size(); ++p) if (kPages[p].kind == k) return p;
    return 0;
}

int TotalActiveCount() {
    int n = 0;
    for (auto& t : g_tabs) n += ActiveCount(t);
    return n;
}

int PageActiveCount(const Page& pg) {
    int n = 0;
    for (int t : pg.tabs) n += ActiveCount(g_tabs[t]);
    return n;
}

void GoTo(int page, const std::string& flash) {
    g_page = page;
    g_search[0] = 0;
    g_flash = flash;
    g_flashAt = GetTickCount();
    g_flashScroll = !flash.empty();
    MarkCfg();
}

const char* kSep = "  \xC2\xB7  ";                    // middle dot

// ------------------------------- sidebar: the pages -------------------------------------------
void DrawSidebar(float height) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 1.f * U()));
    ImGui::BeginChild("sidebar", ImVec2(180.f * U(), height), false);
    int firstFeature = PageOfKind(PK_FAVS) + 1, firstSystem = PageOfKind(PK_BINDS);
    for (int p = 0; p < (int)kPages.size(); ++p) {
        const Page& pg = kPages[p];
        if (p == firstFeature || p == firstSystem) ImGui::Dummy(ImVec2(0, 10.f * U()));
        char label[96];
        snprintf(label, sizeof label, "%s###page%d", pg.name, p);
        bool sel = p == g_page && !g_search[0];
        if (ImGui::Selectable(label, sel, 0, ImVec2(0, ImGui::GetFrameHeight()))) GoTo(p, "");
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
}

// ------------------------------- helpers ------------------------------------------------------
std::vector<Entry*> ChildrenIn(int tab, const char* parent) {
    std::vector<Entry*> out;
    if (tab < 0 || tab >= (int)g_tabs.size()) return out;
    for (auto& e : g_tabs[tab].entries) if (e.parent && !strcmp(e.parent, parent)) out.push_back(&e);
    return out;
}

bool ParentIn(int tab, const char* parent) {
    for (auto& e : g_tabs[tab].entries) if (!strcmp(e.id, parent)) return true;
    return false;
}

// Shown as a row of its own in its tab. Settings of the animations and of the buttons have their
// own places; a setting whose feature lives in another tab stays here rather than disappearing.
bool TopLevel(const Entry& e, int tab) {
    if (!e.parent) return true;
    if (e.parent[0] == '@') return false;
    return !ParentIn(tab, e.parent);
}

const ModeInfo* FindMode(const char* id) {
    for (const ModeInfo& m : kModes) if (!strcmp(m.id, id)) return &m;
    return nullptr;
}

std::string FormatVal(const Entry& e, float v) {
    char b[64];
    snprintf(b, sizeof b, (e.fmt && *e.fmt) ? e.fmt : "%.2f", v);
    return b;
}

// shortens text with "..." so it fits maxW
std::string Fit(ImFont* f, const std::string& s, float maxW) {
    if (f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.f, s.c_str()).x <= maxW) return s;
    std::string t = s;
    while (!t.empty() && f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.f, (t + "...").c_str()).x > maxW) t.pop_back();
    return t + "...";
}

// quiet uppercase label at the top of a column (Players page)
void ColumnTitle(int, const char* text) {
    ImGui::PushFont(g_fontSmall);
    std::string t = text;
    for (char& c : t) c = (char)toupper((unsigned char)c);
    ImGui::TextColored(kGray, "%s", t.c_str());
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 2.f * U()));
}

int CountBind(unsigned vk) {
    if (!vk) return 0;
    int n = (vk == g_menuKey) + (vk == g_unloadKey);
    for (auto& t : g_tabs) for (auto& e : t.entries) if (!e.noCheck && e.bind == vk) ++n;
    for (auto& a : g_actions) if (a.bind == vk) ++n;
    return n;
}

void Changed() { g_dirty = true; MarkCfg(); }

void PagePlayers(float height);
void FunEffects(float rows);
void FunSounds(float rows);
bool FunBackendLoaded();

// next item on this line at an absolute screen x (SameLine(x) is column-relative inside tables)
void SameLineAt(float screenX) {
    ImGui::SameLine();
    ImGui::SetCursorScreenPos(ImVec2(screenX, ImGui::GetCursorScreenPos().y));
}

int TabOfEntry(const Entry* p) {
    for (int t = 0; t < (int)g_tabs.size(); ++t)
        for (auto& e : g_tabs[t].entries) if (&e == p) return t;
    return -1;
}

// small coloured word next to a heading, e.g. "Host required"
void Tag(const char* text, const ImVec4& col) {
    ImGui::SameLine(0, 8.f * U());
    ImGui::TextColored(col, "%s", text);
}

// ------------------------------- feature rows -------------------------------------------------
// The live status the Lua mod reports, in words. Only shown while the feature is on.
void StatusLine(const Entry& e) {
    if (e.noCheck || e.noStatus || !e.on) return;
    StatusInfo s = GetStatus(e);
    if (s.col.x == kGreen.x) return;   // working: nothing to say
    std::string d = s.detail;
    for (const char* pre : { "ok", "warn", "none", "waiting" }) {
        size_t n = strlen(pre);
        if (d.compare(0, n, pre) == 0 && (d.size() == n || d[n] == ':' || d[n] == ' ')) {
            d = d.substr(n);
            while (!d.empty() && (d[0] == ':' || d[0] == ' ')) d.erase(0, 1);
            break;
        }
    }
    std::string line = s.label;
    if (!d.empty()) line += ": " + d;
    Wrapped(line.c_str(), s.col);
    if (!LuaAlive() && strcmp(e.id, "stretch"))
        Wrapped("The Lua mod is not answering: restart the game (it only starts with the game), then inject again.", kYellow);
    else if (s.col.x == kRed.x)
        Wrapped(e.combo ? "Nothing to apply it to yet. Most features need you in a mission; if you are, try another method."
                        : "Nothing to apply it to yet. Most features need you in a mission.", kGray);
}

// Stretch presets: fit the screen's aspect ratio (or back to native)
void StretchPresets(Entry& e) {
    ImGuiIO& io = ImGui::GetIO();
    float screen = io.DisplaySize.y > 0 ? io.DisplaySize.x / io.DisplaySize.y : 16.f / 9.f;
    struct P { const char* name; float aspect; };
    const P presets[] = { { "16:10", 16.f / 10.f }, { "4:3", 4.f / 3.f }, { "5:4", 5.f / 4.f } };
    float bw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 3.f) / 4.f;
    for (const P& pr : presets) {
        if (ImGui::Button(pr.name, ImVec2(bw, 0))) { e.val = (std::max)(e.lo, (std::min)(e.hi, screen / pr.aspect)); e.on = true; Changed(); }
        ImGui::SameLine();
    }
    if (ImGui::Button("Native", ImVec2(bw, 0))) { e.on = false; e.val = 1.f; Changed(); }
}

// label, then a slider that takes the rest of the row (Ctrl+click types a number).
// A switch's own slider is labelled "Amount" so it does not repeat the switch's name.
void SliderSetting(Entry& e, const char* label = nullptr) {
    float x0 = ImGui::GetCursorPosX(), w = ImGui::GetContentRegionAvail().x;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label ? label : e.label);
    if (ImGui::IsItemHovered() && e.tip && *e.tip) ImGui::SetTooltip("%s", e.tip);
    ImGui::SameLine();
    ImGui::SetCursorPosX(x0 + (std::min)(w * 0.42f, 220.f * U()));
    ImGui::SetNextItemWidth(220.f * U());
    if (ImGui::SliderFloat("##v", &e.val, e.lo, e.hi, (e.fmt && *e.fmt) ? e.fmt : "%.2f")) Changed();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Drag, or Ctrl+click to type a number.\nRange %s to %s, default %s.", FormatVal(e, e.lo).c_str(),
                          FormatVal(e, e.hi).c_str(), FormatVal(e, e.def).c_str());
}

// label, then a drop-down of the methods; what the chosen one does is written underneath.
// A switch's own method list is labelled "Method".
void ComboSetting(Entry& e, const char* label = nullptr) {
    float x0 = ImGui::GetCursorPosX(), w = ImGui::GetContentRegionAvail().x;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label ? label : e.label);
    if (ImGui::IsItemHovered() && e.tip && *e.tip) ImGui::SetTooltip("%s", e.tip);
    ImGui::SameLine();
    ImGui::SetCursorPosX(x0 + (std::min)(w * 0.42f, 220.f * U()));
    ImGui::SetNextItemWidth(220.f * U());
    if (ImGui::Combo("##m", &e.mode, e.combo)) Changed();
    if (ImGui::IsItemHovered())
        if (const ModeInfo* m = FindMode(e.id))
            if (e.mode >= 0 && e.mode < (int)m->help.size() && m->help[e.mode] && *m->help[e.mode]) ImGui::SetTooltip("%s", m->help[e.mode]);
}

// label and a colour swatch (click for the picker)
void ColorSetting(Entry& k) {
    ImVec4& c = ColorRef(k.id);
    float x0 = ImGui::GetCursorPosX(), w = ImGui::GetContentRegionAvail().x;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(k.label);
    ImGui::SameLine();
    ImGui::SetCursorPosX(x0 + (std::min)(w * 0.42f, 220.f * U()));
    if (ImGui::ColorEdit4("##c", &c.x, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoLabel))
        Changed();
}

// The row of a switch: the name on the left, ON / OFF, the key and "pinned" on the right.
// Returns 1 for a left click (flips it), 2 for a right click (pins it to Favourites), 0 otherwise.
int SwitchRow(const Entry& e) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetFrameHeight();
    ImGui::InvisibleButton("##row", ImVec2(w, h));
    bool hov = ImGui::IsItemHovered();
    int r = ImGui::IsItemClicked(ImGuiMouseButton_Left) ? 1 : ImGui::IsItemClicked(ImGuiMouseButton_Right) ? 2 : 0;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hov) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(255, 255, 255, 14));
    float ty = p.y + (h - ImGui::GetTextLineHeight()) * 0.5f;
    dl->AddText(ImVec2(p.x + 6.f * U(), ty), e.on ? IM_COL32(248, 248, 248, 255) : IM_COL32(178, 178, 184, 255), e.label);
    float right = p.x + w - 6.f * U();
    const char* st = e.on ? "ON" : "OFF";
    right -= ImGui::CalcTextSize(st).x;
    dl->AddText(ImVec2(right, ty), e.on ? IM_COL32(120, 228, 150, 255) : IM_COL32(120, 120, 128, 255), st);
    if (e.bind) {
        std::string k = KeyName(e.bind);
        right -= ImGui::CalcTextSize(k.c_str()).x + 14.f * U();
        dl->AddText(ImVec2(right, ty), IM_COL32(150, 150, 156, 255), k.c_str());
    }
    if (g_favs.count(e.id)) {
        const char* pin = "pinned";
        right -= ImGui::CalcTextSize(pin).x + 14.f * U();
        dl->AddText(ImVec2(right, ty), IM_COL32(120, 120, 128, 255), pin);
    }
    if (hov) {
        const char* act = g_favs.count(e.id) ? "remove from" : "pin to";
        if (e.tip && *e.tip) ImGui::SetTooltip("%s\nRight-click: %s Favourites", e.tip, act);
        else ImGui::SetTooltip("Right-click: %s Favourites", act);
    }
    return r;
}

// One feature: its row (name, ON / OFF). Switched on, its status and settings open underneath;
// switched off it is the one line. Settings without a switch are always shown where they sit.
void Feature(Entry& e, int tab) {
    ImGui::PushID(e.id);
    FlashScroll(e.id);
    ImVec2 top = ImGui::GetCursorScreenPos();
    float width = ImGui::GetContentRegionAvail().x;
    if (IsColorEntry(e)) {
        ColorSetting(e);
    } else if (e.noCheck) {
        if (e.hasSlider) SliderSetting(e);
        else if (e.combo) ComboSetting(e);
        else ImGui::TextUnformatted(e.label);
    } else {
        int click = SwitchRow(e);
        if (click == 1) { e.on = !e.on; Changed(); }
        if (click == 2) { if (g_favs.count(e.id)) g_favs.erase(e.id); else g_favs.insert(e.id); MarkCfg(); }
        if (e.on) {
            ImGui::Indent(16.f * U());
            StatusLine(e);
            if (CountBind(e.bind) > 1) Wrapped("This key is also bound to something else; pressing it triggers both.", kYellow);
            if (e.hasSlider) SliderSetting(e, "Amount");
            if (e.combo) ComboSetting(e, "Method");
            if (!strcmp(e.id, "stretch")) StretchPresets(e);
            for (Entry* k : ChildrenIn(tab, e.id)) Feature(*k, tab);
            ImGui::Unindent(16.f * U());
            ImGui::Dummy(ImVec2(0, 4.f * U()));
        }
    }
    FlashRect(e.id, top, ImVec2(top.x + width, ImGui::GetCursorScreenPos().y));
    ImGui::PopID();
}

// Everything of one tab: its switches (with their settings), the settings for the buttons, the buttons.
void TabBody(int t) {
    if (t == TAB_ESP_LOOK) {   // live preview with sample people
        float w = ImGui::GetContentRegionAvail().x, h = 170.f * U();
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        esp::DrawPreview(ImGui::GetWindowDrawList(), p0, ImVec2(p0.x + w, p0.y + h));
        ImGui::Dummy(ImVec2(w, h));
        Note("Preview with sample people (suspect = aim target, friend = selected player). "
             "Colour order: aim target > selected player > down > cuffed > surrendered > hidden > type.");
        ImGui::Dummy(ImVec2(0, 6.f * U()));
    }
    int shown = 0;
    for (auto& e : g_tabs[t].entries)
        if (TopLevel(e, t)) { Feature(e, t); ++shown; }

    std::vector<Entry*> acts = ChildrenIn(t, "@actions");
    if (!acts.empty()) {
        Heading("Settings for the buttons below");
        for (Entry* k : acts) Feature(*k, t);
    }
    auto groups = TabActionGroups(t);
    for (int i = 0; i < (int)groups.size(); ++i) {
        std::string title = groups[i].title;
        if (Lower(title) == Lower(g_tabs[t].name)) title = "Actions";
        Heading(title.c_str());
        Actions(groups[i].cmds);
        if (t == TAB_AI && i == 0) Note("These use the game's own developer functions; the bottom line shows what was called.");
    }
    if (shown == 0 && groups.empty() && acts.empty())
        Wrapped(t == TAB_LIVE ? (LuaAlive() ? "Nothing here yet. Features added while the game runs (live mods) show up here."
                                            : "Waiting for the Lua mod.")
                              : t == TAB_FUN ? (FunBackendLoaded() ? "No chaos switches yet." : "Appear here once the live mod 'fun' is loaded (see above).")
                                             : "Nothing here.", kGray);
}

// A page with several tabs names each tab; a single-tab page has only its page title.
void DrawTabs(const std::vector<int>& tabs) {
    for (size_t i = 0; i < tabs.size(); ++i) {
        int t = tabs[i];
        if (tabs.size() > 1) {
            if (i) ImGui::Dummy(ImVec2(0, 14.f * U()));
            ImGui::PushFont(g_fontBold);
            ImGui::TextUnformatted(g_tabs[t].name);
            ImGui::PopFont();
            ImGui::Dummy(ImVec2(0, 2.f * U()));
        }
        TabBody(t);
    }
}

// Active / Favourites: the chosen features with their real controls, under their tab names
void DrawPicked(bool favs) {
    if (!favs && TotalActiveCount() > 0 && StyledButton("Turn all of these off", ImVec2(0, 0), false)) TurnAllOff();
    if (!favs && TotalActiveCount() > 0) ImGui::Dummy(ImVec2(0, 4.f * U()));
    bool any = false;
    for (int t = 0; t < (int)g_tabs.size(); ++t) {
        bool head = false;
        for (auto& e : g_tabs[t].entries) {
            if (!TopLevel(e, t)) continue;
            bool want = favs ? g_favs.count(e.id) > 0 : (e.on && !e.noCheck && !e.noStatus);
            if (!want) continue;
            if (!head) {
                if (any) ImGui::Dummy(ImVec2(0, 8.f * U()));
                ImGui::PushFont(g_fontBold);
                ImGui::TextUnformatted(g_tabs[t].name);
                ImGui::PopFont();
                head = true;
            }
            Feature(e, t);
            any = true;
        }
    }
    if (!any)
        Wrapped(favs ? "Nothing pinned yet. Right-click a feature's name on any page to pin it here."
                     : "Nothing is switched on.", kGray);
}

// ------------------------------- search: results jump to the row -----------------------------
struct Hit { std::string label, where, flash; int page; };

std::vector<Hit> SearchHits() {
    std::vector<Hit> out;
    std::string q = Lower(g_search);
    int animPage = PageOfKind(PK_ANIMS), fxPage = PageOfKind(PK_EFFECTS), plPage = PageOfKind(PK_PLAYERS);
    for (int p = 0; p < (int)kPages.size(); ++p)
        if (Lower(std::string(kPages[p].name) + " " + kPages[p].desc).find(q) != std::string::npos)
            out.push_back({ kPages[p].name, std::string("Page") + kSep + kPages[p].desc, "", p });
    for (int t = 0; t < (int)g_tabs.size(); ++t)
        for (auto& e : g_tabs[t].entries) {
            std::string hay = Lower(std::string(e.label) + " " + (e.tip ? e.tip : "") + " " + e.id);
            if (hay.find(q) == std::string::npos) continue;
            bool anim = e.parent && !strcmp(e.parent, "@anim");
            int page = anim ? animPage : PageOfTab(t);
            std::string where = std::string(kPages[page].name) + kSep + g_tabs[t].name;
            if (e.parent && e.parent[0] != '@')
                if (Entry* par = FindEntry(e.parent)) where += kSep + std::string(par->label);
            out.push_back({ e.label, where, e.id, page });
        }
    for (auto& a : g_actions) {
        if (Lower(std::string(a.label) + " " + a.cmd + " " + (a.tip ? a.tip : "")).find(q) == std::string::npos) continue;
        int t = TabOfAction(a.cmd);
        if (t < 0) continue;
        int page = !strncmp(a.cmd, "anim_", 5) ? animPage : t == TAB_FUN ? fxPage : PageOfTab(t);
        out.push_back({ a.label, std::string(kPages[page].name) + kSep + "button", a.cmd, page });
    }
    for (const char* w : { "teleport", "bring", "heal", "ragdoll", "launch", "ammo", "magazine", "give", "item", "formation", "gravity", "size", "head" })
        if (std::string(w).find(q) != std::string::npos || q.find(w) != std::string::npos) {
            out.push_back({ "Players page", std::string("Players") + kSep + "per-player buttons (" + w + ")", "", plPage });
            break;
        }
    return out;
}

void DrawSearch() {
    std::vector<Hit> hits = SearchHits();
    ImGui::PushFont(g_fontBold);
    ImGui::Text("Results for \"%s\"", g_search);
    ImGui::PopFont();
    Note(hits.empty() ? "Nothing matches. Try a shorter word." : "Click a result to open its page; the row lights up.");
    ImGui::Dummy(ImVec2(0, 6.f * U()));
    for (size_t i = 0; i < hits.size(); ++i) {
        const Hit& h = hits[i];
        ImGui::PushID((int)i);
        float w = ImGui::GetContentRegionAvail().x;
        if (ImGui::Selectable(h.label.c_str(), false, 0, ImVec2(w, 0))) GoTo(h.page, h.flash);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", h.where.c_str());
        ImGui::PopID();
    }
}

// ------------------------------- the page area -----------------------------------------------
void PageTitle(const Page& pg) {
    ImGui::PushFont(g_fontTitle);
    ImGui::TextUnformatted(pg.name);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 6.f * U()));
}

void DrawPage(float height) {
    const Page& pg = kPages[g_page];
    if (pg.kind == PK_PLAYERS && !g_search[0]) { PagePlayers(height); return; }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.f * U(), 8.f * U()));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.f * U(), 6.f * U()));
    ImGui::BeginChild("page", ImVec2(0, height), ImGuiChildFlags_AlwaysUseWindowPadding);
    if (g_search[0]) {
        DrawSearch();
    } else {
        PageTitle(pg);
        switch (pg.kind) {
            case PK_FEATURES: DrawTabs(pg.tabs); break;
            case PK_ANIMS:
                if (ImGui::BeginTable("anim", 2, ImGuiTableFlags_SizingStretchProp)) {
                    ImGui::TableSetupColumn("a", ImGuiTableColumnFlags_WidthStretch, 1.6f);
                    ImGui::TableSetupColumn("b", ImGuiTableColumnFlags_WidthStretch, 1.f);
                    ImGui::TableNextColumn();
                    AnimBrowser();
                    ImGui::TableNextColumn();
                    Heading("Playback");
                    for (Entry* k : ChildrenIn(TAB_CAMERA, "@anim")) Feature(*k, TAB_CAMERA);
                    ImGui::EndTable();
                }
                break;
            case PK_EFFECTS: {
                StartCatalog();
                if (!FunBackendLoaded())
                    Wrapped("Spawn, Play and the chaos switches need the live mod 'fun', which is not loaded: restart the game once. "
                            "You can still browse and search the lists.", kYellow);
                if (g_catalogState == 1) Wrapped("Reading the game's asset list...", kGray);
                // lists take the space left on screen (the chaos switches follow below)
                float rows = (ImGui::GetContentRegionAvail().y - 230.f * U()) / ImGui::GetTextLineHeightWithSpacing();
                rows = (std::max)(8.f, (std::min)(30.f, rows));
                if (g_catalogState == 2 && ImGui::BeginTable("fx", 2, ImGuiTableFlags_SizingStretchSame)) {
                    ImGui::TableNextColumn();
                    Heading("Particle effects");
                    FunEffects(rows);
                    ImGui::TableNextColumn();
                    Heading("Sounds");
                    FunSounds(rows);
                    ImGui::EndTable();
                }
                ImGui::Dummy(ImVec2(0, 10.f * U()));
                DrawTabs(pg.tabs);
                break;
            }
            case PK_BINDS: DrawBindsTab(); break;
            case PK_SETTINGS: DrawDebugTab(); break;
            case PK_ACTIVE: DrawPicked(false); break;
            case PK_FAVS: DrawPicked(true); break;
            default: break;
        }
    }
    ImGui::Dummy(ImVec2(0, 10.f * U()));
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
}

// ------------------------------- footer: keys, and the last result -----------------------------
void DrawFooter() {
    ImGui::Separator();
    std::string msg;
    auto res = g_status.find("cmd_result");
    if (g_capture) msg = "Press a key for '" + g_captureWhat + "'  (Esc cancels, Backspace clears)";
    else if (res != g_status.end() && !res->second.empty()) msg = res->second;
    ImGui::TextColored(kGray, "%s", Fit(ImGui::GetFont(), msg.empty() ? KeyName(g_menuKey) + " closes the menu" : msg,
                                        ImGui::GetWindowContentRegionMax().x - ImGui::GetCursorPosX()).c_str());
    // faint credit in the bottom-right corner of the menu
    {
        const char* wm = "github.com/MilleniumRat";
        ImVec2 ts = ImGui::CalcTextSize(wm);
        ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
        ImGui::GetWindowDrawList()->AddText(ImVec2(wp.x + ws.x - ts.x - 10.f * U(), wp.y + ws.y - ts.y - 6.f * U()),
                                            IM_COL32(255, 255, 255, 46), wm);
    }
}

// ------------------------------- HUD: plain list of what is on, shown while the menu is closed ---
void DrawHud() {
    std::vector<const Entry*> active;
    for (auto& t : g_tabs)
        for (auto& e : t.entries)
            if (!e.noCheck && !e.noStatus && e.on) active.push_back(&e);

    ImGui::SetNextWindowPos(ImVec2(12.f * U(), 12.f * U()));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.f, 0.f, 0.f, 0.60f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.f * U(), 8.f * U()));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.f * U(), 2.f * U()));
    ImGui::Begin("##hud", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextColored(kGray, "RonMenu  (%s)", KeyName(g_menuKey).c_str());
    for (auto* e : active) ImGui::TextUnformatted(e->label);

    // live numbers from the Lua mod
    if (LuaAlive()) {
        auto it = g_status.find("hud_counts");
        int c[8] = {};
        if (it != g_status.end() && sscanf_s(it->second.c_str(), "%d,%d,%d,%d,%d,%d,%d,%d",
                                             &c[0], &c[1], &c[2], &c[3], &c[4], &c[5], &c[6], &c[7]) == 8) {
            char b[128];
            snprintf(b, sizeof b, "Suspects  %d up  %d surr.  %d cuffed  %d down", c[0], c[1], c[2], c[3]);
            ImGui::TextUnformatted(b);
            snprintf(b, sizeof b, "Civilians  %d up  %d compl.  %d cuffed  %d down", c[4], c[5], c[6], c[7]);
            ImGui::TextUnformatted(b);
        }
        it = g_status.find("hud_score");
        if (it != g_status.end()) {
            std::string s = "Score  " + it->second;
            ImGui::TextUnformatted(s.c_str());
        }
        it = g_status.find("hud_bomb");
        if (it != g_status.end()) {
            int secs = atoi(it->second.c_str());
            char b[64];
            snprintf(b, sizeof b, "Bomb  %d:%02d", secs / 60, secs % 60);
            ImGui::TextUnformatted(b);
        }
    }
    ImGui::TextColored(ImVec4(1.f, 1.f, 1.f, 0.18f), "github.com/MilleniumRat");
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
}

// ---------------------------------------------------------------------------------------------
// FUN window: opens next to the main menu. Animations, effects, sounds and chaos toggles.
// ---------------------------------------------------------------------------------------------
bool g_funOpen = true;   // declared extern near the top
int g_funTab = 0;
ImVec2 g_mainPos, g_mainSize;

struct Browser { char filter[64] = ""; std::string lastQ = "\x01"; std::vector<int> results; size_t total = 0; int sel = -1; };
Browser g_fxB, g_sndB;
int g_fxWhere = 0, g_sndWhere = 0;
float g_fxScale = 1.f;

// search box + clipped list over one catalog; returns true on double-click
bool AssetList(const char* id, Browser& b, const std::vector<AnimAsset>& cat, const char* hint, float rows) {
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint((std::string("##q") + id).c_str(), hint, b.filter, sizeof b.filter);
    std::string q = Lower(b.filter);
    if (q != b.lastQ) {
        b.lastQ = q;
        std::vector<std::string> words;
        std::istringstream in(q);
        for (std::string w; in >> w;) words.push_back(w);
        b.results.clear();
        b.total = 0;
        b.sel = -1;
        for (int i = 0; i < (int)cat.size(); ++i) {
            bool all = true;
            for (auto& w : words) if (cat[i].lower.find(w) == std::string::npos) { all = false; break; }
            if (!all) continue;
            ++b.total;
            if (b.results.size() < 800) b.results.push_back(i);
        }
    }
    ImGui::PushFont(g_fontSmall);
    ImGui::TextColored(kGray, "%zu of %zu", b.total, cat.size());
    ImGui::PopFont();
    bool dbl = false;
    ImGui::PushStyleColor(ImGuiCol_FrameBg, kCard);
    if (ImGui::BeginListBox((std::string("##l") + id).c_str(), ImVec2(-FLT_MIN, ImGui::GetTextLineHeightWithSpacing() * rows))) {
        ImGuiListClipper clip;
        clip.Begin((int)b.results.size());
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                const AnimAsset& a = cat[b.results[i]];
                ImGui::PushID(i);
                if (ImGui::Selectable(a.name.c_str(), b.sel == i, ImGuiSelectableFlags_AllowDoubleClick)) {
                    b.sel = i;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) dbl = true;
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", a.folder.c_str());
                ImGui::PopID();
            }
        ImGui::EndListBox();
    }
    ImGui::PopStyleColor();
    return dbl;
}

const AnimAsset* Selected(const Browser& b, const std::vector<AnimAsset>& cat) {
    return (b.sel >= 0 && b.sel < (int)b.results.size()) ? &cat[b.results[b.sel]] : nullptr;
}

bool FunBackendLoaded() { return FindAction("lv_fun_fx") != nullptr; }

// ---------------------------------------------------------------- players tab (backend: live mod "playground")
struct RosterRow { std::string id, name; bool pawn = false, me = false, bot = false; };
struct ItemRow { std::string path, label; };
int g_pgSel = -1;               // index into the roster
int g_pgTarget = 0;             // 0 selected, 1 everyone, 2 me
int g_pgSide = 0, g_pgForm = 0, g_pgWeapon = 0, g_pgItem = -1, g_pgEnv = 0;
float g_pgDist = 150.f, g_pgSpacing = 120.f, g_pgFlop = 3.f, g_pgUp = 900.f, g_pgFwd = 0.f, g_pgRounds = 1.f, g_pgCount = 1.f;
char g_pgFilter[48] = "";

std::vector<std::string> SplitOn(const std::string& s, char c) {
    std::vector<std::string> out;
    size_t a = 0;
    if (s.empty()) return out;
    for (;;) {
        size_t b = s.find(c, a);
        out.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    return out;
}

std::string StatusOf(const char* key) {
    auto it = g_status.find(key);
    return it != g_status.end() ? it->second : "";
}

std::vector<RosterRow> Roster() {
    std::vector<RosterRow> out;
    for (auto& e : SplitOn(StatusOf("pg_roster"), ';')) {
        auto f = SplitOn(e, '|');
        if (f.size() < 5) continue;
        RosterRow r;
        r.id = f[0]; r.name = f[1]; r.pawn = f[2] == "1"; r.me = f[3] == "1"; r.bot = f[4] == "1";
        out.push_back(r);
    }
    return out;
}

std::vector<ItemRow> ItemClasses() {
    std::vector<ItemRow> out;
    for (auto& e : SplitOn(StatusOf("pg_items"), ';')) {
        auto f = SplitOn(e, '|');
        if (f.size() >= 2) out.push_back({ f[0], f[1] });
    }
    return out;
}

void PgCmd(const char* cmd, const std::string& target, const std::string& rest = "") {
    Command(cmd, target + (rest.empty() ? "" : "|" + rest));
}

std::string Fmt(float v) { char b[32]; snprintf(b, sizeof b, "%.0f", v); return b; }

// Players page: the roster stays visible on the left, every button names who it affects.
std::string ShortName(const std::string& n) {
    if (n.size() <= 16) return n;
    size_t cut = n.rfind(' ', 16);   // whole words when possible
    return (cut != std::string::npos && cut > 4) ? n.substr(0, cut) : n.substr(0, 14) + "..";
}

// next button on the line (wraps); greyed out when it can't work
bool PBtn(const std::string& label, bool enabled, bool first = false) {
    FlowNext(label.c_str(), first);
    ImGui::BeginDisabled(!enabled);
    bool r = ImGui::Button(label.c_str());
    ImGui::EndDisabled();
    return r;
}

// a labelled slider on one line: "Distance   [------o------ 150 cm]"
void PSlider(const char* label, const char* id, float* v, float lo, float hi, const char* fmt) {
    float x0 = ImGui::GetCursorScreenPos().x, w = ImGui::GetContentRegionAvail().x;
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImVec4(kText.x, kText.y, kText.z, 0.85f), "%s", label);
    SameLineAt(x0 + (std::min)(w * 0.36f, 150.f * U()));
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::SliderFloat(id, v, lo, hi, fmt);
}

void PChips(const char* label, const char* id, const char* const* items, int n, int* cur) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImVec4(kText.x, kText.y, kText.z, 0.85f), "%s", label);
    ImGui::SameLine();
    Chips(id, items, n, cur);
}

void PSection(const char* title, const char* tag, const ImVec4& tagCol) {
    ImGui::Dummy(ImVec2(0, 4.f * U()));
    ImGui::PushFont(g_fontBold);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    if (tag) Tag(tag, tagCol);
    ImGui::Dummy(ImVec2(0, 2.f * U()));
}

int g_pgAmmoOp = 0;

void PagePlayers(float height) {
    if (!FindAction("lv_pg_tp_to")) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22.f * U(), 16.f * U()));
        ImGui::BeginChild("page", ImVec2(0, height), ImGuiChildFlags_AlwaysUseWindowPadding);
        PageTitle(kPages[g_page]);
        Wrapped("The players backend (live mod 'playground') is not loaded: restart the game once.", kYellow);
        ImGui::EndChild();
        ImGui::PopStyleVar();
        return;
    }
    std::string auth = StatusOf("pg_auth");
    bool host = auth == "host" || auth == "solo";
    std::vector<RosterRow> roster = Roster();
    if (g_pgSel >= (int)roster.size()) g_pgSel = -1;
    if (g_pgSel < 0) for (int i = 0; i < (int)roster.size(); ++i) if (!roster[i].me) { g_pgSel = i; break; }
    if (g_pgSel < 0 && !roster.empty()) g_pgSel = 0;
    const RosterRow* sel = g_pgSel >= 0 ? &roster[g_pgSel] : nullptr;

    // ---- roster
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.063f, 0.069f, 0.080f, 1.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.f * U(), 16.f * U()));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.f * U(), 2.f * U()));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.f);
    ImGui::BeginChild("roster", ImVec2(230.f * U(), height), ImGuiChildFlags_AlwaysUseWindowPadding);
    ColumnTitle(0, "Players in session");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##pgq", "Filter players", g_pgFilter, sizeof g_pgFilter);
    ImGui::Dummy(ImVec2(0, 4.f * U()));
    std::string q = Lower(g_pgFilter);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int i = 0; i < (int)roster.size(); ++i) {
        const RosterRow& r = roster[i];
        if (!q.empty() && Lower(r.name).find(q) == std::string::npos) continue;
        ImGui::PushID(i);
        ImVec2 p = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x, rh = 32.f * U();
        if (ImGui::InvisibleButton("##p", ImVec2(w, rh))) g_pgSel = i;
        bool hov = ImGui::IsItemHovered(), on = g_pgSel == i;
        if (on) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + rh), IM_COL32(255, 255, 255, 22));
        else if (hov) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + rh), IM_COL32(255, 255, 255, 10));
        float cy = p.y + rh * 0.5f;
        dl->AddCircleFilled(ImVec2(p.x + 14.f * U(), cy), 3.5f * U(), r.pawn ? Col(kGreen) : Col(kGray, 0.6f));
        const char* tag = r.me ? "you" : r.bot ? "bot" : !r.pawn ? "no character" : "";
        float tw = *tag ? g_fontSmall->CalcTextSizeA(g_fontSmall->FontSize, FLT_MAX, 0.f, tag).x + 8.f * U() : 0.f;
        dl->AddText(g_fontRegular, g_fontRegular->FontSize, ImVec2(p.x + 26.f * U(), cy - g_fontRegular->FontSize * 0.5f),
                    on ? Col(kText) : Col(kText, 0.8f), Fit(g_fontRegular, r.name, w - 34.f * U() - tw).c_str());
        if (*tag) dl->AddText(g_fontSmall, g_fontSmall->FontSize, ImVec2(p.x + w - tw, cy - g_fontSmall->FontSize * 0.5f), Col(kGray), tag);
        ImGui::PopID();
    }
    if (roster.empty()) Wrapped("Nobody found yet: join or start a session.", kGray);
    ImGui::Dummy(ImVec2(0, 10.f * U()));
    if (auth == "solo") Wrapped("Playing solo: every button works.", kGreen);
    else if (auth == "host") Wrapped("You are the host: every button works.", kGreen);
    else if (!auth.empty()) Wrapped("You are a client in someone else's game. Only the host can change other players: "
                                    "here only 'Teleport to', size, head and the world look work.", kYellow);
    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
    {
        ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(mx.x - 0.5f, mn.y), ImVec2(mx.x - 0.5f, mx.y), kLine);
    }
    ImGui::SameLine();

    // ---- controls
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22.f * U(), 16.f * U()));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.f * U(), 6.f * U()));
    ImGui::BeginChild("pctl", ImVec2(0, height), ImGuiChildFlags_AlwaysUseWindowPadding);

    std::string nm = sel ? (sel->me ? std::string("yourself") : ShortName(sel->name)) : std::string("nobody");
    ImGui::PushFont(g_fontTitle);
    ImGui::Text("Selected player: %s", sel ? sel->name.c_str() : "none");
    ImGui::PopFont();
    if (sel && sel->me) Tag("you", kAccent);
    if (sel && !sel->pawn) Tag("no character", kYellow);
    Wrapped("Plain buttons act on the selected player; 'everyone' buttons include you.", kGray);

    bool selOk = sel && sel->pawn;
    bool selMe = sel && sel->me;
    std::string tid = sel ? sel->id : "";
    if (!sel) Wrapped("Pick a player on the left.", kYellow);
    else if (!sel->pawn) Wrapped("This player has no character right now (dead, spectating or still loading).", kYellow);
    const char* hostTag = host ? nullptr : "Host required";

    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(14.f * U(), 0));
    ImGui::PushStyleColor(ImGuiCol_TableBorderLight, ImVec4(1, 1, 1, 0.05f));
    bool two = ImGui::GetContentRegionAvail().x > 540.f * U();
    if (ImGui::BeginTable("pcols", two ? 2 : 1, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableNextColumn();

        PSection("Position", hostTag, kYellow);
        static const char* sides[] = { "behind", "left", "right", "front" };
        PChips("Side", "pgside", sides, 4, &g_pgSide);
        PSlider("Distance", "##pgdist", &g_pgDist, 60.f, 600.f, "%.0f cm");
        if (PBtn("Teleport to", selOk && !selMe, true)) PgCmd("lv_pg_tp_to", tid, std::string(sides[g_pgSide]) + "|" + Fmt(g_pgDist));
        if (PBtn("Bring here", host && selOk && !selMe)) PgCmd("lv_pg_bring", tid, Fmt(g_pgSpacing));
        if (PBtn("Bring everyone here", host)) PgCmd("lv_pg_bring", "all", Fmt(g_pgSpacing));
        static const char* forms[] = { "line", "circle", "stairs" };
        PChips("Formation", "pgform", forms, 3, &g_pgForm);
        PSlider("Spacing", "##pgsp", &g_pgSpacing, 60.f, 400.f, "%.0f cm");
        if (PBtn(std::string("Put everyone in a ") + forms[g_pgForm], host, true)) PgCmd("lv_pg_form", "all", std::string(forms[g_pgForm]) + "|" + Fmt(g_pgSpacing));
        if (PBtn("Return to previous spot", selOk && (host || selMe))) PgCmd("lv_pg_return", tid);
        if (PBtn("Return everyone", host)) PgCmd("lv_pg_return", "all");

        PSection("Health & physics", hostTag, kYellow);
        if (PBtn("Heal", host && selOk, true)) PgCmd("lv_pg_heal", tid);
        if (PBtn("Heal everyone", host)) PgCmd("lv_pg_heal", "all");
        PSlider("Ragdoll time", "##pgflop", &g_pgFlop, 0.5f, 10.f, "%.1f s");
        char fl[16]; snprintf(fl, sizeof fl, "%.1f", g_pgFlop);
        if (PBtn("Ragdoll", host && selOk, true)) PgCmd("lv_pg_flop", tid, fl);
        if (PBtn("Ragdoll everyone", host)) PgCmd("lv_pg_flop", "all", fl);
        if (PBtn("Get up", host && selOk)) PgCmd("lv_pg_getup", tid);
        if (PBtn("Get everyone up", host)) PgCmd("lv_pg_getup", "all");
        PSlider("Launch up", "##pgup", &g_pgUp, 200.f, 3000.f, "%.0f");
        PSlider("Launch forward", "##pgfw", &g_pgFwd, -1500.f, 1500.f, "%.0f");
        if (PBtn("Launch", host && selOk, true)) PgCmd("lv_pg_launch", tid, Fmt(g_pgUp) + "|" + Fmt(g_pgFwd));
        if (PBtn("Launch everyone", host)) PgCmd("lv_pg_launch", "all", Fmt(g_pgUp) + "|" + Fmt(g_pgFwd));

        PSection("World look", "Local effect", kAccent);
        Wrapped("Fog and sunlight, only on your screen.", kGray);
        static const char* env[] = { "clear", "haze", "mist", "horror", "golden", "moon", "redalert" };
        Chips("pgenv", env, 7, &g_pgEnv);
        if (PBtn(std::string("Apply ") + env[g_pgEnv] + " look", true, true)) Command("lv_pg_env", env[g_pgEnv]);
        if (PBtn("Reset look", true)) Command("lv_pg_env", "reset");

        if (two) ImGui::TableNextColumn();

        PSection("Ammunition", hostTag, kYellow);
        Wrapped("Edits the real magazines; each has its own round count.", kGray);
        static const char* weapons[] = { "in hand", "primary", "sidearm" };
        static const char* wkey[] = { "held", "primary", "secondary" };
        PChips("Weapon", "pgweapon", weapons, 3, &g_pgWeapon);
        static const char* ops[] = { "set rounds", "empty magazine", "refill all", "no spare mags", "last bullet", "one magazine", "restore" };
        static const char* opKey[] = { "set", "empty", "refill", "nospares", "lastbullet", "onemag", "restore" };
        Chips("pgammoop", ops, 7, &g_pgAmmoOp);
        ImGui::BeginDisabled(g_pgAmmoOp != 0);
        PSlider("Rounds", "##pgrounds", &g_pgRounds, 0.f, 100.f, "%.0f");
        ImGui::EndDisabled();
        std::string arg = std::string(wkey[g_pgWeapon]) + "|" + opKey[g_pgAmmoOp] + (g_pgAmmoOp == 0 ? "|" + Fmt(g_pgRounds) : "");
        if (PBtn("Apply", host && selOk, true)) PgCmd("lv_pg_ammo", tid, arg);
        if (PBtn("Apply to everyone", host)) PgCmd("lv_pg_ammo", "all", arg);

        PSection("Equipment", hostTag, kYellow);
        std::vector<ItemRow> items = ItemClasses();
        if (g_pgItem >= (int)items.size()) g_pgItem = -1;
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImVec4(kText.x, kText.y, kText.z, 0.85f), "Item");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##pgitem", g_pgItem >= 0 ? items[g_pgItem].label.c_str() : "pick an item")) {
            for (int i = 0; i < (int)items.size(); ++i)
                if (ImGui::Selectable(items[i].label.c_str(), g_pgItem == i)) g_pgItem = i;
            ImGui::EndCombo();
        }
        PSlider("Count", "##pgcount", &g_pgCount, 1.f, 10.f, "%.0f");
        bool item = g_pgItem >= 0;
        if (item) {
            if (PBtn("Give", host && selOk, true)) PgCmd("lv_pg_give", tid, items[g_pgItem].path + "|" + Fmt(g_pgCount));
            if (PBtn("Give to everyone", host)) PgCmd("lv_pg_give", "all", items[g_pgItem].path + "|" + Fmt(g_pgCount));
            if (PBtn("Take", host && selOk)) PgCmd("lv_pg_take", tid, items[g_pgItem].path + "|" + Fmt(g_pgCount));
        } else {
            Wrapped("Pick an item first (the list holds item types found in your session).", kGray);
        }

        if (FindAction("lv_pgx_scale")) {
            static float scale = 1.6f, head = 2.5f, grav = 0.3f, gravSecs = 15.f, holdSecs = 5.f;
            static int leanDir = 0;
            PSection("Size & head", "Local effect", kAccent);
            Wrapped("Only you see this; the others' game is unchanged.", kGray);
            char b[48];
            PSlider("Size", "##pgxs", &scale, 0.2f, 4.f, "x%.2f");
            snprintf(b, sizeof b, "%.2f", scale);
            if (PBtn("Resize", selOk, true)) PgCmd("lv_pgx_scale", tid, b);
            if (PBtn("Resize everyone", true)) PgCmd("lv_pgx_scale", "all", b);
            if (PBtn("Normal size for everyone", true)) PgCmd("lv_pgx_scale", "all", "1");
            PSlider("Head gear", "##pgxh", &head, 0.3f, 5.f, "x%.1f");
            snprintf(b, sizeof b, "%.2f", head);
            if (PBtn("Big head", selOk, true)) PgCmd("lv_pgx_head", tid, b);
            if (PBtn("Normal heads", true)) PgCmd("lv_pgx_head", "all", "1");

            PSection("Experimental", hostTag ? hostTag : "Untested", hostTag ? kYellow : kGray);
            PSlider("Gravity", "##pgxg", &grav, 0.05f, 2.f, "x%.2f");
            PSlider("Gravity time", "##pgxgs", &gravSecs, 1.f, 120.f, "%.0f s");
            snprintf(b, sizeof b, "%.2f|%.0f", grav, gravSecs);
            if (PBtn("Set gravity", host && selOk, true)) PgCmd("lv_pgx_grav", tid, b);
            if (PBtn("Gravity on everyone", host)) PgCmd("lv_pgx_grav", "all", b);
            PSlider("Hold for", "##pgxhold", &holdSecs, 1.f, 60.f, "%.0f s");
            if (PBtn("Crouch", host && selOk, true)) PgCmd("lv_pgx_crouch", tid, "1|" + Fmt(holdSecs));
            if (PBtn("Stand up", host && selOk)) PgCmd("lv_pgx_crouch", tid, "0|1");
            static const char* lean[] = { "left", "right" };
            PChips("Lean", "pgxlean", lean, 2, &leanDir);
            if (PBtn(std::string("Lean ") + lean[leanDir], host && selOk, true)) PgCmd("lv_pgx_lean", tid, std::string(lean[leanDir]) + "|1|" + Fmt(holdSecs));
            if (PBtn("Stop leaning", host && selOk)) PgCmd("lv_pgx_lean", tid, "none|0|1");
            if (PBtn("Clear chamber", host && selOk)) PgCmd("lv_pgx_chamber", tid);
            if (PBtn("Reset all experimental", true, true)) Command("lv_pgx_reset");
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    ImGui::Dummy(ImVec2(0, 8.f * U()));
    std::string u = StatusOf("pg_undo");
    if (PBtn(u.empty() ? std::string("Undo last change (nothing yet)") : "Undo: " + u, !u.empty(), true)) Command("lv_pg_undo");
    ImGui::Dummy(ImVec2(0, 10.f * U()));
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
}

// Both library columns use the same rows (where / amount / list / selection / buttons) so they line up.
void LibRowLabel(const char* text) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kGray, "%s", text);
    ImGui::SameLine(80.f * U());
}

void LibSelection(const AnimAsset* a) {
    ImGui::PushFont(g_fontSmall);
    if (a) ImGui::TextColored(kText, "Selected: %s", Fit(g_fontSmall, a->name, ImGui::GetContentRegionAvail().x - 70.f * U()).c_str());
    else ImGui::TextColored(kGray, "Selected: nothing (click a row; double-click plays it)");
    ImGui::PopFont();
}

float g_sndVolume = 1.f;

void FunEffects(float rows) {
    static const char* where[] = { "crosshair", "me", "suspects", "everyone" };
    LibRowLabel("Spawn at");
    Chips("fxwhere", where, 4, &g_fxWhere);
    LibRowLabel("Size");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::SliderFloat("##fxscale", &g_fxScale, 0.25f, 5.f, "x%.2f");
    bool dbl = AssetList("fx", g_fxB, g_fx, "Search: fire, smoke, sparks, explosion, glass...", rows);
    const AnimAsset* a = Selected(g_fxB, g_fx);
    LibSelection(a);
    bool backend = FunBackendLoaded();
    char arg[32];
    snprintf(arg, sizeof arg, "|%s|%.2f", where[g_fxWhere], g_fxScale);
    ImGui::BeginDisabled(!backend || !a);
    if ((ImGui::Button("Spawn") || (dbl && backend)) && a) Command("lv_fun_fx", a->path + arg);
    ImGui::SameLine();
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!backend);
    if (ImGui::Button("Clear all effects")) Command("lv_fun_fx_clear");
    ImGui::EndDisabled();
}

void FunSounds(float rows) {
    static const char* where[] = { "me", "crosshair" };
    LibRowLabel("Play at");
    Chips("sndwhere", where, 2, &g_sndWhere);
    LibRowLabel("Volume");
    ImGui::SetNextItemWidth(-FLT_MIN);
    { float pct = g_sndVolume * 100.f; if (ImGui::SliderFloat("##sndvol", &pct, 0.f, 200.f, "%.0f %%")) g_sndVolume = pct / 100.f; }
    bool dbl = AssetList("snd", g_sndB, g_sounds, "Search: alarm, scream, siren, explode, music...", rows);
    const AnimAsset* a = Selected(g_sndB, g_sounds);
    LibSelection(a);
    bool backend = FunBackendLoaded();
    char vol[16];
    snprintf(vol, sizeof vol, "|%.2f", g_sndVolume);
    ImGui::BeginDisabled(!backend || !a);
    if ((ImGui::Button("Play") || (dbl && backend)) && a) Command("lv_fun_snd", a->path + "|" + where[g_sndWhere] + vol);
    ImGui::SameLine();
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!backend);
    if (ImGui::Button("Stop all sounds")) Command("lv_fun_snd_stop");
    ImGui::EndDisabled();
}

// ESP presets: only the look (Appearance + Information groups and colours) changes,
// except "friends", which also picks who is shown.
void SetE(const char* id, bool on, float val = -1.f, int mode = -1) {
    Entry* e = FindEntry(id);
    if (!e) return;
    e->on = on;
    if (val >= 0.f) e->val = (std::min)(e->hi, (std::max)(e->lo, val));
    if (mode >= 0) e->mode = mode;
}

void ResetLook() {
    for (int t : { (int)TAB_ESP_LOOK, (int)TAB_ESP_INFO })
        for (auto& e : g_tabs[t].entries) { e.val = e.def; e.mode = 0; e.on = false; }
    for (const char* id : { "box_outline", "lbl_name", "lbl_dist", "lbl_shadow", "hp_gradient", "arrow_dist" }) SetE(id, true);
    g_colors.clear();
}

void ApplyEspPreset(const std::string& which) {
    if (which == "reset") ResetLook();
    else if (which == "clean") {
        ResetLook();
        SetE("esp_style", false, -1, 1); SetE("box_thick", false, 1.5f); SetE("corner_len", false, 0.22f);
    } else if (which == "detailed") {
        ResetLook();
        SetE("esp_style", false, -1, 5); SetE("esp_health", true); SetE("hp_recent", true);
        SetE("lbl_health", true); SetE("lbl_state", true); SetE("lbl_weapon", true);
        SetE("head_marker", false, -1, 1); SetE("skel_joints", true); SetE("esp_arrows", true);
    } else if (which == "friends") {
        ResetLook();
        SetE("esp_suspects", false); SetE("esp_civ", false); SetE("esp_swat", true); SetE("esp_friends", true);
        SetE("esp_health", true); SetE("lbl_health", true); SetE("lbl_weapon", true); SetE("esp_arrows", true);
    } else if (which == "save") {
        std::ostringstream o;
        for (int t : { (int)TAB_ESP_LOOK, (int)TAB_ESP_INFO })
            for (auto& e : g_tabs[t].entries) o << e.id << "=" << (e.on ? 1 : 0) << "/" << e.val << "/" << e.mode << ";";
        for (auto& kv : g_colors) o << kv.first << "=c" << kv.second.x << "/" << kv.second.y << "/" << kv.second.z << "/" << kv.second.w << ";";
        g_espCustom = o.str();
        g_status["cmd_result"] = "ESP look saved as your preset";
    } else if (which == "load") {
        if (g_espCustom.empty()) { g_status["cmd_result"] = "No saved ESP preset yet"; return; }
        std::istringstream in(g_espCustom);
        for (std::string item; std::getline(in, item, ';');) {
            size_t eq = item.find('=');
            if (eq == std::string::npos) continue;
            std::string id = item.substr(0, eq), v = item.substr(eq + 1);
            if (!v.empty() && v[0] == 'c') {
                ImVec4 c;
                if (sscanf_s(v.c_str() + 1, "%f/%f/%f/%f", &c.x, &c.y, &c.z, &c.w) == 4) g_colors[id] = c;
            } else if (Entry* e = FindEntry(id)) {
                int on = 0, mode = 0;
                float val = e->def;
                if (sscanf_s(v.c_str(), "%d/%f/%d", &on, &val, &mode) == 3) { e->on = on != 0; e->val = val; e->mode = mode; }
            }
        }
        g_status["cmd_result"] = "ESP preset loaded";
    }
    if (which != "save" && which != "load") g_status["cmd_result"] = "ESP preset: " + which;
    g_dirty = true;
    MarkCfg();
}

}  // namespace

namespace menu {

unsigned Color(const char* id) { return ImGui::ColorConvertFloat4ToU32(ColorRef(id)); }

int SelectedPlayerId() {
    std::vector<RosterRow> r = Roster();
    if (g_pgSel < 0 || g_pgSel >= (int)r.size() || r[g_pgSel].me) return -1;
    return atoi(r[g_pgSel].id.c_str());
}

void Init(const std::wstring& modDir) {
    BuildEntries();
    g_modDir = modDir;
    g_stateFile = modDir + L"\\menu_state.txt";
    g_statusFile = modDir + L"\\status.txt";
    g_cfgFile = modDir + L"\\ronmenu.cfg";
    g_animsFile = modDir + L"\\anims.txt";
    LoadConfig();
    g_dirty = true;
    StartCatalog();   // read the animation list in the background right away
}

void Shutdown() {
    if (g_catalogThread.joinable()) g_catalogThread.join();
    // tell the Lua side right away that the menu is gone: it switches every feature off
    if (!g_stateFile.empty()) {
        std::wstring tmp = g_stateFile + L".tmp";
        { std::ofstream f(tmp, std::ios::binary | std::ios::trunc); f << "v=1\nalive=0\n"; }
        MoveFileExW(tmp.c_str(), g_stateFile.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    if (g_cfgDirty) SaveConfig();
}

void SetupStyle() {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowPadding = ImVec2(14, 12);
    s.FramePadding = ImVec2(8, 4);
    s.ItemSpacing = ImVec2(8, 5);
    s.ItemInnerSpacing = ImVec2(6, 4);
    s.WindowRounding = 0;
    s.ChildRounding = 0;
    s.FrameRounding = 0;
    s.PopupRounding = 0;
    s.GrabRounding = 0;
    s.TabRounding = 0;
    s.ScrollbarRounding = 0;
    s.GrabMinSize = 10;
    s.ScrollbarSize = 8;
    s.WindowBorderSize = 0;
    s.ChildBorderSize = 0;
    s.PopupBorderSize = 1;
    s.FrameBorderSize = 0;
    s.SeparatorTextBorderSize = 1;
    s.WindowMinSize = ImVec2(200, 120);

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text]                 = kText;
    c[ImGuiCol_TextDisabled]         = ImVec4(0.40f, 0.40f, 0.42f, 1.00f);
    c[ImGuiCol_WindowBg]             = ImVec4(0.f, 0.f, 0.f, 0.95f);     // black, 95% opaque
    c[ImGuiCol_ChildBg]              = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg]              = ImVec4(0.f, 0.f, 0.f, 0.97f);
    c[ImGuiCol_Border]               = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg]              = ImVec4(0.13f, 0.13f, 0.14f, 1.00f);
    c[ImGuiCol_FrameBgHovered]       = ImVec4(0.18f, 0.18f, 0.19f, 1.00f);
    c[ImGuiCol_FrameBgActive]        = ImVec4(0.22f, 0.22f, 0.23f, 1.00f);
    c[ImGuiCol_SliderGrab]           = ImVec4(0.72f, 0.74f, 0.78f, 1.00f);
    c[ImGuiCol_SliderGrabActive]     = ImVec4(0.90f, 0.91f, 0.94f, 1.00f);
    c[ImGuiCol_CheckMark]            = ImVec4(0.90f, 0.91f, 0.94f, 1.00f);
    c[ImGuiCol_Button]               = ImVec4(0.16f, 0.16f, 0.17f, 1.00f);
    c[ImGuiCol_ButtonHovered]        = ImVec4(0.24f, 0.24f, 0.25f, 1.00f);
    c[ImGuiCol_ButtonActive]         = ImVec4(0.32f, 0.32f, 0.33f, 1.00f);
    c[ImGuiCol_Header]               = ImVec4(0.20f, 0.20f, 0.21f, 1.00f);
    c[ImGuiCol_HeaderHovered]        = ImVec4(0.16f, 0.16f, 0.17f, 1.00f);
    c[ImGuiCol_HeaderActive]         = ImVec4(0.26f, 0.26f, 0.27f, 1.00f);
    c[ImGuiCol_Separator]            = ImVec4(1, 1, 1, 0.12f);
    c[ImGuiCol_ScrollbarBg]          = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(1, 1, 1, 0.12f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1, 1, 1, 0.20f);
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(1, 1, 1, 0.28f);
    c[ImGuiCol_ResizeGrip]           = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered]    = ImVec4(1, 1, 1, 0.20f);
    c[ImGuiCol_ResizeGripActive]     = ImVec4(1, 1, 1, 0.35f);
    c[ImGuiCol_TextSelectedBg]       = ImVec4(1, 1, 1, 0.25f);
    g_baseStyle = s;
    g_styleScale = 0.f;
}

void ApplyScale(float scale) {
    if (scale <= 0.f || scale == g_styleScale) return;
    g_styleScale = scale;
    ImGuiStyle st = g_baseStyle;
    st.ScaleAllSizes(scale);
    ImGui::GetStyle() = st;
}

void LoadFonts(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    io.FontGlobalScale = 1.f;
    char dir[MAX_PATH];
    GetWindowsDirectoryA(dir, MAX_PATH);
    bool segoe = true;
    auto load = [&](const char* file, float px) -> ImFont* {
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%s\\Fonts\\%s", dir, file);
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) return nullptr;
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        cfg.OversampleV = 2;
        return io.Fonts->AddFontFromFileTTF(path, px * scale, &cfg);
    };
    // Segoe UI (every Windows install has it); semibold for emphasis
    const char* mono = "segoeui.ttf";
    const char* monoBold = "seguisb.ttf";
    {
        char probe[MAX_PATH];
        snprintf(probe, sizeof probe, "%s\\Fonts\\seguisb.ttf", dir);
        if (GetFileAttributesA(probe) == INVALID_FILE_ATTRIBUTES) monoBold = "segoeuib.ttf";
    }
    g_fontRegular = load(mono, 15.f);
    if (!g_fontRegular) {
        segoe = false;
        ImFontConfig cfg;
        cfg.SizePixels = 13.f * scale;
        g_fontRegular = io.Fonts->AddFontDefault(&cfg);
    }
    g_fontSmall = load(mono, 13.f);
    g_fontBold = load(monoBold, 15.f);
    g_fontTitle = load(monoBold, 17.f);
    g_fontBig = load(monoBold, 26.f);
    if (!g_fontSmall) g_fontSmall = g_fontRegular;
    if (!g_fontBold) g_fontBold = g_fontRegular;
    if (!g_fontTitle) g_fontTitle = g_fontBold;
    if (!g_fontBig) g_fontBig = g_fontTitle;
    io.FontDefault = g_fontRegular;
    Log("fonts loaded at scale %.2f (%s)", scale, segoe ? mono : "built-in fallback");
}

#ifdef RONMENU_PREVIEW
void PreviewSelectTab(int tab) { if (tab >= 0 && tab < (int)kPages.size()) g_page = tab; }
void PreviewScrollBottom() { g_previewBottom = true; }
void PreviewSelect(const char* key, int) {
    g_flash = key;
    g_flashAt = GetTickCount() + 100000;   // stays lit in the still image
    g_flashScroll = true;
    if (Entry* e = FindEntry(key)) g_page = PageOfTab(TabOfEntry(e));
}
void PreviewSearch(const char* q) { snprintf(g_search, sizeof g_search, "%s", q); }
#endif

const std::wstring& ModDir() { return g_modDir; }
bool IsOpen() { return g_open; }
float UiScale() { return g_uiScale; }
void SetOpen(bool o) { g_open = o; }
bool UnloadRequested() { return g_unload; }
void RequestUnload() { g_unload = true; }

bool IsOn(const char* id) {
    Entry* e = FindEntry(id);
    return e && e->on;
}

float Value(const char* id) {
    Entry* e = FindEntry(id);
    return e ? e->val : 0.f;
}

int Mode(const char* id) {
    Entry* e = FindEntry(id);
    return e ? e->mode : 0;
}

// Every key / mouse-button press goes through here first. Returns true if we used it.
bool OnKeyDown(unsigned vk) {
    if (g_capture) {
        if (vk == VK_ESCAPE) { g_capture = nullptr; return true; }
        *g_capture = (vk == VK_BACK || vk == VK_DELETE) ? 0 : vk;
        if (g_capture == &g_menuKey && g_menuKey == 0) g_menuKey = VK_INSERT;   // never lose the menu
        g_capture = nullptr;
        MarkCfg();
        return true;
    }
    if (vk == g_menuKey) { g_open = !g_open; return true; }
    if (g_unloadKey && vk == g_unloadKey) {
        Log("unload key pressed");
        g_open = false;
        g_unload = true;
        return true;
    }
    if (g_open && ImGui::GetCurrentContext() && ImGui::GetIO().WantTextInput) return false;   // typing in a box
    for (auto& t : g_tabs)
        for (auto& e : t.entries)
            if (e.bind == vk && !e.noCheck) {
                e.on = !e.on;
                g_dirty = true;
                MarkCfg();
                Log("bind: %s -> %d", e.id, e.on ? 1 : 0);
                return true;
            }
    for (auto& a : g_actions)
        if (a.bind == vk) { RunAction(a.cmd); return true; }
    return false;
}

const char* Pool(const std::string& s) {
    g_strPool.push_back(s);
    return g_strPool.back().c_str();
}

std::vector<std::string> SplitBar(const std::string& line) {
    std::vector<std::string> out;
    size_t a = 0;
    for (;;) {
        size_t b = line.find('|', a);
        out.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    return out;
}

// live_ui.txt (Lua -> DLL):  f|id|label|tip|slider 0/1|lo|hi|def|fmt|tab   and   a|id|label|tip|danger 0/1|tab
// tab = a section name (Player, Weapon, Visuals...); missing or unknown = the Live section.
int TabByName(const std::string& name) {
    std::string n = Lower(name);
    if (n == "hidden") return -1;
    if (n == "fun") return TAB_FUN;   // live mods name the chaos group "Fun"
    for (int t = 0; t < (int)g_tabs.size(); ++t) if (Lower(g_tabs[t].name) == n && t != TAB_BINDS && t != TAB_DEBUG) return t;
    for (const MainCat& m : kMain) if (Lower(m.name) == n && strcmp(m.name, "Settings")) return m.subs[0];
    return TAB_LIVE;
}
void ReadLiveUi() {
    std::ifstream f(g_modDir + L"\\live_ui.txt", std::ios::binary);
    std::string raw;
    if (f) { std::ostringstream ss; ss << f.rdbuf(); raw = ss.str(); }
    if (raw == g_liveUiRaw) return;
    g_liveUiRaw = raw;

    // remember the current state of what is there now, then take the old live entries out of every tab
    for (auto& t : g_tabs) {
        for (auto& e : t.entries) {
            if (strncmp(e.id, "lv_", 3)) continue;
            std::string k = std::string("e.") + e.id;
            char b[64]; snprintf(b, sizeof b, "%.4f", e.val);
            g_liveCfg[k + ".on"] = e.on ? "1" : "0";
            g_liveCfg[k + ".val"] = b;
            g_liveCfg[k + ".bind"] = std::to_string(e.bind);
        }
        t.entries.erase(std::remove_if(t.entries.begin(), t.entries.end(),
                                       [](const Entry& e) { return !strncmp(e.id, "lv_", 3); }), t.entries.end());
    }
    g_liveActionTab.clear();
    for (auto& a : g_actions)
        if (!strncmp(a.cmd, "lv_", 3)) g_liveCfg[std::string("a.") + a.cmd + ".bind"] = std::to_string(a.bind);
    g_capture = nullptr;   // pointers into the old lists go away

    auto cfg = [](const std::string& k, const char* fallback) {
        auto it = g_liveCfg.find(k);
        return it != g_liveCfg.end() ? it->second : std::string(fallback);
    };
    std::vector<std::pair<int, Entry>> entries;
    std::vector<Action> actions;
    std::istringstream in(raw);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> p = SplitBar(line);
        if (p.size() < 3 || p[1].empty()) continue;
        std::string id = "lv_" + p[1];
        std::string tip = p.size() > 3 ? p[3] : "";
        if (p[0] == "f") {
            Entry e{};
            e.id = Pool(id);
            e.label = Pool(p[2]);
            e.tip = Pool(tip);
            e.hasSlider = p.size() > 4 && p[4] == "1";
            e.lo = p.size() > 5 ? (float)atof(p[5].c_str()) : 0.f;
            e.hi = p.size() > 6 ? (float)atof(p[6].c_str()) : 1.f;
            e.def = p.size() > 7 ? (float)atof(p[7].c_str()) : e.lo;
            e.fmt = Pool(p.size() > 8 && !p[8].empty() ? p[8] : "%.2f");
            std::string k = "e." + id;
            e.on = cfg(k + ".on", "0") == "1";
            e.val = (std::min)(e.hi, (std::max)(e.lo, (float)atof(cfg(k + ".val", std::to_string(e.def).c_str()).c_str())));
            e.bind = (unsigned)strtoul(cfg(k + ".bind", "0").c_str(), nullptr, 10);
            if (p.size() > 10 && p[10] == "1") { e.noCheck = true; e.noStatus = true; }   // value-only setting, no switch
            if (p.size() > 11 && !p[11].empty()) {   // a setting shown under another feature (native id, else live id)
                Entry* nat = FindEntry(p[11]);
                e.parent = Pool(nat && strncmp(nat->id, "lv_", 3) ? p[11] : "lv_" + p[11]);
                e.noStatus = true;
            }
            int et = TabByName(p.size() > 9 ? p[9] : "");
            if (et >= 0) entries.push_back({ et, e });
        } else if (p[0] == "a") {
            Action a{ Pool(id), Pool(p[2]), tip.empty() ? nullptr : Pool(tip), p.size() > 4 && p[4] == "1", 0 };
            a.bind = (unsigned)strtoul(cfg("a." + id + ".bind", "0").c_str(), nullptr, 10);
            g_liveActionTab[id] = TabByName(p.size() > 5 ? p[5] : "");
            actions.push_back(a);
        }
    }
    for (auto& pe : entries) g_tabs[pe.first].entries.push_back(pe.second);
    g_actions.erase(std::remove_if(g_actions.begin(), g_actions.end(),
                                   [](const Action& a) { return !strncmp(a.cmd, "lv_", 3); }), g_actions.end());
    g_actions.insert(g_actions.end(), actions.begin(), actions.end());
    g_dirty = true;
    Log("live features: %zu, buttons: %zu", entries.size(), actions.size());
}

// bridge\menu_req.txt (MCP -> DLL): lets the MCP server flip menu features and press buttons.
//   set <id> on|off|toggle [value]    val <id> <value>    mode <id> <n>    action <cmd>    open | close
// Answer in bridge\menu_resp.txt: the request id, what was done, then every entry and action.
void ReadRemote() {
    std::wstring dir = g_modDir + L"\\bridge";
    std::wstring req = dir + L"\\menu_req.txt";
    std::ifstream f(req);
    if (!f) return;
    std::vector<std::string> lines;
    for (std::string l; std::getline(f, l);) { if (!l.empty() && l.back() == '\r') l.pop_back(); lines.push_back(l); }
    f.close();
    DeleteFileW(req.c_str());
    std::ostringstream o;
    std::string reqId = "0";
    for (const std::string& l : lines) {
        std::istringstream in(l);
        std::string op, a, b, c;
        in >> op >> a >> b >> c;
        if (op == "id") { reqId = a; continue; }
        if (op.empty() || op == "state") continue;
        if (op == "open" || op == "close") { g_open = op == "open"; o << "result " << op << "\n"; continue; }
        if (op == "unload") { g_unload = true; o << "result unloading RonMenu\n"; continue; }
        if (op == "action") {
            if (!FindAction(a)) { o << "error unknown action " << a << "\n"; continue; }
            RunAction(a.c_str());
            o << "result ran " << a << "\n";
            continue;
        }
        Entry* e = FindEntry(a);
        if (!e) { o << "error unknown feature " << a << "\n"; continue; }
        if (op == "set") {
            if (b == "on") e->on = true; else if (b == "off") e->on = false; else if (b == "toggle") e->on = !e->on;
            if (!c.empty()) e->val = (std::min)(e->hi, (std::max)(e->lo, (float)atof(c.c_str())));
        } else if (op == "val") {
            e->val = (std::min)(e->hi, (std::max)(e->lo, (float)atof(b.c_str())));
        } else if (op == "mode") {
            e->mode = atoi(b.c_str());
        } else { o << "error unknown op " << op << "\n"; continue; }
        g_dirty = true;
        MarkCfg();
        o << "result " << a << " on=" << (e->on ? 1 : 0) << " val=" << e->val << " mode=" << e->mode << "\n";
    }
    o << "menu_open " << (g_open ? 1 : 0) << "\n";
    for (auto& t : g_tabs)
        for (auto& e : t.entries) {
            auto st = g_status.find(e.id);
            o << "entry " << t.name << "|" << e.id << "|" << e.label << "|on=" << (e.on ? 1 : 0) << "|val=" << e.val
              << "|range=" << e.lo << ".." << e.hi << "|mode=" << e.mode << "|slider=" << (e.hasSlider ? 1 : 0)
              << "|toggle=" << (e.noCheck ? 0 : 1) << "|status=" << (st != g_status.end() ? st->second : "") << "\n";
        }
    for (auto& a : g_actions) o << "action " << a.cmd << "|" << a.label << "\n";
    std::wstring tmp = dir + L"\\menu_resp.tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out << "id " << reqId << "\n" << o.str();
    }
    MoveFileExW(tmp.c_str(), (dir + L"\\menu_resp.txt").c_str(), MOVEFILE_REPLACE_EXISTING);
}

void Tick() {
    DWORD now = GetTickCount();
    if (now - g_liveReadAt > 250) { g_liveReadAt = now; ReadLiveUi(); ReadRemote(); }
    PollFlyKeys();
    if (!g_pendingPreset.empty() && g_catalogState >= 2) {
        std::string p = g_pendingPreset;
        g_pendingPreset.clear();
        if (g_catalogState == 2) RunAction(p.c_str());
    }
    if (now - g_lastRead > 500) { g_lastRead = now; ReadStatus(); }
    if ((g_dirty && now - g_lastWrite > 40) || now - g_lastWrite > 1000) {   // at least once a second (heartbeat)
        g_lastWrite = now;
        g_dirty = false;
        WriteState();
    }
    if (g_cfgDirty && now - g_cfgChangedAt > 800) {
        g_cfgDirty = false;
        SaveConfig();
    }
}

void Draw() {
    ImGuiIO& io = ImGui::GetIO();
    if (!g_open) {
        io.MouseDrawCursor = false;
        if (g_showHud) DrawHud();
        return;
    }

    ImVec2 want(1000.f * U(), 680.f * U());
    want.x = (std::min)(want.x, io.DisplaySize.x - 60.f);
    want.y = (std::min)(want.y, io.DisplaySize.y - 60.f);
    ImGui::SetNextWindowSize(want, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(40.f * U(), 40.f * U()), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2((std::min)(760.f * U(), io.DisplaySize.x - 40.f), (std::min)(440.f * U(), io.DisplaySize.y - 40.f)),
                                        ImVec2(FLT_MAX, FLT_MAX));   // large interface sizes still fit the screen
    if (ImGui::Begin("RonMenu##pages", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        DrawHeader();
        float footerH = ImGui::GetTextLineHeightWithSpacing() + 14.f * U();
        float bodyH = ImGui::GetContentRegionAvail().y - footerH;
        DrawSidebar(bodyH);
        ImGui::SameLine();
        DrawPage(bodyH);
        DrawFooter();
        g_mainPos = ImGui::GetWindowPos();
        g_mainSize = ImGui::GetWindowSize();
    }
    ImGui::End();
    io.MouseDrawCursor = g_open;
}

}  // namespace menu
