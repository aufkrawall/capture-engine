#include <gtest/gtest.h>

#include <deque>
#include <vector>

#include "common/capture/capture_pipeline_policy.h"
#include "captureengine/display_timing/display_timing_policy.h"
#include "hook/pacing/capture_pacing.h"
#include "hook/fg/fg_cost_probe.h"
#include "hook/present/dxgi_shared.h"

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

TEST(FinalOutputCaptureTimingTest, SuspendedCaptureClaimIsBoundedToTheEnclosingPresent) {
    EXPECT_FALSE(DXGIShared::WasPostSLPresentedOutputCaptureRouted());
    DXGIShared::BeginPostSLOffKeepAlivePresentScope();
    EXPECT_FALSE(DXGIShared::WasPostSLPresentedOutputCaptureRouted());
    DXGIShared::MarkPostSLPresentedOutputCaptureRouted();
    EXPECT_TRUE(DXGIShared::WasPostSLPresentedOutputCaptureRouted());

    DXGIShared::BeginPostSLOffKeepAlivePresentScope();
    EXPECT_TRUE(DXGIShared::WasPostSLPresentedOutputCaptureRouted());
    DXGIShared::EndPostSLOffKeepAlivePresentScope();
    EXPECT_TRUE(DXGIShared::WasPostSLPresentedOutputCaptureRouted());

    DXGIShared::EndPostSLOffKeepAlivePresentScope();
    EXPECT_FALSE(DXGIShared::WasPostSLPresentedOutputCaptureRouted());
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

// Talos 3x/4x MFG (logs/20261018_222704): with an interval estimate shorter than
// the real output cadence the clock fell up to 8 intervals behind its callbacks
// (virtualLeadUs down to -27888), so frames reached media after their own
// timestamps and the CFR selector recorded repeat+skip pairs.
TEST(FinalOutputCaptureTimingTest, VirtualClockNeverRunsBehindTheCallbackSupplyingItsPixels) {
    policy::FinalOutputTimelineState state;
    constexpr int64_t kFrequency = 10'000'000;
    constexpr int64_t kFirstCallbackQpc = 100'000'000;
    constexpr int64_t kActualOutputIntervalQpc = 90'000;  // ~111 fps
    constexpr float kOverstatedOutputFps = 125.0f;        // estimate 80'000

    int64_t previousTimestamp = 0;
    for (int output = 0; output < 240; ++output) {
        const int64_t callbackQpc = kFirstCallbackQpc + output * kActualOutputIntervalQpc;
        const int64_t timestamp =
            policy::NextFinalOutputTimestampQpc(state, callbackQpc, kFrequency, kOverstatedOutputFps);
        ASSERT_GE(timestamp, callbackQpc) << "output " << output;
        ASSERT_GT(timestamp, previousTimestamp) << "output " << output;
        previousTimestamp = timestamp;
    }
    EXPECT_GT(state.causalAnchorCount.load(), 200u);
    EXPECT_EQ(state.maxCausalAnchorQpc.load(), 10'000);
    EXPECT_EQ(state.virtualLeadClampCount.load(), 0u);
}

TEST(FinalOutputCaptureTimingTest, LateBurstStartsAtItsCallbackAndStillSpreadsForward) {
    policy::FinalOutputTimelineState state;
    constexpr int64_t kFrequency = 10'000'000;
    constexpr int64_t kInterval = 72'000;
    constexpr int64_t kFirstBurstQpc = 100'000'000;
    state.estimatedIntervalQpc.store(kInterval);

    std::vector<int64_t> firstBurst;
    firstBurst.reserve(4);
    for (int output = 0; output < 4; ++output)
        firstBurst.push_back(policy::NextFinalOutputTimestampQpc(state, kFirstBurstQpc + output * 100, kFrequency, 0.0f));
    EXPECT_EQ(firstBurst[0], kFirstBurstQpc);
    EXPECT_EQ(firstBurst[3], kFirstBurstQpc + 3 * kInterval);

    // The next source frame arrives 5 ms after its burst slot would have begun.
    const int64_t lateBurstQpc = kFirstBurstQpc + 4 * kInterval + 50'000;
    const int64_t head = policy::NextFinalOutputTimestampQpc(state, lateBurstQpc, kFrequency, 0.0f);
    const int64_t second = policy::NextFinalOutputTimestampQpc(state, lateBurstQpc + 100, kFrequency, 0.0f);
    EXPECT_EQ(head, lateBurstQpc);
    EXPECT_EQ(second, lateBurstQpc + kInterval);
    EXPECT_EQ(state.causalAnchorCount.load(), 1u);
    EXPECT_EQ(state.maxCausalAnchorQpc.load(), 50'000);
}

TEST(FinalOutputCaptureTimingTest, SkippedOutputsDoNotPushThePlannedOutputAheadOfItsCallback) {
    policy::FinalOutputTimelineState state;
    constexpr int64_t kFrequency = 10'000'000;
    constexpr int64_t kInterval = 83'333;
    constexpr int64_t kLastTimestampQpc = 100'000'000;
    state.estimatedIntervalQpc.store(kInterval);
    state.lastTimestampQpc.store(kLastTimestampQpc);

    // Two contention-skipped outputs; only the planned output's (much later) callback is known.
    const int64_t callbackQpc = kLastTimestampQpc + 10 * kInterval;
    policy::AdvanceFinalOutputTimelineForSkippedOutput(state, kFrequency, 0.0f);
    policy::AdvanceFinalOutputTimelineForSkippedOutput(state, kFrequency, 0.0f);
    EXPECT_EQ(state.lastTimestampQpc.load(), kLastTimestampQpc + 2 * kInterval);
    EXPECT_EQ(state.callbacksSinceSourcePresent.load(), 0u);

    EXPECT_EQ(policy::NextFinalOutputTimestampQpc(state, callbackQpc, kFrequency, 0.0f), callbackQpc);
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

// A correlation has to say which of the two kinds of timestamp it matched. The
// recording timeline applies the matched sample's per-sample cadence residual,
// and a deferred completion the vertical-blank clock could not answer for
// carries the flip-latch time, whose lead over the scanout varies by
// milliseconds under frame generation - noise the file must not inherit.
TEST(FinalOutputCaptureTimingTest, ResolutionReportsWhetherTheMatchedSampleIsAScreenTime) {
    SharedDisplayTiming timing;
    timing.Reset(100, 100, DisplayTimingStatus::Starting);
    const auto watermark = policy::CaptureDisplayTimingPublicationWatermark(timing);
    ASSERT_TRUE(watermark);

    uint64_t matchedSequence = 0;
    int64_t timestampQpc = 0;
    bool screenTimeResolved = false;

    timing.Publish(9'990'000, 10'030'000, 0, /*screenTimeResolved=*/false);
    timing.Publish(10'000'100, 10'040'000, 0, /*screenTimeResolved=*/false);
    ASSERT_EQ(policy::ResolveDisplayTimingAfterWatermark(
                  timing, watermark.sequence, watermark.generation, 1, 100'000'000, 10'000'000,
                  2'000'000, &matchedSequence, &timestampQpc, &screenTimeResolved),
              policy::DisplayTimingResolution::kResolved);
    EXPECT_EQ(matchedSequence, 2u);
    EXPECT_FALSE(screenTimeResolved);

    // The nearest sample decides, not the newest: a resolved sample published
    // after an unresolved one must be reported as resolved.
    timing.Publish(10'010'000, 10'050'000, 0, /*screenTimeResolved=*/true);
    ASSERT_EQ(policy::ResolveDisplayTimingAfterWatermark(
                  timing, watermark.sequence, watermark.generation, 3, 100'100'000, 10'000'000,
                  2'000'000, &matchedSequence, &timestampQpc, &screenTimeResolved),
              policy::DisplayTimingResolution::kResolved);
    EXPECT_EQ(matchedSequence, 3u);
    EXPECT_TRUE(screenTimeResolved);

    // A caller that does not ask still gets the old behaviour.
    EXPECT_EQ(policy::ResolveDisplayTimingAfterWatermark(
                  timing, watermark.sequence, watermark.generation, 3, 100'100'000, 10'000'000,
                  2'000'000, &matchedSequence, &timestampQpc),
              policy::DisplayTimingResolution::kResolved);
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

// An output whose nearest sample the previous output already owns has none of its own. Handing it the next
// sample shifted every later match by one and taught the phase the shift (Talos 20261019_195220: 16 -> 528 ms).
TEST(FinalOutputCaptureTimingTest, ClaimedNearestSampleIsNotReplacedByTheNextOne) {
    SharedDisplayTiming timing;
    timing.Reset(100, 100, DisplayTimingStatus::Starting);
    timing.Publish(990'000, 990'000);
    const auto watermark = policy::CaptureDisplayTimingPublicationWatermark(timing);
    ASSERT_TRUE(watermark);
    timing.Publish(1'000'000, 1'020'000);
    timing.Publish(1'010'000, 1'030'000);
    timing.Publish(1'020'000, 1'040'000);

    uint64_t matchedSequence = 0;
    int64_t timestampQpc = 0;
    ASSERT_EQ(policy::ResolveDisplayTimingAfterWatermark(timing, watermark.sequence, watermark.generation, 1,
                                                         1'001'000, 1'000'000, 200'000, &matchedSequence,
                                                         &timestampQpc),
              policy::DisplayTimingResolution::kResolved);
    ASSERT_EQ(matchedSequence, 2u);
    EXPECT_EQ(policy::ResolveDisplayTimingAfterWatermark(timing, watermark.sequence, watermark.generation, 3,
                                                         1'002'000, 1'000'000, 200'000, &matchedSequence,
                                                         &timestampQpc),
              policy::DisplayTimingResolution::kClaimed)
        << "the next sample belongs to the next output";
    ASSERT_EQ(policy::ResolveDisplayTimingAfterWatermark(timing, watermark.sequence, watermark.generation, 3,
                                                         1'011'000, 1'000'000, 200'000, &matchedSequence,
                                                         &timestampQpc),
              policy::DisplayTimingResolution::kResolved);
    EXPECT_EQ(matchedSequence, 3u);
}

// Media's correlation loop over a stream whose every tenth screen change never arrives: the learned phase must
// stay at the transport latency instead of walking one interval further per missing event.
TEST(FinalOutputCaptureTimingTest, MissingDisplaySamplesCannotWalkTheLearnedPhase) {
    SharedDisplayTiming timing;
    timing.Reset(100, 100, DisplayTimingStatus::Starting);
    const auto generationWatermark = policy::CaptureDisplayTimingPublicationWatermark(timing);
    ASSERT_TRUE(generationWatermark);
    constexpr int64_t kFrequency = 1'000'000;  // QPC ticks are microseconds here
    constexpr int64_t kInterval = 8'333;
    constexpr int64_t kTransport = 20'000;
    constexpr int64_t kStart = 1'000'000;
    struct Observation {
        uint64_t watermark;
        int64_t virtualQpc;
    };
    std::deque<Observation> pending;
    uint64_t lastMatched = 0;
    bool offsetValid = false;
    int64_t offset = 0;
    uint32_t mismatchStreak = 0;
    int64_t maxOffset = 0;
    uint64_t claimed = 0;
    auto resolvePending = [&] {
        while (!pending.empty()) {
            const Observation& observation = pending.front();
            const int64_t target = offsetValid ? observation.virtualQpc + offset : observation.virtualQpc;
            const int64_t window = offsetValid ? kFrequency / 50 : kFrequency / 2;
            uint64_t matched = 0;
            int64_t displayQpc = 0;
            const auto resolution = policy::ResolveDisplayTimingAfterWatermark(
                timing, observation.watermark, generationWatermark.generation, lastMatched + 1, target, kFrequency,
                window, &matched, &displayQpc);
            if (resolution == policy::DisplayTimingResolution::kPending)
                break;
            if (resolution == policy::DisplayTimingResolution::kResolved) {
                lastMatched = matched;
                const int64_t measured = displayQpc - observation.virtualQpc;
                offset = offsetValid ? (offset * 7 + measured) / 8 : measured;
                offsetValid = true;
                mismatchStreak = 0;
            } else if (resolution == policy::DisplayTimingResolution::kPhaseMismatch && offsetValid) {
                if (policy::ShouldReacquireFinalOutputDisplayPhase(++mismatchStreak)) {
                    offsetValid = false;
                    offset = 0;
                    mismatchStreak = 0;
                    lastMatched = 0;
                }
            } else if (resolution == policy::DisplayTimingResolution::kClaimed) {
                ++claimed;
            }
            if (offsetValid)
                maxOffset = std::max(maxOffset, offset);
            pending.pop_front();
        }
    };
    int nextScreenChange = 0;
    for (int output = 0; output < 400; ++output) {
        const int64_t presentQpc = kStart + output * kInterval;
        while (kStart + nextScreenChange * kInterval + kTransport <= presentQpc) {
            if (nextScreenChange % 10 != 9)
                timing.Publish(kStart + nextScreenChange * kInterval + kTransport, presentQpc);
            ++nextScreenChange;
        }
        pending.push_back({timing.writeSequence.load(std::memory_order_acquire), presentQpc});
        resolvePending();
    }
    EXPECT_GT(claimed, 0u);
    EXPECT_TRUE(offsetValid);
    EXPECT_LE(maxOffset, kTransport + kInterval / 2) << "the phase walked with the missing events";
    EXPECT_NEAR(offset, kTransport, kInterval / 2);
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

TEST(DisplayTimingPolicyTest, ProbeMaskParsesTheEtwSuppressionBitWithoutDisturbingOthers) {
    // The sensor process reads the same variable as the hooked process, so the
    // bit has to survive both spellings the documented mask accepts.
    using namespace ce::fg_cost_probe;
    EXPECT_EQ(ParseMask("0x40000") & kDisplayTimingEtwOff, kDisplayTimingEtwOff);
    EXPECT_EQ(ParseMask("262144") & kDisplayTimingEtwOff, kDisplayTimingEtwOff);
    EXPECT_EQ(ParseMask("0x48000") & kDisplayTimingEtwOff, kDisplayTimingEtwOff);
    EXPECT_EQ(ParseMask("0x48000") & kQueueAdoptionOff, kQueueAdoptionOff);
    // Anything else must leave the collection policy alone.
    EXPECT_EQ(ParseMask("0x8000") & kDisplayTimingEtwOff, 0u);
    EXPECT_EQ(ParseMask("") & kDisplayTimingEtwOff, 0u);
    EXPECT_EQ(ParseMask("0x4000g") & kDisplayTimingEtwOff, 0u);
    // The bit is a separate decision from the source policy, which stays intact.
    EXPECT_TRUE(ShouldCollectDisplayTiming(false, FrameTimeSource::DisplayChange, false));
    EXPECT_TRUE(ShouldCollectDisplayTiming(false, FrameTimeSource::Presentation, true));
    EXPECT_FALSE(ShouldCollectDisplayTiming(true, FrameTimeSource::DisplayChange, true));
}
