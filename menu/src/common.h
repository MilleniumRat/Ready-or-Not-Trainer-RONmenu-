// Made by MilleniumRat (github.com/MilleniumRat)
#pragma once
#include <windows.h>
#include <string>

// Writes a line to ronmenu.log (next to the Lua mod). Safe from any thread.
void Log(const char* fmt, ...);
void LogInit(const std::wstring& path);

struct ImDrawList;
struct ImVec2;

namespace menu {
void Init(const std::wstring& modDir);
void Shutdown();             // joins background threads; call before the DLL unloads
void Draw();                 // call between ImGui::NewFrame and ImGui::Render
void Tick();                 // call once per frame: flushes state, polls Lua status
bool IsOpen();
void SetOpen(bool open);
bool OnKeyDown(unsigned vk); // keys + mouse 3/4/5: menu key, unload, binds, bind capture; true if used
bool UnloadRequested();
void RequestUnload();
void SetupStyle();               // colours / rounding, once after the ImGui context exists
void LoadFonts(float scale);     // (re)build the font atlas; only while no renderer backend is active
void ApplyScale(float scale);    // scale style sizes to match the fonts
bool IsOn(const char* id);
float Value(const char* id);
float UiScale();   // interface size chosen in Settings (1 = default)
int Mode(const char* id);
unsigned Color(const char* id);   // ImU32 RGBA from a colour setting
int SelectedPlayerId();           // Players page selection (PlayerState.PlayerId), -1 none
const std::wstring& ModDir();
}

namespace esp {
void Tick();   // re-reads esp.txt from the Lua mod a few times per second
void Draw();   // reads positions from game memory and draws boxes; call inside an ImGui frame
void DrawPreview(ImDrawList* dl, ImVec2 a, ImVec2 b);   // ESP look with sample people   // left click reached the game window (silent aim); game thread
}
