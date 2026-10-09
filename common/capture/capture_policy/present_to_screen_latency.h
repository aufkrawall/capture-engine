#pragma once

#include <stddef.h>
#include <stdint.h>
#include <algorithm>
#include <vector>

#include "common/ipc/display_timing_shared.h"

// Present-to-screen latency of the followed process, measured from the display timing ring: the ETW reducer
// publishes, per displayed transition, the runtime Present start and the kernel screen-change time. The gap is
// how long the application's frames wait in the flip queue and the compositor before they are shown, which is
// the part of a screen-grab video timestamp (it is a composition/screen time, not the Present call) that the
// audio-side capture latency does not contain.

namespace ce::capture_policy {

struct PresentToScreenLatency {
    bool valid = false;
    int64_t medianUs = 0;
    int64_t minUs = 0;
    int64_t maxUs = 0;
    uint32_t samples = 0;
};

constexpr uint32_t kPresentToScreenMinSamples = 24;
constexpr uint32_t kPresentToScreenDefaultWindow = 240;
// A Present to screen gap above this is a stall or a mismatched pair, not the steady-state queue latency.
constexpr int64_t kPresentToScreenPlausibleMaxUs = 250000;

inline PresentToScreenLatency EstimatePresentToScreenLatency(
    const SharedDisplayTiming& timing, uint32_t window = kPresentToScreenDefaultWindow,
    int64_t plausibleMaxUs = kPresentToScreenPlausibleMaxUs) {
    PresentToScreenLatency result;
    if (timing.GetStatus() != DisplayTimingStatus::Active || window == 0)
        return result;
    const uint64_t writeSequence = timing.writeSequence.load(std::memory_order_acquire);
    if (writeSequence == 0)
        return result;
    const uint64_t span = std::min<uint64_t>({static_cast<uint64_t>(window), writeSequence,
                                              static_cast<uint64_t>(DISPLAY_TIMING_RING_SIZE) - 1});
    std::vector<int64_t> gapsUs;
    gapsUs.reserve(static_cast<size_t>(span));
    for (uint64_t sequence = writeSequence - span + 1; sequence <= writeSequence; ++sequence) {
        int64_t screenTimeUs = 0;
        int64_t presentStartTimeUs = 0;
        bool screenTimeResolved = false;
        if (!timing.Read(sequence, screenTimeUs, presentStartTimeUs, screenTimeResolved))
            continue;
        if (!screenTimeResolved || presentStartTimeUs <= 0 || screenTimeUs <= presentStartTimeUs)
            continue;
        const int64_t gapUs = screenTimeUs - presentStartTimeUs;
        if (gapUs > plausibleMaxUs)
            continue;
        gapsUs.push_back(gapUs);
    }
    if (gapsUs.size() < kPresentToScreenMinSamples)
        return result;
    std::sort(gapsUs.begin(), gapsUs.end());
    result.valid = true;
    result.samples = static_cast<uint32_t>(gapsUs.size());
    result.minUs = gapsUs.front();
    result.maxUs = gapsUs.back();
    result.medianUs = gapsUs[gapsUs.size() / 2];
    return result;
}

// The audio-side capture latency (render -> loopback) delays video content so it lines up with late audio. A
// screen-grab video timestamp is a composition/screen time and already trails the application's Present by the
// flip-queue latency, so that part must not be delayed twice: the content delay is the probe latency minus the
// measured present-to-screen latency, never negative. Without a valid measurement (no hook, display timing not
// active yet) the probe latency stands unchanged.
inline double ComputeScreenGrabContentDelayMs(double probeLatencyMs, const PresentToScreenLatency& presentToScreen) {
    if (probeLatencyMs <= 0.0)
        return 0.0;
    if (!presentToScreen.valid)
        return probeLatencyMs;
    return std::max(0.0, probeLatencyMs - static_cast<double>(presentToScreen.medianUs) / 1000.0);
}

// How much of the probe latency the screen-grab path takes off: the media engine anchors the audio timeline
// from its per-source latency while the encoder session delays video from the same value, so both sides must
// subtract the same amount.
inline double ComputeScreenGrabLatencyReductionMs(double probeLatencyMs, const PresentToScreenLatency& presentToScreen) {
    return std::max(0.0, probeLatencyMs - ComputeScreenGrabContentDelayMs(probeLatencyMs, presentToScreen));
}

}  // namespace ce::capture_policy
