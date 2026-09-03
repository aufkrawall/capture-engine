#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "../captureengine/sensor_selection_policy.h"

// The selection rules used to live inside the first-party PowerShell bridge and
// were therefore untestable. They are native now, so every branch that a real
// machine can hit is pinned here.

namespace {

using namespace ce::hardware_sensors::policy;

SensorCandidate MakeCandidate(std::string name, std::string identifier, float value, bool hasValue = true) {
    SensorCandidate candidate;
    candidate.name = std::move(name);
    candidate.identifier = std::move(identifier);
    candidate.value = value;
    candidate.hasValue = hasValue;
    return candidate;
}

const MetricDefinition& MetricByKey(const char* key) {
    for (const MetricDefinition& metric : kMetrics) {
        if (std::string(metric.key) == key)
            return metric;
    }
    ADD_FAILURE() << "unknown metric " << key;
    return kMetrics[0];
}

size_t SelectFor(const std::vector<SensorCandidate>& candidates, const char* key,
                 const std::string& previousIdentifier = std::string()) {
    const MetricDefinition& metric = MetricByKey(key);
    return SelectAutomatic(candidates, metric.preferredNames, metric.preferredNameCount, metric.rejectZero,
                           previousIdentifier);
}

}  // namespace

TEST(SensorSelectionPolicyTest, MetricTableCoversTheWholeWireOrder) {
    ASSERT_EQ(kMetricCount, 9u);
    EXPECT_STREQ(kMetrics[0].key, "cpu_temperature");
    EXPECT_STREQ(kMetrics[4].key, "gpu_fan");
    EXPECT_STREQ(kMetrics[8].key, "gpu_voltage");
    // The fan is the only rail where a genuine zero must survive.
    for (size_t index = 0; index < kMetricCount; ++index) {
        SCOPED_TRACE(kMetrics[index].key);
        EXPECT_EQ(kMetrics[index].rejectZero, std::string(kMetrics[index].key) != "gpu_fan");
        EXPECT_GT(kMetrics[index].maximum, 0.0f);
        EXPECT_GT(kMetrics[index].preferredNameCount, 0u);
    }
}

TEST(SensorSelectionPolicyTest, PrefersTheHighestRankedExactNameOverAHotterSensor) {
    const std::vector<SensorCandidate> candidates = {
        MakeCandidate("CPU Core Max", "/amdcpu/0/temperature/9", 95.0f),
        MakeCandidate("Core (Tctl/Tdie)", "/amdcpu/0/temperature/2", 61.0f),
        MakeCandidate("CPU Package", "/amdcpu/0/temperature/0", 58.0f),
    };
    // "CPU Package" ranks ahead of "Core (Tctl/Tdie)" even though both are
    // usable and a third sensor reads much higher.
    EXPECT_EQ(SelectFor(candidates, "cpu_temperature"), 2u);
}

TEST(SensorSelectionPolicyTest, NumberedInstancesResolveToTheLowestIndex) {
    // Two idle fans a few RPM apart used to rename the selected identifier on
    // nearly every poll, so the reported RPM alternated between physical fans.
    const std::vector<SensorCandidate> candidates = {
        MakeCandidate("GPU Fan 2", "/gpu-nvidia/0/fan/2", 702.0f),
        MakeCandidate("GPU Fan 1", "/gpu-nvidia/0/fan/1", 698.0f),
    };
    EXPECT_EQ(SelectFor(candidates, "gpu_fan"), 1u);

    const std::vector<SensorCandidate> swapped = {
        MakeCandidate("GPU Fan 1", "/gpu-nvidia/0/fan/1", 690.0f),
        MakeCandidate("GPU Fan 2", "/gpu-nvidia/0/fan/2", 900.0f),
    };
    EXPECT_EQ(SelectFor(swapped, "gpu_fan"), 0u);
}

TEST(SensorSelectionPolicyTest, MatchesTheNumberedNameFormsHardwareActuallyUses) {
    uint32_t instance = 0;
    EXPECT_TRUE(MatchesNumberedName("GPU Fan 1", "GPU Fan", instance));
    EXPECT_EQ(instance, 1u);
    EXPECT_TRUE(MatchesNumberedName("Core #12", "Core", instance));
    EXPECT_EQ(instance, 12u);
    EXPECT_TRUE(MatchesNumberedName("core3", "Core", instance));
    EXPECT_EQ(instance, 3u);
    EXPECT_FALSE(MatchesNumberedName("GPU Fan", "GPU Fan", instance));
    EXPECT_FALSE(MatchesNumberedName("GPU Fan 1 (Effective)", "GPU Fan", instance));
    EXPECT_FALSE(MatchesNumberedName("Cores (Average)", "Core", instance));
    EXPECT_FALSE(MatchesNumberedName("GPU Fan 99999999999", "GPU Fan", instance));
}

TEST(SensorSelectionPolicyTest, KeepsAnUnrecognizedSensorItAlreadySelected) {
    const std::vector<SensorCandidate> candidates = {
        MakeCandidate("Vendor Rail A", "/gpu-amd/0/voltage/7", 0.90f),
        MakeCandidate("Vendor Rail B", "/gpu-amd/0/voltage/8", 0.95f),
    };
    // Without the sticky rule the highest reading wins and the identifier
    // changes as soon as the two rails cross.
    EXPECT_EQ(SelectFor(candidates, "gpu_voltage"), 1u);
    EXPECT_EQ(SelectFor(candidates, "gpu_voltage", "/gpu-amd/0/voltage/7"), 0u);
    // A remembered identifier that is no longer usable must not pin the metric.
    const std::vector<SensorCandidate> withDeadPrevious = {
        MakeCandidate("Vendor Rail A", "/gpu-amd/0/voltage/7", 0.0f),
        MakeCandidate("Vendor Rail B", "/gpu-amd/0/voltage/8", 0.95f),
    };
    EXPECT_EQ(SelectFor(withDeadPrevious, "gpu_voltage", "/gpu-amd/0/voltage/7"), 1u);
}

TEST(SensorSelectionPolicyTest, ZeroMeansUnreadableForEveryRailExceptTheFan) {
    const std::vector<SensorCandidate> unreadableCpu = {
        MakeCandidate("CPU Package", "/amdcpu/0/power/0", 0.0f),
        MakeCandidate("Core #1 (SMU)", "/amdcpu/0/power/1", 0.0f),
    };
    EXPECT_EQ(SelectFor(unreadableCpu, "cpu_package_power"), kNoSelection);

    // A stopped fan is a genuine reading and must still be reported.
    const std::vector<SensorCandidate> stoppedFan = {MakeCandidate("GPU Fan", "/gpu-nvidia/0/fan/0", 0.0f)};
    ASSERT_EQ(SelectFor(stoppedFan, "gpu_fan"), 0u);
    EXPECT_TRUE(IsReportableReading(stoppedFan[0], MetricByKey("gpu_fan")));
}

TEST(SensorSelectionPolicyTest, RejectsMissingNegativeAndOutOfRangeReadings) {
    const SensorCandidate missing = MakeCandidate("GPU Core", "/gpu-nvidia/0/temperature/0", 0.0f, false);
    EXPECT_FALSE(IsUsable(missing, true));
    EXPECT_FALSE(IsUsable(missing, false));
    const SensorCandidate negative = MakeCandidate("GPU Core", "/gpu-nvidia/0/temperature/0", -1.0f);
    EXPECT_FALSE(IsUsable(negative, false));
    const SensorCandidate absurd = MakeCandidate("GPU Core", "/gpu-nvidia/0/temperature/0", 4000.0f);
    EXPECT_TRUE(IsUsable(absurd, true));
    EXPECT_FALSE(IsReportableReading(absurd, MetricByKey("gpu_temperature")));
}

TEST(SensorSelectionPolicyTest, ExactSelectorIgnoresNameRankingAndCasing) {
    const std::vector<SensorCandidate> candidates = {
        MakeCandidate("GPU Core", "/gpu-nvidia/0/temperature/0", 41.0f),
        MakeCandidate("GPU Hot Spot", "/gpu-nvidia/0/temperature/2", 55.0f),
    };
    EXPECT_EQ(SelectExact(candidates, "/GPU-NVIDIA/0/TEMPERATURE/2"), 1u);
    EXPECT_EQ(SelectExact(candidates, "/gpu-nvidia/0/temperature/9"), kNoSelection);
}

TEST(SensorSelectionPolicyTest, ActiveGpuFollowsLoadAndHoldsStillOnTies) {
    std::vector<GpuLoadCandidate> candidates(2);
    candidates[0].identifier = "/gpu-nvidia/0";
    candidates[0].coreLoad = 3.0f;
    candidates[0].hasCoreLoad = true;
    candidates[1].identifier = "/gpu-intel/0";
    candidates[1].coreLoad = 71.0f;
    candidates[1].hasCoreLoad = true;
    EXPECT_EQ(SelectActiveGpu(candidates, ""), 1u);

    // Equal load must keep whichever adapter was already selected instead of
    // flipping the reported values between adapters every poll.
    candidates[1].coreLoad = 3.0f;
    EXPECT_EQ(SelectActiveGpu(candidates, "/gpu-intel/0"), 1u);
    EXPECT_EQ(SelectActiveGpu(candidates, "/gpu-nvidia/0"), 0u);
    EXPECT_EQ(SelectActiveGpu(candidates, ""), kNoSelection);
}

TEST(SensorSelectionPolicyTest, ActiveGpuFallsBackToASingleSilentAdapter) {
    std::vector<GpuLoadCandidate> single(1);
    single[0].identifier = "/gpu-amd/0";
    EXPECT_EQ(SelectActiveGpu(single, ""), 0u);

    std::vector<GpuLoadCandidate> silentPair(2);
    silentPair[0].identifier = "/gpu-amd/0";
    silentPair[1].identifier = "/gpu-intel/0";
    EXPECT_EQ(SelectActiveGpu(silentPair, ""), kNoSelection);
    EXPECT_EQ(SelectActiveGpu({}, ""), kNoSelection);
}

TEST(SensorSelectionPolicyTest, IdentifierGrammarBoundsWhatReachesIpc) {
    EXPECT_TRUE(IsValidSensorIdentifier("/gpu-nvidia/0/temperature/0"));
    EXPECT_TRUE(IsValidSensorIdentifier("/amdcpu/0/clock/1.2_3-4"));
    EXPECT_FALSE(IsValidSensorIdentifier(""));
    EXPECT_FALSE(IsValidSensorIdentifier("/"));
    EXPECT_FALSE(IsValidSensorIdentifier("gpu-nvidia/0"));
    EXPECT_FALSE(IsValidSensorIdentifier("/gpu nvidia/0"));
    EXPECT_FALSE(IsValidSensorIdentifier("/gpu\tnvidia/0"));
    EXPECT_FALSE(IsValidSensorIdentifier(std::string("/") + std::string(255, 'a')));
}

TEST(SensorSelectionPolicyTest, CaseInsensitiveComparisonStaysAsciiOnly) {
    EXPECT_TRUE(EqualsIgnoreCase("GPU Core", "gpu core"));
    EXPECT_FALSE(EqualsIgnoreCase("GPU Core", "GPU Cores"));
    EXPECT_FALSE(EqualsIgnoreCase("GPU Core", ""));
    EXPECT_TRUE(EqualsIgnoreCase("", ""));
}
