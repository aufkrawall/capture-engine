#pragma once

#include <windows.h>

#include <cstdint>

#include <dxgi1_6.h>

#include "../dxgi_presentation_color.h"
#include "../fg_runtime_state.h"

struct ID3D12CommandQueue;
struct ID3D12Fence;

#include "fg_metrics_and_transitions.h"

// Stale runtime-owned Streamline state, retained startup swapchains, and authoritative FSR.

namespace ce::dx12_overlay_policy {

inline bool ShouldTrackStaleRuntimeOwnedStreamlineNoFGRealFrameRun(bool streamlineFGRunning, bool runtimeOwnsSwapchain,
                                                                   fg_runtime::RuntimeMode runtimeMode,
                                                                   bool hasOriginalGameQueue,
                                                                   bool commandQueueUsesOriginalGameQueue,
                                                                   bool isInterpolatedFrame) {
    // A late Streamline-owned startup handoff can create a runtime-owned
    // swapchain/queue that never becomes the live non-FG Present path. If top-
    // level real-frame rendering keeps running on the original game queue while
    // Streamline never re-activates, the latched runtime-owned ownership and
    // captured runtime queue are stale and poison later startup/non-FG routing.
    return !streamlineFGRunning && runtimeOwnsSwapchain && runtimeMode == fg_runtime::RuntimeMode::kStreamlineNoFG &&
           hasOriginalGameQueue && commandQueueUsesOriginalGameQueue && !isInterpolatedFrame;
}

inline bool ShouldClearStaleRuntimeOwnedStreamlineNoFGAfterRealFrameRun(int realFrameRunLength) {
    // Require a sustained run so short queue/Present ownership wobble during a
    // genuine startup handoff does not collapse runtime-owned state too early.
    return realFrameRunLength >= 120;
}

inline bool ShouldReleaseRetainedStartupActivationSwapchainAfterStaleNoFGCleanup(bool retainedSwapchainAvailable,
                                                                                 bool staleNoFGCleanupActive) {
    return retainedSwapchainAvailable && staleNoFGCleanupActive;
}

inline bool ShouldReleaseRetainedStreamlineStartupActivationSwapchainForAuthoritativeFFXCreate(
    bool authoritativeFFXRuntimeCreator, bool retainedSwapchainAvailable) {
    // A retained Streamline startup swapchain is an owned CE AddRef used only to
    // wake PostSL during DLSS startup. Once AMD FFX/FSR is authoritatively
    // creating a swapchain for the same window, that retained reference becomes
    // stale and can prevent DXGI from allowing the FSR runtime to take ownership
    // of the HWND.
    return authoritativeFFXRuntimeCreator && retainedSwapchainAvailable;
}

inline bool ShouldShutdownDescFreeBackendViaOverlayAdapter(bool descFreeBackendPresent, bool adapterInitialized,
                                                           bool adapterBackendMatchesDescFree) {
    return descFreeBackendPresent && adapterInitialized && adapterBackendMatchesDescFree;
}

inline bool ShouldPreserveAuthoritativeFSRDuringTransitionCooldown(bool authoritativeFSRActive,
                                                                   bool runtimeTargetIsNone, int fgTransitionCooldown) {
    // Talos can briefly suspend native FSR FG during startup/menu transitions
    // while the runtime-owned swapchain and queue topology are still settling.
    // Treating that transient None edge as a real teardown immediately clears
    // authoritative FSR and collapses the overlay path before the runtime turns
    // FSR back on a few frames later.
    return authoritativeFSRActive && runtimeTargetIsNone && fgTransitionCooldown > 0;
}

inline bool ShouldSuppressHeuristicFSRActivationDuringAuthoritativeStreamlineStartupHandoff(
    bool runtimeOwnsSwapchain, bool streamlineStartupHandoffPending, fg_runtime::RuntimeMode runtimeMode) {
    // A fresh authoritative Streamline startup handoff can move presentation onto
    // a runtime-owned queue while Streamline still reports Off/NoFG. Treating that
    // queue change as heuristic FSR FG poisons the later DLSS comeback before any
    // real FSR proof exists.
    const bool runtimeStillInactive =
        runtimeMode == fg_runtime::RuntimeMode::kStreamlineNoFG || runtimeMode == fg_runtime::RuntimeMode::kOff;
    return runtimeOwnsSwapchain && streamlineStartupHandoffPending && runtimeStillInactive;
}

inline bool ShouldPreserveHeuristicFSRDuringTransientHeuristicBlock(bool canUseFSRHeuristics, bool runtimeOwnsSwapchain,
                                                                    bool hadFSRFGPhase,
                                                                    bool streamlineStartupHandoffPending) {
    // Preserve heuristic FSR only for transient runtime-owned teardown windows
    // that still plausibly belong to native FSR. A fresh authoritative
    // Streamline startup handoff is the opposite case: preserving stale
    // heuristic FSR there lets the later DLSS comeback bypass the repo's own
    // unsafe-GetState startup guards.
    return !canUseFSRHeuristics && runtimeOwnsSwapchain && hadFSRFGPhase && !streamlineStartupHandoffPending;
}

inline bool ShouldPreserveRuntimeOwnedFSRTeardown(bool targetIsNone, bool hadFSRFGPhase, bool runtimeOwnsSwapchain,
                                                  bool streamlineFGRunning) {
    return targetIsNone && hadFSRFGPhase && runtimeOwnsSwapchain && !streamlineFGRunning;
}

inline bool ShouldSuppressHeuristicFSRAfterExplicitNativeFSROff(bool explicitNativeFSROffPending,
                                                                bool runtimeOwnsSwapchain) {
    // A real native-FSR off signal can arrive before the runtime-owned swapchain
    // and queue topology have unwound back to the normal non-FG path. In that
    // teardown window, queue-change/ECL heuristics must not immediately relatch
    // FSR_FG or the overlay status flips back to stale FSR even though the
    // runtime already disabled frame generation explicitly.
    (void)runtimeOwnsSwapchain;
    return explicitNativeFSROffPending;
}

inline bool ShouldUsePrimaryQueueForFrameClassificationDuringPostFSRNonFGRecovery(
    bool recoveringPostFSRNonFG, bool actualFGActive, bool streamlineFGRunning, bool hasSwapchainQueue,
    bool hasOriginalGameQueue, bool hasPrimaryGameQueue, bool originalQueueMatchesPrimaryQueue) {
    if (!recoveringPostFSRNonFG || actualFGActive || streamlineFGRunning || hasSwapchainQueue) {
        return false;
    }

    if (!hasOriginalGameQueue || !hasPrimaryGameQueue) {
        return false;
    }

    // After FSR tears down, Talos can resume presenting real non-FG frames while
    // authoritative ECL traffic settles on the primary DIRECT queue instead of
    // the original Present queue. If frame classification stays pinned to
    // origGame in that window, top-level Presents flip back to "zero ECL" and
    // ProcessFrame stops driving the recovered overlay even though rendering
    // itself is still healthy.
    return !originalQueueMatchesPrimaryQueue;
}

inline bool ShouldGuardSwapchainReinitAfterChange(bool fgCurrentlyActive, bool fgRecentlyWasActive,
                                                  bool hasFGTransitionCooldown, bool recentStreamlineTeardown,
                                                  bool runtimeOwnsSwapchain, bool hasSwapchainQueue,
                                                  bool hasOriginalGameQueue,
                                                  bool swapchainQueueDiffersFromOriginalGameQueue) {
    if (fgCurrentlyActive || fgRecentlyWasActive || hasFGTransitionCooldown) {
        return true;
    }

    // Final DLSS FG teardown can briefly look like "FG fully off" even though
    // the replacement swapchain still belongs to the departing runtime and is
    // bound to a non-game queue. Immediate pre-SL reinit in that window can
    // recreate sync resources on the wrong queue and crash the game.
    return recentStreamlineTeardown && runtimeOwnsSwapchain && hasSwapchainQueue && hasOriginalGameQueue &&
           swapchainQueueDiffersFromOriginalGameQueue;
}

inline bool ShouldDeferInactiveRuntimeOwnedSwapchainOverlayInit(bool actualFGActive, bool streamlineFGRunning,
                                                                bool runtimeOwnsSwapchain, bool hasSwapchainQueue,
                                                                bool hasCommandQueue,
                                                                bool commandQueueMatchesSwapchainQueue,
                                                                bool retainedNoCallbackFSRSuspension) {
    if (actualFGActive || streamlineFGRunning) {
        return false;
    }

    if (!runtimeOwnsSwapchain || !hasSwapchainQueue) {
        return false;
    }

    // RETAINED NO-CALLBACK FSR SUSPENSION (test app session 20260702_142655: overlay INVISIBLE for the
    // WHOLE suspension): AMD keeps the FI swapchain and its runtime-owned queue latched while the app
    // renders on origGame, so "command traffic settles onto the live swapchain queue" can structurally
    // NEVER happen — this defer stranded the overlay blank until resume (every suspended present logged
    // "Deferring inactive runtime-owned swapchain overlay init until queue settles" with cmdQ=origGame,
    // scQ=AMD's FG queue). The suspension exemption has already approved normal overlay rendering ON the
    // runtime-owned swapchain queue (AMD is not interpolating, so the backbuffer submit is the
    // documented-safe route, and the ProcessFrame queue routing picks scQueue for exactly this state), so
    // overlay init must proceed instead of waiting for a settle that cannot occur.
    if (retainedNoCallbackFSRSuspension) {
        return false;
    }

    // After FG shuts off, the swapchain can persist on a runtime-owned queue
    // while command-list tracking is still null or stuck on a departed wrapper.
    // Reinitializing pre-SL overlay resources before command traffic settles
    // onto the live swapchain queue reproduces the Talos crash path.
    return !hasCommandQueue || !commandQueueMatchesSwapchainQueue;
}

inline bool ShouldUnregisterSwapchainDestructionCallbackDuringWrapperDestructor(bool wrapperReleasing,
                                                                                bool realSwapchainAvailable,
                                                                                bool hasDestructionCookie) {
    // Once wrapper Release() has reached zero external refs, the destructor is
    // already on the real swapchain teardown path. Optional DXGI mutation such
    // as notifier unregister can trip DXGI internal debug breakpoints in games
    // that combine Streamline/FFX/third-party overlays during startup. Release
    // our COM refs, but do not poke optional side channels in that edge.
    return !wrapperReleasing && realSwapchainAvailable && hasDestructionCookie;
}

inline bool ShouldClearSwapchainWrapperPrivateDataDuringWrapperDestructor(bool wrapperReleasing,
                                                                          bool realSwapchainAvailable) {
    // Clearing private data is only a hygiene step. During final wrapper
    // release the real swapchain's private-data table may already be inside
    // DXGI teardown, so avoid a crash-prone SetPrivateData call there.
    return !wrapperReleasing && realSwapchainAvailable;
}

inline bool ShouldStartDX12FocusLossOverlayCooldown(bool previousGameForeground, bool currentGameForeground) {
    (void)previousGameForeground;
    (void)currentGameForeground;
    // Focus ownership changes must not blank the injected DX12 overlay. The
    // renderer has per-frame allocator/fence guards now, so ordinary Alt+Tab
    // should keep drawing like third-party overlays instead of hiding for a
    // timed cooldown.
    return false;
}

inline bool ShouldKeepDX12FocusLossOverlayCooldown(bool cooldownTimerActive, bool currentGameForeground) {
    (void)cooldownTimerActive;
    (void)currentGameForeground;
    return false;
}

struct D3D12DeferredOverlaySignalFlushInfo {
    bool hadDeferredSignal = false;
    bool hasFence = false;
    bool hasFenceEvent = false;
    bool signalSucceeded = false;
    HRESULT signalHr = S_OK;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE fenceEvent = nullptr;
    UINT64 fenceValue = 0;
    UINT64 completedValue = 0;
};

struct D3D12FocusLossOverlayFenceWaitContext {
    const char* presentName = nullptr;
    int callCount = 0;
    bool isD3D12Swapchain = false;
    bool isFullscreen = false;
    bool processHasForeground = true;
    bool isIconic = false;
    bool hasZeroSize = false;
    bool presentSucceeded = false;
    bool presentDeviceLost = false;
    bool frameGenerationActive = false;
    bool runtimeOwnedPresentation = false;
    bool usingDedicatedQueue = false;
    HWND foregroundWindow = nullptr;
    DWORD foregroundPid = 0;
    HWND gameWindow = nullptr;
    DWORD processId = 0;
    UINT syncInterval = 0;
    UINT presentFlags = 0;
    HRESULT presentHr = S_OK;
};

inline bool ShouldWaitForD3D12FocusLossPostPresentOverlayFence(bool isD3D12Swapchain, bool isFullscreen,
                                                               bool processHasForeground, bool isIconic,
                                                               bool hasZeroSize, bool presentSucceeded,
                                                               bool presentDeviceLost, bool frameGenerationActive,
                                                               bool runtimeOwnedPresentation, bool usingDedicatedQueue,
                                                               bool hadDeferredOverlaySignal, bool signalSucceeded,
                                                               bool hasFence, bool hasFenceEvent, UINT64 fenceValue) {
    return isD3D12Swapchain && !isFullscreen && !processHasForeground && !isIconic && !hasZeroSize &&
           presentSucceeded && !presentDeviceLost && !frameGenerationActive && !runtimeOwnedPresentation &&
           !usingDedicatedQueue && hadDeferredOverlaySignal && signalSucceeded && hasFence && hasFenceEvent &&
           fenceValue != 0;
}

inline bool ShouldSignalD3D12FocusLossOverlayFenceImmediately(bool isWrappedD3D12Present, bool isFullscreen,
                                                              bool processHasForeground, bool isIconic,
                                                              bool hasZeroSize, bool overlaySubmitSucceeded,
                                                              bool deviceLost, bool frameGenerationActive,
                                                              bool runtimeOwnedPresentation, bool usingDedicatedQueue,
                                                              bool steamDeferredOverlaySubmit, bool hasFence,
                                                              bool hasFenceEvent, bool hasQueue, UINT64 fenceValue) {
    return isWrappedD3D12Present && !isFullscreen && !processHasForeground && !isIconic && !hasZeroSize &&
           overlaySubmitSucceeded && !deviceLost && !frameGenerationActive && !runtimeOwnedPresentation &&
           !usingDedicatedQueue && !steamDeferredOverlaySubmit && hasFence && hasFenceEvent && hasQueue &&
           fenceValue != 0;
}

inline bool ShouldWaitForD3D12FocusLossImmediateOverlayFence(bool immediateFencePolicyAccepted, bool signalSucceeded,
                                                             bool hasFence, bool hasFenceEvent, bool hasQueue,
                                                             UINT64 fenceValue) {
    return immediateFencePolicyAccepted && signalSucceeded && hasFence && hasFenceEvent && hasQueue && fenceValue != 0;
}

inline bool ShouldRequestImmediateDumpForD3D12FocusLossImmediateFenceWait(bool fenceWaitCompleted,
                                                                          bool dumpAlreadyRequested) {
    return !fenceWaitCompleted && !dumpAlreadyRequested;
}

// --- DescFree overlay UPLOAD-ring per-slot GPU-completion guard ---------------
//
// The DX12 DescFree overlay backend round-robins through a small pool of
// persistently-mapped UPLOAD vertex/index buffers.  The CPU must not memcpy new
// geometry into a ring slot while the GPU is still reading the previous frame's
// geometry from that same slot.  While the GPU keeps up this never happens, but
// during the iflip<->composited mode switch triggered by Alt+Tab the GPU stops
// retiring for hundreds of ms while the CPU keeps drawing the overlay at full
// rate; within poolSize frames the CPU wraps and overwrites in-flight data,
// corrupting the draw and wedging the GPU (DXGI_ERROR_DEVICE_HUNG / 2s TDR,
// observed x86/WoW64 only, captured via DRED + NtGdiDdDDICreateAllocation).
//
// DecideOverlayUploadSlotGuardValue() returns the overlay-fence value the GPU
// must reach before the slot used this frame may be reused, or 0 to disable the
// guard.  ShouldWaitForOverlayUploadSlot() decides whether the CPU must block
// before reusing a slot, given that slot's recorded guard and the fence's
// current completed value.  The fence is the real synchronization; pacing the
// CPU to the GPU here keeps the overlay visible every frame (never hidden) while
// preventing the upload-ring data race.
inline uint64_t DecideOverlayUploadSlotGuardValue(bool fgActive, bool hasOverlayFence, uint64_t currentFenceValue) {
    // FG paths advance a separate completion fence (not the overlay fence) and
    // already synchronize per frame, so a guard keyed on the overlay fence would
    // never be reached there -> disable it.  Without an overlay fence there is
    // nothing to wait on.
    if (fgActive || !hasOverlayFence) {
        return 0;
    }
    return currentFenceValue + 1;
}

inline bool ShouldWaitForOverlayUploadSlot(uint64_t slotGuardFenceValue, uint64_t gpuCompletedFenceValue) {
    return slotGuardFenceValue != 0 && gpuCompletedFenceValue < slotGuardFenceValue;
}

inline bool ShouldRecordDescFreeFontUpload(bool uploadPending, bool hasDefaultFontBuffer, bool hasUploadBuffer) {
    return uploadPending && hasDefaultFontBuffer && hasUploadBuffer;
}

inline bool ShouldUseTextureDx12OverlayBackendForProcess(bool is32BitProcess) {
    // Keep x86 on the standard native DX12 backend so the 32-bit path no longer
    // uses DescFree's separate root-SRV text path.  Text itself is emitted as
    // solid glyph spans by ShouldUseSolidDx12TextGeometryForProcess().
    return is32BitProcess;
}

inline bool ShouldUseSolidDx12TextGeometryForProcess(bool is32BitProcess) {
    // Native DX12 font-resource reads were observed hanging on the tested
    // x86/WoW64 NVIDIA path. Driver ownership of the underlying defect is not
    // vendor-confirmed, so keep the conservative architecture-wide policy.
    // Preserve the native direct overlay, but encode glyph coverage as solid
    // alpha geometry so text uses the proven resource-free solid PSO path.
    return is32BitProcess;
}

// v8 visibility-gated hold (supersedes the v3-v7 focus-based holds and the
// focus-loss offscreen composite).
//
// CE holds its swapchain backbuffer overlay/capture GPU work ONLY when the
// swapchain is genuinely not presentable: DXGI Present returned OCCLUDED, or the
// window is minimized (iconic), or the swapchain is zero-sized. In every one of
// those states the overlay is invisible to the user anyway, and it is the state
// in which the single-monitor Alt+Tab device-hung historically occurred (DXGI
// tears down the independent-flip surfaces). A merely-unfocused but STILL-VISIBLE
// window (e.g. a borderless background window, or a window on another monitor)
// keeps presenting S_OK and MUST keep rendering the overlay directly to the
// backbuffer — that is the behavior a proper inject overlay provides and what the user expects.
// Focus is NOT an input here: losing focus never hides the overlay.
inline bool ShouldHoldD3D12OverlayBackbufferWorkForNonPresentableSwapchain(
    bool isWrappedD3D12Present, bool isFullscreen, bool isOccluded, bool isIconic, bool hasZeroSize,
    bool frameGenerationActive, bool runtimeOwnedPresentation, bool usingDedicatedQueue,
    bool steamDeferredOverlaySubmit, bool deviceLost, bool hasQueue) {
    const bool notPresentable = isOccluded || isIconic || hasZeroSize;
    return isWrappedD3D12Present && !isFullscreen && notPresentable && !frameGenerationActive &&
           !runtimeOwnedPresentation && !usingDedicatedQueue && !steamDeferredOverlaySubmit && !deviceLost && hasQueue;
}

inline bool ShouldRequestImmediateDumpForD3D12FocusTransitionDeviceRemoval(bool deviceLost,
                                                                           bool focusTransitionRecentlyActive,
                                                                           bool dumpAlreadyRequested) {
    return deviceLost && focusTransitionRecentlyActive && !dumpAlreadyRequested;
}

// Historical v10 focus-transition predicate, retained as v13 telemetry only.
//
// Early DRED runs showed pure GPU hangs (pageFaultVA=0) after both a direct draw
// and an experimental backbuffer copy during Alt+Tab:
//   - v8 direct draw: hung on DRAWINDEXEDINSTANCED (logs/20260603_020053).
//   - v9 offscreen composite: hung on the bb->offscreen COPYTEXTUREREGION
//     (logs/20260603_150241) — the offscreen path still reads the live backbuffer.
// That initially motivated hiding backbuffer work around the focus edge. Later
// v13 investigation reproduced the hang without a focus transition, isolated it
// to x86 font-resource text draws, and rejected the visible hold. The production
// path uses this telemetry only to log the legacy edge counter and widen the DRED
// capture window; it must never gate overlay rendering. FG/runtime-owned/
// dedicated-queue/Steam-deferred routes retain separate diagnostics.
inline bool IsD3D12FocusTransitionTelemetryActive(bool isWindowed, int transitionFramesRemaining,
                                                  bool frameGenerationActive, bool runtimeOwnedPresentation,
                                                  bool usingDedicatedQueue, bool steamDeferredOverlaySubmit,
                                                  bool deviceLost, bool hasQueue) {
    return isWindowed && transitionFramesRemaining > 0 && !frameGenerationActive && !runtimeOwnedPresentation &&
           !usingDedicatedQueue && !steamDeferredOverlaySubmit && !deviceLost && hasQueue;
}

inline bool ShouldHoldD3D12FocusLossOverlayDrawForPendingFence(bool processHasForeground, bool hasPendingFocusLossFence,
                                                               bool pendingFenceComplete) {
    return !processHasForeground && hasPendingFocusLossFence && !pendingFenceComplete;
}

inline bool ShouldHoldD3D12FocusLossBackbufferWorkForPendingFence(bool processHasForeground,
                                                                  bool hasPendingFocusLossFence,
                                                                  bool pendingFenceComplete) {
    return ShouldHoldD3D12FocusLossOverlayDrawForPendingFence(processHasForeground, hasPendingFocusLossFence,
                                                              pendingFenceComplete);
}

inline bool ShouldClearRecentStreamlineTeardownGraceOnFreshActivation(bool active, bool hadRecentHeuristicGrace,
                                                                      bool hadRecentSwapchainReinitGrace) {
    return active && (hadRecentHeuristicGrace || hadRecentSwapchainReinitGrace);
}

inline bool ShouldDeferOverlayInitUntilCommandQueueSettlesAfterRecentStreamlineTeardown(
    bool actualFGActive, bool streamlineFGRunning, bool recentStreamlineTeardown, bool hasSwapchainQueue,
    bool hasOriginalGameQueue, bool hasPostSLLastWorkingQueue, bool hasCommandQueue,
    bool commandQueueMatchesSwapchainQueue, bool commandQueueMatchesOriginalGameQueue,
    bool commandQueueMatchesPrimaryGameQueue) {
    if (actualFGActive || streamlineFGRunning) {
        return false;
    }

    if (!recentStreamlineTeardown || !hasOriginalGameQueue) {
        return false;
    }

    if (!hasSwapchainQueue) {
        // After the post-FSR DLSS teardown path we intentionally leave
        // g_SwapchainQueue unset until command traffic proves which non-wrapper
        // queue is actually live again. Both origGame and the primary ECL queue
        // reproduced Talos DEVICE_REMOVED on the first resumed non-FG overlay
        // submit. The only safe immediate recovery path is the last PostSL queue
        // that already proved it could render the live swapchain successfully.
        //
        // However, multi-queue games (e.g. Talos Principle) use separate DIRECT
        // queues for ECL (render) and Present (swapchain).  The primary game queue
        // is the first DIRECT queue observed and is always a valid non-wrapper queue.
        // If the command queue has already settled back to the primary queue, that
        // queue is proven safe even when the preserved PostSL last-working queue was
        // cleared by an earlier failed comeback.  Treating it as "unsettled" strands
        // the overlay for the entire grace period.
        if (commandQueueMatchesPrimaryGameQueue) {
            return false;
        }
        return !hasPostSLLastWorkingQueue;
    }

    // After the post-FSR DLSS path turns FG off, late Streamline teardown ECLs
    // can keep repopulating g_CommandQueue with a departed wrapper queue even
    // after the live non-FG swapchain queue has been restored. Rebuilding the
    // pre-SL overlay path before command tracking settles back onto the live
    // queue reproduces Talos DEVICE_REMOVED on the first non-FG submit.
    //
    // However, multi-queue games (e.g. Talos Principle) use separate DIRECT
    // queues for ECL (render) and Present (swapchain).  The primary game queue
    // is the first DIRECT queue observed and is always a valid non-wrapper queue.
    // Treating it as "unsettled" defers overlay reinit for the entire grace
    // period, which itself causes DEVICE_REMOVED from stale GPU state.
    if (commandQueueMatchesPrimaryGameQueue) {
        return false;
    }

    return !hasCommandQueue || (!commandQueueMatchesSwapchainQueue && !commandQueueMatchesOriginalGameQueue);
}

inline bool ShouldIgnoreCommandQueueRegistrationAfterRecentStreamlineTeardown(
    bool recentStreamlineTeardown, bool postFSRNonFGRecovery,
    bool postSLLastWorkingQueueStillActiveDuringRecentTeardown, bool queueMatchesPrimaryQueue,
    bool queueMatchesOriginalGameQueue, bool queueMatchesSwapchainQueue, bool queueMatchesPostSLLastWorkingQueue) {
    // During final Streamline teardown after a post-FSR DLSS phase, helper ECLs
    // can keep arriving on a departed wrapper queue for a short time even though
    // Present has already returned to the non-FG swapchain queue. Do not let
    // those teardown queues repollute g_CommandQueue.
    // The preserved PostSL last-working queue is also teardown-era state in this
    // window. It stays valuable for immediate non-FG overlay recovery, but it
    // must not be re-registered as the live game command queue or the queue-change
    // heuristic will briefly re-detect FSR FG when menus reopen.
    //
    // The preserved PostSL last-working queue can keep surfacing teardown-era
    // ECL traffic for a short time after the coarse Streamline-off grace
    // counter reaches zero. Keep ignoring that exact queue while it is still
    // actively resurfacing so it cannot repollute command tracking right before
    // the non-FG overlay resumes.
    //
    // During the longer post-FSR non-FG recovery window, the overlay itself can
    // continue submitting on that preserved queue because it is the only queue
    // that already proved safe for the recovered swapchain. Those recovery
    // submits must not repollute g_CommandQueue, or routing immediately falls
    // off the only positive-proof queue and back onto unsafe best-guess queues.
    if (queueMatchesPostSLLastWorkingQueue && !queueMatchesPrimaryQueue && !queueMatchesOriginalGameQueue &&
        !queueMatchesSwapchainQueue) {
        return recentStreamlineTeardown || postSLLastWorkingQueueStillActiveDuringRecentTeardown ||
               postFSRNonFGRecovery;
    }

    if (!recentStreamlineTeardown) {
        return false;
    }

    return !queueMatchesPrimaryQueue && !queueMatchesOriginalGameQueue && !queueMatchesSwapchainQueue;
}

inline bool ShouldIgnoreQueueChangeHeuristicDuringRecentStreamlineTeardown(
    bool recentStreamlineTeardown, bool postFSRNonFGRecovery,
    bool postSLLastWorkingQueueStillActiveDuringRecentTeardown, bool queueMatchesPrimaryQueue,
    bool queueMatchesOriginalGameQueue, bool queueMatchesSwapchainQueue, bool queueMatchesPostSLLastWorkingQueue) {
    // The preserved PostSL queue can still resurface as teardown traffic after
    // DLSS FG turns off. Treat it like a departed runtime queue for heuristic
    // purposes so a late menu transition cannot blip back into heuristic FSR FG.
    // Keep honoring that rule while that preserved queue is still actively
    // resurfacing as teardown traffic, not just while the coarse Streamline-off
    // grace counter is still positive.
    //
    // While the post-FSR non-FG recovery path is still active, keep ignoring
    // that preserved queue for heuristic purposes as well. The recovered overlay
    // can legitimately keep using it long after the short teardown pulse ends,
    // and treating those submits as fresh queue-change evidence would falsely
    // re-detect FSR FG.
    if (queueMatchesPostSLLastWorkingQueue && !queueMatchesPrimaryQueue && !queueMatchesOriginalGameQueue &&
        !queueMatchesSwapchainQueue) {
        return recentStreamlineTeardown || postSLLastWorkingQueueStillActiveDuringRecentTeardown ||
               postFSRNonFGRecovery;
    }

    if (!recentStreamlineTeardown) {
        return false;
    }

    return !queueMatchesPrimaryQueue && !queueMatchesOriginalGameQueue && !queueMatchesSwapchainQueue;
}

inline bool ShouldRealignInactiveCommandQueueToSwapchainQueue(bool actualFGActive, bool streamlineFGRunning,
                                                              bool hasSwapchainQueue, bool hasOriginalGameQueue,
                                                              bool hasCommandQueue,
                                                              bool commandQueueMatchesSwapchainQueue,
                                                              bool commandQueueMatchesOriginalGameQueue,
                                                              bool commandQueueMatchesPrimaryGameQueue) {
    if (actualFGActive || streamlineFGRunning) {
        return false;
    }

    if (!hasSwapchainQueue || !hasOriginalGameQueue || !hasCommandQueue) {
        return false;
    }

    // Once Streamline is fully off, a command queue that matches neither the
    // live swapchain queue nor the original game queue is just stale wrapper
    // state. Realign it to the swapchain queue so pre-SL init stops observing a
    // departed wrapper topology.
    //
    // However, multi-queue games use separate DIRECT queues for ECL and Present.
    // The primary game queue (first DIRECT queue seen) is always valid and must
    // NOT be realigned away from, or the ECL ignore logic loses its anchor and
    // the game's legitimate ECL queue gets misclassified as departed.
    if (commandQueueMatchesPrimaryGameQueue) {
        return false;
    }

    return !commandQueueMatchesSwapchainQueue && !commandQueueMatchesOriginalGameQueue;
}
inline bool ShouldDeferOverlayReinitAfterDirectPostFSRStreamlineTeardown(bool hadFSRFGPhase, bool overlayInit,
                                                                         bool syncInit) {
    // The direct Streamline teardown path can invalidate overlay state before
    // ProcessFrame reaches its own SL transition tracking block. In that case,
    // ProcessFrame misses the OFF edge and would otherwise rebuild immediately
    // on the same teardown Present.
    return hadFSRFGPhase && (!overlayInit || !syncInit);
}

inline bool ShouldMutatePostSLLockedQueue(bool hasLockedQueue, bool selectedQueueMatchesLockedQueue,
                                          bool shouldReplaceLockedQueue) {
    if (!hasLockedQueue) {
        return true;
    }

    if (selectedQueueMatchesLockedQueue) {
        return false;
    }

    return shouldReplaceLockedQueue;
}

inline bool ShouldRememberPostSLLastWorkingQueue(bool queueIsSLWrapper) {
    return !queueIsSLWrapper;
}

inline bool ShouldSeedStreamlineStartupBootstrapAsConsumedForConfirmedPostSLResume(
    bool hadFSRFGPhase, bool hasPostSLLastWorkingQueue, bool hasSwapchainQueue,
    bool swapchainQueueMatchesPostSLLastWorkingQueue, bool runtimeOwnedNoFGStateWasClearedAfterLongOrigGameRun,
    bool hasOriginalGameQueue, bool commandQueueMatchesOriginalGameQueue) {
    // Pure-DLSS resume after a real DLSS suspension should reuse the already
    // proven PostSL topology when the live swapchain queue still matches the last
    // queue that successfully rendered PostSL. Reopening the old startup
    // bootstrap seam in that case sends the first resumed Streamline Present back
    // down the synthetic/bypass route and reproduces the GTA menu-close crash.
    if (hadFSRFGPhase) {
        return false;
    }

    if (hasPostSLLastWorkingQueue && hasSwapchainQueue && swapchainQueueMatchesPostSLLastWorkingQueue) {
        return true;
    }

    // A late in-session DLSS enable can also happen after CE already proved that
    // the runtime-owned no-FG handoff was auxiliary/stale and explicitly cleared
    // it after a long real-frame run back on origGame. In that case the next
    // DLSS-on edge should reuse the now-proven top-level game Present path rather
    // than reopening the old fragile startup bootstrap seam.
    return runtimeOwnedNoFGStateWasClearedAfterLongOrigGameRun && hasOriginalGameQueue &&
           commandQueueMatchesOriginalGameQueue;
}

inline bool ShouldReuseValidatedPostSLLastWorkingQueueForStreamlineResumeDuringPostFSRInactiveRecovery(
    bool hadFSRFGPhase, bool postFSRInactiveRecoveryPending, bool hasPostSLLastWorkingQueue, bool hasSwapchainQueue,
    bool explicitSetOptionsActivation, bool safePostFSRBootstrapPath) {
    // After a mixed FSR->DLSS epoch goes fully FG-off, the recovered non-FG path
    // can intentionally keep scQueue unset while it reuses the already validated
    // PostSL last-working queue. If a later DLSS-only resume happens before a
    // fresh non-FG swapchain proof re-establishes scQueue, that validated queue
    // is still the strongest evidence for the live topology. Once the recovery
    // epoch ends, the same pointer is historical evidence only and must not be
    // revived for a later DLSS activation.
    return hadFSRFGPhase && postFSRInactiveRecoveryPending && hasPostSLLastWorkingQueue && !hasSwapchainQueue &&
           (explicitSetOptionsActivation || safePostFSRBootstrapPath);
}

inline bool ShouldInvalidatePostSLLastWorkingQueueOnFreshPostFSRStreamlineHandoff(
    bool hadFSRFGPhase, bool hasSwapchainQueue, bool swapchainQueueDiffersFromOriginalGameQueue,
    bool streamlineStartupHandoffPending, bool hasPostSLLastWorkingQueue,
    bool swapchainQueueMatchesPostSLLastWorkingQueue) {
    // A fresh post-FSR Streamline handoff to a different runtime-owned swapchain
    // queue invalidates the old PostSL last-working queue proof. Reusing proof from
    // the previous DLSS epoch after a newer authoritative handoff can drive the
    // next off-recovery back onto a stale queue if the new comeback tears down
    // before first confirmation.
    return hadFSRFGPhase && hasSwapchainQueue && swapchainQueueDiffersFromOriginalGameQueue &&
           streamlineStartupHandoffPending && hasPostSLLastWorkingQueue && !swapchainQueueMatchesPostSLLastWorkingQueue;
}

}  // namespace ce::dx12_overlay_policy
