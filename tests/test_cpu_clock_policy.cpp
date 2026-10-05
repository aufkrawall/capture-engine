#include <gtest/gtest.h>

#include <limits>

#include "captureengine/sensors/cpu_clock_policy.h"

namespace {
using namespace ce::hardware_sensors::policy;

SensorCandidate Clock(const char* name, const char* identifier, float value, bool hasValue = true) {
    return {name, identifier, value, hasValue};
}
}  // namespace

TEST(CpuClockPolicyTest, HybridAverageUsesOnlyPerformanceCores) {
    const auto summary = SummarizeCpuClocks({
        Clock("P-Core #1", "/intelcpu/0/clock/1", 4800.0f),
        Clock("p-core #2", "/intelcpu/0/clock/2", 5200.0f),
        Clock("E-Core #1", "/intelcpu/0/clock/3", 3000.0f),
        Clock("E-Core #2", "/intelcpu/0/clock/4", 3200.0f),
        Clock("Cores (Average)", "/intelcpu/0/clock/5", 4050.0f),
        Clock("Core #9", "/intelcpu/0/clock/9", 1000.0f),
    });
    ASSERT_TRUE(summary.average.hasValue);
    EXPECT_FLOAT_EQ(summary.average.value, 5000.0f);
    EXPECT_EQ(summary.averagedCoreCount, 2u);
    EXPECT_EQ(summary.average.identifier, "/ce/cpu/clock/pcores_average_2");
}

TEST(CpuClockPolicyTest, ConventionalAverageExcludesBusEffectiveAndAggregateClocks) {
    const auto summary = SummarizeCpuClocks({
        Clock("Core #1", "/amdcpu/0/clock/1", 4000.0f),
        Clock("Core #2", "/amdcpu/0/clock/2", 5000.0f),
        Clock("Bus Speed", "/amdcpu/0/clock/0", 100.0f),
        Clock("Core #1 (Effective)", "/amdcpu/0/clock/3", 2000.0f),
        Clock("Cores (Average)", "/amdcpu/0/clock/4", 3000.0f),
    });
    ASSERT_TRUE(summary.average.hasValue);
    EXPECT_FLOAT_EQ(summary.average.value, 4500.0f);
    EXPECT_EQ(summary.averagedCoreCount, 2u);
}

TEST(CpuClockPolicyTest, MissingPerformanceClocksNeverFallBackToEfficientOrMixedClocks) {
    for (const float invalid : {0.0f, -1.0f, 20001.0f, std::numeric_limits<float>::infinity(),
                               std::numeric_limits<float>::quiet_NaN()}) {
        const auto summary = SummarizeCpuClocks({
            Clock("P-Core #1", "/intelcpu/0/clock/1", invalid),
            Clock("E-Core #1", "/intelcpu/0/clock/2", 3200.0f),
            Clock("Cores (Average)", "/intelcpu/0/clock/3", 4000.0f),
        });
        EXPECT_FALSE(summary.average.hasValue);
    }
    EXPECT_FALSE(SummarizeCpuClocks({Clock("E-Core #1", "/intelcpu/0/clock/1", 3200.0f)}).average.hasValue);
}

TEST(CpuClockPolicyTest, UsesOnlyReadableCoresAndKeepsAggregateOnlyBackends) {
    const auto summary = SummarizeCpuClocks({
        Clock("Core #1", "/cpu/0/clock/1", 4000.0f),
        Clock("Core #2", "/cpu/0/clock/2", 0.0f, false),
        Clock("Core #3", "invalid", 5000.0f),
    });
    ASSERT_TRUE(summary.average.hasValue);
    EXPECT_FLOAT_EQ(summary.average.value, 4000.0f);
    EXPECT_EQ(summary.averagedCoreCount, 1u);
    const auto aggregate = SummarizeCpuClocks({Clock("Cores (Average)", "/cpu/0/clock/1", 4300.0f)});
    EXPECT_TRUE(aggregate.average.hasValue);
    EXPECT_FLOAT_EQ(aggregate.average.value, 4300.0f);
    EXPECT_FALSE(SummarizeCpuClocks({Clock("Bus Speed", "/cpu/0/clock/0", 100.0f)}).average.hasValue);
}
