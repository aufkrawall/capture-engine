#include "streamline_hook_internal.h"


void LogDroppedSuppressedOffForStartupProtectedStreamlineComeback(
    uint32_t viewportKey,  bool hadFSRFGPhase,  bool explicitSetOptionsActivationForCurrentComeback, 
    bool safePostFSRBootstrapPath,  bool startupActivationPending,  bool postSLActiveButUnconfirmed, 
    bool postSLConfirmedRendering,  bool postSLConfirmedButStartupSettling, 
    bool postSLConfirmedButRuntimeStateStabilizing) {


    static std::atomic<int> s_dropLogCount{0};
    const int logCount = s_dropLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 10 || (logCount % 100) == 0) {
        HookLogImportant(
            "Streamline Hook: Dropping stale suppressed slDLSSGSetOptions(OFF) after startup window expiry because "
            "Streamline DLSS startup is already stably active (viewport=%u hadFSR=%d explicit=%d safeBootstrap=%d "
            "pending=%d unconfirmed=%d confirmed=%d settling=%d stabilizing=%d)",
            viewportKey, hadFSRFGPhase ? 1 : 0, explicitSetOptionsActivationForCurrentComeback ? 1 : 0,
            safePostFSRBootstrapPath ? 1 : 0, startupActivationPending ? 1 : 0, postSLActiveButUnconfirmed ? 1 : 0,
            postSLConfirmedRendering ? 1 : 0, postSLConfirmedButStartupSettling ? 1 : 0,
            postSLConfirmedButRuntimeStateStabilizing ? 1 : 0);
    }

}


bool TryServicePostSLStartupActivation(const char* source,  bool clearStartupWindow) {


    auto service = DXGIShared::g_PostSLStartupActivationService.load(std::memory_order_acquire);
    if (!service) {
        static std::atomic<int> s_missingServiceLogCount{0};
        const int logCount = s_missingServiceLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 100) == 0) {
            HookLogImportant(
                "Streamline Hook: PostSL startup activation service unavailable "
                "(source=%s clearWindow=%d)",
                source ? source : "unknown", clearStartupWindow ? 1 : 0);
        }
        return false;
    }

    return service(source, clearStartupWindow);

}


void ResetStartupProtectedOffChurnActiveProof(const char* reason) {


    const bool wasPending = streamline_hook_g_StartupProtectedOffChurnNeedsActiveProof.exchange(false, std::memory_order_acq_rel);
    const uint32_t previousProof = streamline_hook_g_StartupProtectedOffChurnActiveProofCount.exchange(0, std::memory_order_acq_rel);
    if (wasPending || previousProof > 0) {
        static std::atomic<int> s_resetLogCount{0};
        const int logCount = s_resetLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 100) == 0) {
            HookLogImportant(
                "Streamline Hook: Reset startup-protected OFF quiet proof "
                "(reason=%s wasPending=%d activeProof=%u)",
                reason ? reason : "unknown", wasPending ? 1 : 0, previousProof);
        }
    }

}


void LogAcceptedOffDuringActivatedUnconfirmedResume(const char* source,  bool startupWindowActive,  bool hadFSRFGPhase, 
                                                    bool explicitSetOptionsActivationForCurrentComeback, 
                                                    bool safePostFSRBootstrapPath,  bool startupActivationPending, 
                                                    bool postSLActiveButUnconfirmed, 
                                                    bool postSLStartupActivationEntered,  bool postSLConfirmedRendering, 
                                                    bool postSLConfirmedButStartupSettling, 
                                                    bool postSLConfirmedButRuntimeStateStabilizing) {


    static std::atomic<int> s_acceptLogCount{0};
    const int logCount = s_acceptLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 20 || (logCount % 100) == 0) {
        HookLogImportant(
            "Streamline Hook: Accepting Streamline OFF during activated-but-unconfirmed startup resume "
            "(source=%s startupWindow=%d hadFSR=%d explicit=%d safeBootstrap=%d pending=%d unconfirmed=%d "
            "startupActivationEntered=%d confirmed=%d settling=%d stabilizing=%d) — forwarding real suspend instead "
            "of treating it as stale startup churn",
            source ? source : "runtime-state", startupWindowActive ? 1 : 0, hadFSRFGPhase ? 1 : 0,
            explicitSetOptionsActivationForCurrentComeback ? 1 : 0, safePostFSRBootstrapPath ? 1 : 0,
            startupActivationPending ? 1 : 0, postSLActiveButUnconfirmed ? 1 : 0,
            postSLStartupActivationEntered ? 1 : 0, postSLConfirmedRendering ? 1 : 0,
            postSLConfirmedButStartupSettling ? 1 : 0, postSLConfirmedButRuntimeStateStabilizing ? 1 : 0);
    }

}


void MarkStartupProtectedOffChurnObserved(const char* source,  bool postSLConfirmedRendering, 
                                          bool postSLConfirmedButStartupSettling, 
                                          bool postSLConfirmedButRuntimeStateStabilizing) {


    const bool wasPending = streamline_hook_g_StartupProtectedOffChurnNeedsActiveProof.exchange(true, std::memory_order_acq_rel);
    const uint32_t previousProof = streamline_hook_g_StartupProtectedOffChurnActiveProofCount.exchange(0, std::memory_order_acq_rel);
    if (!wasPending || previousProof > 0) {
        static std::atomic<int> s_churnLogCount{0};
        const int logCount = s_churnLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 100) == 0) {
            HookLogImportant(
                "Streamline Hook: Startup-protected OFF churn requires fresh active proof before accepting disable "
                "(source=%s previousProof=%u required=%u confirmed=%d settling=%d stabilizing=%d)",
                source ? source : "runtime-state", previousProof,
                ce::streamline_runtime_policy::GetStartupProtectedOffChurnActiveProofUpdateThreshold(),
                postSLConfirmedRendering ? 1 : 0, postSLConfirmedButStartupSettling ? 1 : 0,
                postSLConfirmedButRuntimeStateStabilizing ? 1 : 0);
        }
    }

}


void MarkStartupProtectedActiveRuntimeProof(const char* source,  int multiplier) {


    if (!streamline_hook_g_StartupProtectedOffChurnNeedsActiveProof.load(std::memory_order_acquire)) {
        return;
    }

    const uint32_t previousProof = streamline_hook_g_StartupProtectedOffChurnActiveProofCount.load(std::memory_order_acquire);
    if (ce::streamline_runtime_policy::HasStartupProtectedOffChurnActiveProof(previousProof)) {
        return;
    }

    const uint32_t newProof = streamline_hook_g_StartupProtectedOffChurnActiveProofCount.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (ce::streamline_runtime_policy::HasStartupProtectedOffChurnActiveProof(newProof)) {
        const bool wasPending = streamline_hook_g_StartupProtectedOffChurnNeedsActiveProof.exchange(false, std::memory_order_acq_rel);
        if (wasPending) {
            HookLogImportant(
                "Streamline Hook: Startup-protected OFF churn quiet proof reached "
                "(source=%s activeProof=%u required=%u multiplier=%dx) — future OFF edges may be accepted",
                source ? source : "runtime-state", newProof,
                ce::streamline_runtime_policy::GetStartupProtectedOffChurnActiveProofUpdateThreshold(), multiplier);
        }
    } else {
        static std::atomic<int> s_activeProofLogCount{0};
        const int logCount = s_activeProofLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 100) == 0) {
            HookLogImportant(
                "Streamline Hook: Startup-protected OFF churn active proof progress "
                "(source=%s activeProof=%u/%u multiplier=%dx)",
                source ? source : "runtime-state", newProof,
                ce::streamline_runtime_policy::GetStartupProtectedOffChurnActiveProofUpdateThreshold(), multiplier);
        }
    }

}


bool IsStartupProtectedOffChurnAwaitingActiveProof(bool startupProtectedComebackProof,  bool postSLConfirmedRendering, 
                                                   bool postSLConfirmedButStartupSettling) {


    return ce::streamline_runtime_policy::ShouldKeepStartupProtectedOffChurnDeferredUntilActiveProof(
        streamline_hook_g_StartupProtectedOffChurnNeedsActiveProof.load(std::memory_order_acquire),
        streamline_hook_g_StartupProtectedOffChurnActiveProofCount.load(std::memory_order_acquire), startupProtectedComebackProof,
        postSLConfirmedRendering, postSLConfirmedButStartupSettling);

}
