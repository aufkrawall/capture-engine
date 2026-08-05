#pragma once

#include <windows.h>
#include <cstdint>
#include "../common/screen_grab_privacy.h"

namespace ce::screen_grab_privacy {

class ScreenGrabPrivacyRuntime {
public:
    void Reset(bool enabled);
    void ResetSource();
    bool PrepareTexture(ID3D11Texture2D* referenceTexture);
    // Warmup-phase focus tracking. Warmup frames are discarded, but the WGC/DXGI
    // look-ahead reservoir built during warmup survives the live handoff, so the
    // verified-focus interval must already be open when the first output frame is
    // selected. Produces no output decision.
    GateDecision ObserveWarmup(bool activeScreenGrab, HWND targetWindow, HMONITOR targetMonitor,
                               bool stableCaptureTarget, const FullscreenFocusSnapshot& focus);
    GateDecision Evaluate(bool activeScreenGrab, HWND targetWindow, HMONITOR targetMonitor, bool stableCaptureTarget,
                          const FullscreenFocusSnapshot& focus, bool hasFreshFrame, int64_t freshFrameQpc);
    bool SubmitBlack(ID3D11Texture2D* referenceTexture, bool isHdr, int64_t mediaTimestampQpc, int64_t scheduledQpc,
                     int64_t timelineElapsedUs, bool useExplicitCfrTimeline);
    void CommitRealOutput();
    void CommitRepeatOutput();
    void LogSummary(int64_t qpcFrequency) const;

    bool IsEnabled() const {
        return gate_.IsEnabled();
    }
    bool RepeatCacheIsBlack() const {
        return repeatCacheIsBlack_;
    }

private:
    struct FocusObservation {
        bool reliable = false;
        bool matchingFullscreen = false;
        int64_t observationQpc = 0;
    };

    static FocusObservation SampleFocusObservation(HWND targetWindow, HMONITOR targetMonitor, bool stableCaptureTarget,
                                                   const FullscreenFocusSnapshot& focus);
    void LogGateTransitions(const GateDecision& decision, const FocusObservation& observation, HWND targetWindow,
                            HMONITOR targetMonitor, const char* phase, bool hasFreshFrame, int64_t freshFrameQpc) const;
    void ResetMediaRepeatCache();

    FocusPrivacyGate gate_;
    BlackFrameTextureCache blackTexture_;
    bool repeatCacheIsBlack_ = false;
};

}  // namespace ce::screen_grab_privacy
