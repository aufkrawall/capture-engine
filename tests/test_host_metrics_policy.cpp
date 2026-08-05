#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../captureengine/host_metrics_policy.h"
#include "../common/shared_defs.h"
#include "source_fragment_reader.h"

namespace {

using scan_host::metrics_policy::AdapterResolutionSource;
using scan_host::metrics_policy::GpuEngineSample;

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

TEST(HostMetricsSourceInvariantTest, SensorAcceptsOnlyLuidStampedByCurrentSourcePid) {
    const std::string source = ReadProjectSource("captureengine/sensor_service.cpp");
    ASSERT_FALSE(source.empty());
    EXPECT_NE(source.find("luidSourcePid == sourcePid"), std::string::npos);
    EXPECT_NE(source.find("ResetGpuTelemetryForSource(s.shm, sourcePid)"), std::string::npos);
    EXPECT_NE(source.find("s.cachedLuid = 0"), std::string::npos);
    EXPECT_NE(source.find("ReadScreenGrabTarget"), std::string::npos);
    EXPECT_NE(source.find("CaptureDeviceLuid"), std::string::npos);
}

TEST(HostMetricsSourceInvariantTest, GraphicsLuidPublishersStampCurrentProcessProvenance) {
    for (const char* path : {"common/capture_base.h", "hook/common/hook_common.cpp", "hook/common/system_metrics.cpp",
                             "hook/vulkan_layer/layer_ipc.cpp"}) {
        const std::string source = ReadProjectSource(path);
        SCOPED_TRACE(path);
        ASSERT_FALSE(source.empty());
        EXPECT_NE(source.find("SetLuidSourcePid(GetCurrentProcessId())"), std::string::npos);
    }
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
