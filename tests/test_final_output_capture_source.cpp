#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "source_fragment_reader.h"

namespace {

std::string ReadSource(const std::filesystem::path& relativePath) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / relativePath);
}

std::string FunctionBody(const std::string& source, const std::string& signature,
                         const std::string& nextSignature) {
    const size_t begin = source.find(signature);
    if (begin == std::string::npos)
        return {};
    const size_t end = source.find(nextSignature, begin + signature.size());
    return source.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}

}  // namespace

TEST(FinalOutputCaptureSourceTest, VulkanCaptureNeverWaitsAndCarriesFinalOutputTiming) {
    const std::string captureSource = ReadSource("hook/vulkan_layer/layer_capture.cpp");
    const std::string ipcSource = ReadSource("hook/vulkan_layer/layer_ipc.cpp");
    ASSERT_FALSE(captureSource.empty());
    ASSERT_FALSE(ipcSource.empty());

    const std::string captureBody =
        FunctionBody(captureSource, "bool CaptureFrame(", "// ---- Vulkan Screenshot ----");
    ASSERT_FALSE(captureBody.empty());
    EXPECT_NE(captureBody.find("std::try_to_lock"), std::string::npos);
    EXPECT_NE(captureBody.find("injectProducerCaptureLockDrops"), std::string::npos);
    EXPECT_NE(captureBody.find("metadata->timestampQpc"), std::string::npos);
    EXPECT_NE(captureBody.find("sourceQpc.QuadPart, metadata"), std::string::npos);

    const std::string publishBody =
        FunctionBody(ipcSource, "void LayerIPC_SignalFrameReady(", "void LayerIPC_Log(");
    ASSERT_FALSE(publishBody.empty());
    EXPECT_NE(publishBody.find("metadata->displayTimingSequence"), std::string::npos);
    EXPECT_NE(publishBody.find("metadata->captureFlags"), std::string::npos);
    EXPECT_NE(publishBody.find("metadata->displayTimingGeneration"), std::string::npos);
}

TEST(FinalOutputCaptureSourceTest, VulkanGeneratedPresentsUseVirtualAndScheduledDisplayTiming) {
    const std::string present = ReadSource("hook/vulkan_layer/vulkan_layer_present.cpp");
    const std::string timing = ReadSource("hook/vulkan_layer/vulkan_final_output_capture.cpp");
    ASSERT_FALSE(present.empty());
    ASSERT_FALSE(timing.empty());

    const std::string captureBody =
        FunctionBody(present, "auto doCapture = [&]()", "auto doScreenshot = [&]()");
    ASSERT_FALSE(captureBody.empty());
    EXPECT_NE(captureBody.find("finalGeneratedOutput"), std::string::npos);
    EXPECT_NE(present.find("finalOutputMeteredPresentsRemaining"), std::string::npos)
        << "VkSetPresentConfigNV applies to the whole following Present batch";
    const size_t plan = captureBody.find("PlanVulkanFinalOutputCapture");
    const size_t throttle = captureBody.find("throttleCapture.load");
    ASSERT_NE(plan, std::string::npos);
    ASSERT_NE(throttle, std::string::npos);
    EXPECT_LT(plan, throttle) << "throttled outputs still consume their virtual ordinal";
    EXPECT_NE(captureBody.find("&captureDone"), std::string::npos);
    EXPECT_NE(captureBody.find("captureMetadata"), std::string::npos);

    EXPECT_NE(timing.find("NextFinalOutputTimestampQpc"), std::string::npos);
    EXPECT_NE(timing.find("CaptureDisplayTimingPublicationWatermark"), std::string::npos);
    EXPECT_NE(timing.find("ObserveFinalOutputSourcePresent"), std::string::npos);
    EXPECT_NE(timing.find("AdjustFinalOutputTimelineForMultiplierChange"), std::string::npos);
    EXPECT_NE(timing.find("kFinalOutputCfrPublicationHeadroomPermille"), std::string::npos);
    EXPECT_NE(timing.find("std::try_to_lock"), std::string::npos);
    EXPECT_NE(timing.find("UpdateFinalOutputCaptureEpoch"), std::string::npos);
    EXPECT_NE(present.find("UpdateFinalOutputCaptureEpoch"), std::string::npos)
        << "an inactive recording must retire the prior Vulkan capture epoch even when doCapture is not called";
    const size_t routePublication = present.find("SetInjectFinalOutputCaptureAvailable");
    const size_t limiterApply = present.find("g_SharedFpsLimiter.Apply");
    ASSERT_NE(routePublication, std::string::npos);
    ASSERT_NE(limiterApply, std::string::npos);
    EXPECT_LT(routePublication, limiterApply)
        << "limiter source semantics must be known before pacing and the media inject handshake";

    const std::string correlator =
        ReadSource("captureengine/media_main_encoder_inject_display_timing.cpp");
    ASSERT_FALSE(correlator.empty());
    EXPECT_NE(correlator.find("queuedFrame.frameIndex == observation.frameIndex"),
              std::string::npos)
        << "delayed display evidence must not retime a reused ring slot";
}

TEST(FinalOutputCaptureSourceTest, DX12RecordingStartsWithFreshFinalOutputClock) {
    const std::string timing = ReadSource("hook/apis/dx12_hook_final_output_capture.cpp");
    ASSERT_FALSE(timing.empty());

    const std::string plan = FunctionBody(
        timing, "DX12FinalOutputCapturePlan DX12_PlanStreamlineFinalOutputCapture(",
        "bool DX12_TryClaimStreamlineFinalOutputCapture(");
    ASSERT_FALSE(plan.empty());
    const size_t captureCandidate = plan.find("plan.captureCandidate =");
    const size_t epochReset = plan.find("UpdateFinalOutputCaptureEpoch");
    const size_t timestamp = plan.find("NextFinalOutputTimestampQpc");
    ASSERT_NE(captureCandidate, std::string::npos);
    ASSERT_NE(epochReset, std::string::npos);
    ASSERT_NE(timestamp, std::string::npos);
    const size_t routePublication = plan.find("DX12_ShouldUseStreamlineFinalOutputCapture");
    ASSERT_NE(routePublication, std::string::npos);
    EXPECT_LT(routePublication, captureCandidate)
        << "source-domain publication must precede the delayed media handshake";
    EXPECT_LT(captureCandidate, epochReset);
    EXPECT_LT(epochReset, timestamp);
    EXPECT_NE(timing.find("SetInjectFinalOutputCaptureAvailable"), std::string::npos)
        << "the final-output route must publish its limiter domain before recording starts";
}

TEST(FinalOutputCaptureSourceTest, VideoRecordingStartsDisplayTimingWithoutSensorOverlayRows) {
    const std::string recording = ReadSource("captureengine/main_recording.cpp");
    const std::string controller = ReadSource("captureengine/main_internal.h");
    const std::string sensor = ReadSource("captureengine/sensor_service.cpp");
    ASSERT_FALSE(recording.empty());
    ASSERT_FALSE(controller.empty());
    ASSERT_FALSE(sensor.empty());

    const std::string toggle =
        FunctionBody(recording, "void ToggleRecording()", "void ToggleAudioOnlyRecording()");
    ASSERT_FALSE(toggle.empty());
    const size_t sensorReady = toggle.find("EnsureSensorProcessReady()");
    const size_t startCommand = toggle.find("ProcessCommand::StartRecording");
    ASSERT_NE(sensorReady, std::string::npos);
    ASSERT_NE(startCommand, std::string::npos);
    EXPECT_LT(sensorReady, startCommand);
    const std::string sensorPolicy = FunctionBody(
        controller, "inline bool ShouldStartSensorProcess", "inline bool HardwareSensorServiceConfigEquals");
    ASSERT_FALSE(sensorPolicy.empty());
    EXPECT_NE(sensorPolicy.find("main_g_Recording"), std::string::npos)
        << "a config reload must not stop display timing during an active recording";
    EXPECT_NE(sensor.find("GetRecordingStartIntent() == RecordingStartIntent::Video"), std::string::npos)
        << "the collector must arm from intent before inject capture becomes live";
    EXPECT_NE(recording.find("recoverProcess(ProcessMode::Sensors"), std::string::npos)
        << "the exact-timing child must recover during a live recording";
}
