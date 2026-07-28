#pragma once

#include <d3d11.h>
#include <windows.h>
#include <cstdint>

namespace ce::screen_grab_privacy {

constexpr LONG kFullscreenTolerancePx = 8;

struct FullscreenFocusSnapshot {
    HWND foregroundRoot = nullptr;
    HMONITOR monitor = nullptr;
    bool stable = false;
    bool fullscreenLike = false;
    bool classificationReliable = false;
};

bool RectNearlyMatches(const RECT& lhs, const RECT& rhs, LONG tolerance = kFullscreenTolerancePx);
bool IsFullscreenGeometry(const RECT* windowRect, const RECT* clientRect, const RECT& monitorRect,
                          LONG tolerance = kFullscreenTolerancePx);
bool GetWindowClientRectInScreen(HWND hwnd, RECT& rect);
HWND NormalizeRootWindow(HWND hwnd);
bool IsWindowFullscreenLike(HWND hwnd);
FullscreenFocusSnapshot CaptureStableFullscreenFocus();
bool IsCaptureTargetValid(HWND targetWindow, HMONITOR targetMonitor);
bool SnapshotMatchesCaptureTarget(const FullscreenFocusSnapshot& snapshot, HWND targetWindow, HMONITOR targetMonitor);

struct GateDecision {
    bool useBlackFrame = false;
    bool enteredBlackout = false;
    bool focusReacquired = false;
    bool exitedBlackout = false;
    bool waitingForSafeFrame = false;
};

class FocusPrivacyGate {
public:
    void Reset(bool enabled);
    void ResetTarget();
    GateDecision Evaluate(bool activeScreenGrab, bool reliableFocusObservation, bool matchingFullscreenFocus,
                          int64_t observationQpc, bool hasFreshFrame, int64_t freshFrameQpc);
    void CommitOutput(bool blackFrame);

    bool IsEnabled() const {
        return enabled_;
    }
    bool LastOutputWasBlack() const {
        return lastOutputWasBlack_;
    }
    bool IsWaitingForSafeFrame() const {
        return waitingForSafeFrame_;
    }
    int64_t SafeFrameThresholdQpc() const {
        return safeFrameThresholdQpc_;
    }
    uint64_t BlackoutEntries() const {
        return blackoutEntries_;
    }
    uint64_t BlackFrames() const {
        return blackFrames_;
    }
    uint64_t AmbiguousObservations() const {
        return ambiguousObservations_;
    }
    uint64_t ResumeWaitMaxQpc() const {
        return resumeWaitMaxQpc_;
    }

private:
    bool enabled_ = false;
    bool focusAccepted_ = false;
    bool blackoutActive_ = false;
    bool waitingForSafeFrame_ = false;
    bool lastOutputWasBlack_ = false;
    int64_t safeFrameThresholdQpc_ = 0;
    uint64_t blackoutEntries_ = 0;
    uint64_t blackFrames_ = 0;
    uint64_t ambiguousObservations_ = 0;
    uint64_t resumeWaitMaxQpc_ = 0;
};

class BlackFrameTextureCache {
public:
    BlackFrameTextureCache() = default;
    ~BlackFrameTextureCache();
    BlackFrameTextureCache(const BlackFrameTextureCache&) = delete;
    BlackFrameTextureCache& operator=(const BlackFrameTextureCache&) = delete;

    bool Prepare(ID3D11Texture2D* referenceTexture);
    void Reset();

    ID3D11Texture2D* Get() const {
        return texture_;
    }
    uint32_t Width() const {
        return width_;
    }
    uint32_t Height() const {
        return height_;
    }
    DXGI_FORMAT Format() const {
        return format_;
    }
    uint64_t Generation() const {
        return generation_;
    }

private:
    ID3D11Device* device_ = nullptr;
    ID3D11Texture2D* texture_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
    uint64_t generation_ = 0;
};

}  // namespace ce::screen_grab_privacy
