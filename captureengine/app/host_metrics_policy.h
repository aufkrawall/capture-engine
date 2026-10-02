#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <vector>

namespace scan_host::metrics_policy {

enum class AdapterResolutionSource : uint32_t {
    Unavailable = 0,
    HookLuid = 1,
    ProcessGpuEngine = 2,
    RetainedProcessGpuEngine = 3,
    CaptureDeviceLuid = 4,
};

struct GpuEngineSample {
    uint32_t processId = 0;
    int64_t adapterLuid = 0;
    double utilization = 0.0;
    bool videoEngine = false;
};

struct AdapterResolution {
    int64_t adapterLuid = 0;
    AdapterResolutionSource source = AdapterResolutionSource::Unavailable;
};

// The configured/injected process remains the profile source in split-renderer
// applications, while a live direct child can own final presentation and publish
// the exact adapter LUID. This mirrors the Vulkan eligibility boundary: accept
// the profiled source itself or its direct renderer child, never an unrelated or
// merely stale publisher PID.
inline bool IsGpuTelemetryPublisherEligible(uint32_t targetPid, uint32_t publisherPid,
                                            uint32_t publisherParentPid) {
    if (targetPid == 0 || publisherPid == 0)
        return false;
    return publisherPid == targetPid || publisherParentPid == targetPid;
}

inline uint32_t CalculateProcessorUsagePercent(uint64_t previousIdle, uint64_t previousKernel,
                                               uint64_t previousUser, uint64_t currentIdle, uint64_t currentKernel,
                                               uint64_t currentUser) {
    if (currentIdle < previousIdle || currentKernel < previousKernel || currentUser < previousUser)
        return 0;
    const uint64_t idle = currentIdle - previousIdle;
    const uint64_t kernel = currentKernel - previousKernel;
    const uint64_t user = currentUser - previousUser;
    if (kernel > UINT64_MAX - user)
        return 0;
    const uint64_t total = kernel + user;
    if (total == 0 || idle >= total)
        return 0;
    const double usage = static_cast<double>(total - idle) * 100.0 / static_cast<double>(total);
    return static_cast<uint32_t>((std::min)(100.0, (std::max)(0.0, usage)));
}

inline char LowerAscii(char value) {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

inline size_t FindAsciiInsensitive(std::string_view text, std::string_view needle, size_t start = 0) {
    if (needle.empty() || needle.size() > text.size())
        return std::string_view::npos;
    for (size_t pos = start; pos + needle.size() <= text.size(); ++pos) {
        bool equal = true;
        for (size_t i = 0; i < needle.size(); ++i) {
            if (LowerAscii(text[pos + i]) != LowerAscii(needle[i])) {
                equal = false;
                break;
            }
        }
        if (equal)
            return pos;
    }
    return std::string_view::npos;
}

inline bool ParseDecimal(std::string_view text, size_t start, uint32_t& value) {
    uint64_t parsed = 0;
    size_t pos = start;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
        parsed = parsed * 10u + static_cast<uint32_t>(text[pos] - '0');
        if (parsed > UINT32_MAX)
            return false;
        ++pos;
    }
    if (pos == start)
        return false;
    value = static_cast<uint32_t>(parsed);
    return true;
}

inline int HexDigit(char value) {
    if (value >= '0' && value <= '9')
        return value - '0';
    value = LowerAscii(value);
    return value >= 'a' && value <= 'f' ? value - 'a' + 10 : -1;
}

inline bool ParseHex32(std::string_view text, size_t start, uint32_t& value, size_t& end) {
    uint64_t parsed = 0;
    size_t pos = start;
    size_t digits = 0;
    while (pos < text.size() && digits < 8) {
        const int digit = HexDigit(text[pos]);
        if (digit < 0)
            break;
        parsed = (parsed << 4) | static_cast<uint32_t>(digit);
        ++pos;
        ++digits;
    }
    if (digits == 0 || (pos < text.size() && HexDigit(text[pos]) >= 0))
        return false;
    value = static_cast<uint32_t>(parsed);
    end = pos;
    return true;
}

inline bool ParseLuid(std::string_view instanceName, int64_t& luid) {
    const std::string_view marker = "luid_0x";
    const size_t markerPos = FindAsciiInsensitive(instanceName, marker);
    if (markerPos == std::string_view::npos)
        return false;

    uint32_t high = 0;
    size_t pos = 0;
    if (!ParseHex32(instanceName, markerPos + marker.size(), high, pos) || pos >= instanceName.size() ||
        instanceName[pos] != '_') {
        return false;
    }
    ++pos;
    if (pos + 2 > instanceName.size() || LowerAscii(instanceName[pos]) != '0' ||
        LowerAscii(instanceName[pos + 1]) != 'x') {
        return false;
    }

    uint32_t low = 0;
    size_t lowEnd = 0;
    if (!ParseHex32(instanceName, pos + 2, low, lowEnd))
        return false;

    const uint64_t combined = (static_cast<uint64_t>(high) << 32) | low;
    luid = static_cast<int64_t>(combined);
    return combined != 0;
}

inline bool IsVideoEngine(std::string_view instanceName) {
    return FindAsciiInsensitive(instanceName, "engtype_videodecode") != std::string_view::npos ||
           FindAsciiInsensitive(instanceName, "engtype_videoencode") != std::string_view::npos ||
           FindAsciiInsensitive(instanceName, "engtype_ofa") != std::string_view::npos ||
           FindAsciiInsensitive(instanceName, "engtype_jpeg") != std::string_view::npos;
}

// Identity of the physical engine an instance names, independent of the process
// that happened to be using it: everything from `phys_` onward, e.g.
// `phys_0_eng_0_engtype_3d`. An instance with no `phys_` token folds into one
// shared bucket rather than silently counting as an engine of its own.
inline uint64_t ParseGpuEngineKey(std::string_view instanceName) {
    const size_t physPos = FindAsciiInsensitive(instanceName, "phys_");
    const std::string_view engine =
        physPos == std::string_view::npos ? std::string_view{} : instanceName.substr(physPos);
    uint64_t hash = 1469598103934665603ull;  // FNV-1a offset basis
    for (const char raw : engine) {
        hash ^= static_cast<uint64_t>(static_cast<unsigned char>(LowerAscii(raw)));
        hash *= 1099511628211ull;
    }
    return hash;
}

struct GpuEngineLoadSample {
    uint64_t engineKey = 0;
    double utilization = 0.0;
};

// Windows publishes one `\GPU Engine(*)\Utilization Percentage` instance per
// (process, adapter, physical engine), and those engines run CONCURRENTLY - 3D,
// Compute and Copy can all be busy in the same microsecond. Adding them together
// therefore does not produce a fraction of elapsed time: it produces a number
// that passes 100 while the GPU still has idle, and clamping that to 100 throws
// away exactly the headroom a reader is looking for. Frame generation is the
// case that makes it unreadable, because it puts the generator's work on compute
// beside the game's raster on 3D, so the sum sits above the clamp whatever the
// GPU is actually doing.
//
// Summing is correct only WITHIN one engine, where processes time-share it. The
// adapter's load is then its busiest engine - the same aggregation Task Manager
// reports, and the only one that stays a fraction of elapsed time.
inline double ResolveAdapterGpuLoadPercent(std::vector<GpuEngineLoadSample> samples) {
    std::sort(samples.begin(), samples.end(),
              [](const GpuEngineLoadSample& left, const GpuEngineLoadSample& right) {
                  return left.engineKey < right.engineKey;
              });
    double busiestEngine = 0.0;
    size_t runStart = 0;
    while (runStart < samples.size()) {
        double engineLoad = 0.0;
        size_t runEnd = runStart;
        while (runEnd < samples.size() && samples[runEnd].engineKey == samples[runStart].engineKey) {
            if (std::isfinite(samples[runEnd].utilization) && samples[runEnd].utilization > 0.0)
                engineLoad += samples[runEnd].utilization;
            ++runEnd;
        }
        busiestEngine = (std::max)(busiestEngine, engineLoad);
        runStart = runEnd;
    }
    return (std::min)(100.0, busiestEngine);
}

// Windows publishes a `\GPU Engine(...)` instance for a process only while
// that process has work on that engine, so the instances come and go. A poll
// that finds none therefore means the process did no GPU work in that interval
// - a real 0% - not that the counter is unreadable.
//
// Publishing "no instance" as unreadable is what made the overlay's GPU row
// strobe between a value and "--" whenever a title went briefly idle: Gothic
// II's intro logos barely touch the GPU, and the row flapped at the poll rate
// (session 20260914_190240). Unreadable is reserved for what is genuinely
// unknown: no resolved adapter, or a counter query that failed outright.
inline bool GpuLoadIsReadable(bool adapterResolved, bool counterQuerySucceeded) {
    return adapterResolved && counterQuerySucceeded;
}

// VRAM usage has no truthful zero: a process with a resolved adapter is using
// some. A poll whose counter instance is momentarily absent therefore holds the
// previous reading rather than blanking the row, and only a run of absences
// long enough to mean the process has stopped producing samples gives up.
inline constexpr uint32_t kVramUsageMissingSampleTolerance = 5;

inline bool VramUsageIsReadable(bool adapterResolved, bool counterReadThisPoll, bool havePreviousReading,
                                uint32_t consecutiveMissingSamples,
                                uint32_t tolerance = kVramUsageMissingSampleTolerance) {
    if (!adapterResolved)
        return false;
    if (counterReadThisPoll)
        return true;
    return havePreviousReading && consecutiveMissingSamples <= tolerance;
}

inline bool ParseGpuEngineSample(std::string_view instanceName, double utilization, GpuEngineSample& sample) {
    const std::string_view pidMarker = "pid_";
    const size_t pidPos = FindAsciiInsensitive(instanceName, pidMarker);
    if (pidPos == std::string_view::npos || !ParseDecimal(instanceName, pidPos + pidMarker.size(), sample.processId) ||
        !ParseLuid(instanceName, sample.adapterLuid)) {
        return false;
    }
    sample.utilization = std::isfinite(utilization) && utilization > 0.0 ? utilization : 0.0;
    sample.videoEngine = IsVideoEngine(instanceName);
    return true;
}

inline AdapterResolution ResolveAdapterLuid(int64_t knownLuid, uint32_t targetPid,
                                            const std::vector<GpuEngineSample>& samples,
                                            int64_t previousProcessLuid = 0,
                                            AdapterResolutionSource knownSource = AdapterResolutionSource::HookLuid) {
    if (knownLuid != 0)
        return {knownLuid, knownSource};
    if (targetPid == 0)
        return {};

    struct Candidate {
        int64_t luid = 0;
        double nonVideoLoad = 0.0;
    };
    std::vector<Candidate> candidates;
    for (const GpuEngineSample& sample : samples) {
        if (sample.processId != targetPid || sample.adapterLuid == 0)
            continue;
        Candidate* candidate = nullptr;
        for (Candidate& current : candidates) {
            if (current.luid == sample.adapterLuid) {
                candidate = &current;
                break;
            }
        }
        if (!candidate) {
            candidates.push_back({});
            candidate = &candidates.back();
            candidate->luid = sample.adapterLuid;
        }
        if (!sample.videoEngine)
            candidate->nonVideoLoad += sample.utilization;
    }

    if (candidates.empty()) {
        return previousProcessLuid != 0
                   ? AdapterResolution{previousProcessLuid, AdapterResolutionSource::RetainedProcessGpuEngine}
                   : AdapterResolution{};
    }

    constexpr double kLoadTieEpsilon = 0.01;
    double bestLoad = 0.0;
    for (const Candidate& candidate : candidates) {
        if (candidate.nonVideoLoad > bestLoad)
            bestLoad = candidate.nonVideoLoad;
    }

    for (const Candidate& candidate : candidates) {
        if (candidate.luid == previousProcessLuid && std::fabs(candidate.nonVideoLoad - bestLoad) <= kLoadTieEpsilon) {
            return {candidate.luid, AdapterResolutionSource::ProcessGpuEngine};
        }
    }

    const Candidate* best = nullptr;
    uint32_t bestCount = 0;
    for (const Candidate& candidate : candidates) {
        if (candidate.nonVideoLoad + kLoadTieEpsilon < bestLoad)
            continue;
        best = &candidate;
        ++bestCount;
    }
    // Multiple equally idle/active adapters are not enough evidence to choose a
    // new identity. Keep prior process evidence across a transient gap, but do
    // not turn deterministic LUID ordering into a false adapter claim.
    if (previousProcessLuid != 0 && bestLoad <= kLoadTieEpsilon)
        return {previousProcessLuid, AdapterResolutionSource::RetainedProcessGpuEngine};
    if (bestCount == 1)
        return {best->luid, AdapterResolutionSource::ProcessGpuEngine};
    return {};
}

}  // namespace scan_host::metrics_policy
