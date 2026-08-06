#include "dx12_fg_switch_test_internal.h"


bool SwitchMode(FGMode target, const char* reason, UINT frameIndex) {
    if (target == dx12_fg_switch_test_g_CurrentMode && !dx12_fg_switch_test_g_ModeSwitchPending) {
        return true;
    }

    testapp::Log("[FG-DIAG] Switching FG mode: %s -> %s (%s frameID=%llu frameIndex=%u)\n", ModeName(dx12_fg_switch_test_g_CurrentMode),
                 ModeName(target), reason ? reason : "unknown", static_cast<unsigned long long>(dx12_fg_switch_test_g_FrameIdCounter),
                 frameIndex);

    if (dx12_fg_switch_test_g_FsrExitTransitionStage == testapp::fg::FsrExitTransitionStage::ReplacementPresentPending) {
        if (g_SwapChain && dx12_fg_switch_test_g_SwapChainUsesStreamline) {
            const bool defersDlssActivation = testapp::fg::ShouldDeferDlssActivationUntilReplacementPresent(
                target == FGMode::DLSS, dx12_fg_switch_test_g_FsrExitTransitionStage);
            testapp::Log(
                "[FG-DIAG] DLSS replacement passthrough Present still pending; deferring mode switch "
                "(target=%s deferDlssActivation=%d frameID=%llu swapChain=%p)\n",
                ModeName(target), defersDlssActivation ? 1 : 0, static_cast<unsigned long long>(dx12_fg_switch_test_g_FrameIdCounter),
                g_SwapChain.Get());
            testapp::LogFlush();
            return true;
        }
        testapp::Log(
            "[FG-DIAG] WARN cannot retry DLSS replacement passthrough Present because its proxy "
            "topology is unavailable; restarting the requested transition (target=%s swapChain=%p "
            "streamline=%d)\n",
            ModeName(target), g_SwapChain.Get(), dx12_fg_switch_test_g_SwapChainUsesStreamline ? 1 : 0);
        dx12_fg_switch_test_g_FsrExitTransitionStage = testapp::fg::FsrExitTransitionStage::None;
        dx12_fg_switch_test_g_FsrRuntimeRetirementPendingForDlss = false;
    } else if (dx12_fg_switch_test_g_FsrExitTransitionStage == testapp::fg::FsrExitTransitionStage::ReplacementPresented) {
        testapp::Log(
            "[FG-DIAG] DLSS replacement passthrough Present completed; %s "
            "(target=%s frameID=%llu swapChain=%p)\n",
            target == FGMode::DLSS ? "activation may proceed" : "following the newer mode request", ModeName(target),
            static_cast<unsigned long long>(dx12_fg_switch_test_g_FrameIdCounter), g_SwapChain.Get());
        dx12_fg_switch_test_g_FsrExitTransitionStage = testapp::fg::FsrExitTransitionStage::None;
        if (target == FGMode::FSR) {
            dx12_fg_switch_test_g_FsrRuntimeRetirementPendingForDlss = false;
        }
    }
    WaitForGpu();

    bool ok = true;
    if (target == FGMode::FSR && dx12_fg_switch_test_g_FsrExitTransitionStage != testapp::fg::FsrExitTransitionStage::None) {
        testapp::Log(
            "[FG-DIAG] Cancelling staged FSR exit because the pending target returned to FSR "
            "(stage=%d frameID=%llu)\n",
            static_cast<int>(dx12_fg_switch_test_g_FsrExitTransitionStage), static_cast<unsigned long long>(dx12_fg_switch_test_g_FrameIdCounter));
        dx12_fg_switch_test_g_FsrExitTransitionStage = testapp::fg::FsrExitTransitionStage::None;
    }

    const testapp::fg::FsrExitTransitionAction fsrExitAction = testapp::fg::ResolveFsrExitTransitionAction(
        dx12_fg_switch_test_g_CurrentMode == FGMode::FSR, target == FGMode::FSR, dx12_fg_switch_test_g_FsrEnabled, dx12_fg_switch_test_g_FsrExitTransitionStage);
    bool fsrExitHandled = false;
    if (fsrExitAction == testapp::fg::FsrExitTransitionAction::PresentPassthrough) {
        fsrExitHandled = true;
        if (g_SwapChain && dx12_fg_switch_test_g_SwapChainOwner == SwapChainOwner::FSR && dx12_fg_switch_test_g_FfxSwapChainCtx) {
            testapp::Log(
                "[FG-DIAG] FSR exit passthrough Present still pending; deferring teardown "
                "(target=%s frameID=%llu)\n",
                ModeName(target), static_cast<unsigned long long>(dx12_fg_switch_test_g_FrameIdCounter));
            testapp::LogFlush();
            return true;
        }
        testapp::Log(
            "[FG-DIAG] WARN cannot retry FSR exit passthrough Present because its proxy topology "
            "is unavailable; continuing teardown (target=%s swapChain=%p owner=%s ctx=%p)\n",
            ModeName(target), g_SwapChain.Get(), SwapChainOwnerName(dx12_fg_switch_test_g_SwapChainOwner), (void*)dx12_fg_switch_test_g_FfxSwapChainCtx);
        dx12_fg_switch_test_g_FsrExitTransitionStage = testapp::fg::FsrExitTransitionStage::PassthroughPresented;
    } else if (fsrExitAction == testapp::fg::FsrExitTransitionAction::DisableAndPresentPassthrough) {
        fsrExitHandled = true;
        const bool disabled = ConfigureFSR(false, nullptr, "leave FSR mode", true);
        dx12_fg_switch_test_g_FsrEnabled = false;
        ok = ok && disabled;
        if (disabled && g_SwapChain && dx12_fg_switch_test_g_SwapChainOwner == SwapChainOwner::FSR && dx12_fg_switch_test_g_FfxSwapChainCtx) {
            // Keep the disabled proxy alive for one ordinary application Present. For a DLSS
            // target it remains alive after that Present while Streamline is initialized, so the
            // cold runtime load cannot remove the only visible DWM presentation surface.
            dx12_fg_switch_test_g_FsrSuspended = true;
            dx12_fg_switch_test_g_FsrExitTransitionStage = testapp::fg::FsrExitTransitionStage::PresentPending;
            testapp::Log(
                "[FG-DIAG] FSR disabled; staging one passthrough Present before teardown "
                "(target=%s frameID=%llu swapChain=%p ctx=%p)\n",
                ModeName(target), static_cast<unsigned long long>(dx12_fg_switch_test_g_FrameIdCounter), g_SwapChain.Get(),
                (void*)dx12_fg_switch_test_g_FfxSwapChainCtx);
            UpdateWindowTitle();
            testapp::LogFlush();
            return true;
        }
        testapp::Log(
            "[FG-DIAG] WARN FSR exit cannot stage a passthrough Present; using immediate teardown "
            "(disabled=%d target=%s swapChain=%p owner=%s ctx=%p)\n",
            disabled ? 1 : 0, ModeName(target), g_SwapChain.Get(), SwapChainOwnerName(dx12_fg_switch_test_g_SwapChainOwner),
            (void*)dx12_fg_switch_test_g_FfxSwapChainCtx);
        ResetFSRSuspensionStressState("leave FSR mode without passthrough Present");
        ok = ok && WaitForFSRSwapChainPresents("leave FSR mode");
        WaitForGpu();
    }
    if (dx12_fg_switch_test_g_FsrExitTransitionStage == testapp::fg::FsrExitTransitionStage::PassthroughPresented) {
        fsrExitHandled = true;
        const bool dlssReady = dx12_fg_switch_test_g_SlInitialized && dx12_fg_switch_test_g_SlDeviceSet && dx12_fg_switch_test_g_DlssInitialized;
        if (testapp::fg::ShouldPrepareDlssBeforeFsrPresentationBreak(true, target == FGMode::DLSS, dlssReady,
                                                                     dx12_fg_switch_test_g_FsrExitTransitionStage)) {
            testapp::Log(
                "[FG-DIAG] FSR exit passthrough Present completed; preparing Streamline while the "
                "disabled FSR presentation surface remains alive (frameID=%llu swapChain=%p ctx=%p)\n",
                static_cast<unsigned long long>(dx12_fg_switch_test_g_FrameIdCounter), g_SwapChain.Get(), (void*)dx12_fg_switch_test_g_FfxSwapChainCtx);
            testapp::LogFlush();
            if (!EnsureStreamlineReadyForDLSS("prepare DLSS before FSR presentation break")) {
                testapp::Log(
                    "[FG-DIAG] Streamline preparation failed before FSR presentation break; "
                    "rolling back to active FSR without destroying its proxy\n");
                dx12_fg_switch_test_g_FsrExitTransitionStage = testapp::fg::FsrExitTransitionStage::None;
                if (dx12_fg_switch_test_g_SlInitialized || dx12_fg_switch_test_g_SlModule) {
                    ShutdownStreamlineSerialized("rollback failed DLSS preparation");
                }
                ResetFSRSuspensionStressState("rollback failed DLSS preparation");
                const UINT activeFrameIndex = g_FrameIndex < g_SwapChainBufferCount ? g_FrameIndex : frameIndex;
                const bool resumed = ConfigureFSR(
                    true, activeFrameIndex < g_SwapChainBufferCount ? g_RenderTargets[activeFrameIndex].Get() : nullptr,
                    "rollback failed DLSS preparation", true);
                dx12_fg_switch_test_g_FsrEnabled = resumed;
                if (resumed) {
                    RegisterFSRUiResource();
                }
                dx12_fg_switch_test_g_ModeSwitchPending = false;
                UpdateWindowTitle();
                testapp::Log("[FG-DIAG] FSR rollback after failed DLSS preparation resumed=%d current=%s\n",
                             resumed ? 1 : 0, ModeName(dx12_fg_switch_test_g_CurrentMode));
                testapp::LogFlush();
                return false;
            }
        }
        const bool preparedDlss = dx12_fg_switch_test_g_SlInitialized && dx12_fg_switch_test_g_SlDeviceSet && dx12_fg_switch_test_g_DlssInitialized;
        if (!testapp::fg::CanCommitFsrPresentationBreak(true, target == FGMode::DLSS, preparedDlss,
                                                        dx12_fg_switch_test_g_FsrExitTransitionStage)) {
            testapp::Log(
                "[FG-DIAG] WARN refusing FSR presentation break before its DLSS replacement is ready "
                "(target=%s slInit=%d slDevice=%d dlssInit=%d)\n",
                ModeName(target), dx12_fg_switch_test_g_SlInitialized ? 1 : 0, dx12_fg_switch_test_g_SlDeviceSet ? 1 : 0, dx12_fg_switch_test_g_DlssInitialized ? 1 : 0);
            testapp::LogFlush();
            return true;
        }
        testapp::Log(
            "[FG-DIAG] FSR exit replacement ready; committing presentation break "
            "(target=%s frameID=%llu slInit=%d slDevice=%d dlssInit=%d)\n",
            ModeName(target), static_cast<unsigned long long>(dx12_fg_switch_test_g_FrameIdCounter), dx12_fg_switch_test_g_SlInitialized ? 1 : 0,
            dx12_fg_switch_test_g_SlDeviceSet ? 1 : 0, dx12_fg_switch_test_g_DlssInitialized ? 1 : 0);
        ResetFSRSuspensionStressState("leave FSR mode after passthrough Present");
        ok = ok && WaitForFSRSwapChainPresents("leave FSR mode after passthrough Present");
        WaitForGpu();
    } else if (!fsrExitHandled && dx12_fg_switch_test_g_FsrEnabled) {
        const bool disabled = ConfigureFSR(false, nullptr, "leave FSR mode", true);
        dx12_fg_switch_test_g_FsrEnabled = false;
        ResetFSRSuspensionStressState("leave FSR mode");
        ok = ok && disabled;
        ok = ok && WaitForFSRSwapChainPresents("leave FSR mode");
        WaitForGpu();
    }
    if (dx12_fg_switch_test_g_DlssEnabled) {
        const bool disabled = SetDLSSFGMode(false);
        dx12_fg_switch_test_g_DlssEnabled = false;
        dx12_fg_switch_test_g_DlssSuspended = false;
        ok = ok && disabled;
        WaitForGpu();
    } else if (dx12_fg_switch_test_g_CurrentMode == FGMode::DLSS && dx12_fg_switch_test_g_DlssSuspended) {
        testapp::Log("[FG-DIAG] Leaving suspended DLSS mode without another disable call (%s)\n",
                     reason ? reason : "unknown");
        dx12_fg_switch_test_g_DlssSuspended = false;
    }

    if (target == FGMode::FSR) {
        if (!LoadFSRRuntimeSerialized("enter FSR mode") || !dx12_fg_switch_test_g_FfxCreateContext) {
            testapp::Log("[FG-DIAG] Cannot switch to FSR FG: FSR runtime is not loaded\n");
            ok = false;
        }
        if (ok && (dx12_fg_switch_test_g_SwapChainOwner != SwapChainOwner::FSR || !dx12_fg_switch_test_g_FfxSwapChainCtx)) {
            DestroyFSRContexts();
            dx12_fg_switch_test_g_FsrInitialized = false;
            ok = ReinitializeDX12ForFSR("enter FSR mode") && ok;
        }
        if (ok && !dx12_fg_switch_test_g_FsrInitialized) {
            dx12_fg_switch_test_g_FsrInitialized = TryInitFSR();
            ok = ok && dx12_fg_switch_test_g_FsrInitialized;
        }
        if (ok && UpscalingActive() && !dx12_fg_switch_test_g_FfxUpscaleCtx) {
            // Non-fatal: without the upscaler context the upscale stage falls back to TAA/TAAU.
            TryInitFSRUpscaleContext();
        }
        ResetFSRPresentCallbackStressState("enter FSR mode");
        UINT activeFrameIndex = g_FrameIndex < g_SwapChainBufferCount ? g_FrameIndex : frameIndex;
        if (ok &&
            ConfigureFSR(true,
                         activeFrameIndex < g_SwapChainBufferCount ? g_RenderTargets[activeFrameIndex].Get() : nullptr,
                         "enter FSR mode", true)) {
            dx12_fg_switch_test_g_FsrEnabled = true;
            ResetFSRSuspensionStressState("enter FSR mode");
            RegisterFSRUiResource();
        } else if (ok) {
            testapp::Log("[FG-DIAG] FSR FG enable failed during switch\n");
            ok = false;
        }
    } else if (target == FGMode::DLSS) {
        const bool enteringFromFsr = dx12_fg_switch_test_g_SwapChainOwner == SwapChainOwner::FSR || dx12_fg_switch_test_g_FfxCtx || dx12_fg_switch_test_g_FfxSwapChainCtx;
        bool recreatedDlssSurface = false;
        if (ok && !EnsureStreamlineReadyForDLSS(enteringFromFsr ? "enter DLSS mode after FSR"
                                                                : "enter DLSS mode from native")) {
            ok = false;
        }
        if (ok && enteringFromFsr) {
            DestroyFSRContexts();
            dx12_fg_switch_test_g_FsrInitialized = false;
            // Streamline, its device binding, and its feature entry points were prepared while the
            // disabled FSR proxy was still the visible surface. Reuse the existing device/queue and
            // replace only swapchain-bound resources, matching the already smooth FSR->OFF handoff.
            recreatedDlssSurface = RecreateSwapChain(false, "enter DLSS mode after prepared FSR exit");
            ok = recreatedDlssSurface && ok;
            dx12_fg_switch_test_g_FsrRuntimeRetirementPendingForDlss = recreatedDlssSurface;
        }
        if (ok && !dx12_fg_switch_test_g_SwapChainUsesStreamline) {
            // Native->DLSS (including FSR->OFF->DLSS) also prepares Streamline before releasing the
            // visible native chain, then performs the shortest possible swapchain-only handoff.
            recreatedDlssSurface = RecreateSwapChain(false, "enter DLSS mode from prepared native surface");
            ok = recreatedDlssSurface && ok;
        }
        if (ok && recreatedDlssSurface) {
            // A newly created Streamline proxy has not displayed anything yet. Enabling DLSS-G now
            // makes its first Present also perform the feature's large lazy resource/model setup,
            // leaving DWM without a replacement image in the meantime. Present one covered FG-off
            // frame through this same proxy first; the pending request activates DLSS next frame.
            dx12_fg_switch_test_g_CurrentMode = FGMode::Off;
            dx12_fg_switch_test_g_FsrExitTransitionStage = testapp::fg::FsrExitTransitionStage::ReplacementPresentPending;
            dx12_fg_switch_test_g_Taa.Reset();
            testapp::Log(
                "[FG-DIAG] DLSS replacement surface ready; staging one covered FG-off passthrough "
                "Present before activation (frameID=%llu swapChain=%p fromFsr=%d)\n",
                static_cast<unsigned long long>(dx12_fg_switch_test_g_FrameIdCounter), g_SwapChain.Get(), enteringFromFsr ? 1 : 0);
            UpdateWindowTitle();
            testapp::LogFlush();
            return true;
        }
        if (!dx12_fg_switch_test_g_DlssInitialized) {
            testapp::Log("[FG-DIAG] Cannot switch to DLSS FG: Streamline DLSS-G was not initialized\n");
            ok = false;
        } else if (ok && SetDLSSFGMode(true)) {
            dx12_fg_switch_test_g_DlssEnabled = true;
            dx12_fg_switch_test_g_DlssSuspended = false;
            // Anchor the one-shot DLSS stress interval to the FG enable. Without this the stress
            // timer (armed at startup) was already expired on entering DLSS mode and suspended FG in
            // the SAME Render iteration / frame token as the enable, before a single present consumed
            // it -- Streamline flags that as "Repeated slDLSSGSetOptions() call ... race condition
            // with Present()" and it leaves the pacer half-initialized. The stress must exercise the
            // realistic sequence: FG actually generates frames for the interval, THEN suspends.
            dx12_fg_switch_test_g_LastDlssSuspendResumeToggleTime = std::chrono::high_resolution_clock::now();
            if (UpscalingActive()) {
                SetDLSSSROptions(true);
            }
            PollDLSSFGState();
        } else if (ok) {
            testapp::Log("[FG-DIAG] DLSS FG enable failed during switch\n");
            ok = false;
        }
    } else {
        if (ok && dx12_fg_switch_test_g_SwapChainOwner == SwapChainOwner::FSR && dx12_fg_switch_test_g_FfxCtx && dx12_fg_switch_test_g_FfxSwapChainCtx) {
            testapp::Log(
                "[FG-DIAG] OFF mode destroys the FSR swapchain/context and recreates a native swapchain "
                "(oldSwapChain=%p swapchainCtx=%p fgCtx=%p)\n",
                g_SwapChain.Get(), (void*)dx12_fg_switch_test_g_FfxSwapChainCtx, (void*)dx12_fg_switch_test_g_FfxCtx);
            DestroyFSRContexts();
            dx12_fg_switch_test_g_FsrInitialized = false;
            ok = RecreateSwapChain(false, "enter OFF mode after FSR") && ok;
            MaybeUnloadFSRRuntimeAfterSwitch("enter OFF mode after FSR");
            StartAsyncFSRRuntimePreload("after entering OFF mode from FSR");
        } else if (ok && (dx12_fg_switch_test_g_FfxCtx || dx12_fg_switch_test_g_FfxSwapChainCtx)) {
            DestroyFSRContexts();
            dx12_fg_switch_test_g_FsrInitialized = false;
            ok = RecreateSwapChain(false, "enter OFF mode") && ok;
            MaybeUnloadFSRRuntimeAfterSwitch("enter OFF mode");
            StartAsyncFSRRuntimePreload("after entering OFF mode");
        } else if (ok && (dx12_fg_switch_test_g_SwapChainUsesStreamline || dx12_fg_switch_test_g_SlInitialized || dx12_fg_switch_test_g_SlModule)) {
            testapp::Log(
                "[FG-DIAG] OFF mode tears down the Streamline proxy swapchain and Streamline itself, then "
                "recreates a native swapchain (swapChain=%p streamline=%d) so Reflex can genuinely turn off\n",
                g_SwapChain.Get(), dx12_fg_switch_test_g_SwapChainUsesStreamline ? 1 : 0);
            ok = ReinitializeDX12ForNativeOff("enter OFF mode after DLSS") && ok;
        }
        if (ok && dx12_fg_switch_test_g_FsrRuntimeRetirementPendingForDlss) {
            MaybeUnloadFSRRuntimeAfterSwitch("after cancelled DLSS replacement entered OFF");
            StartAsyncFSRRuntimePreload("after cancelled DLSS replacement entered OFF");
            dx12_fg_switch_test_g_FsrRuntimeRetirementPendingForDlss = false;
        }
    }

    if (!ok) {
        if (dx12_fg_switch_test_g_FsrRuntimeRetirementPendingForDlss) {
            if (g_SwapChain) {
                MaybeUnloadFSRRuntimeAfterSwitch("after failed DLSS activation");
                StartAsyncFSRRuntimePreload("after failed DLSS activation");
            }
            dx12_fg_switch_test_g_FsrRuntimeRetirementPendingForDlss = false;
        }
        if (target != FGMode::Off) {
            target = dx12_fg_switch_test_g_FsrEnabled ? FGMode::FSR : ((dx12_fg_switch_test_g_DlssEnabled || dx12_fg_switch_test_g_DlssSuspended) ? FGMode::DLSS : FGMode::Off);
        }
        if (!g_SwapChain) {
            testapp::Log("[FG-DIAG] Fatal switch failure: no swapchain after %s request; stopping main loop\n",
                         ModeName(target));
            dx12_fg_switch_test_g_Running = false;
        }
    }
    dx12_fg_switch_test_g_CurrentMode = target;
    dx12_fg_switch_test_g_ModeSwitchPending = false;
    dx12_fg_switch_test_g_LastModeSwitchFrameId = dx12_fg_switch_test_g_FrameIdCounter;
    // Temporal history from the previous mode is meaningless after a switch (different upscaler /
    // recreated resources); restart accumulation cleanly.
    dx12_fg_switch_test_g_Taa.Reset();
    if (ok && target == FGMode::FSR) {
        StartAsyncStreamlinePreload("after entering FSR mode");
    }
    testapp::Log("[FG-DIAG] Mode now %s (ok=%d fsr=%d fsrSuspended=%d dlss=%d dlssSuspended=%d)\n",
                 ModeName(dx12_fg_switch_test_g_CurrentMode), ok ? 1 : 0, dx12_fg_switch_test_g_FsrEnabled ? 1 : 0, dx12_fg_switch_test_g_FsrSuspended ? 1 : 0,
                 dx12_fg_switch_test_g_DlssEnabled ? 1 : 0, dx12_fg_switch_test_g_DlssSuspended ? 1 : 0);
    UpdateWindowTitle();
    testapp::LogFlush();
    return ok;
}
