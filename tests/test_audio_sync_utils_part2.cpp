#include "test_audio_sync_utils_shared.h"

TEST(AudioSyncUtilsTest, TimelineGapClampHandlesDegenerateInputs) {
    EXPECT_EQ(ce::audio::ClampTimelineGapSamplesToCapacity(0, 48000), 0);
    EXPECT_EQ(ce::audio::ClampTimelineGapSamplesToCapacity(-100, 48000), 0);
    // Unknown/zero capacity disables the bound (cannot clamp without a reference).
    EXPECT_EQ(ce::audio::ClampTimelineGapSamplesToCapacity(5000, 0), 5000);
}

TEST(AudioSyncUtilsTest, PacketTimelineAdjustmentClampsNegativePacketStarts) {
    const auto adjustment = ce::audio::ComputePacketTimelineAdjustment(-200, 0, 48);
    EXPECT_EQ(adjustment.gapSamples, 0);
    EXPECT_EQ(adjustment.overlapSamples, 0);
}

TEST(AudioSyncUtilsTest, StartupAwarePacketTimelineAdjustmentUsesWiderStartupSlop) {
    const auto adjustment = ce::audio::ComputeStartupAwarePacketTimelineAdjustment(1100, 960, 48, 7200, 192, 240);
    EXPECT_EQ(adjustment.gapSamples, 0);
    EXPECT_EQ(adjustment.overlapSamples, 0);
}

TEST(AudioSyncUtilsTest, StartupAwarePacketTimelineAdjustmentSuppressesSmallStartupOverlapTrim) {
    const auto adjustment = ce::audio::ComputeStartupAwarePacketTimelineAdjustment(800, 960, 48, 7200, 192, 240);
    EXPECT_EQ(adjustment.gapSamples, 0);
    EXPECT_EQ(adjustment.overlapSamples, 0);
}

TEST(AudioSyncUtilsTest, StartupAwarePacketTimelineAdjustmentKeepsLargeOverlapTrim) {
    const auto adjustment = ce::audio::ComputeStartupAwarePacketTimelineAdjustment(600, 960, 48, 7200, 192, 240);
    EXPECT_EQ(adjustment.gapSamples, 0);
    EXPECT_EQ(adjustment.overlapSamples, 360);
}

TEST(AudioSyncUtilsTest, StartupFirstPacketRebaseOffsetOnlyAppliesAfterSyncPendingCapture) {
    EXPECT_EQ(ce::audio::ComputeStartupFirstPacketRebaseOffset(11504, true, 480, 2400), 11024);
    EXPECT_EQ(ce::audio::ComputeStartupFirstPacketRebaseOffset(11504, false, 480, 2400), 0);
    EXPECT_EQ(ce::audio::ComputeStartupFirstPacketRebaseOffset(2000, true, 480, 2400), 0);
    EXPECT_EQ(ce::audio::ComputeStartupFirstPacketRebaseOffset(480, true, 480, 2400), 0);
}

TEST(AudioSyncUtilsTest, SharedStartupRebasePreservesInterSourceFirstPacketDelta) {
    const int64_t earlySourceStart = 8804;
    const int64_t lateSourceStart = 9855;
    const int64_t sharedOffset = ce::audio::ComputeSharedStartupFirstPacketRebaseOffset(earlySourceStart, 480, 2400);

    const int64_t rebasedEarly = ce::audio::ApplyStartupPacketTimelineRebaseOffset(earlySourceStart, sharedOffset);
    const int64_t rebasedLate = ce::audio::ApplyStartupPacketTimelineRebaseOffset(lateSourceStart, sharedOffset);

    EXPECT_EQ(sharedOffset, 8324);
    EXPECT_EQ(rebasedEarly, 480);
    EXPECT_EQ(rebasedLate, 1531);
    EXPECT_EQ(rebasedLate - rebasedEarly, lateSourceStart - earlySourceStart);
}

TEST(AudioSyncUtilsTest, StartupPacketTimelineRebaseOffsetKeepsLaterPacketsContiguous) {
    EXPECT_EQ(ce::audio::ApplyStartupPacketTimelineRebaseOffset(10747, 0), 10747);
    EXPECT_EQ(ce::audio::ApplyStartupPacketTimelineRebaseOffset(10747, 10267), 480);
    EXPECT_EQ(ce::audio::ApplyStartupPacketTimelineRebaseOffset(11227, 10267), 960);
    EXPECT_EQ(ce::audio::ApplyStartupPacketTimelineRebaseOffset(400, 10267), 0);
}

TEST(AudioSyncUtilsTest, WgcStartupSmoothnessPreservesPendingAudioPackets) {
    EXPECT_TRUE(ce::audio::ShouldPreservePendingAudioPacketsForStartupSync(true, 1200));
    EXPECT_FALSE(ce::audio::ShouldPreservePendingAudioPacketsForStartupSync(true, 0));
    EXPECT_FALSE(ce::audio::ShouldPreservePendingAudioPacketsForStartupSync(false, 1200));
}

TEST(AudioSyncUtilsTest, CoverageLossSuppressedWhenEncoderBottleneckedAndWgcDeliveringFrames) {
    // Encoder bottlenecked but WGC delivers at target rate → NOT coverage loss
    EXPECT_FALSE(ce::audio::HasWgcUnrecoverableCoverageLoss(120, 500, 0, true, 120));
    EXPECT_FALSE(ce::audio::HasWgcUnrecoverableCoverageLoss(120, 500, 0, true, 119));
    // Within +2 tolerance
    EXPECT_FALSE(ce::audio::HasWgcUnrecoverableCoverageLoss(120, 500, 0, true, 118));
}

TEST(AudioSyncUtilsTest, CoverageLossNotSuppressedWhenEncoderBottleneckedButWgcUnderdelivering) {
    // Encoder bottlenecked AND WGC delivers well below target → real coverage loss
    EXPECT_TRUE(ce::audio::HasWgcUnrecoverableCoverageLoss(120, 500, 0, true, 100));
    EXPECT_TRUE(ce::audio::HasWgcUnrecoverableCoverageLoss(120, 500, 0, true, 0));
}

TEST(AudioSyncUtilsTest, CoverageLossNotSuppressedWhenNotBottlenecked) {
    // Not bottlenecked, high lag → coverage loss detected as before
    EXPECT_TRUE(ce::audio::HasWgcUnrecoverableCoverageLoss(120, 500, 0, false, 120));
    EXPECT_TRUE(ce::audio::HasWgcUnrecoverableCoverageLoss(120, 500, 0, false, 0));
}

TEST(AudioSyncUtilsTest, CoverageLossDefaultParamsBackwardCompatible) {
    // Default params (encoderBottlenecked=false, wgcDeliveredFps=0) behave as before
    EXPECT_TRUE(ce::audio::HasWgcUnrecoverableCoverageLoss(120, 16034, 12));
    EXPECT_TRUE(ce::audio::HasWgcUnrecoverableCoverageLoss(120, 76837, 0));
    EXPECT_FALSE(ce::audio::HasWgcUnrecoverableCoverageLoss(0, 16034, 12));
    EXPECT_FALSE(ce::audio::HasWgcUnrecoverableCoverageLoss(120, 200, 12));
    EXPECT_FALSE(ce::audio::HasWgcUnrecoverableCoverageLoss(120, 300, 220));
}

TEST(AudioSyncUtilsTest, SoftKneeLimiterIsBitTransparentBelowKnee) {
    // Samples at or below the knee (0.9) must pass through unchanged so lossless
    // in-range audio is never thinned/compressed.
    float samples[] = {0.0f, 0.1f, -0.1f, 0.5f, -0.5f, 0.8999f, -0.8999f, 0.9f, -0.9f};
    float expected[] = {0.0f, 0.1f, -0.1f, 0.5f, -0.5f, 0.8999f, -0.8999f, 0.9f, -0.9f};
    constexpr size_t n = sizeof(samples) / sizeof(samples[0]);
    ce::audio::ApplySoftKneeLimiter(samples, n);
    for (size_t i = 0; i < n; ++i) {
        EXPECT_FLOAT_EQ(samples[i], expected[i]) << "index " << i;
    }
}

TEST(AudioSyncUtilsTest, SoftKneeLimiterBoundsPeaksBelowFullScaleWithoutCollapsing) {
    // Above the knee, output must stay strictly inside (-1, 1) (no hard clip) yet
    // remain above the knee (the old tanh squashed a clean 0.9 down to ~0.66).
    float samples[] = {0.95f, 1.0f, 3.0f, -0.95f, -1.0f, -3.0f};
    constexpr size_t n = sizeof(samples) / sizeof(samples[0]);
    ce::audio::ApplySoftKneeLimiter(samples, n);
    for (size_t i = 0; i < n; ++i) {
        EXPECT_LT(std::fabs(samples[i]), 1.0f) << "index " << i;
        EXPECT_GT(std::fabs(samples[i]), 0.9f) << "index " << i;
    }
}

TEST(AudioSyncUtilsTest, SoftKneeLimiterIsMonotonicAboveKnee) {
    float arr[3] = {0.95f, 1.5f, 5.0f};
    ce::audio::ApplySoftKneeLimiter(arr, 3);
    EXPECT_LT(arr[0], arr[1]);
    EXPECT_LT(arr[1], arr[2]);
    EXPECT_LT(arr[2], 1.0f);
}

TEST(AudioSyncUtilsTest, SoftKneeLimiterHandlesEmptyAndNull) {
    ce::audio::ApplySoftKneeLimiter(nullptr, 0);
    ce::audio::ApplySoftKneeLimiter(nullptr, 10);
    float dummy = 0.5f;
    ce::audio::ApplySoftKneeLimiter(&dummy, 0);
    EXPECT_FLOAT_EQ(dummy, 0.5f);
}

TEST(AudioSyncUtilsTest, AppAudioTrackIdentityIsCaseInsensitivePerProcessAndTrack) {
    // Same process (any case) + same track => identical key (would be deduped).
    EXPECT_EQ(ce::audio::AppAudioTrackIdentity("Game.exe", 0, 1), ce::audio::AppAudioTrackIdentity("game.EXE", 0, 1));
    // Different track => different key (legitimately fans out to multiple tracks).
    EXPECT_NE(ce::audio::AppAudioTrackIdentity("game.exe", 0, 1), ce::audio::AppAudioTrackIdentity("game.exe", 0, 2));
    // Different process => different key.
    EXPECT_NE(ce::audio::AppAudioTrackIdentity("a.exe", 0, 1), ce::audio::AppAudioTrackIdentity("b.exe", 0, 1));
    // Falls back to PID when no name is set.
    EXPECT_EQ(ce::audio::AppAudioTrackIdentity("", 1234, 1), ce::audio::AppAudioTrackIdentity("", 1234, 1));
    EXPECT_NE(ce::audio::AppAudioTrackIdentity("", 1234, 1), ce::audio::AppAudioTrackIdentity("", 5678, 1));
}

TEST(AudioSyncUtilsTest, SuppressBufferDeferOnlyWhenCatastrophicallyBehind) {
    const int64_t rate = 48000;
    const int64_t maxGap = rate * 2;  // 2s catch-up threshold
    // Healthy/normal pull (small chunk) -> keep the buffer-wait defer protection.
    EXPECT_FALSE(ce::audio::ShouldSuppressBufferDeferForCatchup(240, maxGap, false));
    EXPECT_FALSE(ce::audio::ShouldSuppressBufferDeferForCatchup(rate, maxGap, false));    // 1s behind
    EXPECT_FALSE(ce::audio::ShouldSuppressBufferDeferForCatchup(maxGap, maxGap, false));  // exactly 2s: not yet
    // More than 2s behind (a real read-stall left the track behind) -> suppress defer, force progress.
    EXPECT_TRUE(ce::audio::ShouldSuppressBufferDeferForCatchup(maxGap + 1, maxGap, false));
    EXPECT_TRUE(ce::audio::ShouldSuppressBufferDeferForCatchup(rate * 100, maxGap, false));  // 100s behind
}

TEST(AudioSyncUtilsTest, SuppressBufferDeferNeverFiresDuringInitialStartupCatchup) {
    const int64_t rate = 48000;
    const int64_t maxGap = rate * 2;
    // The legitimate startup catch-up has its own path and must not be treated as a stall,
    // even when its first chunk is large.
    EXPECT_FALSE(ce::audio::ShouldSuppressBufferDeferForCatchup(rate * 100, maxGap, true));
}

// End-to-end regression guard for the multi-app track FREEZE (logs/20260625_022047): a track that
// fell >2s behind after a read-stall entered the warp branch (pull chunk clamped to MAX_SILENCE_CHUNK
// = 0.5s); the buffer-wait defer then required EVERY co-mixed source to have >=0.5s buffered, but a
// second app sitting at the live edge (~100ms) could never reach that, so the whole track deferred
// every iteration and froze into permanent silence. This composes the two production decisions exactly
// as PullAndEncodeAudio does (`!trackLargeBacklogDrain && ShouldDeferCfrAudioPullForSourceBuffer(...)`)
// and pins the fix: it FAILS on the pre-fix logic (would defer -> freeze) and passes post-fix.
TEST(AudioSyncUtilsTest, MultiAppCatchupDoesNotFreezeOnUnderBufferedLiveSource) {
    const int64_t rate = 48000;
    const int64_t maxGap = rate * 2;                // 2s warp threshold (MAX_GAP_SAMPLES)
    const int64_t warpChunk = rate / 2;             // 0.5s clamped pull chunk (MAX_SILENCE_CHUNK)
    const int64_t liveSourceBuffered = rate / 10;   // a co-mixed app keeping up at the live edge (~100ms)
    const int64_t trackBehindSamples = rate * 100;  // track 100s behind after a sustained stall

    // Pre-fix root cause: the buffer-wait defer ALONE would freeze the track, because the live source
    // has less than the (warp-inflated) requested 0.5s chunk and can never accrue it.
    EXPECT_TRUE(ce::audio::ShouldDeferCfrAudioPullForSourceBuffer(
        /*isCfr*/ true, /*forceDrain*/ false, /*optionalUnstarted*/ false,
        /*sparseStartedSourceMaySilence*/ false, /*requestedSamples*/ warpChunk,
        /*bufferedTimelineSamples*/ static_cast<size_t>(liveSourceBuffered)));

    // Fix: a track >2s behind suppresses the defer, so the composed decision makes forward progress.
    const bool suppress = ce::audio::ShouldSuppressBufferDeferForCatchup(trackBehindSamples, maxGap, false);
    EXPECT_TRUE(suppress);
    const bool wouldDeferAndFreeze =
        !suppress && ce::audio::ShouldDeferCfrAudioPullForSourceBuffer(true, false, false, false, warpChunk,
                                                                       static_cast<size_t>(liveSourceBuffered));
    EXPECT_FALSE(wouldDeferAndFreeze);  // the track must catch up, never freeze into permanent silence

    // And the normal (not-behind) path still keeps the buffer-wait protection: a healthy track that is
    // only a small quantum behind does NOT suppress, so brief startup/jitter buffering still applies.
    EXPECT_FALSE(ce::audio::ShouldSuppressBufferDeferForCatchup(/*samplesToEncode*/ 240, maxGap, false));
}

TEST(AudioSyncUtilsTest, EffectiveDeliveredFpsTakesWorstOfMeasuredWindows) {
    // Instantaneous rate is healthy but a windowed minimum dipped -> worst wins.
    EXPECT_EQ(ce::audio::ComputeEffectiveDeliveredFpsForAudioContinuity(120, 90, 100), 90u);
    EXPECT_EQ(ce::audio::ComputeEffectiveDeliveredFpsForAudioContinuity(120, 100, 80), 80u);
}

TEST(AudioSyncUtilsTest, EffectiveDeliveredFpsIgnoresUnmeasuredZeroWindows) {
    // A window reporting 0 (not measured yet) must not drag the effective rate to 0.
    EXPECT_EQ(ce::audio::ComputeEffectiveDeliveredFpsForAudioContinuity(120, 0, 0), 120u);
    EXPECT_EQ(ce::audio::ComputeEffectiveDeliveredFpsForAudioContinuity(120, 0, 95), 95u);
    EXPECT_EQ(ce::audio::ComputeEffectiveDeliveredFpsForAudioContinuity(120, 95, 0), 95u);
}

TEST(AudioSyncUtilsTest, WgcSelectedContentLeadLagSplitsSignedBias) {
    // Positive bias -> content leads the timeline; negative -> it lags.
    EXPECT_EQ(ce::audio::ComputeWgcSelectedContentLeadMs(8000), 8);
    EXPECT_EQ(ce::audio::ComputeWgcSelectedContentLagMs(8000), 0);
    EXPECT_EQ(ce::audio::ComputeWgcSelectedContentLeadMs(-12000), 0);
    EXPECT_EQ(ce::audio::ComputeWgcSelectedContentLagMs(-12000), 12);
    EXPECT_EQ(ce::audio::ComputeWgcSelectedContentLeadMs(0), 0);
    EXPECT_EQ(ce::audio::ComputeWgcSelectedContentLagMs(0), 0);
}

TEST(AudioSyncUtilsTest, WgcVisualContentLagClampsToRange) {
    // shortfall + lag - lead, clamped to [0, max].
    EXPECT_EQ(ce::audio::ComputeWgcVisualContentLagMs(50, 0, 30, 4000), 80);
    EXPECT_EQ(ce::audio::ComputeWgcVisualContentLagMs(50, 70, 0, 4000), 0);  // lead exceeds shortfall -> floored at 0
    EXPECT_EQ(ce::audio::ComputeWgcVisualContentLagMs(10000, 0, 0, 4000), 4000);  // capped at max
}

TEST(AudioSyncUtilsTest, IngestReservoirStaysZeroWhileHeadroomIsHealthy) {
    ce::audio::AudioIngestReservoirState state;
    // Typical healthy CFR run: 60 ms reservoir minus ~12 ms delivery latency -> ~48 ms headroom.
    for (int i = 0; i < 50; ++i) {
        const auto decision = ce::audio::ComputeAudioIngestReservoir(state, /*active*/ true, /*observed*/ true,
                                                                     /*headroomMs*/ 48, /*elapsedMs*/ 40);
        EXPECT_EQ(decision.extraMs, 0);
        EXPECT_FALSE(decision.raised);
        state.extraMs = decision.extraMs;
        state.healthyElapsedMs = decision.healthyElapsedMs;
    }
}

TEST(AudioSyncUtilsTest, IngestReservoirDeepensByTheObservedDeficitPlusMargin) {
    ce::audio::AudioIngestReservoirState state;
    // The consumer overran the capture edge by 30 ms (headroom went negative).
    const auto decision = ce::audio::ComputeAudioIngestReservoir(state, true, true, /*headroomMs*/ -30,
                                                                 /*elapsedMs*/ 8);
    EXPECT_TRUE(decision.raised);
    // Needed = minHeadroom(25) - (-30) = 55, plus the 20 ms margin.
    EXPECT_EQ(decision.extraMs, 75);
    EXPECT_EQ(decision.healthyElapsedMs, 0);
    EXPECT_FALSE(decision.atCap);
}

TEST(AudioSyncUtilsTest, IngestReservoirIsBoundedByTheCap) {
    ce::audio::AudioIngestReservoirState state;
    state.extraMs = ce::audio::kAudioIngestMaxExtraReservoirMs - 10;
    const auto decision = ce::audio::ComputeAudioIngestReservoir(state, true, true, /*headroomMs*/ -5000, 8);
    EXPECT_EQ(decision.extraMs, ce::audio::kAudioIngestMaxExtraReservoirMs);
    EXPECT_TRUE(decision.atCap);
    EXPECT_EQ(ce::audio::RaiseAudioIngestReservoirForShortfall(ce::audio::kAudioIngestMaxExtraReservoirMs, 10000),
              ce::audio::kAudioIngestMaxExtraReservoirMs);
}

TEST(AudioSyncUtilsTest, IngestReservoirDecaysOnlyWithComfortableHeadroomAndNeverOnMissingEvidence) {
    ce::audio::AudioIngestReservoirState state;
    state.extraMs = 100;

    // No placement evidence this window (every source genuinely idle) must hold, not decay.
    auto decision = ce::audio::ComputeAudioIngestReservoir(state, true, /*observed*/ false, 0, 5000);
    EXPECT_EQ(decision.extraMs, 100);
    EXPECT_FALSE(decision.decayed);

    // Headroom just above the minimum is not comfortable enough to give lookahead back.
    decision = ce::audio::ComputeAudioIngestReservoir(state, true, true, /*headroomMs*/ 30, 5000);
    EXPECT_EQ(decision.extraMs, 100);
    EXPECT_EQ(decision.healthyElapsedMs, 0);

    // Comfortable headroom decays one step per interval.
    state.healthyElapsedMs = 0;
    decision = ce::audio::ComputeAudioIngestReservoir(state, true, true, /*headroomMs*/ 120,
                                                      ce::audio::kAudioIngestReservoirDecayIntervalMs);
    EXPECT_TRUE(decision.decayed);
    EXPECT_EQ(decision.extraMs, 100 - ce::audio::kAudioIngestReservoirDecayStepMs);
}

TEST(AudioSyncUtilsTest, IngestReservoirIsInactiveForVfrAndAudioOnly) {
    ce::audio::AudioIngestReservoirState state;
    state.extraMs = 250;
    const auto decision = ce::audio::ComputeAudioIngestReservoir(state, /*active*/ false, true, -500, 40);
    EXPECT_EQ(decision.extraMs, 0);
    EXPECT_EQ(decision.healthyElapsedMs, 0);
}

// Regression guard for the `logs/audiodeath` failure: a single ~67 ms consumer overrun under
// deliberate encoder overload silenced EVERY audio track for the remaining 3.5 minutes. The
// consumer exported silence for the missing range, the per-source write cursor was pinned
// forward, and every later packet was destroyed as timeline overlap - cursor and capture edge
// both advance at wall rate, so the deficit could never repay itself. The fix holds the pull
// for a late-but-live source instead of exporting silence, which is what lets the producer
// overtake the cursor again.
TEST(AudioSyncUtilsTest, LateLiveSourceHoldsThePullInsteadOfExportingDestructiveSilence) {
    const int64_t rate = 48000;
    const int64_t requested = rate / 120;                       // one 8.33 ms CFR quantum
    const size_t buffered = 0;                                  // ring drained by the overrun

    // Pre-fix behaviour: a started source with an empty ring is treated as silence, which pins
    // the cursor ahead of the capture edge and destroys everything that arrives later.
    EXPECT_TRUE(ce::audio::ShouldTreatStartedTimelineSourceShortfallAsSilence(
        /*sparseStartedSourceMaySilence*/ true, buffered, requested,
        ce::audio::kDefaultAudioPullQuantumSamples * 4));

    // Fix: the source is still delivering real packets, so the pull holds instead.
    EXPECT_TRUE(ce::audio::ShouldHoldCfrAudioPullForLateLiveSource(
        /*isCfr*/ true, /*forceDrain*/ false, /*timelineValid*/ true, /*bootstrapComplete*/ true,
        /*captureActive*/ true, /*recentRealPackets*/ true, requested, buffered, /*reservoirExtraMs*/ 0));

    // A genuinely idle source (no recent packets) must still contribute silence so it cannot
    // freeze a co-mixed track.
    EXPECT_FALSE(ce::audio::ShouldHoldCfrAudioPullForLateLiveSource(true, false, true, true, true,
                                                                    /*recentRealPackets*/ false, requested, buffered,
                                                                    0));
    // So must a source whose capture route already ended.
    EXPECT_FALSE(ce::audio::ShouldHoldCfrAudioPullForLateLiveSource(true, false, true, true,
                                                                    /*captureActive*/ false, true, requested, buffered,
                                                                    0));
    // The hold is bounded: once the reservoir is at its cap the track makes progress again.
    EXPECT_FALSE(ce::audio::ShouldHoldCfrAudioPullForLateLiveSource(
        true, false, true, true, true, true, requested, buffered,
        /*reservoirExtraMs*/ ce::audio::kAudioIngestMaxExtraReservoirMs));
    // Stop drain and VFR are never held.
    EXPECT_FALSE(ce::audio::ShouldHoldCfrAudioPullForLateLiveSource(true, /*forceDrain*/ true, true, true, true, true,
                                                                    requested, buffered, 0));
    EXPECT_FALSE(ce::audio::ShouldHoldCfrAudioPullForLateLiveSource(/*isCfr*/ false, false, true, true, true, true,
                                                                    requested, buffered, 0));
    // A source with enough buffered content is never held.
    EXPECT_FALSE(ce::audio::ShouldHoldCfrAudioPullForLateLiveSource(true, false, true, true, true, true, requested,
                                                                    static_cast<size_t>(requested), 0));
}

// The reservoir alone recovers the recorded failure: one 67 ms overrun deepens the lookahead
// past the observed deficit, so the pull target freezes long enough for the producer to pass
// the cursor. Sample positions never move, so this is sync-neutral by construction.
TEST(AudioSyncUtilsTest, IngestReservoirRecoversTheAudiodeathOverrunWithinOneEvaluation) {
    ce::audio::AudioIngestReservoirState state;
    const int64_t observedOverrunMs = -67;  // measured headroom right after the overrun
    const auto decision = ce::audio::ComputeAudioIngestReservoir(state, true, true, observedOverrunMs, 8);
    EXPECT_TRUE(decision.raised);
    // The new reservoir must exceed the deficit, otherwise the cursor stays ahead forever.
    EXPECT_GT(decision.extraMs, -observedOverrunMs);
    EXPECT_LE(decision.extraMs, ce::audio::kAudioIngestMaxExtraReservoirMs);
    // Total lookahead stays a bounded scheduling reservoir, not a content offset.
    EXPECT_LE(ce::audio::kDefaultSteadyAudioPullLatencyMs + decision.extraMs,
              ce::audio::kDefaultSteadyAudioPullLatencyMs + ce::audio::kAudioIngestMaxExtraReservoirMs);
}

TEST(AudioSyncUtilsTest, StarvedSourceResyncIsLastResortOnly) {
    const int64_t deficit = 4800;  // 100 ms
    // Not while the reservoir still has room to grow.
    EXPECT_FALSE(
        ce::audio::ShouldResyncStarvedLiveAudioSource(true, false, /*reservoirAtCap*/ false, true, 5000, deficit));
    // Not before the starvation has actually persisted.
    EXPECT_FALSE(ce::audio::ShouldResyncStarvedLiveAudioSource(true, false, true, true, /*starvedMs*/ 200, deficit));
    // Not for an idle source, during stop drain, or in VFR.
    EXPECT_FALSE(
        ce::audio::ShouldResyncStarvedLiveAudioSource(true, false, true, /*recentPackets*/ false, 5000, deficit));
    EXPECT_FALSE(ce::audio::ShouldResyncStarvedLiveAudioSource(true, /*forceDrain*/ true, true, true, 5000, deficit));
    EXPECT_FALSE(ce::audio::ShouldResyncStarvedLiveAudioSource(/*isCfr*/ false, false, true, true, 5000, deficit));
    // Only when the reservoir is exhausted and a live source is still fully starved.
    EXPECT_TRUE(ce::audio::ShouldResyncStarvedLiveAudioSource(true, false, true, true, 5000, deficit));
}

