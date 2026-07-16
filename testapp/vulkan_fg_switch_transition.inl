// Included by vulkan_fg_switch_test.cpp; make-before-break FG owner transition state machine.

namespace testapp::vkfg {
namespace {

bool ModeCanBeRequested(FgMode mode, const char** unavailableReason) {
    if (mode == FgMode::Dlss) {
        if (!g_App.sl.initialized || !g_App.sl.vulkanInfoSet) {
            *unavailableReason = "Streamline Vulkan integration unavailable";
            return false;
        }
        if (!g_App.sl.dlssFgSupported) {
            *unavailableReason = "DLSS Frame Generation unsupported on selected adapter";
            return false;
        }
    } else if (mode == FgMode::Fsr) {
        if (!g_App.vk.queuePlan.fidelityFxAvailable) {
            *unavailableReason = "FidelityFX distinct Vulkan queue topology unavailable";
            return false;
        }
    }
    return true;
}

bool PrepareTarget(FgMode target) {
    switch (target) {
        case FgMode::Dlss:
            return PrepareStreamlineMode();
        case FgMode::Fsr:
            return PrepareFidelityFxMode();
        case FgMode::Off:
        default:
            return true;
    }
}

void RollbackTargetPreparation() {
    if (g_App.transition.targetMode == FgMode::Fsr &&
        g_App.swapchain.owner != SwapchainOwner::FidelityFX) {
        DestroyFidelityFxContexts(g_App.config.fsrReloadRuntimeOnSwitch,
                                  "rollback prepared FSR transition");
    }
    if (g_App.transition.targetMode == FgMode::Dlss &&
        g_App.swapchain.owner == SwapchainOwner::FidelityFX) {
        RetireStreamlinePresentation(SwapchainOwner::FidelityFX,
                                     "rollback prepared DLSS transition");
    }
}

void MaybeStartQueuedRequest() {
    if (g_App.transition.stage == TransitionStage::Idle &&
        g_App.requestedMode != g_App.transition.currentMode) {
        const FgMode queued = g_App.requestedMode;
        RequestMode(queued, "queued request after transition");
    }
}

}  // namespace

bool SetModeFeatureState(FgMode mode, bool enabled, const char* reason) {
    bool result = true;
    switch (mode) {
        case FgMode::Dlss:
            result = SetDlssFrameGeneration(enabled, reason);
            break;
        case FgMode::Fsr:
            result = SetFsrFrameGeneration(enabled, reason, true);
            break;
        case FgMode::Off:
        default:
            result = true;
            break;
    }
    testapp::Log(
        "[FG-DIAG] Feature state mode=%s requested=%d configured(dlss=%d,fsr=%d) "
        "reflex=%d suspended=%d reason=%s result=%d\n",
        ModeName(mode), enabled ? 1 : 0, g_App.sl.dlssFgConfigured ? 1 : 0,
        g_App.ffx.frameGenerationConfigured ? 1 : 0, g_App.sl.reflexActive ? 1 : 0,
        g_App.transition.suspended ? 1 : 0, reason ? reason : "unknown", result ? 1 : 0);
    testapp::LogFlush();
    return result;
}

bool RequestMode(FgMode mode, const char* reason) {
    g_App.requestedMode = mode;
    if (g_App.transition.stage == TransitionStage::DeviceLost) {
        return false;
    }
    const char* unavailableReason = nullptr;
    if (!ModeCanBeRequested(mode, &unavailableReason)) {
        testapp::Log(
            "[FG-DIAG] Mode request rejected requested=%s current=%s reason=%s "
            "support(dlssSR=%d,dlssFG=%d,reflex=%d,fsrSR=%d,fsrFGQueues=%d)\n",
            ModeName(mode), ModeName(g_App.transition.currentMode),
            unavailableReason ? unavailableReason : "unknown", g_App.sl.dlssSrSupported ? 1 : 0,
            g_App.sl.dlssFgSupported ? 1 : 0, g_App.sl.reflexSupported ? 1 : 0,
            g_App.ffx.upscaleSupported ? 1 : 0,
            g_App.vk.queuePlan.fidelityFxAvailable ? 1 : 0);
        testapp::LogFlush();
        return false;
    }

    if (g_App.transition.stage != TransitionStage::Idle) {
        if (mode == g_App.transition.currentMode &&
            CancelModeTransitionBeforeReplacement(&g_App.transition)) {
            const bool restored = mode == FgMode::Off ||
                SetModeFeatureState(mode, true, "cancel transition and restore old owner");
            testapp::Log(
                "[FG-TRANSITION] cancellation reason=%s restored=%d current=%s owner=%s\n",
                reason ? reason : "unknown", restored ? 1 : 0, ModeName(mode),
                OwnerName(g_App.swapchain.owner));
            LogTransition("cancel-before-replacement");
            return restored;
        }
        testapp::Log(
            "[FG-TRANSITION] queued request=%s reason=%s activeEpoch=%llu stage=%s\n",
            ModeName(mode), reason ? reason : "unknown",
            static_cast<unsigned long long>(g_App.transition.epoch),
            TransitionStageName(g_App.transition.stage));
        testapp::LogFlush();
        return true;
    }

    if (mode == g_App.transition.currentMode) {
        if (mode == FgMode::Off) {
            return true;
        }
        const bool suspend = !g_App.transition.suspended;
        if (!SetModeFeatureState(mode, !suspend, suspend ? "suspend repeated-key mode"
                                                       : "resume repeated-key mode")) {
            return false;
        }
        g_App.transition.suspended = suspend;
        testapp::Log(
            "[FG-TRANSITION] %s mode=%s owner=%s proxyRetained=1 contextsRetained=1 "
            "reflex=%d frameID=%llu reason=%s\n",
            suspend ? "suspend" : "resume", ModeName(mode), OwnerName(g_App.swapchain.owner),
            g_App.sl.reflexActive ? 1 : 0, static_cast<unsigned long long>(g_App.frameId),
            reason ? reason : "unknown");
        LogTransition(suspend ? "suspended" : "resumed");
        return true;
    }

    if (!BeginModeTransition(&g_App.transition, mode)) {
        return false;
    }
    bool disabled = true;
    if (g_App.transition.currentMode != FgMode::Off) {
        disabled = SetModeFeatureState(g_App.transition.currentMode, false,
                                       "begin owner transition");
    }
    if (!disabled) {
        g_App.transition.stage = TransitionStage::Idle;
        g_App.transition.targetMode = g_App.transition.currentMode;
        g_App.transition.targetOwner = g_App.transition.owner;
        ++g_App.transitionFailures;
        LogTransition("old-feature-disable-failed");
        return false;
    }
    MarkOldFgDisabled(&g_App.transition);
    testapp::Log(
        "[FG-TRANSITION] request reason=%s epoch=%llu from=%s/%s to=%s/%s; waiting "
        "for one FG-off present on old surface\n",
        reason ? reason : "unknown", static_cast<unsigned long long>(g_App.transition.epoch),
        ModeName(g_App.transition.currentMode), OwnerName(g_App.transition.oldOwner), ModeName(mode),
        OwnerName(g_App.transition.targetOwner));
    LogTransition("requested");
    return true;
}

void DriveTransitionBeforeFrame() {
    if (g_App.transition.stage == TransitionStage::DeviceLost) {
        return;
    }
    if (g_App.transition.stage == TransitionStage::PreparingReplacement) {
        const bool prepared = PrepareTarget(g_App.transition.targetMode);
        MarkReplacementPrepared(&g_App.transition, prepared);
        LogTransition(prepared ? "replacement-prepared" : "replacement-prepare-failed");
        if (!prepared) {
            ++g_App.transitionFailures;
        }
    }
    if (g_App.transition.stage == TransitionStage::ReplacingSwapchain) {
        bool replaced = DrainSwapchainBoundWork("transactional owner replacement");
        if (replaced) {
            replaced = CreateOrReplaceSwapchain(g_App.transition.targetOwner,
                                                "transactional owner replacement");
        }
        MarkReplacementCreated(&g_App.transition, replaced);
        LogTransition(replaced ? "replacement-committed-fg-off"
                               : "replacement-create-failed");
        if (!replaced) {
            ++g_App.transitionFailures;
        }
    }
    if (g_App.transition.stage == TransitionStage::Rollback) {
        const FgMode failedTarget = g_App.transition.targetMode;
        RollbackTargetPreparation();
        const FgMode oldMode = g_App.transition.currentMode;
        const bool rolledBack = RollbackPreparedTransition(&g_App.transition);
        bool restored = rolledBack;
        if (rolledBack && oldMode != FgMode::Off) {
            restored = SetModeFeatureState(oldMode, true, "rollback restore old FG owner");
        }
        testapp::Log("[FG-TRANSITION] rollback completed=%d restoredOldFeature=%d owner=%s\n",
                     rolledBack ? 1 : 0, restored ? 1 : 0, OwnerName(g_App.swapchain.owner));
        g_App.requestedMode = ResolveRequestedModeAfterTransitionFailure(
            g_App.requestedMode, failedTarget, g_App.transition.currentMode);
        LogTransition("rollback-complete");
        MaybeStartQueuedRequest();
    }
    if (g_App.transition.stage == TransitionStage::Activating) {
        const FgMode target = g_App.transition.targetMode;
        const bool activated = target == FgMode::Off ||
            SetModeFeatureState(target, true, "activate after replacement FG-off present");
        MarkTargetActivated(&g_App.transition, activated);
        if (!activated) {
            ++g_App.transitionFailures;
            g_App.requestedMode = ResolveRequestedModeAfterTransitionFailure(
                g_App.requestedMode, target, g_App.transition.currentMode);
        }
        LogTransition(activated ? "target-activated" : "target-activation-failed-kept-fg-off");
        MaybeStartQueuedRequest();
    }
}

void OnFramePresentedSuccessfully() {
    if (g_App.transition.stage == TransitionStage::OldPassthroughPending) {
        if (MarkOldPassthroughPresented(&g_App.transition, true)) {
            LogTransition("old-surface-passthrough-presented");
        }
    } else if (g_App.transition.stage == TransitionStage::ReplacementPresentPending) {
        if (MarkReplacementPresented(&g_App.transition, true)) {
            LogTransition("replacement-surface-fg-off-presented");
        }
    }
}

}  // namespace testapp::vkfg
