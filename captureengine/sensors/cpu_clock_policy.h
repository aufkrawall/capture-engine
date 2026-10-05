#pragma once

#include "sensor_selection_policy.h"

namespace ce::hardware_sensors::policy {

struct CpuClockSummary {
    SensorCandidate average;
    size_t coreCount = 0;
    size_t averagedCoreCount = 0;
    bool classifiedCores = false;
};

// LHM names physical core clocks Core #N, P-Core #N or E-Core #N.
// Effective clocks, bus clocks and package aggregates are separate readings.
inline CpuClockSummary SummarizeCpuClocks(const std::vector<SensorCandidate>& candidates) {
    CpuClockSummary summary;
    uint32_t instance = 0;
    for (const SensorCandidate& candidate : candidates) {
        if (MatchesNumberedName(candidate.name, "P-Core", instance) ||
            MatchesNumberedName(candidate.name, "E-Core", instance)) {
            summary.classifiedCores = true;
            break;
        }
    }
    double total = 0.0;
    for (const SensorCandidate& candidate : candidates) {
        const bool performance = MatchesNumberedName(candidate.name, "P-Core", instance);
        const bool efficient = MatchesNumberedName(candidate.name, "E-Core", instance);
        const bool conventional = MatchesNumberedName(candidate.name, "Core", instance) ||
                                  MatchesNumberedName(candidate.name, "CPU Core", instance) ||
                                  EqualsIgnoreCase(candidate.name, "CPU Core") ||
                                  EqualsIgnoreCase(candidate.name, "Core");
        if (!performance && !efficient && !conventional)
            continue;
        ++summary.coreCount;
        if ((summary.classifiedCores && !performance) || !IsReportableReading(candidate, kMetrics[kCpuCoreClockMetric]))
            continue;
        total += candidate.value;
        ++summary.averagedCoreCount;
    }
    if (summary.averagedCoreCount != 0) {
        summary.average.name = summary.classifiedCores ? "P-Cores (Average)" : "Cores (Average)";
        summary.average.identifier = std::string("/ce/cpu/clock/") +
                                     (summary.classifiedCores ? "pcores_average_" : "cores_average_") +
                                     std::to_string(summary.averagedCoreCount);
        summary.average.value = static_cast<float>(total / static_cast<double>(summary.averagedCoreCount));
        summary.average.hasValue = true;
    } else if (summary.coreCount == 0) {
        // Older backends may expose only an aggregate. Never substitute that
        // mixed reading when classified cores exist but P-cores are unreadable.
        for (const SensorCandidate& candidate : candidates) {
            if (EqualsIgnoreCase(candidate.name, "Cores (Average)") &&
                IsReportableReading(candidate, kMetrics[kCpuCoreClockMetric])) {
                summary.average = candidate;
                break;
            }
        }
    }
    return summary;
}

}  // namespace ce::hardware_sensors::policy
