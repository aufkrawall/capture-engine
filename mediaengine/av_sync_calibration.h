#pragma once

// A/V self-calibration: pure, hardware-independent logic for measuring the real audio-vs-video
// content offset through CE's actual capture pipeline.
//
// The render->loopback audio probe (audio_latency_probe.h) is an audio-only proxy that
// under-measures the perceived A/V offset: it cannot see the WGC video-clock side and is measured
// at idle. This calibration instead emits, at the SAME QPC, a near-inaudible audio burst (captured
// via loopback) AND a visual flash (captured via WGC), then measures
//   offset = loopback audio qpcPosition - WGC frame SystemRelativeTime
// of the matching burst/flash. Because both markers fire at one QPC and are measured at their
// CAPTURED timestamps, the difference IS the A/V content offset CE encodes - through the exact same
// clocks - with no decomposition needed. This is the test harness / raw-offset-gate methodology
// internalized so it can run automatically and be cached per device.
//
// This header holds only the pure DSP/aggregation pieces unique to the video side (flash rising-edge
// detection) so they are unit-testable; it reuses the audio marker/median/cache logic from
// audio_latency_probe.h. The WASAPI/WGC/window orchestration lives in the .cpp glue.

#include <algorithm>
#include <cstdint>
#include <vector>

#include "audio_latency_probe.h"
#include "audio_time_utils.h"

namespace ce::audio {

// One captured WGC frame reduced to its marker-region luma and its content QPC (SystemRelativeTime,
// in 100-ns units to match loopback qpcPosition).
struct CalibrationVideoFrame {
    uint64_t qpc100ns = 0;
    double luma = 0.0;  // mean luma of the sampled marker region, normalized 0..1
};

// Detect dark->bright rising edges in a sequence of captured frames (hysteresis: cross above
// onThreshold having last been below offThreshold). Returns the content QPC of the first bright
// frame of each flash, in capture order. Used to recover the moment each white flash actually
// appeared in the WGC stream. Robust to flashes spanning multiple frames and to minor luma noise.
inline std::vector<uint64_t> DetectFlashRisingEdges(const std::vector<CalibrationVideoFrame>& frames,
                                                    double onThreshold = 0.6, double offThreshold = 0.3) {
    std::vector<uint64_t> edges;
    bool armed = true;  // ready to detect a rising edge once we've seen a dark frame
    for (const auto& f : frames) {
        if (armed && f.luma >= onThreshold) {
            edges.push_back(f.qpc100ns);
            armed = false;  // wait for it to go dark again before the next edge
        } else if (!armed && f.luma <= offThreshold) {
            armed = true;
        }
    }
    return edges;
}

// Given a scalar time series `values` (normalized ~0..1) with matching per-sample `qpc`, find each
// contiguous run that crosses above onThreshold (until it drops below offThreshold) and return the
// QPC at the CENTER of each run. Used for BOTH video luma (white flash) and audio marker-band power
// (tone burst): when the flash and the burst have the SAME duration and are triggered together,
// detecting the CENTER of each cancels the envelope/ramp bias on both sides, so the difference of
// the two centers is the pure A/V pipeline offset. Hysteresis (on/off thresholds) rejects noise.
inline std::vector<uint64_t> DetectHighRunCenters(const std::vector<double>& values, const std::vector<uint64_t>& qpc,
                                                  double onThreshold = 0.6, double offThreshold = 0.3) {
    std::vector<uint64_t> centers;
    const size_t n = std::min(values.size(), qpc.size());
    bool inRun = false;
    size_t runStart = 0;
    for (size_t i = 0; i < n; ++i) {
        if (!inRun && values[i] >= onThreshold) {
            inRun = true;
            runStart = i;
        } else if (inRun && values[i] <= offThreshold) {
            centers.push_back(qpc[(runStart + (i - 1)) / 2]);
            inRun = false;
        }
    }
    if (inRun && n > 0) {
        centers.push_back(qpc[(runStart + (n - 1)) / 2]);
    }
    return centers;
}

// Normalize a non-negative scalar series to 0..1 by its maximum (for feeding audio marker-band
// power into DetectHighRunCenters with the same thresholds as video luma). Returns false if there
// is no positive maximum (no signal).
inline bool NormalizeByMax(std::vector<double>& values) {
    double mx = 0.0;
    for (double v : values) {
        mx = std::max(mx, v);
    }
    if (mx <= 0.0) {
        return false;
    }
    for (double& v : values) {
        v /= mx;
    }
    return true;
}

// Pair each audio-burst QPC with the nearest-later video-flash QPC (they were triggered together,
// so each audio burst has a matching flash within roughly one A/V offset). Returns per-pair
// offset = audioQpc - videoFlashQpc in milliseconds, for pairs where a flash was found within
// maxPairMs of the audio burst. Audio and video edge lists are in trigger order.
//
// We match by ORDER (the i-th audio burst <-> i-th flash) but guard each pair with a sanity window
// so a missed/spurious edge on either side is rejected rather than mis-paired.
inline std::vector<double> PairAvOffsetsMs(const std::vector<uint64_t>& audioBurstQpc100ns,
                                           const std::vector<uint64_t>& videoFlashQpc100ns, double maxAbsOffsetMs) {
    std::vector<double> offsets;
    const size_t n = std::min(audioBurstQpc100ns.size(), videoFlashQpc100ns.size());
    for (size_t i = 0; i < n; ++i) {
        const double offMs = ComputeRenderLatencyMs(audioBurstQpc100ns[i], videoFlashQpc100ns[i]);
        if (offMs >= -maxAbsOffsetMs && offMs <= maxAbsOffsetMs) {
            offsets.push_back(offMs);
        }
    }
    return offsets;
}

// Cache key for the A/V calibration: device + render format + display geometry (the offset depends
// on the render endpoint AND the WGC/display path, so both are folded in).
inline std::string MakeAvCalibrationCacheKey(const std::string& deviceId, int sampleRate, int channels, int displayW,
                                             int displayH) {
    return "av|" + SanitizeCacheToken(deviceId) + "|" + std::to_string(sampleRate) + "|" + std::to_string(channels) +
           "|" + std::to_string(displayW) + "x" + std::to_string(displayH);
}

}  // namespace ce::audio
