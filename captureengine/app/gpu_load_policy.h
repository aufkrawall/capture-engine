// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

// Pure GPU load sampling, aggregation and temporal-stabilization policy.
//
// Deliberately free of Windows, PDH, D3DKMT and NVML runtime dependencies so
// every branch (PMU sample integration, WDDM node classification/delta math,
// PDH invalid-status detection and glitch rejection) can be verified directly
// by unit tests.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <vector>

#include "host_metrics_policy.h"

namespace scan_host::metrics_policy {

enum class GpuLoadSource : uint8_t {
    Unavailable = 0,
    NvmlSamples,
    NvmlRate,
    D3dkmtNode,
    PdhEngine,
};

inline const char* GpuLoadSourceName(GpuLoadSource source) {
    switch (source) {
        case GpuLoadSource::NvmlSamples:
            return "nvml-samples";
        case GpuLoadSource::NvmlRate:
            return "nvml-rate";
        case GpuLoadSource::D3dkmtNode:
            return "d3dkmt-node";
        case GpuLoadSource::PdhEngine:
            return "pdh-engine";
        default:
            return "unavailable";
    }
}

inline bool IsHardwarePmuSource(GpuLoadSource source) {
    return source == GpuLoadSource::NvmlSamples || source == GpuLoadSource::NvmlRate;
}

struct NvmlUtilSample {
    uint64_t timestampUs = 0;
    uint32_t utilizationPercent = 0;
};

struct NvmlSampleIntegration {
    bool valid = false;
    double percent = 0.0;
    uint32_t sampleCount = 0;
    uint64_t newestTimestampUs = 0;
};

// Integrates hardware PMU GPU utilization samples (typically ~200 ms cadence)
// produced after `lastSeenTimestampUs`, bounded to at most `maxWindowUs` before
// the newest sample so resuming after an idle gap does not dilute the reading
// with seconds-old history.
inline constexpr uint64_t kDefaultNvmlSampleWindowUs = 1'200'000ull;

inline NvmlSampleIntegration IntegrateNvmlGpuSamples(const std::vector<NvmlUtilSample>& samples,
                                                     uint64_t lastSeenTimestampUs,
                                                     uint64_t maxWindowUs = kDefaultNvmlSampleWindowUs) {
    NvmlSampleIntegration result;
    result.newestTimestampUs = lastSeenTimestampUs;

    uint64_t newest = lastSeenTimestampUs;
    for (const NvmlUtilSample& sample : samples) {
        if (sample.timestampUs == 0 || sample.timestampUs <= lastSeenTimestampUs)
            continue;
        if (sample.timestampUs > newest)
            newest = sample.timestampUs;
    }
    if (newest == lastSeenTimestampUs)
        return result;

    const uint64_t cutoff = (maxWindowUs > 0 && newest > maxWindowUs) ? (newest - maxWindowUs) : 0;
    double sum = 0.0;
    uint32_t count = 0;
    for (const NvmlUtilSample& sample : samples) {
        if (sample.timestampUs == 0 || sample.timestampUs <= lastSeenTimestampUs || sample.timestampUs < cutoff)
            continue;
        const uint32_t clamped = sample.utilizationPercent > 100u ? 100u : sample.utilizationPercent;
        sum += static_cast<double>(clamped);
        ++count;
    }
    if (count == 0)
        return result;

    result.valid = true;
    result.percent = std::clamp(sum / static_cast<double>(count), 0.0, 100.0);
    result.sampleCount = count;
    result.newestTimestampUs = newest;
    return result;
}

// Mirrors DXGK_ENGINE_TYPE values from d3dkmdt.h.
enum class D3dkmtEngineType : uint32_t {
    Other = 0,
    ThreeD = 1,
    VideoDecode = 2,
    VideoEncode = 3,
    VideoProcessing = 4,
    SceneAssembly = 5,
    Copy = 6,
    Overlay = 7,
    Crypto = 8,
    VideoCodec = 9,
};

inline bool ContainsAsciiInsensitiveWide(std::wstring_view text, std::string_view needle) {
    if (needle.empty() || needle.size() > text.size())
        return false;
    for (size_t pos = 0; pos + needle.size() <= text.size(); ++pos) {
        bool equal = true;
        for (size_t i = 0; i < needle.size(); ++i) {
            const wchar_t wc = text[pos + i];
            const char narrowed = (wc >= 0 && wc <= 127) ? static_cast<char>(wc) : '\0';
            if (LowerAscii(narrowed) != LowerAscii(needle[i])) {
                equal = false;
                break;
            }
        }
        if (equal)
            return true;
    }
    return false;
}

// Excludes dedicated video/optical-flow/JPEG engines from overlay GPU load,
// matching `IsVideoEngine` for PDH engine instance names.
inline bool IsD3dkmtVideoEngineNode(uint32_t engineType, std::wstring_view friendlyName) {
    const auto type = static_cast<D3dkmtEngineType>(engineType);
    if (type == D3dkmtEngineType::VideoDecode || type == D3dkmtEngineType::VideoEncode ||
        type == D3dkmtEngineType::VideoProcessing || type == D3dkmtEngineType::VideoCodec) {
        return true;
    }
    return ContainsAsciiInsensitiveWide(friendlyName, "videodecode") ||
           ContainsAsciiInsensitiveWide(friendlyName, "videoencode") ||
           ContainsAsciiInsensitiveWide(friendlyName, "videoprocessing") ||
           ContainsAsciiInsensitiveWide(friendlyName, "videocodec") ||
           ContainsAsciiInsensitiveWide(friendlyName, "ofa") ||
           ContainsAsciiInsensitiveWide(friendlyName, "jpeg");
}

struct D3dkmtNodeRunningTimeSample {
    int64_t runningTime100ns = 0;
    bool valid = false;
    bool videoEngine = false;
};

struct D3dkmtLoadResult {
    bool valid = false;
    double percent = 0.0;
};

// Minimum elapsed wall interval (10 ms in 100ns ticks) for a WDDM node delta.
inline constexpr int64_t kMinD3dkmtDeltaElapsed100ns = 100'000;

inline D3dkmtLoadResult ComputeD3dkmtAdapterLoadPercent(const std::vector<D3dkmtNodeRunningTimeSample>& previous,
                                                        const std::vector<D3dkmtNodeRunningTimeSample>& current,
                                                        int64_t elapsed100ns) {
    D3dkmtLoadResult result;
    if (elapsed100ns < kMinD3dkmtDeltaElapsed100ns || previous.empty() || previous.size() != current.size())
        return result;

    double busiest = 0.0;
    uint32_t validNonVideoCount = 0;
    uint32_t invalidOrRegressedNonVideoCount = 0;
    for (size_t i = 0; i < current.size(); ++i) {
        if (current[i].videoEngine || previous[i].videoEngine)
            continue;
        if (!previous[i].valid || !current[i].valid || current[i].runningTime100ns < previous[i].runningTime100ns) {
            ++invalidOrRegressedNonVideoCount;
            continue;
        }
        ++validNonVideoCount;
        const int64_t delta = current[i].runningTime100ns - previous[i].runningTime100ns;
        const double nodePct = static_cast<double>(delta) * 100.0 / static_cast<double>(elapsed100ns);
        if (std::isfinite(nodePct) && nodePct > busiest)
            busiest = nodePct;
    }

    if (validNonVideoCount == 0)
        return result;
    // If a non-video node regressed/failed on this poll and none of the other
    // nodes reported load, treat the poll as incomplete rather than a real 0%.
    if (invalidOrRegressedNonVideoCount > 0 && busiest <= 0.0)
        return result;

    result.valid = true;
    result.percent = std::clamp(busiest, 0.0, 100.0);
    return result;
}

struct PdhNonVideoEngineItem {
    uint64_t engineKey = 0;
    double utilization = 0.0;
    bool valueValid = false;
};

struct PdhGpuLoadPoll {
    double percent = 0.0;
    uint32_t validItems = 0;
    uint32_t invalidItems = 0;
    // True when the active adapter had non-video instances with invalid PDH
    // CStatus (e.g. PDH_CALC_NEGATIVE_VALUE / PDH_CSTATUS_INVALID_DATA) and no
    // valid non-zero load remained - an incomplete poll, not a real 0%.
    bool pollCorrupt = false;
};

inline PdhGpuLoadPoll EvaluatePdhGpuLoadPoll(const std::vector<PdhNonVideoEngineItem>& items) {
    PdhGpuLoadPoll result;
    std::vector<GpuEngineLoadSample> validSamples;
    validSamples.reserve(items.size());
    for (const PdhNonVideoEngineItem& item : items) {
        if (item.valueValid) {
            ++result.validItems;
            validSamples.push_back({item.engineKey, item.utilization});
        } else {
            ++result.invalidItems;
        }
    }
    result.percent = ResolveAdapterGpuLoadPercent(std::move(validSamples));
    result.pollCorrupt = result.invalidItems > 0 && result.percent <= 0.0;
    return result;
}

struct GpuLoadStabilizerOutput {
    bool readable = false;
    double percent = 0.0;
    bool heldCorrupt = false;
    bool suppressedDropout = false;
};

struct GpuLoadStabilizerStats {
    uint32_t heldCorruptPolls = 0;
    uint32_t suppressedDropouts = 0;
    uint32_t confirmedDrops = 0;
};

// Responsive, non-jumpy GPU load filter:
// - Holds the prior reading across up to `kMaxCorruptHoldPolls` corrupt/invalid
//   WDDM/PDH polls instead of flashing 0% or "--".
// - Guards WDDM/PDH sources against single-poll false-zero dropouts (classic
//   HAGS/DX9 95% -> 0% -> 95% flicker): a sudden crash from active load to ~0%
//   is held for one poll and confirmed immediately on a second consecutive low
//   poll, so real idle transitions remain responsive.
// - Hardware PMU interval-averaged samples (NVML) already integrate the full
//   poll window and use light adaptive smoothing only, snapping immediately when
//   upgrading from a WDDM reading so VSync-inflated WDDM state never leaks into
//   the hardware PMU baseline.
class GpuLoadStabilizer {
public:
    static constexpr uint32_t kMaxCorruptHoldPolls = 2;

    void Reset() {
        initialized_ = false;
        published_ = 0.0;
        lastSource_ = GpuLoadSource::Unavailable;
        consecutiveCorruptPolls_ = 0;
        pendingDropout_ = false;
    }

    GpuLoadStabilizerOutput Update(GpuLoadSource source, double rawPercent, bool sampleValid,
                                   bool sampleCorrupt = false) {
        GpuLoadStabilizerOutput out;
        const bool finite = std::isfinite(rawPercent);
        const bool usable = sampleValid && finite && !sampleCorrupt;

        if (!usable) {
            if (initialized_ && consecutiveCorruptPolls_ < kMaxCorruptHoldPolls) {
                ++consecutiveCorruptPolls_;
                ++stats_.heldCorruptPolls;
                out.readable = true;
                out.percent = published_;
                out.heldCorrupt = true;
                return out;
            }
            if (!sampleValid || !finite) {
                return out;
            }
            // Corrupt PDH persisted past the hold budget: accept the clamped value.
        }

        const double clamped = std::clamp(rawPercent, 0.0, 100.0);
        consecutiveCorruptPolls_ = 0;

        if (!initialized_) {
            initialized_ = true;
            published_ = clamped;
            lastSource_ = source;
            pendingDropout_ = false;
            out.readable = true;
            out.percent = published_;
            return out;
        }

        // Switching from WDDM to hardware PMU adopts the PMU value immediately
        // so WDDM FIFO-VSync inflation does not linger into the new baseline.
        if (IsHardwarePmuSource(source) && !IsHardwarePmuSource(lastSource_)) {
            published_ = clamped;
            lastSource_ = source;
            pendingDropout_ = false;
            out.readable = true;
            out.percent = published_;
            return out;
        }
        lastSource_ = source;

        if (!IsHardwarePmuSource(source)) {
            const bool suspiciousDrop =
                (published_ >= 20.0 && clamped <= 2.0 && (published_ - clamped) >= 25.0) ||
                (published_ >= 50.0 && clamped <= 5.0);
            if (suspiciousDrop) {
                if (!pendingDropout_) {
                    pendingDropout_ = true;
                    ++stats_.suppressedDropouts;
                    out.readable = true;
                    out.percent = published_;
                    out.suppressedDropout = true;
                    return out;
                }
                pendingDropout_ = false;
                ++stats_.confirmedDrops;
                published_ = clamped;
                out.readable = true;
                out.percent = published_;
                return out;
            }
            pendingDropout_ = false;
        } else {
            pendingDropout_ = false;
        }

        const double delta = std::fabs(clamped - published_);
        double alpha = 0.50;
        if (source == GpuLoadSource::NvmlSamples) {
            alpha = delta >= 12.0 ? 0.85 : 0.60;
        } else {
            alpha = delta >= 20.0 ? 0.70 : 0.50;
        }

        double next = published_ + alpha * (clamped - published_);
        if (clamped <= 0.5 && published_ < 2.0)
            next = clamped;
        else if (clamped >= 99.5 && published_ > 98.0)
            next = clamped;

        published_ = std::clamp(next, 0.0, 100.0);
        out.readable = true;
        out.percent = published_;
        return out;
    }

    const GpuLoadStabilizerStats& Stats() const { return stats_; }
    void ResetStats() { stats_ = {}; }

private:
    bool initialized_ = false;
    double published_ = 0.0;
    GpuLoadSource lastSource_ = GpuLoadSource::Unavailable;
    uint32_t consecutiveCorruptPolls_ = 0;
    bool pendingDropout_ = false;
    GpuLoadStabilizerStats stats_{};
};

}  // namespace scan_host::metrics_policy
