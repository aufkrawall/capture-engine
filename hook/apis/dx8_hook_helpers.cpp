#include "dx8_hook_internal.h"


D3D8SamplerVTableRecord* ResolveD3D8SamplerVTable(IDirect3DDevice8* device) {


    void** vtable = device ? *(void***)device : nullptr;
    if (vtable && dx8_hook_t_D3D8SamplerVTable == vtable)
        return dx8_hook_t_D3D8SamplerRecord;
    std::lock_guard<std::mutex> lock(dx8_hook_g_D3D8SamplerVTableMutex);
    for (const auto& record : dx8_hook_g_D3D8SamplerVTables) {
        if (record->vtable == vtable) {
            dx8_hook_t_D3D8SamplerVTable = vtable;
            dx8_hook_t_D3D8SamplerRecord = record.get();
            return dx8_hook_t_D3D8SamplerRecord;
        }
    }
    return nullptr;

}

UINT QueryD3D8MaxAnisotropy(void* opaqueDevice) {


    if (!opaqueDevice)
        return 1;
    auto* device = static_cast<IDirect3DDevice8*>(opaqueDevice);
    void** vtable = *(void***)device;
    using GetDeviceCaps8_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice8*, D3DCAPS9*);
    auto getCaps = reinterpret_cast<GetDeviceCaps8_t>(vtable[7]);
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
    D3DCAPS9 caps = {};
    return getCaps && SUCCEEDED(getCaps(device, &caps)) ? std::max<UINT>(1, caps.MaxAnisotropy) : 1;

}

void InstallD3D8SamplerHooks(IDirect3DDevice8* device) {


    if (!device)
        return;
    void** vtable = *(void***)device;
    std::lock_guard<std::mutex> lock(dx8_hook_g_D3D8SamplerVTableMutex);
    D3D8SamplerVTableRecord* record = nullptr;
    for (const auto& candidate : dx8_hook_g_D3D8SamplerVTables) {
        if (candidate->vtable == vtable) {
            record = candidate.get();
            break;
        }
    }
    if (!record) {
        auto newRecord = std::make_unique<D3D8SamplerVTableRecord>();
        newRecord->vtable = vtable;
        newRecord->setState.store(reinterpret_cast<D3D8SetTextureStageState_t>(
                                      vtable[D3D8_VTABLE_SETTEXTURESTAGESTATE]),
                                  std::memory_order_relaxed);
        newRecord->getState.store(reinterpret_cast<D3D8GetTextureStageState_t>(
                                      vtable[D3D8_VTABLE_GETTEXTURESTAGESTATE]),
                                  std::memory_order_relaxed);
        newRecord->applyStateBlock.store(
            reinterpret_cast<D3D8ApplyStateBlock_t>(vtable[D3D8_VTABLE_APPLYSTATEBLOCK]),
            std::memory_order_relaxed);
        record = newRecord.get();
        dx8_hook_g_D3D8SamplerVTables.push_back(std::move(newRecord));
    }

    if (!record->setHooked) {
        D3D8SetTextureStageState_t original = record->setState.load(std::memory_order_relaxed);
        if (VTableHook::Create(reinterpret_cast<void*>(&vtable[D3D8_VTABLE_SETTEXTURESTAGESTATE]),
                               reinterpret_cast<LPVOID>(&DetourD3D8SetTextureStageState),
                               reinterpret_cast<LPVOID*>(&original)) == VTableHook::Success) {
            record->setState.store(original, std::memory_order_release);
            record->setHooked = true;
            if (!dx8_hook_oD3D8SetTextureStageState)
                dx8_hook_oD3D8SetTextureStageState = original;
        }
    }
    if (!record->getHooked) {
        D3D8GetTextureStageState_t original = record->getState.load(std::memory_order_relaxed);
        if (VTableHook::Create(reinterpret_cast<void*>(&vtable[D3D8_VTABLE_GETTEXTURESTAGESTATE]),
                               reinterpret_cast<LPVOID>(&DetourD3D8GetTextureStageState),
                               reinterpret_cast<LPVOID*>(&original)) == VTableHook::Success) {
            record->getState.store(original, std::memory_order_release);
            record->getHooked = true;
            if (!dx8_hook_oD3D8GetTextureStageState)
                dx8_hook_oD3D8GetTextureStageState = original;
        }
    }
    if (!record->applyHooked) {
        D3D8ApplyStateBlock_t original = record->applyStateBlock.load(std::memory_order_relaxed);
        if (VTableHook::Create(reinterpret_cast<void*>(&vtable[D3D8_VTABLE_APPLYSTATEBLOCK]),
                               reinterpret_cast<LPVOID>(&DetourD3D8ApplyStateBlock),
                               reinterpret_cast<LPVOID*>(&original)) == VTableHook::Success) {
            record->applyStateBlock.store(original, std::memory_order_release);
            record->applyHooked = true;
            if (!dx8_hook_oD3D8ApplyStateBlock)
                dx8_hook_oD3D8ApplyStateBlock = original;
        }
    }
    dx8_hook_g_DX8HooksInitialized = record->setHooked && record->getHooked;
    HookLog("DX8: Sampler hooks reconciled for vtable=%p", vtable);

}

DWORD ParseD3D8MSAA(const char* msaa) {


    if (strcmp(msaa, "2x") == 0)
        return 2;  // D3DMULTISAMPLE_2_SAMPLES
    if (strcmp(msaa, "4x") == 0)
        return 4;  // D3DMULTISAMPLE_4_SAMPLES
    if (strcmp(msaa, "8x") == 0)
        return 8;  // D3DMULTISAMPLE_8_SAMPLES
    return 0;      // D3DMULTISAMPLE_NONE

}

void ApplyDX8MSAAOverride(IDirect3D8* d3d,  UINT adapter,  UINT deviceType,  D3D8_PRESENT_PARAMETERS* pp) {


    if (!pp || !g_IPC || !g_IPC->GetSharedMem())
        return;

    const char* msaa = g_IPC->GetSharedMem()->graphicsConfig.msaaSamples;
    if (msaa[0] == 'd')
        return;  // default

    DWORD msType = ParseD3D8MSAA(msaa);
    if (msType != 0) {
        // D3D8 CheckDeviceMultiSampleType: adapter, deviceType, format, windowed,
        // msType Using d3d8 vtable directly for CheckDeviceMultiSampleType (index
        // 5)
        typedef HRESULT(STDMETHODCALLTYPE * CheckMS_t)(IDirect3D8*, UINT, UINT, UINT, BOOL, DWORD);
        void** vtable = *(void***)d3d;
        CheckMS_t pCheckMS = (CheckMS_t)vtable[5];

        if (SUCCEEDED(pCheckMS(d3d, adapter, deviceType, pp->BackBufferFormat, pp->Windowed, msType))) {
            pp->MultiSampleType = msType;
            pp->SwapEffect = 1;  // D3DSWAPEFFECT_DISCARD
            HookLog("DX8: Forcing MSAA %d samples", (int)msType);
        } else {
            HookLog("DX8: MSAA %d samples NOT SUPPORTED", (int)msType);
        }
    } else if (strcmp(msaa, "off") == 0) {
        pp->MultiSampleType = 0;
        HookLog("DX8: Forcing MSAA OFF");
    }

}

void InstallD3D8DeviceHooks(IDirect3DDevice8* device) {


    if (!device) {
        return;
    }

    void** deviceVTable = *(void***)device;

    if (VTableHook::Create(reinterpret_cast<void*>(&deviceVTable[D3D8_VTABLE_PRESENT]), (LPVOID)&DetourD3D8Present, (LPVOID*)&dx8_hook_oD3D8Present) ==
        VTableHook::Success) {
        HookLog("DX8: Present hook installed");
    }

    if (VTableHook::Create(reinterpret_cast<void*>(&deviceVTable[D3D8_VTABLE_RESET]), (LPVOID)&DetourD3D8Reset, (LPVOID*)&dx8_hook_oD3D8Reset) ==
        VTableHook::Success) {
        HookLog("DX8: Reset hook installed");
    }

    InstallD3D8SamplerHooks(device);

}

void InstallD3D8CreateDeviceHook(IDirect3D8* d3d8) {


    if (!d3d8) {
        return;
    }

    void** d3d8VTable = *(void***)d3d8;
    if (VTableHook::Create(reinterpret_cast<void*>(&d3d8VTable[D3D8_VTABLE_CREATEDEVICE]), (LPVOID)&DetourD3D8CreateDevice,
                           (LPVOID*)&dx8_hook_oD3D8CreateDevice) == VTableHook::Success) {
        HookLog("DX8: CreateDevice hook installed");
    }

}

void TryInstallDirect3DCreate8Hook(HMODULE d3d8Module) {


    if (!d3d8Module) {
        return;
    }

    std::lock_guard<std::mutex> lock(dx8_hook_g_DX8InitMutex);
    if (dx8_hook_oDirect3DCreate8) {
        dx8_hook_g_HooksInitialized = true;
        return;
    }

    Direct3DCreate8_t direct3DCreate8 =
        reinterpret_cast<Direct3DCreate8_t>(GetProcAddress(d3d8Module, "Direct3DCreate8"));
    if (!direct3DCreate8) {
        HookLog("DX8: Failed to get Direct3DCreate8");
        return;
    }

    void* trampoline = nullptr;
    if (!InlineHook::Install(reinterpret_cast<void*>(direct3DCreate8), reinterpret_cast<void*>(DetourDirect3DCreate8),
                             &trampoline)) {
        HookLog("DX8: Failed to install Direct3DCreate8 hook");
        return;
    }

    dx8_hook_oDirect3DCreate8 = reinterpret_cast<Direct3DCreate8_t>(trampoline);
    dx8_hook_g_HooksInitialized = true;
    HookLog("DX8: Direct3DCreate8 hook installed");

}

IDirect3D8* WINAPI DetourDirect3DCreate8(UINT dx8_hook_sdkVersion) {


    if (!dx8_hook_oDirect3DCreate8) {
        return nullptr;
    }

    IDirect3D8* d3d8 = dx8_hook_oDirect3DCreate8(dx8_hook_sdkVersion);
    InstallD3D8CreateDeviceHook(d3d8);
    return d3d8;

}

D3D8GetCreationParameters_t GetD3D8GetCreationParameters(IDirect3DDevice8* device) {


    return reinterpret_cast<D3D8GetCreationParameters_t>(
        (*reinterpret_cast<void***>(device))[D3D8_VTABLE_GETCREATIONPARAMETERS]);

}

D3D8GetBackBuffer_t GetD3D8GetBackBuffer(IDirect3DDevice8* device) {


    return reinterpret_cast<D3D8GetBackBuffer_t>((*reinterpret_cast<void***>(device))[D3D8_VTABLE_GETBACKBUFFER]);

}

D3D8CreateImageSurface_t GetD3D8CreateImageSurface(IDirect3DDevice8* device) {


    return reinterpret_cast<D3D8CreateImageSurface_t>(
        (*reinterpret_cast<void***>(device))[D3D8_VTABLE_CREATEIMAGESURFACE]);

}

D3D8CopyRects_t GetD3D8CopyRects(IDirect3DDevice8* device) {


    return reinterpret_cast<D3D8CopyRects_t>((*reinterpret_cast<void***>(device))[D3D8_VTABLE_COPYRECTS]);

}

D3D8GetFrontBuffer_t GetD3D8GetFrontBuffer(IDirect3DDevice8* device) {


    return reinterpret_cast<D3D8GetFrontBuffer_t>((*reinterpret_cast<void***>(device))[D3D8_VTABLE_GETFRONTBUFFER]);

}

HWND ResolveD3D8TargetWindow(IDirect3DDevice8* device,  HWND hDestWindowOverride) {


    if (hDestWindowOverride && IsWindow(hDestWindowOverride)) {
        return hDestWindowOverride;
    }

    if (device) {
        // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
        D3DDEVICE_CREATION_PARAMETERS params = {};
        D3D8GetCreationParameters_t getCreationParameters = GetD3D8GetCreationParameters(device);
        if (getCreationParameters && SUCCEEDED(getCreationParameters(device, &params)) && params.hFocusWindow &&
            IsWindow(params.hFocusWindow)) {
            return params.hFocusWindow;
        }
    }

    if (dx8_hook_g_CachedHwnd && IsWindow(dx8_hook_g_CachedHwnd)) {
        return dx8_hook_g_CachedHwnd;
    }

    HWND foreground = GetForegroundWindow();
    if (!foreground || !IsWindow(foreground)) {
        return nullptr;
    }

    DWORD windowPid = 0;
    GetWindowThreadProcessId(foreground, &windowPid);
    return windowPid == GetCurrentProcessId() ? foreground : nullptr;

}

bool ResolveD3D8RenderSize(IDirect3DDevice8* device,  HWND hwnd,  uint32_t* outWidth,  uint32_t* outHeight) {


    if (!outWidth || !outHeight) {
        return false;
    }

    *outWidth = 0;
    *outHeight = 0;

    if (device) {
        IDirect3DSurface8* backBuffer = nullptr;
        HRESULT hr = GetD3D8GetBackBuffer(device)(device, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
        if (SUCCEEDED(hr) && backBuffer) {
            // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
            D3D8_SURFACE_DESC_LOCAL dx8_hook_desc = {};
            if (SUCCEEDED(D3D8SurfaceGetDesc(backBuffer, &dx8_hook_desc)) && dx8_hook_desc.Width > 0 && dx8_hook_desc.Height > 0) {
                *outWidth = dx8_hook_desc.Width;
                *outHeight = dx8_hook_desc.Height;
                ReleaseD3D8Surface(backBuffer);
                return true;
            }
            ReleaseD3D8Surface(backBuffer);
        }
    }

    if (hwnd) {
        RECT rect = {};
        if (GetClientRect(hwnd, &rect)) {
            const LONG width = rect.right - rect.left;
            const LONG height = rect.bottom - rect.top;
            if (width > 0 && height > 0) {
                *outWidth = static_cast<uint32_t>(width);
                *outHeight = static_cast<uint32_t>(height);
                return true;
            }
        }
    }

    return false;

}

bool DX8HelperRequired(SharedMemoryLayout* shm,  bool isRecording) {


    return isRecording || (shm && shm->graphicsConfig.prerenderLimit >= 0.0f);

}

HRESULT D3D8SurfaceGetDesc(IDirect3DSurface8* dx8_hook_surface,  D3D8_SURFACE_DESC_LOCAL* dx8_hook_desc) {


    D3D8SurfaceGetDesc_t fn =
        reinterpret_cast<D3D8SurfaceGetDesc_t>((*reinterpret_cast<void***>(dx8_hook_surface))[D3D8_SURFACE_VTABLE_GETDESC]);
    return fn(dx8_hook_surface, dx8_hook_desc);

}

HRESULT D3D8SurfaceLockRect(IDirect3DSurface8* dx8_hook_surface,  D3DLOCKED_RECT* lockedRect,  const RECT* rect, 
                                   DWORD flags) {


    D3D8SurfaceLockRect_t fn =
        reinterpret_cast<D3D8SurfaceLockRect_t>((*reinterpret_cast<void***>(dx8_hook_surface))[D3D8_SURFACE_VTABLE_LOCKRECT]);
    return fn(dx8_hook_surface, lockedRect, rect, flags);

}

HRESULT D3D8SurfaceUnlockRect(IDirect3DSurface8* dx8_hook_surface) {


    D3D8SurfaceUnlockRect_t fn = reinterpret_cast<D3D8SurfaceUnlockRect_t>(
        (*reinterpret_cast<void***>(dx8_hook_surface))[D3D8_SURFACE_VTABLE_UNLOCKRECT]);
    return fn(dx8_hook_surface);

}

void ReleaseD3D8Surface(IDirect3DSurface8*& dx8_hook_surface) {


    if (!dx8_hook_surface) {
        return;
    }

    D3D8SurfaceRelease_t fn =
        reinterpret_cast<D3D8SurfaceRelease_t>((*reinterpret_cast<void***>(dx8_hook_surface))[D3D8_SURFACE_VTABLE_RELEASE]);
    fn(dx8_hook_surface);
    dx8_hook_surface = nullptr;

}

uint8_t Expand4To8(uint32_t value) {


    return static_cast<uint8_t>((value << 4) | value);

}

uint8_t Expand5To8(uint32_t value) {


    return static_cast<uint8_t>((value << 3) | (value >> 2));

}

uint8_t Expand6To8(uint32_t value) {


    return static_cast<uint8_t>((value << 2) | (value >> 4));

}

uint32_t PackBgra8(uint8_t blue,  uint8_t green,  uint8_t red,  uint8_t alpha) {


    return static_cast<uint32_t>(blue) | (static_cast<uint32_t>(green) << 8) | (static_cast<uint32_t>(red) << 16) |
           (static_cast<uint32_t>(alpha) << 24);

}

void ApplyPrerenderLimitDX8(IDirect3DDevice8* device,  float dx8_hook_limit) {


    if (dx8_hook_limit < 0.0f)
        return;

    // We need D3D9Ex device for queries
    if (!dx8_hook_g_DX8Capture.d3d9DeviceEx) {
        HWND hwnd = ResolveD3D8TargetWindow(device, dx8_hook_g_CachedHwnd);
        if (hwnd) {
            if (!dx8_hook_g_DX8Capture.EnsureOverlayDevice(device, hwnd))
                return;
        } else
            return;
    }

    IDirect3DDevice9Ex* dev = dx8_hook_g_DX8Capture.d3d9DeviceEx;

    if (dx8_hook_g_PrerenderQueries.empty()) {
        dx8_hook_g_PrerenderQueries.resize(16, nullptr);
    }

    bool isFractional = (dx8_hook_limit > 0.01f && dx8_hook_limit < 1.0f);

    if (dx8_hook_limit == 0.0f) {
        // Strict Serial (Wait for current frame)
        IDirect3DQuery9* q = dx8_hook_g_PrerenderQueries[dx8_hook_g_PrerenderFrameIndex % dx8_hook_g_PrerenderQueries.size()];
        if (!q) {
            dev->CreateQuery(D3DQUERYTYPE_EVENT, &q);
            dx8_hook_g_PrerenderQueries[dx8_hook_g_PrerenderFrameIndex % dx8_hook_g_PrerenderQueries.size()] = q;
        }
        if (q) {
            q->Issue(D3DISSUE_END);
            while (q->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE) {
                SwitchToThread();
            }
        }
    } else {
        // Buffered Limit: For fractional limits (e.g., 0.5), we use Buffered 1
        // (Lookback 1) combined with an idle gap to approximate sub-frame latency.
        int effectiveLimit = isFractional ? 1 : (int)dx8_hook_limit;
        int lookback = effectiveLimit;

        IDirect3DQuery9* currentQ = dx8_hook_g_PrerenderQueries[dx8_hook_g_PrerenderFrameIndex % dx8_hook_g_PrerenderQueries.size()];
        if (!currentQ) {
            dev->CreateQuery(D3DQUERYTYPE_EVENT, &currentQ);
            dx8_hook_g_PrerenderQueries[dx8_hook_g_PrerenderFrameIndex % dx8_hook_g_PrerenderQueries.size()] = currentQ;
        }
        if (currentQ)
            currentQ->Issue(D3DISSUE_END);

        if (dx8_hook_g_PrerenderFrameIndex >= (uint64_t)lookback) {
            IDirect3DQuery9* waitQ = dx8_hook_g_PrerenderQueries[(dx8_hook_g_PrerenderFrameIndex - lookback) % dx8_hook_g_PrerenderQueries.size()];
            if (waitQ) {
                while (waitQ->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE) {
                    SwitchToThread();
                }
            }
        }
    }
    dx8_hook_g_PrerenderFrameIndex++;

    // Strict Serial + Fixed Idle Gap for fractional limits
    if (isFractional) {
        // effectiveLimit already set to 0 for Strict Serial above

        // After the wait completes, calculate and apply a fixed idle gap
        float fps = dx8_hook_g_PerfMetrics.GetCurrentFPS();
        double targetFrameTimeUs = (fps > 1.0f) ? (1000000.0 / fps) : 16666.0;

        // Fixed Idle Gap = TargetFrameTime * (1.0 - limit) * 0.10
        int64_t idleGapUs = (int64_t)(targetFrameTimeUs * (1.0 - dx8_hook_limit) * 0.10);
        if (idleGapUs > 0) {
            if (idleGapUs > 10000)
                idleGapUs = 10000;  // Cap at 10ms
            PrecisionSleep(idleGapUs);
        }
    }

}

void DrawDX8Overlay(IDirect3DDevice8* device,  HWND hwnd) {


    if (!device || !hwnd)
        return;

    uint32_t renderWidth = 0;
    uint32_t renderHeight = 0;
    if (!ResolveD3D8RenderSize(device, hwnd, &renderWidth, &renderHeight)) {
        return;
    }

    if (!g_OverlayAdapter.IsInitialized()) {
        dx8_hook_g_CachedHwnd = hwnd;
        InputManager::Get().HookWindow(hwnd);  // Hook input for menu
        g_OverlayAdapter.SetHwnd(hwnd);

        if (g_OverlayAdapter.InitDX8(device)) {
            g_OverlayAdapter.SetHwnd(hwnd);
            EarlyLog("DX8: OverlayAdapter initialized (direct DX8)");
        }

        if (!dx8_hook_g_DX8HooksInitialized && device) {
            InstallD3D8SamplerHooks(device);
            HookLog("DX8: State hooks initialized");
        }
    }

    g_OverlayAdapter.SetMetrics(&dx8_hook_g_PerfMetrics);
    g_OverlayAdapter.SetIPCClient(g_IPC);
    g_OverlayAdapter.SetDroppedFrames(dx8_hook_g_DX8Capture.droppedFrames.load(std::memory_order_relaxed));
    g_OverlayAdapter.SetGraphicsAPI("DX8", "active IDirect3DDevice8");

    {
        DX8StateHookBypassScope bypassScope;
        g_OverlayAdapter.RenderOverlay(static_cast<int>(renderWidth), static_cast<int>(renderHeight));
    }
    static uint32_t overlayRenderSubmitCount = 0;
    overlayRenderSubmitCount++;
    if (overlayRenderSubmitCount <= 8) {
        HookLogImportant("DX8: Overlay render submitted (hwnd=%p, size=%ux%u count=%u)", hwnd, renderWidth,
                         renderHeight, overlayRenderSubmitCount);
    }

}

HRESULT STDMETHODCALLTYPE DetourD3D8Present(IDirect3DDevice8* device,  const RECT* pSourceRect, 
                                                   const RECT* pDestRect,  HWND hDestWindowOverride, 
                                                   const RGNDATA* dx8_hook_pDirtyRegion) {


    if (HookIsShuttingDown())
        return D3D_OK;
    D3D8SamplerVTableRecord* samplerRecord = ResolveD3D8SamplerVTable(device);
    const D3D8SetTextureStageState_t setState = samplerRecord
                                                   ? samplerRecord->setState.load(std::memory_order_acquire)
                                                   : dx8_hook_oD3D8SetTextureStageState;
    const D3D8GetTextureStageState_t getState = samplerRecord
                                                   ? samplerRecord->getState.load(std::memory_order_acquire)
                                                   : dx8_hook_oD3D8GetTextureStageState;
    ce::legacy_d3d_sampler_state::RefreshConfiguration(
        ce::legacy_d3d_sampler_state::Api::D3D8, device,
        reinterpret_cast<ce::legacy_d3d_sampler_state::SetTextureStageStateFn>(setState),
        reinterpret_cast<ce::legacy_d3d_sampler_state::GetTextureStageStateFn>(getState), QueryD3D8MaxAnisotropy);
    // Update performance metrics
    static int64_t qpcFreq = 0;
    if (qpcFreq == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        qpcFreq = f.QuadPart;
    }
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    int64_t us = (qpc.QuadPart * 1000000) / qpcFreq;
    dx8_hook_g_PerfMetrics.Update(us);

    SharedMemoryLayout* shm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
    bool captureIncludeOverlay = shm ? shm->overlayConfig.captureIncludeOverlay : true;
    bool shouldDrawOverlay = shm && shm->overlayConfig.showOverlay;
    bool isRecording = g_IPC && g_IPC->IsRecording();
    bool helperRequired = DX8HelperRequired(shm, isRecording);
    HWND targetHwnd = ResolveD3D8TargetWindow(device, hDestWindowOverride);
    if (targetHwnd) {
        dx8_hook_g_CachedHwnd = targetHwnd;
    }
    auto ensureCapture = [&]() {
        if (!targetHwnd) {
            return false;
        }
        if (!dx8_hook_g_DX8Capture.initialized) {
            dx8_hook_g_DX8Capture.Init(device, targetHwnd);
        }
        return dx8_hook_g_DX8Capture.initialized;
    };

    // CPU Prerender Limit
    if (g_IPC && g_IPC->GetSharedMem()->graphicsConfig.prerenderLimit >= 0) {
        ApplyPrerenderLimitDX8(device, g_IPC->GetSharedMem()->graphicsConfig.prerenderLimit);
    }

    if (isRecording && shouldDrawOverlay && !captureIncludeOverlay && ensureCapture()) {
        dx8_hook_g_DX8Capture.CaptureFrame(device, false);
    }

    if (shouldDrawOverlay) {
        DrawDX8Overlay(device, targetHwnd);
    }

    HRESULT hr = dx8_hook_oD3D8Present(device, pSourceRect, pDestRect, hDestWindowOverride, dx8_hook_pDirtyRegion);

    if (isRecording && (!shouldDrawOverlay || captureIncludeOverlay) && ensureCapture()) {
        dx8_hook_g_DX8Capture.CaptureFrame(device, true);
    } else if (!helperRequired && (dx8_hook_g_DX8Capture.initialized || dx8_hook_g_DX8Capture.d3d9DeviceEx)) {
        dx8_hook_g_DX8Capture.Cleanup();
    }

    // Apply FPS limiter
    g_SharedFpsLimiter.SetIPCClient(g_IPC);
    g_SharedFpsLimiter.Apply();

    return hr;

}

HRESULT STDMETHODCALLTYPE DetourD3D8Reset(IDirect3DDevice8* device,  void* dx8_hook_pPresentationParameters) {


    HookLog("DX8: Reset called");

    // Cleanup ImGui
    // Cleanup OverlayAdapter
    if (g_OverlayAdapter.IsInitialized()) {
        g_OverlayAdapter.Shutdown();
    }

    // Cleanup capture
    dx8_hook_g_DX8Capture.PrepareForDeviceReset();

    // VSync Override
    if (g_IPC) {
        std::string mode = g_IPC->GetSharedMem()->graphicsConfig.vsyncMode;
        if (mode != "default" && dx8_hook_pPresentationParameters) {
            D3D8_PRESENT_PARAMETERS* pp = (D3D8_PRESENT_PARAMETERS*)dx8_hook_pPresentationParameters;
            if (mode == "off" || mode == "mailbox")
                pp->FullScreen_PresentationInterval = 0x80000000;  // D3DPRESENT_INTERVAL_IMMEDIATE
            else if (mode == "fifo" || mode == "adaptive")
                pp->FullScreen_PresentationInterval = 0x00000001;
        }

        // Backbuffer Count override
        int count = g_IPC->GetSharedMem()->graphicsConfig.backbufferCount;
        if (count >= 2 && count <= 6 && dx8_hook_pPresentationParameters) {
            D3D8_PRESENT_PARAMETERS* pp = (D3D8_PRESENT_PARAMETERS*)dx8_hook_pPresentationParameters;
            pp->BackBufferCount = (UINT)count - 1;
            HookLog("DX8: Reset: Overriding BackBufferCount to %d", count);
        }

        // MSAA override
        if (dx8_hook_pPresentationParameters) {
            // We need the IDirect3D8 object to check support, but Reset doesn't
            // provide it We'll trust the user and just apply it if it's discarded
            // swap effect anyway
            D3D8_PRESENT_PARAMETERS* pp = (D3D8_PRESENT_PARAMETERS*)dx8_hook_pPresentationParameters;
            const char* msaa = g_IPC->GetSharedMem()->graphicsConfig.msaaSamples;
            if (msaa[0] != 'd') {
                DWORD msType = ParseD3D8MSAA(msaa);
                if (msType != 0) {
                    pp->MultiSampleType = msType;
                    pp->SwapEffect = 1;  // DISCARD
                } else if (strcmp(msaa, "off") == 0) {
                    pp->MultiSampleType = 0;
                }
            }
        }
    }

    const HRESULT hr = dx8_hook_oD3D8Reset(device, dx8_hook_pPresentationParameters);
    if (SUCCEEDED(hr)) {
        ce::legacy_d3d_sampler_state::ResetDevice(ce::legacy_d3d_sampler_state::Api::D3D8, device);
    }
    return hr;

}

HRESULT STDMETHODCALLTYPE DetourD3D8SetTextureStageState(IDirect3DDevice8* device,  DWORD Stage,  DWORD Type, 
                                                                DWORD dx8_hook_Value) {


    D3D8SamplerVTableRecord* record = ResolveD3D8SamplerVTable(device);
    const D3D8SetTextureStageState_t setState =
        record ? record->setState.load(std::memory_order_acquire) : dx8_hook_oD3D8SetTextureStageState;
    const D3D8GetTextureStageState_t getState =
        record ? record->getState.load(std::memory_order_acquire) : dx8_hook_oD3D8GetTextureStageState;
    if (dx8_hook_g_DX8StateHookBypassDepth > 0) {
        return setState(device, Stage, Type, dx8_hook_Value);
    }
    return ce::legacy_d3d_sampler_state::SetTextureStageState(
        ce::legacy_d3d_sampler_state::Api::D3D8, device, Stage, Type, dx8_hook_Value,
        reinterpret_cast<ce::legacy_d3d_sampler_state::SetTextureStageStateFn>(setState),
        reinterpret_cast<ce::legacy_d3d_sampler_state::GetTextureStageStateFn>(getState), QueryD3D8MaxAnisotropy);

}

HRESULT STDMETHODCALLTYPE DetourD3D8GetTextureStageState(IDirect3DDevice8* device,  DWORD Stage,  DWORD Type, 
                                                                 DWORD* dx8_hook_pValue) {


    D3D8SamplerVTableRecord* record = ResolveD3D8SamplerVTable(device);
    const D3D8SetTextureStageState_t setState =
        record ? record->setState.load(std::memory_order_acquire) : dx8_hook_oD3D8SetTextureStageState;
    const D3D8GetTextureStageState_t getState =
        record ? record->getState.load(std::memory_order_acquire) : dx8_hook_oD3D8GetTextureStageState;
    if (dx8_hook_g_DX8StateHookBypassDepth > 0) {
        return getState(device, Stage, Type, dx8_hook_pValue);
    }
    return ce::legacy_d3d_sampler_state::GetTextureStageState(
        ce::legacy_d3d_sampler_state::Api::D3D8, device, Stage, Type, dx8_hook_pValue,
        reinterpret_cast<ce::legacy_d3d_sampler_state::GetTextureStageStateFn>(getState),
        reinterpret_cast<ce::legacy_d3d_sampler_state::SetTextureStageStateFn>(setState), QueryD3D8MaxAnisotropy);

}

HRESULT STDMETHODCALLTYPE DetourD3D8ApplyStateBlock(IDirect3DDevice8* device,  DWORD dx8_hook_Token) {


    D3D8SamplerVTableRecord* record = ResolveD3D8SamplerVTable(device);
    const D3D8ApplyStateBlock_t applyStateBlock =
        record ? record->applyStateBlock.load(std::memory_order_acquire) : dx8_hook_oD3D8ApplyStateBlock;
    if (!applyStateBlock)
        return E_FAIL;
    const HRESULT hr = applyStateBlock(device, dx8_hook_Token);
    if (SUCCEEDED(hr) && dx8_hook_g_DX8StateHookBypassDepth == 0) {
        const D3D8SetTextureStageState_t setState =
            record ? record->setState.load(std::memory_order_acquire) : dx8_hook_oD3D8SetTextureStageState;
        const D3D8GetTextureStageState_t getState =
            record ? record->getState.load(std::memory_order_acquire) : dx8_hook_oD3D8GetTextureStageState;
        ce::legacy_d3d_sampler_state::ReconcileAfterExternalStateChange(
            ce::legacy_d3d_sampler_state::Api::D3D8, device,
            reinterpret_cast<ce::legacy_d3d_sampler_state::SetTextureStageStateFn>(setState),
            reinterpret_cast<ce::legacy_d3d_sampler_state::GetTextureStageStateFn>(getState), QueryD3D8MaxAnisotropy);
    }
    return hr;

}
