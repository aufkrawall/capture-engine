#include "ddraw_hook_internal.h"

static ce::graphics_api_identity::DirectDrawVersion DirectDrawVersionFromIID(REFIID iid) {
    if (IsEqualIID(iid, IID_IDirectDraw7))
        return ce::graphics_api_identity::DirectDrawVersion::DirectDraw7;
    if (IsEqualIID(iid, IID_IDirectDraw4))
        return ce::graphics_api_identity::DirectDrawVersion::DirectDraw4;
    if (IsEqualIID(iid, ddraw_hook_kIID_IDirectDraw3))
        return ce::graphics_api_identity::DirectDrawVersion::DirectDraw3;
    if (IsEqualIID(iid, IID_IDirectDraw2))
        return ce::graphics_api_identity::DirectDrawVersion::DirectDraw2;
    if (IsEqualIID(iid, IID_IDirectDraw))
        return ce::graphics_api_identity::DirectDrawVersion::DirectDraw;
    return ce::graphics_api_identity::DirectDrawVersion::Unknown;
}

bool HookDirectDrawObject(void* directDrawObject, REFIID iid) {
    HookLog("DDraw: HookDirectDrawObject called (object=%p, iidIsDDraw7=%d, iidIsDDraw4=%d)", directDrawObject,
            IsEqualIID(iid, IID_IDirectDraw7) ? 1 : 0, IsEqualIID(iid, IID_IDirectDraw4) ? 1 : 0);

    if (!directDrawObject)
        return false;

    if (ShouldSuppressDirectDrawHooking()) {
        return false;
    }

    const auto requestedVersion = DirectDrawVersionFromIID(iid);
    if (requestedVersion != ce::graphics_api_identity::DirectDrawVersion::Unknown && ddraw_hook_g_DDrawBootstrapDepth != 0) {
        static std::atomic<int> s_ignoredBootstrapIdentityLogs{0};
        if (s_ignoredBootstrapIdentityLogs.fetch_add(1, std::memory_order_relaxed) < 4) {
            HookLog("[GraphicsAPI] ignored synthetic DirectDraw bootstrap interface api=%s",
                    ce::graphics_api_identity::DirectDrawLabel(requestedVersion));
        }
    }
    if (requestedVersion != ce::graphics_api_identity::DirectDrawVersion::Unknown && ddraw_hook_g_DDrawBootstrapDepth == 0) {
        ddraw_hook_g_LegacyD3DCallbackVersion.store(0, std::memory_order_release);
        ddraw_hook_g_ActiveLegacyD3DVersion.store(0, std::memory_order_release);
        const int previous =
            ddraw_hook_g_ActiveDirectDrawVersion.exchange(static_cast<int>(requestedVersion), std::memory_order_acq_rel);
        if (previous != static_cast<int>(requestedVersion)) {
            HookLogImportant("[GraphicsAPI] DirectDraw interface accepted api=%s evidence=application creation",
                             ce::graphics_api_identity::DirectDrawLabel(requestedVersion));
        }
    }

    if (IsEqualIID(iid, IID_IDirectDraw7)) {
        InstallDirectDrawHooksForInstance(reinterpret_cast<IDirectDraw7*>(directDrawObject), "wrapper CreateEx");
        return true;
    }

    if (IsEqualIID(iid, IID_IDirectDraw4)) {
        auto* ddraw4 = reinterpret_cast<IDirectDraw4*>(directDrawObject);
        InstallDirectDraw4HooksForInstance(ddraw4, "wrapper CreateEx");

        IDirectDraw7* ddraw7 = nullptr;
        if (SUCCEEDED(ddraw4->QueryInterface(IID_IDirectDraw7, reinterpret_cast<void**>(&ddraw7))) && ddraw7) {
            InstallDirectDrawHooksForInstance(ddraw7, "wrapper CreateEx upgrade");
            ddraw7->Release();
        }
        return true;
    }

    if (requestedVersion == ce::graphics_api_identity::DirectDrawVersion::DirectDraw ||
        requestedVersion == ce::graphics_api_identity::DirectDrawVersion::DirectDraw2 ||
        requestedVersion == ce::graphics_api_identity::DirectDrawVersion::DirectDraw3) {
        auto* legacy = reinterpret_cast<IDirectDraw*>(directDrawObject);
        InstallLegacyDirectDrawHooksForInstance(legacy, requestedVersion, "wrapper creation");

        IDirectDraw7* ddraw7 = nullptr;
        if (SUCCEEDED(legacy->QueryInterface(IID_IDirectDraw7, reinterpret_cast<void**>(&ddraw7))) && ddraw7) {
            InstallDirectDrawHooksForInstance(ddraw7, "wrapper creation upgrade");
            ddraw7->Release();
        }
        return true;
    }

    return false;
}

void HandleCapture(IDirectDrawSurface7* primarySurface, IDirectDrawSurface7* explicitSourceSurface) {
    if (HookIsShuttingDown())
        return;
    ddraw_hook_g_CaptureRecurse++;
    if (ddraw_hook_g_CaptureRecurse > 1) {
        ddraw_hook_g_CaptureRecurse--;
        return;
    }

    g_RenderWatchdog.Heartbeat();

    // Update performance metrics
    static int64_t qpcFreq = 0;
    if (qpcFreq == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        qpcFreq = f.QuadPart;
    }
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    int64_t us = DisplayTimingQpcToUs(qpc.QuadPart, qpcFreq);
    ddraw_hook_g_PerfMetrics.Update(us);

    SharedMemoryLayout* shm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
    bool captureIncludeOverlay = shm ? shm->overlayConfig.captureIncludeOverlay : true;
    bool shouldDrawOverlay = shm && shm->overlayConfig.showOverlay;
    bool isRecording = g_IPC && g_IPC->IsRecording();
    HWND targetHwnd = ResolveDirectDrawTargetWindow();
    uint32_t surfaceWidth = 0;
    uint32_t surfaceHeight = 0;
    const bool haveSurfaceSize =
        GetSurfaceSize(primarySurface, surfaceWidth, surfaceHeight) && surfaceWidth > 0 && surfaceHeight > 0;
    IDirectDrawSurface7* presentationSurface =
        ResolvePreferredPresentationSurface(primarySurface, explicitSourceSurface);

    static bool loggedFirstHandleCapture = false;
    if (!loggedFirstHandleCapture) {
        HookLogImportant(
            "DDraw: First HandleCapture surface=%p hwnd=%p recording=%d showOverlay=%d captureIncludeOverlay=%d "
            "size=%ux%u presentation=%p",
            primarySurface, targetHwnd, isRecording ? 1 : 0, shouldDrawOverlay ? 1 : 0, captureIncludeOverlay ? 1 : 0,
            surfaceWidth, surfaceHeight, presentationSurface);
        loggedFirstHandleCapture = true;
    }

    if (shouldDrawOverlay && haveSurfaceSize) {
        ddraw_hook_g_DDrawCapture.EnsureOverlayDevice(targetHwnd, surfaceWidth, surfaceHeight);
    }

    // Lambda for overlay drawing: overlay MUST ALWAYS be drawn onto the primary surface (front display buffer)
    auto doOverlay = [&]() {
        if (shouldDrawOverlay) {
            DrawDDrawOverlay(primarySurface);
        }
    };

    // Lambda for capture operation
    auto doCapture = [&]() {
        if (isRecording) {
            if (!ddraw_hook_g_DDrawCapture.initialized && haveSurfaceSize) {
                ddraw_hook_g_DDrawCapture.EnsureCaptureResources(primarySurface, targetHwnd, surfaceWidth, surfaceHeight);
            }

            if (ddraw_hook_g_DDrawCapture.initialized) {
                // If recording includes overlay, capture the primary surface which now contains the composited overlay.
                // Otherwise capture from the clean presentation surface (or primary surface if none).
                IDirectDrawSurface7* captureTarget =
                    captureIncludeOverlay ? primarySurface : (presentationSurface ? presentationSurface : primarySurface);
                ddraw_hook_g_DDrawCapture.CaptureFrameFromSurface(captureTarget);
            }
        }
    };

    // Order capture/overlay based on config
    if (captureIncludeOverlay) {
        doOverlay();  // Draw overlay first onto primary surface
        doCapture();  // Then capture primary surface (includes overlay)
    } else {
        doCapture();  // Capture first (clean frame)
        doOverlay();  // Then draw overlay onto primary surface (visible on screen but not in recording)
    }

    // Apply FPS limiter
    g_SharedFpsLimiter.SetIPCClient(g_IPC);
    g_SharedFpsLimiter.Apply();

    ddraw_hook_g_CaptureRecurse--;
}

void HandleCaptureSurface4(IDirectDrawSurface4* primarySurface, IDirectDrawSurface4* explicitSourceSurface) {
    IDirectDrawSurface7* primarySurface7 = QuerySurface7(primarySurface);
    if (!primarySurface7) {
        static int primaryUpgradeFailLogCount = 0;
        if (primaryUpgradeFailLogCount < 4) {
            HookLog("DDraw: Failed to upgrade DirectDraw4 primary surface to DirectDraw7 for capture/overlay");
            primaryUpgradeFailLogCount++;
        }
        return;
    }

    IDirectDrawSurface7* explicitSourceSurface7 = QuerySurface7(explicitSourceSurface);
    HandleCapture(primarySurface7, explicitSourceSurface7);

    if (explicitSourceSurface7) {
        explicitSourceSurface7->Release();
    }
    primarySurface7->Release();
}

void HandleCaptureLegacySurface(IDirectDrawSurface* primarySurface, IDirectDrawSurface* explicitSourceSurface) {
    IDirectDrawSurface7* primarySurface7 = QuerySurface7(primarySurface);
    if (!primarySurface7) {
        static std::atomic<int> s_upgradeFailureLogCount{0};
        if (s_upgradeFailureLogCount.fetch_add(1, std::memory_order_relaxed) < 4) {
            HookLogImportant("DDraw: Failed to upgrade legacy primary surface to Surface7 for capture/overlay");
        }
        return;
    }
    IDirectDrawSurface7* explicitSourceSurface7 = QuerySurface7(explicitSourceSurface);
    HandleCapture(primarySurface7, explicitSourceSurface7);
    if (explicitSourceSurface7)
        explicitSourceSurface7->Release();
    primarySurface7->Release();
}
