#include "dx8_hook_internal.h"


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
