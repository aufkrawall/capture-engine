#include "dx12_hook_internal.h"

// PostSL/Streamline startup activation, serviced from the ExecuteCommandLists detour.
//
// The startup-handoff Present can bypass the synthetic Present path, which defers PostSL
// activation until the Streamline startup transition window expires. If ProcessFrame stops
// running (a freeze), the deferred callback never fires, so the window expiry has to be
// detected from a path that keeps running: the ECL hook, which sits on the present thread.
//
// Split out of the detour itself because it is a self-contained service that references
// nothing in the submission it rides on, and the detour has a size ceiling to respect.
void DX12_ServiceECLPostSLStartupActivation() {
    static bool s_startupWindowWasActive = false;
    static bool s_callbackTriggeredWithCachedSwapchain = false;
    static std::atomic<ULONGLONG> s_lastVisibleOverlayStartupProgressTriggerMs{0};
    static std::atomic<int> s_visibleOverlayStartupProgressLogCount{0};
    static std::atomic<int> s_visibleOverlayStartupProgressCompleteLogCount{0};
    const bool activationPending =
        DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire);
    const bool callbackInstalled =
        DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_acquire) != nullptr;
    const bool postSLActiveButUnconfirmed = HookIsPostSLOverlayActiveButUnconfirmed();
    const bool postSLStartupActivationEntered = HookHasPostSLSyntheticStartupActivationEntered();
    const bool postSLConfirmedRendering = dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire);
    const bool overlayVisible = GetHookOverlayConfig().showOverlay;
    const bool windowActive = DXGIShared::IsStreamlineStartupTransitionWindowActive();
    const bool startupTransitionWindowJustExpired = s_startupWindowWasActive && !windowActive;
    const bool nativeFSRPresentPathActive = HookHasRuntimeOwnedNativeFGPresentPath();
    const bool nativeFSRActive = g_FGCompat.IsFSRFGApiActive();
    const bool shouldProbeStartupActivationSwapchain =

        ce::dx12_overlay_policy::ShouldProbePostSLStartupActivationSwapchainFromECL(
            activationPending, callbackInstalled, postSLConfirmedRendering, nativeFSRPresentPathActive,
            nativeFSRActive);
    const bool activationSwapchainAvailable =
        shouldProbeStartupActivationSwapchain && HasStartupActivationSwapchainCandidateForECLProbe();
    if (!shouldProbeStartupActivationSwapchain && activationPending && callbackInstalled) {
        static std::atomic<int> s_eclStartupProbeSuppressedLogCount{0};
        const int logCount = s_eclStartupProbeSuppressedLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 200) == 0) {
            HookLogImportant(
                "DX12: ECL startup activation swapchain probe suppressed "
                "(activationPending=%d callbackInstalled=%d confirmed=%d nativeFGPath=%d apiFSR=%d "
                "retained=%p last=%p log=%d)",
                activationPending ? 1 : 0, callbackInstalled ? 1 : 0, postSLConfirmedRendering ? 1 : 0,
                nativeFSRPresentPathActive ? 1 : 0, nativeFSRActive ? 1 : 0, dx12_hook_g_StreamlineStartupActivationSwapchain,
                dx12_hook_g_LastSwapChain, logCount + 1);
        }
    }
    const bool safePostFSRBootstrapPath = HookHasSafePostFSRBootstrapPath();
    const bool allowExpiryTriggeredStartupActivation =
        ce::dx12_overlay_policy::ShouldTriggerExpiryDrivenECLPostSLStartupActivation(
            startupTransitionWindowJustExpired, activationPending, callbackInstalled, dx12_hook_g_HadFSRFGPhase,
            safePostFSRBootstrapPath);
    const bool continueVisibleOverlayStartupProgress =
        ce::dx12_overlay_policy::ShouldContinueECLDrivenPostSLStartupProgress(
            overlayVisible, activationPending, postSLStartupActivationEntered, postSLConfirmedRendering,
            callbackInstalled, activationSwapchainAvailable, dx12_hook_g_HadFSRFGPhase, safePostFSRBootstrapPath);
    const ULONGLONG nowMs = GetTickCount64();
    const ULONGLONG lastVisibleOverlayStartupProgressTriggerMs =
        s_lastVisibleOverlayStartupProgressTriggerMs.load(std::memory_order_acquire);
    const bool visibleOverlayStartupProgressTick = continueVisibleOverlayStartupProgress && !windowActive &&
                                                   (lastVisibleOverlayStartupProgressTriggerMs == 0 ||
                                                    nowMs - lastVisibleOverlayStartupProgressTriggerMs >= 16);

    if (allowExpiryTriggeredStartupActivation || visibleOverlayStartupProgressTick) {
        // If the deferred ECL probe is pending and the startup window has expired,
        // try to probe now.  ProcessFrame may not be running (synthetic re-entrant
        // Present path), so the deferred probe check in ProcessFrame would never fire.
        DX12_ServiceDeferredECLProbe();

        if (activationSwapchainAvailable) {
            if (startupTransitionWindowJustExpired) {
                HookLogImportant(
                    "DX12: ECL hook detected startup transition window expiry with pending PostSL activation — "
                    "triggering retained-swapchain PostSL activation service "
                    "(startupWindowExpired=1 activationPending=1 activeButUnconfirmed=%d "
                    "startupActivationEntered=%d callbackInstalled=1)",
                    postSLActiveButUnconfirmed ? 1 : 0, postSLStartupActivationEntered ? 1 : 0);
            } else {
                const int logCount =
                    s_visibleOverlayStartupProgressLogCount.fetch_add(1, std::memory_order_relaxed);
                if (logCount < 10 || (logCount % 120) == 0) {
                    HookLogImportant(
                        "DX12: ECL hook continuing visible-overlay PostSL startup progress while render remains "
                        "unconfirmed (startupPending=%d activeButUnconfirmed=%d "
                        "startupActivationEntered=%d retainedSwapchain=1)",
                        activationPending ? 1 : 0, postSLActiveButUnconfirmed ? 1 : 0,
                        postSLStartupActivationEntered ? 1 : 0);
                }
            }

            s_callbackTriggeredWithCachedSwapchain = true;
            if (!startupTransitionWindowJustExpired) {
                s_lastVisibleOverlayStartupProgressTriggerMs.store(nowMs, std::memory_order_release);
            }
            const bool invoked = DX12_TryInvokePostSLStartupActivationCallback(
                startupTransitionWindowJustExpired ? "DX12::ECL startup-window expiry"
                                                   : "DX12::ECL visible startup progress",
                startupTransitionWindowJustExpired);
            s_callbackTriggeredWithCachedSwapchain = false;
            if (invoked) {
                if (startupTransitionWindowJustExpired) {
                    HookLogImportant("DX12: ECL hook retained-swapchain PostSL callback completed");
                } else {
                    const int logCount =
                        s_visibleOverlayStartupProgressCompleteLogCount.fetch_add(1, std::memory_order_relaxed);
                    if (logCount < 10 || (logCount % 120) == 0) {
                        HookLogImportant("DX12: ECL hook visible-overlay PostSL progress callback completed");
                    }
                }
            }
        } else {
            static int s_nullSwapchainSkipLog = 0;
            if (s_nullSwapchainSkipLog < 5) {
                HookLogImportant(
                    "DX12: ECL hook skipping PostSL callback — no retained/fresh activation swapchain "
                    "(allowExpiry=%d visibleTick=%d)",
                    allowExpiryTriggeredStartupActivation ? 1 : 0, visibleOverlayStartupProgressTick ? 1 : 0);
                ++s_nullSwapchainSkipLog;
            }
        }
    } else if (startupTransitionWindowJustExpired && activationPending && callbackInstalled &&
               !allowExpiryTriggeredStartupActivation) {
        HookLogImportant(
            "DX12: ECL hook leaving pending PostSL activation dormant after startup window expiry "
            "because post-FSR bootstrap path is still unsafe "
            "(activationPending=1 hadFSR=%d safeBootstrap=%d activationSwapchainAvailable=%d)",
            dx12_hook_g_HadFSRFGPhase ? 1 : 0, safePostFSRBootstrapPath ? 1 : 0, activationSwapchainAvailable ? 1 : 0);
    } else if (s_callbackTriggeredWithCachedSwapchain) {
        // Log if we're still processing after callback was triggered (callbacks from ProcessFrame)
        static int s_postEclCallbackLogCount = 0;
        if (s_postEclCallbackLogCount < 5) {
            HookLogImportant(
                "DX12: PostSL callback path after ECL hook trigger "
                "(windowActive=%d startupPending=%d callbackInstalled=%d)",
                windowActive ? 1 : 0, activationPending ? 1 : 0, callbackInstalled ? 1 : 0);
            s_postEclCallbackLogCount++;
        }
    }
    s_startupWindowWasActive = windowActive;
}
