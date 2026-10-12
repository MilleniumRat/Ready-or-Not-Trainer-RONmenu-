// Made by MilleniumRat (github.com/MilleniumRat)
// ESP. The Lua mod writes esp.txt a few times per second:
//   cam=<addr>  pov_off / loc_off / rot_off      where the camera view and actor transforms live
//   c=<root addr>,<kind>,<state>,<half height>,<radius>,<health 0-1 or -1>
//   s=<16 bone offsets relative to the actor, "x,y,z" or "_", separated by |>   (follows its c= line)
//   o=<kind>,<x>,<y>,<z>,<label>                  static objects: trap / bomb / evidence / weapon
//   pc=<addr> ctrl_off=<hex>                       player controller + its ControlRotation (aimbot)
//   me=<root addr>                                 your own root component (3D view)
// The aimbot runs here, every frame: it picks a target from the live positions and turns the
// controller's ControlRotation towards it (smoothed), measured against the real camera view.
// Every frame we read the camera and each character's root location + rotation straight from
// game memory, so boxes follow perfectly; skeleton offsets ride on that live root position.
#include "common.h"
#include "imgui.h"
#include "stretch.h"

#include <algorithm>
#include <array>
#include <map>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

enum Kind { SUSPECT, CIVILIAN, SWAT, FRIEND };
enum State { ALIVE, SURRENDERED, ARRESTED, DEAD };
enum Style { STYLE_2D, STYLE_3D, STYLE_SKELETON, STYLE_SKELETON_BOX };
constexpr int kBones = 16;

struct Character {
    uintptr_t root;
    Kind kind;
    State state;
    float halfHeight, radius, health;
    int vis = -1;   // 1 visible, 0 hidden, -1 not checked
    float hpAbs = -1, hpMax = -1;   // -1 = not readable
    std::string weapon, name;
    int pid = -1;                   // players: PlayerState.PlayerId
    bool hasBones = false;
    bool boneOk[kBones] = {};
    float bone[kBones][3] = {};
};

struct Object {
    std::string kind, label;
    double pos[3];
};

std::vector<Character> g_chars;
std::vector<Object> g_objects;
uintptr_t g_cam = 0, g_pc = 0, g_me = 0;
uint32_t g_povOff = 0x1330, g_locOff = 0x130, g_rotOff = 0x148, g_ctrlOff = 0x318;
DWORD g_lastRead = 0;

double Now() { return GetTickCount64() / 1000.0; }

bool g_adsAim = false;   // copy of the fx.txt ADS flag (fx data is declared further down)

// aimbot state, shared with the drawing
uintptr_t g_aimTarget = 0;
double g_aimPoint[3] = {};
bool g_aimHas = false, g_aimActive = false;

// head neck chest pelvis | arm L | arm R | leg L | leg R
const int kSegments[][2] = {
    { 0, 1 }, { 1, 2 }, { 2, 3 },
    { 1, 4 }, { 4, 5 }, { 5, 6 },
    { 1, 7 }, { 7, 8 }, { 8, 9 },
    { 3, 10 }, { 10, 11 }, { 11, 12 },
    { 3, 13 }, { 13, 14 }, { 14, 15 },
};

// Memory may have been freed since Lua wrote the address: never let a bad read crash the game.
bool SafeRead(uintptr_t addr, void* out, size_t n) {
    if (addr < 0x10000 || addr > 0x7FFFFFFFFFFFull) return false;
    __try {
        memcpy(out, reinterpret_cast<const void*>(addr), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool Sane(double v) { return std::isfinite(v) && std::fabs(v) < 1e8; }

void ParseBones(Character& c, const std::string& s) {
    size_t start = 0;
    for (int i = 0; i < kBones && start <= s.size(); ++i) {
        size_t bar = s.find('|', start);
        std::string part = s.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
        float x, y, z;
        if (part != "_" && sscanf_s(part.c_str(), "%f,%f,%f", &x, &y, &z) == 3) {
            c.bone[i][0] = x; c.bone[i][1] = y; c.bone[i][2] = z;
            c.boneOk[i] = true;
            c.hasBones = true;
        }
        if (bar == std::string::npos) break;
        start = bar + 1;
    }
}

void ReadFile_() {
    std::ifstream f(menu::ModDir() + L"\\esp.txt");
    if (!f) return;   // keep last data while Lua swaps the file
    std::vector<Character> chars;
    std::vector<Object> objs;
    uintptr_t cam = 0, pc = 0, me = 0;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("cam=", 0) == 0) cam = strtoull(line.c_str() + 4, nullptr, 16);
        else if (line.rfind("pov_off=", 0) == 0) g_povOff = (uint32_t)strtoul(line.c_str() + 8, nullptr, 16);
        else if (line.rfind("loc_off=", 0) == 0) g_locOff = (uint32_t)strtoul(line.c_str() + 8, nullptr, 16);
        else if (line.rfind("rot_off=", 0) == 0) g_rotOff = (uint32_t)strtoul(line.c_str() + 8, nullptr, 16);
        else if (line.rfind("pc=", 0) == 0) pc = strtoull(line.c_str() + 3, nullptr, 16);
        else if (line.rfind("me=", 0) == 0) me = strtoull(line.c_str() + 3, nullptr, 16);
        else if (line.rfind("ctrl_off=", 0) == 0) g_ctrlOff = (uint32_t)strtoul(line.c_str() + 9, nullptr, 16);
        else if (line.rfind("c=", 0) == 0) {
            // root,kind,state,halfHeight,radius,health,vis,hpAbs,hpMax,weapon,name,playerId
            std::vector<std::string> f;
            size_t st = 2;
            while (true) {
                size_t comma = line.find(',', st);
                f.push_back(line.substr(st, comma == std::string::npos ? std::string::npos : comma - st));
                if (comma == std::string::npos) break;
                st = comma + 1;
            }
            if (f.size() >= 3) {
                Character c{};
                c.root = (uintptr_t)strtoull(f[0].c_str(), nullptr, 16);
                c.kind = f[1] == "civilian" ? CIVILIAN : f[1] == "swat" ? SWAT : f[1] == "friend" ? FRIEND : SUSPECT;
                c.state = f[2] == "surrendered" ? SURRENDERED : f[2] == "arrested" ? ARRESTED : f[2] == "dead" ? DEAD : ALIVE;
                float hh = f.size() > 3 ? (float)atof(f[3].c_str()) : 90.f, rad = f.size() > 4 ? (float)atof(f[4].c_str()) : 34.f;
                c.halfHeight = (hh > 10 && hh < 300) ? hh : 90.f;
                c.radius = (rad > 5 && rad < 200) ? rad : 34.f;
                c.health = f.size() > 5 ? (float)atof(f[5].c_str()) : -1.f;
                c.vis = f.size() > 6 ? atoi(f[6].c_str()) : -1;
                c.hpAbs = f.size() > 7 ? (float)atof(f[7].c_str()) : -1.f;
                c.hpMax = f.size() > 8 ? (float)atof(f[8].c_str()) : -1.f;
                if (f.size() > 9) c.weapon = f[9];
                if (f.size() > 10) c.name = f[10];
                c.pid = f.size() > 11 ? atoi(f[11].c_str()) : -1;
                chars.push_back(c);
            }
        } else if (line.rfind("s=", 0) == 0 && !chars.empty()) {
            ParseBones(chars.back(), line.substr(2));
        } else if (line.rfind("o=", 0) == 0) {
            Object o{};
            char kind[32] = {};
            double x, y, z;
            int consumed = 0;
            if (sscanf_s(line.c_str() + 2, "%31[^,],%lf,%lf,%lf,%n", kind, (unsigned)sizeof kind, &x, &y, &z, &consumed) >= 4) {
                o.kind = kind;
                o.pos[0] = x; o.pos[1] = y; o.pos[2] = z;
                if (consumed > 0) o.label = line.substr(2 + consumed);
                objs.push_back(o);
            }
        }
    }
    if (!cam) return;
    g_cam = cam;
    g_pc = pc;
    g_me = me;
    g_chars = std::move(chars);
    g_objects = std::move(objs);
}

struct View {
    double loc[3];
    double rot[3];                // pitch, yaw, roll (degrees)
    float fov;
    double ax[3], ay[3], az[3];   // forward, right, up
    double focal;                 // pixels
    float cx, cy;
};

bool BuildView(View& v) {
    struct { double loc[3]; double rot[3]; float fov; } pov;
    if (!SafeRead(g_cam + g_povOff, &pov, sizeof pov)) return false;
    for (double d : pov.loc) if (!Sane(d)) return false;
    if (!(pov.fov > 5.f && pov.fov < 170.f)) return false;

    const double d2r = 3.14159265358979 / 180.0;
    double p = pov.rot[0] * d2r, y = pov.rot[1] * d2r, r = pov.rot[2] * d2r;
    double sp = sin(p), cp = cos(p), sy = sin(y), cy = cos(y), sr = sin(r), cr = cos(r);
    // Unreal's FRotationMatrix axes
    v.ax[0] = cp * cy;                   v.ax[1] = cp * sy;                   v.ax[2] = sp;
    v.ay[0] = sr * sp * cy - cr * sy;    v.ay[1] = sr * sp * sy + cr * cy;    v.ay[2] = -sr * cp;
    v.az[0] = -(cr * sp * cy + sr * sy); v.az[1] = cy * sr - cr * sp * sy;    v.az[2] = cr * cp;
    memcpy(v.loc, pov.loc, sizeof v.loc);
    memcpy(v.rot, pov.rot, sizeof v.rot);
    v.fov = pov.fov;

    ImVec2 ds = ImGui::GetIO().DisplaySize;
    v.cx = ds.x * 0.5f;
    v.cy = ds.y * 0.5f;
    v.focal = v.cx / tan(pov.fov * d2r * 0.5);   // FOV is horizontal in Unreal
    return true;
}

bool Project(const View& v, const double w[3], ImVec2& out) {
    double d[3] = { w[0] - v.loc[0], w[1] - v.loc[1], w[2] - v.loc[2] };
    double x = d[0] * v.ay[0] + d[1] * v.ay[1] + d[2] * v.ay[2];
    double y = d[0] * v.az[0] + d[1] * v.az[1] + d[2] * v.az[2];
    double z = d[0] * v.ax[0] + d[1] * v.ax[1] + d[2] * v.ax[2];
    if (z < 1.0) return false;
    // the stretch pass widens the frame around the centre, so widen our projection the same way
    out.x = (float)(v.cx + x * v.focal / z * stretch::Applied());
    out.y = (float)(v.cy - y * v.focal / z);
    return true;
}

bool ProjectSegment(const View& v, const double a[3], const double b[3], ImVec2& pa, ImVec2& pb) {
    auto depth = [&](const double w[3]) {
        return (w[0] - v.loc[0]) * v.ax[0] + (w[1] - v.loc[1]) * v.ax[1] + (w[2] - v.loc[2]) * v.ax[2];
    };
    double za = depth(a), zb = depth(b);
    const double nearZ = 5.0;
    if (za < nearZ && zb < nearZ) return false;
    double A[3] = { a[0], a[1], a[2] }, B[3] = { b[0], b[1], b[2] };
    if (za < nearZ) { double t = (nearZ - za) / (zb - za); for (int i = 0; i < 3; ++i) A[i] = a[i] + (b[i] - a[i]) * t; }
    if (zb < nearZ) { double t = (nearZ - zb) / (za - zb); for (int i = 0; i < 3; ++i) B[i] = b[i] + (a[i] - b[i]) * t; }
    return Project(v, A, pa) && Project(v, B, pb);
}

ImU32 ColorFor(const Character& c, int alpha = 255) {
    if (c.state == DEAD) return IM_COL32(150, 150, 150, alpha * 220 / 255);
    if (c.state == ARRESTED) return IM_COL32(90, 170, 255, alpha);
    if (c.state == SURRENDERED) return IM_COL32(255, 210, 60, alpha);
    ImU32 k = c.kind == CIVILIAN ? menu::Color("col_civ") : c.kind == SWAT ? menu::Color("col_swat")
            : c.kind == FRIEND ? menu::Color("col_friend") : menu::Color("col_suspect");
    return (k & 0x00FFFFFF) | ((ImU32)alpha << 24);
}

const char* LabelFor(const Character& c) {
    static const char* names[2][4] = {
        { "Suspect", "Suspect (surrendered)", "Suspect (cuffed)", "Suspect (down)" },
        { "Civilian", "Civilian (complying)", "Civilian (cuffed)", "Civilian (down)" },
    };
    if (c.kind == SWAT) return "SWAT";
    if (c.kind == FRIEND) return "Player";
    return names[c.kind][c.state];
}

void Line(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float t) {
    dl->AddLine(a, b, IM_COL32(0, 0, 0, 150), t + 2.f);
    dl->AddLine(a, b, col, t);
}

void Text(ImDrawList* dl, ImVec2 centerTop, ImU32 col, const char* s) {
    ImVec2 ts = ImGui::CalcTextSize(s);
    ImVec2 p(centerTop.x - ts.x * 0.5f, centerTop.y);
    dl->AddText(ImVec2(p.x + 1, p.y + 1), IM_COL32(0, 0, 0, 200), s);
    dl->AddText(p, col, s);
}

void HealthBar(ImDrawList* dl, ImVec2 top, ImVec2 bottom, float hp) {
    float h = bottom.y - top.y;
    ImVec2 a(top.x - 6.f, top.y), b(top.x - 3.f, bottom.y);
    dl->AddRectFilled(ImVec2(a.x - 1, a.y - 1), ImVec2(b.x + 1, b.y + 1), IM_COL32(0, 0, 0, 170));
    ImU32 c = hp > 0.6f ? IM_COL32(80, 220, 110, 255) : hp > 0.3f ? IM_COL32(240, 200, 60, 255) : IM_COL32(240, 70, 60, 255);
    dl->AddRectFilled(ImVec2(a.x, b.y - h * hp), b, c);
}

bool Box3D(ImDrawList* dl, const View& v, const double loc[3], double yawDeg, float rad, float hh, ImU32 col) {
    double yaw = yawDeg * 3.14159265358979 / 180.0;
    double c = cos(yaw), s = sin(yaw);
    ImVec2 p[8];
    int i = 0;
    for (int zs = -1; zs <= 1; zs += 2)
        for (int k = 0; k < 4; ++k) {
            double lx = (k == 0 || k == 3) ? -rad : rad;
            double ly = (k < 2) ? -rad : rad;
            double w[3] = { loc[0] + lx * c - ly * s, loc[1] + lx * s + ly * c, loc[2] + zs * hh };
            if (!Project(v, w, p[i++])) return false;
        }
    for (int k = 0; k < 4; ++k) {
        Line(dl, p[k], p[(k + 1) % 4], col, 1.4f);           // bottom
        Line(dl, p[4 + k], p[4 + (k + 1) % 4], col, 1.4f);   // top
        Line(dl, p[k], p[4 + k], col, 1.4f);                 // sides
    }
    return true;
}

bool Skeleton(ImDrawList* dl, const View& v, const Character& c, const double loc[3], ImU32 col) {
    if (!c.hasBones) return false;
    ImVec2 p[kBones];
    bool ok[kBones] = {};
    for (int i = 0; i < kBones; ++i) {
        if (!c.boneOk[i]) continue;
        double w[3] = { loc[0] + c.bone[i][0], loc[1] + c.bone[i][1], loc[2] + c.bone[i][2] };
        ok[i] = Project(v, w, p[i]);
    }
    int drawn = 0;
    for (auto& seg : kSegments) {
        int a = seg[0], b = seg[1];
        if (!ok[b]) continue;
        if (!ok[a] && a == 1 && ok[2]) a = 2;   // no neck bone: hang arms off the chest
        if (!ok[a]) continue;
        Line(dl, p[a], p[b], col, 1.6f);
        ++drawn;
    }
    if (ok[0]) {
        float r = 4.f;
        if (ok[1]) r = (std::max)(3.f, std::hypot(p[0].x - p[1].x, p[0].y - p[1].y) * 0.75f);
        dl->AddCircle(p[0], r, IM_COL32(0, 0, 0, 150), 16, 3.f);
        dl->AddCircle(p[0], r, col, 16, 1.5f);
    }
    return drawn > 0;
}

void DrawObject(ImDrawList* dl, const View& v, const Object& o) {
    if (o.kind == "trapoff" && menu::IsOn("trap_activeonly")) return;
    double dx = o.pos[0] - v.loc[0], dy = o.pos[1] - v.loc[1], dz = o.pos[2] - v.loc[2];
    double dist = sqrt(dx * dx + dy * dy + dz * dz) / 100.0;
    if (dist > menu::Value("obj_dist")) return;
    ImVec2 s;
    if (!Project(v, o.pos, s)) return;
    ImU32 col = IM_COL32(200, 200, 200, 230);
    const char* name = o.kind.c_str();
    if (o.kind == "trap")     { col = menu::Color("col_trap");     name = "Trap"; }
    if (o.kind == "trapoff")  { col = IM_COL32(140, 140, 150, 200); name = "Trap"; }
    if (o.kind == "body")     { col = menu::Color("col_body");     name = "Body"; }
    if (o.kind == "bomb")     { col = menu::Color("col_bomb");     name = "Bomb"; }
    if (o.kind == "evidence") { col = menu::Color("col_evidence"); name = "Evidence"; }
    if (o.kind == "weapon")   { col = menu::Color("col_weapon");   name = "Weapon"; }
    if (o.kind == "objective") { col = menu::Color("col_objective"); name = "Objective"; }
    if (o.kind == "objdone")  { col = IM_COL32(140, 140, 150, 200); name = "Objective (done)"; }
    float r = 6.f;
    ImVec2 d[4] = { { s.x, s.y - r }, { s.x + r, s.y }, { s.x, s.y + r }, { s.x - r, s.y } };
    dl->AddQuadFilled(d[0], d[1], d[2], d[3], IM_COL32(0, 0, 0, 120));
    dl->AddQuad(d[0], d[1], d[2], d[3], col, 2.f);
    char label[96];
    if (!o.label.empty()) snprintf(label, sizeof label, "%s %s  %.0fm", name, o.label.c_str(), dist);
    else snprintf(label, sizeof label, "%s  %.0fm", name, dist);
    float sc = menu::Value("obj_size");
    ImFont* f = ImGui::GetFont();
    float size = ImGui::GetFontSize() * sc;
    ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0.f, label);
    ImVec2 tp(s.x - ts.x * 0.5f, s.y + r + 2.f);
    dl->AddText(f, size, ImVec2(tp.x + 1, tp.y + 1), IM_COL32(0, 0, 0, 200), label);
    dl->AddText(f, size, tp, col, label);
}

// ------------------------------------------------------------------------------------------
// Aimbot
// ------------------------------------------------------------------------------------------
const double kD2R = 3.14159265358979 / 180.0;

double Norm180(double a) {
    a = fmod(a + 180.0, 360.0);
    if (a < 0) a += 360.0;
    return a - 180.0;
}

bool GameFocused() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

bool AimKeyDown() {
    auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    switch (menu::Mode("aim_key")) {
        case 0: return down(VK_RBUTTON);
        case 1: return down(VK_LBUTTON);
        case 2: return down(VK_XBUTTON1);
        case 3: return down(VK_XBUTTON2);
        case 4: return down(VK_SHIFT);
        case 5: return down(VK_MENU);
        default: return true;   // always
    }
}

// world position of the aim point on this character
bool AimPointFor(const Character& c, const double loc[3], const View& v, double out[3]) {
    int bone = menu::Mode("aim_bone");   // 0 head 1 neck 2 chest 3 pelvis 4 nearest
    static const double capsuleZ[4] = { 0.78, 0.62, 0.35, 0.0 };   // fallback: share of the half height
    if (bone == 4) {
        double best = 1e9;
        bool any = false;
        for (int b = 0; b < 4; ++b) {
            double p[3];
            if (c.hasBones && c.boneOk[b]) { p[0] = loc[0] + c.bone[b][0]; p[1] = loc[1] + c.bone[b][1]; p[2] = loc[2] + c.bone[b][2]; }
            else { p[0] = loc[0]; p[1] = loc[1]; p[2] = loc[2] + c.halfHeight * capsuleZ[b]; }
            double d[3] = { p[0] - v.loc[0], p[1] - v.loc[1], p[2] - v.loc[2] };
            double len = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (len < 1) continue;
            double cosA = (d[0] * v.ax[0] + d[1] * v.ax[1] + d[2] * v.ax[2]) / len;
            double ang = acos((std::max)(-1.0, (std::min)(1.0, cosA)));
            if (ang < best) { best = ang; memcpy(out, p, sizeof p); any = true; }
        }
        return any;
    }
    if (c.hasBones && c.boneOk[bone]) {
        out[0] = loc[0] + c.bone[bone][0]; out[1] = loc[1] + c.bone[bone][1]; out[2] = loc[2] + c.bone[bone][2];
    } else {
        out[0] = loc[0]; out[1] = loc[1]; out[2] = loc[2] + c.halfHeight * capsuleZ[bone];
    }
    return true;
}

// angle between the view direction and the direction to p (degrees)
double AngleTo(const View& v, const double p[3]) {
    double d[3] = { p[0] - v.loc[0], p[1] - v.loc[1], p[2] - v.loc[2] };
    double len = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len < 1) return 999;
    double cosA = (d[0] * v.ax[0] + d[1] * v.ax[1] + d[2] * v.ax[2]) / len;
    return acos((std::max)(-1.0, (std::min)(1.0, cosA))) / kD2R;
}

void RunAimbot(const View& v) {
    g_aimHas = false;
    g_aimActive = false;
    if (!menu::IsOn("aimbot")) { g_aimTarget = 0; return; }
    const double fov = menu::Value("aimbot");
    const int targets = menu::Mode("aim_targets");   // 0 suspects 1 civilians 2 both
    const int priority = menu::Mode("aim_priority"); // 0 crosshair 1 distance 2 health
    const bool skipDown = menu::IsOn("aim_skip_down");
    const bool onlyVisible = menu::IsOn("aim_visible");
    const bool sticky = menu::IsOn("aim_sticky");

    const Character* best = nullptr;
    double bestScore = 1e18, bestPoint[3] = {};
    for (const Character& c : g_chars) {
        if (c.state == DEAD) continue;
        if (skipDown && c.state != ALIVE) continue;
        if (c.kind == SWAT) continue;
        if (c.kind == FRIEND) {   // players only when you pick "Selected friend" on purpose
            int sel = menu::SelectedPlayerId();
            if (!menu::IsOn("aim_friend") || (sel >= 0 && c.pid != sel)) continue;
        } else {
            if (targets == 0 && c.kind != SUSPECT) continue;
            if (targets == 1 && c.kind != CIVILIAN) continue;
        }
        if (onlyVisible && c.vis == 0) continue;
        double loc[3];
        if (!SafeRead(c.root + g_locOff, loc, sizeof loc) || !Sane(loc[0]) || !Sane(loc[1]) || !Sane(loc[2])) continue;
        double p[3];
        if (!AimPointFor(c, loc, v, p)) continue;
        double ang = AngleTo(v, p);
        bool isCurrent = sticky && c.root == g_aimTarget;
        if (ang > (isCurrent ? fov * 1.5 : fov)) continue;
        double dist = sqrt((p[0] - v.loc[0]) * (p[0] - v.loc[0]) + (p[1] - v.loc[1]) * (p[1] - v.loc[1]) + (p[2] - v.loc[2]) * (p[2] - v.loc[2]));
        if (dist > menu::Value("aim_maxdist") * 100.0) continue;
        double score = priority == 1 ? dist : priority == 2 ? (c.health >= 0 ? c.health : 1.0) * 1000 + ang : ang;
        if (isCurrent) score -= 1e12;   // keep the current target while it stays inside
        if (score < bestScore) { bestScore = score; best = &c; memcpy(bestPoint, p, sizeof p); }
    }
    if (!best) { g_aimTarget = 0; return; }
    g_aimHas = true;
    memcpy(g_aimPoint, bestPoint, sizeof bestPoint);

    if (menu::IsOpen() || !GameFocused() || !AimKeyDown() || !g_pc) return;
    if (menu::IsOn("aim_ads_only") && !g_adsAim) return;
    g_aimTarget = best->root;
    g_aimActive = true;

    double d[3] = { bestPoint[0] - v.loc[0], bestPoint[1] - v.loc[1], bestPoint[2] - v.loc[2] };
    double yawT = atan2(d[1], d[0]) / kD2R;
    double pitchT = atan2(d[2], sqrt(d[0] * d[0] + d[1] * d[1])) / kD2R;
    double dYaw = Norm180(yawT - v.rot[1]);
    double dPitch = Norm180(pitchT - v.rot[0]);

    // smoothing: 1 = snap, higher = slower; frame-rate independent
    double smooth = (std::max)(1.0, (double)menu::Value("aim_smooth"));
    double dt = (std::min)(0.1, (double)ImGui::GetIO().DeltaTime);
    double k = smooth <= 1.0 ? 1.0 : 1.0 - pow(1.0 - 1.0 / smooth, dt * 60.0);
    k *= (std::max)(0.05, (std::min)(1.0, (double)menu::Value("aim_strength")));

    double ctrl[3];
    uintptr_t at = g_pc + g_ctrlOff;
    if (!SafeRead(at, ctrl, sizeof ctrl) || !Sane(ctrl[0]) || !Sane(ctrl[1])) return;
    ctrl[0] = fmod(ctrl[0] + dPitch * k + 360.0, 360.0);
    ctrl[1] = fmod(ctrl[1] + dYaw * k + 360.0, 360.0);
    __try {
        memcpy(reinterpret_cast<void*>(at), ctrl, sizeof(double) * 2);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// approximate gun position: a little below and right of the camera
void GunPoint(const View& v, double out[3]) {
    for (int i = 0; i < 3; ++i) out[i] = v.loc[i] + v.ax[i] * 35.0 + v.ay[i] * 12.0 - v.az[i] * 14.0;
}

void DrawAimOverlay(ImDrawList* dl, const View& v) {
    if (!menu::IsOn("aimbot") || menu::IsOpen()) return;
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    if (menu::IsOn("aim_fov_circle")) {
        double r = tan(menu::Value("aimbot") * kD2R) * v.focal;
        if (r > 2 && r < ds.x) {
            ImU32 col = g_aimActive ? IM_COL32(255, 90, 90, 200) : IM_COL32(255, 255, 255, 90);
            dl->AddCircle(ImVec2(v.cx, v.cy), (float)r, IM_COL32(0, 0, 0, 90), 96, 3.f);
            dl->AddCircle(ImVec2(v.cx, v.cy), (float)r, col, 96, 1.4f);
        }
    }
    if (!g_aimHas) return;
    ImVec2 t;
    if (Project(v, g_aimPoint, t)) {
        ImU32 col = g_aimActive ? IM_COL32(255, 70, 70, 255) : IM_COL32(255, 220, 90, 230);
        dl->AddCircle(t, 7.f, IM_COL32(0, 0, 0, 160), 20, 3.f);
        dl->AddCircle(t, 7.f, col, 20, 1.6f);
        dl->AddLine(ImVec2(t.x - 11, t.y), ImVec2(t.x - 4, t.y), col, 1.6f);
        dl->AddLine(ImVec2(t.x + 4, t.y), ImVec2(t.x + 11, t.y), col, 1.6f);
        dl->AddLine(ImVec2(t.x, t.y - 11), ImVec2(t.x, t.y - 4), col, 1.6f);
        dl->AddLine(ImVec2(t.x, t.y + 4), ImVec2(t.x, t.y + 11), col, 1.6f);
    }
    if (menu::IsOn("aim_line")) {   // 3D line from your gun to the aim point
        double g[3];
        GunPoint(v, g);
        ImVec2 a, b;
        if (ProjectSegment(v, g, g_aimPoint, a, b)) {
            ImU32 col = g_aimActive ? IM_COL32(255, 70, 70, 220) : IM_COL32(255, 220, 90, 160);
            // dashes so it reads as a path, not a wall line
            const int n = 24;
            for (int i = 0; i < n; i += 2) {
                float t0 = (float)i / n, t1 = (float)(i + 1) / n;
                dl->AddLine(ImVec2(a.x + (b.x - a.x) * t0, a.y + (b.y - a.y) * t0),
                            ImVec2(a.x + (b.x - a.x) * t1, a.y + (b.y - a.y) * t1), col, 2.f);
            }
        }
    }
}


// ------------------------------------------------------------------------------------------
// fx.txt (live mod "combat"): confirmed hits, grenade arc, gunshots, grenades, ADS state
//   ads=0|1   h=<id>,<x,y,z>,<damage>,<head>,<kill>,<shot|melee>,<age>   arc=x,y,z;...   land=x,y,z
//   snd=<x,y,z>,<age>   g=<x,y,z>,<label>
// ------------------------------------------------------------------------------------------
struct Hit { int id; double pos[3]; float dmg; bool head, kill; double bornAt; };
struct Ping { double pos[3]; double bornAt; };
struct Nade { double pos[3]; std::string label; };
std::vector<Hit> g_hits;
std::vector<Ping> g_pings;
std::vector<Nade> g_nades;
std::vector<std::array<double, 3>> g_arc;
bool g_hasLand = false, g_ads = false, g_rel = false;
double g_land[3] = {};
int g_lastHitId = 0;
DWORD g_fxRead = 0;

bool FxWanted() {
    return menu::IsOn("hitmarkers") || menu::IsOn("grenade_arc") || menu::IsOn("sound_markers") ||
           (menu::IsOn("esp") && menu::IsOn("esp_projectiles")) || menu::IsOn("trigger") || menu::IsOn("aimbot");
}

void ReadFx() {
    std::ifstream f(menu::ModDir() + L"\\fx.txt");
    if (!f) { g_arc.clear(); g_hasLand = false; g_nades.clear(); return; }
    std::vector<std::array<double, 3>> arc;
    std::vector<Nade> nades;
    std::vector<Ping> pings;
    bool land = false;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("ads=", 0) == 0) g_ads = line[4] == '1';
        else if (line.rfind("rel=", 0) == 0) g_rel = line[4] == '1';
        else if (line.rfind("h=", 0) == 0) {
            Hit h{};
            int head = 0, kill = 0;
            char kind[16] = {};
            double age = 0;
            if (sscanf_s(line.c_str() + 2, "%d,%lf,%lf,%lf,%f,%d,%d,%15[^,],%lf", &h.id, &h.pos[0], &h.pos[1], &h.pos[2], &h.dmg,
                         &head, &kill, kind, (unsigned)sizeof kind, &age) >= 7 && h.id > g_lastHitId) {
                g_lastHitId = h.id;
                h.head = head != 0;
                h.kill = kill != 0;
                h.bornAt = Now() - age;
                g_hits.push_back(h);
                if (g_hits.size() > 30) g_hits.erase(g_hits.begin());
            }
        } else if (line.rfind("arc=", 0) == 0) {
            size_t i = 4;
            while (i < line.size()) {
                size_t semi = line.find(';', i);
                std::string part = line.substr(i, semi == std::string::npos ? std::string::npos : semi - i);
                std::array<double, 3> p{};
                if (sscanf_s(part.c_str(), "%lf,%lf,%lf", &p[0], &p[1], &p[2]) == 3) arc.push_back(p);
                if (semi == std::string::npos) break;
                i = semi + 1;
            }
        } else if (line.rfind("land=", 0) == 0) {
            land = sscanf_s(line.c_str() + 5, "%lf,%lf,%lf", &g_land[0], &g_land[1], &g_land[2]) == 3;
        } else if (line.rfind("snd=", 0) == 0) {
            Ping p{};
            double age = 0;
            if (sscanf_s(line.c_str() + 4, "%lf,%lf,%lf,%lf", &p.pos[0], &p.pos[1], &p.pos[2], &age) == 4) {
                p.bornAt = Now() - age;
                pings.push_back(p);
            }
        } else if (line.rfind("g=", 0) == 0) {
            Nade n{};
            int consumed = 0;
            if (sscanf_s(line.c_str() + 2, "%lf,%lf,%lf,%n", &n.pos[0], &n.pos[1], &n.pos[2], &consumed) >= 3) {
                if (consumed > 0) n.label = line.substr(2 + consumed);
                nades.push_back(n);
            }
        }
    }
    g_arc = std::move(arc);
    g_hasLand = land;
    g_nades = std::move(nades);
    g_pings = std::move(pings);
}

// ------------------------------------------------------------------------------------------
// Synthetic left clicks (triggerbot): they go through the game's normal input, so the
// weapon's own fire rules (fire mode, reload, safety) decide whether a shot comes out.
// ------------------------------------------------------------------------------------------
bool g_clickDown = false;
double g_clickDownAt = 0;

void Click(bool down) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    SendInput(1, &in, sizeof in);
    g_clickDown = down;
    if (down) g_clickDownAt = Now();
}

void ReleaseClick() {
    if (g_clickDown && Now() - g_clickDownAt > 0.035) Click(false);
}

bool Held(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

bool HoldOk(const char* modeId, bool hasMouse4) {
    switch (menu::Mode(modeId)) {
        case 0: return true;
        case 1: return Held(VK_RBUTTON);
        case 2: return Held(VK_SHIFT);
        case 3: return Held(VK_MENU);
        case 4: return hasMouse4 && Held(VK_XBUTTON1);
        default: return true;
    }
}

bool CharLoc(const Character& c, double loc[3]) {
    return SafeRead(c.root + g_locOff, loc, sizeof(double) * 3) && Sane(loc[0]) && Sane(loc[1]) && Sane(loc[2]);
}

double g_trigOnSince = -1, g_trigLast = 0;

void RunTrigger(const View& v) {
    if (!menu::IsOn("trigger") || menu::IsOpen() || !GameFocused() || !HoldOk("trigger_hold", true) ||
        (menu::IsOn("trigger_ads") && !g_ads)) { g_trigOnSince = -1; return; }
    if (Held(VK_LBUTTON) && !g_clickDown) { g_trigOnSince = -1; return; }   // you are firing yourself
    const double maxDist = menu::Value("trigger_dist") * 100.0;
    bool onTarget = false;
    for (const Character& c : g_chars) {
        if (c.state == DEAD) continue;
        if (menu::IsOn("trigger_skip_down") && c.state != ALIVE) continue;
        if (menu::IsOn("trigger_suspects") && c.kind != SUSPECT) continue;
        if (c.kind == SWAT || c.kind == FRIEND) continue;
        if (c.vis == 0) continue;
        double loc[3];
        if (!CharLoc(c, loc)) continue;
        double d = sqrt((loc[0] - v.loc[0]) * (loc[0] - v.loc[0]) + (loc[1] - v.loc[1]) * (loc[1] - v.loc[1]) + (loc[2] - v.loc[2]) * (loc[2] - v.loc[2]));
        if (d > maxDist) continue;
        double head[3] = { loc[0], loc[1], loc[2] + c.halfHeight * 0.95 }, feet[3] = { loc[0], loc[1], loc[2] - c.halfHeight };
        ImVec2 h, f;
        if (!Project(v, head, h) || !Project(v, feet, f)) continue;
        float w = (f.y - h.y) * (c.radius / c.halfHeight) * 0.75f * stretch::Applied();
        if (v.cx > h.x - w * 0.5f && v.cx < h.x + w * 0.5f && v.cy > h.y && v.cy < f.y) { onTarget = true; break; }
    }
    double now = Now();
    if (!onTarget) { g_trigOnSince = -1; return; }
    if (g_trigOnSince < 0) g_trigOnSince = now;
    double delay = menu::Value("trigger") / 1000.0;
    if (now - g_trigOnSince >= delay && !g_clickDown && now - g_trigLast >= 0.09) {
        Click(true);
        g_trigLast = now;
    }
}


// ------------------------------------------------------------------------------------------
// Hit markers, radar, grenade arc, gunshot markers, grenades
// ------------------------------------------------------------------------------------------
void DrawHitmarkers(ImDrawList* dl, const View& v) {
    if (!menu::IsOn("hitmarkers")) return;
    double dur = menu::Value("hitmarkers"), now = Now();
    static const ImU32 cols[4] = { IM_COL32(255, 255, 255, 255), IM_COL32(255, 70, 70, 255), IM_COL32(255, 220, 70, 255), IM_COL32(110, 160, 255, 255) };
    int ci = menu::Mode("hit_color");
    int style = menu::Mode("hit_style");
    for (const Hit& h : g_hits) {
        double age = now - h.bornAt;
        if (age < 0 || age > dur) continue;
        float a = (float)(1.0 - age / dur);
        ImU32 base = (h.head && menu::IsOn("hit_head")) ? IM_COL32(255, 196, 40, 255) : cols[(ci >= 0 && ci < 4) ? ci : 0];
        if (h.kill) base = IM_COL32(255, 60, 60, 255);
        ImU32 col = (base & 0x00FFFFFF) | ((ImU32)(255 * a) << 24);
        ImU32 shadow = IM_COL32(0, 0, 0, (int)(150 * a));
        float r0 = 6.f + (float)age * 6.f, r1 = r0 + (h.kill ? 11.f : 8.f);
        if (style == 0 || style == 2)
            for (int q = 0; q < 4; ++q) {
                float sx = (q & 1) ? 1.f : -1.f, sy = (q & 2) ? 1.f : -1.f;
                ImVec2 p0(v.cx + sx * r0 * 0.707f, v.cy + sy * r0 * 0.707f), p1(v.cx + sx * r1 * 0.707f, v.cy + sy * r1 * 0.707f);
                dl->AddLine(p0, p1, shadow, 4.f);
                dl->AddLine(p0, p1, col, 2.f);
            }
        if (style == 1 || style == 2) {
            dl->AddCircle(ImVec2(v.cx, v.cy), r1, shadow, 24, 4.f);
            dl->AddCircle(ImVec2(v.cx, v.cy), r1, col, 24, 2.f);
        }
        if (menu::IsOn("hit_numbers")) {
            double p[3] = { h.pos[0], h.pos[1], h.pos[2] + 70.0 + age * 90.0 };
            ImVec2 s;
            if (Project(v, p, s)) {
                char b[32];
                snprintf(b, sizeof b, h.head ? "%.0f HEAD" : "%.0f", h.dmg);
                Text(dl, s, col, b);
            }
        }
    }
}

void DrawRadar(const View& v) {
    if (!menu::IsOn("radar")) return;
    double me[3];
    if (!g_me || !SafeRead(g_me + g_locOff, me, sizeof me) || !Sane(me[0])) memcpy(me, v.loc, sizeof me);
    float size = menu::Value("radar_size");
    double range = menu::Value("radar") * 100.0;
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    float m = 20.f;
    int pos = menu::Mode("radar_pos");
    ImVec2 o((pos == 1 || pos == 3) ? ds.x - size - m : m, (pos == 2 || pos == 3) ? ds.y - size - m : m + 40.f);
    if (pos == 4) o = ImVec2(menu::Value("radar_x") * (ds.x - size), menu::Value("radar_y") * (ds.y - size));   // custom
    bool round = menu::Mode("radar_shape") == 0;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImVec2 c(o.x + size * 0.5f, o.y + size * 0.5f);
    float half = size * 0.5f - 6.f;
    ImU32 bgc = IM_COL32(12, 14, 18, (int)(255 * menu::Value("radar_bg")));
    if (round) { dl->AddCircleFilled(c, size * 0.5f, bgc, 64); dl->AddCircle(c, size * 0.5f, menu::Color("col_radar_border"), 64, 1.5f); }
    else { dl->AddRectFilled(o, ImVec2(o.x + size, o.y + size), bgc, 10.f); dl->AddRect(o, ImVec2(o.x + size, o.y + size), menu::Color("col_radar_border"), 10.f, 0, 1.5f); }
    if (menu::IsOn("radar_rings")) {
        dl->AddCircle(c, half * 0.5f, IM_COL32(255, 255, 255, 22), 48);
        dl->AddCircle(c, half, IM_COL32(255, 255, 255, 22), 48);
    }
    dl->AddLine(ImVec2(c.x, o.y + 6), ImVec2(c.x, o.y + size - 6), IM_COL32(255, 255, 255, 14));
    dl->AddLine(ImVec2(o.x + 6, c.y), ImVec2(o.x + size - 6, c.y), IM_COL32(255, 255, 255, 14));
    bool rot = menu::IsOn("radar_rotate");
    double yaw = v.rot[1] * kD2R, cy = cos(yaw), sy = sin(yaw);
    auto toRadar = [&](double dx, double dy, ImVec2& out) {
        double fwd, right;
        if (rot) { fwd = cy * dx + sy * dy; right = -sy * dx + cy * dy; }
        else { fwd = dx; right = dy; }
        double r = sqrt(fwd * fwd + right * right);
        if (r > range) {
            if (!menu::IsOn("radar_clamp")) return false;
            fwd *= range / r; right *= range / r;   // pinned to the edge
        }
        out = ImVec2(c.x + (float)(right / range * half), c.y - (float)(fwd / range * half));
        return true;
    };
    for (const Character& ch : g_chars) {
        if (ch.kind == CIVILIAN && !menu::IsOn("radar_civ")) continue;
        if ((ch.kind == SWAT || ch.kind == FRIEND) && !menu::IsOn("radar_team")) continue;
        double loc[3];
        if (!CharLoc(ch, loc)) continue;
        ImVec2 p;
        if (!toRadar(loc[0] - me[0], loc[1] - me[1], p)) continue;
        ImU32 col = ColorFor(ch);
        float r = (ch.state == DEAD ? 0.6f : 1.f) * menu::Value("radar_marker");
        if (ch.root == g_aimTarget && g_aimTarget) dl->AddCircle(p, r + 4.f, menu::Color("col_aim"), 16, 1.5f);
        dl->AddCircleFilled(p, r + 1.5f, IM_COL32(0, 0, 0, 160));
        dl->AddCircleFilled(p, r, col);
        if (menu::IsOn("radar_height")) {
            double dz = loc[2] - me[2];
            if (dz > 150) dl->AddTriangleFilled(ImVec2(p.x, p.y - 10), ImVec2(p.x - 3.5f, p.y - 5), ImVec2(p.x + 3.5f, p.y - 5), col);
            else if (dz < -150) dl->AddTriangleFilled(ImVec2(p.x, p.y + 10), ImVec2(p.x - 3.5f, p.y + 5), ImVec2(p.x + 3.5f, p.y + 5), col);
        }
    }
    // you: an arrow along your view
    double a = rot ? 0.0 : yaw;
    ImVec2 tip(c.x + (float)(sin(a) * 9), c.y - (float)(cos(a) * 9));
    ImVec2 l(c.x + (float)(sin(a - 2.5) * 6), c.y - (float)(cos(a - 2.5) * 6)), r(c.x + (float)(sin(a + 2.5) * 6), c.y - (float)(cos(a + 2.5) * 6));
    dl->AddTriangleFilled(tip, l, r, IM_COL32(110, 160, 255, 255));
    char lab[16];
    snprintf(lab, sizeof lab, "%.0f m", range / 100.0);
    dl->AddText(ImVec2(o.x + 8, o.y + size - ImGui::GetFontSize() - 6), IM_COL32(200, 205, 215, 160), lab);
}

void DrawArc(ImDrawList* dl, const View& v) {
    if (!menu::IsOn("grenade_arc") || g_arc.size() < 2) return;
    for (size_t i = 0; i + 1 < g_arc.size(); ++i) {
        ImVec2 a, b;
        if (ProjectSegment(v, g_arc[i].data(), g_arc[i + 1].data(), a, b)) {
            dl->AddLine(a, b, IM_COL32(0, 0, 0, 120), 4.f);
            dl->AddLine(a, b, IM_COL32(255, 170, 60, 230), 2.f);
        }
    }
    if (g_hasLand && menu::IsOn("arc_landing")) {
        ImVec2 prev;
        bool havePrev = false;
        for (int i = 0; i <= 24; ++i) {
            double t = i / 24.0 * 6.2831853;
            double p[3] = { g_land[0] + cos(t) * 60.0, g_land[1] + sin(t) * 60.0, g_land[2] + 3.0 };
            ImVec2 s;
            bool ok = Project(v, p, s);
            if (ok && havePrev) dl->AddLine(prev, s, IM_COL32(255, 170, 60, 230), 2.f);
            prev = s;
            havePrev = ok;
        }
    }
}

void DrawPings(ImDrawList* dl, const View& v) {
    if (!menu::IsOn("sound_markers")) return;
    double keep = menu::Value("sound_markers"), range = menu::Value("snd_range") * 100.0, now = Now();
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    for (const Ping& p : g_pings) {
        double age = now - p.bornAt;
        double dx = p.pos[0] - v.loc[0], dy = p.pos[1] - v.loc[1], dz = p.pos[2] - v.loc[2];
        double dist = sqrt(dx * dx + dy * dy + dz * dz);
        if (age < 0 || age > keep || dist > range) continue;
        float a = (float)(1.0 - age / keep);
        ImU32 col = IM_COL32(255, 120, 60, (int)(230 * a));
        ImVec2 s;
        if (Project(v, p.pos, s) && s.x > 0 && s.y > 0 && s.x < ds.x && s.y < ds.y) {
            float r = 8.f + (float)fmod(age, 0.8) * 25.f;
            dl->AddCircle(s, r, col, 24, 2.f);
            char b[16];
            snprintf(b, sizeof b, "%.0fm", dist / 100.0);
            Text(dl, ImVec2(s.x, s.y + 10), col, b);
        } else {   // off screen: an arrow on a ring around the crosshair
            double right = dx * v.ay[0] + dy * v.ay[1] + dz * v.ay[2];
            double fwd = dx * v.ax[0] + dy * v.ax[1] + dz * v.ax[2];
            double ang = atan2(right, fwd);
            float R = ds.y * 0.28f;
            ImVec2 c(v.cx + (float)sin(ang) * R, v.cy - (float)cos(ang) * R);
            ImVec2 tip(c.x + (float)sin(ang) * 12, c.y - (float)cos(ang) * 12);
            ImVec2 l(c.x + (float)sin(ang - 2.3) * 8, c.y - (float)cos(ang - 2.3) * 8), r2(c.x + (float)sin(ang + 2.3) * 8, c.y - (float)cos(ang + 2.3) * 8);
            dl->AddTriangleFilled(tip, l, r2, col);
        }
    }
}

void DrawNades(ImDrawList* dl, const View& v) {
    if (!menu::IsOn("esp_projectiles")) return;
    for (const Nade& n : g_nades) {
        ImVec2 s;
        if (!Project(v, n.pos, s)) continue;
        ImU32 col = IM_COL32(255, 150, 40, 255);
        dl->AddQuadFilled(ImVec2(s.x, s.y - 6), ImVec2(s.x + 6, s.y), ImVec2(s.x, s.y + 6), ImVec2(s.x - 6, s.y), col);
        double d = sqrt((n.pos[0] - v.loc[0]) * (n.pos[0] - v.loc[0]) + (n.pos[1] - v.loc[1]) * (n.pos[1] - v.loc[1]) + (n.pos[2] - v.loc[2]) * (n.pos[2] - v.loc[2]));
        char b[96];
        snprintf(b, sizeof b, "%s  %.0fm", n.label.c_str(), d / 100.0);
        Text(dl, ImVec2(s.x, s.y + 8), col, b);
    }
}

// ------------------------------------------------------------------------------------------
// ESP v2: one drawing routine for real characters and for the menu preview.
// Colour priority (first match wins): aim target > selected friend > down > cuffed >
// surrendered > hidden behind walls > category.
// ------------------------------------------------------------------------------------------
struct Visual {
    ImVec2 head, feet;            // top and bottom of the capsule on screen
    float width = 0;
    bool hasBones = false;
    ImVec2 bones[kBones];
    bool boneOk[kBones] = {};
    ImU32 col = 0, textCol = 0;
    float alpha = 1.f;
    float hp = -1, hpAbs = -1, hpMax = -1, hpLost = 0;   // hpLost: recent damage share, fades
    std::string name, state, weapon;
    double dist = 0;              // cm
    bool is3D = false;            // the caller drew a 3D box already
};

ImU32 WithAlpha(ImU32 c, float a) {
    int al = (int)(((c >> 24) & 0xFF) * (std::max)(0.f, (std::min)(1.f, a)));
    return (c & 0x00FFFFFF) | ((ImU32)al << 24);
}

ImU32 KindColor(const Character& c) {
    switch (c.kind) {
        case CIVILIAN: return menu::Color("col_civ");
        case SWAT:     return menu::Color("col_swat");
        case FRIEND:   return menu::Color("col_friend");
        default:       return menu::Color("col_suspect");
    }
}

ImU32 ResolveColor(const Character& c, bool aimTarget, bool selected) {
    if (aimTarget && menu::IsOn("esp_aimhl")) return menu::Color("col_aim");
    if (selected && menu::IsOn("esp_selhl")) return menu::Color("col_sel");
    if (c.state == DEAD) return menu::Color("col_dead");
    if (c.state == ARRESTED) return menu::Color("col_cuffed");
    if (c.state == SURRENDERED) return menu::Color("col_surr");
    if (c.vis == 0 && menu::IsOn("esp_vis_color")) return menu::Color("col_hidden");
    return KindColor(c);
}

const char* StateWord(const Character& c) {
    switch (c.state) {
        case SURRENDERED: return c.kind == CIVILIAN ? "complying" : "surrendered";
        case ARRESTED: return "cuffed";
        case DEAD: return "down";          // the game reports dead and unconscious together
        default: return "active";
    }
}

const char* KindWord(const Character& c) {
    switch (c.kind) { case CIVILIAN: return "Civilian"; case SWAT: return "SWAT"; case FRIEND: return "Player"; default: return "Suspect"; }
}

void TextAt(ImDrawList* dl, ImVec2 pos, ImU32 col, const char* s, float scale, bool shadow) {
    ImFont* f = ImGui::GetFont();
    float size = ImGui::GetFontSize() * scale;
    if (shadow) dl->AddText(f, size, ImVec2(pos.x + 1, pos.y + 1), WithAlpha(IM_COL32(0, 0, 0, 220), ((col >> 24) & 0xFF) / 255.f), s);
    dl->AddText(f, size, pos, col, s);
}

ImVec2 TextSize(const char* s, float scale) {
    return ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize() * scale, FLT_MAX, 0.f, s);
}

void DrawVisual(ImDrawList* dl, const Visual& v) {
    const int style = menu::Mode("esp_style");   // 0 box 1 corner 2 rounded 3 3D 4 skeleton 5 skeleton+box
    const float thick = (std::max)(0.5f, menu::Value("box_thick"));
    const bool outline = menu::IsOn("box_outline");
    ImU32 col = WithAlpha(v.col, v.alpha);
    ImU32 shadow = WithAlpha(menu::Color("col_outline"), v.alpha);
    ImVec2 a(v.head.x - v.width * 0.5f, v.head.y), b(v.head.x + v.width * 0.5f, v.feet.y);
    float w = b.x - a.x, h = b.y - a.y;

    // box
    bool drewSkel = false;
    if (style == 4 || style == 5) {
        if (v.hasBones) {
            float st = (std::max)(0.5f, menu::Value("skel_thick"));
            ImU32 sc = WithAlpha(col, menu::Value("skel_alpha"));
            for (auto& seg : kSegments) {
                int s0 = seg[0], s1 = seg[1];
                if (!v.boneOk[s1]) continue;
                if (!v.boneOk[s0] && s0 == 1 && v.boneOk[2]) s0 = 2;
                if (!v.boneOk[s0]) continue;
                if (outline) dl->AddLine(v.bones[s0], v.bones[s1], shadow, st + 2.f);
                dl->AddLine(v.bones[s0], v.bones[s1], sc, st);
                drewSkel = true;
            }
            if (menu::IsOn("skel_joints")) {
                float r = menu::Value("joint_size");
                for (int i = 0; i < kBones; ++i) if (v.boneOk[i]) dl->AddCircleFilled(v.bones[i], r, sc);
            }
        }
    }
    bool boxNeeded = !v.is3D && (style <= 2 || style == 5 || !drewSkel);
    if (boxNeeded) {
        ImU32 bc = style == 5 ? WithAlpha(col, 0.45f) : col;
        float fill = menu::Value("box_fill");
        int bs = (style == 4 || style == 5) ? 0 : style;   // skeleton fallback: full box
        float round = bs == 2 ? (std::min)(w, h) * 0.18f : 0.f;
        if (fill > 0.01f) dl->AddRectFilled(a, b, WithAlpha(col, fill * 0.6f), round);
        if (bs == 1) {   // corners
            float cl = (std::max)(3.f, (std::min)(w, h) * menu::Value("corner_len"));
            ImVec2 pts[4][3] = {
                { ImVec2(a.x, a.y + cl), a, ImVec2(a.x + cl, a.y) },
                { ImVec2(b.x - cl, a.y), ImVec2(b.x, a.y), ImVec2(b.x, a.y + cl) },
                { ImVec2(a.x, b.y - cl), ImVec2(a.x, b.y), ImVec2(a.x + cl, b.y) },
                { ImVec2(b.x - cl, b.y), b, ImVec2(b.x, b.y - cl) } };
            for (auto& p : pts) {
                if (outline) dl->AddPolyline(p, 3, shadow, 0, thick + 2.f);
                dl->AddPolyline(p, 3, bc, 0, thick);
            }
        } else {
            if (outline) dl->AddRect(ImVec2(a.x - 1, a.y - 1), ImVec2(b.x + 1, b.y + 1), shadow, round, 0, thick + 2.f);
            dl->AddRect(a, b, bc, round, 0, thick);
        }
    }

    // head marker
    int hm = menu::Mode("head_marker");   // 0 none 1 circle 2 dot 3 square
    if (hm > 0) {
        ImVec2 hp = v.boneOk[0] ? v.bones[0] : ImVec2(v.head.x, v.head.y + h * 0.08f);
        float r = (std::max)(2.f, h * 0.045f * menu::Value("head_size"));
        if (hm == 1) { if (outline) dl->AddCircle(hp, r, shadow, 16, 3.f); dl->AddCircle(hp, r, col, 16, 1.5f); }
        else if (hm == 2) dl->AddCircleFilled(hp, r * 0.6f, col);
        else { dl->AddRect(ImVec2(hp.x - r, hp.y - r), ImVec2(hp.x + r, hp.y + r), col, 0, 0, 1.5f); }
    }

    // health bar
    if (menu::IsOn("esp_health") && v.hp >= 0.f) {
        int pos = menu::Mode("hp_pos");   // 0 left 1 right 2 above 3 below
        float t = (std::max)(2.f, menu::Value("hp_thick"));
        float hp = (std::max)(0.f, (std::min)(1.f, v.hp));
        ImU32 fg = menu::IsOn("hp_gradient")
            ? (hp > 0.5f ? IM_COL32((int)(255 * (1 - hp) * 2), 220, 80, 255) : IM_COL32(240, (int)(220 * hp * 2), 60, 255))
            : menu::Color("col_hp");
        fg = WithAlpha(fg, v.alpha);
        ImU32 bg = WithAlpha(IM_COL32(0, 0, 0, 170), v.alpha);
        ImVec2 p0, p1, f0, f1, l0, l1;
        bool vertical = pos <= 1;
        if (vertical) {
            float x = pos == 0 ? a.x - 4.f - t : b.x + 4.f;
            p0 = ImVec2(x, a.y); p1 = ImVec2(x + t, b.y);
            f0 = ImVec2(x, b.y - h * hp); f1 = p1;
            l0 = ImVec2(x, b.y - h * (std::min)(1.f, hp + v.hpLost)); l1 = ImVec2(x + t, f0.y);
        } else {
            float y = pos == 2 ? a.y - 4.f - t : b.y + 4.f;
            p0 = ImVec2(a.x, y); p1 = ImVec2(b.x, y + t);
            f0 = p0; f1 = ImVec2(a.x + w * hp, y + t);
            l0 = ImVec2(f1.x, y); l1 = ImVec2(a.x + w * (std::min)(1.f, hp + v.hpLost), y + t);
        }
        dl->AddRectFilled(ImVec2(p0.x - 1, p0.y - 1), ImVec2(p1.x + 1, p1.y + 1), bg);
        if (v.hpLost > 0.001f && menu::IsOn("hp_recent")) dl->AddRectFilled(l0, l1, WithAlpha(IM_COL32(255, 255, 255, 200), v.alpha));
        dl->AddRectFilled(f0, f1, fg);
    }

    // labels
    std::vector<std::string> parts;
    if (menu::IsOn("lbl_name")) parts.push_back(v.name);
    if (menu::IsOn("lbl_state") && !v.state.empty()) parts.push_back(v.state);
    if (menu::IsOn("lbl_health")) {
        char hb[48];
        if (v.hpAbs >= 0 && v.hpMax > 0) snprintf(hb, sizeof hb, "%.0f/%.0f (%.0f%%)", v.hpAbs, v.hpMax, 100.0 * v.hpAbs / v.hpMax);
        else if (v.hpAbs >= 0) snprintf(hb, sizeof hb, "%.0f hp", v.hpAbs);
        else snprintf(hb, sizeof hb, "health ?");
        parts.push_back(hb);
    }
    if (menu::IsOn("lbl_weapon") && !v.weapon.empty()) parts.push_back(v.weapon);
    if (menu::IsOn("lbl_dist")) {
        char db[24];
        if (menu::Mode("lbl_units") == 1) snprintf(db, sizeof db, "%.0f ft", v.dist / 30.48);
        else snprintf(db, sizeof db, "%.0f m", v.dist / 100.0);
        parts.push_back(db);
    }
    if (!parts.empty()) {
        float sc = menu::Value("lbl_size");
        bool shadowOn = menu::IsOn("lbl_shadow");
        bool below = menu::Mode("lbl_pos") == 1;
        ImU32 tc = WithAlpha(menu::IsOn("lbl_catcolor") ? v.col : v.textCol, v.alpha);
        std::vector<std::string> lines;
        if (menu::IsOn("lbl_compact")) {
            std::string one;
            for (auto& p : parts) { if (!one.empty()) one += "  "; one += p; }
            lines.push_back(one);
        } else lines = parts;
        float lh = TextSize("Ag", sc).y + menu::Value("lbl_spacing");
        bool barAbove = menu::IsOn("esp_health") && v.hp >= 0 && menu::Mode("hp_pos") == 2;
        bool barBelow = menu::IsOn("esp_health") && v.hp >= 0 && menu::Mode("hp_pos") == 3;
        float gap = 3.f + ((below ? barBelow : barAbove) ? menu::Value("hp_thick") + 5.f : 0.f);
        float y = below ? b.y + gap : a.y - gap - lh * lines.size();
        for (auto& l : lines) {
            ImVec2 ts = TextSize(l.c_str(), sc);
            TextAt(dl, ImVec2(v.head.x - ts.x * 0.5f, y), tc, l.c_str(), sc, shadowOn);
            y += lh;
        }
    }
}

// sample people for the menu preview
void DrawPreviewImpl(ImDrawList* dl, ImVec2 p0, ImVec2 p1) {
    dl->AddRectFilled(p0, p1, IM_COL32(18, 22, 28, 255), 8.f);
    dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 18), 8.f);
    float W = p1.x - p0.x, H = p1.y - p0.y;
    struct S { float x, scale; Kind kind; State state; float hp; const char* name; const char* weapon; int vis; };
    const S samples[3] = { { 0.22f, 1.f, SUSPECT, ALIVE, 0.72f, "Suspect", "AK-47", 1 },
                           { 0.55f, 0.8f, CIVILIAN, SURRENDERED, 1.f, "Civilian", "", 0 },
                           { 0.82f, 0.62f, FRIEND, ALIVE, 0.4f, "Friend", "MP5", 1 } };
    // skeleton proportions (x offset, y share from head to feet) for the 16 bones
    static const float bx[kBones] = { 0, 0, 0, 0, -.32f, -.40f, -.42f, .32f, .40f, .42f, -.16f, -.18f, -.18f, .16f, .18f, .18f };
    static const float by[kBones] = { .07f, .15f, .30f, .50f, .2f, .34f, .47f, .2f, .34f, .47f, .55f, .74f, .95f, .55f, .74f, .95f };
    for (const S& s : samples) {
        Character c{};
        c.kind = s.kind; c.state = s.state; c.vis = s.vis;
        Visual v;
        float hgt = H * 0.62f * s.scale;
        float cx = p0.x + W * s.x, feetY = p0.y + H * 0.86f;
        v.head = ImVec2(cx, feetY - hgt);
        v.feet = ImVec2(cx, feetY);
        v.width = hgt * 0.42f;
        v.hasBones = true;
        for (int i = 0; i < kBones; ++i) { v.bones[i] = ImVec2(cx + bx[i] * v.width, v.head.y + by[i] * hgt); v.boneOk[i] = true; }
        v.col = ResolveColor(c, s.kind == SUSPECT, s.kind == FRIEND);
        v.textCol = menu::Color("col_text");
        v.hp = s.hp; v.hpMax = 100; v.hpAbs = s.hp * 100;
        v.hpLost = s.kind == SUSPECT ? 0.15f : 0.f;
        v.name = s.name; v.weapon = s.weapon; v.state = StateWord(c);
        v.dist = 1200.0 / s.scale;
        DrawVisual(dl, v);
    }
}

// ------------------------------------------------------------------------------------------
// Characters: filters, colours, distance fade, tracers, off-screen arrows, draw cap
// ------------------------------------------------------------------------------------------
std::map<uintptr_t, std::pair<float, double>> g_hpSeen;   // root -> (health share, time it dropped)
std::map<uintptr_t, float> g_hpLostShare;

bool KindShown(const Character& c, double dist) {
    switch (c.kind) {
        case SUSPECT:  if (!menu::IsOn("esp_suspects")) return false; return dist <= menu::Value("esp_dist") * 100.0;
        case CIVILIAN: if (!menu::IsOn("esp_civ")) return false; return dist <= menu::Value("esp_dist_civ") * 100.0;
        case SWAT:     if (!menu::IsOn("esp_swat")) return false; return dist <= menu::Value("esp_dist_team") * 100.0;
        case FRIEND:   if (!menu::IsOn("esp_friends")) return false; return dist <= menu::Value("esp_dist_team") * 100.0;
    }
    return false;
}

bool StateShown(const Character& c) {
    if (c.state == DEAD) return menu::IsOn("esp_down");
    if (c.state == ARRESTED) return menu::IsOn("esp_cuffed");
    if (c.state == SURRENDERED) return menu::IsOn("esp_surr");
    return true;
}

void DrawCharacters(const View& v) {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    const int style = menu::Mode("esp_style");
    const int selPid = menu::SelectedPlayerId();
    struct Item { const Character* c; double loc[3]; double dist; };
    std::vector<Item> items;
    for (const Character& c : g_chars) {
        Item it{ &c, {}, 0 };
        if (!CharLoc(c, it.loc)) continue;
        double dx = it.loc[0] - v.loc[0], dy = it.loc[1] - v.loc[1], dz = it.loc[2] - v.loc[2];
        it.dist = sqrt(dx * dx + dy * dy + dz * dz);
        if (!KindShown(c, it.dist) || !StateShown(c)) continue;
        if (menu::IsOn("esp_visonly") && c.vis == 0) continue;
        items.push_back(it);
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.dist < b.dist; });
    size_t cap = (size_t)(std::max)(1.f, menu::Value("esp_maxdraw"));
    if (items.size() > cap) items.resize(cap);

    double now = Now();
    for (const Item& it : items) {
        const Character& c = *it.c;
        bool aimT = c.root == g_aimTarget && g_aimTarget != 0;
        bool selT = c.kind == FRIEND && selPid >= 0 && c.pid == selPid;
        ImU32 col = ResolveColor(c, aimT, selT);
        float alpha = 1.f;
        if (menu::IsOn("esp_fade")) {
            double maxD = (std::max)(1000.0, (double)menu::Value("esp_dist") * 100.0);
            alpha = (float)(std::max)(0.3, 1.0 - it.dist / maxD * 0.7);
        }

        double head[3] = { it.loc[0], it.loc[1], it.loc[2] + c.halfHeight };
        double feet[3] = { it.loc[0], it.loc[1], it.loc[2] - c.halfHeight };
        ImVec2 h, f;
        bool onScreen = Project(v, head, h) && Project(v, feet, f) && f.y > 0 && h.y < ds.y && h.x > -50 && h.x < ds.x + 50;

        // tracers
        if (menu::IsOn("esp_lines") && onScreen && (!menu::IsOn("line_aimonly") || aimT || selT)) {
            int o = menu::Mode("line_origin");
            ImVec2 from = o == 1 ? ImVec2(v.cx, v.cy) : o == 2 ? ImVec2(ds.x * 0.5f, 0) : ImVec2(ds.x * 0.5f, ds.y);
            ImU32 lc = WithAlpha(col, menu::Value("line_alpha") * alpha);
            dl->AddLine(from, ImVec2(h.x, o == 2 ? h.y : f.y), lc, menu::Value("line_thick"));
        }
        // off-screen arrows
        if (!onScreen) {
            if (!menu::IsOn("esp_arrows")) continue;
            double dx = it.loc[0] - v.loc[0], dy = it.loc[1] - v.loc[1], dz = it.loc[2] - v.loc[2];
            double right = dx * v.ay[0] + dy * v.ay[1] + dz * v.ay[2];
            double fwd = dx * v.ax[0] + dy * v.ax[1] + dz * v.ax[2];
            double ang = atan2(right, fwd);
            float R = ds.y * 0.5f * menu::Value("arrow_radius");
            float sz = menu::Value("arrow_size");
            ImVec2 cc(v.cx + (float)sin(ang) * R, v.cy - (float)cos(ang) * R);
            ImVec2 tip(cc.x + (float)sin(ang) * sz, cc.y - (float)cos(ang) * sz);
            ImVec2 l(cc.x + (float)sin(ang - 2.4) * sz * 0.7f, cc.y - (float)cos(ang - 2.4) * sz * 0.7f);
            ImVec2 r(cc.x + (float)sin(ang + 2.4) * sz * 0.7f, cc.y - (float)cos(ang + 2.4) * sz * 0.7f);
            ImU32 ac = WithAlpha(col, menu::Value("arrow_alpha"));
            dl->AddTriangleFilled(tip, l, r, ac);
            if (menu::IsOn("arrow_dist")) {
                char b[16];
                snprintf(b, sizeof b, "%.0f", it.dist / 100.0);
                ImVec2 ts = TextSize(b, 0.8f);
                TextAt(dl, ImVec2(cc.x - ts.x * 0.5f, cc.y + sz * 0.6f), ac, b, 0.8f, true);
            }
            continue;
        }
        float height = f.y - h.y;
        if (height < menu::Value("esp_minsize")) continue;

        Visual vis;
        vis.head = h; vis.feet = f;
        vis.width = height * (c.radius / c.halfHeight) * 1.1f * stretch::Applied();
        vis.col = col;
        vis.textCol = menu::Color("col_text");
        vis.alpha = alpha;
        vis.hp = c.health; vis.hpAbs = c.hpAbs; vis.hpMax = c.hpMax;
        vis.name = c.kind == FRIEND && !c.name.empty() ? c.name : KindWord(c);
        vis.state = StateWord(c);
        vis.weapon = c.weapon;
        vis.dist = it.dist;
        // recent damage: the share lost in the last second
        auto seen = g_hpSeen.find(c.root);
        if (c.health >= 0) {
            if (seen == g_hpSeen.end()) g_hpSeen[c.root] = { c.health, 0.0 };
            else if (c.health < seen->second.first - 0.001f) { g_hpLostShare[c.root] = seen->second.first - c.health; seen->second = { c.health, now }; }
            else seen->second.first = c.health;
            auto s2 = g_hpSeen.find(c.root);
            double age = now - s2->second.second;
            vis.hpLost = age < 1.0 ? g_hpLostShare[c.root] * (float)(1.0 - age) : 0.f;
        }
        if (c.hasBones && (style == 4 || style == 5 || menu::Mode("head_marker") > 0 || menu::IsOn("skel_joints"))) {
            for (int i = 0; i < kBones; ++i) {
                if (!c.boneOk[i]) continue;
                double w[3] = { it.loc[0] + c.bone[i][0], it.loc[1] + c.bone[i][1], it.loc[2] + c.bone[i][2] };
                vis.boneOk[i] = Project(v, w, vis.bones[i]);
                vis.hasBones |= vis.boneOk[i];
            }
        }
        if (style == 3) {
            double rot[3] = { 0, 0, 0 };
            SafeRead(c.root + g_rotOff, rot, sizeof rot);
            vis.is3D = Box3D(dl, v, it.loc, Sane(rot[1]) ? rot[1] : 0.0, c.radius, c.halfHeight, WithAlpha(col, alpha));
        }
        DrawVisual(dl, vis);
    }
}

}  // namespace

namespace esp {

bool FeedWanted() {
    return menu::IsOn("esp") || menu::IsOn("aimbot") || menu::IsOn("trigger") ||
           menu::IsOn("radar") || menu::IsOn("hitmarkers") || menu::IsOn("grenade_arc") || menu::IsOn("sound_markers");
}

void Tick() {
    DWORD t = GetTickCount();
    if (FxWanted() && t - g_fxRead >= 50) { g_fxRead = t; ReadFx(); g_adsAim = g_ads; }
    if (!FeedWanted()) { g_cam = 0; g_pc = 0; g_chars.clear(); g_objects.clear(); return; }
    DWORD now = GetTickCount();
    if (now - g_lastRead < 80) return;
    g_lastRead = now;
    ReadFile_();
}

void Draw() {
    // simulated clicks never stay held when the menu opens or the game loses focus
    if (g_clickDown && (menu::IsOpen() || !GameFocused())) Click(false);
    ReleaseClick();
    if (!FeedWanted() || !g_cam) return;
    View v;
    if (!BuildView(v)) return;
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    RunAimbot(v);
    RunTrigger(v);
    DrawArc(bg, v);
    DrawPings(bg, v);
    DrawAimOverlay(bg, v);
    DrawHitmarkers(ImGui::GetForegroundDrawList(), v);
    DrawRadar(v);
    if (!menu::IsOn("esp")) return;
    DrawNades(bg, v);

    const int style = menu::Mode("esp_style");
    const bool showDown = menu::IsOn("esp_down");
    const bool lines = menu::IsOn("esp_lines");
    const bool health = menu::IsOn("esp_health");
    const double maxDist = menu::Value("esp_dist") * 100.0;   // m -> cm
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImVec2 ds = ImGui::GetIO().DisplaySize;

    for (const Object& o : g_objects) DrawObject(dl, v, o);
    DrawCharacters(v);
}

void DrawPreview(ImDrawList* dl, ImVec2 a, ImVec2 b) { DrawPreviewImpl(dl, a, b); }

}  // namespace esp
