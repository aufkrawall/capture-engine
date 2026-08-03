                    "hz=%d)",
                    dwmUs, expectedUs, hz);
            }
        } else {
            s_DwmBadCadenceCount = 0;
        }

        // If DwmFlush didn't actually block (or blocked at an unexpected cadence),
        // fall back.
    }

    // Fallback: deterministic pacer to the desktop refresh.
    PaceToRefreshQpc();
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

// Forward declaration for present call timing (defined below with PresentBegin/End)
struct PresentTiming;
static thread_local struct PresentTimingFwd {
    int64_t presentCallTime = 0;
} g_PresentCallTiming;

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
        return D3D_OK;
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

    const bool topLevelPresent = (g_PresentRecurse == 0);
    IDirect3DSurface9* backBuffer = nullptr;
    DX9_PresentBegin(device, backBuffer);

    LARGE_INTEGER p0, p1;
    QueryPerformanceCounter(&p0);
    HRESULT hr = oD3D9PresentTrampoline(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
    QueryPerformanceCounter(&p1);
    g_PresentCallTiming.presentCallTime = p1.QuadPart - p0.QuadPart;

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
        return D3D_OK;
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

    const bool topLevelPresent = (g_PresentRecurse == 0);
    IDirect3DSurface9* backBuffer = nullptr;
    DX9_PresentBegin(device, backBuffer);

    LARGE_INTEGER p0, p1;
    QueryPerformanceCounter(&p0);
    HRESULT hr = oD3D9PresentExTrampoline(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
    QueryPerformanceCounter(&p1);
    g_PresentCallTiming.presentCallTime = p1.QuadPart - p0.QuadPart;

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
        return D3D_OK;
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

    if (g_PresentRecurse == 0 && SUCCEEDED(swapChain->GetDevice(&device))) {
        DX9_PresentBegin(device, backBuffer);
        ownsPresentScope = true;
    }

    LARGE_INTEGER p0, p1;
    QueryPerformanceCounter(&p0);
    HRESULT hr =
        oD3D9SwapChainPresentTrampoline(swapChain, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
    QueryPerformanceCounter(&p1);
    g_PresentCallTiming.presentCallTime = p1.QuadPart - p0.QuadPart;

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

    LogDirect("=== InstallD3D9InlineHooks START (installed=%d, inProgress=%d)", g_InlineHooksInstalled ? 1 : 0,
              g_InlineHooksInProgress ? 1 : 0);

    EarlyLog("DX9: InstallD3D9InlineHooks called (installed=%d, inProgress=%d)", g_InlineHooksInstalled ? 1 : 0,
             g_InlineHooksInProgress ? 1 : 0);

    if (g_InlineHooksInstalled)
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
        if (InlineHook::Install(presentAddr, (void*)DetourD3D9PresentInline, &trampoline)) {
            oD3D9PresentTrampoline = (PFN_D3D9_Present_Inline)trampoline;
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
        if (InlineHook::Install(presentExAddr, (void*)DetourD3D9PresentExInline, &trampoline)) {
            oD3D9PresentExTrampoline = (PFN_D3D9_PresentEx_Inline)trampoline;
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
        if (InlineHook::Install(swapChainPresentAddr, (void*)DetourD3D9SwapChainPresentInline, &trampoline)) {
            oD3D9SwapChainPresentTrampoline = (PFN_D3D9_SwapChain_Present_Inline)trampoline;
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
        g_InlineHooksInstalled = true;
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

static HRESULT STDMETHODCALLTYPE DetourPresent(IDirect3DDevice9* device, const RECT* pSourceRect, const RECT* pDestRect,
                                               HWND hDestWindowOverride, const RGNDATA* pDirtyRegion);
static HRESULT STDMETHODCALLTYPE DetourSetSamplerState(IDirect3DDevice9* device, DWORD Sampler,
                                                       D3DSAMPLERSTATETYPE Type, DWORD Value);
static HRESULT STDMETHODCALLTYPE DetourGetSamplerState(IDirect3DDevice9* device, DWORD Sampler,
                                                       D3DSAMPLERSTATETYPE Type, DWORD* Value);
static HRESULT STDMETHODCALLTYPE DetourSetTexture(IDirect3DDevice9* device, DWORD Stage,
                                                  IDirect3DBaseTexture9* Texture);
static HRESULT STDMETHODCALLTYPE DetourSetTextureStageState(IDirect3DDevice9* device, DWORD Stage,
                                                            D3DTEXTURESTAGESTATETYPE Type, DWORD Value);
static HRESULT STDMETHODCALLTYPE DetourCreateStateBlock(IDirect3DDevice9* device, D3DSTATEBLOCKTYPE type,
                                                        IDirect3DStateBlock9** stateBlock);
static HRESULT STDMETHODCALLTYPE DetourEndStateBlock(IDirect3DDevice9* device, IDirect3DStateBlock9** stateBlock);
static HRESULT STDMETHODCALLTYPE DetourStateBlockApply(IDirect3DStateBlock9* stateBlock);
static void InstallD3D9StateBlockHooks(IDirect3DStateBlock9* stateBlock, const char* reason);

static const char* D3D9FormatName(D3DFORMAT format) {
    switch (format) {
        case D3DFMT_A8R8G8B8:
            return "A8R8G8B8";
        case D3DFMT_X8R8G8B8:
            return "X8R8G8B8";
        case D3DFMT_A2B10G10R10:
            return "A2B10G10R10";
        case D3DFMT_UNKNOWN:
            return "UNKNOWN";
        default:
            return "OTHER";
    }
}

static D3DMULTISAMPLE_TYPE ParseD3D9MSAA(const char* msaa) {
    if (strcmp(msaa, "2x") == 0 || strcmp(msaa, "2") == 0)
        return D3DMULTISAMPLE_2_SAMPLES;
    if (strcmp(msaa, "4x") == 0 || strcmp(msaa, "4") == 0)
        return D3DMULTISAMPLE_4_SAMPLES;
    if (strcmp(msaa, "8x") == 0 || strcmp(msaa, "8") == 0)
        return D3DMULTISAMPLE_8_SAMPLES;
    return D3DMULTISAMPLE_NONE;
}

static void ApplyMSAAOverride(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE deviceType, D3DPRESENT_PARAMETERS* pp) {
    if (!pp)
        return;

    const auto& gfx = GetActiveGraphicsConfig();
    const char* msaa = gfx.msaaSamples.c_str();
    if (msaa[0] == 'd')
        return;  // default

    D3DMULTISAMPLE_TYPE msType = ParseD3D9MSAA(msaa);

    EarlyLog(
        "DX9: ApplyMSAAOverride checking '%s' (Parsed=%d). BBFormat=%d "
        "Windowed=%d",
        msaa, msType, pp->BackBufferFormat, pp->Windowed);

    if (msType != D3DMULTISAMPLE_NONE) {
        DWORD quality;
        // Ensure format is valid for check? If 0 (Unknown), use adapter format?
        D3DFORMAT fmt = pp->BackBufferFormat;
        if (fmt == D3DFMT_UNKNOWN)
            fmt = D3DFMT_X8R8G8B8;  // Fallback guess

        if (SUCCEEDED(d3d->CheckDeviceMultiSampleType(adapter, deviceType, fmt, pp->Windowed, msType, &quality))) {
            pp->MultiSampleType = msType;
            pp->MultiSampleQuality = 0;
            pp->SwapEffect = D3DSWAPEFFECT_DISCARD;
            // Also clear flags that might conflict
            pp->Flags &= ~D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;

            HookLog("DX9: Forcing MSAA %d samples (Format %d)", (int)msType, fmt);
        } else {
            HookLog("DX9: MSAA %d samples NOT SUPPORTED for Format %d", (int)msType, fmt);
        }
    } else if (strcmp(msaa, "off") == 0) {
        pp->MultiSampleType = D3DMULTISAMPLE_NONE;
        pp->MultiSampleQuality = 0;
        HookLog("DX9: Forcing MSAA OFF");
    }
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

// Proactive apply in Present

// Version-specific d3d9.dll runtime patching and transparent D3D9Ex device
// promotion are intentionally absent. The native capture path uses a private
// helper-owned shared ring while preserving the application's device type.

// DX9 Capture class with D3D11 interop
class DX9Capture : public HookCaptureBase {
public:
    std::recursive_mutex captureMutex;

    // Capture State
    bool firstFrame = true;
    bool initializationFailed = false;  // Prevent endless retries if HW really fails
    bool generationResetPending = false;

    DX9Capture() {
        CaptureBase::initialized = false;
        initializationFailed = false;
        firstFrame = true;
    }

    // D3D9 resources
    IDirect3DDevice9* d3d9Device = nullptr;
    IDirect3DDevice9Ex* d3d9DeviceEx = nullptr;   // Interface to Ex device if avail
    IDirect3DTexture9* sharedTexture9 = nullptr;  // The shared texture resource
    IDirect3DSurface9* copySurface = nullptr;     // Surface level 0 of sharedTexture9

    HANDLE sharedHandle9 = NULL;  // Handle for D3D11 interop
    D3DFORMAT d3d9Format = D3DFMT_UNKNOWN;
    D3DFORMAT d3d9SharedFormat = D3DFMT_UNKNOWN;
    HRESULT hr = S_OK;

    // D3D11 resources for sharing
    ID3D11Device* d3d11Device = nullptr;
    ID3D11DeviceContext* d3d11Context = nullptr;
    ID3D11Texture2D* d3d11SharedTexture = nullptr;  // The texture opened in D3D11
    IDirect3DTexture9* overlayTexture9 = nullptr;

    ID3D11Texture2D* sharedTextures[CAPTURE_TEXTURE_COUNT]{};
    IDXGISurface1* gdiSharedRingSurfaces[CAPTURE_TEXTURE_COUNT]{};
    // NOTE: sharedTextureHandles and sharedFenceHandle are now member variables
    // (std::atomic<HANDLE>) from CaptureBase class to prevent race conditions

    // D3D11.3 Fence support
    ID3D11Fence* fence = nullptr;
    ID3D11DeviceContext4* context4 = nullptr;
    bool useFences = false;
    UINT64 fenceValue = 0;

    // Legacy readback surfaces/queries (used by staging + old shmem path)
    bool useShmem = false;
    IDirect3DSurface9* shmemSurfaces[CAPTURE_TEXTURE_COUNT] = {nullptr};
    IDirect3DQuery9* shmemQueries[CAPTURE_TEXTURE_COUNT] = {nullptr};
    bool shmemTextureReady[CAPTURE_TEXTURE_COUNT] = {false};
    uint32_t shmemPitch = 0;
    int shmemCurTex = 0;
    int shmemCopyWait = 0;

    // D3D11 staging path for non-Ex devices: uses GetRenderTargetData + D3D11
    // UpdateSubresource to avoid slow shmem IPC, while still providing real
    // shared texture handles to the encoder.
    bool useD3D11Staging = false;
    bool stagingUseGpuIntermediate = false;
    IDirect3DTexture9* stagingTextures[CAPTURE_TEXTURE_COUNT] = {nullptr};
    IDirect3DSurface9* stagingRenderSurfaces[CAPTURE_TEXTURE_COUNT] = {nullptr};
    int stagingWriteIdx = 0;
    int stagingReadIdx = 0;
    int stagingPending = 0;
    int64_t stagingLastSubmitQpc = 0;
    int64_t stagingTimestampQpc[CAPTURE_TEXTURE_COUNT] = {};

    // Deferred readback: StretchRect happens before Present, GetRenderTargetData
    // happens after Present to avoid blocking the D3D9 Present call.
    int stagingPendingBlitIdx = -1;  // Index of intermediate needing readback

    // Zero-copy deferred copy: StretchRect to shared surface before Present,
    // CopySubresourceRegion to encoder ring after Present (when StretchRect done).
    bool zeroCopyPendingCopy = false;
    int zeroCopyPendingIdx = -1;
    int64_t zeroCopyPendingTimestampQpc = 0;
    IDirect3DQuery9* zeroCopyQuery = nullptr;  // D3D9 event query for cross-API sync

    // Direct D3D9 shared ring path: the game device stretches directly into a
    // ring of shared D3D9 textures, then we only signal the cross-process fence
    // after the D3D9 event query confirms the GPU copy completed.
    bool useDirectD3D9SharedRing = false;
    bool directSharedUsesHelperProducer = false;
    IDirect3DTexture9* directSharedTextures9[CAPTURE_TEXTURE_COUNT] = {};
    IDirect3DSurface9* directSharedSurfaces9[CAPTURE_TEXTURE_COUNT] = {};
    IDirect3DQuery9* directSharedQueries9[CAPTURE_TEXTURE_COUNT] = {};
    IDirect3DTexture9* directSharedProducerTextures9[CAPTURE_TEXTURE_COUNT] = {};
    IDirect3D9* directSharedFactory = nullptr;
    IDirect3DDevice9* directSharedProducerDevice = nullptr;
    IDirect3D9Ex* directSharedFactoryEx = nullptr;
    IDirect3DDevice9Ex* directSharedProducerDeviceEx = nullptr;
    HWND directSharedHelperWindow = nullptr;
    struct DirectSharedHelperConfig {
        UINT adapterOrdinal = UINT_MAX;
        D3DDEVTYPE deviceType = D3DDEVTYPE_HAL;
        DWORD behaviorFlags = 0;
        bool valid = false;
    };
    DirectSharedHelperConfig directSharedLegacyConfig = {};
    DirectSharedHelperConfig directSharedExConfig = {};
    bool directSharedPending[CAPTURE_TEXTURE_COUNT] = {};
    int64_t directSharedPendingTimestampQpc[CAPTURE_TEXTURE_COUNT] = {};
    int directSharedSubmitIdx = 0;
    int directSharedDrainIdx = 0;
    int directSharedPendingCount = 0;

    // Per-frame staging metrics (set by CaptureFrame, read by PresentEnd)
    int32_t stagingStretchRectUs = 0;
    int32_t stagingReadbackSubmitUs = 0;
    int32_t stagingQueryWaitUs = 0;
    int32_t stagingLockRectUs = 0;
    int32_t stagingD3D11UploadUs = 0;
    int32_t stagingCurrentDepth = 0;
    int32_t stagingTotalDropped = 0;

    // Per-frame zero-copy metrics (set by PostPresentReadback, read by PresentEnd)
    int32_t zeroCopyQueryWaitUs = 0;
    int32_t zeroCopyReadbackUs = 0;

    // GDI interop for zero-copy capture on native D3D9.
    // Uses GetDC/BitBlt for GPU-accelerated D3D9->D3D11 transfer (WDDM 2.0+).
    // The heavy GetDC+BitBlt work runs on a dedicated capture thread to avoid
    // blocking the render thread. Render thread only does StretchRect (async GPU).
    bool useGDIInterop = false;
    IDirect3DSurface9* gdiCopySurfaces[2] = {};  // Double-buffered lockable D3D9 RTs
    ID3D11Texture2D* gdiTexture = nullptr;       // D3D11 GDI-compatible intermediate
    IDXGISurface1* gdiSurface = nullptr;         // DXGI surface for GetDC
    bool gdiDirectSharedRing = false;            // Write GDI blits straight into shared ring textures
    int gdiWriteIdx = 0;                         // Current write buffer index (0 or 1)
    bool gdiHasPrevFrame = false;                // True after first StretchRect completes
    int64_t gdiLastCaptureQpc = 0;               // Rate-limiting timestamp
    int64_t gdiBufferTimestampQpc[2] = {};
    std::atomic<bool> gdiBufferBusy[2] = {{false}, {false}};  // Per-buffer busy flags
    bool allowAsyncD3D9WorkerCapture = false;  // Safe only when the hooked device was created multithreaded.

    // Background capture thread proc for D3D11 staging path.
    // Processes LockRect + UpdateSubresource + SignalFrameReady off the render
    // thread. The render thread only does D3D9 submit + query check + enqueue.
    void StagingCaptureThreadProc() {
        captureThreadRunning = true;
        EarlyLog("DX9: Staging capture thread started");

        while (!captureThreadShutdown.load(std::memory_order_acquire)) {
            uint32_t rIdx = pendingReadIdx.load(std::memory_order_acquire);
            uint32_t wIdx = pendingWriteIdx.load(std::memory_order_acquire);

            if (rIdx == wIdx) {
                WaitForSingleObject(captureEvent, 50);
                continue;
            }

            PendingCaptureFrame& frame = pendingRing[rIdx % CAPTURE_RING_SIZE];
            const int consumeIdx = static_cast<int>(frame.backBufferIndex);

            // LockRect on SYSTEMMEM surface - instant after query confirmed DMA done
            D3DLOCKED_RECT rect;
            DWORD lockFlags = D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK;
            HRESULT lockHr = shmemSurfaces[consumeIdx]->LockRect(&rect, NULL, lockFlags);

            if (SUCCEEDED(lockHr)) {
                const int texIdx = AcquirePublishedTextureSlot();
                const bool canUpload = texIdx >= 0 && d3d11Context && sharedTextures[texIdx];
                if (canUpload) {
                    d3d11Context->UpdateSubresource(sharedTextures[texIdx], 0, NULL, rect.pBits, rect.Pitch, 0);
                }
