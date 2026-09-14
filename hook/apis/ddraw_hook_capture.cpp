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

void ComposePresentation(IDirectDrawSurface7* visibleSurface, IDirectDrawSurface7* presentSource,
                         ce::ddraw_present_policy::PresentKind kind, bool haveChangedRect,
                         const ce::ddraw_present_policy::Rect& changedRect) {
    if (HookIsShuttingDown())
        return;

    const auto target = ce::ddraw_present_policy::SelectCompositeTarget(kind, presentSource != nullptr);
    if (target == ce::ddraw_present_policy::CompositeTarget::None)
        return;

    IDirectDrawSurface7* compositeTarget =
        target == ce::ddraw_present_policy::CompositeTarget::PresentSource ? presentSource : visibleSurface;
    if (!compositeTarget)
        return;

    // Compositing re-enters DirectDraw through the same hooked surface methods
    // (Lock/Unlock, GetDC/ReleaseDC), so the recursion guard has to cover the
    // whole presentation, not just the capture inside it.
    ddraw_hook_g_CaptureRecurse++;
    if (ddraw_hook_g_CaptureRecurse > 1) {
        ddraw_hook_g_CaptureRecurse--;
        return;
    }

    SharedMemoryLayout* shm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
    const bool captureIncludeOverlay = shm ? shm->overlayConfig.captureIncludeOverlay : true;
    const bool shouldDrawOverlay = shm && shm->overlayConfig.showOverlay;
    const bool isRecording = g_IPC && g_IPC->IsRecording();
    HWND targetHwnd = ResolveDirectDrawTargetWindow();

    uint32_t surfaceWidth = 0;
    uint32_t surfaceHeight = 0;
    const bool haveSurfaceSize =
        GetSurfaceSize(compositeTarget, surfaceWidth, surfaceHeight) && surfaceWidth > 0 && surfaceHeight > 0;

    static bool loggedFirstPresentation = false;
    if (!loggedFirstPresentation) {
        HookLogImportant(
            "DDraw: First presentation kind=%d visible=%p presentSource=%p composite=%p hwnd=%p recording=%d "
            "showOverlay=%d captureIncludeOverlay=%d size=%ux%u",
            static_cast<int>(kind), visibleSurface, presentSource, compositeTarget, targetHwnd, isRecording ? 1 : 0,
            shouldDrawOverlay ? 1 : 0, captureIncludeOverlay ? 1 : 0, surfaceWidth, surfaceHeight);
        loggedFirstPresentation = true;
    }

    if (shouldDrawOverlay && haveSurfaceSize) {
        ddraw_hook_g_DDrawCapture.EnsureOverlayDevice(targetHwnd, surfaceWidth, surfaceHeight);
    }

    // A direct-scanout change is already on screen; only the part of it that
    // overwrote the overlay's own pixels needs the overlay put back.
    if (shouldDrawOverlay && kind == ce::ddraw_present_policy::PresentKind::DirectScanout && haveChangedRect) {
        RECT overlayBounds = {};
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        if (g_OverlayAdapter.GetLastRenderedBounds(static_cast<int>(surfaceWidth), static_cast<int>(surfaceHeight),
                                                   overlayBounds)) {
            const ce::ddraw_present_policy::Rect bounds{
                static_cast<int>(overlayBounds.left), static_cast<int>(overlayBounds.top),
                static_cast<int>(overlayBounds.right), static_cast<int>(overlayBounds.bottom)};
            if (!ce::ddraw_present_policy::DirectScanoutNeedsComposite(bounds, true, changedRect)) {
                ddraw_hook_g_CaptureRecurse--;
                return;
            }
        }
    }

    auto doOverlay = [&]() {
        if (shouldDrawOverlay) {
            DrawDDrawOverlay(compositeTarget);
        }
    };

    // A partial scanout update restores the overlay but is not a frame: feeding
    // every HUD or cursor blit to the recorder would publish the same frame
    // many times over.
    const bool changeCoversSurface =
        !haveChangedRect || (changedRect.left <= 0 && changedRect.top <= 0 &&
                             changedRect.right >= static_cast<int>(surfaceWidth) &&
                             changedRect.bottom >= static_cast<int>(surfaceHeight));

    auto doCapture = [&]() {
        if (!isRecording || !changeCoversSurface)
            return;
        if (!ddraw_hook_g_DDrawCapture.initialized && haveSurfaceSize) {
            ddraw_hook_g_DDrawCapture.EnsureCaptureResources(compositeTarget, targetHwnd, surfaceWidth, surfaceHeight);
        }
        if (ddraw_hook_g_DDrawCapture.initialized) {
            ddraw_hook_g_DDrawCapture.CaptureFrameFromSurface(compositeTarget);
        }
    };

    // Both the recording and the screen now read the same image, so the only
    // question left is whether the recording sees the overlay in it.
    if (captureIncludeOverlay) {
        doOverlay();
        doCapture();
    } else {
        doCapture();
        doOverlay();
    }

    ddraw_hook_g_CaptureRecurse--;
}

void NotePresentationComplete() {
    if (HookIsShuttingDown())
        return;
    // The composite locks and unlocks DirectDraw surfaces through the same
    // hooked methods, so a nested Unlock of the scanout surface reaches here
    // while a presentation is still being composited. Counting that as a frame
    // would corrupt the frame-time series and let the limiter sleep inside the
    // composite.
    if (ddraw_hook_g_CaptureRecurse != 0)
        return;

    g_RenderWatchdog.Heartbeat();

    static int64_t qpcFreq = 0;
    if (qpcFreq == 0) {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        qpcFreq = frequency.QuadPart;
    }
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    ddraw_hook_g_PerfMetrics.Update(DisplayTimingQpcToUs(qpc.QuadPart, qpcFreq));

    g_SharedFpsLimiter.SetIPCClient(g_IPC);
    g_SharedFpsLimiter.Apply();
}

void HandlePresentationSurface4(IDirectDrawSurface4* visibleSurface, IDirectDrawSurface4* presentSource,
                                ce::ddraw_present_policy::PresentKind kind, bool haveChangedRect,
                                const ce::ddraw_present_policy::Rect& changedRect) {
    IDirectDrawSurface7* visibleSurface7 = QuerySurface7(visibleSurface);
    if (!visibleSurface7) {
        static int primaryUpgradeFailLogCount = 0;
        if (primaryUpgradeFailLogCount < 4) {
            HookLog("DDraw: Failed to upgrade DirectDraw4 surface to DirectDraw7 for capture/overlay");
            primaryUpgradeFailLogCount++;
        }
        return;
    }

    IDirectDrawSurface7* presentSource7 = QuerySurface7(presentSource);
    ComposePresentation(visibleSurface7, presentSource7, kind, haveChangedRect, changedRect);

    if (presentSource7) {
        presentSource7->Release();
    }
    visibleSurface7->Release();
}

void HandlePresentationLegacySurface(IDirectDrawSurface* visibleSurface, IDirectDrawSurface* presentSource,
                                     ce::ddraw_present_policy::PresentKind kind, bool haveChangedRect,
                                     const ce::ddraw_present_policy::Rect& changedRect) {
    IDirectDrawSurface7* visibleSurface7 = QuerySurface7(visibleSurface);
    if (!visibleSurface7) {
        static std::atomic<int> s_upgradeFailureLogCount{0};
        if (s_upgradeFailureLogCount.fetch_add(1, std::memory_order_relaxed) < 4) {
            HookLogImportant("DDraw: Failed to upgrade legacy surface to Surface7 for capture/overlay");
        }
        return;
    }
    IDirectDrawSurface7* presentSource7 = QuerySurface7(presentSource);
    ComposePresentation(visibleSurface7, presentSource7, kind, haveChangedRect, changedRect);
    if (presentSource7)
        presentSource7->Release();
    visibleSurface7->Release();
}
