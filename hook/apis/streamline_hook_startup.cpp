#include "streamline_hook_internal.h"


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
    const uint32_t previousProof = ce::streamline_runtime_policy::GetStartupProtectedOffChurnActiveProof(
        streamline_hook_g_StartupProtectedOffChurnActiveProofCount.exchange(0, std::memory_order_acq_rel),
        streamline_hook_g_StartupProtectedOffChurnActiveFrameCount.exchange(0, std::memory_order_acq_rel));
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
    const uint32_t previousProof = ce::streamline_runtime_policy::GetStartupProtectedOffChurnActiveProof(
        streamline_hook_g_StartupProtectedOffChurnActiveProofCount.exchange(0, std::memory_order_acq_rel),
        streamline_hook_g_StartupProtectedOffChurnActiveFrameCount.exchange(0, std::memory_order_acq_rel));
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

    if (ce::streamline_runtime_policy::HasStartupProtectedOffChurnActiveProof(GetStartupProtectedOffChurnActiveProof())) {
        return;
    }

    const uint32_t newUpdates = streamline_hook_g_StartupProtectedOffChurnActiveProofCount.fetch_add(1, std::memory_order_acq_rel) + 1;
    const uint32_t newProof = ce::streamline_runtime_policy::GetStartupProtectedOffChurnActiveProof(
        newUpdates, streamline_hook_g_StartupProtectedOffChurnActiveFrameCount.load(std::memory_order_acquire));
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
        GetStartupProtectedOffChurnActiveProof(), startupProtectedComebackProof,
        postSLConfirmedRendering, postSLConfirmedButStartupSettling);

}


uint32_t GetStartupProtectedOffChurnActiveProof() {
    return ce::streamline_runtime_policy::GetStartupProtectedOffChurnActiveProof(
        streamline_hook_g_StartupProtectedOffChurnActiveProofCount.load(std::memory_order_acquire),
        streamline_hook_g_StartupProtectedOffChurnActiveFrameCount.load(std::memory_order_acquire));
}


// Called for every forwarded PCL present-start marker, so the common case is one relaxed load.
void NoteStartupProtectedActiveTitleFrame(uint64_t frameId) {
    if (!streamline_hook_g_StartupProtectedOffChurnNeedsActiveProof.load(std::memory_order_acquire)) {
        return;
    }
    static std::atomic<uint64_t> s_lastCountedFrame{UINT64_MAX};
    const bool newTitleFrame = s_lastCountedFrame.exchange(frameId, std::memory_order_acq_rel) != frameId;
    if (!ce::streamline_runtime_policy::ShouldCountTitleFrameAsStartupProtectedActiveProof(
            true, newTitleFrame, DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire),
            HookIsPostSLOverlayConfirmedRendering())) {
        return;
    }
    if (ce::streamline_runtime_policy::HasStartupProtectedOffChurnActiveProof(GetStartupProtectedOffChurnActiveProof())) {
        return;
    }

    const uint32_t newFrames =
        streamline_hook_g_StartupProtectedOffChurnActiveFrameCount.fetch_add(1, std::memory_order_acq_rel) + 1;
    const uint32_t newProof = ce::streamline_runtime_policy::GetStartupProtectedOffChurnActiveProof(
        streamline_hook_g_StartupProtectedOffChurnActiveProofCount.load(std::memory_order_acquire), newFrames);
    if (ce::streamline_runtime_policy::HasStartupProtectedOffChurnActiveProof(newProof) &&
        streamline_hook_g_StartupProtectedOffChurnNeedsActiveProof.exchange(false, std::memory_order_acq_rel)) {
        HookLogImportant(
            "Streamline Hook: Startup-protected OFF churn quiet proof reached "
            "(source=title frames activeFrames=%u activeUpdates=%u required=%u frame=%llu) — future OFF edges may "
            "be accepted",
            newFrames, streamline_hook_g_StartupProtectedOffChurnActiveProofCount.load(std::memory_order_acquire),
            ce::streamline_runtime_policy::GetStartupProtectedOffChurnActiveProofUpdateThreshold(),
            static_cast<unsigned long long>(frameId));
    }
}
