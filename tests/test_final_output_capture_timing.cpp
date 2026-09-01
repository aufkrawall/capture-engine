#include <gtest/gtest.h>

#include <deque>

#include "../common/capture_pipeline_policy.h"
#include "../captureengine/display_timing_policy.h"
#include "../hook/common/capture_pacing.h"
#include "../hook/common/dxgi_shared.h"

namespace policy = ce::capture_policy;

namespace {
bool g_sawFinalOutputPresentScope = false;

void ObserveFinalOutputPresentScope(IDXGISwapChain*) {
    g_sawFinalOutputPresentScope = DXGIShared::IsPostSLFinalOutputPresentCallback();
}
}  // namespace

TEST(FinalOutputCaptureTimingTest, RuntimePresentScopeExcludesSyntheticPostSLServiceCalls) {
    g_sawFinalOutputPresentScope = false;
    EXPECT_FALSE(DXGIShared::IsPostSLFinalOutputPresentCallback());
    DXGIShared::InvokePostSLCallbackForFinalOutputPresent(&ObserveFinalOutputPresentScope, nullptr);
    EXPECT_TRUE(g_sawFinalOutputPresentScope);
    EXPECT_FALSE(DXGIShared::IsPostSLFinalOutputPresentCallback());
}

TEST(FinalOutputCaptureTimingTest, CpuBurstIsSpreadAcrossOutputIntervals) {
    policy::FinalOutputTimelineState state;
    constexpr int64_t kFrequency = 10'000'000;
    constexpr int64_t kCallbackQpc = 100'000'000;

    const int64_t first = policy::NextFinalOutputTimestampQpc(state, kCallbackQpc, kFrequency, 144.0f);
    const int64_t second = policy::NextFinalOutputTimestampQpc(state, kCallbackQpc + 100, kFrequency, 144.0f);
    const int64_t third = policy::NextFinalOutputTimestampQpc(state, kCallbackQpc + 200, kFrequency, 144.0f);

    EXPECT_EQ(first, kCallbackQpc);
    EXPECT_NEAR(second - first, kFrequency / 144.0, 2.0);
    EXPECT_NEAR(third - second, kFrequency / 144.0, 2.0);
}

TEST(FinalOutputCaptureTimingTest, ValidFourOutputBurstDoesNotHitVirtualLeadGuard) {
    policy::FinalOutputTimelineState state;
    constexpr int64_t kFrequency = 10'000'000;
    constexpr int64_t kCallbackQpc = 100'000'000;

    int64_t timestamp = 0;
    for (int output = 0; output < 4; ++output) {
        timestamp = policy::NextFinalOutputTimestampQpc(
            state, kCallbackQpc + output * 100, kFrequency, 144.0f);
    }

    EXPECT_GT(timestamp, kCallbackQpc);
    EXPECT_EQ(state.virtualLeadClampCount.load(), 0u);
}

TEST(FinalOutputCaptureTimingTest, RecordingEpochDiscardsIdleVirtualClockPhase) {
    policy::FinalOutputTimelineState state;
    state.lastTimestampQpc.store(200'000'000);
    state.estimatedIntervalQpc.store(152'770);
    state.lastSourcePresentQpc.store(100'000'000);
    state.callbacksSinceSourcePresent.store(99);
    state.sourceGroupIntervalValid.store(true);
    state.sourceGroupExpiryCount.store(2);
    state.virtualLeadClampCount.store(3);

    EXPECT_FALSE(policy::UpdateFinalOutputCaptureEpoch(state, false));
    EXPECT_TRUE(policy::UpdateFinalOutputCaptureEpoch(state, true));
    EXPECT_EQ(state.lastTimestampQpc.load(), 0);
    EXPECT_EQ(state.estimatedIntervalQpc.load(), 0);
    EXPECT_EQ(state.lastSourcePresentQpc.load(), 0);
    EXPECT_EQ(state.sourceGroupAuthorityQpc.load(), 0);
    EXPECT_EQ(state.callbacksSinceSourcePresent.load(), 0u);
    EXPECT_FALSE(state.sourceGroupIntervalValid.load());
    EXPECT_EQ(state.sourceGroupExpiryCount.load(), 0u);
    EXPECT_EQ(state.virtualLeadClampCount.load(), 0u);
    EXPECT_FALSE(policy::UpdateFinalOutputCaptureEpoch(state, true));
    EXPECT_FALSE(policy::UpdateFinalOutputCaptureEpoch(state, false));
}

TEST(FinalOutputCaptureTimingTest, StaleSourceGroupCannotRunVirtualClockIntoFuture) {
    policy::FinalOutputTimelineState state;
    constexpr int64_t kFrequency = 10'000'000;
    constexpr int64_t kFirstCallbackQpc = 100'000'000;
    constexpr int64_t kActualOutputIntervalQpc = 72'000;
    constexpr int64_t kStaleSourceIntervalQpc = 152'770;
    constexpr float kObservedOutputFps =
        static_cast<float>(kFrequency) / static_cast<float>(kActualOutputIntervalQpc);

    state.lastTimestampQpc.store(kFirstCallbackQpc);
    state.estimatedIntervalQpc.store(kStaleSourceIntervalQpc);
    state.lastSourcePresentQpc.store(kFirstCallbackQpc);
    state.sourceGroupAuthorityQpc.store(kFirstCallbackQpc);
    state.sourceGroupIntervalValid.store(true);

    int64_t previousTimestamp = kFirstCallbackQpc;
    int64_t callbackQpc = kFirstCallbackQpc;
    for (int output = 1; output <= 1200; ++output) {
        callbackQpc = kFirstCallbackQpc + output * kActualOutputIntervalQpc;
        const int64_t timestamp = policy::NextFinalOutputTimestampQpc(
            state, callbackQpc, kFrequency, kObservedOutputFps);
        EXPECT_GT(timestamp, previousTimestamp);
        previousTimestamp = timestamp;
    }

    EXPECT_GE(state.sourceGroupExpiryCount.load(), 1u);
    EXPECT_GE(state.virtualLeadClampCount.load(), 1u);
    EXPECT_FALSE(state.sourceGroupIntervalValid.load());
    EXPECT_NEAR(state.estimatedIntervalQpc.load(), kActualOutputIntervalQpc, 1000);
    EXPECT_LE(previousTimestamp - callbackQpc, state.estimatedIntervalQpc.load() * 4);
}

TEST(FinalOutputCaptureTimingTest, CompletedSourceGroupRefinesUnknownOutputRate) {
    policy::FinalOutputTimelineState state;
    constexpr int64_t kFrequency = 10'000'000;
    constexpr int64_t kFirstSourceQpc = 50'000'000;

    policy::ObserveFinalOutputSourcePresent(state, kFirstSourceQpc, kFrequency);
    for (int i = 0; i < 4; ++i)
        policy::NextFinalOutputTimestampQpc(state, kFirstSourceQpc + 333'333, kFrequency, 240.0f);
    policy::ObserveFinalOutputSourcePresent(state, kFirstSourceQpc + 333'333, kFrequency);

    EXPECT_NEAR(state.estimatedIntervalQpc.load(), 83'333, 5'000);
}

TEST(FinalOutputCaptureTimingTest, DisplayWatermarkWaitsPastDelayedBacklogForNearestTimestamp) {
    SharedDisplayTiming timing;
    timing.Reset(100, 100, DisplayTimingStatus::Starting);
    const auto watermark = policy::CaptureDisplayTimingPublicationWatermark(timing);
    ASSERT_TRUE(watermark);
    EXPECT_EQ(watermark.sequence, 0u);

    uint64_t matchedSequence = 0;
    int64_t timestampQpc = 0;
    timing.Publish(9'980'000, 10'020'000);
    timing.Publish(9'990'000, 10'030'000);
    EXPECT_EQ(policy::ResolveDisplayTimingAfterWatermark(
                  timing, watermark.sequence, watermark.generation, 1, 100'000'000, 10'000'000,
                  2'000'000, &matchedSequence, &timestampQpc),
              policy::DisplayTimingResolution::kPending);

    timing.Publish(10'000'100, 10'040'000);
    EXPECT_EQ(policy::ResolveDisplayTimingAfterWatermark(
                  timing, watermark.sequence, watermark.generation, 1, 100'000'000, 10'000'000,
                  2'000'000, &matchedSequence, &timestampQpc),
              policy::DisplayTimingResolution::kResolved);
    EXPECT_EQ(matchedSequence, 3u);
    EXPECT_EQ(timestampQpc, 100'001'000);
}

TEST(FinalOutputCaptureTimingTest, DisplayGenerationChangeInvalidatesWatermark) {
    SharedDisplayTiming timing;
    timing.Reset(100, 100, DisplayTimingStatus::Starting);
    const auto watermark = policy::CaptureDisplayTimingPublicationWatermark(timing);
    ASSERT_TRUE(watermark);

    timing.Reset(200, 200, DisplayTimingStatus::Starting);
    uint64_t matchedSequence = 0;
    int64_t timestampQpc = 0;
    EXPECT_EQ(policy::ResolveDisplayTimingAfterWatermark(
                  timing, watermark.sequence, watermark.generation, 1, 100'000'000, 10'000'000,
                  2'000'000, &matchedSequence, &timestampQpc),
              policy::DisplayTimingResolution::kInvalid);
}

TEST(FinalOutputCaptureTimingTest, LateDisplayPhaseShiftPreservesCommittedOrderAndIntervals) {
    policy::FinalOutputTimestampOrderState state;
    state.previousQpc = 1'000;

    const int64_t first = policy::PreserveFinalOutputTimestampOrder(state, 900);
    const int64_t second = policy::PreserveFinalOutputTimestampOrder(state, 1'000);

    EXPECT_EQ(first, 1'001);
    EXPECT_EQ(second, 1'101);
}

TEST(FinalOutputCaptureTimingTest, DisplayCadenceExcludesAbsoluteTransportLatency) {
    constexpr int64_t virtualTimestamp = 10'000;

    EXPECT_EQ(policy::NormalizeFinalOutputDisplayTimestampQpc(
                  virtualTimestamp, /*displayTimestampQpc=*/10'500,
                  /*smoothedCorrelationPhaseQpc=*/500),
              virtualTimestamp);
    EXPECT_EQ(policy::NormalizeFinalOutputDisplayTimestampQpc(
                  virtualTimestamp, /*displayTimestampQpc=*/10'520,
                  /*smoothedCorrelationPhaseQpc=*/500),
              virtualTimestamp + 20);
    EXPECT_EQ(policy::NormalizeFinalOutputDisplayTimestampQpc(
                  virtualTimestamp, /*displayTimestampQpc=*/10'480,
                  /*smoothedCorrelationPhaseQpc=*/500),
              virtualTimestamp - 20);
}

TEST(FinalOutputCaptureTimingTest, GrowingDisplayQueuePhaseStaysNearVirtualTimeline) {
    int64_t smoothedTransportPhase = 150;
    int64_t maximumCorrection = 0;
    for (int frame = 0; frame < 600; ++frame) {
        const int64_t virtualTimestamp = 100'000 + static_cast<int64_t>(frame) * 100;
        const int64_t measuredTransportPhase = 150 + frame;
        smoothedTransportPhase =
            (smoothedTransportPhase * 7 + measuredTransportPhase) / 8;
        const int64_t normalized = policy::NormalizeFinalOutputDisplayTimestampQpc(
            virtualTimestamp, virtualTimestamp + measuredTransportPhase,
            smoothedTransportPhase);
        maximumCorrection =
            std::max(maximumCorrection, normalized - virtualTimestamp);
    }

    EXPECT_LT(maximumCorrection, 10);
    EXPECT_GT(smoothedTransportPhase, 700)
        << "the driver transport phase may grow without moving CFR content into the future";
}

TEST(FinalOutputCaptureTimingTest, FinalOutputToBasePathKeepsTimestampContinuity) {
    const int64_t offset = policy::GetCapturePathContinuityOffsetQpc(
        /*previousAdjustedQpc=*/1'070, /*firstRawQpc=*/1'000);

    EXPECT_EQ(offset, 71);
    EXPECT_EQ(1'000 + offset, 1'071);
    EXPECT_EQ(policy::GetCapturePathContinuityOffsetQpc(1'000, 1'100), 0);
}

TEST(CapturePipelinePolicyTest, InjectTimestampRetentionCoversDisplayPhaseJump) {
    constexpr size_t baseline = 13;
    constexpr size_t reserve = 1;
    constexpr int64_t contentDelay = 316'667;
    constexpr int64_t cutsceneDisplayPhase = 700'000;

    EXPECT_EQ(policy::GetInjectTimestampRetentionLimit(
                  baseline, reserve, contentDelay,
                  /*sourceIntervalQpc=*/83'333, /*maximumAdaptiveLimit=*/28),
              baseline);
    EXPECT_EQ(policy::GetInjectTimestampRetentionLimit(
                  baseline, reserve, contentDelay + cutsceneDisplayPhase,
                  /*sourceIntervalQpc=*/83'333, /*maximumAdaptiveLimit=*/28),
              16u);
    EXPECT_EQ(policy::GetInjectTimestampRetentionLimit(
                  baseline, reserve, contentDelay + cutsceneDisplayPhase + /*virtualLeadQpc=*/300'000,
                  /*sourceIntervalQpc=*/83'333, /*maximumAdaptiveLimit=*/28),
              19u);
    EXPECT_EQ(policy::GetInjectTimestampRetentionLimit(
                  baseline, reserve, contentDelay + cutsceneDisplayPhase,
                  /*sourceIntervalQpc=*/41'667, /*maximumAdaptiveLimit=*/28),
              28u);
    EXPECT_EQ(policy::GetInjectTimestampRetentionLimit(
                  baseline, reserve, /*requiredTimestampSpanQpc=*/5'000'000,
                  /*sourceIntervalQpc=*/41'667, /*maximumAdaptiveLimit=*/28),
              28u);
}

TEST(CapturePipelinePolicyTest, AdaptiveInjectRetentionLeavesProducerTextureLeases) {
    constexpr size_t minimumRequired = 7;
    const size_t ceiling = policy::GetInjectRetentionCeiling(
        minimumRequired, /*metadataSlotCount=*/32, /*textureSlotCount=*/16);

    EXPECT_EQ(ceiling, 14u);
    EXPECT_EQ(policy::GetInjectTimestampRetentionLimit(
                  /*baselineLimit=*/13, /*injectReserveFrames=*/1,
                  /*requiredTimestampSpanQpc=*/5'000'000,
                  /*sourceIntervalQpc=*/41'667, ceiling),
              14u);
    EXPECT_EQ(std::min(/*desiredStartupLimit=*/size_t{49}, ceiling), 14u)
        << "unattainable startup headroom must not consume every producer texture";
    EXPECT_EQ(policy::GetInjectRetentionCeiling(
                  /*minimumRequiredLimit=*/15, /*metadataSlotCount=*/32,
                  /*textureSlotCount=*/16),
              15u)
        << "the physical cap must not silently shorten a configured A/V delay";
}

TEST(CapturePipelinePolicyTest, RetainedInjectHistoryDoesNotThrottleProducer) {
    EXPECT_FALSE(policy::ShouldThrottleInjectProducer(
        /*ingressQueueDepth=*/0, /*ingressQueueCapacity=*/32,
        /*latestFenceWaitUs=*/0));
    EXPECT_FALSE(policy::ShouldThrottleInjectProducer(
        /*ingressQueueDepth=*/15, /*ingressQueueCapacity=*/32,
        /*latestFenceWaitUs=*/0));
    EXPECT_TRUE(policy::ShouldThrottleInjectProducer(
        /*ingressQueueDepth=*/16, /*ingressQueueCapacity=*/32,
        /*latestFenceWaitUs=*/0));
    EXPECT_TRUE(policy::ShouldThrottleInjectProducer(
        /*ingressQueueDepth=*/0, /*ingressQueueCapacity=*/32,
        /*latestFenceWaitUs=*/16'001));
}

TEST(CapturePipelinePolicyTest, InjectCapPreservesNearOrFutureFrontForSelection) {
    constexpr int64_t target = 1'000;
    constexpr int64_t leadTolerance = 50;

    EXPECT_TRUE(policy::ShouldPreserveInjectFrontAtBufferCap(
        /*frontTimestampQpc=*/1'051, target, leadTolerance));
    EXPECT_TRUE(policy::ShouldPreserveInjectFrontAtBufferCap(
        /*frontTimestampQpc=*/1'050, target, leadTolerance));
    EXPECT_TRUE(policy::ShouldPreserveInjectFrontAtBufferCap(
        /*frontTimestampQpc=*/950, target, leadTolerance));
    EXPECT_FALSE(policy::ShouldPreserveInjectFrontAtBufferCap(
        /*frontTimestampQpc=*/949, target, leadTolerance));
}

TEST(CapturePipelinePolicyTest, InjectDisplayPhaseStepCannotLatchRepeatLoop) {
    auto run = [](size_t bufferLimit, bool preserveFutureFront) {
        std::deque<int64_t> sourceFrames;
        int emits = 0;
        int64_t lastEmitted = 0;
        for (int tick = 0; tick < 80; ++tick) {
            const int64_t target = 10'000 + static_cast<int64_t>(tick) * 100;
            sourceFrames.push_back(target + 1'500);
            while (sourceFrames.size() > bufferLimit) {
                const bool preserve =
                    preserveFutureFront &&
                    policy::ShouldPreserveInjectFrontAtBufferCap(
                        sourceFrames.front(), target, /*leadToleranceQpc=*/50);
                if (preserve) {
                    sourceFrames.pop_back();
                } else {
                    sourceFrames.pop_front();
                }
            }
            if (!sourceFrames.empty()) {
                const auto decision = policy::DecideCfrNearestPlayout(
                    sourceFrames.front(), target, /*leadToleranceQpc=*/50, lastEmitted);
                if (decision.emit) {
                    lastEmitted = sourceFrames.front();
                    sourceFrames.pop_front();
                    ++emits;
                }
            }
        }
        return emits;
    };

    constexpr size_t undersizedLegacyLimit = 13;
    const size_t adaptiveLimit = policy::GetInjectTimestampRetentionLimit(
        undersizedLegacyLimit, /*injectReserveFrames=*/1,
        /*requiredTimestampSpanQpc=*/1'500, /*sourceIntervalQpc=*/100,
        /*maximumAdaptiveLimit=*/28);

    EXPECT_EQ(run(undersizedLegacyLimit, false), 0);
    EXPECT_GT(run(adaptiveLimit, false), 60);
    EXPECT_GT(run(undersizedLegacyLimit, true), 30);
}

TEST(FinalOutputCaptureTimingTest, DisplayWatermarkAndMinimumSequencePreventSampleReuse) {
    SharedDisplayTiming timing;
    timing.Reset(100, 100, DisplayTimingStatus::Starting);
    timing.Publish(990'000, 990'000);
    const auto watermark = policy::CaptureDisplayTimingPublicationWatermark(timing);
    ASSERT_TRUE(watermark);
    ASSERT_EQ(watermark.sequence, 1u);
    timing.Publish(1'000'000, 1'020'000);
    timing.Publish(1'010'000, 1'030'000);
    timing.Publish(1'020'000, 1'040'000);

    uint64_t matchedSequence = 0;
    int64_t timestampQpc = 0;
    ASSERT_EQ(policy::ResolveDisplayTimingAfterWatermark(
                  timing, watermark.sequence, watermark.generation, 1, 1'001'000, 1'000'000,
                  200'000, &matchedSequence, &timestampQpc),
              policy::DisplayTimingResolution::kResolved);
    EXPECT_EQ(matchedSequence, 2u);
    EXPECT_EQ(timestampQpc, 1'000'000);

    const uint64_t firstMatchedSequence = matchedSequence;
    ASSERT_EQ(policy::ResolveDisplayTimingAfterWatermark(
                  timing, watermark.sequence, watermark.generation, firstMatchedSequence + 1,
                  1'011'000, 1'000'000, 200'000, &matchedSequence, &timestampQpc),
              policy::DisplayTimingResolution::kResolved);
    EXPECT_EQ(matchedSequence, 3u);
    EXPECT_EQ(timestampQpc, 1'010'000);
}

TEST(FinalOutputCaptureTimingTest, InitialDisplayPhaseCanExceedSteadyStateTolerance) {
    SharedDisplayTiming timing;
    timing.Reset(100, 100, DisplayTimingStatus::Starting);
    const auto watermark = policy::CaptureDisplayTimingPublicationWatermark(timing);
    ASSERT_TRUE(watermark);
    timing.Publish(10'060'000, 10'090'000);

    uint64_t matchedSequence = 0;
    int64_t timestampQpc = 0;
    EXPECT_EQ(policy::ResolveDisplayTimingAfterWatermark(
                  timing, watermark.sequence, watermark.generation, 1, 100'000'000, 10'000'000,
                  2'500'000, &matchedSequence, &timestampQpc),
              policy::DisplayTimingResolution::kResolved);
    EXPECT_EQ(timestampQpc, 100'600'000);
    EXPECT_EQ(policy::ResolveDisplayTimingAfterWatermark(
                  timing, watermark.sequence, watermark.generation, 1, 100'000'000, 10'000'000,
                  200'000, &matchedSequence, &timestampQpc),
              policy::DisplayTimingResolution::kPhaseMismatch);
}

TEST(FinalOutputCaptureTimingTest, SustainedDisplayPhaseMismatchRequestsReacquisition) {
    EXPECT_FALSE(policy::ShouldReacquireFinalOutputDisplayPhase(0));
    EXPECT_FALSE(policy::ShouldReacquireFinalOutputDisplayPhase(3));
    EXPECT_TRUE(policy::ShouldReacquireFinalOutputDisplayPhase(4));
    EXPECT_TRUE(policy::ShouldReacquireFinalOutputDisplayPhase(20));
}

TEST(FinalOutputCaptureTimingTest, SourceGroupIntervalCannotBeOverwrittenByBurstyPresentationMetric) {
    policy::FinalOutputTimelineState state;
    constexpr int64_t kFrequency = 10'000'000;
    constexpr int64_t kFirstSourceQpc = 50'000'000;

    policy::ObserveFinalOutputSourcePresent(state, kFirstSourceQpc, kFrequency);
    for (int i = 0; i < 4; ++i)
        policy::NextFinalOutputTimestampQpc(state, kFirstSourceQpc + 333'333, kFrequency, 240.0f);
    policy::ObserveFinalOutputSourcePresent(state, kFirstSourceQpc + 333'333, kFrequency);
    const int64_t groupInterval = state.estimatedIntervalQpc.load();

    policy::NextFinalOutputTimestampQpc(state, kFirstSourceQpc + 666'666, kFrequency, 60.0f);
    EXPECT_EQ(state.estimatedIntervalQpc.load(), groupInterval);
}

TEST(FinalOutputCaptureTimingTest, MultiplierChangeScalesStrongIntervalWithoutLosingPhase) {
    policy::FinalOutputTimelineState state;
    state.lastTimestampQpc.store(100'000'000);
    state.estimatedIntervalQpc.store(83'333);
    state.lastSourcePresentQpc.store(99'900'000);
    state.callbacksSinceSourcePresent.store(1);

    policy::AdjustFinalOutputTimelineForMultiplierChange(state, 2, 4, 100'000'000);

    EXPECT_EQ(state.estimatedIntervalQpc.load(), 41'666);
    EXPECT_EQ(state.lastTimestampQpc.load(), 100'000'000);
    EXPECT_EQ(state.lastSourcePresentQpc.load(), 99'900'000);
    EXPECT_EQ(state.sourceGroupAuthorityQpc.load(), 100'000'000);
    EXPECT_EQ(state.callbacksSinceSourcePresent.load(), 1u);
    EXPECT_TRUE(state.sourceGroupIntervalValid.load());
}

TEST(FinalOutputCaptureTimingTest, MultiplierTransitionAuthorityExpiresWithoutSourceBoundary) {
    policy::FinalOutputTimelineState state;
    constexpr int64_t kFrequency = 10'000'000;
    constexpr int64_t kTransitionQpc = 100'000'000;
    state.lastTimestampQpc.store(kTransitionQpc);
    state.estimatedIntervalQpc.store(83'333);

    policy::AdjustFinalOutputTimelineForMultiplierChange(
        state, 2, 4, kTransitionQpc);
    policy::NextFinalOutputTimestampQpc(
        state, kTransitionQpc + 3'000'000, kFrequency, 144.0f);

    EXPECT_FALSE(state.sourceGroupIntervalValid.load());
    EXPECT_EQ(state.sourceGroupExpiryCount.load(), 1u);
    EXPECT_NEAR(state.estimatedIntervalQpc.load(), kFrequency / 144.0, 2.0);
}

TEST(FinalOutputCaptureTimingTest, VulkanMeteringConfigurationCoversEveryPresentInBatch) {
    std::atomic<uint32_t> remaining{0};

    EXPECT_TRUE(policy::ConsumeFinalOutputMeteredBatchPresent(remaining, 4));
    EXPECT_EQ(remaining.load(), 3u);
    EXPECT_TRUE(policy::ConsumeFinalOutputMeteredBatchPresent(remaining, 0));
    EXPECT_TRUE(policy::ConsumeFinalOutputMeteredBatchPresent(remaining, 0));
    EXPECT_TRUE(policy::ConsumeFinalOutputMeteredBatchPresent(remaining, 0));
    EXPECT_FALSE(policy::ConsumeFinalOutputMeteredBatchPresent(remaining, 0));
    EXPECT_EQ(remaining.load(), 0u);
}

TEST(FinalOutputCaptureTimingTest, InjectRecordingCollectsDisplayTimingWithoutChangingOverlaySource) {
    EXPECT_TRUE(ShouldCollectDisplayTiming(false, FrameTimeSource::Presentation, true));
    EXPECT_TRUE(ShouldCollectDisplayTiming(false, FrameTimeSource::DisplayChange, false));
    EXPECT_FALSE(ShouldCollectDisplayTiming(true, FrameTimeSource::DisplayChange, true));
    EXPECT_FALSE(ShouldCollectDisplayTiming(false, FrameTimeSource::Presentation, false));
}

TEST(FinalOutputCaptureTimingTest, FinalOutputPublicationUsesTwoTimesCfrHeadroom) {
    EXPECT_EQ(policy::GetInjectCfrSourcePublicationFps(
                  60, policy::kFinalOutputCfrPublicationHeadroomPermille),
              120u);
    EXPECT_EQ(policy::GetInjectCfrSourcePublicationIntervalUs(
                  120, policy::kFinalOutputCfrPublicationHeadroomPermille),
              4166);
}

TEST(FinalOutputCaptureTimingTest, FinalOutputCadenceUsesIndependentTwoTimesGate) {
    SharedMemoryLayout sharedMemory;
    sharedMemory.runtimeState.captureRequested.store(true, std::memory_order_release);
    sharedMemory.runtimeState.SetRuntimeFlag(kCaptureRuntimeFlagInjectVideoCaptureRequested, true);
    sharedMemory.fpsLimiter.SetCaptureFps(60);
    CaptureCadenceGateState gate;

    EXPECT_FALSE(ShouldSkipCaptureForTargetCadenceAtUs(
        &sharedMemory, "test", 1'000'000, gate, policy::kFinalOutputCfrPublicationHeadroomPermille));
    EXPECT_TRUE(ShouldSkipCaptureForTargetCadenceAtUs(
        &sharedMemory, "test", 1'007'000, gate, policy::kFinalOutputCfrPublicationHeadroomPermille));
    EXPECT_FALSE(ShouldSkipCaptureForTargetCadenceAtUs(
        &sharedMemory, "test", 1'008'000, gate, policy::kFinalOutputCfrPublicationHeadroomPermille));

    gate.Reset();
    EXPECT_FALSE(ShouldSkipCaptureForTargetCadenceAtUs(
        &sharedMemory, "test", 1'007'000, gate, policy::kFinalOutputCfrPublicationHeadroomPermille));
}
