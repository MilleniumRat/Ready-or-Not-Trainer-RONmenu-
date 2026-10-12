// Made by MilleniumRat (github.com/MilleniumRat)
#include "common.h"
#include "overlay.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>

static HMODULE g_mod;
static HANDLE g_instance;   // "already injected" marker, released on unload so a new version can load

static void ReleaseInstance() {
    if (g_instance) { CloseHandle(g_instance); g_instance = nullptr; }
}
static std::wstring g_logPath;
static std::mutex g_logMtx;

void LogInit(const std::wstring& path) { g_logPath = path; }

void Log(const char* fmt, ...) {
    if (g_logPath.empty()) return;
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    SYSTEMTIME st; GetLocalTime(&st);
    std::lock_guard<std::mutex> lk(g_logMtx);
    FILE* f = _wfopen(g_logPath.c_str(), L"ab");
    if (!f) return;
    fprintf(f, "%02d:%02d:%02d.%03d %s\r\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
    fclose(f);
}

static DWORD WINAPI MainThread(LPVOID) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    dir.resize(dir.find_last_of(L"\\/"));
    std::wstring modDir = dir + L"\\ue4ss\\Mods\\RonTrainer";
    bool haveMod = GetFileAttributesW(modDir.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (!haveMod) modDir = dir;   // no UE4SS: menu still works, nothing reads its state

    LogInit(modDir + L"\\ronmenu.log");
    DeleteFileW((modDir + L"\\ronmenu.log").c_str());
    Log("RonMenu loaded, pid %lu, mod dir %ls (UE4SS mod present: %d)", GetCurrentProcessId(), modDir.c_str(), haveMod);

    menu::Init(modDir);
    if (!overlay::Install()) {
        Log("overlay install failed, unloading");
        menu::Shutdown();
        ReleaseInstance();
        FreeLibraryAndExitThread(g_mod, 0);
    }
    Log("running; INSERT opens the menu");
    while (!menu::UnloadRequested()) Sleep(100);

    Log("unloading");
    overlay::Uninstall();
    menu::Shutdown();   // wait for background work (animation list) before the code goes away
    Log("unloaded");
    ReleaseInstance();
    FreeLibraryAndExitThread(g_mod, 0);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_mod = mod;
        DisableThreadLibraryCalls(mod);
        wchar_t name[64];
        // new name: builds before this one never released their marker
        swprintf(name, 64, L"Local\\RonMenu2_%lu", GetCurrentProcessId());
        HANDLE m = CreateMutexW(nullptr, FALSE, name);
        if (GetLastError() == ERROR_ALREADY_EXISTS) { if (m) CloseHandle(m); return TRUE; }   // another copy is running
        g_instance = m;
        CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
    }
    return TRUE;
}
