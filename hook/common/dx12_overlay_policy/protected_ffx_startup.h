#pragma once

#include <windows.h>

#include <cstdint>

#include <dxgi1_6.h>

#include "../dxgi_presentation_color.h"
#include "../fg_runtime_state.h"

struct ID3D12CommandQueue;
struct ID3D12Fence;

#include "postsl_keepalive.h"

// Protected official-FFX startup: overlay-only routes and cooldown bypasses.

namespace ce::dx12_overlay_policy {

inline bool ShouldAllowOverlayOnlyDuringProtectedOfficialFFXStartup(bool protectedOfficialFFXStartupPending,
                                                                    bool ffxStartupAlreadyResolved,
                                                                    bool hasStagedDirectQueue) {
    (void)protectedOfficialFFXStartupPending;
    (void)ffxStartupAlreadyResolved;
    (void)hasStagedDirectQueue;
    // The staged DIRECT queue is useful deferred takeover state, but it is not
    // proof that separate CE overlay submissions are safe before AMD reaches an
    // enabled ffxConfigure / present-callback packet. GTA's DLSS->FSR handoff
    // showed that even "overlay-only" ECLs in this pre-enable window can leave
    // the AMD presenter/interpolation threads waiting inside ffxQuery.
    return false;
}

// The staged official-FFX create queue is deferred takeover evidence only. It is AMD's internal
// present queue in GTA and must never receive CE overlay work before an enabled configure proves
// the final route. Startup visibility instead rides the already-hooked game-facing proxy Present:
// the proxy exposes its current passthrough backbuffer, and the descriptor binding resolves the
// target-compatible game/producer queue that AMD orders before its internal present.
inline bool ShouldUseProtectedOfficialFFXStartupProxyBackbufferRoute(bool protectedOfficialFFXStartupPending,
                                                                     bool ffxStartupAlreadyResolved,
                                                                     bool proxyPresentHookInstalled) {
    return protectedOfficialFFXStartupPending && !ffxStartupAlreadyResolved && proxyPresentHookInstalled;
}

// Retained as a hard guardrail for legacy transition call sites. Sustained frame progress, a live
// backend, or a staged queue cannot turn the nested real-swapchain Present into a safe submit path.
inline bool ShouldAllowNormalOverlayDrawDuringDormantProtectedOfficialFFXStartup(
    bool protectedOfficialFFXStartupPending, bool overlayInit, bool syncInit, bool hasDistinctStagedTakeoverQueue,
    bool hasDirectFFXApiConfirmation, bool ffxPresentCallbackActive, bool sustainedGameProgress) {
    (void)protectedOfficialFFXStartupPending;
    (void)overlayInit;
    (void)syncInit;
    (void)hasDistinctStagedTakeoverQueue;
    (void)hasDirectFFXApiConfirmation;
    (void)ffxPresentCallbackActive;
    (void)sustainedGameProgress;
    return false;
}

inline bool ShouldBypassFGTransitionCooldownForProtectedOfficialFFXOverlayOnly(bool protectedOverlayOnlyEligible,
                                                                               bool hasStagedDirectQueue) {
    (void)protectedOverlayOnlyEligible;
    (void)hasStagedDirectQueue;
    // Pre-enable protected official FFX startup must stay GPU-quiet. Visibility
    // resumes through the FFX present callback once enabled configure/callback
    // proof exists, not through a generic cooldown bypass.
    return false;
}

inline bool ShouldPreserveOverlayBackendAcrossProtectedOfficialFFXStartupSwapchainChange(
    bool protectedOfficialFFXStartupPending, bool ffxStartupAlreadyResolved) {
    // The protected swapchain is not yet a drawable CE target, but tearing down
    // the old backend and immediately rebuilding against the staged FFX queue is
    // worse: it creates the exact pre-enable GPU traffic that can wedge AMD FSR.
    // Preserve backend/resources and let the callback path take over once proof
    // arrives.
    return ShouldQuiesceCESideEffectsDuringProtectedOfficialFFXStartup(protectedOfficialFFXStartupPending,
                                                                       ffxStartupAlreadyResolved);
}

inline bool ShouldQuiesceStreamlinePostSLDuringProtectedOfficialFFXStartup(
    bool protectedOfficialFFXStartupPending, bool ffxStartupAlreadyResolved, bool postSLCallbackInstalled,
    bool postSLActive, bool postSLConfirmedRendering, bool streamlineFGRunning, bool startupActivationPending) {
    // Heavy official-FFX takeover side effects wait for ffxConfigure(enable), but
    // an already-live Streamline/PostSL path must stop immediately. Otherwise a
    // re-entrant Streamline callback can submit overlay work to the old DLSS path
    // after the AMD runtime has begun replacing the swapchain.
    return ShouldQuiesceCESideEffectsDuringProtectedOfficialFFXStartup(protectedOfficialFFXStartupPending,
                                                                       ffxStartupAlreadyResolved) &&
           (postSLCallbackInstalled || postSLActive || postSLConfirmedRendering || streamlineFGRunning ||
            startupActivationPending);
}

inline uint32_t GetProtectedOfficialFFXStartupProcessFrameProgressThreshold() {
    return 120;
}

inline uint32_t GetProtectedOfficialFFXStartupECLProgressThreshold() {
    return 4096;
}

inline bool ShouldFinalizeProtectedOfficialFFXStartupAfterSustainedFrameProgress(
    bool protectedOfficialFFXStartupPending, bool ffxStartupAlreadyResolved, uint32_t processFrameSkips,
    uint32_t eclPassThroughs) {
    (void)protectedOfficialFFXStartupPending;
    (void)ffxStartupAlreadyResolved;
    (void)processFrameSkips;
    (void)eclPassThroughs;
    // GTA freeze dumps showed the old progress-only graduation could resume CE
    // overlay/capture side effects while the official AMD presenter thread was
    // still inside its private startup/query path. Only a real direct configure
    // or present-callback proof is authoritative.
    return false;
}

inline bool ShouldApplySwapchainDescriptorOverridesForCreate(bool callerFromThirdPartyOverlay,
                                                             bool authoritativeFrameGenerationRuntimeCreator) {
    // Runtime FG components treat swapchain creation as part of their own
    // startup handshake. Preserve their descriptor byte-for-byte; even
    // "safe" CE additions such as the waitable-object flag can change that
    // handshake before the runtime has accepted its configure packet.
    return !callerFromThirdPartyOverlay && !authoritativeFrameGenerationRuntimeCreator;
}

inline bool ShouldTreatNativeFSRSwapchainAsRuntimeOwnedForConfigure(bool runtimeOwnsSwapchain,
                                                                    bool protectedOfficialFFXStartupPending) {
    // The first disabled startup-arming ffxConfigure can arrive before CE has
    // safely claimed the official AMD runtime swapchain queue. Keep that packet
    // in the startup-arming path instead of treating it as a real OFF.
    return runtimeOwnsSwapchain || protectedOfficialFFXStartupPending;
}

inline bool ShouldTreatRuntimeOwnedSwapchainAsNativeFSRPresentPath(bool runtimeOwnsSwapchain,
                                                                   bool directFFXApiConfirmation,
                                                                   bool nativeFSRStartupArmingPending) {
    // FSR context creation alone is not proof that native FSR owns presentation:
    // some games create a frame-generation context while still configured OFF.
    // Treat a runtime-owned swapchain as native-FSR presentation only after a
    // direct enabled configure, or while an explicit native-FSR startup arming
    // path is pending.
    return runtimeOwnsSwapchain && (directFFXApiConfirmation || nativeFSRStartupArmingPending);
}

inline bool ShouldClearStaleNativeFGPresentOwnershipOnStreamlineComeback(
    bool hadFSRFGPhase, bool explicitSetOptionsActivation, bool authoritativeStreamlineHandoff,
    bool authoritativeFSRActive, bool hasSwapchainQueue, bool swapchainQueueDiffersFromOriginalGameQueue,
    bool streamlineStartupHandoffPending, bool runtimeOwnedNativeFGPresentPath,
    bool nativeFSRInternalNoCallbackComposition) {
    // After a real FSR -> DLSS comeback, the preserved non-origGame swapchain queue
    // can already belong to the new authoritative Streamline handoff. In that
    // state, a stale native-FSR Present-ownership latch from the prior runtime must
    // not keep the later DLSS startup path classified as still runtime-owned native
    // FG. Clear only that stale native-FSR ownership latch; the generic runtime-
    // owned swapchain fact can still remain true for the new Streamline-owned
    // queue topology. The exact authoritative Streamline swapchain handoff is
    // sufficient comeback proof even while DLSS remains suspended in a menu and
    // never emits SetOptions(ON); require FSR itself to be inactive on that path.
    // The internal no-callback route is an independent retained suspension latch:
    // it can remain true after the broader native-FG ownership predicate has
    // already reclassified the non-original queue as Streamline's.
    const bool provenComeback = explicitSetOptionsActivation ||
                                (authoritativeStreamlineHandoff && !authoritativeFSRActive);
    const bool handoffEstablished = streamlineStartupHandoffPending || authoritativeStreamlineHandoff;
    return hadFSRFGPhase && provenComeback && hasSwapchainQueue &&
           swapchainQueueDiffersFromOriginalGameQueue && handoffEstablished &&
           (runtimeOwnedNativeFGPresentPath || nativeFSRInternalNoCallbackComposition);
}

inline bool ShouldPassThroughCreateSwapchainAccessDeniedForStreamline(bool streamlineModuleLoaded,
                                                                      bool streamlineFGRunning,
                                                                      bool streamlineStartupHandoffPending,
                                                                      bool callerFromFFXFGModule,
                                                                      bool ffxFrameGenerationInStack) {
    if (!streamlineModuleLoaded) {
        return false;
    }

    // During mixed DLSS/FSR sessions, authoritative FFX takeover is also a
    // runtime-managed swapchain lifecycle boundary. CE must not tear down
    // overlay resources or retry CreateSwapChainForHwnd on that path.
    if (callerFromFFXFGModule || ffxFrameGenerationInStack) {
        return true;
    }

    return streamlineFGRunning || streamlineStartupHandoffPending;
}

// The deep CreateSwapChainForHwnd hook forwards through its below-the-chain trampoline, which performs
// the genuine DXGI create WITHOUT entering the foreign overlay entry chain. A foreign overlay that
// tracks swapchain creates through its own CreateSwapChainForHwnd entry handler — and holds a reference
// to the old swapchain (and with it the HWND association DXGI checks) until a replacement create arrives
// through that handler — then never gets to release it, and every retry stays E_ACCESSDENIED
// (dx12_fg_switch_test OFF->FSR with Steam overlay and RTSS injected, session 20260813_200741: the
// replacement create fails through the full CE cleanup, the game's own fallback create fails the same
// way, and the app exits cleanly — no dump, because nothing faults). Retrying once through the live
// entry chain runs those foreign handlers before the genuine create so they can release the old
// swapchain. Foreign-overlay callers are excluded so an overlay's own internal create cannot re-enter
// its own entry handler, and shutdown stays below any foreign code.
inline bool ShouldRetryAccessDeniedCreateThroughLiveEntryChain(bool foreignEntryChainExists,
                                                               bool callerFromThirdPartyOverlay,
                                                               bool hookShuttingDown) {
    return foreignEntryChainExists && !callerFromThirdPartyOverlay && !hookShuttingDown;
}

// DRED arming level for CE_DX12_DRED.
//
// Full DRED auto-breadcrumbs force the application's command-list Reset() to do a
// per-frame kernel GPU allocation; that stalls Present during the Alt+Tab mode
// switch (logs/20260606_145929) AND shifts steady-state timing enough to mask a
// timing-sensitive GPU hang. Page-fault-only arming does NOT enable
// auto-breadcrumbs, so it adds no per-Reset allocation — it still records the
// faulting GPU virtual address + existing/recently-freed allocation nodes on a
// device-removal, which is the smoking gun for a GPU-side DEVICE_HUNG. Prefer
// page-fault-only for the uncapped steady-state DEVICE_HUNG repro.
enum class DredArmMode : int {
    kOff = 0,
    kPageFaultOnly = 1,  // page-fault output only — low perturbation
    kFull = 2,           // auto-breadcrumbs + page-fault + context — high perturbation
};

// Decide DRED arming level from the CE_DX12_DRED env/flag value. `isSet` is whether
// the value is present; `value` its (possibly null) contents.
//   page-fault-only: "pf" / "pagefault" / "page-fault" / "2"
//   full:            "1" / "on" / "true" / "yes" / "full"
//   off:             unset / "0" / "off" / "false" / anything unrecognized
inline DredArmMode DecideDredArmMode(const char* value, bool isSet) {
    if (!isSet || value == nullptr || value[0] == '\0') {
        return DredArmMode::kOff;
    }
    if (_stricmp(value, "pf") == 0 || _stricmp(value, "pagefault") == 0 || _stricmp(value, "page-fault") == 0 ||
        _stricmp(value, "2") == 0) {
        return DredArmMode::kPageFaultOnly;
    }
    if (_stricmp(value, "1") == 0 || _stricmp(value, "on") == 0 || _stricmp(value, "true") == 0 ||
        _stricmp(value, "yes") == 0 || _stricmp(value, "full") == 0) {
        return DredArmMode::kFull;
    }
    return DredArmMode::kOff;
}

// Pure decision for whether DRED arming is enabled at all (either mode). Kept as a
// thin wrapper over DecideDredArmMode so DRED stays OFF unless explicitly requested.
inline bool ShouldEnableDredFromEnv(const char* value, bool isSet) {
    return DecideDredArmMode(value, isSet) != DredArmMode::kOff;
}

// When the D3D12 device is already removed/hung, forwarding the application's
// command lists into the torn-down driver dereferences freed UMD state and crashes
// inside the driver (observed: a 32-bit DEVICE_HUNG TDR is followed ~1s later by an
// nvwgf2um access violation while the app's render loop keeps calling
// ExecuteCommandLists). A D3D12 device is permanently lost once removed, so dropping
// the submission is the only safe action — the app still learns of the loss when its
// next Present returns DXGI_ERROR_DEVICE_*. Returns true only while the device is
// healthy.
inline bool ShouldForwardAppCommandListsToDriver(bool deviceRemoved) {
    return !deviceRemoved;
}

// Streamline UIColorAndAlpha tags use eValidUntilPresent: a submitted UI record
// covers only that source frame. During a cold DLSS-G activation, keep replacing
// the record on each new source frame until PostSL has consumed the requested
// output handoff. Source-frame rollover must not spend this budget; otherwise a
// tag can expire immediately before the first PostSL output and expose one blank
// present. The caller serializes access while this small policy object remains
// independently unit-testable.
class StreamlineUiActivationCoverageBudget {
public:
    void Reset() {
        remainingOutputs_ = 0;
    }

    void Arm(uint32_t maximumOutputPresents) {
        remainingOutputs_ = maximumOutputPresents;
    }

    bool NeedsCurrentFrameRecord() const {
        return remainingOutputs_ != 0;
    }

    bool ConsumePostSLOutput() {
        if (remainingOutputs_ == 0) {
            return false;
        }
        --remainingOutputs_;
        return true;
    }

    uint32_t Remaining() const {
        return remainingOutputs_;
    }

private:
    uint32_t remainingOutputs_ = 0;
};

inline bool ShouldRequireExactPostSLBackbufferDrawForStartup(bool forcedStartupTransportDraw, bool hadFSRFGPhase,
                                                             bool safePostFSRBootstrapPath,
                                                             bool explicitEnablePureDLSSColdStartProof,
                                                             bool officialUiCoverageActive) {
    // Official UIColorAndAlpha coverage is useful for later generated outputs,
    // but some runtimes can expose their first proxy output before that tag is
    // visible. The first proven PostSL callback must therefore seed the exact
    // output backbuffer itself after either a safe post-FSR handoff or an
    // explicit pure-DLSS cold start, then retire the bounded UI handoff so every
    // later proxy buffer also receives an exact PostSL draw. GetState-only
    // activation does not provide enough provenance for the cold-start path.
    return forcedStartupTransportDraw || (officialUiCoverageActive && ((hadFSRFGPhase && safePostFSRBootstrapPath) ||
                                                                       explicitEnablePureDLSSColdStartProof));
}

// ---------------------------------------------------------------------------
// [OVERLAY COVERAGE] per-present overlay-coverage accounting.
// ---------------------------------------------------------------------------
// Goal: zero presented-frames-without-overlay across all FG transitions. Each
// accounted present is either covered (an overlay draw of any route — normal,
// PostSL, FFX present callback — was observed for it) or uncovered; uncovered
// presents form streaks that are the direct measure of visible overlay blanks.
//
// `drawObserved` is the caller's draw-counter delta since the previous
// accounted present. `inheritCoverageIfNoDraw` handles presents whose overlay
// content is composed by the FG runtime from a previous covered present
// (zero-ECL interpolated frames, SL-owned top-level transport presents): they
// count as covered while the current uncovered streak is zero, and extend the
// streak otherwise. That keeps healthy FG sessions free of false 1-present
// streak noise while real blank windows still grow one continuous streak.
//
// Thread-safety is the caller's job (the hook serializes calls with a tiny
// spin lock); this type stays plain so it is unit-testable.
struct OverlayPresentCoverageResult {
    bool covered = false;
    bool uncoveredStreakStarted = false;
    bool uncoveredStreakEnded = false;
    uint64_t endedStreakLength = 0;
    bool newLongestStreak = false;
};

inline bool ShouldAccountOverlayVisibilityPresent(uint64_t overlayDrawCount) {
    return overlayDrawCount != 0;
}

inline bool ShouldAccountPostSLCallbackAsSeparatePresent(bool hasSwapchain, bool observerOnly,
                                                         bool drawBelongsToEnclosingProcessFramePresent) {
    // An inline exact-proxy keep-alive draw is part of the ProcessFrame Present
    // that invoked it. Accounting both scopes consumes the draw in the inner
    // callback and falsely classifies the enclosing Present as uncovered.
    return hasSwapchain && !observerOnly && !drawBelongsToEnclosingProcessFramePresent;
}

class OverlayPresentCoverageTracker {
public:
    OverlayPresentCoverageResult NotePresent(bool drawObserved, bool inheritCoverageIfNoDraw) {
        OverlayPresentCoverageResult result;
        ++totalPresents_;
        result.covered = drawObserved || (inheritCoverageIfNoDraw && currentUncoveredStreak_ == 0);
        if (result.covered) {
            if (currentUncoveredStreak_ > 0) {
                result.uncoveredStreakEnded = true;
                result.endedStreakLength = currentUncoveredStreak_;
                currentUncoveredStreak_ = 0;
            }
            return result;
        }
        ++uncoveredPresents_;
        result.uncoveredStreakStarted = (currentUncoveredStreak_ == 0);
        ++currentUncoveredStreak_;
        if (currentUncoveredStreak_ > longestUncoveredStreak_) {
            longestUncoveredStreak_ = currentUncoveredStreak_;
            result.newLongestStreak = true;
        }
        return result;
    }

    uint64_t TotalPresents() const {
        return totalPresents_;
    }
    uint64_t UncoveredPresents() const {
        return uncoveredPresents_;
    }
    uint64_t CurrentUncoveredStreak() const {
        return currentUncoveredStreak_;
    }
    uint64_t LongestUncoveredStreak() const {
        return longestUncoveredStreak_;
    }

private:
    uint64_t totalPresents_ = 0;
    uint64_t uncoveredPresents_ = 0;
    uint64_t currentUncoveredStreak_ = 0;
    uint64_t longestUncoveredStreak_ = 0;
};

}  // namespace ce::dx12_overlay_policy
