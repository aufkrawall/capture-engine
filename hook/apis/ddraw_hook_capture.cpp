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

void LogDirectDrawPresentationMix(const char* reason) {
    auto& diag = ddraw_hook_g_PresentationDiagnostics;
    const char* routeLabel = ddraw_hook_g_OverlayRoute == DDrawOverlayRoute::NativeLegacyD3D ? "native-d3d7"
                             : ddraw_hook_g_OverlayRoute == DDrawOverlayRoute::HelperComposite ? "cpu-composite"
                                                                                               : "undecided";
    const uint32_t nativePresentations = diag.nativePresentations.load(std::memory_order_relaxed);
    const auto averageUs = [](std::atomic<uint64_t>& total, uint32_t divisor) {
        return static_cast<unsigned long long>(total.load(std::memory_order_relaxed) / (std::max)(1u, divisor));
    };
    HookLogImportant(
        "DDraw: Presentation mix (%s) flips=%u blitPresents=%u directScanoutBlits=%u ignoredBlits=%u "
        "primaryUnlocks=%u scanoutUnlocks=%u getDcPresents=%u surfaces=%u hookReuses=%u nativeScene=%u "
        "nativePresent=%u nativeFail=%u nativeRepair=%u nativeDeferred=%u captureNativeDeferred=%u "
        "composites=%u ok=%u noGeometry=%u "
        "writeFailed=%u reentrant=%u skippedNoPublishedImage=%u skippedOutsideOverlay=%u leftToFlip=%u writeMarks=%u clean=%u "
        "partial=%u "
        "full=%u raster=%u rasterFull=%u rasterIncr=%u spriteReuse=%u compositeAvgUs=%llu compositeMaxUs=%u "
        "rasterAvgUs=%llu rasterMaxUs=%u lockAvgUs=%llu lockMaxUs=%u writeAvgUs=%llu writeMaxUs=%u "
        "route=%s routeSwitches=%u",
        reason, diag.flips.load(std::memory_order_relaxed), diag.blitPresents.load(std::memory_order_relaxed),
        diag.directScanoutBlits.load(std::memory_order_relaxed), diag.ignoredBlits.load(std::memory_order_relaxed),
        diag.primaryUnlockPresentations.load(std::memory_order_relaxed),
        diag.scanoutUnlocks.load(std::memory_order_relaxed), diag.getDcPresentations.load(std::memory_order_relaxed),
        diag.surfaceCreations.load(std::memory_order_relaxed),
        diag.surfaceHookReuses.load(std::memory_order_relaxed),
        diag.nativeSceneDraws.load(std::memory_order_relaxed), nativePresentations,
        diag.nativeDrawFailures.load(std::memory_order_relaxed),
        diag.nativeRepairRegions.load(std::memory_order_relaxed),
        diag.nativeUnsafeDeferrals.load(std::memory_order_relaxed),
        diag.captureNativeDeferrals.load(std::memory_order_relaxed),
        diag.composites.load(std::memory_order_relaxed),
        diag.compositeSucceeded.load(std::memory_order_relaxed),
        diag.compositeNoGeometry.load(std::memory_order_relaxed),
        diag.compositeWriteFailed.load(std::memory_order_relaxed),
        diag.reentrantPresentations.load(std::memory_order_relaxed),
        diag.skippedNoPublishedImage.load(std::memory_order_relaxed),
        diag.skippedOutsideOverlay.load(std::memory_order_relaxed),
        diag.scanoutWritesLeftToFlip.load(std::memory_order_relaxed),
        diag.applicationWritesMarked.load(std::memory_order_relaxed),
        diag.compositesSkippedClean.load(std::memory_order_relaxed),
        diag.compositePartialWrites.load(std::memory_order_relaxed),
        diag.compositeFullWrites.load(std::memory_order_relaxed),
        diag.spriteRasterizations.load(std::memory_order_relaxed),
        diag.spriteFullRasters.load(std::memory_order_relaxed),
        diag.spriteIncrementalUpdates.load(std::memory_order_relaxed),
        diag.spriteReuses.load(std::memory_order_relaxed),
        averageUs(diag.compositeMicrosecondsTotal,
                  diag.compositeTimedPresentations.load(std::memory_order_relaxed)),
        diag.compositeMicrosecondsMax.load(std::memory_order_relaxed),
        averageUs(diag.rasterMicrosecondsTotal, diag.rasterPasses.load(std::memory_order_relaxed)),
        diag.rasterMicrosecondsMax.load(std::memory_order_relaxed),
        averageUs(diag.lockMicrosecondsTotal, diag.surfaceWritePasses.load(std::memory_order_relaxed)),
        diag.lockMicrosecondsMax.load(std::memory_order_relaxed),
        averageUs(diag.writeMicrosecondsTotal, diag.surfaceWritePasses.load(std::memory_order_relaxed)),
        diag.writeMicrosecondsMax.load(std::memory_order_relaxed), routeLabel, ddraw_hook_g_OverlayRouteSwitches);
}

bool ComposePresentation(IDirectDrawSurface7* visibleSurface, IDirectDrawSurface7* presentSource,
                         ce::ddraw_present_policy::PresentKind kind, bool haveChangedRect,
                         const ce::ddraw_present_policy::Rect& changedRect) {
    if (HookIsShuttingDown())
        return false;

    auto& diag = ddraw_hook_g_PresentationDiagnostics;
    switch (kind) {
        case ce::ddraw_present_policy::PresentKind::FlipChain:
            diag.flips.fetch_add(1, std::memory_order_relaxed);
            break;
        case ce::ddraw_present_policy::PresentKind::BlitPresent:
            diag.blitPresents.fetch_add(1, std::memory_order_relaxed);
            break;
        case ce::ddraw_present_policy::PresentKind::DirectScanout:
            (haveChangedRect ? diag.directScanoutBlits : diag.scanoutUnlocks).fetch_add(1, std::memory_order_relaxed);
            break;
        case ce::ddraw_present_policy::PresentKind::None:
            break;
    }

    if (kind == ce::ddraw_present_policy::PresentKind::FlipChain) {
        ddraw_hook_g_ScanoutWritesSinceFlip.store(0, std::memory_order_relaxed);
    } else if (kind == ce::ddraw_present_policy::PresentKind::DirectScanout) {
        const uint32_t writes = ddraw_hook_g_ScanoutWritesSinceFlip.fetch_add(1, std::memory_order_relaxed) + 1;
        if (!ce::ddraw_present_policy::ScanoutWriteIsPresentation(ScanoutSurfaceOwnsFlipChain(visibleSurface),
                                                                 writes)) {
            // Writing the scanned-out member of an advancing flip chain is not
            // its presentation. Touching it here creates the visible tear the
            // next Flip immediately replaces.
            diag.scanoutWritesLeftToFlip.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }

    const auto target = ce::ddraw_present_policy::SelectCompositeTarget(kind, presentSource != nullptr);
    if (target == ce::ddraw_present_policy::CompositeTarget::None) {
        if (kind != ce::ddraw_present_policy::PresentKind::None)
            diag.skippedNoPublishedImage.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    IDirectDrawSurface7* compositeTarget =
        target == ce::ddraw_present_policy::CompositeTarget::PresentSource ? presentSource : visibleSurface;
    if (!compositeTarget)
        return false;

    // Compositing re-enters DirectDraw through the same hooked surface methods
    // (Lock/Unlock, GetDC/ReleaseDC), so the recursion guard has to cover the
    // whole presentation, not just the capture inside it.
    ddraw_hook_g_CaptureRecurse++;
    if (ddraw_hook_g_CaptureRecurse > 1) {
        // CE's own composite locks and unlocks DirectDraw surfaces through the
        // same hooks, so it generates presentations of its own. They were
        // silently dropped and made the mix counters impossible to reconcile.
        diag.reentrantPresentations.fetch_add(1, std::memory_order_relaxed);
        ddraw_hook_g_CaptureRecurse--;
        return false;
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
                diag.skippedOutsideOverlay.fetch_add(1, std::memory_order_relaxed);
                ddraw_hook_g_CaptureRecurse--;
                return false;
            }
        }
    }

    auto doOverlay = [&]() {
        if (shouldDrawOverlay) {
            diag.composites.fetch_add(1, std::memory_order_relaxed);
            DrawDDrawOverlay(compositeTarget, kind, haveChangedRect, changedRect);
        } else {
            // Hiding the overlay while a partial-update screen is up (a loading
            // screen that only repaints a progress bar) leaves CE's rectangle
            // with nothing to repaint it, so the saved application backdrop has
            // to be put back explicitly.
            ddraw_hook_g_DDrawCapture.RestoreCompositeRegion(compositeTarget);
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
        if (!captureIncludeOverlay) {
            size_t repairCount = 0;
            const auto nativeState = QueryNativeLegacyD3DOverlay(compositeTarget, nullptr, 0, repairCount);
            if (nativeState != ce::ddraw_native_overlay::State::Absent) {
                // Native pixels have no saved CPU backdrop. During the unusual
                // transition from a D3D scene to partial 2D updates, defer this
                // one capture rather than violate overlay-excluded recording;
                // CFR repeats the previous clean frame until the game replaces
                // the old native pixels.
                ddraw_hook_g_DDrawCapture.droppedFrames.fetch_add(1, std::memory_order_relaxed);
                const uint32_t occurrence =
                    diag.captureNativeDeferrals.fetch_add(1, std::memory_order_relaxed) + 1;
                if (occurrence <= 4 || (occurrence & (occurrence - 1)) == 0) {
                    HookLogImportant("DDraw: Deferring overlay-excluded capture while native overlay pixels may "
                                     "remain (surface=%p state=%d occurrence=%u)",
                                     compositeTarget, static_cast<int>(nativeState), occurrence);
                }
                return;
            }
        }
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
    return true;
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

    // A presentation mix that changes shape - flips stopping while blits carry
    // the screen, or nothing being composited at all - is the signature of the
    // loading-screen and menu paths, and is not recoverable from the overlay.
    constexpr uint32_t kPresentationMixLogIntervalMs = 10000;
    const uint32_t nowTick = static_cast<uint32_t>(GetTickCount());
    auto& diag = ddraw_hook_g_PresentationDiagnostics;
    const uint32_t lastTick = diag.lastLogTick.load(std::memory_order_relaxed);
    if (lastTick == 0 || nowTick - lastTick >= kPresentationMixLogIntervalMs) {
        diag.lastLogTick.store(nowTick, std::memory_order_relaxed);
        LogDirectDrawPresentationMix("periodic");
    }
}

bool HandlePresentationSurface4(IDirectDrawSurface4* visibleSurface, IDirectDrawSurface4* presentSource,
                                ce::ddraw_present_policy::PresentKind kind, bool haveChangedRect,
                                const ce::ddraw_present_policy::Rect& changedRect) {
    IDirectDrawSurface7* visibleSurface7 = QuerySurface7(visibleSurface);
    if (!visibleSurface7) {
        static int primaryUpgradeFailLogCount = 0;
        if (primaryUpgradeFailLogCount < 4) {
            HookLog("DDraw: Failed to upgrade DirectDraw4 surface to DirectDraw7 for capture/overlay");
            primaryUpgradeFailLogCount++;
        }
        return false;
    }

    IDirectDrawSurface7* presentSource7 = QuerySurface7(presentSource);
    const bool accepted = ComposePresentation(visibleSurface7, presentSource7, kind, haveChangedRect, changedRect);

    if (presentSource7) {
        presentSource7->Release();
    }
    visibleSurface7->Release();
    return accepted;
}

bool HandlePresentationLegacySurface(IDirectDrawSurface* visibleSurface, IDirectDrawSurface* presentSource,
                                     ce::ddraw_present_policy::PresentKind kind, bool haveChangedRect,
                                     const ce::ddraw_present_policy::Rect& changedRect) {
    IDirectDrawSurface7* visibleSurface7 = QuerySurface7(visibleSurface);
    if (!visibleSurface7) {
        static std::atomic<int> s_upgradeFailureLogCount{0};
        if (s_upgradeFailureLogCount.fetch_add(1, std::memory_order_relaxed) < 4) {
            HookLogImportant("DDraw: Failed to upgrade legacy surface to Surface7 for capture/overlay");
        }
        return false;
    }
    IDirectDrawSurface7* presentSource7 = QuerySurface7(presentSource);
    const bool accepted = ComposePresentation(visibleSurface7, presentSource7, kind, haveChangedRect, changedRect);
    if (presentSource7)
        presentSource7->Release();
    visibleSurface7->Release();
    return accepted;
}
