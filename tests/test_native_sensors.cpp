// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include <gtest/gtest.h>

#include "captureengine/sensors/native_sensors.h"
#include "common/config/config.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

// Native hardware sensors (libcengine 11, part 7): the source plan, the NVML provider against a fake driver table,
// the CPU clock provider and the merge with the LibreHardwareMonitor bridge.
namespace {
using namespace ce::hardware_sensors;

constexpr size_t kCpuTemperature = 0;
constexpr size_t kGpuTemperature = 1;
constexpr size_t kGpuPackagePower = 3;
constexpr size_t kGpuFan = 4;
constexpr size_t kCpuCoreClock = 5;
constexpr size_t kGpuCoreClock = 6;
constexpr size_t kGpuMemoryClock = 7;
constexpr size_t kGpuVoltage = 8;
constexpr size_t kCpuMaxCoreClock = 9;

constexpr MetricMask kNvmlMetrics =
    MetricBit(kGpuTemperature) | MetricBit(kGpuPackagePower) | MetricBit(kGpuCoreClock) | MetricBit(kGpuMemoryClock);
constexpr MetricMask kCpuClockMetrics = MetricBit(kCpuCoreClock) | MetricBit(kCpuMaxCoreClock);

HardwareSensorsConfig Config(const char* backend) {
    HardwareSensorsConfig config;
    config.backend = backend;
    return config;
}

TEST(NativeSensorPlanTest, AutoReadsNativelyWhatAProviderServesAndBridgesTheRest) {
    HardwareSensorsConfig bridge;
    const SourcePlan plan = PlanSources(Config("auto"), {kNvmlMetrics, kCpuClockMetrics}, &bridge);
    EXPECT_EQ(plan.sources[kGpuTemperature], MetricSource::Native);
    EXPECT_EQ(plan.provider[kGpuTemperature], 0);
    EXPECT_EQ(plan.sources[kCpuCoreClock], MetricSource::Native);
    EXPECT_EQ(plan.provider[kCpuCoreClock], 1);
    EXPECT_EQ(plan.sources[kCpuMaxCoreClock], MetricSource::Native);
    EXPECT_EQ(plan.sources[kCpuTemperature], MetricSource::Bridge);
    EXPECT_EQ(plan.sources[kGpuFan], MetricSource::Bridge);
    EXPECT_EQ(plan.sources[kGpuVoltage], MetricSource::Bridge);
    EXPECT_TRUE(plan.bridgeNeeded);
    // The bridge reads only what is left.
    EXPECT_EQ(bridge.gpuTemperature, "off");
    EXPECT_EQ(bridge.cpuCoreClock, "off");
    EXPECT_EQ(bridge.cpuTemperature, "auto");
    EXPECT_EQ(bridge.gpuVoltage, "auto");
}

TEST(NativeSensorPlanTest, NoBridgeWhenEverythingRequestedIsNative) {
    HardwareSensorsConfig config = Config("auto");
    config.cpuTemperature = "off";
    config.cpuPackagePower = "off";
    config.gpuFan = "off";
    config.gpuVoltage = "off";
    HardwareSensorsConfig bridge;
    const SourcePlan plan = PlanSources(config, {kNvmlMetrics, kCpuClockMetrics}, &bridge);
    EXPECT_FALSE(plan.bridgeNeeded);
}

TEST(NativeSensorPlanTest, NativeNeverBridgesAndLhmNeverReadsNatively) {
    HardwareSensorsConfig bridge;
    const SourcePlan native = PlanSources(Config("native"), {kNvmlMetrics}, &bridge);
    EXPECT_FALSE(native.bridgeNeeded);
    EXPECT_EQ(native.sources[kGpuTemperature], MetricSource::Native);
    EXPECT_EQ(native.sources[kCpuTemperature], MetricSource::Unavailable);
    const SourcePlan lhm = PlanSources(Config("lhm"), {kNvmlMetrics}, &bridge);
    EXPECT_EQ(lhm.sources[kGpuTemperature], MetricSource::Bridge);
    EXPECT_EQ(bridge.gpuTemperature, "auto");
}

TEST(NativeSensorPlanTest, AnExactLibreHardwareMonitorIdentifierStaysOnTheBridge) {
    HardwareSensorsConfig config = Config("auto");
    config.gpuTemperature = "/gpu-nvidia/0/temperature/0";
    HardwareSensorsConfig bridge;
    const SourcePlan plan = PlanSources(config, {kNvmlMetrics}, &bridge);
    EXPECT_EQ(plan.sources[kGpuTemperature], MetricSource::Bridge);
    EXPECT_EQ(bridge.gpuTemperature, "/gpu-nvidia/0/temperature/0");
}

TEST(NativeSensorPlanTest, DisabledSensorsReadNothing) {
    HardwareSensorsConfig config = Config("auto");
    config.enabled = "off";
    HardwareSensorsConfig bridge;
    const SourcePlan plan = PlanSources(config, {kNvmlMetrics, kCpuClockMetrics}, &bridge);
    for (size_t metric = 0; metric < ce::hardware_sensors::policy::kMetricCount; ++metric)
        EXPECT_EQ(plan.sources[metric], MetricSource::Off) << metric;
    EXPECT_FALSE(plan.bridgeNeeded);
}

TEST(NativeSensorPlanTest, TheDescriptionNamesEverySource) {
    HardwareSensorsConfig bridge;
    const SourcePlan plan = PlanSources(Config("auto"), {kNvmlMetrics}, &bridge);
    const std::string text = DescribeSourcePlan(plan, {"nvml"});
    EXPECT_NE(text.find("gpu_temperature=nvml"), std::string::npos) << text;
    EXPECT_NE(text.find("cpu_temperature=lhm"), std::string::npos) << text;
}

// A fake NVML: a fixed reading per call; a refused metric returns NVML_ERROR_NOT_SUPPORTED (3).
struct FakeNvml {
    static inline int initResult = 0;
    static inline unsigned int deviceCount = 1;
    static inline bool powerSupported = true;
    static inline int shutdowns = 0;
    static int Init() { return initResult; }
    static int Shutdown() {
        ++shutdowns;
        return 0;
    }
    static int Count(unsigned int* count) {
        *count = deviceCount;
        return 0;
    }
    static int Handle(unsigned int, void** device) {
        *device = reinterpret_cast<void*>(0x1);
        return 0;
    }
    static int Name(void*, char* name, unsigned int size) {
        strncpy_s(name, size, "NVIDIA Test GPU", _TRUNCATE);
        return 0;
    }
    static int Temperature(void*, int, unsigned int* value) {
        *value = 61;
        return 0;
    }
    static int Power(void*, unsigned int* value) {
        if (!powerSupported)
            return 3;
        *value = 215500;  // mW
        return 0;
    }
    static int Clock(void*, int type, unsigned int* value) {
        *value = type == 0 ? 2610 : 10501;
        return 0;
    }
    static NvmlApi Api() {
        NvmlApi api;
        api.init = &Init;
        api.shutdown = &Shutdown;
        api.deviceGetCount = &Count;
        api.deviceGetHandleByIndex = &Handle;
        api.deviceGetName = &Name;
        api.deviceGetTemperature = &Temperature;
        api.deviceGetPowerUsage = &Power;
        api.deviceGetClockInfo = &Clock;
        return api;
    }
};

TEST(NativeSensorNvmlTest, ReadsTemperaturePowerAndClocksOfGpuZero) {
    FakeNvml::initResult = 0;
    FakeNvml::deviceCount = 1;
    FakeNvml::powerSupported = true;
    std::unique_ptr<NativeSensorProvider> provider = OpenNvmlProvider(FakeNvml::Api(), 1);
    ASSERT_TRUE(provider);
    EXPECT_STREQ(provider->Name(), "nvml");
    EXPECT_EQ(provider->Metrics(), kNvmlMetrics);
    HardwareSensorSnapshot snapshot;
    provider->Sample(snapshot);
    EXPECT_TRUE(snapshot.gpuTemperature.valid);
    EXPECT_FLOAT_EQ(snapshot.gpuTemperature.value, 61.0f);
    EXPECT_FLOAT_EQ(snapshot.gpuPackagePower.value, 215.5f);  // watts
    EXPECT_FLOAT_EQ(snapshot.gpuCoreClock.value, 2610.0f);
    EXPECT_FLOAT_EQ(snapshot.gpuMemoryClock.value, 10501.0f);
    EXPECT_EQ(snapshot.gpuTemperature.identifier, "native:nvml:NVIDIA Test GPU");
    EXPECT_FALSE(snapshot.gpuFan.valid) << "not read natively";
}

TEST(NativeSensorNvmlTest, AMetricTheDriverRefusesIsLeftToTheBridge) {
    FakeNvml::initResult = 0;
    FakeNvml::deviceCount = 1;
    FakeNvml::powerSupported = false;
    std::unique_ptr<NativeSensorProvider> provider = OpenNvmlProvider(FakeNvml::Api(), 1);
    ASSERT_TRUE(provider);
    EXPECT_EQ(provider->Metrics() & MetricBit(kGpuPackagePower), 0u);
    FakeNvml::powerSupported = true;
}

TEST(NativeSensorNvmlTest, NoNvidiaGpuOrNoDriverMeansNoProvider) {
    FakeNvml::shutdowns = 0;
    FakeNvml::initResult = 9;  // NVML_ERROR_DRIVER_NOT_LOADED
    EXPECT_FALSE(OpenNvmlProvider(FakeNvml::Api(), 1));
    FakeNvml::initResult = 0;
    FakeNvml::deviceCount = 0;
    EXPECT_FALSE(OpenNvmlProvider(FakeNvml::Api(), 1));
    EXPECT_EQ(FakeNvml::shutdowns, 1) << "an initialized NVML is shut down again";
    FakeNvml::deviceCount = 1;
    EXPECT_FALSE(OpenNvmlProvider(NvmlApi{}, 1)) << "an empty table (no nvml.dll)";
}

// With several GPUs LibreHardwareMonitor keeps choosing the busiest one, as before: NVML serves only a machine whose
// one hardware GPU is its one NVIDIA GPU.
TEST(NativeSensorNvmlTest, SeveralGpusLeaveTheChoiceToTheBridge) {
    FakeNvml::initResult = 0;
    FakeNvml::powerSupported = true;
    FakeNvml::shutdowns = 0;
    FakeNvml::deviceCount = 1;
    EXPECT_FALSE(OpenNvmlProvider(FakeNvml::Api(), 2)) << "an NVIDIA GPU beside an integrated one";
    FakeNvml::deviceCount = 2;
    EXPECT_FALSE(OpenNvmlProvider(FakeNvml::Api(), 2)) << "two NVIDIA GPUs";
    EXPECT_EQ(FakeNvml::shutdowns, 2) << "every refused open shuts NVML down again";
    FakeNvml::deviceCount = 1;
    EXPECT_TRUE(OpenNvmlProvider(FakeNvml::Api(), 1));
}

TEST(NativeSensorCpuClockTest, ProcessorInstancesParseOnlyGroupAndNumber) {
    uint16_t group = 0;
    uint8_t number = 0;
    ASSERT_TRUE(ParseProcessorInstance(L"1,17", &group, &number));
    EXPECT_EQ(group, 1u);
    EXPECT_EQ(number, 17u);
    for (const wchar_t* name : {L"_Total", L"0,_Total", L"0,", L",3", L"0,3x", L"0,300", L""})
        EXPECT_FALSE(ParseProcessorInstance(name, &group, &number)) << static_cast<const void*>(name);
    EXPECT_FALSE(ParseProcessorInstance(nullptr, &group, &number));
}

// Effective clock = nominal x performance. A hybrid CPU averages its performance cores (the highest efficiency
// class), like the bridge's P-core average; the maximum covers every core.
TEST(NativeSensorCpuClockTest, AHybridCpuAveragesItsPerformanceCoresAndTakesTheMaximumOfAll) {
    const std::vector<LogicalProcessorPerformance> hybrid = {
        {0, 0, 1, 200.0},  // P-core at 2x nominal
        {0, 1, 1, 180.0},  // P-core
        {0, 2, 0, 250.0},  // E-core: fastest of all, but not in the average
        {0, 3, 0, 100.0},
    };
    const CpuClockSample sample = ReduceCpuClocks(2000.0, hybrid);
    ASSERT_TRUE(sample.valid);
    EXPECT_FLOAT_EQ(sample.averageMHz, 3800.0f);
    EXPECT_FLOAT_EQ(sample.maximumMHz, 5000.0f);

    const std::vector<LogicalProcessorPerformance> uniform = {{0, 0, 0, 150.0}, {0, 1, 0, 50.0}};
    const CpuClockSample all = ReduceCpuClocks(4000.0, uniform);
    ASSERT_TRUE(all.valid);
    EXPECT_FLOAT_EQ(all.averageMHz, 4000.0f) << "one class: every core counts";
    EXPECT_FLOAT_EQ(all.maximumMHz, 6000.0f);

    EXPECT_FALSE(ReduceCpuClocks(0.0, uniform).valid) << "no nominal frequency, no clock";
    EXPECT_FALSE(ReduceCpuClocks(4000.0, {}).valid);
    EXPECT_FALSE(ReduceCpuClocks(4000.0, {{0, 0, 0, 0.0}}).valid) << "a zero reading is no reading";
}

TEST(NativeSensorCpuClockTest, TheFirstRateSampleIsInvalidThenAverageAndMaximumArrive) {
    int reads = 0;
    std::unique_ptr<NativeSensorProvider> provider = OpenCpuClockProvider([&reads]() {
        CpuClockSample sample;
        if (++reads > 1) {
            sample.valid = true;
            sample.averageMHz = 4100.0f;
            sample.maximumMHz = 5600.0f;
        }
        return sample;
    });
    ASSERT_TRUE(provider);
    HardwareSensorSnapshot first;
    provider->Sample(first);
    EXPECT_FALSE(first.cpuCoreClock.valid);
    HardwareSensorSnapshot second;
    provider->Sample(second);
    EXPECT_FLOAT_EQ(second.cpuCoreClock.value, 4100.0f);
    EXPECT_FLOAT_EQ(second.cpuMaxCoreClock.value, 5600.0f);
}

class FixedProvider final : public NativeSensorProvider {
public:
    const char* Name() const override { return "fixed"; }
    MetricMask Metrics() const override { return MetricBit(kGpuTemperature); }
    void Sample(HardwareSensorSnapshot& snapshot) override {
        snapshot.gpuTemperature.value = 55.0f;
        snapshot.gpuTemperature.valid = true;
        snapshot.gpuTemperature.identifier = "native:fixed";
        snapshot.cpuTemperature.value = 99.0f;  // not this provider's metric: must not leak into the merge
        snapshot.cpuTemperature.valid = true;
    }
};

TEST(NativeSensorSourcesTest, OnlyAssignedNativeMetricsReachTheSnapshotAndItIsFresh) {
    HardwareSensorsConfig config = Config("native");
    HardwareSensorSources sources(config, []() {
        std::vector<std::unique_ptr<NativeSensorProvider>> providers;
        providers.push_back(std::make_unique<FixedProvider>());
        return providers;
    });
    EXPECT_TRUE(sources.Start());
    EXPECT_FALSE(sources.Plan().bridgeNeeded);
    const HardwareSensorSnapshot snapshot = sources.GetSnapshot();
    EXPECT_TRUE(snapshot.gpuTemperature.valid);
    EXPECT_FLOAT_EQ(snapshot.gpuTemperature.value, 55.0f);
    EXPECT_FALSE(snapshot.cpuTemperature.valid);
    EXPECT_GT(snapshot.receivedTickMs, 0u);
    EXPECT_GT(snapshot.sequence, 0u);
}

}  // namespace
