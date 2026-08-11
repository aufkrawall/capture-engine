#include "dx9_hook_internal.h"

// Inline hook trampolines (set by inline hook installation)
static PFN_D3D9_Present_Inline oD3D9PresentTrampoline = nullptr;

static PFN_D3D9_PresentEx_Inline oD3D9PresentExTrampoline = nullptr;

static PFN_D3D9_SwapChain_Present_Inline oD3D9SwapChainPresentTrampoline = nullptr;

namespace {

struct Direct3DCreate9Publication {
    Direct3DCreate9_t fallback = nullptr;
};

void PublishD3D9PresentTrampoline(void* trampoline, void*) {
    oD3D9PresentTrampoline = reinterpret_cast<PFN_D3D9_Present_Inline>(trampoline);
}

void PublishD3D9PresentExTrampoline(void* trampoline, void*) {
    oD3D9PresentExTrampoline = reinterpret_cast<PFN_D3D9_PresentEx_Inline>(trampoline);
}

void PublishD3D9SwapChainPresentTrampoline(void* trampoline, void*) {
    oD3D9SwapChainPresentTrampoline = reinterpret_cast<PFN_D3D9_SwapChain_Present_Inline>(trampoline);
}

void PublishDirect3DCreate9Trampoline(void* trampoline, void* context) {
    auto* publication = static_cast<Direct3DCreate9Publication*>(context);
    dx9_hook_oDirect3DCreate9 =
        trampoline ? reinterpret_cast<Direct3DCreate9_t>(trampoline) : publication->fallback;
}

}  // namespace

static std::atomic<bool> g_InlineHooksInProgress{false};  // Guard against re-entry (atomic for thread safety)

static bool g_HooksInitialized = false;

static bool g_ResetHooksInstalled = false;

static int64_t g_LastSleepUs = 0;

DX9InternalBypassScope::DX9InternalBypassScope() {
    ++dx9_hook_g_InternalHelperBypassDepth;
}

DX9InternalBypassScope::~DX9InternalBypassScope() {
    if (dx9_hook_g_InternalHelperBypassDepth > 0) {
        --dx9_hook_g_InternalHelperBypassDepth;
    }
}

void DX9_RegisterInternalHelperDevice(IDirect3DDevice9* device) {
    if (!device) {
        return;
    }

    bool inserted = false;
    {
        std::lock_guard<std::mutex> lock(dx9_hook_g_InternalHelperDeviceMutex);
        inserted = dx9_hook_g_InternalHelperDevices.insert(device).second;
    }
    {
        std::lock_guard<std::mutex> lock(dx9_hook_g_D3D9IdentityMutex);
        dx9_hook_g_D3D9ExDevices.erase(device);
    }

    static std::atomic<int> s_registerLogCount{0};
    if (inserted && s_registerLogCount.fetch_add(1, std::memory_order_relaxed) < 8) {
        HookLogImportant("DX9: Registered internal helper device %p", device);
    }
}

void DX9_UnregisterInternalHelperDevice(IDirect3DDevice9* device) {
    if (!device) {
        return;
    }

    bool erased = false;
    {
        std::lock_guard<std::mutex> lock(dx9_hook_g_InternalHelperDeviceMutex);
        erased = dx9_hook_g_InternalHelperDevices.erase(device) > 0;
    }

    static std::atomic<int> s_unregisterLogCount{0};
    if (erased && s_unregisterLogCount.fetch_add(1, std::memory_order_relaxed) < 8) {
        HookLogImportant("DX9: Unregistered internal helper device %p", device);
    }
}

bool IsDXVKD3D9WrapperLoaded() {
    HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
    if (!d3d9)
        return false;

    char d3d9Path[MAX_PATH] = {};
    DWORD d3d9Len = GetModuleFileNameA(d3d9, d3d9Path, MAX_PATH);
    if (d3d9Len == 0 || d3d9Len >= MAX_PATH)
        return false;

    char systemDir[MAX_PATH] = {};
    UINT systemLen = GetSystemDirectoryA(systemDir, MAX_PATH);
    if (systemLen == 0 || systemLen >= MAX_PATH)
        return false;

    if (_strnicmp(d3d9Path, systemDir, systemLen) == 0 && (d3d9Path[systemLen] == '\\' || d3d9Path[systemLen] == '/')) {
        return false;
    }
    return true;
}

static bool GetD3D9PresentAddresses(void** ppPresent, void** ppPresentEx, void** ppSwapChainPresent) {
    HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
    if (!d3d9)
        return false;

    WNDCLASSEXA wc = {sizeof(wc)};
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = "D3D9Temp";
    RegisterClassExA(&wc);

    HWND hwnd =
        CreateWindowA("D3D9Temp", "Temp", WS_OVERLAPPED, 0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        UnregisterClassA("D3D9Temp", wc.hInstance);
        return false;
    }

    typedef HRESULT(WINAPI * PFN_D3D9Create9Ex)(UINT, IDirect3D9Ex**);
    PFN_D3D9Create9Ex pCreate9Ex = (PFN_D3D9Create9Ex)GetProcAddress(d3d9, "Direct3DCreate9Ex");

    IDirect3D9Ex* d3d9ex = nullptr;
    IDirect3DDevice9Ex* deviceEx = nullptr;
    IDirect3DSwapChain9* swapChain = nullptr;

    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = hwnd;

    bool success = false;

    if (pCreate9Ex && SUCCEEDED(pCreate9Ex(D3D_SDK_VERSION, &d3d9ex))) {
        if (SUCCEEDED(d3d9ex->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                             D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, nullptr, &deviceEx))) {
            uintptr_t* vtable = *(uintptr_t**)deviceEx;

            *ppPresent = (void*)vtable[17];
            *ppPresentEx = (void*)vtable[132];

            if (SUCCEEDED(deviceEx->GetSwapChain(0, &swapChain))) {
                uintptr_t* scVtable = *(uintptr_t**)swapChain;
                *ppSwapChainPresent = (void*)scVtable[3];
                swapChain->Release();
            }

            success = true;
            deviceEx->Release();
        }
        d3d9ex->Release();
    }

    if (!success) {
        typedef IDirect3D9*(WINAPI * PFN_D3D9Create9)(UINT);
        PFN_D3D9Create9 pCreate9 = (PFN_D3D9Create9)GetProcAddress(d3d9, "Direct3DCreate9");

        if (pCreate9) {
            IDirect3D9* d3d9Base = pCreate9(D3D_SDK_VERSION);
            if (d3d9Base) {
                IDirect3DDevice9* device = nullptr;
                if (SUCCEEDED(d3d9Base->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                                     D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device))) {
                    uintptr_t* vtable = *(uintptr_t**)device;
                    *ppPresent = (void*)vtable[17];
                    *ppPresentEx = nullptr;

                    if (SUCCEEDED(device->GetSwapChain(0, &swapChain))) {
                        uintptr_t* scVtable = *(uintptr_t**)swapChain;
                        *ppSwapChainPresent = (void*)scVtable[3];
                        swapChain->Release();
                    }

                    success = true;
                    device->Release();
                }
                d3d9Base->Release();
            }
        }
    }

    DestroyWindow(hwnd);
    UnregisterClassA("D3D9Temp", wc.hInstance);

    return success;
}

static HRESULT STDMETHODCALLTYPE DetourD3D9PresentInline(IDirect3DDevice9* device, const RECT* pSourceRect,
                                                         const RECT* pDestRect, HWND hDestWindowOverride,
                                                         const RGNDATA* pDirtyRegion) {
    static int entryLogCount = 0;
    if (entryLogCount < 5) {
        EarlyLog("DX9: DetourD3D9PresentInline called (device=%p, count=%d)", device, entryLogCount);
        entryLogCount++;
    }
    if (HookIsShuttingDown()) {
        if (oD3D9PresentTrampoline)
            return oD3D9PresentTrampoline(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
        return D3DERR_INVALIDCALL;
    }
    if (ShouldSkipDX9PresentForVulkan()) {
        if (oD3D9PresentTrampoline)
            return oD3D9PresentTrampoline(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
        return D3D_OK;
    }
    if (ShouldBypassDX9HooksForDevice(device)) {
        if (oD3D9PresentTrampoline) {
            return oD3D9PresentTrampoline(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
        }
        return D3D_OK;
    }

    const bool topLevelPresent = (dx9_hook_g_PresentRecurse == 0);
    IDirect3DSurface9* backBuffer = nullptr;
    DX9_PresentBegin(device, backBuffer);

    LARGE_INTEGER p0, p1;
    QueryPerformanceCounter(&p0);
    HRESULT hr = oD3D9PresentTrampoline(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
    QueryPerformanceCounter(&p1);
    dx9_hook_g_PresentCallTiming.presentCallTime = p1.QuadPart - p0.QuadPart;

    DX9_PresentEnd(device, backBuffer);

    int64_t qpcFreq = GetQpcFreqCached();
    int64_t presentUs = qpcFreq ? ((p1.QuadPart - p0.QuadPart) * 1000000) / qpcFreq : 0;
    if (topLevelPresent) {
        MaybeWaitForVSyncAfterPresent((int)presentUs);
    }

    return hr;
}

static HRESULT STDMETHODCALLTYPE DetourD3D9PresentExInline(IDirect3DDevice9Ex* device, const RECT* pSourceRect,
                                                           const RECT* pDestRect, HWND hDestWindowOverride,
                                                           const RGNDATA* pDirtyRegion, DWORD dwFlags) {
    static int entryLogCount = 0;
    if (entryLogCount < 5) {
        EarlyLog("DX9: DetourD3D9PresentExInline called (device=%p, flags=0x%X, count=%d)", device, dwFlags,
                 entryLogCount);
        entryLogCount++;
    }
    if (HookIsShuttingDown()) {
        if (oD3D9PresentExTrampoline)
            return oD3D9PresentExTrampoline(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
        return D3DERR_INVALIDCALL;
    }
    if (ShouldSkipDX9PresentForVulkan()) {
        if (oD3D9PresentExTrampoline)
            return oD3D9PresentExTrampoline(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
        return D3D_OK;
    }
    if (ShouldBypassDX9HooksForDevice(device)) {
        if (oD3D9PresentExTrampoline) {
            return oD3D9PresentExTrampoline(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
        }
        return D3D_OK;
    }

    VSyncOverride vsync = GetVSyncOverride();
    if (vsync.shouldOverride && vsync.presentInterval > 0) {
        dwFlags &= ~D3DPRESENT_FORCEIMMEDIATE;
        dwFlags &= ~D3DPRESENT_DONOTWAIT;
    }

    const bool topLevelPresent = (dx9_hook_g_PresentRecurse == 0);
    IDirect3DSurface9* backBuffer = nullptr;
    DX9_PresentBegin(device, backBuffer);

    LARGE_INTEGER p0, p1;
    QueryPerformanceCounter(&p0);
    HRESULT hr = oD3D9PresentExTrampoline(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
    QueryPerformanceCounter(&p1);
    dx9_hook_g_PresentCallTiming.presentCallTime = p1.QuadPart - p0.QuadPart;

    DX9_PresentEnd(device, backBuffer);

    int64_t qpcFreq = GetQpcFreqCached();
    int64_t presentUs = qpcFreq ? ((p1.QuadPart - p0.QuadPart) * 1000000) / qpcFreq : 0;
    if (topLevelPresent) {
        MaybeWaitForVSyncAfterPresent((int)presentUs);
    }

    return hr;
}

static HRESULT STDMETHODCALLTYPE DetourD3D9SwapChainPresentInline(IDirect3DSwapChain9* swapChain,
                                                                  const RECT* pSourceRect, const RECT* pDestRect,
                                                                  HWND hDestWindowOverride, const RGNDATA* pDirtyRegion,
                                                                  DWORD dwFlags) {
    static int entryLogCount = 0;
    if (entryLogCount < 5) {
        EarlyLog("DX9: DetourD3D9SwapChainPresentInline called (swap=%p, flags=0x%X, count=%d)", swapChain, dwFlags,
                 entryLogCount);
        entryLogCount++;
    }
    if (HookIsShuttingDown()) {
        if (oD3D9SwapChainPresentTrampoline)
            return oD3D9SwapChainPresentTrampoline(swapChain, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion,
                                                   dwFlags);
        return D3DERR_INVALIDCALL;
    }
    if (ShouldSkipDX9PresentForVulkan()) {
        if (oD3D9SwapChainPresentTrampoline)
            return oD3D9SwapChainPresentTrampoline(swapChain, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion,
                                                   dwFlags);
        return D3D_OK;
    }

    VSyncOverride vsync = GetVSyncOverride();
    if (vsync.shouldOverride && vsync.presentInterval > 0) {
        dwFlags &= ~D3DPRESENT_FORCEIMMEDIATE;
        dwFlags &= ~D3DPRESENT_DONOTWAIT;
    }

    IDirect3DDevice9* device = nullptr;
    bool ownsPresentScope = false;
    IDirect3DSurface9* backBuffer = nullptr;

    if (dx9_hook_g_PresentRecurse == 0 && SUCCEEDED(swapChain->GetDevice(&device))) {
        DX9_PresentBegin(device, backBuffer);
        ownsPresentScope = true;
    }

    LARGE_INTEGER p0, p1;
    QueryPerformanceCounter(&p0);
    HRESULT hr =
        oD3D9SwapChainPresentTrampoline(swapChain, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
    QueryPerformanceCounter(&p1);
    dx9_hook_g_PresentCallTiming.presentCallTime = p1.QuadPart - p0.QuadPart;

    if (device) {
        DX9_PresentEnd(device, backBuffer);
        device->Release();
    }

    int64_t qpcFreq = GetQpcFreqCached();
    int64_t presentUs = qpcFreq ? ((p1.QuadPart - p0.QuadPart) * 1000000) / qpcFreq : 0;
    if (ownsPresentScope) {
        MaybeWaitForVSyncAfterPresent((int)presentUs);
    }

    return hr;
}

static bool InstallD3D9InlineHooks() {
    // Guard against re-entry - this function may be called recursively
    // if GetD3D9PresentAddresses triggers a hook that calls back here

    // Use EarlyLog for diagnostics (writes to hook_debug.log when enabled)
    auto LogDirect = [](const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        char buf[1024];
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        EarlyLog("%s", buf);
    };

    LogDirect("=== InstallD3D9InlineHooks START (installed=%d, inProgress=%d)", dx9_hook_g_InlineHooksInstalled ? 1 : 0,
              g_InlineHooksInProgress ? 1 : 0);

    EarlyLog("DX9: InstallD3D9InlineHooks called (installed=%d, inProgress=%d)", dx9_hook_g_InlineHooksInstalled ? 1 : 0,
             g_InlineHooksInProgress ? 1 : 0);

    if (dx9_hook_g_InlineHooksInstalled)
        return true;
    if (g_InlineHooksInProgress) {
        EarlyLog("DX9: InstallD3D9InlineHooks - re-entry blocked");
        return true;  // Already being installed, don't re-enter
    }

    g_InlineHooksInProgress = true;
    LogDirect("Guard set, proceeding to GetD3D9PresentAddresses");
    EarlyLog("DX9: InstallD3D9InlineHooks - guard set, proceeding");

    void* presentAddr = nullptr;
    void* presentExAddr = nullptr;
    void* swapChainPresentAddr = nullptr;

    LogDirect("Calling GetD3D9PresentAddresses...");
    EarlyLog("DX9: Calling GetD3D9PresentAddresses...");
    if (!GetD3D9PresentAddresses(&presentAddr, &presentExAddr, &swapChainPresentAddr)) {
        LogDirect("GetD3D9PresentAddresses FAILED - cannot install inline hooks");
        EarlyLog("DX9: GetD3D9PresentAddresses FAILED - cannot install inline hooks");
        g_InlineHooksInProgress = false;
        return false;
    }

    LogDirect("Present addresses: Present=%p, PresentEx=%p, SwapChain=%p", presentAddr, presentExAddr,
              swapChainPresentAddr);
    EarlyLog("DX9: Present addresses found: Present=%p, PresentEx=%p, SwapChain=%p", presentAddr, presentExAddr,
             swapChainPresentAddr);

    bool anySuccess = false;

    if (presentAddr) {
        LogDirect("Installing Present inline hook at %p...", presentAddr);
        EarlyLog("DX9: Installing Present inline hook at %p...", presentAddr);
        void* trampoline = nullptr;
        if (InlineHook::InstallPublished(presentAddr, (void*)DetourD3D9PresentInline, &trampoline,
                                         PublishD3D9PresentTrampoline, nullptr)) {
            LogDirect("Present inline hook SUCCESS (addr=%p, trampoline=%p)", presentAddr, trampoline);
            EarlyLog("DX9: Present inline hook installed (addr=%p, trampoline=%p)", presentAddr, trampoline);
            anySuccess = true;
        } else {
            LogDirect("Present inline hook FAILED at %p", presentAddr);
            EarlyLog("DX9: Present inline hook FAILED at %p", presentAddr);
        }
    } else {
        EarlyLog("DX9: Present address is NULL - skipping hook");
    }

    if (presentExAddr) {
        EarlyLog("DX9: Installing PresentEx inline hook at %p...", presentExAddr);
        void* trampoline = nullptr;
        if (InlineHook::InstallPublished(presentExAddr, (void*)DetourD3D9PresentExInline, &trampoline,
                                         PublishD3D9PresentExTrampoline, nullptr)) {
            EarlyLog("DX9: PresentEx inline hook installed (addr=%p, trampoline=%p)", presentExAddr, trampoline);
            anySuccess = true;
        } else {
            EarlyLog("DX9: PresentEx inline hook FAILED at %p (non-fatal)", presentExAddr);
        }
    } else {
        EarlyLog("DX9: PresentEx address is NULL - skipping hook (expected on non-Ex)");
    }

    if (swapChainPresentAddr) {
        EarlyLog("DX9: Installing SwapChain::Present inline hook at %p...", swapChainPresentAddr);
        void* trampoline = nullptr;
        if (InlineHook::InstallPublished(swapChainPresentAddr, (void*)DetourD3D9SwapChainPresentInline,
                                         &trampoline, PublishD3D9SwapChainPresentTrampoline, nullptr)) {
            EarlyLog(
                "DX9: SwapChain::Present inline hook installed (addr=%p, "
                "trampoline=%p)",
                swapChainPresentAddr, trampoline);
            anySuccess = true;
        } else {
            EarlyLog("DX9: SwapChain::Present inline hook FAILED at %p (non-fatal)", swapChainPresentAddr);
        }
    } else {
        EarlyLog("DX9: SwapChain::Present address is NULL - skipping hook");
    }

    if (anySuccess) {
        LogDirect("At least one inline hook installed successfully");
        EarlyLog("DX9: At least one inline hook installed successfully");
        HookLogImportant("DX9: Inline hooks installed (Present=%p, PresentEx=%p, SwapChain=%p)", (void*)presentAddr,
                         (void*)presentExAddr, (void*)swapChainPresentAddr);
        dx9_hook_g_InlineHooksInstalled = true;
    } else {
        LogDirect("ALL inline hooks failed - falling back to DX9 vtable hooks");
        EarlyLog("DX9: ALL inline hooks failed - falling back to vtable hooks");
        HookLogImportant("DX9: ALL inline hooks FAILED - falling back to vtable hooks");
    }

    g_InlineHooksInProgress = false;
    LogDirect("InstallD3D9InlineHooks complete (success=%d)", anySuccess ? 1 : 0);
    EarlyLog("DX9: InstallD3D9InlineHooks complete (success=%d)", anySuccess ? 1 : 0);
    return anySuccess;
}

static int GetMSAASampleCount(IDirect3DDevice9* device) {
    IDirect3DSurface9* rt = nullptr;
    if (SUCCEEDED(device->GetRenderTarget(0, &rt)) && rt) {
        D3DSURFACE_DESC desc;
        HRESULT hr = rt->GetDesc(&desc);
        rt->Release();
        if (SUCCEEDED(hr)) {
            if (desc.MultiSampleType >= D3DMULTISAMPLE_2_SAMPLES && desc.MultiSampleType <= D3DMULTISAMPLE_16_SAMPLES) {
                return (int)desc.MultiSampleType;
            }
        }
    }
    return 0;
}

void DX9Hook::Init() {
    auto LogDirect = [](const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        char buf[1024];
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        EarlyLog("%s", buf);
    };

    LogDirect("=== DX9Hook::Init() START ===");

    HMODULE d3d9Module = GetModuleHandleA("d3d9.dll");
    LogDirect("d3d9.dll = %p", (void*)d3d9Module);

    if (!d3d9Module) {
        LogDirect("DX9: d3d9.dll not loaded, returning");
        return;
    }

    LogDirect("Calling InstallD3D9InlineHooks...");
    bool inlineResult = InstallD3D9InlineHooks();
    LogDirect("InstallD3D9InlineHooks returned %d", inlineResult ? 1 : 0);

    // Hook Export Functions
    // Using IAT hooking (in iat_hook.cpp) or active VTable hooking for DX9.

    // Check if Direct3DCreate9(Ex) are available for active hooking fallback
    void* pD3DCreate9 = (void*)GetProcAddress(d3d9Module, "Direct3DCreate9");
    void* pD3DCreate9Ex = (void*)GetProcAddress(d3d9Module, "Direct3DCreate9Ex");

    // Install inline hook on Direct3DCreate9 so ALL callers (main exe + any
    // middleware DLL) are intercepted, regardless of which module calls it.
    // This lets DetourDirect3DCreate9 hook CreateDevice on the returned factory.
    // Note: oDirect3DCreate9 may already be set by the IAT hook; we overwrite it
    // with the trampoline so calling it doesn't re-enter the inline hook.
    static bool s_direct3DCreate9InlineInstalled = false;
    if (pD3DCreate9 && !s_direct3DCreate9InlineInstalled) {
        Direct3DCreate9Publication publication{dx9_hook_oDirect3DCreate9};
        void* trampoline = nullptr;
        if (InlineHook::InstallPublished(pD3DCreate9, (void*)DetourDirect3DCreate9, &trampoline,
                                         PublishDirect3DCreate9Trampoline, &publication)) {
            s_direct3DCreate9InlineInstalled = true;
            EarlyLog("DX9: Direct3DCreate9 inline hook installed (trampoline=%p)", trampoline);
        } else {
            EarlyLog("DX9: Direct3DCreate9 inline hook failed");
        }
    }

    // Hook CreateDevice on the plain IDirect3D9 vtable.  This is critical for
    // late injection: the game may have already called Direct3DCreate9() and holds
    // a plain IDirect3D9 whose vtable is DIFFERENT from IDirect3D9Ex.  By creating
    // a temporary IDirect3D9 and hooking its vtable, we intercept CreateDevice on
    // ALL plain IDirect3D9 instances (vtable is shared across all instances of the
    // same COM class).
    if (!dx9_hook_oCreateDevice && pD3DCreate9) {
        // Use the trampoline (bypasses our inline hook) if available, else raw address
        typedef IDirect3D9*(WINAPI * PFN_Create9)(UINT);
        PFN_Create9 pfnCreate9 = dx9_hook_oDirect3DCreate9 ? (PFN_Create9)dx9_hook_oDirect3DCreate9 : (PFN_Create9)pD3DCreate9;
        IDirect3D9* dummyD3D9 = pfnCreate9(D3D_SDK_VERSION);
        if (dummyD3D9) {
            uintptr_t* vtable = *(uintptr_t**)dummyD3D9;
            bool vtableValid = (vtable != nullptr) && (reinterpret_cast<uintptr_t>(vtable) >= 0x10000) &&
                               (reinterpret_cast<uintptr_t>(vtable) < 0x7FFFFFFF0000);
            if (vtable && vtableValid) {
                VTableHook::Create(&vtable[16], (void*)&DetourCreateDevice, (void**)&dx9_hook_oCreateDevice);
                EarlyLog("DX9: Plain IDirect3D9::CreateDevice hooked (vtable=%p)", (void*)vtable);
            }
            dummyD3D9->Release();
        } else {
            EarlyLog("DX9: Failed to create dummy IDirect3D9 for vtable hook");
        }
    }

    LogDirect("DX9Hook::Init() Passive Complete");

    // Check for test apps that force DX9 but might load other DLLs
    bool isTestApp = false;
    char modPath[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, modPath, MAX_PATH)) {
        const char* exeName = strrchr(modPath, '\\');
        exeName = exeName ? exeName + 1 : modPath;
        if (strnicmp(exeName, "dx9_test", 8) == 0)
            isTestApp = true;
    }

    // Skip Active Hooking if a different graphics API is the primary renderer
    const char* skipReason = nullptr;
    if (GetModuleHandleA("d3d12.dll") && !isTestApp) {
        skipReason = "d3d12.dll (DX12 game)";
    } else if ((GetModuleHandleA("d3d11.dll") || GetModuleHandleA("d3d10.dll") || GetModuleHandleA("d3d10_1.dll")) &&
               !isTestApp) {
        // DX11/DX10 usually implies D3D11/D3D10 is primary, unless it's a test app
        skipReason = GetModuleHandleA("d3d11.dll") ? "d3d11.dll (DX11 game)" : "d3d10.dll (DX10 game)";
    } else if (GetModuleHandleA("vulkan-1.dll") && !isTestApp) {
        skipReason = "vulkan-1.dll (Vulkan game)";
    }

    // Note: opengl32.dll check removed. Many DX9 games load it but don't use it.
    // We want active init to ensure reliable hooking even in those cases.

    const bool inlineHooksReady = dx9_hook_g_InlineHooksInstalled.load(std::memory_order_acquire);

    LogDirect("skipReason=%s, inlineHooksReady=%d, oPresent=%p", skipReason ? skipReason : "null",
              inlineHooksReady ? 1 : 0, (void*)dx9_hook_oPresent);

    if (skipReason) {
        LogDirect("DX9: Skipping active init (%s, inlineHooksReady=%d)", skipReason, inlineHooksReady ? 1 : 0);
        return;
    }

    // CRITICAL: If inline hooks failed, try to find existing D3D9 devices FIRST
    // This is needed for late injection when the game already created its device
    // and another overlay has hooked d3d9.dll functions (blocking our inline hooks)
    // We must do this BEFORE creating a dummy device, because dummy device VTable
    // hooks won't affect the game's real device (each device has its own VTable copy)
    // DISABLED - scanner finds false positives and causes crashes
    // if (!inlineHooksReady && !oPresent) {
    //   LogDirect("DX9: Inline hooks failed, scanning for existing D3D9 devices...");
    //   ScanForExistingD3D9Devices();
    //
    //   if (oPresent) {
    //     LogDirect("DX9: Successfully hooked existing device via scanner!");
    //   } else {
    //     LogDirect("DX9: Scanner found no devices, will create dummy device");
    //   }
    // }

    // If we still don't have hooks, try active hooking with a dummy device
    // This is a fallback for cases where no device exists yet (early injection)
    // or the scanner failed to find the game's device
    LogDirect("Checking oPresent=%p for dummy device creation...", (void*)dx9_hook_oPresent);

    if (!dx9_hook_oPresent) {
        LogDirect("DX9: Creating dummy device for VTable hooks...");

        // Active Hooking: Create a dummy device to force vtable hooks
        // This is needed for "early" injection where the game hasn't created its device yet

        // 1. Create a specific window class for our dummy window
        WNDCLASSEXA wc = {0};
        wc.cbSize = sizeof(wc);
        wc.style = CS_CLASSDC;
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = GetModuleHandle(NULL);
        wc.lpszClassName = "DX9Hook_Dummy";
        RegisterClassExA(&wc);

        HWND hWnd = CreateWindowA("DX9Hook_Dummy", "DX9 Dummy", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, NULL, NULL,
                                  wc.hInstance, NULL);

        LogDirect("Dummy window created: hWnd=%p", (void*)hWnd);

        if (hWnd && d3d9Module) {
            // Try Direct3DCreate9Ex first
            if (pD3DCreate9Ex) {
                LogDirect("Trying Direct3DCreate9Ex...");
                typedef HRESULT(WINAPI * Direct3DCreate9Ex_t)(UINT, IDirect3D9Ex**);
                Direct3DCreate9Ex_t create9Ex = (Direct3DCreate9Ex_t)pD3DCreate9Ex;
                IDirect3D9Ex* d3d9ex = nullptr;

                if (SUCCEEDED(create9Ex(D3D_SDK_VERSION, &d3d9ex))) {
                    LogDirect("Direct3DCreate9Ex succeeded, creating device...");
                    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
                    D3DPRESENT_PARAMETERS pp = {0};
                    pp.Windowed = TRUE;
                    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
                    pp.hDeviceWindow = hWnd;

                    IDirect3DDevice9Ex* deviceEx = nullptr;
                    if (SUCCEEDED(d3d9ex->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hWnd,
                                                         D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, NULL, &deviceEx))) {
                        LogDirect("D3D9Ex device created, calling InstallDeviceHooks...");
                        InstallDeviceHooks(deviceEx, true);
                        LogDirect("InstallDeviceHooks returned, oPresent=%p", (void*)dx9_hook_oPresent);
                        deviceEx->Release();
                    }
                    d3d9ex->Release();
                }
            }

            // Fallback to Direct3DCreate9 if Ex failed or wasn't tried, AND hooks are
            // not fully installed (InstallDeviceHooks checks for oPresent/oReset
            // internally)
            if ((!dx9_hook_oPresent || !dx9_hook_oReset) && pD3DCreate9) {
                LogDirect("Trying Direct3DCreate9 fallback...");
                typedef IDirect3D9*(WINAPI * Direct3DCreate9_t)(UINT);
                Direct3DCreate9_t create9 = (Direct3DCreate9_t)pD3DCreate9;
                IDirect3D9* d3d9 = create9(D3D_SDK_VERSION);

                if (d3d9) {
                    LogDirect("Direct3DCreate9 succeeded, creating device...");
                    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
                    D3DPRESENT_PARAMETERS pp = {0};
                    pp.Windowed = TRUE;
                    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
                    pp.hDeviceWindow = hWnd;

                    IDirect3DDevice9* device = nullptr;
                    if (SUCCEEDED(d3d9->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hWnd,
                                                     D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device))) {
                        LogDirect("D3D9 device created, calling InstallDeviceHooks...");
                        InstallDeviceHooks(device, true);
                        LogDirect("InstallDeviceHooks returned, oPresent=%p", (void*)dx9_hook_oPresent);
                        device->Release();
                    }
                    d3d9->Release();
                }
            }
        }

        if (hWnd) {
            DestroyWindow(hWnd);
            UnregisterClassA("DX9Hook_Dummy", wc.hInstance);
        }
    }

    LogDirect("DX9Hook::Init() complete (inlineHooks=%d, oPresent=%p, oReset=%p)",
              dx9_hook_g_InlineHooksInstalled.load() ? 1 : 0, (void*)dx9_hook_oPresent, (void*)dx9_hook_oReset);
}

void DX9Hook::Shutdown() {
    EarlyLog("DX9Hook::Shutdown()");
    ce::dx9_sampler_state::LogSummary();

    if (g_OverlayAdapter.IsInitialized()) {
        g_OverlayAdapter.Shutdown();
    }

    dx9_hook_g_DX9Capture.ForceCleanup();
}

void DX9Hook::OnHostDisconnect() {
    EarlyLog("DX9Hook::OnHostDisconnect()");
    dx9_hook_g_DX9Capture.ForceCleanup();
}
