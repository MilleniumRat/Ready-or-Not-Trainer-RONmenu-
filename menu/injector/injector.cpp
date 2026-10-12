// Made by MilleniumRat (github.com/MilleniumRat)
// Loads RonMenu.dll into a running Ready Or Not process. If a RonMenu is already running it is asked
// to unload first, so running this after a rebuild swaps in the new version.
//   injector.exe                -> injects RonMenu.dll sitting next to injector.exe
//   injector.exe C:\path\x.dll  -> injects that DLL
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <string>

static DWORD FindGame() {
    const wchar_t* names[] = { L"ReadyOrNotSteam-Win64-Shipping.exe", L"ReadyOrNot-Win64-Shipping.exe" };
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{ sizeof pe };
    DWORD pid = 0;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !pid; ok = Process32NextW(snap, &pe))
        for (auto* n : names)
            if (_wcsicmp(pe.szExeFile, n) == 0) { pid = pe.th32ProcessID; break; }
    CloseHandle(snap);
    return pid;
}

// Any RonMenu*.dll in the game (older builds, or copies this injector made)?
static bool AnyLoaded(DWORD pid) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W me{ sizeof me };
    bool found = false;
    for (BOOL ok = Module32FirstW(snap, &me); ok && !found; ok = Module32NextW(snap, &me))
        found = _wcsnicmp(me.szModule, L"RonMenu", 7) == 0;
    CloseHandle(snap);
    return found;
}

// Ask the running copy to unload itself (the menu reads bridge\menu_req.txt), then wait for it.
static bool UnloadRunning(DWORD pid) {
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return false;
    wchar_t exe[MAX_PATH];
    DWORD len = MAX_PATH;
    BOOL ok = QueryFullProcessImageNameW(proc, 0, exe, &len);
    CloseHandle(proc);
    if (!ok) return false;
    std::wstring dir = exe;
    dir.resize(dir.find_last_of(L"\\/"));
    std::wstring bridge = dir + L"\\ue4ss\\Mods\\RonTrainer\\bridge";
    CreateDirectoryW(bridge.c_str(), nullptr);
    std::wstring tmp = bridge + L"\\menu_req.inj", req = bridge + L"\\menu_req.txt";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return false;
    fputs("id 1\nunload\n", f);
    fclose(f);
    MoveFileExW(tmp.c_str(), req.c_str(), MOVEFILE_REPLACE_EXISTING);
    wprintf(L"An older RonMenu is running: asking it to unload...\n");
    for (int i = 0; i < 150; ++i) {   // up to 15 s (the menu only reads requests while the game renders)
        Sleep(100);
        if (!AnyLoaded(pid)) { Sleep(500); return true; }
    }
    DeleteFileW(req.c_str());
    return false;
}

// Each inject loads a fresh copy with its own name: Windows would otherwise hand back the old,
// still-mapped module for the same path. Old copies that are no longer loaded get deleted.
static std::wstring FreshCopy(const std::wstring& dll) {
    std::wstring dir = dll.substr(0, dll.find_last_of(L"\\/") + 1);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"RonMenu_*.dll").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do { DeleteFileW((dir + fd.cFileName).c_str()); } while (FindNextFileW(h, &fd));   // fails silently if in use
        FindClose(h);
    }
    wchar_t name[64];
    swprintf(name, 64, L"RonMenu_%llu.dll", (unsigned long long)GetTickCount64());
    std::wstring copy = dir + name;
    if (!CopyFileW(dll.c_str(), copy.c_str(), FALSE)) return dll;
    return copy;
}

// The game's main window (Unreal), once it is visible.
static HWND g_found;
static BOOL CALLBACK FindWindowCb(HWND h, LPARAM pid) {
    DWORD p = 0;
    GetWindowThreadProcessId(h, &p);
    wchar_t cls[64];
    if (p == (DWORD)pid && IsWindowVisible(h) && GetClassNameW(h, cls, 64) && wcscmp(cls, L"UnrealWindow") == 0) {
        g_found = h;
        return FALSE;
    }
    return TRUE;
}

// --wait: sit in the background until the game is up, then inject.
static DWORD WaitForGame() {
    wprintf(L"Waiting for Ready Or Not to start...\n");
    DWORD pid = 0;
    while (!(pid = FindGame())) Sleep(2000);
    wprintf(L"Game started (pid %lu), waiting for its window...\n", pid);
    for (;;) {
        g_found = nullptr;
        EnumWindows(FindWindowCb, (LPARAM)pid);
        if (g_found) break;
        if (!FindGame()) return 0;   // game closed again
        Sleep(1000);
    }
    wprintf(L"Window is up, giving the renderer a few seconds...\n");
    Sleep(15000);
    return FindGame();
}

int wmain(int argc, wchar_t** argv) {
    bool wait = false;
    std::wstring dll;
    if (argc > 1 && wcscmp(argv[1], L"--wait") == 0) { wait = true; --argc; ++argv; }
    if (argc > 1) {
        dll = argv[1];
    } else {
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        dll = self;
        dll.resize(dll.find_last_of(L"\\/") + 1);
        dll += L"RonMenu.dll";
    }
    wchar_t full[MAX_PATH];
    GetFullPathNameW(dll.c_str(), MAX_PATH, full, nullptr);
    dll = full;
    if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        wprintf(L"DLL not found: %s\n", dll.c_str());
        return 1;
    }

    DWORD pid = wait ? WaitForGame() : FindGame();
    if (!pid) { wprintf(L"Ready Or Not is not running. Start the game first (or use autoinject.bat).\n"); return 1; }
    wprintf(L"Game found, pid %lu\n", pid);

    if (AnyLoaded(pid) && !UnloadRunning(pid)) {
        wprintf(L"The running RonMenu did not unload. Press END in the game (unload key), then run this again.\n");
        return 1;
    }
    dll = FreshCopy(dll);

    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                              PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!proc) { wprintf(L"OpenProcess failed (%lu). Try running as administrator.\n", GetLastError()); return 1; }

    SIZE_T bytes = (dll.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote || !WriteProcessMemory(proc, remote, dll.c_str(), bytes, nullptr)) {
        wprintf(L"Could not write into the game process (%lu).\n", GetLastError());
        CloseHandle(proc);
        return 1;
    }
    auto loadLib = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    HANDLE th = CreateRemoteThread(proc, nullptr, 0, loadLib, remote, 0, nullptr);
    if (!th) { wprintf(L"CreateRemoteThread failed (%lu).\n", GetLastError()); CloseHandle(proc); return 1; }
    WaitForSingleObject(th, 10000);
    DWORD code = 0;
    GetExitCodeThread(th, &code);
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    CloseHandle(th);
    CloseHandle(proc);

    if (!code) { wprintf(L"LoadLibrary failed inside the game (missing dependency or blocked).\n"); return 1; }
    wprintf(L"Injected. Press INSERT in the game to open the menu.\n");
    return 0;
}
