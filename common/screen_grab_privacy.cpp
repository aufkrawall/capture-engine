#include "screen_grab_privacy.h"

#include <algorithm>

namespace ce::screen_grab_privacy {

namespace {

bool IsSupportedBlackFormat(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_R10G10B10A2_UNORM ||
           format == DXGI_FORMAT_R16G16B16A16_FLOAT;
}

bool TryClassifyWindowFullscreenLike(HWND hwnd, HMONITOR* classifiedMonitor, bool* fullscreenLike) {
    if (classifiedMonitor) {
        *classifiedMonitor = nullptr;
    }
    if (fullscreenLike) {
        *fullscreenLike = false;
    }
    if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd)) {
        return false;
    }

    const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo = {sizeof(MONITORINFO)};
    if (!monitor || !GetMonitorInfo(monitor, &monitorInfo)) {
        return false;
    }

    RECT windowRect = {};
    RECT clientRect = {};
    const bool haveWindowRect = GetWindowRect(hwnd, &windowRect) != FALSE;
    const bool haveClientRect = GetWindowClientRectInScreen(hwnd, clientRect);
    if (!haveWindowRect && !haveClientRect) {
        return false;
    }
    if (classifiedMonitor) {
        *classifiedMonitor = monitor;
    }
    if (fullscreenLike) {
        *fullscreenLike =
            IsFullscreenGeometry(haveWindowRect ? &windowRect : nullptr, haveClientRect ? &clientRect : nullptr,
                                 monitorInfo.rcMonitor);
    }
    return true;
}

}  // namespace

bool RectNearlyMatches(const RECT& lhs, const RECT& rhs, LONG tolerance) {
    const auto absDiff = [](LONG a, LONG b) -> LONG { return (a >= b) ? (a - b) : (b - a); };
    return absDiff(lhs.left, rhs.left) <= tolerance && absDiff(lhs.top, rhs.top) <= tolerance &&
           absDiff(lhs.right, rhs.right) <= tolerance && absDiff(lhs.bottom, rhs.bottom) <= tolerance;
}

bool IsFullscreenGeometry(const RECT* windowRect, const RECT* clientRect, const RECT& monitorRect, LONG tolerance) {
    return (windowRect && RectNearlyMatches(*windowRect, monitorRect, tolerance)) ||
           (clientRect && RectNearlyMatches(*clientRect, monitorRect, tolerance));
}

bool GetWindowClientRectInScreen(HWND hwnd, RECT& rect) {
    RECT clientRect = {};
    if (!GetClientRect(hwnd, &clientRect)) {
        return false;
    }

    POINT topLeft = {clientRect.left, clientRect.top};
    POINT bottomRight = {clientRect.right, clientRect.bottom};
    if (!ClientToScreen(hwnd, &topLeft) || !ClientToScreen(hwnd, &bottomRight)) {
        return false;
    }

    rect = {topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
    return true;
}

HWND NormalizeRootWindow(HWND hwnd) {
    if (!hwnd) {
        return nullptr;
    }
    const HWND root = GetAncestor(hwnd, GA_ROOT);
    return root ? root : hwnd;
}

bool IsWindowFullscreenLike(HWND hwnd) {
    bool fullscreenLike = false;
    return TryClassifyWindowFullscreenLike(hwnd, nullptr, &fullscreenLike) && fullscreenLike;
}

FullscreenFocusSnapshot CaptureStableFullscreenFocus() {
    FullscreenFocusSnapshot snapshot;
    const HWND firstForeground = NormalizeRootWindow(GetForegroundWindow());
    if (!firstForeground) {
        return snapshot;
    }

    HMONITOR monitor = nullptr;
    bool fullscreenLike = false;
    const bool classificationReliable = TryClassifyWindowFullscreenLike(firstForeground, &monitor, &fullscreenLike);
    const HWND secondForeground = NormalizeRootWindow(GetForegroundWindow());
    snapshot.foregroundRoot = firstForeground;
    snapshot.monitor = monitor;
    snapshot.stable = firstForeground == secondForeground && monitor != nullptr;
    snapshot.fullscreenLike = snapshot.stable && fullscreenLike;
    snapshot.classificationReliable = snapshot.stable && classificationReliable;
    return snapshot;
}

bool IsCaptureTargetValid(HWND targetWindow, HMONITOR targetMonitor) {
    if ((targetWindow != nullptr) == (targetMonitor != nullptr)) {
        return false;
    }
    if (targetWindow) {
        return IsWindow(NormalizeRootWindow(targetWindow)) != FALSE;
    }
    MONITORINFO monitorInfo = {sizeof(MONITORINFO)};
    return GetMonitorInfo(targetMonitor, &monitorInfo) != FALSE;
}

bool SnapshotMatchesCaptureTarget(const FullscreenFocusSnapshot& snapshot, HWND targetWindow, HMONITOR targetMonitor) {
    if (!snapshot.stable || !snapshot.classificationReliable || !snapshot.fullscreenLike ||
        !snapshot.foregroundRoot || !snapshot.monitor || (targetWindow != nullptr) == (targetMonitor != nullptr)) {
        return false;
    }
    if (targetWindow) {
        return snapshot.foregroundRoot == NormalizeRootWindow(targetWindow);
    }
    return targetMonitor && snapshot.monitor == targetMonitor;
}

void FocusPrivacyGate::Reset(bool enabled) {
    enabled_ = enabled;
    focusAccepted_ = false;
    blackoutActive_ = false;
    waitingForSafeFrame_ = enabled;
    lastOutputWasBlack_ = false;
    safeFrameThresholdQpc_ = 0;
    blackoutEntries_ = 0;
    blackFrames_ = 0;
    ambiguousObservations_ = 0;
    resumeWaitMaxQpc_ = 0;
}

void FocusPrivacyGate::ResetTarget() {
    focusAccepted_ = false;
    blackoutActive_ = false;
    waitingForSafeFrame_ = enabled_;
    lastOutputWasBlack_ = false;
    safeFrameThresholdQpc_ = 0;
}

GateDecision FocusPrivacyGate::Evaluate(bool activeScreenGrab, bool reliableFocusObservation,
                                        bool matchingFullscreenFocus, int64_t observationQpc, bool hasFreshFrame,
                                        int64_t freshFrameQpc) {
    GateDecision decision;
    if (!enabled_ || !activeScreenGrab) {
        decision.useBlackFrame = false;
        return decision;
    }

    if (!reliableFocusObservation || !matchingFullscreenFocus) {
        if (!reliableFocusObservation) {
            ++ambiguousObservations_;
        }
        if (!blackoutActive_) {
            ++blackoutEntries_;
            decision.enteredBlackout = true;
        }
        focusAccepted_ = false;
        blackoutActive_ = true;
        waitingForSafeFrame_ = true;
        safeFrameThresholdQpc_ = 0;
        decision.useBlackFrame = true;
        decision.waitingForSafeFrame = true;
        return decision;
    }

    if (!focusAccepted_) {
        focusAccepted_ = true;
        if (!blackoutActive_) {
            ++blackoutEntries_;
            decision.enteredBlackout = true;
        }
        blackoutActive_ = true;
        waitingForSafeFrame_ = true;
        safeFrameThresholdQpc_ = std::max<int64_t>(1, observationQpc);
        decision.focusReacquired = true;
    }

    if (waitingForSafeFrame_) {
        if (!hasFreshFrame || freshFrameQpc < safeFrameThresholdQpc_) {
            decision.useBlackFrame = true;
            decision.waitingForSafeFrame = true;
            return decision;
        }
        const int64_t resumeWaitQpc = observationQpc - safeFrameThresholdQpc_;
        resumeWaitMaxQpc_ =
            std::max<uint64_t>(resumeWaitMaxQpc_, static_cast<uint64_t>(std::max<int64_t>(0, resumeWaitQpc)));
        waitingForSafeFrame_ = false;
        blackoutActive_ = false;
        decision.exitedBlackout = true;
    }

    decision.useBlackFrame = false;
    return decision;
}

void FocusPrivacyGate::CommitOutput(bool blackFrame) {
    lastOutputWasBlack_ = blackFrame;
    if (blackFrame) {
        ++blackFrames_;
    }
}

BlackFrameTextureCache::~BlackFrameTextureCache() {
    Reset();
}

bool BlackFrameTextureCache::Prepare(ID3D11Texture2D* referenceTexture) {
    if (!referenceTexture) {
        return false;
    }

    D3D11_TEXTURE2D_DESC referenceDesc = {};
    referenceTexture->GetDesc(&referenceDesc);
    ID3D11Device* referenceDevice = nullptr;
    referenceTexture->GetDevice(&referenceDevice);
    if (!referenceDevice) {
        return false;
    }

    const bool cacheMatches = device_ == referenceDevice && texture_ && width_ == referenceDesc.Width &&
                              height_ == referenceDesc.Height && format_ == referenceDesc.Format;
    if (cacheMatches) {
        referenceDevice->Release();
        return true;
    }
    if (!IsSupportedBlackFormat(referenceDesc.Format) || referenceDesc.Width == 0 || referenceDesc.Height == 0) {
        referenceDevice->Release();
        return false;
    }

    Reset();
    D3D11_TEXTURE2D_DESC blackDesc = {};
    blackDesc.Width = referenceDesc.Width;
    blackDesc.Height = referenceDesc.Height;
    blackDesc.MipLevels = 1;
    blackDesc.ArraySize = 1;
    blackDesc.Format = referenceDesc.Format;
    blackDesc.SampleDesc.Count = 1;
    blackDesc.Usage = D3D11_USAGE_DEFAULT;
    blackDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    ID3D11Texture2D* blackTexture = nullptr;
    ID3D11RenderTargetView* blackRtv = nullptr;
    ID3D11DeviceContext* context = nullptr;
    HRESULT hr = referenceDevice->CreateTexture2D(&blackDesc, nullptr, &blackTexture);
    if (SUCCEEDED(hr)) {
        hr = referenceDevice->CreateRenderTargetView(blackTexture, nullptr, &blackRtv);
    }
    if (SUCCEEDED(hr)) {
        referenceDevice->GetImmediateContext(&context);
        if (!context) {
            hr = E_FAIL;
        }
    }
    if (SUCCEEDED(hr)) {
        const FLOAT opaqueBlack[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        context->ClearRenderTargetView(blackRtv, opaqueBlack);
        device_ = referenceDevice;
        texture_ = blackTexture;
        width_ = blackDesc.Width;
        height_ = blackDesc.Height;
        format_ = blackDesc.Format;
        ++generation_;
    } else {
        referenceDevice->Release();
        if (blackTexture) {
            blackTexture->Release();
        }
    }
    if (context) {
        context->Release();
    }
    if (blackRtv) {
        blackRtv->Release();
    }
    return SUCCEEDED(hr);
}

void BlackFrameTextureCache::Reset() {
    if (texture_) {
        texture_->Release();
        texture_ = nullptr;
    }
    if (device_) {
        device_->Release();
        device_ = nullptr;
    }
    width_ = 0;
    height_ = 0;
    format_ = DXGI_FORMAT_UNKNOWN;
}

}  // namespace ce::screen_grab_privacy
