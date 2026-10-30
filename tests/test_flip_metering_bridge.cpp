#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "hook/ngx/flip_metering_bridge.h"
#include "hook/ngx/flip_metering_bridge_policy.h"
#include "hook/vulkan_layer/vulkan_flip_metering_bridge_policy.h"
#include "hook/vulkan_layer/vulkan_present_chain_policy.h"

// GTA V Enhanced on vkd3d-proton + dxvk-nvapi, session 20261027_140018: DLSS frame generation 2x ran,
// but NvAPI_D3D12_SetFlipConfig had no implementation and vkd3d-proton never enabled
// VK_NV_present_metering, so the batch flipped at GPU completion (display-change stddev 1.8 ms,
// jaggedness 2.7-3.1 ms against even ~7.5/8.1 ms presents; nvFlipSchedule applied=0, native D3D12
// applied=2728). These cover the bridge that carries the D3D12 batch size onto the Vulkan present.

namespace {

namespace hookside = ce::flip_metering_bridge;
namespace layerside = ce::vulkan_flip_metering_bridge;

constexpr uint32_t kStart = hookside::kMarkerOutOfBandPresentStart;
constexpr uint32_t kEnd = hookside::kMarkerOutOfBandPresentEnd;

TEST(FlipMeteringBridge, ReadsBothFlipConfigVersionsAndWritesOnlyTheFeedback) {
    hookside::FlipConfigV1 v1 = {hookside::kFlipConfigVer1, 2, 0xFFu, {7u, 7u, 7u}};
    hookside::FlipConfigRequest request = hookside::ReadFlipConfig(&v1);
    EXPECT_EQ(request.status, hookside::kNvApiOk);
    EXPECT_EQ(request.framesPerBatch, 2u);
    EXPECT_EQ(request.presentedFramesPerBatch, 0u) << "V1's reserved words are not V2's fields";
    EXPECT_FLOAT_EQ(request.timePerBatchMs, 0.0f);
    hookside::CompleteFlipConfig(&v1, 0u);
    EXPECT_EQ(v1.flipConfigFeedbackMask, 0u);

    hookside::FlipConfigV2 v2 = {hookside::kFlipConfigVer2, 4, 0x3u, 3, 16.6f, 7.25f};
    request = hookside::ReadFlipConfig(&v2);
    EXPECT_EQ(request.status, hookside::kNvApiOk);
    EXPECT_EQ(request.framesPerBatch, 4u);
    EXPECT_EQ(request.version >> 16, 2u);
    EXPECT_EQ(request.presentedFramesPerBatch, 3u);
    EXPECT_FLOAT_EQ(request.timePerBatchMs, 16.6f);
    EXPECT_FLOAT_EQ(request.minControlFrameTimeMs, 7.25f);
    hookside::CompleteFlipConfig(&v2, hookside::kFlipConfigFeedbackNoIndependentFlip);
    EXPECT_EQ(v2.flipConfigFeedbackMask, hookside::kFlipConfigFeedbackNoIndependentFlip);
    EXPECT_EQ(v2.nFramesPerBatch, 4u);
    EXPECT_EQ(v2.nPresentedFramesPerBatch, 3u);
    EXPECT_FLOAT_EQ(v2.timePerBatchMs, 16.6f);

    // Metering disabled for a batch is a valid request.
    hookside::FlipConfigV2 off = {hookside::kFlipConfigVer2, 0, 0, 1, 0.0f, 0.0f};
    EXPECT_EQ(hookside::ReadFlipConfig(&off).status, hookside::kNvApiOk);
}

TEST(FlipMeteringBridge, RejectsWhatTheDriverRejects) {
    EXPECT_EQ(hookside::ReadFlipConfig(nullptr).status, hookside::kNvApiInvalidPointer);
    hookside::FlipConfigV2 wrongVersion = {hookside::kFlipConfigVer2 + 1, 2, 0x5u, 2, 0.0f, 0.0f};
    EXPECT_EQ(hookside::ReadFlipConfig(&wrongVersion).status, hookside::kNvApiIncompatibleStructVersion);
    EXPECT_EQ(wrongVersion.flipConfigFeedbackMask, 0x5u) << "a rejected call writes nothing";
    hookside::FlipConfigV2 tooMany = {hookside::kFlipConfigVer2, hookside::kMaxFramesPerFlipBatch + 1, 0, 0, 0, 0};
    EXPECT_EQ(hookside::ReadFlipConfig(&tooMany).status, hookside::kNvApiInvalidArgument);
}

// Session 20261027_151844: NVIDIA's ICD refused every vkd3d batch with presentConfigFeedback 0x1 while CE
// answered SetFlipConfig with feedback 0, so DLSS-G stopped pacing and frame pairs flipped 0.3 ms apart.
TEST(FlipMeteringBridge, TheDriversRefusalReachesTheGeneratorAsNoIndependentFlip) {
    using hookside::FlipConfigFeedbackFromVulkan;
    EXPECT_EQ(FlipConfigFeedbackFromVulkan(true, 0u), 0u);
    EXPECT_EQ(FlipConfigFeedbackFromVulkan(true, 0x1u), hookside::kFlipConfigFeedbackNoIndependentFlip);
    EXPECT_EQ(FlipConfigFeedbackFromVulkan(true, 0x2u), hookside::kFlipConfigFeedbackMirroringDetected);
    EXPECT_EQ(FlipConfigFeedbackFromVulkan(true, 0x3u), 0x3u);
    // An answer with no NVAPI-defined bit is still "not metered".
    EXPECT_EQ(FlipConfigFeedbackFromVulkan(true, 0x80u), hookside::kFlipConfigFeedbackNoIndependentFlip);
    // No layer to ask: unconfirmed metering is no metering.
    EXPECT_EQ(FlipConfigFeedbackFromVulkan(false, 0u), hookside::kFlipConfigFeedbackNoIndependentFlip);
}

TEST(FlipMeteringBridge, ScheduleKeepsTheDriversLatestAnswerForALiveSwapchain) {
    layerside::BatchSchedule schedule;
    EXPECT_EQ(schedule.DriverFeedback(), 0u) << "nothing asked yet";

    schedule.OnMeteredPresentAnswered(0xA, 0x1u);
    EXPECT_EQ(schedule.DriverFeedback(), 0x1u);
    schedule.OnSwapchainRetired(0xB);
    EXPECT_EQ(schedule.DriverFeedback(), 0x1u) << "another swapchain's destruction changes nothing";

    schedule.OnMeteredPresentAnswered(0xA, 0u);
    EXPECT_EQ(schedule.DriverFeedback(), 0u) << "a later acceptance supersedes the refusal";
    schedule.OnMeteredPresentAnswered(0xA, 0x1u);
    schedule.OnSwapchainRetired(0xA);
    EXPECT_EQ(schedule.DriverFeedback(), 0u) << "the successor swapchain is asked afresh";
    schedule.OnSwapchainRetired(0);
    EXPECT_EQ(schedule.Stats().driverMetered, 1u);
    EXPECT_EQ(schedule.Stats().driverRefused, 2u);
}

TEST(FlipMeteringBridge, ClaimsOnlyTheV2FlipConfigFeature) {
    hookside::NgxGetDriverFeatureSupportParamsV1 params = {};
    params.version = hookside::kNgxGetDriverFeatureSupportVer1;
    params.featureCount = 3;
    params.featureSupportInfo[0] = {hookside::kNgxFeatureSetFlipConfigV2, 0u, {}};
    params.featureSupportInfo[1] = {0x836AF07Bu, 1u, {}};  // FRAME_PRESENT_NOTIFY_HYBRID
    params.featureSupportInfo[2] = {0x429A2D34u, 0x6u, {}};
    EXPECT_EQ(hookside::AnswerDriverFeatureSupport(&params), hookside::kNvApiOk);
    EXPECT_EQ(params.featureSupportInfo[0].supportBits & 1u, 1u);
    EXPECT_EQ(params.featureSupportInfo[1].supportBits & 1u, 0u);
    EXPECT_EQ(params.featureSupportInfo[2].supportBits, 0x6u) << "reserved bits are not the bridge's";

    params.featureCount = hookside::kMaxNgxFeaturesPerQuery + 1;
    EXPECT_EQ(hookside::AnswerDriverFeatureSupport(&params), hookside::kNvApiInvalidArgument);
    params.version = hookside::kNgxGetDriverFeatureSupportVer1 + 1;
    EXPECT_EQ(hookside::AnswerDriverFeatureSupport(&params), hookside::kNvApiIncompatibleStructVersion);
    EXPECT_EQ(hookside::AnswerDriverFeatureSupport(nullptr), hookside::kNvApiInvalidPointer);
}

TEST(FlipMeteringBridge, AnswersOnlyMissingFlipMeteringEntryPointsWithAMeteringDevice) {
    using hookside::DecideQueryInterface;
    using hookside::QueryAnswer;
    EXPECT_EQ(DecideQueryInterface(0x73BF8338u, false, true), QueryAnswer::kPassThrough);
    // The driver's own implementation stays authoritative.
    EXPECT_EQ(DecideQueryInterface(hookside::kNvApiIdD3D12SetFlipConfig, true, true), QueryAnswer::kPassThrough);
    EXPECT_EQ(DecideQueryInterface(hookside::kNvApiIdD3D12SetFlipConfig, false, false),
              QueryAnswer::kDeclinedNoMeteringDevice);
    EXPECT_EQ(DecideQueryInterface(hookside::kNvApiIdD3D12SetFlipConfig, false, true), QueryAnswer::kSetFlipConfig);
    EXPECT_EQ(DecideQueryInterface(hookside::kNvApiIdNgxGetDriverFeatureSupport, false, true),
              QueryAnswer::kDriverFeatureSupport);
}

// Through the runtime interface: a process with no resident CE Vulkan layer has nothing to meter with.
TEST(FlipMeteringBridge, RuntimeDeclinesWithoutAResidentVulkanLayer) {
    EXPECT_EQ(hookside::MaybeAnswerQueryInterface(hookside::kNvApiIdD3D12SetFlipConfig, nullptr, nullptr), nullptr);
    EXPECT_EQ(hookside::MaybeAnswerQueryInterface(0x12345678u, nullptr, nullptr), nullptr);
    int resident = 0;
    EXPECT_EQ(hookside::MaybeAnswerQueryInterface(hookside::kNvApiIdD3D12SetFlipConfig, &resident, nullptr), nullptr);
}

TEST(FlipMeteringBridge, BinderTakesTheOpenFrameOrTheNextStart) {
    hookside::BatchBinder binder;
    hookside::Binding binding;

    // SetFlipConfig before the batch's first START: bound when the START arrives.
    EXPECT_FALSE(binder.OnFlipConfig(2, binding));
    ASSERT_TRUE(binder.OnLatencyMarker(100, kStart, binding));
    EXPECT_EQ(binding.frameId, 100u);
    EXPECT_EQ(binding.framesPerBatch, 2u);
    EXPECT_FALSE(binder.OnLatencyMarker(100, kEnd, binding));
    // The batch's second present re-opens the same frame without a new configuration.
    EXPECT_FALSE(binder.OnLatencyMarker(100, kStart, binding));
    EXPECT_FALSE(binder.OnLatencyMarker(100, kEnd, binding));

    // SetFlipConfig after the batch's first START: bound to the open frame at once.
    EXPECT_FALSE(binder.OnLatencyMarker(101, kStart, binding));
    ASSERT_TRUE(binder.OnFlipConfig(3, binding));
    EXPECT_EQ(binding.frameId, 101u);
    EXPECT_EQ(binding.framesPerBatch, 3u);
    EXPECT_FALSE(binder.OnLatencyMarker(101, kEnd, binding));

    // Other markers and frame id 0 never bind.
    EXPECT_FALSE(binder.OnFlipConfig(2, binding));
    EXPECT_FALSE(binder.OnLatencyMarker(102, 0u /*SIMULATION_START*/, binding));
    EXPECT_FALSE(binder.OnLatencyMarker(0, kStart, binding));
    ASSERT_TRUE(binder.OnLatencyMarker(102, kStart, binding));
    EXPECT_EQ(binding.frameId, 102u);

    // A configuration superseded before any START is counted, the newer one wins.
    EXPECT_FALSE(binder.OnLatencyMarker(102, kEnd, binding));
    EXPECT_FALSE(binder.OnFlipConfig(2, binding));
    EXPECT_FALSE(binder.OnFlipConfig(4, binding));
    EXPECT_EQ(binder.ReplacedPending(), 1u);
    ASSERT_TRUE(binder.OnLatencyMarker(103, kStart, binding));
    EXPECT_EQ(binding.framesPerBatch, 4u);
}

TEST(FlipMeteringBridge, EnablesMeteringOnlyOnACapableD3D12TranslationDevice) {
    using layerside::DecideDeviceEnablement;
    using layerside::DeviceDecision;
    layerside::DeviceInput input;
    input.d3d12TranslationEngine = true;
    input.extensionAvailable = true;
    input.featureSupported = true;
    EXPECT_EQ(DecideDeviceEnablement(input), DeviceDecision::kEnable);

    layerside::DeviceInput native = input;
    native.d3d12TranslationEngine = false;
    EXPECT_EQ(DecideDeviceEnablement(native), DeviceDecision::kNotD3D12Translation);
    layerside::DeviceInput owned = input;
    owned.applicationEnabledExtension = true;
    EXPECT_EQ(DecideDeviceEnablement(owned), DeviceDecision::kApplicationOwnsMetering);
    owned = input;
    owned.applicationChainHasFeatures = true;
    EXPECT_EQ(DecideDeviceEnablement(owned), DeviceDecision::kApplicationOwnsMetering);
    layerside::DeviceInput missing = input;
    missing.extensionAvailable = false;
    EXPECT_EQ(DecideDeviceEnablement(missing), DeviceDecision::kExtensionUnavailable);
    missing = input;
    missing.featureSupported = false;
    EXPECT_EQ(DecideDeviceEnablement(missing), DeviceDecision::kFeatureUnsupported);

    EXPECT_TRUE(layerside::IsD3D12TranslationEngine("vkd3d"));
    EXPECT_FALSE(layerside::IsD3D12TranslationEngine("DXVK"));
    EXPECT_FALSE(layerside::IsD3D12TranslationEngine(nullptr));
}

// 20261029_135421 / 141337: NVIDIA's driver kept every vkd3d instance on a presenter that refused all
// metering requests, chosen by the exact engine name; any other name got the metering presenter.
TEST(FlipMeteringBridge, OnlyAnNvidiaFrameGenerationProcessShowsTheDriverAnotherEngineName) {
    using layerside::DecideDriverEngineName;
    using layerside::DriverEngineNameDecision;
    layerside::DriverEngineNameInput input;
    input.d3d12TranslationEngine = true;
    input.nvidiaVulkanDriverMapped = true;
    input.streamlineMapped = true;
    EXPECT_EQ(DecideDriverEngineName(input), DriverEngineNameDecision::kDriverFacingName);

    layerside::DriverEngineNameInput other = input;
    other.d3d12TranslationEngine = false;
    EXPECT_EQ(DecideDriverEngineName(other), DriverEngineNameDecision::kNotD3D12Translation);
    other = input;
    other.nvidiaVulkanDriverMapped = false;
    EXPECT_EQ(DecideDriverEngineName(other), DriverEngineNameDecision::kNotNvidiaDriver);
    other = input;
    other.streamlineMapped = false;
    EXPECT_EQ(DecideDriverEngineName(other), DriverEngineNameDecision::kNoFrameGenerationRuntime);

    // The driver matches "vkd3d" and "DXVK" exactly and case-sensitively.
    EXPECT_STRNE(layerside::kDriverFacingD3D12TranslationEngineName, "vkd3d");
    EXPECT_STRNE(layerside::kDriverFacingD3D12TranslationEngineName, "DXVK");
    EXPECT_FALSE(layerside::IsD3D12TranslationEngine(layerside::kDriverFacingD3D12TranslationEngineName));
}

struct SwapchainRecord {
    uint64_t lastBatchFrameId = 0;
    bool metered = false;
};

layerside::PresentDecision Present(layerside::BatchSchedule& schedule, SwapchainRecord& sc, uint64_t frameId) {
    return schedule.OnPresent(frameId, sc.lastBatchFrameId, sc.metered);
}

TEST(FlipMeteringBridge, ScheduleMetersTheFirstPresentOfEachBoundBatch) {
    layerside::BatchSchedule schedule;
    SwapchainRecord sc;
    schedule.Publish(10, 2);

    layerside::PresentDecision first = Present(schedule, sc, 10);
    EXPECT_EQ(first.outcome, layerside::PresentOutcome::kBatchStartMetered);
    EXPECT_EQ(first.framesPerBatch, 2u);
    EXPECT_TRUE(first.metered);
    // The batch's generated and real present share the frame id; only the first carries the node.
    layerside::PresentDecision second = Present(schedule, sc, 10);
    EXPECT_EQ(second.outcome, layerside::PresentOutcome::kBatchContinuation);
    EXPECT_EQ(second.framesPerBatch, 0u);
    EXPECT_TRUE(second.metered);

    // Frame generation off: frames keep their markers, nothing is bound, nothing is metered.
    layerside::PresentDecision unbound = Present(schedule, sc, 11);
    EXPECT_EQ(unbound.outcome, layerside::PresentOutcome::kBatchStartUnmetered);
    EXPECT_FALSE(unbound.metered);
    EXPECT_FALSE(Present(schedule, sc, 11).metered);
    // A generator disabling metering for a batch publishes size 0.
    schedule.Publish(12, 0);
    EXPECT_EQ(Present(schedule, sc, 12).outcome, layerside::PresentOutcome::kBatchStartUnmetered);
    // No marker at all.
    schedule.Publish(13, 2);
    EXPECT_TRUE(Present(schedule, sc, 13).metered);
    EXPECT_EQ(Present(schedule, sc, 0).outcome, layerside::PresentOutcome::kUnmarked);
    EXPECT_FALSE(sc.metered);
}

// Keyed by frame id, a batch vkd3d never presented cannot shift the ones after it - counting presents would.
TEST(FlipMeteringBridge, ScheduleSurvivesABatchThatNeverReachedVulkan) {
    layerside::BatchSchedule schedule;
    SwapchainRecord sc;
    schedule.Publish(20, 2);
    schedule.Publish(21, 3);  // its presents are dropped (e.g. a swapchain recreation)
    schedule.Publish(22, 2);
    EXPECT_EQ(Present(schedule, sc, 20).framesPerBatch, 2u);
    EXPECT_EQ(Present(schedule, sc, 20).framesPerBatch, 0u);
    layerside::PresentDecision next = Present(schedule, sc, 22);
    EXPECT_EQ(next.framesPerBatch, 2u);
    EXPECT_EQ(schedule.Stats().skipped, 1u);

    // A full table drops its oldest entry, which is the one whose batch is furthest behind.
    for (uint64_t id = 100; id < 100 + layerside::kPendingBatchCapacity + 1; ++id)
        schedule.Publish(id, 2);
    EXPECT_EQ(schedule.Stats().evicted, 1u);
    EXPECT_EQ(Present(schedule, sc, 100).outcome, layerside::PresentOutcome::kBatchStartUnmetered);
    EXPECT_EQ(Present(schedule, sc, 101).outcome, layerside::PresentOutcome::kBatchStartMetered);
}

// The D3D12 thread and vkd3d's present thread together, as GTA's 2x batches run: START/Present/END per
// present, SetFlipConfig before the batch's first Present, the Vulkan presents behind them in order.
TEST(FlipMeteringBridge, D3D12BatchesReachTheirFirstVulkanPresent) {
    hookside::BatchBinder binder;
    layerside::BatchSchedule schedule;
    hookside::Binding binding;
    auto publishIf = [&](bool bound) {
        if (bound)
            schedule.Publish(binding.frameId, binding.framesPerBatch);
    };

    std::vector<uint64_t> vulkanPresents;
    for (uint64_t frame = 500; frame < 504; ++frame) {
        publishIf(binder.OnFlipConfig(2, binding));
        for (int present = 0; present < 2; ++present) {
            publishIf(binder.OnLatencyMarker(frame, kStart, binding));
            vulkanPresents.push_back(frame);  // vkd3d replays the frame id before vkQueuePresentKHR
            publishIf(binder.OnLatencyMarker(frame, kEnd, binding));
        }
    }

    SwapchainRecord sc;
    std::vector<uint32_t> nodes;
    nodes.reserve(vulkanPresents.size());
    for (uint64_t frameId : vulkanPresents)
        nodes.push_back(Present(schedule, sc, frameId).framesPerBatch);
    EXPECT_EQ(nodes, (std::vector<uint32_t>{2, 0, 2, 0, 2, 0, 2, 0}));
    EXPECT_EQ(schedule.Stats().meteredBatches, 4u);
}

// The same two threads when the driver refuses: the batch after the refusal is configured with NO_IFLIP, so
// the generator paces it, while the Vulkan request keeps going out so a later acceptance is seen.
TEST(FlipMeteringBridge, ARefusedBatchHandsPacingBackToTheGenerator) {
    hookside::BatchBinder binder;
    layerside::BatchSchedule schedule;
    hookside::Binding binding;
    SwapchainRecord sc;
    constexpr uint64_t kSwapchain = 0x5C;
    auto runBatch = [&](uint64_t frame, uint32_t driverAnswer) {
        hookside::FlipConfigV2 config = {hookside::kFlipConfigVer2, 2, 0u, 2, 16.0f, 0.0f};
        EXPECT_EQ(hookside::ReadFlipConfig(&config).status, hookside::kNvApiOk);
        if (binder.OnFlipConfig(config.nFramesPerBatch, binding))
            schedule.Publish(binding.frameId, binding.framesPerBatch);
        hookside::CompleteFlipConfig(&config, hookside::FlipConfigFeedbackFromVulkan(true, schedule.DriverFeedback()));
        for (int present = 0; present < 2; ++present) {
            if (binder.OnLatencyMarker(frame, kStart, binding))
                schedule.Publish(binding.frameId, binding.framesPerBatch);
            const layerside::PresentDecision decision = Present(schedule, sc, frame);
            if (decision.framesPerBatch != 0)
                schedule.OnMeteredPresentAnswered(kSwapchain, driverAnswer);
            binder.OnLatencyMarker(frame, kEnd, binding);
        }
        return config.flipConfigFeedbackMask;
    };

    EXPECT_EQ(runBatch(700, 0x1u), 0u) << "the first batch is configured before the driver has answered";
    EXPECT_EQ(runBatch(701, 0x1u), hookside::kFlipConfigFeedbackNoIndependentFlip);
    EXPECT_EQ(runBatch(702, 0u), hookside::kFlipConfigFeedbackNoIndependentFlip);
    EXPECT_EQ(runBatch(703, 0u), 0u) << "the driver accepted 702, so 703 is the driver's to meter";
    EXPECT_EQ(schedule.Stats().meteredBatches, 4u) << "every batch still asked the driver";
    EXPECT_EQ(schedule.Stats().driverRefused, 2u);
}

// Sessions 20261027_155313 and 20261029_123525: DLSS-G configured ~30 single-frame batches while warming up,
// the ICD refused the first one that reached Vulkan with 0x1, the refusal came back as NO_IFLIP and DLSS-G
// asked for no metering for the rest of the swapchain's life - before a batch with a generated frame was
// ever put to the driver. The driver here refuses what Remix never asks for and meters real batches.
TEST(FlipMeteringBridge, SingleFrameWarmUpBatchesNeverPutMeteringToTheDriver) {
    hookside::BatchBinder binder;
    layerside::BatchSchedule schedule;
    hookside::Binding binding;
    SwapchainRecord sc;
    constexpr uint64_t kSwapchain = 0x5D;
    std::vector<uint32_t> nodes;
    auto runBatch = [&](uint64_t frame, uint32_t framesPerBatch) {
        hookside::FlipConfigV2 config = {hookside::kFlipConfigVer2, framesPerBatch, 0u, framesPerBatch, 16.0f, 0.0f};
        EXPECT_EQ(hookside::ReadFlipConfig(&config).status, hookside::kNvApiOk);
        if (binder.OnFlipConfig(config.nFramesPerBatch, binding))
            schedule.Publish(binding.frameId, binding.framesPerBatch);
        hookside::CompleteFlipConfig(&config, hookside::FlipConfigFeedbackFromVulkan(true, schedule.DriverFeedback()));
        for (uint32_t present = 0; present < framesPerBatch; ++present) {
            if (binder.OnLatencyMarker(frame, kStart, binding))
                schedule.Publish(binding.frameId, binding.framesPerBatch);
            const layerside::PresentDecision decision = Present(schedule, sc, frame);
            nodes.push_back(decision.framesPerBatch);
            if (decision.framesPerBatch != 0)
                schedule.OnMeteredPresentAnswered(kSwapchain, decision.framesPerBatch < 2 ? 0x1u : 0u);
            binder.OnLatencyMarker(frame, kEnd, binding);
        }
        return config.flipConfigFeedbackMask;
    };

    for (uint64_t frame = 800; frame < 803; ++frame)
        EXPECT_EQ(runBatch(frame, 1), 0u) << "warm-up frame " << frame;
    EXPECT_FALSE(sc.metered) << "a single frame has no flips to space";
    EXPECT_EQ(runBatch(803, 2), 0u);
    EXPECT_EQ(runBatch(804, 2), 0u) << "the first real batch was the driver's to answer, and it metered it";
    EXPECT_EQ(nodes, (std::vector<uint32_t>{0, 0, 0, 2, 0, 2, 0}));
    EXPECT_EQ(schedule.Stats().singleFrameBatches, 3u);
    EXPECT_EQ(schedule.Stats().meteredBatches, 2u);
    EXPECT_EQ(schedule.Stats().driverRefused, 0u);
    EXPECT_EQ(schedule.Stats().driverMetered, 2u);
}

TEST(FlipMeteringBridge, KeepsAskingTheDriverOnlyWhileItsLatestAnswerIsARefusal) {
    using hookside::ResolveVulkanBatchRequest;
    constexpr uint32_t kNoIFlip = hookside::kFlipConfigFeedbackNoIndependentFlip;
    hookside::VulkanBatchRequest r = ResolveVulkanBatchRequest(2, 2, 0);
    EXPECT_EQ(r.framesPerBatch, 2u);
    EXPECT_FALSE(r.driverProbe);
    r = ResolveVulkanBatchRequest(2, 2, kNoIFlip);
    EXPECT_EQ(r.framesPerBatch, 2u) << "the generator's own request always goes out";
    EXPECT_FALSE(r.driverProbe);
    r = ResolveVulkanBatchRequest(0, 2, kNoIFlip);
    EXPECT_EQ(r.framesPerBatch, 2u) << "after a refusal CE asks with the frames the generator presents";
    EXPECT_TRUE(r.driverProbe);
    r = ResolveVulkanBatchRequest(0, 2, 0);
    EXPECT_EQ(r.framesPerBatch, 0u) << "metering turned off without a refusal (forced vsync) stays off";
    EXPECT_FALSE(r.driverProbe);
    r = ResolveVulkanBatchRequest(0, 0, kNoIFlip);
    EXPECT_EQ(r.framesPerBatch, 0u) << "a V1 caller says nothing about the batch";
    EXPECT_FALSE(r.driverProbe);
}

// Session 20261029_131715 against the 2026-10-29 probe: NVIDIA's Vulkan driver refuses the first requests on
// a new swapchain and meters every later one; DLSS-G asks while the feedback is good and configures no
// metering after a refusal. Without CE asking on its own the generator never got the driver's acceptance.
TEST(FlipMeteringBridge, TheDriversWarmUpRefusalDoesNotEndMetering) {
    hookside::BatchBinder binder;
    layerside::BatchSchedule schedule;
    hookside::Binding binding;
    SwapchainRecord sc;
    constexpr uint64_t kSwapchain = 0x5E;
    uint32_t driverRequests = 0;
    uint32_t generatorRequest = 2;
    std::vector<uint32_t> feedbackSeen;
    for (uint64_t frame = 900; frame < 912; ++frame) {
        hookside::FlipConfigV2 config = {hookside::kFlipConfigVer2, generatorRequest, 0u, 2, 16.0f, 0.0f};
        const hookside::FlipConfigRequest request = hookside::ReadFlipConfig(&config);
        ASSERT_EQ(request.status, hookside::kNvApiOk);
        const uint32_t feedback = hookside::FlipConfigFeedbackFromVulkan(true, schedule.DriverFeedback());
        const hookside::VulkanBatchRequest vulkanRequest =
            hookside::ResolveVulkanBatchRequest(request.framesPerBatch, request.presentedFramesPerBatch, feedback);
        if (binder.OnFlipConfig(vulkanRequest.framesPerBatch, binding))
            schedule.Publish(binding.frameId, binding.framesPerBatch);
        hookside::CompleteFlipConfig(&config, feedback);
        for (int present = 0; present < 2; ++present) {
            if (binder.OnLatencyMarker(frame, kStart, binding))
                schedule.Publish(binding.frameId, binding.framesPerBatch);
            const layerside::PresentDecision decision = Present(schedule, sc, frame);
            if (decision.framesPerBatch != 0)
                schedule.OnMeteredPresentAnswered(kSwapchain, driverRequests++ < 2 ? 0x1u : 0u);
            binder.OnLatencyMarker(frame, kEnd, binding);
        }
        feedbackSeen.push_back(config.flipConfigFeedbackMask);
        generatorRequest = config.flipConfigFeedbackMask == 0 ? 2u : 0u;
    }
    EXPECT_EQ(generatorRequest, 2u) << "the generator took metering back";
    EXPECT_EQ(feedbackSeen.back(), 0u);
    EXPECT_EQ(schedule.Stats().driverRefused, 2u);
    EXPECT_GE(schedule.Stats().driverMetered, 8u) << "every batch after the warm-up was the driver's to meter";
}

// A metered batch's present keeps the generator's own mode selection: forcing FIFO onto it takes the
// flip scheduling away (Portal RTX 20260913_184745).
TEST(FlipMeteringBridge, MeteredBatchPresentsAreNotForcedBackToFifo) {
    ce::vulkan_present_chain::PresentModeSelectionInput input;
    input.vblankPacedPresentationRequested = true;
    input.swapchainPresentModeKnown = true;
    input.swapchainPresentMode = VK_PRESENT_MODE_FIFO_KHR;
    input.hasPresentModeSelection = true;
    input.presentModeSelectionIsChainHead = true;
    input.presentModeSelectionSwapchainCount = 1;
    input.selectedPresentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    EXPECT_TRUE(ce::vulkan_present_chain::DecidePresentModeSelection(input).forceCreatedMode);
    input.meteredBatchPresent = true;
    const auto decision = ce::vulkan_present_chain::DecidePresentModeSelection(input);
    EXPECT_FALSE(decision.forceCreatedMode);
    EXPECT_FALSE(decision.blockedByChainPosition);
}

}  // namespace
