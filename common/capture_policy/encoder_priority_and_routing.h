#pragma once

#include <stddef.h>
#include <stdint.h>
#include <algorithm>

#include "cfr_startup.h"

// Adaptive encoder GPU priority, overlay warnings, and capture-backend routing.

namespace ce::capture_policy {

inline bool ShouldRaiseAdaptiveEncoderGpuPriority(double encodeMs, double frameIntervalMs,
                                                  double raiseBudgetRatio = kEncoderGpuPriorityRaiseBudgetRatio) {
    return encodeMs > 0.0 && frameIntervalMs > 0.0 && encodeMs >= frameIntervalMs * raiseBudgetRatio;
}

inline bool ShouldRestoreNeutralEncoderGpuPriority(double encodeMs, double frameIntervalMs,
                                                   double restoreBudgetRatio = kEncoderGpuPriorityRestoreBudgetRatio) {
    return frameIntervalMs > 0.0 && encodeMs >= 0.0 && encodeMs <= frameIntervalMs * restoreBudgetRatio;
}

inline bool IsAdaptiveEncoderGpuPriorityPressureActive(double encodeMs, double frameIntervalMs,
                                                       bool encoderPressureActive) {
    return encoderPressureActive || ShouldRaiseAdaptiveEncoderGpuPriority(encodeMs, frameIntervalMs);
}

inline bool ShouldResetAdaptiveEncoderGpuPriorityPressure(double encodeMs, double frameIntervalMs,
                                                          bool encoderPressureActive = false) {
    return !encoderPressureActive && ShouldRestoreNeutralEncoderGpuPriority(encodeMs, frameIntervalMs);
}

inline bool IsWgcCaptureLimitedForOverlay(uint32_t captureHealthFlags) {
    const uint32_t captureLimitedFlags = kWgcCaptureHealthFlagSourceStarved | kWgcCaptureHealthFlagSchedulerLimited;
    return (captureHealthFlags & captureLimitedFlags) != 0;
}

inline uint32_t SelectWgcOverlayWarningKind(uint32_t overloadFlags, uint32_t captureHealthFlags) {
    if (IsWgcCaptureLimitedForOverlay(captureHealthFlags)) {
        return kOverlayWarningNone;
    }

    if ((overloadFlags & kEncoderOverloadFlagEncoder) != 0) {
        return kOverlayWarningEncoderOverload;
    }

    return kOverlayWarningNone;
}

inline uint32_t GetEncoderBudgetUtilizationPermille(double encodeMs, double frameIntervalMs) {
    if (encodeMs <= 0.0 || frameIntervalMs <= 0.0) {
        return 0u;
    }

    const double utilizationPermille = (encodeMs * 1000.0) / frameIntervalMs;
    const double clampedPermille = std::clamp(utilizationPermille, 0.0, 1000000.0);
    return static_cast<uint32_t>(clampedPermille + 0.5);
}

inline bool IsEncoderTooSlowForTargetFps(double encodeMs, double frameIntervalMs, uint32_t targetFps,
                                         double toleranceFps = 0.5) {
    if (frameIntervalMs <= 0.0 || targetFps == 0u) {
        return false;
    }

    const double sustainableFps = GetEncoderSustainableOutputFps(encodeMs);
    return sustainableFps > 0.0 && sustainableFps + toleranceFps < static_cast<double>(targetFps);
}

struct WgcAdaptiveTelemetry {
    uint32_t outputFps = 0;
    uint32_t recentDeliveredFps = 0;
    uint32_t recentDeliveredMin250Fps = 0;
    uint32_t recentDeliveredMin500Fps = 0;
    uint32_t recentInputMin250Fps = 0;
    uint32_t recentInputMin500Fps = 0;
    uint32_t averageJitterUs = 0;
    uint32_t emptyTickPermille = 0;
    uint32_t bufferedWgcFrames = 0;
    uint32_t encoderQueueDepth = 0;
    double duplicateRatio = 0.0;
};

inline bool IsWgcActiveDelayRecoverableJitter(const WgcAdaptiveTelemetry& telemetry) {
    const uint32_t frameIntervalUs = GetWgcFrameIntervalUs(telemetry.outputFps);
    if (frameIntervalUs == 0 || telemetry.averageJitterUs == 0) {
        return false;
    }

    return static_cast<uint64_t>(telemetry.averageJitterUs) * 1000ull >=
           static_cast<uint64_t>(frameIntervalUs) * kWgcActiveDelayRecoverableJitterPermille;
}

inline bool IsWgcActiveDelaySourceLimitedJitter(const WgcAdaptiveTelemetry& telemetry) {
    const uint32_t frameIntervalUs = GetWgcFrameIntervalUs(telemetry.outputFps);
    if (frameIntervalUs == 0 || telemetry.averageJitterUs == 0) {
        return false;
    }

    if (static_cast<uint64_t>(telemetry.averageJitterUs) * 1000ull <
        static_cast<uint64_t>(frameIntervalUs) * kWgcActiveDelaySourceLimitedJitterPermille) {
        return false;
    }

    return telemetry.emptyTickPermille >= kWgcLowSourceExitEmptyTickPermille ||
           telemetry.recentDeliveredMin250Fps < telemetry.outputFps ||
           telemetry.recentInputMin250Fps < telemetry.outputFps;
}

inline bool ShouldUseNativeWgcCursorCapture(bool /*recordingCursorRequested*/) {
    // Native WGC cursor capture can force the live cursor out of the hardware
    // plane and perturb flip-model/VRR promotion. Keep the user's cursor in the
    // recording by compositing it in the encoder instead.
    return false;
}

inline bool ShouldPreferForegroundFullscreenWindowForAutoWgc(bool autoCaptureConfig, bool explicitInjectConfig,
                                                             bool injectWhitelisted, bool hasSourcePid,
                                                             bool hasMatchedConfiguredWgcWindow, bool foregroundUsable,
                                                             bool foregroundFullscreenLike) {
    return autoCaptureConfig && !explicitInjectConfig && !injectWhitelisted && !hasSourcePid &&
           !hasMatchedConfiguredWgcWindow && foregroundUsable && foregroundFullscreenLike;
}

// Backend priority for MONITOR-scope (desktop) capture targets.
// Auto priority is: inject (whitelisted game) > WGC window capture (configured window,
// source-PID window, foreground fullscreen window) > DXGI Desktop Duplication for the
// remaining pure desktop/monitor fallback. Duplication reads the composed desktop
// surface directly instead of standing up a WGC monitor item, so it is the preferred
// desktop-recording path when no window-scoped target exists. WGC monitor capture
// remains the fallback when duplication is unavailable (rotated output, cross-adapter
// output, protected content, unsupported format for the requested bit depth, or API
// failure). Explicit capture_method=wgc keeps WGC monitor capture; explicit
// capture_method=dxgi_dup always prefers duplication.
inline bool ShouldPreferDxgiDuplicationForMonitorCapture(bool explicitDxgiDupConfig, bool explicitWgcConfig,
                                                         bool autoCaptureConfig) {
    if (explicitDxgiDupConfig) {
        return true;
    }
    if (explicitWgcConfig) {
        return false;
    }
    return autoCaptureConfig;
}

// Returns bits per color channel for duplication surface formats; 0 for formats
// the pipeline does not process. The first acquired texture is authoritative:
// DXGI_OUTDUPL_DESC::ModeDesc.Format is only a display-mode hint.
inline uint32_t GetDxgiDuplicationSourceContentBits(uint32_t dxgiFormat) {
    constexpr uint32_t kFormatR16G16B16A16Float = 10;  // DXGI_FORMAT_R16G16B16A16_FLOAT
    constexpr uint32_t kFormatR10G10B10A2Unorm = 24;   // DXGI_FORMAT_R10G10B10A2_UNORM
    constexpr uint32_t kFormatB8G8R8A8Unorm = 87;      // DXGI_FORMAT_B8G8R8A8_UNORM
    if (dxgiFormat == kFormatR16G16B16A16Float) {
        return 16;
    }
    if (dxgiFormat == kFormatR10G10B10A2Unorm) {
        return 10;
    }
    if (dxgiFormat == kFormatB8G8R8A8Unorm) {
        return 8;
    }
    return 0;
}

inline bool IsAcceptableDxgiDuplicationFrameFormat(uint32_t dxgiFormat, bool requireHighPrecision, bool outputIsHdr) {
    constexpr uint32_t kFormatR16G16B16A16Float = 10;
    constexpr uint32_t kFormatR10G10B10A2Unorm = 24;
    constexpr uint32_t kFormatB8G8R8A8Unorm = 87;
    if (outputIsHdr) {
        return dxgiFormat == kFormatR16G16B16A16Float;
    }
    if (requireHighPrecision) {
        return dxgiFormat == kFormatR10G10B10A2Unorm || dxgiFormat == kFormatR16G16B16A16Float;
    }
    return dxgiFormat == kFormatB8G8R8A8Unorm || dxgiFormat == kFormatR10G10B10A2Unorm ||
           dxgiFormat == kFormatR16G16B16A16Float;
}

// An explicit DXGI + 10-bit request is a strict contract: silently switching
// backend or upconverting an 8-bit surface would make the configured guarantee
// unobservable. Auto mode may still use WGC as its compatibility fallback.
inline bool ShouldAllowWgcFallbackAfterDxgiFailure(bool explicitDxgiConfig, bool explicitTenBit) {
    return !(explicitDxgiConfig && explicitTenBit);
}

// Auto-mode backend choice for an UNHOOKED fullscreen-like game target (no inject).
// WGC capture sessions demote the live cursor from the hardware plane to
// DWM-composed (software) rendering — architectural WGC behavior since 2020,
// with no public API opt-out (IsCursorCaptureEnabled(false) only masks the
// cursor out of delivered frames). DXGI duplication never demotes the cursor
// (its API contract delivers the pointer as separate metadata). For fullscreen
// games, monitor-scope duplication content is equivalent to window-scope WGC
// content, so duplication is preferred to preserve the hardware cursor; WGC
// window capture remains the fallback (cross-adapter/rotated outputs) and the
// choice for genuinely windowed targets.
inline bool ShouldPreferDxgiDuplicationForFullscreenAutoTarget(bool autoCaptureConfig, bool explicitInjectConfig,
                                                               bool injectWhitelisted, bool targetFullscreenLike,
                                                               bool fullscreenPrefersDuplication) {
    return autoCaptureConfig && !explicitInjectConfig && !injectWhitelisted && targetFullscreenLike &&
           fullscreenPrefersDuplication;
}

}  // namespace ce::capture_policy
