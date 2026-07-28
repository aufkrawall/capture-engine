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
    void ResetMediaRepeatCache();

    FocusPrivacyGate gate_;
    BlackFrameTextureCache blackTexture_;
    bool repeatCacheIsBlack_ = false;
};

}  // namespace ce::screen_grab_privacy
