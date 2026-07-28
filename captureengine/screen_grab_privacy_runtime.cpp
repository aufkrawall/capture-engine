#include "screen_grab_privacy_runtime.h"

#include "../common/cursor_capture_state.h"
#include "../common/logging.h"
#include "mediaengine_loader.h"

namespace ce::screen_grab_privacy {

void ScreenGrabPrivacyRuntime::Reset(bool enabled) {
    gate_.Reset(enabled);
    blackTexture_.Reset();
    repeatCacheIsBlack_ = false;
}

void ScreenGrabPrivacyRuntime::ResetSource() {
    gate_.ResetTarget();
    blackTexture_.Reset();
    repeatCacheIsBlack_ = false;
    ResetMediaRepeatCache();
}

bool ScreenGrabPrivacyRuntime::PrepareTexture(ID3D11Texture2D* referenceTexture) {
    return !gate_.IsEnabled() || blackTexture_.Prepare(referenceTexture);
}

GateDecision ScreenGrabPrivacyRuntime::Evaluate(bool activeScreenGrab, HWND targetWindow, HMONITOR targetMonitor,
                                                bool stableCaptureTarget, const FullscreenFocusSnapshot& focus,
                                                bool hasFreshFrame, int64_t freshFrameQpc) {
    if (!gate_.IsEnabled()) {
        return {};
    }

    LARGE_INTEGER observationQpc = {};
    QueryPerformanceCounter(&observationQpc);
    const bool reliableObservation =
        stableCaptureTarget && focus.stable && focus.classificationReliable &&
        IsCaptureTargetValid(targetWindow, targetMonitor);
    const bool matchingFullscreen =
        reliableObservation && SnapshotMatchesCaptureTarget(focus, targetWindow, targetMonitor);
    const GateDecision decision =
        gate_.Evaluate(activeScreenGrab, reliableObservation, matchingFullscreen, observationQpc.QuadPart,
                       hasFreshFrame, freshFrameQpc);
    if (decision.enteredBlackout) {
        LogInfo(
            "[PrivacyBlackout] Entered: reason=%s target=%s; output remains opaque black and audio/PTS continue "
            "unchanged",
            reliableObservation ? "no-matching-fullscreen-focus" : "ambiguous-focus-or-target",
            targetWindow ? "window" : (targetMonitor ? "monitor" : "unresolved"));
    }
    if (decision.focusReacquired) {
        LogInfo(
            "[PrivacyBlackout] Matching fullscreen focus reacquired; waiting for sourceQpc >= %lld before revealing "
            "pixels",
            static_cast<long long>(gate_.SafeFrameThresholdQpc()));
    }
    if (decision.exitedBlackout) {
        LogInfo(
            "[PrivacyBlackout] Exited on a post-focus source frame: sourceQpc=%lld thresholdQpc=%lld (audio/PTS "
            "unchanged)",
            static_cast<long long>(freshFrameQpc), static_cast<long long>(gate_.SafeFrameThresholdQpc()));
    }
    return decision;
}

bool ScreenGrabPrivacyRuntime::SubmitBlack(ID3D11Texture2D* referenceTexture, bool isHdr, int64_t mediaTimestampQpc,
                                           int64_t scheduledQpc, int64_t timelineElapsedUs,
                                           bool useExplicitCfrTimeline) {
    const uint64_t previousBlackGeneration = blackTexture_.Generation();
    if (!blackTexture_.Prepare(referenceTexture)) {
        LogError("[PrivacyBlackout] GPU opaque-black texture preparation failed");
        ResetMediaRepeatCache();
        return false;
    }
    if (previousBlackGeneration != 0 && previousBlackGeneration != blackTexture_.Generation()) {
        gate_.ResetTarget();
        repeatCacheIsBlack_ = false;
        ResetMediaRepeatCache();
    }

    const ce::cursor::CaptureState hiddenCursor;
    bool succeeded =
        MediaEngine_ProcessFrameD3D11 &&
        MediaEngine_ProcessFrameD3D11(blackTexture_.Get(), mediaTimestampQpc, blackTexture_.Width(),
                                      blackTexture_.Height(), isHdr, 0, 0, timelineElapsedUs,
                                      &hiddenCursor);
    if (!succeeded && repeatCacheIsBlack_ && gate_.LastOutputWasBlack() && MediaEngine_CanRepeatLastFrame &&
        MediaEngine_CanRepeatLastFrame()) {
        succeeded = useExplicitCfrTimeline && MediaEngine_RepeatLastFrameWithTimeline
                        ? MediaEngine_RepeatLastFrameWithTimeline(scheduledQpc, timelineElapsedUs, &hiddenCursor)
                        : (MediaEngine_RepeatLastFrame && MediaEngine_RepeatLastFrame(scheduledQpc, &hiddenCursor));
    }
    if (!succeeded) {
        LogError("[PrivacyBlackout] GPU opaque-black frame encode failed");
        ResetMediaRepeatCache();
        return false;
    }

    gate_.CommitOutput(true);
    repeatCacheIsBlack_ = true;
    return true;
}

void ScreenGrabPrivacyRuntime::CommitRealOutput() {
    gate_.CommitOutput(false);
    repeatCacheIsBlack_ = false;
}

void ScreenGrabPrivacyRuntime::CommitRepeatOutput() {
    gate_.CommitOutput(repeatCacheIsBlack_);
}

void ScreenGrabPrivacyRuntime::LogSummary(int64_t qpcFrequency) const {
    if (!gate_.IsEnabled()) {
        return;
    }
    const int64_t resumeWaitMaxUs =
        qpcFrequency > 0 ? static_cast<int64_t>(gate_.ResumeWaitMaxQpc() * 1000000ull /
                                               static_cast<uint64_t>(qpcFrequency))
                         : 0;
    LogInfo(
        "[PrivacyBlackout SUMMARY] entries=%llu blackFrames=%llu ambiguousObservations=%llu resumeWaitMaxUs=%lld "
        "finalOutputBlack=%d waitingForSafeFrame=%d",
        static_cast<unsigned long long>(gate_.BlackoutEntries()),
        static_cast<unsigned long long>(gate_.BlackFrames()),
        static_cast<unsigned long long>(gate_.AmbiguousObservations()), static_cast<long long>(resumeWaitMaxUs),
        gate_.LastOutputWasBlack() ? 1 : 0, gate_.IsWaitingForSafeFrame() ? 1 : 0);
}

void ScreenGrabPrivacyRuntime::ResetMediaRepeatCache() {
    if (MediaEngine_ResetRepeatFrameCache) {
        MediaEngine_ResetRepeatFrameCache();
    }
    repeatCacheIsBlack_ = false;
}

}  // namespace ce::screen_grab_privacy
