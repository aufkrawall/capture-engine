#pragma once

// Pure selection policy for the optional LibreHardwareMonitor bridge.
//
// This used to live inside the first-party PowerShell script and therefore had
// no test coverage at all. It is deliberately free of Windows, COM and CLR
// dependencies so the unit tests can drive every branch directly.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ce::hardware_sensors::policy {

inline constexpr size_t kNoSelection = static_cast<size_t>(-1);

enum class MetricScope : uint8_t { Cpu, Gpu };

struct MetricDefinition {
    // Configuration key, also used as the log name for the selected sensor.
    const char* key;
    // LibreHardwareMonitor SensorType enum member the metric is read from.
    const char* sensorType;
    MetricScope scope;
    // Zero means "not readable" rather than "idle" for every rail except the
    // fan: without elevation LibreHardwareMonitor cannot open its kernel driver
    // and the CPU power, clock, voltage and temperature rails all read exactly
    // zero. A stopped fan really is 0 RPM, so it is the one exception.
    bool rejectZero;
    float maximum;
    const char* const* preferredNames;
    size_t preferredNameCount;
};

namespace detail {

inline constexpr const char* kCpuTemperatureNames[] = {"CPU Package", "Core (Tctl/Tdie)", "CPU (Tctl/Tdie)",
                                                       "CPU Die (average)"};
inline constexpr const char* kGpuTemperatureNames[] = {"GPU Core"};
inline constexpr const char* kCpuPackagePowerNames[] = {"CPU Package", "Package", "CPU PPT"};
inline constexpr const char* kGpuPackagePowerNames[] = {"GPU Package", "GPU Board", "GPU Power"};
inline constexpr const char* kGpuFanNames[] = {"GPU Fan"};
inline constexpr const char* kCpuCoreClockNames[] = {"Cores (Average)", "CPU Core", "Core"};
inline constexpr const char* kGpuCoreClockNames[] = {"GPU Core"};
inline constexpr const char* kGpuMemoryClockNames[] = {"GPU Memory"};
inline constexpr const char* kGpuVoltageNames[] = {"GPU Core Voltage", "GPU Core"};

}  // namespace detail

// Wire order of the CE_LHM_SAMPLE value/identifier pairs, declared once so the
// bridge, the parser and the tests cannot drift apart. New metrics append: an
// older reader meeting a longer line rejects it on field count instead of
// misreading a shifted field.
inline constexpr MetricDefinition kMetrics[] = {
    {"cpu_temperature", "Temperature", MetricScope::Cpu, true, 250.0f, detail::kCpuTemperatureNames, 4},
    {"gpu_temperature", "Temperature", MetricScope::Gpu, true, 250.0f, detail::kGpuTemperatureNames, 1},
    {"cpu_package_power", "Power", MetricScope::Cpu, true, 5000.0f, detail::kCpuPackagePowerNames, 3},
    {"gpu_package_power", "Power", MetricScope::Gpu, true, 5000.0f, detail::kGpuPackagePowerNames, 3},
    {"gpu_fan", "Fan", MetricScope::Gpu, false, 100000.0f, detail::kGpuFanNames, 1},
    {"cpu_core_clock", "Clock", MetricScope::Cpu, true, 20000.0f, detail::kCpuCoreClockNames, 3},
    {"gpu_core_clock", "Clock", MetricScope::Gpu, true, 20000.0f, detail::kGpuCoreClockNames, 1},
    {"gpu_memory_clock", "Clock", MetricScope::Gpu, true, 20000.0f, detail::kGpuMemoryClockNames, 1},
    {"gpu_voltage", "Voltage", MetricScope::Gpu, true, 10.0f, detail::kGpuVoltageNames, 2},
};

inline constexpr size_t kMetricCount = sizeof(kMetrics) / sizeof(kMetrics[0]);

struct SensorCandidate {
    std::string name;
    std::string identifier;
    float value = 0.0f;
    bool hasValue = false;
};

inline bool EqualsIgnoreCase(std::string_view left, std::string_view right) {
    if (left.size() != right.size())
        return false;
    for (size_t index = 0; index < left.size(); ++index) {
        unsigned char a = static_cast<unsigned char>(left[index]);
        unsigned char b = static_cast<unsigned char>(right[index]);
        if (a >= 'A' && a <= 'Z')
            a = static_cast<unsigned char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z')
            b = static_cast<unsigned char>(b - 'A' + 'a');
        if (a != b)
            return false;
    }
    return true;
}

// A reading is unusable when the sensor reported nothing, reported a negative
// value, or reported the zero that means "no kernel driver" for this metric.
inline bool IsUsable(const SensorCandidate& candidate, bool rejectZero) {
    if (!candidate.hasValue || candidate.value < 0.0f || candidate.value != candidate.value)
        return false;
    return !(rejectZero && candidate.value <= 0.0f);
}

// Matches the numbered forms multi-instance hardware uses instead of an exact
// name: "GPU Fan 1", "GPU Fan #2", "Core #1". Returns the instance index.
inline bool MatchesNumberedName(std::string_view name, std::string_view base, uint32_t& instanceIndex) {
    if (name.size() <= base.size() || !EqualsIgnoreCase(name.substr(0, base.size()), base))
        return false;
    size_t position = base.size();
    while (position < name.size() && name[position] == ' ')
        ++position;
    if (position < name.size() && name[position] == '#')
        ++position;
    while (position < name.size() && name[position] == ' ')
        ++position;
    if (position >= name.size())
        return false;
    uint64_t parsed = 0;
    for (; position < name.size(); ++position) {
        if (name[position] < '0' || name[position] > '9')
            return false;
        parsed = parsed * 10 + static_cast<uint64_t>(name[position] - '0');
        if (parsed > 0xFFFFFFFFull)
            return false;
    }
    instanceIndex = static_cast<uint32_t>(parsed);
    return true;
}

// An explicitly configured identifier is honoured even when its reading is
// currently unusable; the caller reports the metric as unavailable instead.
inline size_t SelectExact(const std::vector<SensorCandidate>& candidates, std::string_view identifier) {
    for (size_t index = 0; index < candidates.size(); ++index) {
        if (EqualsIgnoreCase(candidates[index].identifier, identifier))
            return index;
    }
    return kNoSelection;
}

// Automatic selection, in the order that keeps a selection stable across polls:
//  1. an exact preferred name, in preference order;
//  2. the lowest-numbered instance of a preferred name - leaving this to step 4
//     handed the choice to whichever instance read highest in one sample, so two
//     idle fans a few RPM apart renamed the selected identifier nearly every
//     poll and the reported RPM alternated between physical fans;
//  3. the identifier selected last time, while it stays usable, so step 4
//     cannot reselect a different unrecognized sensor every sample;
//  4. the highest usable reading of the requested type.
inline size_t SelectAutomatic(const std::vector<SensorCandidate>& candidates, const char* const* preferredNames,
                              size_t preferredNameCount, bool rejectZero, std::string_view previousIdentifier) {
    for (size_t preferred = 0; preferred < preferredNameCount; ++preferred) {
        for (size_t index = 0; index < candidates.size(); ++index) {
            if (EqualsIgnoreCase(candidates[index].name, preferredNames[preferred]) &&
                IsUsable(candidates[index], rejectZero)) {
                return index;
            }
        }
    }
    for (size_t preferred = 0; preferred < preferredNameCount; ++preferred) {
        size_t selected = kNoSelection;
        uint32_t selectedInstance = 0;
        for (size_t index = 0; index < candidates.size(); ++index) {
            uint32_t instance = 0;
            if (!IsUsable(candidates[index], rejectZero) ||
                !MatchesNumberedName(candidates[index].name, preferredNames[preferred], instance)) {
                continue;
            }
            if (selected == kNoSelection || instance < selectedInstance) {
                selected = index;
                selectedInstance = instance;
            }
        }
        if (selected != kNoSelection)
            return selected;
    }
    if (!previousIdentifier.empty()) {
        for (size_t index = 0; index < candidates.size(); ++index) {
            if (EqualsIgnoreCase(candidates[index].identifier, previousIdentifier) &&
                IsUsable(candidates[index], rejectZero)) {
                return index;
            }
        }
    }
    size_t highest = kNoSelection;
    for (size_t index = 0; index < candidates.size(); ++index) {
        if (!IsUsable(candidates[index], rejectZero))
            continue;
        if (highest == kNoSelection || candidates[index].value > candidates[highest].value)
            highest = index;
    }
    return highest;
}

struct GpuLoadCandidate {
    std::string identifier;
    float coreLoad = 0.0f;
    bool hasCoreLoad = false;
};

// Follows the GPU reporting the highest GPU Core load, and keeps the previous
// choice when several report the same load so an idle multi-GPU system cannot
// flip between adapters. A single GPU that reports no load at all is still the
// answer; several silent GPUs are not resolvable and yield no selection.
inline size_t SelectActiveGpu(const std::vector<GpuLoadCandidate>& candidates, std::string_view previousIdentifier) {
    size_t best = kNoSelection;
    size_t tieCount = 0;
    for (size_t index = 0; index < candidates.size(); ++index) {
        if (!candidates[index].hasCoreLoad)
            continue;
        if (best == kNoSelection || candidates[index].coreLoad > candidates[best].coreLoad) {
            best = index;
            tieCount = 1;
        } else if (candidates[index].coreLoad == candidates[best].coreLoad) {
            ++tieCount;
        }
    }
    if (best != kNoSelection && tieCount == 1)
        return best;
    if (best != kNoSelection && !previousIdentifier.empty()) {
        for (size_t index = 0; index < candidates.size(); ++index) {
            if (candidates[index].hasCoreLoad && candidates[index].coreLoad == candidates[best].coreLoad &&
                EqualsIgnoreCase(candidates[index].identifier, previousIdentifier)) {
                return index;
            }
        }
    }
    if (best == kNoSelection && candidates.size() == 1)
        return 0;
    return kNoSelection;
}

// The bridge protocol carries identifiers verbatim, so they are constrained to
// the LibreHardwareMonitor identifier grammar before they reach IPC.
inline bool IsValidSensorIdentifier(std::string_view identifier) {
    if (identifier.size() < 2 || identifier.size() > 255 || identifier.front() != '/')
        return false;
    for (const char character : identifier) {
        const unsigned char value = static_cast<unsigned char>(character);
        const bool allowed = (value >= '0' && value <= '9') || (value >= 'A' && value <= 'Z') ||
                             (value >= 'a' && value <= 'z') || value == '_' || value == '-' || value == '.' ||
                             value == '/';
        if (!allowed)
            return false;
    }
    return true;
}

inline bool IsReportableReading(const SensorCandidate& candidate, const MetricDefinition& metric) {
    return IsUsable(candidate, metric.rejectZero) && candidate.value <= metric.maximum &&
           IsValidSensorIdentifier(candidate.identifier);
}

}  // namespace ce::hardware_sensors::policy
