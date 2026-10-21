// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

// Native hardware sensors (libcengine 11, part 7): GPU values straight from the NVIDIA driver and CPU clocks from
// Windows, without LibreHardwareMonitor's .NET bridge. HardwareSensorSources combines them with the bridge:
// [HardwareSensors] backend=auto reads natively what a provider on this machine can serve and hands only the rest to
// LibreHardwareMonitor (which is not started at all when nothing is left); native never starts the bridge; lhm is the
// previous behavior. Every value keeps the snapshot contract of sensor_plugin.h (invalid, never a fake zero).

#include "sensor_plugin.h"
#include "sensor_selection_policy.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct HardwareSensorsConfig;

namespace ce::hardware_sensors {

// Bit per metric index of policy::kMetrics (cpu_max_core_clock included).
using MetricMask = uint32_t;
inline constexpr MetricMask MetricBit(size_t metric) { return MetricMask{1} << metric; }

class NativeSensorProvider {
public:
    virtual ~NativeSensorProvider() = default;
    virtual const char* Name() const = 0;
    // The metrics this provider reads on this machine (decided when it opened).
    virtual MetricMask Metrics() const = 0;
    // Writes every metric it reads into `snapshot`; a failed read leaves that value invalid.
    virtual void Sample(HardwareSensorSnapshot& snapshot) = 0;
};

// NVIDIA through NVML (nvml.dll from the driver): temperature, board power, core and memory clock. The function table
// is injectable for tests; LoadSystemNvml() resolves the driver's library or returns an empty table.
struct NvmlApi {
    void* module = nullptr;
    int (*init)() = nullptr;
    int (*shutdown)() = nullptr;
    int (*deviceGetCount)(unsigned int*) = nullptr;
    int (*deviceGetHandleByIndex)(unsigned int, void**) = nullptr;
    int (*deviceGetName)(void*, char*, unsigned int) = nullptr;
    int (*deviceGetTemperature)(void*, int, unsigned int*) = nullptr;
    int (*deviceGetPowerUsage)(void*, unsigned int*) = nullptr;
    int (*deviceGetClockInfo)(void*, int, unsigned int*) = nullptr;
};
NvmlApi LoadSystemNvml();
// Serves only a machine whose one hardware GPU is the one NVIDIA GPU: with several GPUs the bridge keeps choosing the
// busiest, as it always has. `hardwareAdapters` counts DXGI adapters that are not software (CountHardwareAdapters).
std::unique_ptr<NativeSensorProvider> OpenNvmlProvider(const NvmlApi& api, unsigned int hardwareAdapters);
unsigned int CountHardwareAdapters();

// CPU clocks from Windows performance counters: Processor Frequency (nominal MHz) x % Processor Performance per
// logical processor, the effective clock Task Manager shows; no driver and no elevation needed. Like the bridge's
// policy (cpu_clock_policy.h) the average covers the performance cores of a hybrid CPU, the maximum every core.
struct CpuClockSample {
    bool valid = false;
    float averageMHz = 0.0f;
    float maximumMHz = 0.0f;
};
struct LogicalProcessorPerformance {
    uint16_t group = 0;
    uint8_t number = 0;
    uint8_t efficiencyClass = 0;  // higher = faster cores (GetSystemCpuSetInformation)
    double performancePercent = 0.0;
};
// "<group>,<number>" as the Processor Information counter names a logical processor; false for totals.
bool ParseProcessorInstance(const wchar_t* name, uint16_t* group, uint8_t* number);
CpuClockSample ReduceCpuClocks(double nominalMHz, const std::vector<LogicalProcessorPerformance>& processors);
std::unique_ptr<NativeSensorProvider> OpenCpuClockProvider(std::function<CpuClockSample()> read = {});

// Where each metric comes from.
enum class MetricSource : uint8_t { Off, Native, Bridge, Unavailable };
struct SourcePlan {
    std::array<MetricSource, policy::kMetricCount> sources{};
    std::array<int, policy::kMetricCount> provider{};  // index into the providers for Native, else -1
    bool bridgeNeeded = false;
};

// Pure: backend (auto|native|lhm), the requested selectors and what the providers serve -> the plan, plus the
// selectors handed to the bridge (natively served metrics are turned off there).
SourcePlan PlanSources(const HardwareSensorsConfig& config, const std::vector<MetricMask>& providerMetrics,
                       HardwareSensorsConfig* bridgeSelectors);
std::string DescribeSourcePlan(const SourcePlan& plan, const std::vector<std::string>& providerNames);

class HardwareSensorSources {
public:
    using ProviderFactory = std::function<std::vector<std::unique_ptr<NativeSensorProvider>>()>;
    explicit HardwareSensorSources(const HardwareSensorsConfig& config, ProviderFactory factory = {});
    ~HardwareSensorSources();
    HardwareSensorSources(const HardwareSensorSources&) = delete;
    HardwareSensorSources& operator=(const HardwareSensorSources&) = delete;

    // Opens the native providers and, when the plan needs it, the bridge. True when any source runs.
    bool Start();
    void Poll();
    // Samples the native providers (once per call) over the bridge's latest snapshot.
    HardwareSensorSnapshot GetSnapshot() const;
    const SourcePlan& Plan() const { return plan_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    SourcePlan plan_;
};

}  // namespace ce::hardware_sensors
