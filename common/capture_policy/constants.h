#pragma once

#include <stddef.h>
#include <stdint.h>
#include <algorithm>

// Tunables shared by every capture backend.

namespace ce::capture_policy {

constexpr uint32_t kRecordingWarmupMinMs = 120;
constexpr uint32_t kRecordingWarmupMaxMs = 350;
constexpr size_t kInjectWarmupCommitFloorFrames = 3;
constexpr size_t kMaxInjectBufferedHeadroomFrames = 12;
constexpr size_t kStartupInjectBufferedHeadroomFrames = 48;
// Leave room in the shared 32-slot inject ring for the producer, ingest handoff,
// and the frame currently owned by the encoder when timestamp-phase retention
// expands the live jitter buffer.
constexpr size_t kInjectFrameRingSafetySlots = 4;
constexpr uint32_t kMaxInjectDeferredFrameRetries = 3;
constexpr uint32_t kInjectCfrPublicationHeadroomPermille = 4000;
// A final presented-output stream already has an ordered display cadence. Two
// candidates per CFR tick retain nearest-frame coverage without copying every
// generated display frame on high-refresh multi-frame-generation paths.
constexpr uint32_t kFinalOutputCfrPublicationHeadroomPermille = 2000;
constexpr int64_t kInjectCfrPublicationEarlySlackMinUs = 250;
constexpr int64_t kInjectCfrPublicationEarlySlackMaxUs = 1500;
constexpr uint32_t kInjectCfrSelectionLeadTolerancePermille = 500;
constexpr uint32_t kCfrPhaseLockCadenceTolerancePermille = 180;
constexpr uint32_t kCfrPhaseLockPhaseTolerancePermille = 200;
constexpr uint32_t kCfrPhaseLockRephaseTolerancePermille = 100;
constexpr uint32_t kCfrPhaseLockMinStableIntervals = 12;
constexpr uint32_t kCfrPhaseLockConfirmations = 8;
constexpr uint32_t kCfrPhaseLockReleaseIntervals = 4;
constexpr uint32_t kCfrPhaseLockRephaseConfirmations = 8;
constexpr uint32_t kCfrPhaseLockIncoherentReleaseIntervals = 3;
constexpr uint32_t kInjectLiveHealthyMaxFrameAgeTicks = 3;
constexpr uint32_t kInjectLivePressureMaxFrameAgeTicks = 12;
constexpr uint64_t kEncoderStartupWindowMs = 1500;
constexpr double kEncoderCapacityWarningRatio = 0.85;
constexpr uint32_t kAutoWgcFallbackDelayNoPidMs = 100;
constexpr uint32_t kAutoWgcFallbackDelayWithPidMs = 200;
constexpr uint32_t kWgcLowSourceEmptyTickPermille = 80;
constexpr uint32_t kWgcLowSourceExitEmptyTickPermille = 40;
constexpr uint32_t kWgcLowSourceEnterMarginFps = 2;
constexpr uint32_t kWgcLowSourceEnterHoldMs = 120;
constexpr uint32_t kWgcLowSourceExitHoldMs = 250;
constexpr uint32_t kWgcMaxSelectionLagTicks = 1;
constexpr uint32_t kWgcLowSourceMaxSelectionLagTicks = 3;
constexpr uint32_t kWgcWarmupBufferedFrames = 3;
constexpr uint32_t kWgcWarmupStableSourceFps = 118;
constexpr size_t kWgcWarmupFreshFrames = 2;
constexpr uint32_t kWgcReservePressurePermille = 600;
constexpr uint32_t kWgcReserveBiasPermille = 250;
constexpr uint32_t kWgcReserveFragileBiasPermille = 375;
constexpr uint32_t kWgcDeepUnderfeedReserveBiasPermille = 750;
constexpr uint32_t kWgcSingleFreshHoldInputPermille = 995;
constexpr uint32_t kWgcSteadyReserveBuildInputPermille = 995;
constexpr uint32_t kWgcSelectionDelayTicks = 1;
constexpr uint32_t kWgcCoverageDelayMaxTicks = 32;
constexpr uint32_t kWgcCfrSelectionMaxLeadTicks = 3;
constexpr uint32_t kWgcActiveDelayResidualTolerancePermille = 900;
constexpr uint32_t kWgcActiveDelayResidualHardLimitUs = 10000;
constexpr uint32_t kWgcActiveDelayResidualMeanTargetUs = 5000;
constexpr uint32_t kWgcActiveDelayResidualP95TargetUs = 10000;
constexpr uint32_t kWgcActiveDelaySoftLateTargetMinUs = 5000;
constexpr uint32_t kWgcActiveDelaySoftLateTargetMaxUs = 7000;
constexpr uint32_t kWgcActiveDelayRepeatClusterPenaltyPermille = 250;
constexpr uint32_t kWgcActiveDelayRepeatClusterPenaltyMaxPermille = 1000;
constexpr uint32_t kWgcActiveDelayPolicyHoldFaultMinCount = 120;
constexpr uint32_t kWgcActiveDelayPolicyHoldFaultPermille = 250;
constexpr uint32_t kWgcActiveDelaySourceRecoveryHoldMs = 1500;
constexpr uint32_t kWgcActiveDelayRecoverableJitterPermille = 1000;
constexpr uint32_t kWgcActiveDelaySourceLimitedJitterPermille = 2000;
constexpr uint32_t kWgcCfrSmoothnessExcessRepeatFaultMinCount = 120;
constexpr uint32_t kWgcCfrSmoothnessExcessRepeatFaultPermille = 50;
constexpr uint32_t kWgcCfrSmoothnessPolicyRepeatNoticeMinCount = 24;
constexpr uint32_t kWgcCfrSmoothnessPolicyRepeatNoticePermille = 5;
constexpr uint32_t kWgcCfrSmoothnessExcessRepeatClusterFaultTicks = 24;
constexpr uint32_t kWgcDelayReservoirTargetExtraFrames = 1;
constexpr uint32_t kWgcSmoothnessBufferDefaultMaxMs = 300;
constexpr uint32_t kWgcSmoothnessBufferDefaultVramBudgetMb = 3000;
constexpr uint32_t kWgcSmoothnessBufferPoolSafetyFrames = 4;
constexpr uint32_t kWgcSmoothnessBufferPoolInFlightFrames = 1;
constexpr uint32_t kWgcSmoothnessBufferPoolSelectedSlackFrames = 1;
constexpr uint32_t kWgcSmoothnessSourceFramePoolDefaultBuffers = 8;
constexpr uint32_t kWgcSmoothnessSourceFramePoolMinBuffers = 3;
constexpr uint32_t kWgcSmoothnessSourceFramePoolCompactHighFpsMaxBuffers = 12;
constexpr uint32_t kWgcSmoothnessEstimatedSyncDelayMs = 33;
constexpr uint32_t kWgcSmoothnessBufferMinPoolFrames = 8;
constexpr uint32_t kWgcSmoothnessBufferMaxPoolFrames = 64;
constexpr uint32_t kWgcSmoothnessBufferPoolHeadroomSlots = 8;
// The retained WGC reservoir stores source frames, not output CFR slots. Size the
// millisecond smoothness budget for the normal source-above-output case
// (e.g. 140/144 Hz VRR into 120 fps CFR), otherwise a "300 ms" reservoir is only
// about 250 ms of 144 Hz source history.
constexpr uint32_t kWgcSmoothnessBufferSourceRatePermille = 1250;
// WGC smoothness FLOOR: a baseline jitter-buffer delay that engages the active-delay
// smoothness machinery (nearest-target playout + reservoir) even when there is NO
// audio-latency content delay -- i.e. video-only capture, or a low-confidence loopback
// latency probe, where WGC otherwise runs near-live and is maximally exposed to bursty
// DWM->frame-pool delivery under GPU load. The floor is auto-derived from measured startup
// WGC delivery jitter, then HELD FIXED for the session (never retimed live), so it cannot
// reintroduce the realized-delay-collapse / "ghost image" judder family. It is applied
// symmetrically (older startup video + correspondingly later live-start), so it is
// sync-neutral by construction and NEVER moves the audio anchor (audio stays byte-exact).
// kWgcSmoothnessFloorMinFrames is the minimum depth (frames) the floor engages with so the
// nearest-target playout has room to absorb at least one delivery hiccup.
constexpr uint32_t kWgcSmoothnessFloorMinFrames = 2;
// Jitter headroom (frames) above the active-delay reservoir target before the uniform-cadence pacer
// trims the oldest surplus. Bounds the realized content delay so a VRR / GPU-bound source whose
// present rate transiently rises above the CFR output rate cannot inflate the reservoir without
// bound. Kept small so the realized delay stays close to the target while still absorbing ~1 frame
// of VRR jitter without trimming on every wobble.
constexpr size_t kWgcActiveDelayPaceMaxExcessFrames = 1;
constexpr uint32_t kWgcMaxLiveVisualDebtMs = 250;
constexpr uint32_t kWgcMaxLiveVisualDebtFrames = 32;
constexpr uint32_t kWgcMaxLiveSchedulerRebaseTicksPerLoop = 1;
constexpr uint32_t kWgcEncoderLimitedLiveVisualDebtMs = 50;
constexpr uint32_t kWgcEncoderLimitedLiveVisualDebtFrames = 3;
constexpr uint32_t kWgcEncoderLimitedSourceBufferFloorFrames = 4;
constexpr uint32_t kWgcEncoderLimitedLiveSchedulerRebaseTicksPerLoop = 4;
constexpr uint32_t kCfrShortfallCatchupThresholdTicks = 2;
constexpr uint32_t kCfrShortfallForceCatchupThresholdTicks = 18;
// Four submissions at this cost fit inside three CFR intervals, so a maximum
// four-tick recovery burst repays one slot instead of increasing wall-clock debt.
constexpr double kWgcFreshCatchupServiceBudgetRatio = 0.75;
// When fresh screen-grab submissions cannot sustain CFR but cached repeats are
// cheaper, reserve normal service headroom. If repeats approach the interval,
// spend only part of their remaining real-time headroom on fresh-frame liveness.
constexpr double kWgcOverloadRepeatPacerBudgetRatio = 0.95;
constexpr double kWgcOverloadRepeatPacerMinAdvantageRatio = 0.05;
constexpr double kWgcOverloadRepeatPacerMarginalHeadroomUseRatio = 0.75;
constexpr double kWgcOverloadRepeatPacerDegradedFreshFraction = 0.05;
constexpr uint32_t kWgcOverloadRepeatPacerMinSamples = 8;
constexpr uint32_t kWgcOverloadRepeatPacerRecoveryConfirmTicks = 8;
constexpr uint32_t kCfrOverloadRepeatProbeIntervalTicks = 4;
constexpr double kCfrRepeatCatchupServiceBudgetRatio = 0.90;
constexpr double kCfrOverloadPacerSourceMinFramesPerTick = 0.90;
constexpr double kCfrOverloadPacerSourceMarginFps = 4.0;
constexpr double kCfrDynamicOverlayRepeatEnterBudgetRatio = 0.95;
constexpr double kCfrDynamicOverlayRepeatExitBudgetRatio = 0.75;
constexpr uint32_t kCfrDynamicOverlayRepeatEnterConfirmFrames = 2;
constexpr uint32_t kCfrDynamicOverlayRepeatExitConfirmFrames = 8;
constexpr uint32_t kInjectCfrRecoveryExitShortfallTicks = 1;
constexpr double kWgcSevereShortfallDurationMs = 500.0;
constexpr uint32_t kWgcDeepUnderfeedMarginFps = 8;
constexpr uint32_t kWgcDeepUnderfeedEmptyTickPermille = 350;
constexpr uint32_t kWgcDeepUnderfeedStaleFallbackLagTicks = 5;
constexpr uint32_t kWgcRecoverySourceMarginFps = 4;
constexpr uint32_t kWgcRecoveryEmptyTickPermille = 200;
constexpr uint32_t kWgcRecoveryEnterShortfallTicks = 3;
constexpr uint32_t kWgcRecoveryEnterHoldMs = 120;
constexpr uint32_t kWgcRecoveryExitHoldMs = 450;
constexpr double kWgcAudioLeadCatchupThresholdMs = 40.0;
// Long enough to distinguish a sustained below-output source from a transient
// delivery wobble when classifying recovery state. This is telemetry only: CFR
// WGC capture must never feed the resulting post-sampler rate back into
// MinUpdateInterval.
constexpr uint32_t kWgcStableUnderfeedClassificationMs = 3000;
constexpr double kEncoderGpuPriorityRaiseBudgetRatio = 0.75;
constexpr double kEncoderGpuPriorityRestoreBudgetRatio = 0.50;
constexpr uint32_t kEncoderOverloadFlagEncoder = 1u;
constexpr uint32_t kEncoderOverloadFlagMux = 2u;
constexpr uint32_t kWgcCaptureHealthFlagSourceStarved = 1u;
constexpr uint32_t kWgcCaptureHealthFlagSchedulerLimited = 2u;
constexpr uint32_t kOverlayWarningNone = 0u;
constexpr uint32_t kOverlayWarningEncoderOverload = 1u;
constexpr uint32_t kOverlayWarningRecordingRecovering = 2u;
constexpr uint32_t kOverlayWarningRecordingDegraded = 3u;

// Recording-health flags are telemetry and user feedback only. They must never
// alter CFR scheduling, timestamps, audio, encoder settings, or mux policy.
constexpr uint32_t kRecordingHealthFlagEncoderPressureObserved = 1u << 0;
constexpr uint32_t kRecordingHealthFlagMuxPressureObserved = 1u << 1;
constexpr uint32_t kRecordingHealthFlagTimelineDebt = 1u << 2;
constexpr uint32_t kRecordingHealthFlagRecovering = 1u << 3;
constexpr uint32_t kRecordingHealthFlagVideoDegraded = 1u << 4;
constexpr uint32_t kRecordingHealthFlagSevere = 1u << 5;
constexpr uint32_t kRecordingHealthCauseMask =
    kRecordingHealthFlagEncoderPressureObserved | kRecordingHealthFlagMuxPressureObserved;
constexpr uint32_t kRecordingHealthLatchedMask =
    kRecordingHealthCauseMask | kRecordingHealthFlagVideoDegraded | kRecordingHealthFlagSevere;
constexpr uint32_t kRecordingHealthCausalDebtMs = 250;
constexpr uint32_t kRecordingHealthDegradedDebtMs = 500;
constexpr uint32_t kRecordingHealthSevereDebtMs = 2000;
constexpr uint32_t kRecordingHealthPressureConfirmationSamples = 2;

}  // namespace ce::capture_policy
