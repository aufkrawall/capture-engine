#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "../captureengine/host_metrics_policy.h"
#include "../common/shared_defs.h"
#include "source_fragment_reader.h"

namespace {

using scan_host::metrics_policy::AdapterResolutionSource;
using scan_host::metrics_policy::GpuEngineLoadSample;
using scan_host::metrics_policy::GpuEngineSample;
using scan_host::metrics_policy::ParseGpuEngineKey;
using scan_host::metrics_policy::ResolveAdapterGpuLoadPercent;

// Portal RTX 20260914_120049: an in-game step from 3x to 4x multi-frame
// generation cut the base render rate from 47.8 to 36.0 groups per second
// (measured from the present bursts in perf_metrics_1620.csv) and the reported
// GPU load did not move off ~100%. The engines run concurrently, so adding a
// game's raster on 3D to a frame generator's work on compute produced a number
// that was already past the 100 clamp - and the clamp is where the freed
// headroom went.
TEST(HostMetricsPolicyTest, AdapterGpuLoadIsTheBusiestEngineNotTheSumOfAllOfThem) {
    const uint64_t render = ParseGpuEngineKey("pid_100_luid_0x0_0x1_phys_0_eng_0_engtype_3d");
    const uint64_t compute = ParseGpuEngineKey("pid_100_luid_0x0_0x1_phys_0_eng_1_engtype_compute");
    const uint64_t copy = ParseGpuEngineKey("pid_100_luid_0x0_0x1_phys_0_eng_2_engtype_copy");
    ASSERT_NE(render, compute);
    ASSERT_NE(render, copy);
    ASSERT_NE(compute, copy);

    // Three engines at 60/55/20 is a GPU that is 60% busy, not a saturated one.
    EXPECT_DOUBLE_EQ(ResolveAdapterGpuLoadPercent({{render, 60.0}, {compute, 55.0}, {copy, 20.0}}), 60.0);
    // Freeing a quarter of the render engine must be visible, which is the whole
    // point: the old sum stayed pinned at the clamp across this change.
    EXPECT_DOUBLE_EQ(ResolveAdapterGpuLoadPercent({{render, 45.0}, {compute, 62.0}, {copy, 20.0}}), 62.0);

    // Processes time-share one engine, so within an engine the sum is correct.
    const uint64_t otherProcessRender = ParseGpuEngineKey("pid_200_luid_0x0_0x1_phys_0_eng_0_engtype_3d");
    EXPECT_EQ(otherProcessRender, render) << "the engine identity must not depend on which process used it";
    EXPECT_DOUBLE_EQ(ResolveAdapterGpuLoadPercent({{render, 40.0}, {otherProcessRender, 35.0}, {compute, 50.0}}),
                     75.0);

    // A second physical engine of the same type is its own engine.
    const uint64_t secondCopy = ParseGpuEngineKey("pid_100_luid_0x0_0x1_phys_1_eng_2_engtype_copy");
    EXPECT_NE(secondCopy, copy);
    EXPECT_DOUBLE_EQ(ResolveAdapterGpuLoadPercent({{copy, 30.0}, {secondCopy, 30.0}}), 30.0);
}

TEST(HostMetricsPolicyTest, AdapterGpuLoadStaysInRangeOnDegenerateSamples) {
    EXPECT_DOUBLE_EQ(ResolveAdapterGpuLoadPercent({}), 0.0);

    const uint64_t render = ParseGpuEngineKey("pid_100_luid_0x0_0x1_phys_0_eng_0_engtype_3d");
    // PDH can report a saturated engine slightly over 100, and a negative or
    // non-finite reading is not a utilization at all.
    EXPECT_DOUBLE_EQ(ResolveAdapterGpuLoadPercent({{render, 103.7}}), 100.0);
    EXPECT_DOUBLE_EQ(ResolveAdapterGpuLoadPercent({{render, -5.0}}), 0.0);
    EXPECT_DOUBLE_EQ(
        ResolveAdapterGpuLoadPercent({{render, std::numeric_limits<double>::quiet_NaN()}, {render, 12.0}}), 12.0);

    // An instance with no `phys_` token is one shared bucket, never an engine
    // per instance - unkeyed readings must not inflate the busiest engine.
    const uint64_t unkeyed = ParseGpuEngineKey("pid_100_luid_0x0_0x1");
    EXPECT_EQ(unkeyed, ParseGpuEngineKey("pid_200_luid_0x0_0x1"));
    EXPECT_NE(unkeyed, render);
}

std::string ReadProjectSource(const std::filesystem::path& relativePath) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / relativePath);
}

GpuEngineSample Sample(uint32_t pid, int64_t luid, double utilization, bool video = false) {
    return {pid, luid, utilization, video};
}

}  // namespace

TEST(HostMetricsPolicyTest, ParsesPidAndLuidCaseInsensitivelyFromGpuEngineInstances) {
    GpuEngineSample sample;
    ASSERT_TRUE(scan_host::metrics_policy::ParseGpuEngineSample(
        "pid_6844_luid_0x00000000_0x0000BAB1_phys_0_eng_3_engtype_3D", 17.5, sample));
    EXPECT_EQ(sample.processId, 6844u);
    EXPECT_EQ(sample.adapterLuid, 0xBAB1);
    EXPECT_DOUBLE_EQ(sample.utilization, 17.5);
    EXPECT_FALSE(sample.videoEngine);

    ASSERT_TRUE(scan_host::metrics_policy::ParseGpuEngineSample(
        "PID_6844_LUID_0X00000000_0X0000bab1_phys_0_eng_7_ENGTYPE_VideoDecode", 4.0, sample));
    EXPECT_TRUE(sample.videoEngine);

    ASSERT_TRUE(scan_host::metrics_policy::ParseGpuEngineSample(
        "pid_9_luid_0x89ABCDEF_0x01234567_phys_0_eng_1_engtype_Compute", 1.0, sample));
    EXPECT_EQ(static_cast<uint64_t>(sample.adapterLuid), 0x89ABCDEF01234567ull);
}

TEST(HostMetricsPolicyTest, ExactHookLuidAlwaysWinsOverProcessInference) {
    const std::vector<GpuEngineSample> samples = {Sample(42, 0x2222, 100.0)};
    const auto resolution = scan_host::metrics_policy::ResolveAdapterLuid(0x1111, 42, samples, 0x2222);
    EXPECT_EQ(resolution.adapterLuid, 0x1111);
    EXPECT_EQ(resolution.source, AdapterResolutionSource::HookLuid);
}

TEST(HostMetricsPolicyTest, ExactCaptureDeviceLuidRetainsNonInjectProvenance) {
    const std::vector<GpuEngineSample> samples = {Sample(42, 0x2222, 100.0)};
    const auto resolution = scan_host::metrics_policy::ResolveAdapterLuid(
        0x1111, 42, samples, 0x2222, AdapterResolutionSource::CaptureDeviceLuid);
    EXPECT_EQ(resolution.adapterLuid, 0x1111);
    EXPECT_EQ(resolution.source, AdapterResolutionSource::CaptureDeviceLuid);
}

TEST(HostMetricsPolicyTest, GraphicsLuidAcceptsProfileSourceOrItsDirectRendererChild) {
    EXPECT_TRUE(scan_host::metrics_policy::IsGpuTelemetryPublisherEligible(
        /*targetPid=*/42, /*publisherPid=*/42, /*publisherParentPid=*/0));
    EXPECT_TRUE(scan_host::metrics_policy::IsGpuTelemetryPublisherEligible(
        /*targetPid=*/42, /*publisherPid=*/84, /*publisherParentPid=*/42));
}

TEST(HostMetricsPolicyTest, GraphicsLuidRejectsMissingAndUnrelatedPublishers) {
    EXPECT_FALSE(scan_host::metrics_policy::IsGpuTelemetryPublisherEligible(0, 42, 0));
    EXPECT_FALSE(scan_host::metrics_policy::IsGpuTelemetryPublisherEligible(42, 0, 0));
    EXPECT_FALSE(scan_host::metrics_policy::IsGpuTelemetryPublisherEligible(
        /*targetPid=*/42, /*publisherPid=*/84, /*publisherParentPid=*/41));
}

TEST(HostMetricsPolicyTest, MissingLegacyLuidResolvesFromTargetProcessEngines) {
    const std::vector<GpuEngineSample> samples = {
        Sample(7, 0x1111, 80.0),
        Sample(42, 0xBAB1, 0.0),
        Sample(42, 0xBAB1, 15.0),
    };
    const auto resolution = scan_host::metrics_policy::ResolveAdapterLuid(0, 42, samples);
    EXPECT_EQ(resolution.adapterLuid, 0xBAB1);
    EXPECT_EQ(resolution.source, AdapterResolutionSource::ProcessGpuEngine);
}

TEST(HostMetricsPolicyTest, MultiGpuProcessSelectsActiveNonVideoAdapter) {
    const std::vector<GpuEngineSample> samples = {
        Sample(42, 0x1111, 0.0),
        Sample(42, 0x1111, 30.0, true),
        Sample(42, 0x2222, 65.0),
        Sample(42, 0x2222, 12.0),
    };
    const auto resolution = scan_host::metrics_policy::ResolveAdapterLuid(0, 42, samples, 0x1111);
    EXPECT_EQ(resolution.adapterLuid, 0x2222);
}

TEST(HostMetricsPolicyTest, ValidZeroLoadKeepsStableProcessAdapter) {
    const std::vector<GpuEngineSample> samples = {
        Sample(42, 0x1111, 0.0),
        Sample(42, 0x2222, 0.0),
    };
    const auto resolution = scan_host::metrics_policy::ResolveAdapterLuid(0, 42, samples, 0x2222);
    EXPECT_EQ(resolution.adapterLuid, 0x2222);
    EXPECT_EQ(resolution.source, AdapterResolutionSource::ProcessGpuEngine);

    const auto retained = scan_host::metrics_policy::ResolveAdapterLuid(0, 42, {}, 0x2222);
    EXPECT_EQ(retained.adapterLuid, 0x2222);
    EXPECT_EQ(retained.source, AdapterResolutionSource::RetainedProcessGpuEngine);
}

TEST(HostMetricsPolicyTest, AmbiguousInitialZeroLoadDoesNotGuessAnAdapter) {
    const std::vector<GpuEngineSample> samples = {
        Sample(42, 0x1111, 0.0),
        Sample(42, 0x2222, 0.0),
    };
    const auto resolution = scan_host::metrics_policy::ResolveAdapterLuid(0, 42, samples);
    EXPECT_EQ(resolution.adapterLuid, 0);
    EXPECT_EQ(resolution.source, AdapterResolutionSource::Unavailable);
}

TEST(HostMetricsPolicyTest, MissingPriorAdapterIsRetainedUntilAnotherAdapterHasLoad) {
    const std::vector<GpuEngineSample> idleReplacement = {Sample(42, 0x2222, 0.0)};
    const auto retained = scan_host::metrics_policy::ResolveAdapterLuid(0, 42, idleReplacement, 0x1111);
    EXPECT_EQ(retained.adapterLuid, 0x1111);
    EXPECT_EQ(retained.source, AdapterResolutionSource::RetainedProcessGpuEngine);

    const std::vector<GpuEngineSample> activeReplacement = {Sample(42, 0x2222, 12.0)};
    const auto switched = scan_host::metrics_policy::ResolveAdapterLuid(0, 42, activeReplacement, 0x1111);
    EXPECT_EQ(switched.adapterLuid, 0x2222);
    EXPECT_EQ(switched.source, AdapterResolutionSource::ProcessGpuEngine);
}

TEST(HostMetricsPolicyTest, EvidenceFromOldPidCannotResolveReplacementSource) {
    const std::vector<GpuEngineSample> oldProcessSamples = {Sample(41, 0x1111, 90.0)};
    const auto resolution = scan_host::metrics_policy::ResolveAdapterLuid(0, 42, oldProcessSamples, 0);
    EXPECT_EQ(resolution.adapterLuid, 0);
    EXPECT_EQ(resolution.source, AdapterResolutionSource::Unavailable);
}

TEST(HostMetricsPolicyTest, ProcessorUsageRejectsCounterRegressionAndIdleUnderflow) {
    EXPECT_EQ(scan_host::metrics_policy::CalculateProcessorUsagePercent(100, 200, 300, 90, 240, 360), 0u);
    EXPECT_EQ(scan_host::metrics_policy::CalculateProcessorUsagePercent(100, 200, 300, 180, 240, 320), 0u);
}

TEST(HostMetricsPolicyTest, ProcessorUsageUsesKernelPlusUserIncludingIdle) {
    // total delta=100, idle delta=25, therefore busy=75%.
    EXPECT_EQ(scan_host::metrics_policy::CalculateProcessorUsagePercent(100, 200, 300, 125, 260, 340), 75u);
}

TEST(SharedSystemMetricsTest, ZeroGpuAndVramSamplesHaveIndependentValidity) {
    SharedMemoryLayout sharedMemory;
    sharedMemory.systemMetrics.gpuUsage.store(0.0f, std::memory_order_relaxed);
    sharedMemory.systemMetrics.vramUsage.store(0.0f, std::memory_order_relaxed);
    sharedMemory.systemMetrics.validityMask.store(SYSTEM_METRIC_GPU_USAGE_VALID | SYSTEM_METRIC_VRAM_USAGE_VALID,
                                                  std::memory_order_release);

    const uint32_t validity = sharedMemory.systemMetrics.validityMask.load(std::memory_order_acquire);
    EXPECT_EQ(sharedMemory.systemMetrics.gpuUsage.load(std::memory_order_relaxed), 0.0f);
    EXPECT_EQ(sharedMemory.systemMetrics.vramUsage.load(std::memory_order_relaxed), 0.0f);
    EXPECT_NE(validity & SYSTEM_METRIC_GPU_USAGE_VALID, 0u);
    EXPECT_NE(validity & SYSTEM_METRIC_VRAM_USAGE_VALID, 0u);
    EXPECT_EQ(validity & SYSTEM_METRIC_VRAM_TOTAL_VALID, 0u);
}

TEST(SharedSystemMetricsTest, PublicationSequenceDistinguishesStableAndUpdatingSnapshots) {
    SharedMemoryLayout sharedMemory;
    EXPECT_EQ(sharedMemory.systemMetrics.publicationSequence.load(std::memory_order_acquire), 0u);
    sharedMemory.systemMetrics.publicationSequence.fetch_add(1, std::memory_order_acq_rel);
    EXPECT_NE(sharedMemory.systemMetrics.publicationSequence.load(std::memory_order_acquire) & 1u, 0u);
    sharedMemory.systemMetrics.publicationSequence.fetch_add(1, std::memory_order_release);
    EXPECT_EQ(sharedMemory.systemMetrics.publicationSequence.load(std::memory_order_acquire) & 1u, 0u);
}

TEST(SharedSystemMetricsTest, OptionalHardwareValuesKeepValidZeroPowerAndFanReadings) {
    SharedMemoryLayout sharedMemory;
    sharedMemory.systemMetrics.cpuPackagePowerW.store(0.0f, std::memory_order_relaxed);
    sharedMemory.systemMetrics.gpuFanRpm.store(0.0f, std::memory_order_relaxed);
    sharedMemory.systemMetrics.validityMask.store(
        SYSTEM_METRIC_CPU_PACKAGE_POWER_VALID | SYSTEM_METRIC_GPU_FAN_VALID, std::memory_order_release);

    const uint32_t validity = sharedMemory.systemMetrics.validityMask.load(std::memory_order_acquire);
    EXPECT_NE(validity & SYSTEM_METRIC_CPU_PACKAGE_POWER_VALID, 0u);
    EXPECT_NE(validity & SYSTEM_METRIC_GPU_FAN_VALID, 0u);
    EXPECT_FLOAT_EQ(sharedMemory.systemMetrics.cpuPackagePowerW.load(std::memory_order_relaxed), 0.0f);
    EXPECT_FLOAT_EQ(sharedMemory.systemMetrics.gpuFanRpm.load(std::memory_order_relaxed), 0.0f);
}

TEST(HostMetricsSourceInvariantTest, HookConsumesValidityInsteadOfNonzeroHeuristics) {
    const std::string source = ReadProjectSource("hook/common/system_metrics.cpp");
    const std::string overlay = ReadProjectSource("hook/common/overlay_adapter.cpp");
    ASSERT_FALSE(source.empty());
    ASSERT_FALSE(overlay.empty());

    EXPECT_NE(source.find("publication.validityMask & SYSTEM_METRIC_GPU_USAGE_VALID"), std::string::npos);
    EXPECT_NE(source.find("publication.validityMask & SYSTEM_METRIC_VRAM_USAGE_VALID"), std::string::npos);
    EXPECT_EQ(source.find("gpu > 0.0f || vramMB > 0.0f"), std::string::npos);
    EXPECT_NE(overlay.find("cachedSystemMetrics.vramUsageValid"), std::string::npos);
}

TEST(HostMetricsSourceInvariantTest, SensorAcceptsOnlySourceOrDirectChildGraphicsLuid) {
    const std::string source = ReadProjectSource("captureengine/sensor_service.cpp");
    ASSERT_FALSE(source.empty());
    EXPECT_NE(source.find("QueryDirectParentProcessId(luidSourcePid)"), std::string::npos);
    EXPECT_NE(source.find("IsGpuTelemetryPublisherEligible"), std::string::npos);
    EXPECT_NE(source.find("ResetGpuTelemetryForSource(s.shm, sourcePid)"), std::string::npos);
    EXPECT_NE(source.find("s.cachedLuid = 0"), std::string::npos);
    EXPECT_NE(source.find("ReadScreenGrabTarget"), std::string::npos);
    EXPECT_NE(source.find("CaptureDeviceLuid"), std::string::npos);
    EXPECT_NE(source.find("UpdateSystemMetrics(\n                s.shm, sourcePid"), std::string::npos)
        << "the profiled process must remain the shared telemetry/config source";
}

// Only the publishing renderer clears its own inherited-renderer claim, so a
// renderer that is terminated leaves the claim set for the rest of the session.
// The host reaps it once the process is provably gone, and must never reap on a
// snapshot that failed to answer.
TEST(HostMetricsSourceInvariantTest, SensorReapsAnInheritedRendererClaimWhoseProcessIsGone) {
    const std::string source = ReadProjectSource("captureengine/sensor_service.cpp");
    ASSERT_FALSE(source.empty());
    EXPECT_NE(source.find("QueryDirectParentProcessId(inheritedRendererPid, &inheritedRendererAlive)"),
              std::string::npos);
    EXPECT_NE(source.find("inheritedRendererPid != 0 && !inheritedRendererAlive"), std::string::npos);
    EXPECT_NE(source.find("inheritedRendererClaim.compare_exchange_strong"), std::string::npos);
    EXPECT_NE(source.find("bool* processFound = nullptr"), std::string::npos)
        << "a failed process snapshot must not be reported as a dead process";
}

TEST(HostMetricsSourceInvariantTest, GraphicsLuidPublishersStampCurrentProcessProvenance) {
    for (const char* path : {"common/capture_base.h", "hook/common/hook_common.cpp",
                             "hook/common/system_metrics.cpp"}) {
        const std::string source = ReadProjectSource(path);
        SCOPED_TRACE(path);
        ASSERT_FALSE(source.empty());
        EXPECT_NE(source.find("SetLuidSourcePid(GetCurrentProcessId())"), std::string::npos);
    }

    const std::string layerIpc = ReadProjectSource("hook/vulkan_layer/layer_ipc.cpp");
    ASSERT_FALSE(layerIpc.empty());
    EXPECT_NE(layerIpc.find("publisherPid = GetCurrentProcessId()"), std::string::npos);
    EXPECT_NE(layerIpc.find("SetLuidSourcePid(publisherPid)"), std::string::npos);
}

TEST(HostMetricsSourceInvariantTest, DirectDrawOverlayHelperPublishesAdapterWithoutRecording) {
    const std::string source = ReadProjectSource("hook/apis/ddraw_hook.cpp");
    ASSERT_FALSE(source.empty());

    // The internal header carries prototypes; anchor on the definitions in the
    // ddraw_hook_helpers / ddraw_hook_capture_impl units (rfind: definitions
    // follow the prototypes in the logical source).
    const size_t helperCreate = source.rfind("bool CreateD3D9ExWrapper");
    const size_t luidQuery = source.find("GetAdapterLUID(D3DADAPTER_DEFAULT", helperCreate);
    const size_t luidPublish = source.find("ReportLUID(luidLow, luidHigh)", luidQuery);
    const size_t recordingCapture = source.rfind("bool CaptureFrameFromSurface");
    ASSERT_NE(helperCreate, std::string::npos);
    ASSERT_NE(luidQuery, std::string::npos);
    ASSERT_NE(luidPublish, std::string::npos);
    ASSERT_NE(recordingCapture, std::string::npos);
    EXPECT_LT(helperCreate, luidQuery);
    EXPECT_LT(luidQuery, luidPublish);
    // The helper publishes the adapter LUID independently of the capture path;
    // the two live in separate semantic units now, so only presence is asserted
    // for the capture entry point.
}
