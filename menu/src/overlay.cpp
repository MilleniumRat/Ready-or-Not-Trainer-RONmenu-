// Made by MilleniumRat (github.com/MilleniumRat)
// D3D11 / D3D12 ImGui overlay for a game we did not write.
//
// How it works:
//  1. Create throw-away D3D11 and D3D12 swap chains / queues just to read their vtables.
//     COM vtables are shared by every object of the same implementation class, so patching
//     a slot in that vtable also hooks the game's own swap chain and command queue.
//  2. Hook IDXGISwapChain::Present (+Present1, ResizeBuffers, ResizeBuffers1) and
//     ID3D12CommandQueue::ExecuteCommandLists (only to learn the game's direct queue).
//  3. On the first Present, look at which device the swap chain has (D3D12 or D3D11), set up the
//     matching ImGui backend and draw on top of the back buffer before it is presented.
//  4. Subclass the game window so we get keyboard/mouse input and can hide it from the game
//     while the menu is open.
#include "overlay.h"
#include "common.h"
#include "stretch.h"

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <atomic>
#include <mutex>
#include <vector>

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_dx12.h"
#include "backends/imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

// ---------------------------------------------------------------------------------------------
// vtable patching
// ---------------------------------------------------------------------------------------------
constexpr int kMaxSlots = 48;
struct PatchedVT {
    void** vt;
    void*  orig[kMaxSlots];
    void*  hook[kMaxSlots];
    bool   used[kMaxSlots];
};
std::vector<PatchedVT*> g_vts;

void* OrigOf(void* self, int idx) {
    void** vt = *reinterpret_cast<void***>(self);
    for (auto* v : g_vts)
        if (v->vt == vt) return v->orig[idx];
    return nullptr;
}

bool PatchSlot(void** vt, int idx, void* hook) {
    PatchedVT* e = nullptr;
    for (auto* v : g_vts) if (v->vt == vt) e = v;
    if (!e) { e = new PatchedVT{}; e->vt = vt; g_vts.push_back(e); }
    if (e->used[idx]) return true;
    DWORD old;
    if (!VirtualProtect(&vt[idx], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return false;
    e->orig[idx] = vt[idx];
    e->hook[idx] = hook;
    e->used[idx] = true;
    vt[idx] = hook;
    VirtualProtect(&vt[idx], sizeof(void*), old, &old);
    return true;
}

void UnpatchAll() {
    for (auto* v : g_vts) {
        for (int i = 0; i < kMaxSlots; ++i) {
            if (!v->used[i]) continue;
            DWORD old;
            VirtualProtect(&v->vt[i], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
            if (v->vt[i] == v->hook[i]) v->vt[i] = v->orig[i];
            VirtualProtect(&v->vt[i], sizeof(void*), old, &old);
        }
        delete v;
    }
    g_vts.clear();
}

// ---------------------------------------------------------------------------------------------
// IAT patching (ClipCursor / SetCursorPos while the menu is open)
// ---------------------------------------------------------------------------------------------
struct IatPatch { void** slot; void* orig; void* hook; };
std::vector<IatPatch> g_iat;

void* IatHook(HMODULE mod, const char* dll, const char* func, void* hook) {
    auto* base = reinterpret_cast<BYTE*>(mod);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto* d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d) {
        if (_stricmp(reinterpret_cast<char*>(base + d->Name), dll) != 0) continue;
        auto* oft = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->OriginalFirstThunk);
        auto* ft = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->FirstThunk);
        for (; oft->u1.AddressOfData; ++oft, ++ft) {
            if (IMAGE_SNAP_BY_ORDINAL(oft->u1.Ordinal)) continue;
            auto* ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + oft->u1.AddressOfData);
            if (strcmp(ibn->Name, func) != 0) continue;
            void** slot = reinterpret_cast<void**>(&ft->u1.Function);
            DWORD old;
            VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old);
            void* orig = *slot;
            *slot = hook;
            VirtualProtect(slot, sizeof(void*), old, &old);
            g_iat.push_back({ slot, orig, hook });
            return orig;
        }
    }
    return nullptr;
}

void IatUnhookAll() {
    for (auto& p : g_iat) {
        DWORD old;
        VirtualProtect(p.slot, sizeof(void*), PAGE_READWRITE, &old);
        if (*p.slot == p.hook) *p.slot = p.orig;
        VirtualProtect(p.slot, sizeof(void*), old, &old);
    }
    g_iat.clear();
}

// ---------------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------------
enum class Api { None, DX11, DX12 };

struct Frame12 { ID3D12CommandAllocator* alloc = nullptr; UINT64 fenceVal = 0; };

struct State {
    std::mutex mtx;
    std::atomic<int> inflight{0};
    std::atomic<bool> shuttingDown{false};
    std::atomic<bool> failed{false};
    std::atomic<bool> presentSeen{false};

    HWND hwnd = nullptr;
    WNDPROC origProc = nullptr;
    bool imguiCtx = false;
    bool win32Init = false;
    float fontScale = 0.f;   // scale the font atlas was last built for

    Api api = Api::None;
    IDXGISwapChain* scPtr = nullptr;   // identity only, no reference held
    bool menuWasOpen = false;
    RECT savedClip{};
    bool hadClip = false;

    // D3D12
    ID3D12Device* dev12 = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12DescriptorHeap* rtvHeap = nullptr;
    ID3D12DescriptorHeap* srvHeap = nullptr;
    ID3D12GraphicsCommandList* cl = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE fenceEvent = nullptr;
    UINT64 fenceCounter = 0;
    UINT64 frameNo = 0;
    std::vector<Frame12> frames;
    UINT rtvInc = 0;
    DXGI_FORMAT rtvFmt = DXGI_FORMAT_UNKNOWN;

    bool imgui12Init = false;
    bool imgui11Init = false;

    // D3D11
    ID3D11Device* dev11 = nullptr;
    ID3D11DeviceContext* ctx11 = nullptr;
} g;

thread_local bool t_inPresent = false;

struct InflightGuard {
    InflightGuard() { ++g.inflight; }
    ~InflightGuard() { --g.inflight; }
};

DXGI_FORMAT Typed(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:     return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:     return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:  return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        default: return f;
    }
}

template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

// ---------------------------------------------------------------------------------------------
// D3D12 backend
// ---------------------------------------------------------------------------------------------
void WaitGpu12() {
    if (!g.queue || !g.fence || !g.fenceEvent) return;
    UINT64 v = ++g.fenceCounter;
    if (SUCCEEDED(g.queue->Signal(g.fence, v)) && g.fence->GetCompletedValue() < v) {
        g.fence->SetEventOnCompletion(v, g.fenceEvent);
        WaitForSingleObject(g.fenceEvent, 1000);
    }
}

bool Init12(IDXGISwapChain* sc) {
    if (FAILED(sc->GetDevice(IID_PPV_ARGS(&g.dev12)))) return false;
    DXGI_SWAP_CHAIN_DESC d{};
    sc->GetDesc(&d);

    // A D3D12 swap chain hands out the command queue it was created with.
    if (FAILED(sc->GetDevice(IID_PPV_ARGS(&g.queue))) || !g.queue) {
        Log("could not get the swap chain's command queue");
        return false;
    }
    {
        D3D12_COMMAND_QUEUE_DESC qd = g.queue->GetDesc();
        Log("swap chain queue %p type %d", g.queue, (int)qd.Type);
    }

    UINT n = d.BufferCount;
    g.rtvFmt = Typed(d.BufferDesc.Format);

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = n;
    if (FAILED(g.dev12->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.rtvHeap)))) return false;
    g.rtvInc = g.dev12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 1;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g.dev12->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.srvHeap)))) return false;

    g.frames.assign(n, Frame12{});
    for (auto& f : g.frames)
        if (FAILED(g.dev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.alloc)))) return false;
    if (FAILED(g.dev12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.frames[0].alloc, nullptr, IID_PPV_ARGS(&g.cl)))) return false;
    g.cl->Close();
    if (FAILED(g.dev12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence)))) return false;
    g.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g.fenceCounter = 0;
    g.frameNo = 0;

    ImGui_ImplDX12_InitInfo ii;
    ii.Device = g.dev12;
    ii.CommandQueue = g.queue;
    ii.NumFramesInFlight = (int)n;
    ii.RTVFormat = g.rtvFmt;
    ii.SrvDescriptorHeap = g.srvHeap;
    ii.LegacySingleSrvCpuDescriptor = g.srvHeap->GetCPUDescriptorHandleForHeapStart();
    ii.LegacySingleSrvGpuDescriptor = g.srvHeap->GetGPUDescriptorHandleForHeapStart();
    if (!ImGui_ImplDX12_Init(&ii)) return false;
    g.imgui12Init = true;
    Log("D3D12 backend ready: %u buffers, format %d", n, (int)g.rtvFmt);
    return true;
}

void Cleanup12() {
    if (g.imgui12Init) {
        WaitGpu12();
        ImGui_ImplDX12_Shutdown();
        g.imgui12Init = false;
    }
    if (g.api != Api::DX12 && !g.dev12) return;
    for (auto& f : g.frames) SafeRelease(f.alloc);
    g.frames.clear();
    SafeRelease(g.cl);
    SafeRelease(g.fence);
    if (g.fenceEvent) { CloseHandle(g.fenceEvent); g.fenceEvent = nullptr; }
    SafeRelease(g.rtvHeap);
    SafeRelease(g.srvHeap);
    SafeRelease(g.queue);
    SafeRelease(g.dev12);
    g.api = Api::None;
}

void Render12(IDXGISwapChain* sc) {
    IDXGISwapChain3* sc3 = nullptr;
    if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&sc3)))) return;
    UINT idx = sc3->GetCurrentBackBufferIndex();
    sc3->Release();
    if (idx >= g.frames.size()) return;
    // The allocator ring advances once per rendered frame, in step with ImGui's own buffer ring.
    Frame12& f = g.frames[g.frameNo % g.frames.size()];
    if (g.frameNo < 3) Log("render frame %llu (backbuffer %u)", (unsigned long long)g.frameNo, idx);
    ++g.frameNo;

    if (g.fence->GetCompletedValue() < f.fenceVal) {
        g.fence->SetEventOnCompletion(f.fenceVal, g.fenceEvent);
        WaitForSingleObject(g.fenceEvent, 1000);
    }

    ID3D12Resource* bb = nullptr;
    if (FAILED(sc->GetBuffer(idx, IID_PPV_ARGS(&bb)))) return;

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += (SIZE_T)idx * g.rtvInc;
    D3D12_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format = g.rtvFmt;
    rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    g.dev12->CreateRenderTargetView(bb, &rd, rtv);

    f.alloc->Reset();
    g.cl->Reset(f.alloc, nullptr);

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = bb;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g.cl->ResourceBarrier(1, &b);

    g.cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    ID3D12DescriptorHeap* heaps[] = { g.srvHeap };
    g.cl->SetDescriptorHeaps(1, heaps);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g.cl);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g.cl->ResourceBarrier(1, &b);
    g.cl->Close();

    ID3D12CommandList* lists[] = { g.cl };
    g.queue->ExecuteCommandLists(1, lists);

    f.fenceVal = ++g.fenceCounter;
    g.queue->Signal(g.fence, f.fenceVal);
    bb->Release();
}

// ---------------------------------------------------------------------------------------------
// D3D11 backend
// ---------------------------------------------------------------------------------------------
bool Init11(IDXGISwapChain* sc) {
    if (FAILED(sc->GetDevice(IID_PPV_ARGS(&g.dev11)))) return false;
    g.dev11->GetImmediateContext(&g.ctx11);
    if (!ImGui_ImplDX11_Init(g.dev11, g.ctx11)) return false;
    g.imgui11Init = true;
    Log("D3D11 backend ready");
    return true;
}

void Cleanup11() {
    if (g.imgui11Init) { ImGui_ImplDX11_Shutdown(); g.imgui11Init = false; }
    stretch::Release11();
    if (g.api != Api::DX11 && !g.dev11) return;
    SafeRelease(g.ctx11);
    SafeRelease(g.dev11);
    g.api = Api::None;
}

void Render11(IDXGISwapChain* sc) {
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&bb)))) return;
    ID3D11RenderTargetView* rtv = nullptr;
    g.dev11->CreateRenderTargetView(bb, nullptr, &rtv);
    if (rtv) stretch::Apply11(g.dev11, g.ctx11, bb, rtv, menu::IsOn("stretch") ? menu::Value("stretch") : 1.f);
    bb->Release();
    if (!rtv) return;
    g.ctx11->OMSetRenderTargets(1, &rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    rtv->Release();
}

void CleanupBackend() {
    Cleanup12();
    Cleanup11();
    g.scPtr = nullptr;
}

// ---------------------------------------------------------------------------------------------
// Window / input
// ---------------------------------------------------------------------------------------------
typedef BOOL(WINAPI* PFN_ClipCursor)(const RECT*);
typedef BOOL(WINAPI* PFN_SetCursorPos)(int, int);
PFN_ClipCursor g_origClip = nullptr;
PFN_SetCursorPos g_origSetPos = nullptr;

BOOL WINAPI hkClipCursor(const RECT* r) { return menu::IsOpen() ? TRUE : g_origClip(r); }
BOOL WINAPI hkSetCursorPos(int x, int y) { return menu::IsOpen() ? TRUE : g_origSetPos(x, y); }

void OnMenuToggled(bool open) {
    if (open) {
        g.hadClip = GetClipCursor(&g.savedClip) != 0;
        ClipCursor(nullptr);
    } else {
        if (g.hadClip) ClipCursor(&g.savedClip);
    }
}

bool IsSwallowed(UINT m) {
    switch (m) {
        case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK:
        case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_CHAR: case WM_SYSCHAR:
        case WM_INPUT:
            return true;
    }
    return false;
}

LRESULT CALLBACK hkWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (g.shuttingDown) return CallWindowProcW(g.origProc, h, m, w, l);
    InflightGuard guard;

    // Every key and the extra mouse buttons go to the menu first: menu key, unload key, binds,
    // and key capture while a bind is being set.
    unsigned vk = 0;
    if ((m == WM_KEYDOWN || m == WM_SYSKEYDOWN) && !(l & (1 << 30))) vk = (unsigned)w;
    else if (m == WM_MBUTTONDOWN) vk = VK_MBUTTON;
    else if (m == WM_XBUTTONDOWN) vk = GET_XBUTTON_WPARAM(w) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2;
    if (vk) {
        bool wasOpen = menu::IsOpen();
        if (menu::OnKeyDown(vk)) {
            if (menu::UnloadRequested() && wasOpen) OnMenuToggled(false);
            return m == WM_XBUTTONDOWN ? TRUE : 0;
        }
    }

    if (g.win32Init) ImGui_ImplWin32_WndProcHandler(h, m, w, l);


    if (menu::IsOpen()) {
        if (m == WM_SETCURSOR) return TRUE;
        if (IsSwallowed(m)) return DefWindowProcW(h, m, w, l);
    }
    return CallWindowProcW(g.origProc, h, m, w, l);
}

// ---------------------------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------------------------
// UI scale from the game window height (1.0 at 1080p).
float ScaleFor(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    float s = (float)(rc.bottom - rc.top) / 1080.f;
    if (s < 1.f) s = 1.f;
    if (s > 2.5f) s = 2.5f;
    return s;
}

void SetupImGui(HWND hwnd) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    g.imguiCtx = true;
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    menu::SetupStyle();
    g.win32Init = ImGui_ImplWin32_Init(hwnd);
    Log("ImGui context ready, win32 init %d", (int)g.win32Init);
}

void HookWindow(HWND hwnd) {
    g.hwnd = hwnd;
    g.origProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(hkWndProc)));
    HMODULE exe = GetModuleHandleW(nullptr);
    g_origClip = reinterpret_cast<PFN_ClipCursor>(IatHook(exe, "user32.dll", "ClipCursor", reinterpret_cast<void*>(hkClipCursor)));
    g_origSetPos = reinterpret_cast<PFN_SetCursorPos>(IatHook(exe, "user32.dll", "SetCursorPos", reinterpret_cast<void*>(hkSetCursorPos)));
    Log("window hooked (hwnd %p), IAT ClipCursor %s, SetCursorPos %s", hwnd,
        g_origClip ? "ok" : "missing", g_origSetPos ? "ok" : "missing");
}

void RenderFrame(IDXGISwapChain* sc) {
    DXGI_SWAP_CHAIN_DESC d{};
    if (FAILED(sc->GetDesc(&d)) || !d.OutputWindow) return;

    if (g.scPtr && g.scPtr != sc) { Log("swap chain changed, re-initializing backend"); CleanupBackend(); }

    if (!g.imguiCtx) SetupImGui(d.OutputWindow);
    if (!g.hwnd) HookWindow(d.OutputWindow);
    else if (g.hwnd != d.OutputWindow) return;   // not the game window

    // interface size changed in Settings: rebuild the backend so the fonts are re-rasterized
    if (g.api != Api::None && g.fontScale > 0.f && fabsf(ScaleFor(d.OutputWindow) * menu::UiScale() - g.fontScale) > 0.001f) {
        Log("interface size changed, rebuilding fonts");
        CleanupBackend();
    }

    if (g.api == Api::None) {
        // (re)build the font atlas at the right pixel size before the backend uploads it
        float fs = ScaleFor(d.OutputWindow) * menu::UiScale();
        if (fs != g.fontScale) { menu::LoadFonts(fs); g.fontScale = fs; }
        ID3D12Device* t12 = nullptr;
        ID3D11Device* t11 = nullptr;
        if (SUCCEEDED(sc->GetDevice(IID_PPV_ARGS(&t12)))) {
            t12->Release();
            g.api = Api::DX12;
            if (!Init12(sc)) { Log("D3D12 init failed"); CleanupBackend(); g.failed = true; return; }
        } else if (SUCCEEDED(sc->GetDevice(IID_PPV_ARGS(&t11)))) {
            t11->Release();
            g.api = Api::DX11;
            if (!Init11(sc)) { Log("D3D11 init failed"); CleanupBackend(); g.failed = true; return; }
        } else {
            return;
        }
        g.scPtr = sc;
    }

    bool open = menu::IsOpen();
    if (open != g.menuWasOpen) { g.menuWasOpen = open; OnMenuToggled(open); }
    if (open) ClipCursor(nullptr);

    menu::ApplyScale(g.fontScale);
    menu::Tick();
    ImGuiIO& io = ImGui::GetIO();
    if (open) io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    else      io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;   // never touch the game's cursor while closed

    if (g.api == Api::DX12) ImGui_ImplDX12_NewFrame(); else ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    esp::Tick();
    esp::Draw();
    menu::Draw();
    ImGui::Render();

    if (g.api == Api::DX12) {
        stretch::SetUnsupported(menu::IsOn("stretch")
            ? "none: stretched resolution needs DirectX 11 (Steam launch option -dx11)" : "off");
        Render12(sc);
    } else {
        Render11(sc);
    }
}

void RenderGuarded(IDXGISwapChain* sc) {
    __try {
        RenderFrame(sc);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g.failed = true;
    }
}

void OnPresent(IDXGISwapChain* sc, UINT flags) {
    if (t_inPresent || g.failed || g.shuttingDown || (flags & DXGI_PRESENT_TEST)) return;
    t_inPresent = true;
    {
        std::lock_guard<std::mutex> lk(g.mtx);
        if (!g.presentSeen.exchange(true)) Log("first Present seen");
        if (!g.shuttingDown) RenderGuarded(sc);
        if (g.failed) Log("overlay crashed or failed to initialize; it is now disabled for this session");
    }
    t_inPresent = false;
}

// ---------------------------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE hkPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    InflightGuard guard;
    OnPresent(sc, flags);
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
    return reinterpret_cast<Fn>(OrigOf(sc, 8))(sc, sync, flags);
}

HRESULT STDMETHODCALLTYPE hkPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) {
    InflightGuard guard;
    OnPresent(sc, flags);
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
    return reinterpret_cast<Fn>(OrigOf(sc, 22))(sc, sync, flags, p);
}

void OnResize() {
    std::lock_guard<std::mutex> lk(g.mtx);
    if (g.api != Api::None) { Log("ResizeBuffers: releasing backend"); CleanupBackend(); }
}

HRESULT STDMETHODCALLTYPE hkResizeBuffers(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl) {
    InflightGuard guard;
    OnResize();
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
    return reinterpret_cast<Fn>(OrigOf(sc, 13))(sc, n, w, h, f, fl);
}

HRESULT STDMETHODCALLTYPE hkResizeBuffers1(IDXGISwapChain3* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl,
                                           const UINT* mask, IUnknown* const* queues) {
    InflightGuard guard;
    OnResize();
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*, IUnknown* const*);
    return reinterpret_cast<Fn>(OrigOf(sc, 39))(sc, n, w, h, f, fl, mask, queues);
}

// ---------------------------------------------------------------------------------------------
// Dummy objects to read vtables
// ---------------------------------------------------------------------------------------------
LRESULT CALLBACK DummyProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

struct VTables {
    void** swap12 = nullptr;
    void** queue12 = nullptr;
    void** swap11 = nullptr;
};

VTables GetVTables() {
    VTables out;
    WNDCLASSEXW wc{ sizeof wc };
    wc.lpfnWndProc = DummyProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"RonMenuDummy";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);

    // D3D12
    {
        HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
        HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
        if (d3d12 && dxgi) {
            auto createDev = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(d3d12, "D3D12CreateDevice"));
            using PFN_CF = HRESULT(WINAPI*)(REFIID, void**);
            auto createFactory = reinterpret_cast<PFN_CF>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
            ID3D12Device* dev = nullptr;
            ID3D12CommandQueue* q = nullptr;
            IDXGIFactory2* fac = nullptr;
            IDXGISwapChain1* sc = nullptr;
            if (createDev && createFactory &&
                SUCCEEDED(createDev(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) {
                D3D12_COMMAND_QUEUE_DESC qd{};
                qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
                if (SUCCEEDED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q))) &&
                    SUCCEEDED(createFactory(IID_PPV_ARGS(&fac)))) {
                    DXGI_SWAP_CHAIN_DESC1 sd{};
                    sd.Width = 100; sd.Height = 100;
                    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                    sd.SampleDesc.Count = 1;
                    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                    sd.BufferCount = 2;
                    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
                    if (SUCCEEDED(fac->CreateSwapChainForHwnd(q, hwnd, &sd, nullptr, nullptr, &sc))) {
                        out.swap12 = *reinterpret_cast<void***>(sc);
                        out.queue12 = *reinterpret_cast<void***>(q);
                    }
                }
            }
            SafeRelease(sc); SafeRelease(fac); SafeRelease(q); SafeRelease(dev);
        } else {
            Log("d3d12.dll / dxgi.dll not loaded in this process");
        }
    }
    // D3D11
    {
        HMODULE d3d11 = GetModuleHandleW(L"d3d11.dll");
        if (d3d11) {
            auto create = reinterpret_cast<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>(GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain"));
            DXGI_SWAP_CHAIN_DESC sd{};
            sd.BufferCount = 1;
            sd.BufferDesc.Width = 100; sd.BufferDesc.Height = 100;
            sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            sd.OutputWindow = hwnd;
            sd.SampleDesc.Count = 1;
            sd.Windowed = TRUE;
            sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
            IDXGISwapChain* sc = nullptr; ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
            D3D_FEATURE_LEVEL fl;
            if (create && SUCCEEDED(create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx)))
                out.swap11 = *reinterpret_cast<void***>(sc);
            SafeRelease(sc); SafeRelease(ctx); SafeRelease(dev);
        }
    }
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return out;
}

}  // namespace

namespace overlay {

bool Install() {
    VTables vt = GetVTables();
    Log("vtables: swap12=%p queue12=%p swap11=%p", vt.swap12, vt.queue12, vt.swap11);
    if (!vt.swap12 && !vt.swap11) return false;

    bool ok = true;
    for (void** sw : { vt.swap12, vt.swap11 }) {
        if (!sw) continue;
        ok &= PatchSlot(sw, 8, reinterpret_cast<void*>(hkPresent));
        ok &= PatchSlot(sw, 13, reinterpret_cast<void*>(hkResizeBuffers));
        if (sw == vt.swap12) {   // IDXGISwapChain1+ slots only exist on the D3D12-created chain
            ok &= PatchSlot(sw, 22, reinterpret_cast<void*>(hkPresent1));
            ok &= PatchSlot(sw, 39, reinterpret_cast<void*>(hkResizeBuffers1));
        }
    }
    Log("hooks installed: %s", ok ? "ok" : "with errors");
    return ok;
}

void Uninstall() {
    g.shuttingDown = true;
    UnpatchAll();
    IatUnhookAll();
    if (g.hwnd && g.origProc && GetWindowLongPtrW(g.hwnd, GWLP_WNDPROC) == reinterpret_cast<LONG_PTR>(hkWndProc))
        SetWindowLongPtrW(g.hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g.origProc));
    for (int i = 0; i < 200 && g.inflight.load() > 0; ++i) Sleep(10);
    Sleep(300);

    std::lock_guard<std::mutex> lk(g.mtx);
    if (g.menuWasOpen && g.hadClip) ClipCursor(&g.savedClip);
    CleanupBackend();
    if (g.win32Init) { ImGui_ImplWin32_Shutdown(); g.win32Init = false; }
    if (g.imguiCtx) { ImGui::DestroyContext(); g.imguiCtx = false; }
}

}  // namespace overlay
