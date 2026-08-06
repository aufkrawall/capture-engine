#include "dx9_hook_internal.h"


void DrawDX9Overlay(IDirect3DDevice9* device) {


    if (ShouldSkipDX9OverlayForVulkan()) {
        return;
    }
    static int drawLogCount = 0;
    static int initFailCount = 0;

    if (drawLogCount < 5) {
        SharedMemoryLayout* dbgShm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
        HookLogImportant("DX9: DrawDX9Overlay #%d, IsInitialized=%d, IPC=%p, SHM=%p, showOverlay=%d", drawLogCount,
                         g_OverlayAdapter.IsInitialized() ? 1 : 0, (void*)g_IPC, (void*)dbgShm,
                         dbgShm ? dbgShm->ReadOverlayConfig().showOverlay : -1);
        drawLogCount++;
    }

    if (!g_OverlayAdapter.IsInitialized()) {
        // Get the window handle
        D3DDEVICE_CREATION_PARAMETERS params;
        HRESULT paramsHr = device->GetCreationParameters(&params);
        if (FAILED(paramsHr)) {
            if (initFailCount < 3) {
                EarlyLog("DX9: GetCreationParameters failed (hr=0x%08X)", paramsHr);
                initFailCount++;
            }
            return;
        }
        dx9_hook_g_CachedHwnd = params.hFocusWindow;

        // Hook Input
        InputManager::Get().HookWindow(dx9_hook_g_CachedHwnd);
        g_OverlayAdapter.SetHwnd(dx9_hook_g_CachedHwnd);

        EarlyLog("DX9: Attempting OverlayAdapter::InitDX9 (device=%p, hwnd=%p)", (void*)device, (void*)dx9_hook_g_CachedHwnd);
        if (g_OverlayAdapter.InitDX9(device)) {
            g_OverlayAdapter.SetHwnd(dx9_hook_g_CachedHwnd);
            EarlyLog("DX9: OverlayAdapter initialized successfully");
        } else {
            if (initFailCount < 3) {
                EarlyLog("DX9: OverlayAdapter::InitDX9 FAILED");
                initFailCount++;
            }
            return;
        }
    }

    // Get viewport size
    D3DVIEWPORT9 vp;
    device->GetViewport(&vp);

    static int vpLogCount = 0;
    if (vpLogCount < 3) {
        HookLogImportant("DX9: DrawDX9Overlay vp=%ux%u (device=%p, IPC=%p)", vp.Width, vp.Height, (void*)device,
                         (void*)g_IPC);
        vpLogCount++;
    }

    g_OverlayAdapter.SetMetrics(&dx9_hook_g_PerfMetrics);
    g_OverlayAdapter.SetIPCClient(g_IPC);
    g_OverlayAdapter.SetDroppedFrames(dx9_hook_g_DX9Capture.droppedFrames.load(std::memory_order_relaxed));
    const bool isEx = ResolveD3D9DeviceIsEx(device);
    const char* finalApi = ce::graphics_api_identity::D3D9Label(isEx, IsDXVKD3D9WrapperLoaded());
    g_OverlayAdapter.SetGraphicsAPI(finalApi, isEx ? "active IDirect3DDevice9Ex" : "active IDirect3DDevice9");

    // Render Custom Overlay
    // Note: RenderOverlay calls BeginFrame/RenderContent/EndFrame.
    // DX9 backend handles state saving/restoring internally.
    dx9_hook_g_InOverlayRender = true;
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    g_OverlayAdapter.RenderOverlay(vp.Width, vp.Height);
    dx9_hook_g_InOverlayRender = false;

}
void CaptureDX9Screenshot(IDirect3DDevice9* device,  SharedMemoryLayout* shm,  uint64_t requestId) {


    if (!device || !shm || requestId == 0)
        return;

    bool queued = false;
    IDirect3DSurface9* bb = nullptr;
    if (SUCCEEDED(device->GetRenderTarget(0, &bb)) && bb) {
        D3DSURFACE_DESC bbDesc;
        bb->GetDesc(&bbDesc);

        IDirect3DSurface9* staging = nullptr;
        if (SUCCEEDED(device->CreateOffscreenPlainSurface(bbDesc.Width, bbDesc.Height, bbDesc.Format, D3DPOOL_SYSTEMMEM,
                                                          &staging, NULL))) {
            if (SUCCEEDED(device->GetRenderTargetData(bb, staging))) {
                D3DLOCKED_RECT locked;
                if (SUCCEEDED(staging->LockRect(&locked, NULL, D3DLOCK_READONLY))) {
                    if (locked.Pitch > 0) {
                        queued = QueueScreenshotPixels(shm, requestId, static_cast<const uint8_t*>(locked.pBits),
                                                       bbDesc.Width, bbDesc.Height, static_cast<uint32_t>(locked.Pitch),
                                                       ScreenshotPixelFormat::BGRA8, ScreenshotColorEncoding::SRGB);
                    }
                    staging->UnlockRect();
                }
            }
            staging->Release();
        }
        bb->Release();
    }
    if (!queued)
        CompleteScreenshotRequest(shm, requestId, ScreenshotRequestStatus::Failed, ERROR_READ_FAULT);

}
