#pragma once

// Render->loopback audio capture-latency self-measurement.
//
// WASAPI GetStreamLatency() reports 0 for HDMI/AVR/Bluetooth render endpoints (and only the
// buffer latency, not the full render->loopback path, for others), so it cannot reliably tell us
// how late loopback audio lands vs the video content clock. This module measures the real
// render-endpoint latency directly: it renders a brief NEAR-INAUDIBLE marker (a short windowed
// tone near Nyquist / ultrasonic, low amplitude) to the default render endpoint and detects its
// onset in a LOOPBACK capture of that same endpoint. Loopback taps the DIGITAL render mix before
// the DAC/speaker, so even an ultrasonic marker is captured perfectly and the measured delay is
// the driver-hidden render->loopback path latency.
//
// This header holds only the PURE, hardware-independent logic (marker synthesis, onset detection,
// latency arithmetic, and the per-device cache format) so it is fully unit-testable. The WASAPI
// orchestration lives in audio_latency_probe.cpp.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "audio_time_utils.h"

namespace ce::audio {

// Cache schema marker. Bump when the marker design or measurement semantics change so stale
// cached values are ignored rather than silently reused.
inline constexpr const char* kLatencyCacheHeader = "# CE audio render-endpoint latency cache v1";

// Implausible measurements are rejected (fail-safe to config). A real render->loopback path is
// bounded by a few hundred ms even for deep AVR/Bluetooth buffers.
inline constexpr double kMinPlausibleLatencyMs = 0.0;
inline constexpr double kMaxPlausibleLatencyMs = 2000.0;

inline constexpr double kProbePi = 3.14159265358979323846;

// Marker design resolved per render-endpoint sample rate.
struct ProbeMarkerSpec {
    double sampleRate = 48000.0;
    double markerFreqHz = 0.0;  // near Nyquist / ultrasonic, inaudible but loopback-detectable
    double amplitude = 0.0;     // low amplitude (digital, pre-DAC); inaudible
    int leadInFrames = 0;       // silence before the marker so detection has a clean noise floor
    int markerFrames = 0;       // marker burst length
    int tailFrames = 0;         // silence after the marker so the capture overruns the onset
    bool valid = false;

    int totalFrames() const {
        return leadInFrames + markerFrames + tailFrames;
    }
};

// Resolve a marker for the given endpoint rate. The marker frequency is placed high enough to be
// inaudible to humans (>= ~19 kHz) yet below Nyquist, scaling up on high-rate (96/192 kHz)
// endpoints to fully ultrasonic. Returns valid=false for unusable sample rates.
inline ProbeMarkerSpec ResolveProbeMarkerSpec(int sampleRate) {
    ProbeMarkerSpec spec;
    if (sampleRate < 16000) {
        return spec;  // too low to host an inaudible marker below Nyquist
    }
    spec.sampleRate = static_cast<double>(sampleRate);
    const double nyquist = spec.sampleRate * 0.5;
    // Prefer fully ultrasonic (>= 20 kHz). On 44.1/48 kHz endpoints Nyquist is ~22/24 kHz, so sit
    // just below Nyquist (still inaudible to almost everyone) at 0.46*rate. On higher-rate
    // endpoints clamp to a comfortable ultrasonic band so we do not ride the anti-image filter.
    double freq = std::min(nyquist * 0.92, std::max(20000.0, spec.sampleRate * 0.46));
    if (freq >= nyquist) {
        freq = nyquist * 0.92;
    }
    spec.markerFreqHz = freq;
    spec.amplitude = 0.06;  // ~ -24 dBFS: clearly detectable digitally, inaudible acoustically
    // Durations in milliseconds, converted to frames. Lead-in lets the loopback stream settle;
    // the marker is long enough for several Goertzel windows; the tail guarantees we capture past
    // the onset even with a deep endpoint buffer.
    spec.leadInFrames = static_cast<int>(spec.sampleRate * 0.060);  // 60 ms
    spec.markerFrames = static_cast<int>(spec.sampleRate * 0.040);  // 40 ms
    spec.tailFrames = static_cast<int>(spec.sampleRate * 0.020);    // 20 ms
    spec.valid = spec.markerFrames > 0;
    return spec;
}

// Generate the mono probe waveform: leadInFrames of silence, markerFrames of a Hann-windowed
// sine at markerFreqHz, then tailFrames of silence. The Hann window keeps the burst band-limited
// (no broadband click) so it stays inaudible and energy stays concentrated at markerFreqHz.
inline void GenerateProbeMarkerMono(const ProbeMarkerSpec& spec, std::vector<float>& out) {
    out.clear();
    if (!spec.valid) {
        return;
    }
    out.assign(static_cast<size_t>(spec.totalFrames()), 0.0f);
    const double w = 2.0 * kProbePi * spec.markerFreqHz / spec.sampleRate;
    for (int i = 0; i < spec.markerFrames; ++i) {
        const double hann = 0.5 - 0.5 * std::cos(2.0 * kProbePi * i / std::max(1, spec.markerFrames - 1));
        const double s = spec.amplitude * hann * std::sin(w * i);
        out[static_cast<size_t>(spec.leadInFrames + i)] = static_cast<float>(s);
    }
}

// Goertzel power (normalized by window length) of `n` samples at freqHz. Used as a narrowband
// energy detector for the marker tone; robust against unrelated audio that is not at markerFreqHz.
inline double GoertzelPower(const float* x, int n, double sampleRate, double freqHz) {
    if (!x || n <= 0 || sampleRate <= 0.0) {
        return 0.0;
    }
    const double w = 2.0 * kProbePi * freqHz / sampleRate;
    const double coeff = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (int i = 0; i < n; ++i) {
        const double s0 = static_cast<double>(x[i]) + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    return power / static_cast<double>(n);
}

// Detect the marker by its narrowband energy and return the frame at the CENTER of the
// highest-power window. Because the marker is Hann-windowed, its energy peak sits at the burst
// MIDPOINT, which is a deterministic, envelope-unbiased reference (a leading-edge/threshold
// detector would land somewhere on the slow Hann ramp and inject a systematic latency bias). The
// caller compares this center against the render-side marker center, so the envelope cancels.
// Returns -1 when no confident narrowband burst is present (fail-safe). A narrowband Goertzel
// detector is used so unrelated audio that is not at markerFreqHz does not move the peak.
//
// windowFrames/hopFrames default to ~5 ms / ~2.5 ms when passed <= 0. A several-ms window gives the
// narrowband Goertzel enough frequency resolution to reject loud out-of-band audio (e.g. music
// playing during the probe) while staying far shorter than the marker burst.
inline int DetectMarkerCenterFrame(const float* captured, size_t frames, int sampleRate, double markerFreqHz,
                                   int windowFrames = 0, int hopFrames = 0, double peakOverFloorMin = 8.0) {
    if (!captured || sampleRate <= 0 || markerFreqHz <= 0.0) {
        return -1;
    }
    if (windowFrames <= 0) {
        windowFrames = std::max(64, sampleRate / 200);
    }
    if (hopFrames <= 0) {
        hopFrames = std::max(1, windowFrames / 2);
    }
    if (frames < static_cast<size_t>(windowFrames)) {
        return -1;
    }

    const size_t lastStart = frames - static_cast<size_t>(windowFrames);
    std::vector<double> powers;
    std::vector<size_t> starts;
    powers.reserve(lastStart / static_cast<size_t>(hopFrames) + 1);
    starts.reserve(powers.capacity());
    double peak = 0.0;
    size_t peakIdx = 0;
    for (size_t start = 0; start <= lastStart; start += static_cast<size_t>(hopFrames)) {
        const double p = GoertzelPower(captured + start, windowFrames, static_cast<double>(sampleRate), markerFreqHz);
        if (p > peak) {
            peak = p;
            peakIdx = powers.size();
        }
        powers.push_back(p);
        starts.push_back(start);
    }
    if (peak <= 0.0 || powers.empty()) {
        return -1;
    }

    // Confidence: the peak must clearly exceed the median marker-band power. The median is a robust
    // floor (the burst occupies only a fraction of windows), and a sustained tone sits far above it
    // while white noise / silence does not.
    std::vector<double> sorted = powers;
    std::sort(sorted.begin(), sorted.end());
    const double medianFloor = sorted[sorted.size() / 2];
    const double effectiveFloor = std::max(medianFloor, peak * 1e-12);
    if (peak < effectiveFloor * peakOverFloorMin) {
        return -1;  // no confident narrowband burst above the floor
    }

    // Sustained-burst check: a real marker is a continuous tone spanning many consecutive windows,
    // so the peak sits inside a run of windows >= half the peak power. An isolated white-noise spike
    // does not. This is the key discriminator that keeps silence/noise from false-positiving.
    const double runThreshold = peak * 0.5;
    int runLen = 1;
    for (size_t i = peakIdx; i > 0 && powers[i - 1] >= runThreshold; --i) {
        ++runLen;
    }
    for (size_t i = peakIdx + 1; i < powers.size() && powers[i] >= runThreshold; ++i) {
        ++runLen;
    }
    // The marker burst is several ms long; require at least a few hops of sustained energy.
    if (runLen < 4) {
        return -1;
    }

    return static_cast<int>(starts[peakIdx]) + windowFrames / 2;
}

// latency = loopback-captured marker-center QPC - render presentation QPC of the marker center.
// Both inputs are in 100-ns QPC units. Returns a signed value; the caller validates plausibility.
inline double ComputeRenderLatencyMs(uint64_t loopbackMarkerQpc100ns, uint64_t renderMarkerQpc100ns) {
    const double diff100ns =
        static_cast<double>(static_cast<int64_t>(loopbackMarkerQpc100ns) - static_cast<int64_t>(renderMarkerQpc100ns));
    return diff100ns / static_cast<double>(kHundredNanosecondsPerMillisecond);
}

inline bool IsPlausibleLatencyMs(double latencyMs) {
    return latencyMs >= kMinPlausibleLatencyMs && latencyMs <= kMaxPlausibleLatencyMs;
}

// Robust aggregation of repeated independent measurements. Going fully automatic (no hand-set
// value) means a single noisy shot must not over/under-correct, so the probe takes several shots
// and this picks the consensus: the median, then the median of the subset that agrees within
// maxSpreadMs. Returns ok=false unless at least minCount shots agree, so a flaky/contended endpoint
// falls back to "no correction" rather than a bad guess.
struct MedianLatencyResult {
    bool ok = false;
    double latencyMs = 0.0;
    int agreeingCount = 0;
};

inline MedianLatencyResult MedianWithConsistency(std::vector<double> values, int minCount, double maxSpreadMs) {
    MedianLatencyResult r;
    if (static_cast<int>(values.size()) < minCount) {
        return r;
    }
    std::sort(values.begin(), values.end());
    const double median = values[values.size() / 2];
    std::vector<double> agree;
    for (double v : values) {
        if (std::fabs(v - median) <= maxSpreadMs) {
            agree.push_back(v);
        }
    }
    if (static_cast<int>(agree.size()) < minCount) {
        return r;  // no consensus -> caller falls back to no correction
    }
    std::sort(agree.begin(), agree.end());
    r.latencyMs = agree[agree.size() / 2];
    r.agreeingCount = static_cast<int>(agree.size());
    r.ok = true;
    return r;
}

// ---- Per-device latency cache (text file, key=latencyMs) -------------------------------------

struct LatencyCacheEntry {
    std::string key;
    double latencyMs = 0.0;
};

// Cache keys must not contain '=' / CR / LF (the line format). Device IDs do not, but sanitize
// defensively. The key folds in sample rate + channels so a reconfigured endpoint re-measures.
inline std::string SanitizeCacheToken(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c == '=' || c == '\r' || c == '\n') {
            c = '_';
        }
    }
    return out;
}

inline std::string MakeRenderEndpointCacheKey(const std::string& deviceId, int sampleRate, int channels) {
    return SanitizeCacheToken(deviceId) + "|" + std::to_string(sampleRate) + "|" + std::to_string(channels);
}

// Parse the cache text. Lines are "key=latencyMs"; the header line and blank/comment lines are
// ignored. Tolerant of missing/extra whitespace and a missing header (returns whatever parses).
inline bool ParseLatencyCache(const std::string& text, std::vector<LatencyCacheEntry>& out) {
    out.clear();
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find('\n', pos);
        std::string line = (end == std::string::npos) ? text.substr(pos) : text.substr(pos, end - pos);
        pos = (end == std::string::npos) ? text.size() + 1 : end + 1;

        // Trim CR/space.
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        size_t lead = 0;
        while (lead < line.size() && (line[lead] == ' ' || line[lead] == '\t')) {
            ++lead;
        }
        line = line.substr(lead);
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) {
            continue;
        }
        const std::string key = line.substr(0, eq);
        const std::string valueStr = line.substr(eq + 1);
        try {
            const double value = std::stod(valueStr);
            out.push_back(LatencyCacheEntry{key, value});
        } catch (...) {
            continue;
        }
    }
    return true;
}

inline std::string SerializeLatencyCache(const std::vector<LatencyCacheEntry>& entries) {
    std::string text = kLatencyCacheHeader;
    text += "\n";
    for (const auto& e : entries) {
        // 3 decimals is well below the ~half-frame measurement floor.
        char buf[64];
        snprintf(buf, sizeof(buf), "%.3f", e.latencyMs);
        text += e.key;
        text += "=";
        text += buf;
        text += "\n";
    }
    return text;
}

inline bool LookupLatencyCache(const std::vector<LatencyCacheEntry>& entries, const std::string& key, double* outMs) {
    for (const auto& e : entries) {
        if (e.key == key) {
            if (outMs) {
                *outMs = e.latencyMs;
            }
            return true;
        }
    }
    return false;
}

inline void UpsertLatencyCache(std::vector<LatencyCacheEntry>& entries, const std::string& key, double latencyMs) {
    for (auto& e : entries) {
        if (e.key == key) {
            e.latencyMs = latencyMs;
            return;
        }
    }
    entries.push_back(LatencyCacheEntry{key, latencyMs});
}

// ---- WASAPI orchestration (audio_latency_probe.cpp; NOT linked into unit tests) ---------------

// Result of a render-endpoint latency probe.
struct RenderLatencyProbeResult {
    bool ok = false;         // a usable latency is available (cache hit or fresh measurement)
    bool measured = false;   // true if a fresh WASAPI measurement was performed this call
    bool fromCache = false;  // true if served from the on-disk cache
    double latencyMs = 0.0;  // render->loopback latency for the default render endpoint
    std::string deviceKey;   // cache key (deviceId|rate|channels)
    int sampleRate = 0;
    int channels = 0;
};

// Measure (or load from cache) the default render endpoint's render->loopback capture latency by
// rendering a brief near-inaudible marker and detecting its onset in a loopback capture of the
// same endpoint. `cacheDir` is the directory for the persistent per-device cache file (may be
// empty to disable caching). When `forceRemeasure` is true any cached value is ignored. This call
// is fail-safe: it never throws and returns ok=false on any error (caller falls back to config).
// Plays a faint calibration sound once per uncached device. Logs every component via DLL_Log.
RenderLatencyProbeResult MeasureRenderEndpointLatency(const std::string& cacheDir, bool forceRemeasure);

}  // namespace ce::audio
