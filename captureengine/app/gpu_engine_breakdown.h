#pragma once

// Per-engine-type GPU utilization while a video encoder runs.
//
// The overlay's GPU load deliberately ignores the video engines and reports the
// busiest of the rest. Encoder-overload diagnosis needs the opposite view: is
// the Video Encode engine itself saturated, and how much of the 3D engine does
// the game leave for CaptureEngine's conversion? With FFmpeg's synchronous
// NVENC mode an encode's completion cannot be observed without blocking, but
// Windows' per-engine running time answers the same question: a Video Encode
// engine near 100% means NVENC is the limit; one well below it while submits
// still block means the encoder waits on work queued in front of it (session
// 20261015_213903: p5 at ~122 fps capacity with the conversion completing a
// full pipeline depth late).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "host_metrics_policy.h"

namespace scan_host::metrics_policy {

enum class GpuEngineType : uint8_t { ThreeD = 0, Compute, Copy, VideoEncode, VideoDecode, Other };
inline constexpr size_t kGpuEngineTypeCount = 6;

inline GpuEngineType ClassifyGpuEngineType(std::string_view instanceName) {
    const size_t marker = FindAsciiInsensitive(instanceName, "engtype_");
    if (marker == std::string_view::npos)
        return GpuEngineType::Other;
    const std::string_view type = instanceName.substr(marker + 8);
    auto startsWith = [&](std::string_view prefix) {
        if (type.size() < prefix.size())
            return false;
        for (size_t i = 0; i < prefix.size(); ++i) {
            if (LowerAscii(type[i]) != prefix[i])
                return false;
        }
        return true;
    };
    if (startsWith("3d"))
        return GpuEngineType::ThreeD;
    if (startsWith("compute"))
        return GpuEngineType::Compute;
    if (startsWith("copy"))
        return GpuEngineType::Copy;
    if (startsWith("videoencode"))
        return GpuEngineType::VideoEncode;
    if (startsWith("videodecode"))
        return GpuEngineType::VideoDecode;
    return GpuEngineType::Other;
}

struct GpuEngineTypedSample {
    uint64_t engineKey = 0;  // physical engine, see ParseGpuEngineKey
    GpuEngineType type = GpuEngineType::Other;
    uint32_t processId = 0;
    double utilization = 0.0;
};

// One poll of one adapter: each type's busiest physical engine (processes
// time-share an engine, so their utilizations add within it), and how the
// busiest 3D engine splits between the target and everyone else.
struct GpuEngineBreakdown {
    double busiest[kGpuEngineTypeCount] = {};
    double threeDTarget = 0.0;
    double threeDOthers = 0.0;
};

inline double ClampUtilization(double value) {
    return std::isfinite(value) && value > 0.0 ? (std::min)(100.0, value) : 0.0;
}

inline GpuEngineBreakdown ResolveGpuEngineBreakdown(std::vector<GpuEngineTypedSample> samples, uint32_t targetPid) {
    std::sort(samples.begin(), samples.end(), [](const GpuEngineTypedSample& left, const GpuEngineTypedSample& right) {
        return left.engineKey < right.engineKey;
    });
    GpuEngineBreakdown breakdown;
    double busiest3dTotal = -1.0;
    size_t runStart = 0;
    while (runStart < samples.size()) {
        double total = 0.0;
        double target = 0.0;
        size_t runEnd = runStart;
        while (runEnd < samples.size() && samples[runEnd].engineKey == samples[runStart].engineKey) {
            const double value = ClampUtilization(samples[runEnd].utilization);
            total += value;
            if (targetPid != 0 && samples[runEnd].processId == targetPid)
                target += value;
            ++runEnd;
        }
        total = (std::min)(100.0, total);
        const size_t type = static_cast<size_t>(samples[runStart].type);
        breakdown.busiest[type] = (std::max)(breakdown.busiest[type], total);
        if (samples[runStart].type == GpuEngineType::ThreeD && total > busiest3dTotal) {
            busiest3dTotal = total;
            breakdown.threeDTarget = (std::min)(target, total);
            breakdown.threeDOthers = (std::max)(0.0, total - breakdown.threeDTarget);
        }
        runStart = runEnd;
    }
    return breakdown;
}

inline constexpr uint64_t kGpuEngineWindowMs = 10000;

// A poll belongs to an encoding window only while some process keeps a Video
// Encode engine busy; the overlay's idle polls before and after a recording
// would otherwise dilute the averages (session 20261015_221846: a 10 s
// recording got one window, half of it pre-recording idle, and lost its tail).
inline bool IsEncodingPoll(const GpuEngineBreakdown& sample) {
    return sample.busiest[static_cast<size_t>(GpuEngineType::VideoEncode)] > 0.0;
}

// Averages and peaks of the encoding polls in one window. The owner feeds only
// encoding polls and takes the line when the window is due, when the encoder
// goes idle and when the target or adapter changes, so a recording's tail is
// reported too and idle sessions add no log lines.
class GpuEngineWindow {
public:
    void Add(const GpuEngineBreakdown& sample, uint64_t nowMs) {
        if (count_ == 0)
            firstMs_ = nowMs;
        lastMs_ = nowMs;
        ++count_;
        for (size_t i = 0; i < kGpuEngineTypeCount; ++i)
            Accumulate(sample.busiest[i], sum_[i], max_[i]);
        Accumulate(sample.threeDTarget, targetSum_, targetMax_);
        Accumulate(sample.threeDOthers, othersSum_, othersMax_);
    }

    bool Due(uint64_t nowMs) const { return count_ > 0 && nowMs - firstMs_ >= kGpuEngineWindowMs; }
    bool Empty() const { return count_ == 0; }
    uint32_t Count() const { return count_; }

    // end: "window" (full window), "encoderIdle" (tail), "targetChange".
    std::string TakeLine(uint32_t targetPid, const char* end) {
        auto avg = [&](double sum) { return count_ > 0 ? sum / count_ : 0.0; };
        auto at = [&](GpuEngineType type) { return static_cast<size_t>(type); };
        char line[448] = {};
        std::snprintf(line, sizeof(line),
                      "[Metrics] GPU engines polls=%u spanMs=%llu end=%s avg/max: 3d=%.0f/%.0f%% "
                      "(target pid %u=%.0f/%.0f%% others=%.0f/%.0f%%) compute=%.0f/%.0f%% copy=%.0f/%.0f%% videoEncode=%.0f/%.0f%% "
                      "videoDecode=%.0f/%.0f%% other=%.0f/%.0f%%",
                      count_, static_cast<unsigned long long>(lastMs_ - firstMs_), end,
                      avg(sum_[at(GpuEngineType::ThreeD)]), max_[at(GpuEngineType::ThreeD)], targetPid,
                      avg(targetSum_), targetMax_, avg(othersSum_), othersMax_, avg(sum_[at(GpuEngineType::Compute)]),
                      max_[at(GpuEngineType::Compute)], avg(sum_[at(GpuEngineType::Copy)]),
                      max_[at(GpuEngineType::Copy)], avg(sum_[at(GpuEngineType::VideoEncode)]),
                      max_[at(GpuEngineType::VideoEncode)], avg(sum_[at(GpuEngineType::VideoDecode)]),
                      max_[at(GpuEngineType::VideoDecode)], avg(sum_[at(GpuEngineType::Other)]),
                      max_[at(GpuEngineType::Other)]);
        *this = GpuEngineWindow();
        return line;
    }

private:
    static void Accumulate(double value, double& sum, double& max) {
        sum += value;
        max = (std::max)(max, value);
    }

    uint32_t count_ = 0;
    uint64_t firstMs_ = 0;
    uint64_t lastMs_ = 0;
    double sum_[kGpuEngineTypeCount] = {};
    double max_[kGpuEngineTypeCount] = {};
    double targetSum_ = 0.0;
    double targetMax_ = 0.0;
    double othersSum_ = 0.0;
    double othersMax_ = 0.0;
};

}  // namespace scan_host::metrics_policy
