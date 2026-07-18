#!/usr/bin/env python3

import argparse
import array
import collections
import csv
import datetime
import json
import math
import os
import re
import statistics
import subprocess
import sys
import tempfile
from fractions import Fraction
from pathlib import Path
from typing import NoReturn


LOG_PATTERNS = {
    "audio_start_reset_issued": re.compile(r"\[A/V START\] Shared startup reset issued"),
    "audio_start_reset_committed": re.compile(r"\[A/V START\] Shared startup reset committed"),
    "audio_capture_epoch_start": re.compile(r"\[AudioRoute\] Ordered epoch start reached"),
    "audio_capture_epoch_transition": re.compile(r"\[AudioRoute\] Capture epoch transition"),
    "audio_codec_contract": re.compile(r"\[AudioCodecContract\]"),
    "audio_finalization": re.compile(r"\[AudioFinalization\]"),
    "audio_finalization_protocol_error": re.compile(r"\[AudioFinalization\].*protocolError=1"),
    "audio_resource_destroyed": re.compile(r"\[AudioResource\] Destroyed per-recording"),
    "audio_worker_start": re.compile(r"\[AppAudioWorker\] Started generation="),
    "audio_worker_exit": re.compile(r"\[AppAudioWorker\] Exit generation="),
    "audio_worker_overrun": re.compile(r"\[AppAudioWorker\].*(?:overrun=[1-9]|Overrun=[1-9])"),
    "audio_worker_scheduling_stall": re.compile(r"\[AudioLoop\] Scheduling summary: events=[1-9]\d*"),
    "inject_cfr_recovery_stalled": re.compile(r"\[Inject CFR\] Recovery still active:"),
    "audio_app_process_tree_selected": re.compile(r"\[AppAudioCapture\] Process-name tree resolution"),
    "audio_app_active_no_data": re.compile(
        r"\[AppAudioCapture\] WARNING: process-loopback stream is active but has delivered no data packets"
    ),
    "audio_app_stop_active_no_data": re.compile(r"\[STOP AUDIO\] Source \d+ \(app-active-no-data\)"),
    "audio_app_capture_mode_contract": re.compile(r"\[AppAudioCapture\] Capture mode contract:"),
    "audio_app_first_packet_qualified": re.compile(r"\[AppAudioCapture\] First-packet qualification succeeded:"),
    "audio_app_event_first_packet_fallback": re.compile(
        r"\[AppAudioCapture\] WARNING: event-driven activation failed first-packet qualification"
    ),
    "cfr_finalization_lattice": re.compile(r"\[FinalizationLattice\] CFR endpoint contract"),
    "cfr_finalization_lattice_error": re.compile(r"\[FinalizationLattice\] ERROR"),
    "audio_latency_cap": re.compile(r"\[PullAudio\] Audio latency cap:"),
    "audio_retain_trim": re.compile(r"\[PullAudio\] WARNING: WGC CFR audio headroom exhausted"),
    "audio_retained_trim_summary": re.compile(r"\[PullAudio\] Retained-audio trim summary"),
    "audio_latency_trim_summary": re.compile(r"\[PullAudio\] Latency trim aggregate summary"),
    "audio_coverage_trim": re.compile(r"\[PullAudio\] WGC overload sync trim:"),
    "audio_tier2_trim_summary": re.compile(r"\[PullAudio\] Tier2 drift trim summary"),
    "audio_wgc_lead_cap_trim": re.compile(r"\[PullAudio\] WGC CFR lead cap trim:"),
    "audio_overflow_protection_trim": re.compile(r"\[PullAudio\] Ring buffer overflow protection"),
    "audio_post_resample_trim": re.compile(r"\[PullAudio\] WARNING: Post-resample buffer trim"),
    "audio_cfr_ring_capacity": re.compile(r"\[PullAudio\] WARNING: CFR audio ring near capacity"),
    "audio_cfr_post_resample_backlog": re.compile(r"\[PullAudio\] WARNING: CFR post-resample backlog exceeded guard"),
    "wgc_cfr_lead_warning": re.compile(r"\[PullAudio\] WGC CFR lead warning:"),
    "wgc_coverage_mode_active": re.compile(r"CovMode=1"),
    "audio_large_gap": re.compile(r"\[PullAudio\] Large A/V gap"),
    # CFR warp branch: the track fell >2s behind the live target (the read-stall freeze precursor).
    # In a clean deterministic run a track never falls this far behind, so any occurrence is a fault;
    # a stall scenario expects it as the recovery path and sets an explicit threshold instead.
    "audio_large_cfr_backlog": re.compile(r"\[PullAudio\] Large CFR audio backlog"),
    # Read-stall recovery fired (alt-tab/DPC/encoder freeze drove an app-source backlog past the
    # catastrophic threshold). Expected only after a real multi-second stall; never in a clean run.
    "audio_catastrophic_resync": re.compile(r"\[PullAudio\] App source catastrophic backlog resync"),
    # App-audio latency crossed the live warning threshold. This remains an explicit-gate event for
    # scenario-local validation, but session triage adjudicates it against the final target-relative
    # excess/integrity summary: warnings that stayed within target slack are context, not a fault.
    "audio_app_latency_elevated": re.compile(r"\[AppLatency\] WARNING: app audio"),
    "audio_underrun": re.compile(r"\[PullAudio\] WARNING: Source underrun(?!.*forceDrain=1)"),
    "audio_stop_tail_padding": re.compile(r"\[PullAudio\] WARNING: Source underrun.*forceDrain=1"),
    "audio_source_padding_summary": re.compile(r"\[STOP AUDIO\] Source \d+: .* pad:[1-9]\d*"),
    "audio_overflow": re.compile(r"\[PullAudio\] WARNING: Ring buffer overflow"),
    "audio_silence_fill": re.compile(r"\[PullAudio\] Track \d+ silent - generating"),
    "audio_forced_bootstrap": re.compile(r"\[PullAudio\] Track \d+ bootstrap complete .* forced=1"),
    "audio_bootstrap_trim": re.compile(r"\[PullAudio\] Track \d+ bootstrap complete .* trimmed=[1-9]"),
    "audio_late_source_cursor": re.compile(r"\[AudioLoop\] Late source cursor advance"),
    "audio_late_app_live_join": re.compile(r"\[AudioLoop\] Late app source live join", re.IGNORECASE),
    "audio_late_app_source_backlog": re.compile(
        r"\[PullAudio\] Source primed .*lateStart=(?:[1-9]\d{3,}|\d{5,})ms", re.IGNORECASE
    ),
    "audio_app_source_gap_silence": re.compile(r"\[PullAudio\] App source gap silence", re.IGNORECASE),
    "audio_zero_drift_residual": re.compile(r"\[A/V ZERO DRIFT WARNING\]", re.IGNORECASE),
    "wgc_output_limited": re.compile(r"\[WGC CFR\] (?:Output limited|Encoder cannot sustain target)"),
    "wgc_stop_drain_aborted": re.compile(r"\[EncoderThread\] CFR stop drain aborted"),
    "wgc_fresh_catchup": re.compile(r"\[EncoderThread\] CFR Catchup applied using fresh frame"),
    "wgc_too_new_slot_repeat": re.compile(r"\[EncoderThread\] WGC CFR slot repeat: buffered frame is too new"),
    "wgc_stale_visual_debt_drop": re.compile(r"\[EncoderThread\] WGC CFR stale visual debt drop"),
    "wgc_video_prewarm_failed": re.compile(r"WGC transactional video prewarm FAILED"),
    "wgc_start_contract_selected": re.compile(r"WGC CFR start contract selected"),
    "wgc_start_contract_committed": re.compile(r"WGC CFR start contract committed"),
    "wgc_start_contract_error": re.compile(
        r"ERROR: WGC (?:CFR start contract selection failed|"
        r"first frame encoded without a valid transactional start contract)"
    ),
    "wgc_cfr_max_rate_producer_contract": re.compile(r"\[WGC CFR\] Producer contract:.*producerTargetFps=0"),
    "wgc_cfr_producer_contract_fault": re.compile(
        r"(?:\[WGC CFR\] Producer contract:.*producerTargetFps=[1-9]\d*|"
        r"\[WGC CFR\] ERROR: producer contract violation|"
        r"\[WGC\] ERROR: MinUpdateInterval readback mismatch|"
        r"\[Cadence Health\].*\bWgcThr=[1-9]\d*)",
        re.IGNORECASE,
    ),
    "wgc_stale_fresh_catchup_blocked": re.compile(r"\[EncoderThread\] WGC CFR stale fresh-catchup blocked"),
    "wgc_visual_timeline_debt_drop": re.compile(r"\[EncoderThread\] WGC CFR visual timeline debt drop"),
    "wgc_live_scheduler_rebase": re.compile(r"\[EncoderThread\] WGC CFR live scheduler rebase"),
    "wgc_encoder_limited_source_drop": re.compile(
        r"\[EncoderThread\] WGC CFR encoder-limited source drop", re.IGNORECASE
    ),
    "wgc_encoder_limited_mode_mismatch": re.compile(
        r"\[WGC CFR\] encoder-limited mode mismatch", re.IGNORECASE
    ),
    "wgc_selected_source_backtrack": re.compile(
        r"\[EncoderThread\] WGC CFR selected source backtrack blocked", re.IGNORECASE
    ),
    "wgc_cadence_event": re.compile(r"\[WGC CFR CADENCE EVENT\]", re.IGNORECASE),
    "wgc_smoothness_summary": re.compile(r"\[WGC CFR SMOOTHNESS SUMMARY\]", re.IGNORECASE),
    "wgc_stop_frozen_tail_drop": re.compile(r"\[EncoderThread\] WGC CFR stop drain discarded frozen-tail debt"),
    "wgc_stop_hold_repeats": re.compile(r"\[EncoderThread\] WGC CFR stop drain using held pre-stop frame"),
    "wgc_drain_duplicate_summary": re.compile(
        r"\[WGC CFR SUMMARY\].*DupReason\(src=\d+ def=\d+ timer=\d+ drain=[1-9]\d*"
    ),
    "wgc_post_stop_frame_drop": re.compile(r"\[EncoderThread\] WGC CFR post-stop frame drop"),
    "audio_extreme_drift": re.compile(r"\[PullAudio\] WARNING: Extreme drift detected"),
    "writer_finalize_timeout": re.compile(
        r"\[VideoEncoder\] Stop: (?:ERROR writer_finalize_timeout|WARNING - Writer thread did not finish)",
        re.IGNORECASE,
    ),
    "post_mux_probe_timeout": re.compile(r"\[VideoEncoder\] post_mux_probe_timeout", re.IGNORECASE),
    "post_mux_probe_hang": re.compile(r"writer_finalize_timeout.*phase=post_mux_probe", re.IGNORECASE),
    "writer_finalize_slow": re.compile(r"\[VideoEncoder\] Stop: WARNING writer_finalize_slow", re.IGNORECASE),
    "writer_sync_finalize": re.compile(r"\[VideoEncoder\] Sync Stop: Finalizing file", re.IGNORECASE),
    "cfr_packet_coverage_fault": re.compile(
        r"\[VideoEncoder\] CFR packet coverage:.*missing=[1-9]\d*", re.IGNORECASE
    ),
}

STRICT_SYNC_LOG_EVENTS = (
    "audio_latency_cap",
    "audio_retain_trim",
    "audio_retained_trim_summary",
    "audio_latency_trim_summary",
    "audio_coverage_trim",
    "audio_tier2_trim_summary",
    "audio_wgc_lead_cap_trim",
    "audio_overflow_protection_trim",
    "audio_post_resample_trim",
    "audio_large_gap",
    "audio_large_cfr_backlog",
    "audio_catastrophic_resync",
    "audio_underrun",
    "audio_source_padding_summary",
    "audio_overflow",
    "audio_forced_bootstrap",
    "audio_bootstrap_trim",
    "audio_late_app_source_backlog",
    "audio_app_source_gap_silence",
    "audio_zero_drift_residual",
    "audio_extreme_drift",
    "wgc_fresh_catchup",
    "wgc_stop_drain_aborted",
    "wgc_stop_hold_repeats",
    "wgc_drain_duplicate_summary",
    "writer_finalize_timeout",
    "cfr_packet_coverage_fault",
)

CADENCE_AGEMAX_RE = re.compile(r"AgeMax=(\d+)us")
CADENCE_SELMISS_RE = re.compile(r"SelMiss=(\d+)")
CADENCE_STALEUNI_RE = re.compile(r"StaleUni=(\d+)")
CADENCE_ANCIENT_RE = re.compile(r"Ancient=(\d+)")
CADENCE_REPNOFRESH_RE = re.compile(r"RepFreshMiss=(\d+)")
CADENCE_OVER_RE = re.compile(r"Over=0x([0-9A-Fa-f]+)")
CADENCE_WGC_SEL_BIAS_RE = re.compile(r"WgcSelBias=(-?\d+)us")
CADENCE_SHORTFALL_RE = re.compile(r"Shortfall=\d+/([0-9.]+)ms")
CADENCE_LEAD_EXCESS_RE = re.compile(r"LeadExcess=([0-9.]+)ms")
CADENCE_OLDEST_RE = re.compile(r"Oldest=([0-9.]+)ms")
CADENCE_BUFNOW_RE = re.compile(r"BufNow=(\d+)")
CADENCE_WGC_LIVE_REBASE_RE = re.compile(r"WgcLiveRebase=\d+/\d+/(\d+)")
CADENCE_ENC_LOW_BYPASS_RE = re.compile(r"EncLowBypass=(\d+)/(\d+)")
CADENCE_MODE_MISMATCH_RE = re.compile(r"ModeMis=(\d+)/(\d+)")
CADENCE_SOURCE_BACKTRACK_RE = re.compile(r"SrcBack=(\d+)/(\d+)")
WGC_SUMMARY_LIVE_REBASE_RE = re.compile(r"\bLiveRebase=\d+/(\d+)")
WGC_STARTUP_FRAME_AGE_RE = re.compile(r"WGC startup sync post-delay barrier satisfied:.*frameAge=(\d+)us")
WGC_LIVE_START_QPC_RE = re.compile(r"\bliveStartQpc=(\d+)")
PRESENT_HEARTBEAT_GAP_RE = re.compile(r"DetourPresent: heartbeat #\d+ gap=([0-9.]+)ms", re.IGNORECASE)
WGC_SOURCE_STARVED_RE = re.compile(
    r"\[WGC CFR\] Source-starved episode: duration=(\d+)ms out=(\d+) dup=(\d+) minIn=(\d+) minDel=(\d+) "
    r"freshMiss=(\d+)pm minBuf=(\d+)",
    re.IGNORECASE,
)
WGC_ATTRIBUTION_RE = re.compile(r"\[WGC CFR ATTRIBUTION\]\s*(.*)", re.IGNORECASE)
WGC_CADENCE_EVENT_RE = re.compile(r"\[WGC CFR CADENCE EVENT\]\s*mode=([A-Za-z_]+)\s*(.*)", re.IGNORECASE)
WGC_SUMMARY_RE = re.compile(
    r"\[WGC CFR SUMMARY\].*Live=(\d+) Dup=(\d+) DupPct=([0-9.]+)% "
    r".*DupReason\(src=(\d+) def=(\d+) timer=(\d+) drain=(\d+)\).*"
    r"SourceLimitedRepeats=(\d+) StarvedEpisodes=(\d+).*?longest=(\d+)ms "
    r"longestDup=(\d+)(?: longestContiguousDup=(\d+) \((\d+)ms\))? worstIn=(\d+) worstDel=(\d+)",
    re.IGNORECASE,
)
WGC_SMOOTHNESS_SUMMARY_RE = re.compile(
    r"\[WGC CFR SMOOTHNESS SUMMARY\].*encoderLimitedDrops=(\d+) maxDropTicks=(\d+) cadenceEvents=(\d+) "
    r"phaseErrorMax=(\d+)us shortfallMax=([0-9.]+)ms staleDebtDrops=(\d+) liveRebase=(\d+)/(\d+) "
    r"tooNewRepeats=(\d+)(?: syncDelayHolds=(\d+) tooNewLeadMax=(\d+)us avDelay=([0-9.]+)ms"
    r"(?: startupDelay=([0-9.]+)ms scheduleOffset=(-?\d+)us effectiveDelay=([0-9.]+)ms)?)?"
    r"(?: lowSourceBypass=(\d+) modeMismatch=(\d+) sourceBacktrack=(\d+))?",
    re.IGNORECASE,
)
WGC_SMOOTHNESS_EXTRA_RE = re.compile(
    r"syncDelaySourceLimitedHolds=(\d+) syncDelayPolicyHolds=(\d+) "
    r"startupReserveFrames=(\d+) startupReserveSpan=(-?\d+)us startupDelayTarget=(-?\d+)us "
    r"startupReserveSelected=(\d+) startupReserveReason=([A-Za-z0-9_-]+)",
    re.IGNORECASE,
)
WGC_SMOOTHNESS_BUFFER_RE = re.compile(
    r"smoothBuf=(?P<enabled>\d+) smoothTargetMs=(?P<target_ms>\d+) "
    r"smoothFrames=(?P<actual_frames>\d+)/(?P<retained_frames>\d+)/(?P<desired_frames>\d+) "
    r"smoothDelay=(?P<delay_ms>[0-9.]+)ms "
    r"(?:(?:smoothTargetDelay=\d+us smoothActualDelay=\d+us "
    r"smoothDelayDeficit=\d+us startupDelayDeficit=\d+us )?)"
    r"smoothPoolSlots=(?P<pool_slots>\d+)"
    r"(?: sourceFramePoolBuffers=(?P<source_buffers>\d+) budgetSurfaces=(?P<budget_surfaces>\d+) "
    r"syncFrames=(?P<sync_frames>\d+) "
    r"(?:(?:extraFrames=(?P<extra_frames>\d+) )?"
    r"(?:retainedCap=(?P<retained_cap>\d+) )?"
    r"(?:reservedFreeSlots=(?P<reserved_free_slots>\d+) )?)?"
    r"safetySlots=(?P<safety_slots>\d+) "
    r"(?:(?:retainedCapTrim=(?P<retained_cap_trim>\d+) "
    r"ingressAccepted=(?P<ingress_accepted>\d+) ingressDecimated=(?P<ingress_decimated>\d+) "
    r"(?:(?:ingressPlaySoft=(?P<ingress_play_soft>\d+) "
    r"ingressPlayCredit=(?P<ingress_play_credit>\d+) )?)"
    r"ingressRetained=(?P<ingress_retained>\d+)/(?P<ingress_cap>\d+) "
    r"ingressLowWater=(?P<ingress_low_water>\d+) )?)"
    r"leasedMax=(?P<leased_max>\d+) "
    r"(?:(?:freeNow=(?P<free_now>\d+) )?)"
    r"freeMin=(?P<free_min>\d+) "
    r"(?:(?:poolPressureTrim=(?P<pool_pressure_trim>\d+) )?)"
    r"poolSaturatedDrops=(?P<pool_saturated_drops>\d+) "
    r"overwritePrevented=(?P<overwrite_prevented>\d+) "
    r"leaseMismatches=(?P<lease_mismatches>\d+))?"
    r"(?: smoothVramMB=(?P<vram_mb>[0-9.]+))? "
    r"smoothCapLimited=(?P<cap_limited>\d+)(?: smoothReason=(?P<reason>[A-Za-z0-9_-]+))?",
    re.IGNORECASE,
)
WGC_SMOOTHNESS_INGRESS_RE = re.compile(
    r"\[WGC CFR SMOOTHNESS INGRESS\].*accepted=(?P<accepted>\d+) decimated=(?P<decimated>\d+) "
    r"retained=(?P<retained>\d+)/(?P<cap>\d+) lowWater=(?P<low_water>\d+) "
    r"accLowWater=(?P<acc_low_water>\d+) accRecovery=(?P<acc_recovery>\d+) "
    r"accSourceBelow=(?P<acc_source_below>\d+) accHealthy=(?P<acc_healthy>\d+) "
    r"(?:(?:accPlaySoft=(?P<acc_play_soft>\d+) accPlayCredit=(?P<acc_play_credit>\d+) )?)"
    r"decSoftReserve=(?P<dec_soft_reserve>\d+) decHardReserve=(?P<dec_hard_reserve>\d+) "
    r"decCredit=(?P<dec_credit>\d+) softReservePressure=(?P<soft_pressure>\d+) "
    r"hardReservePressure=(?P<hard_pressure>\d+) "
    r"(?:(?:dupTsSeen=(?P<dup_ts_seen>\d+) dupTsSkipped=(?P<dup_ts_skipped>\d+) )?)"
    r"lastReason=(?P<last_reason>[A-Za-z0-9_-]+)",
    re.IGNORECASE,
)
WGC_SMOOTHNESS_SOURCE_RE = re.compile(
    r"\[WGC CFR SMOOTHNESS SOURCE\].*acceptedTotal=(?P<accepted_total>\d+) "
    r"cfrTicksTotal=(?P<cfr_ticks_total>\d+) rollingAccepted=(?P<rolling_accepted>\d+) "
    r"rollingCfrTicks=(?P<rolling_cfr_ticks>\d+) rollingDeficit=(?P<rolling_deficit>\d+) "
    r"rollingSurplus=(?P<rolling_surplus>\d+) lastWindowAccepted=(?P<last_window_accepted>\d+) "
    r"lastWindowCfrTicks=(?P<last_window_cfr_ticks>\d+) windowSlots=(?P<window_slots>\d+)",
    re.IGNORECASE,
)
WGC_DELAY_REALIZATION_RE = re.compile(
    r"delayReservoirLowWaterFrames=(\d+) delayReservoirTargetFrames=(\d+) "
    r"delayReservoirLowWaterTicks=(\d+) realizedDelayAvg=(\d+)us realizedDelayMin=(\d+)us "
    r"realizedDelayMax=(\d+)us delayResidualAvg=([+-]?\d+)/(\d+)us "
    r"delayResidualMax=(\d+)us delayResidualP95=(\d+)us delayResidualLateMax=(\d+)us "
    r"delayResidualEarlyMax=(\d+)us",
    re.IGNORECASE,
)
WGC_SMOOTHNESS_FLOOR_RE = re.compile(
    r"\[WGC CFR SMOOTHNESS FLOOR\] smoothFloorSource=(\w+) smoothFloorConfigured=(\d+) "
    r"smoothFloorMs=(\d+) smoothFloorRequestedUs=([+-]?\d+) smoothFloorDelayUs=([+-]?\d+) "
    r"smoothFloorClampedBy=(\w+) smoothFloorRealizedTargetUs=([+-]?\d+) "
    r"measuredDeliveryGapUs\(avg/max\)=(\d+)/(\d+) measuredSourceJitterUs\(avg/max\)=(\d+)/(\d+) "
    r"realizedDelay\(min/avg/max\)Us=(\d+)/(\d+)/(\d+) residualLateMaxUs=(\d+) avContentDelayActive=(\d+)",
    re.IGNORECASE,
)
WGC_DELAY_RAW_RESIDUAL_RE = re.compile(
    r"rawResidualAvg=([+-]?\d+)/(\d+)us rawResidualMax=(\d+)us rawResidualP95=(\d+)us "
    r"rawResidualLateMax=(\d+)us rawResidualEarlyMax=(\d+)us "
    r"predictedResidualAvg=([+-]?\d+)/(\d+)us predictedResidualP95=(\d+)us "
    r"predictedResidualLateMax=(\d+)us rawMinusPredictedAvg=([+-]?\d+)/(\d+)us "
    r"rawMinusPredictedMax=(\d+)us",
    re.IGNORECASE,
)
WGC_DELAY_RELAXED_RE = re.compile(
    r"delayResidualRelaxedSelections=(\d+) delayResidualRelaxedMax=(\d+)us"
    r"(?: delayResidualRelaxedRejectedSync=(\d+) delayRepeatClusterPressure=(\d+) "
    r"delayRepeatClusterMax=(\d+)"
    r"(?: delayResidualRelaxedBetter=(\d+) delayResidualRelaxedCluster=(\d+) "
    r"delayResidualRelaxedRejectedHeadroom=(\d+) delayResidualRelaxedRejectedCost=(\d+) "
    r"(?:delaySoftLateRejected=(\d+) delaySoftLateAccepted=(\d+) "
    r"delayOlderFrameAvoidedRepeat=(\d+) delaySourceLimitedRepeats=(\d+) )?"
    r"(?:delayRepeatRescue=\d+/\d+ delayRepeatRescueRejected=\d+/\d+/\d+ "
    r"(?:delayRepeatPromoted=\d+/\d+ delayRepeatPromoteRejectedSoft=\d+ "
    r"delayRepeatSafeAfterPromote=\d+ )?"
    r"delayRepeatSafeCandidate=\d+ delayRepeatNoSafeCandidate=\d+ "
    r"(?:delayRepeatSoftSafeCandidate=\d+ delayRepeatNoSoftSafeCandidate=\d+ )?"
    r"delayRepeatWindowClass=\d+/\d+/\d+ "
    r"(?:delayRepeatWindowState=\d+/\d+/\d+/\d+/\d+ delayPostStallSafeFrames=\d+ )?"
    r"delayRepeatReserveMax=\d+/\d+us )?"
    r"delaySourceRecoveryHolds=(\d+) delaySourceRecoveryTicks=(\d+))?)?",
    re.IGNORECASE,
)
WGC_DELAY_REPEAT_RESCUE_RE = re.compile(
    r"delayRepeatRescue=(\d+)/(\d+) delayRepeatRescueRejected=(\d+)/(\d+)/(\d+) "
    r"(?:delayRepeatPromoted=(\d+)/(\d+) delayRepeatPromoteRejectedSoft=(\d+) "
    r"delayRepeatSafeAfterPromote=(\d+) )?"
    r"delayRepeatSafeCandidate=(\d+) delayRepeatNoSafeCandidate=(\d+) "
    r"(?:delayRepeatSoftSafeCandidate=(\d+) delayRepeatNoSoftSafeCandidate=(\d+) )?"
    r"delayRepeatWindowClass=(\d+)/(\d+)/(\d+) "
    r"(?:delayRepeatWindowState=(\d+)/(\d+)/(\d+)/(\d+)/(\d+) delayPostStallSafeFrames=(\d+) )?"
    r"delayRepeatReserveMax=(\d+)/(\d+)us",
    re.IGNORECASE,
)
WGC_DELAY_POST_REJECT_RE = re.compile(r"delayPostSelectionRejectedSync=(\d+)", re.IGNORECASE)
WGC_SMOOTHNESS_LOWER_BOUND_RE = re.compile(
    r"delayPostSelectionRejectedSync=(\d+)"
    r"(?: delayPostSelectionRescuedSync=(\d+) sourceRepeatLowerBound=(\d+) "
    r"excessRepeats=(\d+) policyAddedRepeats=(\d+) excessRepeatClusters=(\d+) "
    r"excessRepeatClusterMax=(\d+) smoothnessNotMaximal=(\d+))?",
    re.IGNORECASE,
)
WGC_PERF_RE = re.compile(r"\[WGC Perf\].*", re.IGNORECASE)
WGC_QUALITY_RE = re.compile(r"\[WGC CFR QUALITY\]\s*(.*)", re.IGNORECASE)
WGC_SOURCE_COVERAGE_RE = re.compile(r"\[WGC CFR SOURCE COVERAGE\]\s*(.*)", re.IGNORECASE)
INJECT_PERF_RE = re.compile(r"\[Inject Perf\].*", re.IGNORECASE)
INJECT_CONTENTION_RE = re.compile(
    r"\[Inject Contention(?: SUMMARY)?\].*CaptureLock=(\d+) CpuLease=(\d+) GpuBusy=(\d+) "
    r"RingFull=(\d+) EventSignals=(\d+)(?: PubToIngest=(\d+)/(\d+)us)?",
    re.IGNORECASE,
)
INJECT_CFR_SUMMARY_RE = re.compile(
    r"\[Inject CFR SUMMARY\].*Live=(\d+) Dup=(\d+) DupPct=([0-9.]+)% "
    r"DupReason\(src=(\d+) def=(\d+) timer=(\d+) drain=(\d+)\).*"
    r"FreshCatchup=(\d+) RepeatCatchup=(\d+) StaleTrim=(\d+)(?: Recovery=(\d+)/(\d+))?",
    re.IGNORECASE,
)
INJECT_CFR_SOURCE_RE = re.compile(
    r"\[Inject CFR SUMMARY\] SourceFps=([0-9.]+)\.\.([0-9.]+) JitterMax=(\d+)us SelMax=(\d+)us",
    re.IGNORECASE,
)
INJECT_CFR_QUALITY_SUMMARY_RE = re.compile(
    r"\[Inject CFR QUALITY SUMMARY\] TargetSelect=(\d+) Superseded=(\d+) TargetHold=(\d+) "
    r"HoldWithCandidate=(\d+) BufferCapTrim=(\d+) TargetResidualMax=(\d+)us",
    re.IGNORECASE,
)
INJECT_CFR_REPEAT_PRESSURE_RE = re.compile(r"\[Inject CFR\] Repeat pressure:\s*(.*)", re.IGNORECASE)
CFR_PHASE_LOCK_SUMMARY_RE = re.compile(
    r"\[CFR PHASE LOCK SUMMARY\] Backend=(\w+) Enabled=(\d+) Locked=(\d+) Offset=(-?\d+)us "
    r"Stable=(\d+) Unstable=(\d+) Acquire=(\d+) Rephase=(\d+) Release=(\d+) Multiplier=(\d+)",
    re.IGNORECASE,
)
FINAL_PACKET_TIMELINE_RE = re.compile(
    r"Final packet timeline: target=(\d+) us videoEnd=(\d+) us audioMinEnd=(\d+) us audioMaxEnd=(\d+) us "
    r"maxPacketDelta=(\d+) us.*audioPastTarget=(\d+)",
    re.IGNORECASE,
)
FINAL_METADATA_RE = re.compile(
    r"Final metadata durations: target=(\d+) us video=(\d+) us audioMin=(\d+) us audioMax=(\d+) us maxDelta=(\d+) us "
    r".*overload\(encoder=(\d+) mux=(\d+)\) backpressure=(\d+)",
    re.IGNORECASE,
)
POST_MUX_AUDIO_MISMATCH_RE = re.compile(r"Post-mux audio duration mismatch .*maxDelta=(\d+)", re.IGNORECASE)
POST_MUX_AUDIO_PRIMING_RE = re.compile(
    r"Post-mux audio codec priming evidence .*maxDelta=(\d+) "
    r"primingTolerance=(\d+) roundingTolerance=(\d+)",
    re.IGNORECASE,
)
AUDIO_CODEC_CONTRACT_RE = re.compile(
    r"\[AudioCodecContract\] encoder=(\S+) id=(\d+) fmt=(\S+) rate=(\d+) channels=(\d+) mask=(0x[0-9a-f]+) "
    r"rawBits=(\d+) frame=(\d+) caps=(0x[0-9a-f]+) initialPadding=(\d+) finalPolicy=(\d+) "
    r"codecDelay=(\d+) discardPadding=(\d+)",
    re.IGNORECASE,
)
AUDIO_FINALIZATION_RE = re.compile(
    r"\[AudioFinalization\] encoder=(\S+) stream=(-?\d+) target=(\d+) input=(\d+) expectedSilence=(\d+) "
    r"submitted=(\d+) priming=(\d+) terminalPadding=(\d+) packetEnd=(-?\d+) expectedDecoded=(\d+) "
    r"packets=(\d+) bytes=(\d+) durationless=(\d+) drainEof=(\d+) protocolError=(\d+)",
    re.IGNORECASE,
)
PACKET_MISMATCH_RE = re.compile(r"Packet-level A/V duration mismatch", re.IGNORECASE)
LOG_LINE_TIMESTAMP_RE = re.compile(r"^\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3,6})\]")
STOP_AUDIO_TRACK_RE = re.compile(
    r"\[STOP AUDIO TRACK\] Track (\d+): encoded=(\d+) expected=(\d+) diff=([+-]?\d+) "
    r"\(([+-]?[0-9.]+) ms\).*sources=\[([^\]]*)\]",
    re.IGNORECASE,
)
STOP_AUDIO_SOURCE_RE = re.compile(
    r"\[STOP AUDIO\] Source (\d+): track=(\d+) encoded=(\d+).*?pad:(\d+) qgap:(\d+)"
    r"(?: qjoin:(\d+) qjoinKeep:(\d+))?.*?ringPeak=(\d+) ringUnderruns=(\d+)"
    r"(?: process=([^\s]+))?",
    re.IGNORECASE,
)
STOP_AUDIO_LATENCY_RE = re.compile(
    r"\[STOP AUDIO LATENCY\] Source (\d+) track=(\d+) appAudioDelay avg=([0-9.]+)ms max=(\d+)ms",
    re.IGNORECASE,
)
ZERO_DRIFT_WARNING_RE = re.compile(
    r"\[A/V ZERO DRIFT WARNING\] Track (\d+) residual_samples=([+-]?\d+) residual_us=([+-]?\d+) "
    r"target_samples=(\d+) cursor_samples=(\d+)",
    re.IGNORECASE,
)
EXTERNAL_OVERLAY_RE = re.compile(r"\b(Steam|Rockstar|RTSS|ReShade|SpecialK|Streamline|FFX)\b", re.IGNORECASE)
CRASH_LOG_RE = re.compile(r"\b(CRASH DETECTED|Unhandled exception|Exception Code:|VEH Exception:)\b", re.IGNORECASE)
LATE_APP_LIVE_JOIN_SRC_RE = re.compile(r"\[AudioLoop\] Late app source live join src=(\d+)", re.IGNORECASE)
LATE_APP_PRIMED_SRC_RE = re.compile(
    r"\[PullAudio\] Source primed\s+-\s+src=(\d+).*?lateStart=(\d+)ms",
    re.IGNORECASE,
)

WGC_ACTIVE_DELAY_POLICY_HOLD_FAULT_MIN_COUNT = 120
WGC_ACTIVE_DELAY_POLICY_HOLD_FAULT_PERMILLE = 250
WGC_CFR_SMOOTHNESS_EXCESS_REPEAT_FAULT_MIN_COUNT = 120
WGC_CFR_SMOOTHNESS_POLICY_REPEAT_NOTICE_MIN_COUNT = 24
WGC_CFR_SMOOTHNESS_POLICY_REPEAT_NOTICE_PERMILLE = 5
WGC_CFR_SMOOTHNESS_EXCESS_REPEAT_CLUSTER_FAULT_TICKS = 24
WGC_AUDIO_LATE_RISK_SOFT_ACCEPT_MIN_COUNT = 10
WGC_AUDIO_LATE_RISK_P95_US = 7000
WGC_AUDIO_LATE_RISK_NEAR_CAP_US = 9500
# A single residual maximum slightly above 10 ms is normal evidence at a 120 Hz
# source boundary. Require persistent average/p95 evidence or a materially larger
# isolated excursion before calling the timestamp domains or A/V delay faulty.
WGC_RESIDUAL_ISOLATED_MAX_FAULT_US = 25000
# Realized content-delay spread (max - min) on an active-delay run above which the displayed
# content age swings enough to be visible as non-uniform playback / abnormal judder, distinct
# from plain CFR repeat/drop. The GPU-bound Strange Brigade run swung 22.9..44.4 ms (~21.5 ms).
WGC_REALIZED_DELAY_INSTABILITY_SPREAD_US = 12000

TRIAGE_AUDIO_FAULT_EVENTS = {
    "audio_finalization_protocol_error",
    "audio_worker_overrun",
    "audio_latency_cap",
    "audio_retain_trim",
    "audio_retained_trim_summary",
    "audio_latency_trim_summary",
    "audio_coverage_trim",
    "audio_tier2_trim_summary",
    "audio_wgc_lead_cap_trim",
    "audio_overflow_protection_trim",
    "audio_post_resample_trim",
    "audio_large_gap",
    "audio_underrun",
    "audio_overflow",
    "audio_forced_bootstrap",
    "audio_bootstrap_trim",
    "audio_late_app_source_backlog",
    "audio_app_source_gap_silence",
    "audio_zero_drift_residual",
    "audio_extreme_drift",
}

TRIAGE_VISUAL_FAULT_EVENTS = {
    "cfr_finalization_lattice_error",
    "cfr_packet_coverage_fault",
    "wgc_fresh_catchup",
    "wgc_stop_drain_aborted",
    "wgc_stop_hold_repeats",
    "wgc_drain_duplicate_summary",
    "wgc_start_contract_error",
    "wgc_cfr_producer_contract_fault",
    "inject_cfr_recovery_stalled",
}

TRIAGE_MUX_FAULT_EVENTS = {
    "writer_finalize_timeout",
    "post_mux_probe_hang",
}


def fail(message) -> NoReturn:
    print(f"error: {message}", file=sys.stderr)
    raise SystemExit(1)


def run_command(command, text=True):
    result = subprocess.run(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=text,
        encoding="utf-8" if text else None,
        errors="replace" if text else None,
        check=False,
    )
    if result.returncode != 0:
        fail(
            "command failed ({code}): {cmd}\n{stderr}".format(
                code=result.returncode, cmd=" ".join(str(part) for part in command), stderr=result.stderr.strip()
            )
        )
    return result


def split_decoder_stderr(stderr):
    actionable = []
    ignored_environment = []
    for raw_line in stderr.splitlines():
        line = raw_line.strip()
        if not line:
            continue
        is_missing_windows_app_manifest = (
            "GLib-GIO-WARNING" in line
            and "Failed to open application manifest" in line
            and "C:\\Windows\\SystemApps\\" in line
            and "error code 0x2" in line
        )
        if is_missing_windows_app_manifest:
            ignored_environment.append(line)
        else:
            actionable.append(line)
    return "\n".join(actionable), "\n".join(ignored_environment)


def run_ffprobe_json(ffprobe, args):
    result = run_command([str(ffprobe), "-v", "error", *args, "-of", "json"])
    try:
        return json.loads(result.stdout)
    except json.JSONDecodeError as exc:
        fail(f"ffprobe returned invalid JSON: {exc}")


def build_read_interval(start_time, duration):
    if duration <= 0.0:
        return None
    safe_start = max(0.0, start_time)
    return f"{safe_start:.6f}%+{duration:.6f}"


def parse_float(value, default=0.0):
    if value in (None, "", "N/A"):
        return default
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def parse_int(value, default=0):
    if value in (None, "", "N/A"):
        return default
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def parse_base0_int(value, default=0):
    if value in (None, "", "N/A"):
        return default
    try:
        return int(str(value), 0)
    except (TypeError, ValueError):
        return default


def parse_named_int_field(line, name, default=None):
    match = re.search(r"\b" + re.escape(name) + r"=(-?\d+)", line)
    return parse_int(match.group(1)) if match else default


def parse_named_float_field(line, name, default=None):
    match = re.search(r"\b" + re.escape(name) + r"=(-?[0-9.]+)", line)
    return parse_float(match.group(1)) if match else default


def parse_ratio(text):
    if not text or text == "0/0":
        return 0.0
    if "/" in text:
        numerator, denominator = text.split("/", 1)
        denominator_value = parse_float(denominator, 0.0)
        if denominator_value == 0.0:
            return 0.0
        return parse_float(numerator, 0.0) / denominator_value
    return parse_float(text, 0.0)


def parse_ratio_fraction(text):
    if not text or text == "0/0":
        return Fraction(0, 1)
    try:
        return Fraction(str(text))
    except (ValueError, ZeroDivisionError):
        return Fraction(0, 1)


def round_fraction(value):
    if value < 0:
        return -round_fraction(-value)
    return (value.numerator * 2 + value.denominator) // (2 * value.denominator)


def safe_mean(values):
    return statistics.mean(values) if values else 0.0


def safe_pstdev(values):
    return statistics.pstdev(values) if len(values) > 1 else 0.0


def format_seconds(value):
    return f"{value:.6f}s"


def format_metric(value):
    if isinstance(value, int):
        return str(value)
    if isinstance(value, float):
        return f"{value:.6f}" if abs(value) < 1.0 else f"{value:.3f}"
    return str(value)


def parse_named_int_thresholds(entries, valid_names, option_name):
    thresholds = {}
    for entry in entries:
        if "=" not in entry:
            fail(f"{option_name} expects NAME=VALUE entries, got: {entry}")
        name, value_text = entry.split("=", 1)
        name = name.strip()
        value_text = value_text.strip()
        if name not in valid_names:
            fail(
                f"unknown {option_name} name '{name}'. Valid names: {', '.join(sorted(valid_names))}"
            )
        try:
            value = int(value_text)
        except ValueError:
            fail(f"invalid integer value for {option_name} {name}: {value_text}")
        thresholds[name] = value
    return thresholds


def make_upper_bound_check(name, actual, limit, unit="", tolerance=0.0):
    suffix = f" {unit}" if unit else ""
    return {
        "name": name,
        "passed": actual <= limit + tolerance,
        "actual": f"{format_metric(actual)}{suffix}",
        "expected": f"<= {format_metric(limit)}{suffix}",
    }


def make_lower_bound_check(name, actual, limit, unit=""):
    suffix = f" {unit}" if unit else ""
    return {
        "name": name,
        "passed": actual >= limit,
        "actual": f"{format_metric(actual)}{suffix}",
        "expected": f">= {format_metric(limit)}{suffix}",
    }


def print_checks(checks):
    print("checks:")
    if not checks:
        print("  none")
        return
    for check in checks:
        status = "PASS" if check["passed"] else "FAIL"
        print(
            "  {status} {name}: actual={actual} expected={expected}".format(
                status=status,
                name=check["name"],
                actual=check["actual"],
                expected=check["expected"],
            )
        )


def evaluate_thresholds(args, nominal_fps, video_timing, duplicate_runs, audio_duration_spread, video_audio_max_delta,
                        log_summary):
    checks = []

    mean_frame_delta_error_us = None
    if video_timing["frame_count"] > 1 and nominal_fps > 0.0:
        expected_delta = 1.0 / nominal_fps
        mean_frame_delta_error_us = abs(video_timing["delta_mean"] - expected_delta) * 1_000_000.0

    if args.max_mean_frame_delta_error_us is not None:
        if mean_frame_delta_error_us is None:
            fail("--max-mean-frame-delta-error-us requires valid video timing with at least 2 frames and nominal FPS")
        checks.append(
            make_upper_bound_check(
                "mean_frame_delta_error", mean_frame_delta_error_us, args.max_mean_frame_delta_error_us, "us"
            )
        )

    if args.max_audio_spread_ms is not None:
        checks.append(
            make_upper_bound_check("audio_duration_spread", audio_duration_spread * 1000.0, args.max_audio_spread_ms,
                                   "ms", tolerance=0.0005)
        )

    if args.max_video_audio_delta_ms is not None:
        checks.append(
            make_upper_bound_check(
                "max_video_audio_duration_delta", video_audio_max_delta * 1000.0, args.max_video_audio_delta_ms,
                "ms", tolerance=0.0005
            )
        )

    if args.max_longest_duplicate_run is not None:
        if duplicate_runs is None:
            fail("--max-longest-duplicate-run requires --framehash")
        checks.append(
            make_upper_bound_check(
                "longest_duplicate_run", duplicate_runs["longest_run"], args.max_longest_duplicate_run, "frames"
            )
        )

    if args.max_repeated_frames is not None:
        if duplicate_runs is None:
            fail("--max-repeated-frames requires --framehash")
        checks.append(
            make_upper_bound_check(
                "repeated_frame_count", duplicate_runs["repeated_frame_count"], args.max_repeated_frames, "frames"
            )
        )

    log_event_thresholds = parse_named_int_thresholds(args.max_log_event, LOG_PATTERNS.keys(), "--max-log-event")
    if args.strict_sync_events:
        for name in STRICT_SYNC_LOG_EVENTS:
            log_event_thresholds.setdefault(name, 0)
    if log_event_thresholds and log_summary is None:
        fail("--max-log-event requires --log")
    for name, limit in sorted(log_event_thresholds.items()):
        checks.append(make_upper_bound_check(f"log.{name}", log_summary["counts"].get(name, 0), limit, "count"))

    cadence_metric_values = {
        "age_max_us": 0 if log_summary is None else log_summary["max_age_max_us"],
        "sel_miss": 0 if log_summary is None else log_summary["max_sel_miss"],
        "stale_unique": 0 if log_summary is None else log_summary["max_stale_unique"],
        "ancient": 0 if log_summary is None else log_summary["max_ancient"],
        "rep_no_fresh": 0 if log_summary is None else log_summary["max_rep_no_fresh"],
        "wgc_sel_bias_abs_us": 0 if log_summary is None else log_summary["max_wgc_sel_bias_abs_us"],
        "wgc_shortfall_ms": 0 if log_summary is None else log_summary["max_wgc_shortfall_ms"],
        "wgc_lead_excess_ms": 0 if log_summary is None else log_summary["max_wgc_lead_excess_ms"],
        "wgc_oldest_ms": 0 if log_summary is None else log_summary["max_wgc_oldest_ms"],
        "wgc_buffered_frames": 0 if log_summary is None else log_summary["max_wgc_buffered_frames"],
        "wgc_live_rebase_max_ticks": 0 if log_summary is None else log_summary["max_wgc_live_rebase_ticks"],
        "wgc_startup_frame_age_us": 0 if log_summary is None else log_summary["max_wgc_startup_frame_age_us"],
        "wgc_encoder_limited_drops": 0 if log_summary is None else log_summary["max_wgc_encoder_limited_drops"],
        "wgc_phase_error_us": 0 if log_summary is None else log_summary["max_wgc_phase_error_us"],
    }
    cadence_metric_thresholds = parse_named_int_thresholds(
        args.max_cadence_metric, cadence_metric_values.keys(), "--max-cadence-metric"
    )
    if cadence_metric_thresholds and log_summary is None:
        fail("--max-cadence-metric requires --log")
    for name, limit in sorted(cadence_metric_thresholds.items()):
        checks.append(make_upper_bound_check(f"cadence.{name}", cadence_metric_values[name], limit, "count"))

    return checks, mean_frame_delta_error_us


def analyze_streams(ffprobe, capture_path):
    data = run_ffprobe_json(
        ffprobe,
        [
            "-show_streams",
            "-show_format",
            str(capture_path),
        ],
    )
    if not isinstance(data, dict):
        fail("ffprobe stream output was not a JSON object")
    streams = data.get("streams", [])
    if not isinstance(streams, list):
        streams = []
    format_info = data.get("format", {})
    if not isinstance(format_info, dict):
        format_info = {}
    typed_streams = [stream for stream in streams if isinstance(stream, dict)]
    video_streams = [stream for stream in typed_streams if stream.get("codec_type") == "video"]
    audio_streams = [stream for stream in typed_streams if stream.get("codec_type") == "audio"]
    if not video_streams:
        fail("capture has no video stream")
    return format_info, video_streams, audio_streams


def summarize_cfr_packet_coverage(packet_pts, packet_durations, nominal_fps):
    packet_count = len(packet_pts)
    if packet_count == 0 or nominal_fps <= 0.0:
        return {
            "packet_count": packet_count,
            "expected_packets": 0,
            "missing_packets": 0,
            "max_gap_ticks": 0.0,
            "first_pts": 0.0,
            "last_pts": 0.0,
            "packet_end": 0.0,
            "span": 0.0,
            "complete": False,
        }
    # ffprobe reports packets in decode/mux order; codecs with B-frame
    # reordering can therefore have non-monotonic PTS in that order.
    ordered_pts = sorted(packet_pts)
    frame_duration = 1.0 / nominal_fps
    last_duration = packet_durations[-1] if packet_durations else 0.0
    if last_duration <= 0.0:
        last_duration = frame_duration
    span = max(0.0, ordered_pts[-1] + last_duration - ordered_pts[0])
    expected_packets = max(1, int(round(span * nominal_fps)))
    gaps = [ordered_pts[index] - ordered_pts[index - 1] for index in range(1, packet_count)]
    max_gap_ticks = (max(gaps) * nominal_fps) if gaps else 1.0
    missing_packets = max(0, expected_packets - packet_count)
    complete = missing_packets == 0 and max_gap_ticks <= 1.01
    return {
        "packet_count": packet_count,
        "expected_packets": expected_packets,
        "missing_packets": missing_packets,
        "max_gap_ticks": max_gap_ticks,
        "first_pts": ordered_pts[0],
        "last_pts": ordered_pts[-1],
        "packet_end": ordered_pts[-1] + last_duration,
        "span": span,
        "complete": complete,
    }


def analyze_cfr_packet_coverage(ffprobe, capture_path, nominal_fps):
    data = run_ffprobe_json(
        ffprobe,
        [
            "-select_streams",
            "v:0",
            "-show_packets",
            "-show_entries",
            "packet=pts_time,duration_time",
            str(capture_path),
        ],
    )
    packets = data.get("packets", []) if isinstance(data, dict) else []
    typed_packets = [packet for packet in packets if isinstance(packet, dict) and "pts_time" in packet]
    pts = [parse_float(packet.get("pts_time")) for packet in typed_packets]
    durations = [parse_float(packet.get("duration_time")) for packet in typed_packets]
    return summarize_cfr_packet_coverage(pts, durations, nominal_fps)


def analyze_video_timing(ffprobe, capture_path, read_interval=None, nominal_fps=0.0):
    args = [
        "-select_streams",
        "v:0",
        "-show_frames",
        "-show_entries",
        "frame=best_effort_timestamp_time,pkt_duration_time",
    ]
    if read_interval:
        args.extend(["-read_intervals", read_interval])
    args.append(str(capture_path))
    frame_data = run_ffprobe_json(ffprobe, args)
    if not isinstance(frame_data, dict):
        fail("ffprobe video frame output was not a JSON object")
    frames = frame_data.get("frames", [])
    if not isinstance(frames, list):
        frames = []
    typed_frames = [frame for frame in frames if isinstance(frame, dict)]
    pts = [
        parse_float(frame.get("best_effort_timestamp_time"))
        for frame in typed_frames
        if "best_effort_timestamp_time" in frame
    ]
    if not pts:
        return {
            "frame_count": 0,
            "first_pts": 0.0,
            "last_pts": 0.0,
            "frame_end": 0.0,
            "duration": 0.0,
            "delta_histogram": collections.Counter(),
            "delta_mean": 0.0,
            "delta_min": 0.0,
            "delta_max": 0.0,
            "delta_stdev": 0.0,
        }

    deltas = [round(pts[i] - pts[i - 1], 6) for i in range(1, len(pts))]
    durations = [parse_float(frame.get("pkt_duration_time")) for frame in typed_frames]
    last_duration = durations[-1] if durations else 0.0
    if last_duration <= 0.0 and nominal_fps > 0.0:
        last_duration = 1.0 / nominal_fps
    elif last_duration <= 0.0 and deltas:
        last_duration = deltas[-1]
    frame_end = pts[-1] + last_duration
    return {
        "source": "full-scan",
        "frame_count": len(pts),
        "first_pts": pts[0],
        "last_pts": pts[-1],
        "frame_end": frame_end,
        "duration": max(frame_end - pts[0], 0.0),
        "delta_histogram": collections.Counter(deltas),
        "delta_mean": safe_mean(deltas),
        "delta_min": min(deltas) if deltas else 0.0,
        "delta_max": max(deltas) if deltas else 0.0,
        "delta_stdev": safe_pstdev(deltas),
    }


def analyze_video_stream_metadata(stream_info, format_duration):
    nominal_fps = parse_ratio(stream_info.get("avg_frame_rate") or stream_info.get("r_frame_rate"))
    start_time = parse_float(stream_info.get("start_time"))
    duration = parse_float(stream_info.get("duration"))
    if duration <= 0.0:
        duration = max(format_duration - start_time, 0.0)
    frame_count = parse_int(stream_info.get("nb_frames"))
    if frame_count <= 0 and nominal_fps > 0.0 and duration > 0.0:
        frame_count = max(int(round(duration * nominal_fps)), 0)
    delta = (1.0 / nominal_fps) if nominal_fps > 0.0 else 0.0
    delta_histogram = (
        collections.Counter({round(delta, 6): max(frame_count - 1, 0)})
        if delta > 0.0
        else collections.Counter()
    )
    last_pts = max(start_time + duration - delta, start_time) if frame_count > 0 else start_time
    return {
        "source": "stream-metadata",
        "frame_count": frame_count,
        "first_pts": start_time,
        "last_pts": last_pts,
        "frame_end": start_time + duration,
        "duration": duration,
        "delta_histogram": delta_histogram,
        "delta_mean": delta,
        "delta_min": delta,
        "delta_max": delta,
        "delta_stdev": 0.0,
    }


def analyze_video_duplicate_runs(ffmpeg, capture_path, scale_width):
    result = run_command(
        [
            str(ffmpeg),
            "-v",
            "error",
            "-i",
            str(capture_path),
            "-map",
            "0:v:0",
            "-an",
            "-sn",
            "-dn",
            "-vf",
            f"scale={scale_width}:-2:flags=fast_bilinear,format=gray",
            "-f",
            "framemd5",
            "-",
        ]
    )
    hashes = []
    for line in result.stdout.splitlines():
        if not line or line.startswith("#"):
            continue
        parts = [part.strip() for part in line.split(",")]
        if len(parts) < 6:
            continue
        hashes.append(parts[-1])

    run_lengths = []
    if hashes:
        current_run = 1
        for index in range(1, len(hashes)):
            if hashes[index] == hashes[index - 1]:
                current_run += 1
            else:
                run_lengths.append(current_run)
                current_run = 1
        run_lengths.append(current_run)

    repeated_runs = [run for run in run_lengths if run > 1]
    return {
        "framehash_count": len(hashes),
        "run_count": len(run_lengths),
        "repeated_run_count": len(repeated_runs),
        "repeated_frame_count": sum(run - 1 for run in repeated_runs),
        "longest_run": max(run_lengths) if run_lengths else 0,
        "repeated_histogram": collections.Counter(repeated_runs),
    }


def analyze_audio_stream(ffprobe, capture_path, audio_ordinal, stream_info, read_interval=None):
    args = [
        "-select_streams",
        f"a:{audio_ordinal}",
        "-show_frames",
        "-show_entries",
        "frame=nb_samples,best_effort_timestamp_time,pkt_duration_time",
    ]
    if read_interval:
        args.extend(["-read_intervals", read_interval])
    args.append(str(capture_path))
    frame_data = run_ffprobe_json(ffprobe, args)
    if not isinstance(frame_data, dict):
        fail("ffprobe audio frame output was not a JSON object")
    frames = frame_data.get("frames", [])
    if not isinstance(frames, list):
        frames = []
    typed_frames = [frame for frame in frames if isinstance(frame, dict)]
    sample_rate = parse_int(stream_info.get("sample_rate"))
    total_samples = 0
    first_pts = None
    last_pts = 0.0
    last_duration = 0.0
    for frame in typed_frames:
        total_samples += parse_int(frame.get("nb_samples"))
        if first_pts is None and "best_effort_timestamp_time" in frame:
            first_pts = parse_float(frame.get("best_effort_timestamp_time"))
        if "best_effort_timestamp_time" in frame:
            last_pts = parse_float(frame.get("best_effort_timestamp_time"))
        if "pkt_duration_time" in frame:
            last_duration = parse_float(frame.get("pkt_duration_time"))
    decoded_duration = (total_samples / sample_rate) if sample_rate > 0 else 0.0
    frame_end = last_pts + last_duration if typed_frames else 0.0
    return {
        "source": "full-scan",
        "audio_ordinal": audio_ordinal,
        "stream_index": parse_int(stream_info.get("index")),
        "codec": stream_info.get("codec_name", ""),
        "sample_rate": sample_rate,
        "channels": parse_int(stream_info.get("channels")),
        "sample_total": total_samples,
        "decoded_duration": decoded_duration,
        "frame_start": first_pts if first_pts is not None else 0.0,
        "frame_end": frame_end,
    }


def analyze_audio_stream_metadata(audio_ordinal, stream_info, format_duration):
    sample_rate = parse_int(stream_info.get("sample_rate"))
    duration = parse_float(stream_info.get("duration"))
    start_time = parse_float(stream_info.get("start_time"))
    if duration <= 0.0:
        duration = max(format_duration - start_time, 0.0)
    duration_ts = parse_int(stream_info.get("duration_ts"))
    time_base = parse_ratio(stream_info.get("time_base"))
    sample_total = 0
    if sample_rate > 0:
        if duration_ts > 0 and time_base > 0.0:
            sample_total = int(round(duration_ts * time_base * sample_rate))
        elif duration > 0.0:
            sample_total = int(round(duration * sample_rate))
    return {
        "source": "stream-metadata",
        "audio_ordinal": audio_ordinal,
        "stream_index": parse_int(stream_info.get("index")),
        "codec": stream_info.get("codec_name", ""),
        "sample_rate": sample_rate,
        "channels": parse_int(stream_info.get("channels")),
        "sample_total": sample_total,
        "decoded_duration": duration,
        "frame_start": start_time,
        "frame_end": start_time + duration,
    }


def analyze_audio_decode(ffmpeg, capture_path, audio_ordinal):
    result = subprocess.run(
        [
            str(ffmpeg),
            "-nostdin",
            "-v",
            "error",
            "-i",
            str(capture_path),
            "-map",
            f"0:a:{audio_ordinal}",
            "-f",
            "null",
            os.devnull,
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )
    actionable_stderr, ignored_stderr = split_decoder_stderr(result.stderr)
    return {
        "audio_ordinal": audio_ordinal,
        "returncode": result.returncode,
        "stderr": actionable_stderr,
        "ignored_environment_stderr": ignored_stderr,
    }


def analyze_audio_tail_marker(ffmpeg, capture_path, audio_ordinal, stream_info, threshold):
    sample_rate = parse_int(stream_info.get("sample_rate"))
    channels = max(1, parse_int(stream_info.get("channels"), 1))
    if sample_rate <= 0:
        return {
            "audio_ordinal": audio_ordinal,
            "sample_rate": sample_rate,
            "channels": channels,
            "samples": 0,
            "last_marker_sample": None,
            "last_marker_time": None,
            "tail_silence_ms": None,
            "stderr": "invalid sample rate",
            "returncode": 1,
        }

    command = [
        str(ffmpeg),
        "-nostdin",
        "-v",
        "error",
        "-i",
        str(capture_path),
        "-map",
        f"0:a:{audio_ordinal}",
        "-acodec",
        "pcm_f32le",
        "-f",
        "f32le",
        "-",
    ]
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    frame_bytes = channels * 4
    total_samples = 0
    first_marker_sample = None
    last_marker_sample = None
    peak_sample = 0.0
    clipping_samples = 0
    silent_samples = 0
    longest_silence_samples = 0
    current_silence_samples = 0
    discontinuities = 0
    previous_frame = None
    identical_channel_frames = 0
    signature = []
    signature_stride = max(1, sample_rate // 1000)
    signature_limit = 120000
    pending = b""
    assert process.stdout is not None
    while True:
        chunk = process.stdout.read(1 << 18)
        if not chunk:
            break
        pending += chunk
        usable = (len(pending) // frame_bytes) * frame_bytes
        if usable <= 0:
            continue
        payload = pending[:usable]
        pending = pending[usable:]
        values = array.array("f")
        values.frombytes(payload)
        if sys.byteorder != "little":
            values.byteswap()
        frames = usable // frame_bytes
        for frame in range(frames):
            base = frame * channels
            channel_values = values[base : base + channels]
            peak = max(abs(value) for value in channel_values)
            peak_sample = max(peak_sample, peak)
            clipping_samples += sum(1 for value in channel_values if abs(value) >= 1.0)
            if peak > threshold:
                if first_marker_sample is None:
                    first_marker_sample = total_samples + frame
                last_marker_sample = total_samples + frame
                current_silence_samples = 0
            else:
                silent_samples += 1
                current_silence_samples += 1
                longest_silence_samples = max(longest_silence_samples, current_silence_samples)
            if previous_frame is not None and any(
                abs(channel_values[channel] - previous_frame[channel]) > 1.5 for channel in range(channels)
            ):
                discontinuities += 1
            previous_frame = channel_values
            if channels > 1 and all(
                abs(channel_values[channel] - channel_values[0]) <= 1e-7 for channel in range(1, channels)
            ):
                identical_channel_frames += 1
            absolute_sample = total_samples + frame
            if absolute_sample % signature_stride == 0 and len(signature) < signature_limit:
                signature.append(sum(channel_values) / channels)
        total_samples += frames

    stderr = b""
    if process.stderr is not None:
        stderr = process.stderr.read()
    returncode = process.wait()
    if total_samples > 0 and last_marker_sample is not None:
        tail_silence_ms = (total_samples - 1 - last_marker_sample) * 1000.0 / sample_rate
        last_marker_time = (last_marker_sample + 1) / sample_rate
    else:
        tail_silence_ms = None
        last_marker_time = None
    actionable_stderr, ignored_stderr = split_decoder_stderr(stderr.decode("utf-8", errors="replace"))
    return {
        "audio_ordinal": audio_ordinal,
        "sample_rate": sample_rate,
        "channels": channels,
        "samples": total_samples,
        "first_marker_sample": first_marker_sample,
        "last_marker_sample": last_marker_sample,
        "last_marker_time": last_marker_time,
        "tail_silence_ms": tail_silence_ms,
        "stderr": actionable_stderr,
        "ignored_environment_stderr": ignored_stderr,
        "returncode": returncode,
        "peak": peak_sample,
        "clipping_samples": clipping_samples,
        "silent_samples": silent_samples,
        "longest_silence_samples": longest_silence_samples,
        "discontinuities": discontinuities,
        "identical_channel_frames": identical_channel_frames,
        "signature_rate": sample_rate / signature_stride,
        "signature": signature,
    }


def analyze_inter_track_correlations(decoded_tracks, max_offset_ms=10):
    correlations = []
    for left_index in range(len(decoded_tracks)):
        for right_index in range(left_index + 1, len(decoded_tracks)):
            left = decoded_tracks[left_index]
            right = decoded_tracks[right_index]
            if not left.get("signature") or not right.get("signature"):
                continue
            signature_rate = min(left["signature_rate"], right["signature_rate"])
            if abs(left["signature_rate"] - right["signature_rate"]) > 0.01 or signature_rate <= 0:
                continue
            max_lag = max(0, int(round(max_offset_ms * signature_rate / 1000.0)))
            best = None
            for lag in range(-max_lag, max_lag + 1):
                left_start = max(0, -lag)
                right_start = max(0, lag)
                count = min(len(left["signature"]) - left_start, len(right["signature"]) - right_start)
                if count < 32:
                    continue
                left_values = left["signature"][left_start : left_start + count]
                right_values = right["signature"][right_start : right_start + count]
                left_mean = sum(left_values) / count
                right_mean = sum(right_values) / count
                numerator = 0.0
                left_energy = 0.0
                right_energy = 0.0
                for left_value, right_value in zip(left_values, right_values):
                    left_delta = left_value - left_mean
                    right_delta = right_value - right_mean
                    numerator += left_delta * right_delta
                    left_energy += left_delta * left_delta
                    right_energy += right_delta * right_delta
                denominator = math.sqrt(left_energy * right_energy)
                correlation = numerator / denominator if denominator > 0 else 0.0
                if best is None or correlation > best[0]:
                    best = (correlation, lag)
            if best is not None:
                correlations.append(
                    {
                        "left": left["audio_ordinal"],
                        "right": right["audio_ordinal"],
                        "correlation": best[0],
                        "offset_ms": best[1] * 1000.0 / signature_rate,
                    }
                )
    return correlations


def analyze_completed_capture_exact(ffprobe, ffmpeg, capture_path, threshold=1e-4):
    format_info, video_streams, audio_streams = analyze_streams(ffprobe, capture_path)
    video_stream = video_streams[0]
    fps_text = video_stream.get("avg_frame_rate") or video_stream.get("r_frame_rate")
    fps_fraction = parse_ratio_fraction(fps_text)
    nominal_fps = float(fps_fraction) if fps_fraction > 0 else 0.0
    packet_coverage = analyze_cfr_packet_coverage(ffprobe, capture_path, nominal_fps)
    frame_count = packet_coverage["packet_count"]
    target_frame_count = packet_coverage["expected_packets"]
    target_duration = Fraction(target_frame_count, 1) / fps_fraction if fps_fraction > 0 else Fraction(0, 1)
    decoded_tracks = [
        analyze_audio_tail_marker(ffmpeg, capture_path, ordinal, stream_info, threshold)
        for ordinal, stream_info in enumerate(audio_streams)
    ]
    track_reports = []
    endpoints = []
    for ordinal, (stream_info, decoded) in enumerate(zip(audio_streams, decoded_tracks)):
        sample_rate = decoded["sample_rate"]
        exact_target = target_duration * sample_rate if sample_rate > 0 else Fraction(0, 1)
        lattice_representable = exact_target.denominator == 1
        expected_samples = exact_target.numerator if lattice_representable else round_fraction(exact_target)
        decoder_clean = decoded["returncode"] == 0 and decoded["stderr"] == ""
        endpoint_exact = lattice_representable and decoded["samples"] == expected_samples
        if sample_rate > 0:
            endpoints.append(Fraction(decoded["samples"], sample_rate))
        track_reports.append(
            {
                "audio_ordinal": ordinal,
                "stream_index": parse_int(stream_info.get("index"), ordinal),
                "codec": stream_info.get("codec_name", ""),
                "sample_rate": sample_rate,
                "channels": decoded["channels"],
                "decoded_samples": decoded["samples"],
                "expected_samples": expected_samples,
                "sample_delta": decoded["samples"] - expected_samples,
                "lattice_representable": lattice_representable,
                "endpoint_exact": endpoint_exact,
                "decoder_clean": decoder_clean,
                "decoder_returncode": decoded["returncode"],
                "decoder_stderr": decoded["stderr"],
                "decoder_environment_stderr": decoded.get("ignored_environment_stderr", ""),
                "first_content_sample": decoded["first_marker_sample"],
                "last_content_sample": decoded["last_marker_sample"],
                "tail_silence_ms": decoded["tail_silence_ms"],
                "peak": decoded["peak"],
                "clipping_samples": decoded["clipping_samples"],
                "silent_frames": decoded["silent_samples"],
                "longest_silence_frames": decoded["longest_silence_samples"],
                "discontinuities": decoded["discontinuities"],
                "identical_channel_frames": decoded["identical_channel_frames"],
            }
        )
    endpoint_durations_identical = not endpoints or all(endpoint == endpoints[0] for endpoint in endpoints[1:])
    all_tracks_exact = all(track["endpoint_exact"] and track["decoder_clean"] for track in track_reports)
    correlations = analyze_inter_track_correlations(decoded_tracks)
    return {
        "capture": str(capture_path),
        "container_duration": parse_float(format_info.get("duration")),
        "video": {
            "codec": video_stream.get("codec_name", ""),
            "fps": fps_text,
            "frame_count": frame_count,
            "target_frame_count": target_frame_count,
            "target_duration_numerator": target_duration.numerator,
            "target_duration_denominator": target_duration.denominator,
            "packet_duration": packet_coverage["span"],
            "packet_coverage": packet_coverage,
        },
        "tracks": track_reports,
        "correlations": correlations,
        "endpoint_durations_identical": endpoint_durations_identical,
        "all_tracks_exact": all_tracks_exact,
        "decoder_clean": all(track["decoder_clean"] for track in track_reports),
        "cfr_packet_coverage_exact": packet_coverage["complete"],
        "passed": all_tracks_exact and endpoint_durations_identical and packet_coverage["complete"],
    }


def attach_completed_capture_report(report, completed_capture):
    report["completed_capture"] = completed_capture
    if not completed_capture["all_tracks_exact"] or not completed_capture["endpoint_durations_identical"]:
        if "ce_audio_timeline_fault" not in report["verdicts"]:
            report["verdicts"].append("ce_audio_timeline_fault")
        report["faults"]["audio_timeline"] = True
    if not completed_capture["cfr_packet_coverage_exact"]:
        if "ce_visual_timeline_fault" not in report["verdicts"]:
            report["verdicts"].append("ce_visual_timeline_fault")
        report["faults"]["visual_timeline"] = True
    if len(report["verdicts"]) > 1 and "unknown" in report["verdicts"]:
        report["verdicts"].remove("unknown")


def count_unjoined_late_app_source_backlog(text):
    live_join_sources = {match.group(1) for match in LATE_APP_LIVE_JOIN_SRC_RE.finditer(text)}
    count = 0
    matched_structured_line = False
    for match in LATE_APP_PRIMED_SRC_RE.finditer(text):
        matched_structured_line = True
        if parse_int(match.group(2), 0) >= 1000 and match.group(1) not in live_join_sources:
            count += 1
    if matched_structured_line:
        return count
    return len(LOG_PATTERNS["audio_late_app_source_backlog"].findall(text))


def analyze_log(log_path):
    if not log_path:
        return None
    text = log_path.read_text(encoding="utf-8", errors="replace")
    return analyze_log_text(text)


def analyze_log_text(text):
    lines = text.splitlines()
    counts = {name: len(pattern.findall(text)) for name, pattern in LOG_PATTERNS.items()}
    counts["audio_late_app_source_backlog"] = count_unjoined_late_app_source_backlog(text)
    cadence_window_count = 0

    cadence_metrics = {
        "age_max_us": [],
        "sel_miss": [],
        "stale_unique": [],
        "ancient": [],
        "rep_no_fresh": [],
        "overload_flags": [],
        "wgc_sel_bias_abs_us": [],
        "wgc_shortfall_ms": [],
        "wgc_lead_excess_ms": [],
        "wgc_oldest_ms": [],
        "wgc_buffered_frames": [],
        "wgc_live_rebase_max_ticks": [],
        "wgc_startup_frame_age_us": [],
        "wgc_encoder_limited_drops": [],
        "wgc_phase_error_us": [],
        "wgc_sync_delay_holds": [],
        "wgc_too_new_lead_us": [],
        "wgc_av_delay_ms": [],
        "wgc_sync_delay_source_limited_holds": [],
        "wgc_sync_delay_policy_holds": [],
        "wgc_low_source_bypass": [],
        "wgc_mode_mismatch": [],
        "wgc_source_backtrack": [],
        "wgc_delay_reservoir_low_ticks": [],
        "wgc_delay_residual_avg_abs_us": [],
        "wgc_delay_residual_max_us": [],
        "wgc_delay_residual_p95_us": [],
    }
    for line in lines:
        smoothness_match = WGC_SMOOTHNESS_SUMMARY_RE.search(line)
        if smoothness_match:
            cadence_metrics["wgc_encoder_limited_drops"].append(parse_int(smoothness_match.group(1)))
            cadence_metrics["wgc_phase_error_us"].append(parse_int(smoothness_match.group(4)))
            cadence_metrics["wgc_sync_delay_holds"].append(parse_int(smoothness_match.group(10)))
            cadence_metrics["wgc_too_new_lead_us"].append(parse_int(smoothness_match.group(11)))
            cadence_metrics["wgc_av_delay_ms"].append(parse_float(smoothness_match.group(12)))
            smoothness_extra = WGC_SMOOTHNESS_EXTRA_RE.search(line)
            if smoothness_extra:
                cadence_metrics["wgc_sync_delay_source_limited_holds"].append(parse_int(smoothness_extra.group(1)))
                cadence_metrics["wgc_sync_delay_policy_holds"].append(parse_int(smoothness_extra.group(2)))
            delay_realization = WGC_DELAY_REALIZATION_RE.search(line)
            if delay_realization:
                cadence_metrics["wgc_delay_reservoir_low_ticks"].append(parse_int(delay_realization.group(3)))
                cadence_metrics["wgc_delay_residual_avg_abs_us"].append(parse_int(delay_realization.group(8)))
                cadence_metrics["wgc_delay_residual_max_us"].append(parse_int(delay_realization.group(9)))
                cadence_metrics["wgc_delay_residual_p95_us"].append(parse_int(delay_realization.group(10)))
            cadence_metrics["wgc_low_source_bypass"].append(parse_int(smoothness_match.group(16)))
            cadence_metrics["wgc_mode_mismatch"].append(parse_int(smoothness_match.group(17)))
            cadence_metrics["wgc_source_backtrack"].append(parse_int(smoothness_match.group(18)))
        elif "[WGC CFR SMOOTHNESS " in line:
            smoothness_extra = WGC_SMOOTHNESS_EXTRA_RE.search(line)
            if smoothness_extra:
                cadence_metrics["wgc_sync_delay_source_limited_holds"].append(parse_int(smoothness_extra.group(1)))
                cadence_metrics["wgc_sync_delay_policy_holds"].append(parse_int(smoothness_extra.group(2)))
            delay_realization = WGC_DELAY_REALIZATION_RE.search(line)
            if delay_realization:
                cadence_metrics["wgc_delay_reservoir_low_ticks"].append(parse_int(delay_realization.group(3)))
                cadence_metrics["wgc_delay_residual_avg_abs_us"].append(parse_int(delay_realization.group(8)))
                cadence_metrics["wgc_delay_residual_max_us"].append(parse_int(delay_realization.group(9)))
                cadence_metrics["wgc_delay_residual_p95_us"].append(parse_int(delay_realization.group(10)))
        startup_frame_age_match = WGC_STARTUP_FRAME_AGE_RE.search(line)
        if startup_frame_age_match:
            cadence_metrics["wgc_startup_frame_age_us"].append(parse_int(startup_frame_age_match.group(1)))
        summary_live_rebase_match = WGC_SUMMARY_LIVE_REBASE_RE.search(line)
        if summary_live_rebase_match:
            cadence_metrics["wgc_live_rebase_max_ticks"].append(parse_int(summary_live_rebase_match.group(1)))
        if "[Cadence Health]" not in line:
            continue
        cadence_window_count += 1
        age_max_match = CADENCE_AGEMAX_RE.search(line)
        if age_max_match:
            cadence_metrics["age_max_us"].append(parse_int(age_max_match.group(1)))
        sel_miss_match = CADENCE_SELMISS_RE.search(line)
        if sel_miss_match:
            cadence_metrics["sel_miss"].append(parse_int(sel_miss_match.group(1)))
        stale_unique_match = CADENCE_STALEUNI_RE.search(line)
        if stale_unique_match:
            cadence_metrics["stale_unique"].append(parse_int(stale_unique_match.group(1)))
        ancient_match = CADENCE_ANCIENT_RE.search(line)
        if ancient_match:
            cadence_metrics["ancient"].append(parse_int(ancient_match.group(1)))
        rep_no_fresh_match = CADENCE_REPNOFRESH_RE.search(line)
        if rep_no_fresh_match:
            cadence_metrics["rep_no_fresh"].append(parse_int(rep_no_fresh_match.group(1)))
        overload_match = CADENCE_OVER_RE.search(line)
        if overload_match:
            cadence_metrics["overload_flags"].append(int(overload_match.group(1), 16))
        wgc_sel_bias_match = CADENCE_WGC_SEL_BIAS_RE.search(line)
        if wgc_sel_bias_match:
            cadence_metrics["wgc_sel_bias_abs_us"].append(abs(parse_int(wgc_sel_bias_match.group(1))))
        shortfall_match = CADENCE_SHORTFALL_RE.search(line)
        if shortfall_match:
            cadence_metrics["wgc_shortfall_ms"].append(int(round(parse_float(shortfall_match.group(1)))))
        lead_excess_match = CADENCE_LEAD_EXCESS_RE.search(line)
        if lead_excess_match:
            cadence_metrics["wgc_lead_excess_ms"].append(int(round(parse_float(lead_excess_match.group(1)))))
        oldest_match = CADENCE_OLDEST_RE.search(line)
        if oldest_match:
            cadence_metrics["wgc_oldest_ms"].append(int(round(parse_float(oldest_match.group(1)))))
        buf_now_match = CADENCE_BUFNOW_RE.search(line)
        if buf_now_match:
            cadence_metrics["wgc_buffered_frames"].append(parse_int(buf_now_match.group(1)))
        live_rebase_match = CADENCE_WGC_LIVE_REBASE_RE.search(line)
        if live_rebase_match:
            cadence_metrics["wgc_live_rebase_max_ticks"].append(parse_int(live_rebase_match.group(1)))
        low_source_bypass_match = CADENCE_ENC_LOW_BYPASS_RE.search(line)
        if low_source_bypass_match:
            cadence_metrics["wgc_low_source_bypass"].append(parse_int(low_source_bypass_match.group(2)))
        mode_mismatch_match = CADENCE_MODE_MISMATCH_RE.search(line)
        if mode_mismatch_match:
            cadence_metrics["wgc_mode_mismatch"].append(parse_int(mode_mismatch_match.group(2)))
        source_backtrack_match = CADENCE_SOURCE_BACKTRACK_RE.search(line)
        if source_backtrack_match:
            cadence_metrics["wgc_source_backtrack"].append(parse_int(source_backtrack_match.group(2)))

    return {
        "counts": counts,
        "cadence_windows": cadence_window_count,
        "max_age_max_us": max(cadence_metrics["age_max_us"]) if cadence_metrics["age_max_us"] else 0,
        "max_sel_miss": max(cadence_metrics["sel_miss"]) if cadence_metrics["sel_miss"] else 0,
        "max_stale_unique": max(cadence_metrics["stale_unique"]) if cadence_metrics["stale_unique"] else 0,
        "max_ancient": max(cadence_metrics["ancient"]) if cadence_metrics["ancient"] else 0,
        "max_rep_no_fresh": max(cadence_metrics["rep_no_fresh"]) if cadence_metrics["rep_no_fresh"] else 0,
        "max_wgc_sel_bias_abs_us": max(cadence_metrics["wgc_sel_bias_abs_us"])
        if cadence_metrics["wgc_sel_bias_abs_us"]
        else 0,
        "max_wgc_shortfall_ms": max(cadence_metrics["wgc_shortfall_ms"])
        if cadence_metrics["wgc_shortfall_ms"]
        else 0,
        "max_wgc_lead_excess_ms": max(cadence_metrics["wgc_lead_excess_ms"])
        if cadence_metrics["wgc_lead_excess_ms"]
        else 0,
        "max_wgc_oldest_ms": max(cadence_metrics["wgc_oldest_ms"]) if cadence_metrics["wgc_oldest_ms"] else 0,
        "max_wgc_buffered_frames": max(cadence_metrics["wgc_buffered_frames"])
        if cadence_metrics["wgc_buffered_frames"]
        else 0,
        "max_wgc_live_rebase_ticks": max(cadence_metrics["wgc_live_rebase_max_ticks"])
        if cadence_metrics["wgc_live_rebase_max_ticks"]
        else 0,
        "max_wgc_startup_frame_age_us": max(cadence_metrics["wgc_startup_frame_age_us"])
        if cadence_metrics["wgc_startup_frame_age_us"]
        else 0,
        "max_wgc_encoder_limited_drops": max(cadence_metrics["wgc_encoder_limited_drops"])
        if cadence_metrics["wgc_encoder_limited_drops"]
        else 0,
        "max_wgc_phase_error_us": max(cadence_metrics["wgc_phase_error_us"])
        if cadence_metrics["wgc_phase_error_us"]
        else 0,
        "max_wgc_low_source_bypass": max(cadence_metrics["wgc_low_source_bypass"])
        if cadence_metrics["wgc_low_source_bypass"]
        else 0,
        "max_wgc_mode_mismatch": max(cadence_metrics["wgc_mode_mismatch"])
        if cadence_metrics["wgc_mode_mismatch"]
        else 0,
        "max_wgc_source_backtrack": max(cadence_metrics["wgc_source_backtrack"])
        if cadence_metrics["wgc_source_backtrack"]
        else 0,
        "max_wgc_sync_delay_source_limited_holds": max(cadence_metrics["wgc_sync_delay_source_limited_holds"])
        if cadence_metrics["wgc_sync_delay_source_limited_holds"]
        else 0,
        "max_wgc_sync_delay_policy_holds": max(cadence_metrics["wgc_sync_delay_policy_holds"])
        if cadence_metrics["wgc_sync_delay_policy_holds"]
        else 0,
        "max_wgc_delay_reservoir_low_ticks": max(cadence_metrics["wgc_delay_reservoir_low_ticks"])
        if cadence_metrics["wgc_delay_reservoir_low_ticks"]
        else 0,
        "max_wgc_delay_residual_avg_abs_us": max(cadence_metrics["wgc_delay_residual_avg_abs_us"])
        if cadence_metrics["wgc_delay_residual_avg_abs_us"]
        else 0,
        "max_wgc_delay_residual_us": max(cadence_metrics["wgc_delay_residual_max_us"])
        if cadence_metrics["wgc_delay_residual_max_us"]
        else 0,
        "max_wgc_delay_residual_p95_us": max(cadence_metrics["wgc_delay_residual_p95_us"])
        if cadence_metrics["wgc_delay_residual_p95_us"]
        else 0,
        "saw_encoder_overload": any(flags & 0x1 for flags in cadence_metrics["overload_flags"]),
        "saw_mux_overload": any(flags & 0x2 for flags in cadence_metrics["overload_flags"]),
    }


def read_text_if_exists(path):
    try:
        return path.read_text(encoding="utf-8", errors="replace") if path and path.exists() else ""
    except OSError:
        return ""


def parse_key_value_manifest(path):
    manifest = {}
    for line in read_text_if_exists(path).splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        manifest[key.strip()] = value.strip()
    return manifest


def parse_session_manifest(session_dir):
    return parse_key_value_manifest(session_dir / "session_manifest.txt")


MEDIA_LOG_RE = re.compile(r"^media_(?P<recording_id>[A-Za-z0-9_-]+)_(?P<pid>[0-9]+)\.log$", re.IGNORECASE)


def is_media_log_path(path):
    lower_name = path.name.lower()
    return lower_name == "media.log" or MEDIA_LOG_RE.match(path.name) is not None


def discover_recording_evidence(session_dir):
    manifests_by_log = {}
    for path in sorted(session_dir.glob("recording_*.manifest")):
        manifest = parse_key_value_manifest(path)
        media_name = manifest.get("media_log", "")
        if media_name:
            manifests_by_log[media_name.lower()] = (path, manifest)

    recordings = []
    for path in sorted(session_dir.glob("*.log")):
        if not is_media_log_path(path):
            continue
        match = MEDIA_LOG_RE.match(path.name)
        recording_id = match.group("recording_id") if match else "legacy"
        media_pid = int(match.group("pid")) if match else None
        manifest_path = None
        recording_manifest = {}
        manifest_entry = manifests_by_log.get(path.name.lower())
        if manifest_entry:
            manifest_path, recording_manifest = manifest_entry
            recording_id = recording_manifest.get("recording_id", recording_id)
            try:
                media_pid = int(recording_manifest.get("media_pid", media_pid))
            except (TypeError, ValueError):
                pass
        recordings.append(
            {
                "recording_id": recording_id,
                "media_pid": media_pid,
                "media_log": path,
                "manifest_path": manifest_path,
                "manifest": recording_manifest,
            }
        )
    return recordings


def resolve_recording_evidence(session_dir, recording_id=None, media_log=None):
    recordings = discover_recording_evidence(session_dir)
    if media_log is not None:
        selected_path = Path(media_log)
        if not selected_path.is_absolute():
            selected_path = session_dir / selected_path
        if not selected_path.exists():
            raise ValueError(f"media log not found: {selected_path}")
        for recording in recordings:
            if recording["media_log"].resolve() == selected_path.resolve():
                return recording, recordings
        return {
            "recording_id": recording_id or "explicit",
            "media_pid": None,
            "media_log": selected_path,
            "manifest_path": None,
            "manifest": {},
        }, recordings

    if recording_id:
        matching = [item for item in recordings if item["recording_id"].lower() == recording_id.lower()]
        if len(matching) != 1:
            detail = ", ".join(
                f"{item['recording_id']}:{item['media_log'].name}" for item in recordings
            ) or "none"
            raise ValueError(
                f"recording id {recording_id!r} matched {len(matching)} media logs; available: {detail}"
            )
        return matching[0], recordings

    if len(recordings) > 1:
        detail = ", ".join(f"{item['recording_id']}:{item['media_log'].name}" for item in recordings)
        raise ValueError(
            "multiple recordings exist in this controller session; select one with --recording-id or "
            f"--media-log, or use --all-recordings. Available: {detail}"
        )
    if recordings:
        return recordings[0], recordings
    return {
        "recording_id": None,
        "media_pid": None,
        "media_log": session_dir / "media.log",
        "manifest_path": None,
        "manifest": {},
    }, recordings


def normalize_screen_capture_backend(value):
    normalized = str(value or "").strip().lower().replace("-", "_")
    if normalized in ("dxgiduplication", "dxgi_duplication", "desktop_dup", "duplication"):
        return "dxgi_dup"
    if normalized in ("dxgi_dup", "wgc"):
        return normalized
    return ""


def resolve_screen_capture_backend(manifest, media_evidence):
    configured_backend = normalize_screen_capture_backend(manifest.get("capture_method", ""))
    if configured_backend:
        return configured_backend
    for quality in reversed(media_evidence.get("wgc_quality", [])):
        backend = normalize_screen_capture_backend(quality.get("backend", ""))
        if backend:
            return backend
    for perf in reversed(media_evidence.get("wgc_perf", [])):
        backend = normalize_screen_capture_backend(perf.get("backend", ""))
        if backend:
            return backend
    return "screen_capture"


def parse_wgc_perf_line(line):
    def find_int(pattern, default=0):
        match = re.search(pattern, line)
        return parse_int(match.group(1), default) if match else default

    cb_gap_match = re.search(r"CbGap:\s*(-?\d+)/(-?\d+)us", line)
    min_in_match = re.search(r"MinIn250/500:\s*(\d+)/(\d+)", line)
    min_del_match = re.search(r"MinDel250/500:\s*(\d+)/(\d+)", line)
    km_fail_match = re.search(r"KMFail:\s*(\d+)/(\d+)", line)
    flush_match = re.search(r"Flush:\s*(\d+)/(\d+)", line)
    return {
        "pool_lease_evidence": bool(re.search(r"PoolLease:", line)),
        "input": find_int(r"Input:\s*(\d+)"),
        "queued": find_int(r"Queued:\s*(\d+)"),
        "drop_ingress": find_int(r"DropIngress:\s*(\d+)"),
        "duplicate_timestamps_seen": find_int(r"SrcDupTs:\s*seen=(\d+)"),
        "duplicate_timestamps_skipped": find_int(r"SrcDupTs:\s*seen=\d+\s*skip=(\d+)"),
        "duplicate": find_int(r"Dup:\s*(\d+)"),
        "late": find_int(r"Late:\s*(\d+)"),
        "min_in_250": parse_int(min_in_match.group(1)) if min_in_match else 0,
        "min_in_500": parse_int(min_in_match.group(2)) if min_in_match else 0,
        "min_del_250": parse_int(min_del_match.group(1)) if min_del_match else 0,
        "min_del_500": parse_int(min_del_match.group(2)) if min_del_match else 0,
        "fresh_miss_pm": find_int(r"FreshMiss:\s*(\d+)pm"),
        "buf_min": find_int(r"BufMin:\s*(\d+)"),
        "no_fresh": find_int(r"NoFresh:\s*(\d+)"),
        "cb_gap_avg_us": parse_int(cb_gap_match.group(1)) if cb_gap_match else 0,
        "cb_gap_max_us": parse_int(cb_gap_match.group(2)) if cb_gap_match else 0,
        "copy_us": find_int(r"Copy:\s*(-?\d+)us"),
        "fence_us": find_int(r"Fence:\s*(-?\d+)us"),
        "mux_kb": find_int(r"Mux:\s*(\d+)KB"),
        "overload_flags": int(re.search(r"Overload:\s*0x([0-9A-Fa-f]+)", line).group(1), 16)
        if re.search(r"Overload:\s*0x([0-9A-Fa-f]+)", line)
        else 0,
        "keyed_acquire_fail": parse_int(km_fail_match.group(1)) if km_fail_match else 0,
        "keyed_release_fail": parse_int(km_fail_match.group(2)) if km_fail_match else 0,
        "flush_count": parse_int(flush_match.group(1)) if flush_match else 0,
        "flush_skipped": parse_int(flush_match.group(2)) if flush_match else 0,
        "pool_lease_max": find_int(r"PoolLease:\s*max=(\d+)"),
        "pool_free_min": find_int(r"freeMin=(\d+)"),
        "pool_saturated_drops": find_int(r"satDrop=(\d+)"),
        "pool_overwrite_prevented": find_int(r"overwritePrevented=(\d+)"),
        "pool_lease_mismatch": find_int(r"mismatch=(\d+)"),
        "source_frame_pool_buffers": find_int(r"sourceFramePoolBuffers=(\d+)"),
        "copy_pool_slots": find_int(r"copyPoolSlots=(\d+)"),
        "budget_surfaces": find_int(r"budgetSurfaces=(\d+)"),
        "sync_frames": find_int(r"syncFrames=(\d+)"),
        "extra_frames": find_int(r"extraFrames=(\d+)"),
        "retained_cap": find_int(r"retainedCap=(\d+)"),
        "reserved_free_slots": find_int(r"reservedFree=(\d+)"),
        "safety_slots": find_int(r"safetySlots=(\d+)"),
        "source_format": find_int(r"sourceFmt=(\d+)"),
        "copy_format": find_int(r"copyFmt=(\d+)"),
        "compact_retained": find_int(r"compactRetained=(\d+)"),
        "source_budget_mb": parse_named_float_field(line, "sourceBudgetMB", 0.0),
        "copy_budget_mb": parse_named_float_field(line, "copyBudgetMB", 0.0),
        "source_surface_mb": parse_named_float_field(line, "sourceSurfaceMB", 0.0),
        "copy_surface_mb": parse_named_float_field(line, "copySurfaceMB", 0.0),
        "convert_us": parse_named_int_field(line, "convertUs", 0),
        "ingress_accepted": find_int(r"Ingress:\s*accepted=(\d+)"),
        "ingress_decimated": find_int(r"decimated=(\d+)"),
        "ingress_retained": find_int(r"retained=(\d+)/\d+"),
        "ingress_retained_cap": find_int(r"retained=\d+/(\d+)"),
        "ingress_low_water": find_int(r"lowWater=(\d+)"),
        "ingress_accepted_low_water": find_int(r"accLow=(\d+)"),
        "ingress_accepted_recovery": find_int(r"accRec=(\d+)"),
        "ingress_accepted_source_below": find_int(r"accSrcBelow=(\d+)"),
        "ingress_accepted_healthy": find_int(r"accHealthy=(\d+)"),
        "ingress_accepted_playout_soft": find_int(r"accPlaySoft=(\d+)"),
        "ingress_accepted_playout_credit": find_int(r"accPlayCredit=(\d+)"),
        "ingress_decimated_soft_reserve": find_int(r"decSoft=(\d+)"),
        "ingress_decimated_hard_reserve": find_int(r"decHard=(\d+)"),
        "ingress_decimated_credit": find_int(r"decCredit=(\d+)"),
        "ingress_soft_reserve_pressure": find_int(r"softPress=(\d+)"),
        "ingress_hard_reserve_pressure": find_int(r"hardPress=(\d+)"),
        "ingress_reason": (re.search(r"reason=([A-Za-z0-9_-]+)", line).group(1)
                           if re.search(r"reason=([A-Za-z0-9_-]+)", line)
                           else ""),
        "backend": (re.search(r"Backend:\s*([A-Za-z0-9_-]+)", line).group(1)
                    if re.search(r"Backend:\s*([A-Za-z0-9_-]+)", line)
                    else ""),
        "dup_missed": find_int(r"DupMissed:\s*(\d+)"),
    }


def parse_wgc_quality_line(line):
    payload_match = WGC_QUALITY_RE.search(line)
    payload = payload_match.group(1) if payload_match else line
    values = parse_attribution_payload(payload)
    duplicate_counts = values.get("duplicates", "0/0").split("/", 1)
    return {
        "duplicate_pct": parse_float(values.get("duplicatePct"), 0.0),
        "duplicates": parse_int(duplicate_counts[0] if duplicate_counts else 0),
        "live": parse_int(duplicate_counts[1] if len(duplicate_counts) > 1 else 0),
        "worst_1s_unique": parse_int(values.get("worst1sUnique"), 0),
        "worst_1s_repeats": parse_int(values.get("worst1sRepeats"), 0),
        "worst_1s_emit": parse_int(values.get("worst1sEmit"), 0),
        "limiter": values.get("limiter", ""),
        "source_limited_repeats": parse_int(values.get("sourceLimitedRepeats"), 0),
        "pool_pressure": parse_int(values.get("poolPressure"), 0),
        "free_min": parse_int(values.get("freeMin"), 0),
        "pool_saturated_drops": parse_int(values.get("poolSaturatedDrops"), 0),
        "ingress_hard": parse_int(values.get("ingressHard"), 0),
        "ingress_soft": parse_int(values.get("ingressSoft"), 0),
        "ingress_decimated": parse_int(values.get("ingressDecimated"), 0),
        "pool_pressure_trim": parse_int(values.get("poolPressureTrim"), 0),
        "ingress_accepted_playout_soft": parse_int(values.get("ingressPlaySoft"), 0),
        "ingress_accepted_playout_credit": parse_int(values.get("ingressPlayCredit"), 0),
        "overwrite_prevented": parse_int(values.get("overwritePrevented"), 0),
        "sync_protected_repeats": parse_int(values.get("syncProtectedRepeats"), 0),
        "policy_added_repeats": parse_int(values.get("policyAddedRepeats"), 0),
        "excess_repeats": parse_int(values.get("excessRepeats"), 0),
        "smooth_delay_deficit_us": parse_int(values.get("smoothDelayDeficitUs"), 0),
        "startup_delay_deficit_us": parse_int(values.get("startupDelayDeficitUs"), 0),
        "duplicate_timestamps_seen": parse_int(values.get("dupTsSeen"), 0),
        "duplicate_timestamps_skipped": parse_int(values.get("dupTsSkipped"), 0),
        "encoder_overload": values.get("encoderOverload", "0x0"),
        "mux_backpressure": parse_int(values.get("muxBackpressure"), 0),
        "compact_retained": parse_int(values.get("compactRetained"), 0),
        "source_format": parse_int(values.get("sourceFmt"), 0),
        "retained_format": parse_int(values.get("retainedFmt"), 0),
        "convert_us": parse_int(values.get("convertUs"), 0),
        "backend": values.get("backend", ""),
        "final_av_sync": values.get("finalAvSync", ""),
        "line": line,
    }


def parse_wgc_source_coverage_line(line):
    payload_match = WGC_SOURCE_COVERAGE_RE.search(line)
    payload = payload_match.group(1) if payload_match else line
    values = parse_attribution_payload(payload)
    duplicate_counts = values.get("duplicates", "0/0").split("/", 1)
    return {
        "coverage": values.get("coverage", ""),
        "reason": values.get("reason", ""),
        "best_effort": parse_int(values.get("bestEffort"), 0),
        "output_fps": parse_int(values.get("outputFps"), 0),
        "duplicates": parse_int(duplicate_counts[0] if duplicate_counts else 0),
        "live": parse_int(duplicate_counts[1] if len(duplicate_counts) > 1 else 0),
        "source_limited_repeats": parse_int(values.get("sourceLimitedRepeats"), 0),
        "source_repeat_lower_bound": parse_int(values.get("sourceRepeatLowerBound"), 0),
        "sync_source_repeat_lower_bound": parse_int(values.get("syncSourceRepeatLowerBound"), 0),
        "delivery_repeat_lower_bound": parse_int(values.get("deliveryRepeatLowerBound"), 0),
        "excess_repeats": parse_int(values.get("excessRepeats"), 0),
        "policy_added_repeats": parse_int(values.get("policyAddedRepeats"), 0),
        "policy_no_source_repeats": parse_int(values.get("policyNoSourceRepeats"), 0),
        "clean_encoder_mux": parse_int(values.get("cleanEncoderMux"), 0),
        "clean_pool": parse_int(values.get("cleanPool"), 0),
        "clean_selection": parse_int(values.get("cleanSelection"), 0),
        "encoder_overload": values.get("encoderOverload", "0x0"),
        "mux_backpressure": parse_int(values.get("muxBackpressure"), 0),
        "pool_pressure": parse_int(values.get("poolPressure"), 0),
        "pool_free_min": parse_int(values.get("poolFreeMin"), 0),
        "final_av_sync": values.get("finalAvSync", ""),
        "note": values.get("note", ""),
        "line": line,
    }


def parse_inject_perf_line(line):
    def find_int(pattern, default=0):
        match = re.search(pattern, line)
        return parse_int(match.group(1), default) if match else default

    return {
        "input": find_int(r"Input:\s*(\d+)"),
        "queued": find_int(r"Queued:\s*(\d+)"),
        "drop_full": find_int(r"DropFull:\s*(\d+)"),
        "drop_pace": find_int(r"DropPace:\s*(\d+)"),
        "publication_fps": find_int(r"PubFps:\s*(\d+)"),
        "host_queue": find_int(r"HostQ:\s*(\d+)"),
        "encoder_queue": find_int(r"EncQ:\s*(\d+)"),
        "duplicate": find_int(r"Dup:\s*(\d+)"),
        "late": find_int(r"Late:\s*(\d+)"),
        "trim": find_int(r"Trim:\s*(\d+)"),
        "selection_drop": find_int(r"SelDrop:\s*(\d+)"),
        "deferred": find_int(r"Def:\s*(\d+)"),
        "encode_us": find_int(r"Encode:\s*(-?\d+)us"),
        "fence_us": find_int(r"Fence:\s*(-?\d+)us"),
        "mux_kb": find_int(r"Mux:\s*(\d+)KB"),
        "overload_flags": int(re.search(r"Overload:\s*0x([0-9A-Fa-f]+)", line).group(1), 16)
        if re.search(r"Overload:\s*0x([0-9A-Fa-f]+)", line)
        else 0,
        "line": line,
    }


def parse_attribution_payload(payload):
    values = {}
    for key, value in re.findall(r"([A-Za-z_][A-Za-z0-9_]*)=([^|\s]+)", payload):
        values[key] = value.rstrip(",")
    return values


def parse_recording_window_spec(spec):
    if not spec:
        return None
    match = re.fullmatch(r"\s*([0-9]+(?:\.[0-9]+)?)\s*:\s*([0-9]+(?:\.[0-9]+)?)\s*", spec)
    if not match:
        raise ValueError("recording window must use START:END seconds, for example 25:45")
    start_s = parse_float(match.group(1))
    end_s = parse_float(match.group(2))
    if end_s <= start_s:
        raise ValueError("recording window END must be greater than START")
    return start_s, end_s


def parse_live_start_qpc(media_text):
    match = WGC_LIVE_START_QPC_RE.search(media_text)
    return parse_int(match.group(1), 0) if match else 0


def parse_log_timestamp_us(line):
    match = LOG_LINE_TIMESTAMP_RE.match(line)
    if not match:
        return -1
    timestamp = match.group(1)
    try:
        parsed = datetime.datetime.strptime(timestamp, "%Y-%m-%d %H:%M:%S.%f")
    except ValueError:
        return -1
    epoch = datetime.datetime(parsed.year, parsed.month, parsed.day)
    return int(round((parsed - epoch).total_seconds() * 1000000.0))


def parse_live_start_wall_us(media_text):
    for line in media_text.splitlines():
        if "[A/V START] Shared startup anchor selected" in line or "[EncoderThread] Recording live" in line:
            timestamp_us = parse_log_timestamp_us(line)
            if timestamp_us >= 0:
                return timestamp_us
    return -1


def parse_live_start_qpc_wall_us(media_text):
    for line in media_text.splitlines():
        if "liveStartQpc=" in line:
            timestamp_us = parse_log_timestamp_us(line)
            if timestamp_us >= 0:
                return timestamp_us
    return parse_live_start_wall_us(media_text)


def parse_stop_start_wall_us(media_text):
    for line in media_text.splitlines():
        if "[Media] Stopping recording" in line:
            timestamp_us = parse_log_timestamp_us(line)
            if timestamp_us >= 0:
                return timestamp_us
    return -1


def choose_perf_qpc_us_from_live_start(live_start_qpc, perf_summaries):
    if live_start_qpc <= 0:
        return 0
    perf_min = min((item.get("min_qpc_us", 0) for item in perf_summaries if item.get("min_qpc_us", 0) > 0), default=0)
    perf_max = max((item.get("max_qpc_us", 0) for item in perf_summaries), default=0)
    if perf_min <= 0 or perf_max <= 0:
        return live_start_qpc

    best_value = live_start_qpc
    best_distance = None
    for divisor in (1, 10, 1000, 1000000):
        candidate = live_start_qpc // divisor
        if perf_min <= candidate <= perf_max:
            return candidate
        distance = min(abs(candidate - perf_min), abs(candidate - perf_max))
        if best_distance is None or distance < best_distance:
            best_distance = distance
            best_value = candidate
    return best_value


def build_recording_window_info(media_text, recording_window_spec, perf_summaries):
    parsed = parse_recording_window_spec(recording_window_spec)
    if not parsed:
        return None
    live_start_qpc = parse_live_start_qpc(media_text)
    live_start_qpc_us = choose_perf_qpc_us_from_live_start(live_start_qpc, perf_summaries)
    live_start_wall_us = parse_live_start_wall_us(media_text)
    start_s, end_s = parsed
    start_offset_us = int(round(start_s * 1000000.0))
    end_offset_us = int(round(end_s * 1000000.0))
    if live_start_qpc_us <= 0:
        return {
            "spec": recording_window_spec,
            "start_s": start_s,
            "end_s": end_s,
            "live_start_qpc": live_start_qpc,
            "live_start_qpc_us": 0,
            "start_qpc_us": 0,
            "end_qpc_us": 0,
            "live_start_wall_us": live_start_wall_us,
            "start_wall_us": live_start_wall_us + start_offset_us if live_start_wall_us >= 0 else -1,
            "end_wall_us": live_start_wall_us + end_offset_us if live_start_wall_us >= 0 else -1,
            "active": False,
            "reason": "missing_live_start_qpc",
        }
    return {
        "spec": recording_window_spec,
        "start_s": start_s,
        "end_s": end_s,
        "live_start_qpc": live_start_qpc,
        "live_start_qpc_us": live_start_qpc_us,
        "start_qpc_us": live_start_qpc_us + start_offset_us,
        "end_qpc_us": live_start_qpc_us + end_offset_us,
        "live_start_wall_us": live_start_wall_us,
        "start_wall_us": live_start_wall_us + start_offset_us if live_start_wall_us >= 0 else -1,
        "end_wall_us": live_start_wall_us + end_offset_us if live_start_wall_us >= 0 else -1,
        "active": True,
        "reason": "ok",
    }


def build_full_recording_perf_window_info(media_text, perf_summaries):
    live_start_qpc = parse_live_start_qpc(media_text)
    live_start_qpc_us = choose_perf_qpc_us_from_live_start(live_start_qpc, perf_summaries)
    live_start_wall_us = parse_live_start_qpc_wall_us(media_text)
    stop_wall_us = parse_stop_start_wall_us(media_text)
    if live_start_qpc_us <= 0 or live_start_wall_us < 0 or stop_wall_us <= live_start_wall_us:
        return None
    duration_us = stop_wall_us - live_start_wall_us
    return {
        "spec": "full-recording",
        "start_s": 0.0,
        "end_s": duration_us / 1000000.0,
        "live_start_qpc": live_start_qpc,
        "live_start_qpc_us": live_start_qpc_us,
        "start_qpc_us": live_start_qpc_us,
        "end_qpc_us": live_start_qpc_us + duration_us,
        "live_start_wall_us": live_start_wall_us,
        "start_wall_us": live_start_wall_us,
        "end_wall_us": stop_wall_us,
        "active": True,
        "reason": "derived_selected_recording_bounds",
        "automatic": True,
    }


def filter_media_text_for_recording_window(media_text, recording_window_info):
    if not recording_window_info or not recording_window_info.get("active"):
        return media_text
    start_wall_us = recording_window_info.get("start_wall_us", 0)
    end_wall_us = recording_window_info.get("end_wall_us", 0)
    if start_wall_us < 0 or end_wall_us <= start_wall_us:
        return media_text

    windowed_lines = []
    for line in media_text.splitlines():
        timestamp_us = parse_log_timestamp_us(line)
        if timestamp_us >= 0 and start_wall_us <= timestamp_us < end_wall_us:
            windowed_lines.append(line)
    return "\n".join(windowed_lines)


def merge_window_media_evidence(window_evidence, full_evidence):
    merged = dict(window_evidence)
    for key in (
        "wgc_summary",
        "wgc_quality",
        "wgc_smoothness_summary",
        "inject_summary",
        "inject_source_summary",
        "inject_quality_summary",
        "cfr_phase_lock_summary",
        "inject_contention",
        "final_packet_timelines",
        "final_metadata",
        "post_mux_audio_mismatch_delta_us",
        "post_mux_audio_priming",
        "audio_codec_contracts",
        "audio_finalizations",
        "stop_audio_tracks",
        "stop_audio_sources",
        "stop_app_audio_latency",
    ):
        merged[key] = full_evidence.get(key, [])
    return merged


def update_wgc_smoothness_item_from_line(item, line):
    smoothness_extra = WGC_SMOOTHNESS_EXTRA_RE.search(line)
    if smoothness_extra:
        item.update(
            {
                "sync_delay_source_limited_holds": parse_int(smoothness_extra.group(1)),
                "sync_delay_policy_holds": parse_int(smoothness_extra.group(2)),
                "startup_reserve_frames": parse_int(smoothness_extra.group(3)),
                "startup_reserve_span_us": parse_int(smoothness_extra.group(4)),
                "startup_delay_target_us": parse_int(smoothness_extra.group(5)),
                "startup_reserve_selected": parse_int(smoothness_extra.group(6)),
                "startup_reserve_reason": smoothness_extra.group(7),
            }
        )

    smoothness_buffer = WGC_SMOOTHNESS_BUFFER_RE.search(line)
    if smoothness_buffer:
        groups = smoothness_buffer.groupdict()
        item.update(
            {
                "smoothness_buffer_enabled": parse_int(groups.get("enabled")),
                "smoothness_buffer_target_ms": parse_int(groups.get("target_ms")),
                "smoothness_buffer_actual_frames": parse_int(groups.get("actual_frames")),
                "smoothness_buffer_retained_frames": parse_int(groups.get("retained_frames")),
                "smoothness_buffer_desired_frames": parse_int(groups.get("desired_frames")),
                "smoothness_buffer_delay_ms": parse_float(groups.get("delay_ms")),
                "smoothness_buffer_pool_slots": parse_int(groups.get("pool_slots")),
                "pool_lifetime_evidence": 1 if groups.get("source_buffers") is not None else 0,
                "smoothness_source_frame_pool_buffers": parse_int(groups.get("source_buffers")),
                "smoothness_budget_surfaces": parse_int(groups.get("budget_surfaces")),
                "smoothness_sync_frames": parse_int(groups.get("sync_frames")),
                "smoothness_extra_frames": parse_int(groups.get("extra_frames")),
                "smoothness_retained_frame_cap": parse_int(groups.get("retained_cap")),
                "smoothness_reserved_free_slots": parse_int(groups.get("reserved_free_slots")),
                "smoothness_safety_slots": parse_int(groups.get("safety_slots")),
                "smoothness_retained_cap_trim": parse_int(groups.get("retained_cap_trim")),
                "wgc_ingress_accepted": parse_int(groups.get("ingress_accepted")),
                "wgc_ingress_decimated": parse_int(groups.get("ingress_decimated")),
                "wgc_ingress_accepted_playout_soft": parse_int(groups.get("ingress_play_soft")),
                "wgc_ingress_accepted_playout_credit": parse_int(groups.get("ingress_play_credit")),
                "wgc_ingress_retained_frames": parse_int(groups.get("ingress_retained")),
                "wgc_ingress_retained_cap": parse_int(groups.get("ingress_cap")),
                "wgc_ingress_low_water": parse_int(groups.get("ingress_low_water")),
                "pool_lease_max": parse_int(groups.get("leased_max")),
                "pool_free_now": parse_int(groups.get("free_now")),
                "pool_free_min": parse_int(groups.get("free_min")),
                "pool_pressure_trim": parse_int(groups.get("pool_pressure_trim")),
                "pool_saturated_drops": parse_int(groups.get("pool_saturated_drops")),
                "pool_overwrite_prevented": parse_int(groups.get("overwrite_prevented")),
                "pool_lease_mismatches": parse_int(groups.get("lease_mismatches")),
                "smoothness_buffer_vram_mb": parse_float(groups.get("vram_mb") or "0"),
                "smoothness_buffer_cap_limited": parse_int(groups.get("cap_limited")),
                "smoothness_buffer_reason": groups.get("reason") or "",
            }
        )

    ingress = WGC_SMOOTHNESS_INGRESS_RE.search(line)
    if ingress:
        groups = ingress.groupdict()
        item.update(
            {
                "wgc_ingress_accepted": parse_int(groups.get("accepted")),
                "wgc_ingress_decimated": parse_int(groups.get("decimated")),
                "wgc_ingress_retained_frames": parse_int(groups.get("retained")),
                "wgc_ingress_retained_cap": parse_int(groups.get("cap")),
                "wgc_ingress_low_water": parse_int(groups.get("low_water")),
                "wgc_ingress_accepted_low_water": parse_int(groups.get("acc_low_water")),
                "wgc_ingress_accepted_recovery": parse_int(groups.get("acc_recovery")),
                "wgc_ingress_accepted_source_below": parse_int(groups.get("acc_source_below")),
                "wgc_ingress_accepted_healthy": parse_int(groups.get("acc_healthy")),
                "wgc_ingress_accepted_playout_soft": parse_int(groups.get("acc_play_soft")),
                "wgc_ingress_accepted_playout_credit": parse_int(groups.get("acc_play_credit")),
                "wgc_ingress_decimated_soft_reserve": parse_int(groups.get("dec_soft_reserve")),
                "wgc_ingress_decimated_hard_reserve": parse_int(groups.get("dec_hard_reserve")),
                "wgc_ingress_decimated_credit": parse_int(groups.get("dec_credit")),
                "wgc_ingress_soft_reserve_pressure": parse_int(groups.get("soft_pressure")),
                "wgc_ingress_hard_reserve_pressure": parse_int(groups.get("hard_pressure")),
                "wgc_duplicate_timestamps_seen": parse_int(groups.get("dup_ts_seen")),
                "wgc_duplicate_timestamps_skipped": parse_int(groups.get("dup_ts_skipped")),
                "wgc_ingress_last_reason": groups.get("last_reason") or "",
            }
        )

    source = WGC_SMOOTHNESS_SOURCE_RE.search(line)
    if source:
        groups = source.groupdict()
        item.update(
            {
                "wgc_source_accepted_total": parse_int(groups.get("accepted_total")),
                "wgc_source_cfr_ticks_total": parse_int(groups.get("cfr_ticks_total")),
                "wgc_source_rolling_accepted": parse_int(groups.get("rolling_accepted")),
                "wgc_source_rolling_cfr_ticks": parse_int(groups.get("rolling_cfr_ticks")),
                "wgc_source_rolling_deficit": parse_int(groups.get("rolling_deficit")),
                "wgc_source_rolling_surplus": parse_int(groups.get("rolling_surplus")),
                "wgc_source_last_window_accepted": parse_int(groups.get("last_window_accepted")),
                "wgc_source_last_window_cfr_ticks": parse_int(groups.get("last_window_cfr_ticks")),
                "wgc_source_window_slots": parse_int(groups.get("window_slots")),
            }
        )

    delay_realization = WGC_DELAY_REALIZATION_RE.search(line)
    if delay_realization:
        item.update(
            {
                "delay_reservoir_low_water_frames": parse_int(delay_realization.group(1)),
                "delay_reservoir_target_frames": parse_int(delay_realization.group(2)),
                "delay_reservoir_low_water_ticks": parse_int(delay_realization.group(3)),
                "realized_delay_avg_us": parse_int(delay_realization.group(4)),
                "realized_delay_min_us": parse_int(delay_realization.group(5)),
                "realized_delay_max_us": parse_int(delay_realization.group(6)),
                "delay_residual_avg_signed_us": parse_int(delay_realization.group(7)),
                "delay_residual_avg_abs_us": parse_int(delay_realization.group(8)),
                "delay_residual_max_us": parse_int(delay_realization.group(9)),
                "delay_residual_p95_us": parse_int(delay_realization.group(10)),
                "delay_residual_late_max_us": parse_int(delay_realization.group(11)),
                "delay_residual_early_max_us": parse_int(delay_realization.group(12)),
            }
        )

    floor = WGC_SMOOTHNESS_FLOOR_RE.search(line)
    if floor:
        item.update(
            {
                "smooth_floor_source": floor.group(1),
                "smooth_floor_configured": parse_int(floor.group(2)),
                "smooth_floor_ms": parse_int(floor.group(3)),
                "smooth_floor_requested_us": parse_int(floor.group(4)),
                "smooth_floor_delay_us": parse_int(floor.group(5)),
                "smooth_floor_clamped_by": floor.group(6),
                "smooth_floor_realized_target_us": parse_int(floor.group(7)),
                "smooth_floor_delivery_gap_avg_us": parse_int(floor.group(8)),
                "smooth_floor_delivery_gap_max_us": parse_int(floor.group(9)),
                "smooth_floor_source_jitter_avg_us": parse_int(floor.group(10)),
                "smooth_floor_source_jitter_max_us": parse_int(floor.group(11)),
                "smooth_floor_realized_min_us": parse_int(floor.group(12)),
                "smooth_floor_realized_avg_us": parse_int(floor.group(13)),
                "smooth_floor_realized_max_us": parse_int(floor.group(14)),
                "smooth_floor_residual_late_max_us": parse_int(floor.group(15)),
                "smooth_floor_av_content_delay_active": parse_int(floor.group(16)),
            }
        )

    delay_raw = WGC_DELAY_RAW_RESIDUAL_RE.search(line)
    if delay_raw:
        item.update(
            {
                "raw_residual_avg_signed_us": parse_int(delay_raw.group(1)),
                "raw_residual_avg_abs_us": parse_int(delay_raw.group(2)),
                "raw_residual_max_us": parse_int(delay_raw.group(3)),
                "raw_residual_p95_us": parse_int(delay_raw.group(4)),
                "raw_residual_late_max_us": parse_int(delay_raw.group(5)),
                "raw_residual_early_max_us": parse_int(delay_raw.group(6)),
                "predicted_residual_avg_signed_us": parse_int(delay_raw.group(7)),
                "predicted_residual_avg_abs_us": parse_int(delay_raw.group(8)),
                "predicted_residual_p95_us": parse_int(delay_raw.group(9)),
                "predicted_residual_late_max_us": parse_int(delay_raw.group(10)),
                "raw_minus_predicted_avg_signed_us": parse_int(delay_raw.group(11)),
                "raw_minus_predicted_avg_abs_us": parse_int(delay_raw.group(12)),
                "raw_minus_predicted_max_us": parse_int(delay_raw.group(13)),
            }
        )

    delay_relaxed = WGC_DELAY_RELAXED_RE.search(line)
    if delay_relaxed:
        item.update(
            {
                "delay_relaxed_selections": parse_int(delay_relaxed.group(1)),
                "delay_relaxed_max_us": parse_int(delay_relaxed.group(2)),
                "delay_relaxed_rejected_sync": parse_int(delay_relaxed.group(3)),
                "delay_repeat_cluster_pressure": parse_int(delay_relaxed.group(4)),
                "delay_repeat_cluster_max_ticks": parse_int(delay_relaxed.group(5)),
                "delay_relaxed_better_target": parse_int(delay_relaxed.group(6)),
                "delay_relaxed_repeat_cluster": parse_int(delay_relaxed.group(7)),
                "delay_relaxed_rejected_headroom": parse_int(delay_relaxed.group(8)),
                "delay_relaxed_rejected_cost": parse_int(delay_relaxed.group(9)),
                "delay_soft_late_rejected": parse_int(delay_relaxed.group(10)),
                "delay_soft_late_accepted": parse_int(delay_relaxed.group(11)),
                "delay_older_frame_avoided_repeat": parse_int(delay_relaxed.group(12)),
                "delay_source_limited_repeats": parse_int(delay_relaxed.group(13)),
                "delay_source_recovery_holds": parse_int(delay_relaxed.group(14)),
                "delay_source_recovery_ticks": parse_int(delay_relaxed.group(15)),
            }
        )

    delay_repeat_rescue = WGC_DELAY_REPEAT_RESCUE_RE.search(line)
    if delay_repeat_rescue:
        repeat_fields = {
            "delay_repeat_rescue_success": parse_int(delay_repeat_rescue.group(1)),
            "delay_repeat_rescue_attempts": parse_int(delay_repeat_rescue.group(2)),
            "delay_repeat_rescue_rejected_sync": parse_int(delay_repeat_rescue.group(3)),
            "delay_repeat_rescue_rejected_headroom": parse_int(delay_repeat_rescue.group(4)),
            "delay_repeat_rescue_rejected_cost": parse_int(delay_repeat_rescue.group(5)),
            "delay_repeat_promoted_before_repeat": parse_int(delay_repeat_rescue.group(6)),
            "delay_repeat_promotion_attempts": parse_int(delay_repeat_rescue.group(7)),
            "delay_repeat_promotion_rejected_soft": parse_int(delay_repeat_rescue.group(8)),
            "delay_repeat_safe_after_promotion": parse_int(delay_repeat_rescue.group(9)),
            "delay_repeat_safe_candidate": parse_int(delay_repeat_rescue.group(10)),
            "delay_repeat_no_safe_candidate": parse_int(delay_repeat_rescue.group(11)),
            "delay_repeat_window_healthy": parse_int(delay_repeat_rescue.group(14)),
            "delay_repeat_window_recoverable": parse_int(delay_repeat_rescue.group(15)),
            "delay_repeat_window_source_limited": parse_int(delay_repeat_rescue.group(16)),
            "delay_repeat_state_healthy": parse_int(delay_repeat_rescue.group(17)),
            "delay_repeat_state_recoverable": parse_int(delay_repeat_rescue.group(18)),
            "delay_repeat_state_source_limited": parse_int(delay_repeat_rescue.group(19)),
            "delay_repeat_state_hard_stall": parse_int(delay_repeat_rescue.group(20)),
            "delay_repeat_state_post_stall": parse_int(delay_repeat_rescue.group(21)),
            "delay_post_stall_safe_frames": parse_int(delay_repeat_rescue.group(22)),
            "delay_repeat_reserve_depth_max": parse_int(delay_repeat_rescue.group(23)),
            "delay_repeat_reserve_span_max_us": parse_int(delay_repeat_rescue.group(24)),
        }
        if delay_repeat_rescue.group(12) is not None:
            repeat_fields["delay_repeat_soft_safe_candidate"] = parse_int(delay_repeat_rescue.group(12))
            repeat_fields["delay_repeat_no_soft_safe_candidate"] = parse_int(delay_repeat_rescue.group(13))
        item.update(repeat_fields)

    repeat_named_fields = {
        "delayNearCapAccepted": "delay_near_cap_accepted",
        "delayHardOnlyCandidates": "delay_hard_only_candidates",
        "delaySyncProtectedRepeats": "delay_sync_protected_repeats",
        "delayOldestSoftSafeAgeMax": "delay_oldest_soft_safe_age_max_us",
        "delayUniformCadence": "delay_uniform_cadence",
        "delayUniformHold": "delay_uniform_hold",
        "delayPaceCapTrim": "delay_pace_cap_trim",
    }
    for field_name, key in repeat_named_fields.items():
        value = parse_named_int_field(line, field_name)
        if value is not None:
            item[key] = value

    lower_bound_fields = {
        "delayPostSelectionRejectedSync": "delay_post_selection_rejected_sync",
        "delayPostSelectionRescuedSync": "delay_post_selection_rescued_sync",
        "sourceRepeatLowerBound": "source_repeat_lower_bound",
        "syncSourceRepeatLowerBound": "sync_source_repeat_lower_bound",
        "deliveryRepeatLowerBound": "delivery_repeat_lower_bound",
        "policyNoSourceRepeats": "policy_no_source_repeats",
        "excessRepeats": "excess_repeats",
        "policyAddedRepeats": "policy_added_repeats",
        "excessRepeatClusters": "excess_repeat_clusters",
        "excessRepeatClusterMax": "excess_repeat_cluster_max_ticks",
        "smoothnessNotMaximal": "smoothness_not_maximal",
        "mixedPolicyFault": "mixed_policy_fault",
    }
    saw_lower_bound_tail = False
    for field_name, key in lower_bound_fields.items():
        value = parse_named_int_field(line, field_name)
        if value is None:
            continue
        item[key] = value
        if field_name != "delayPostSelectionRejectedSync":
            saw_lower_bound_tail = True

    if parse_named_int_field(line, "smoothnessNotMaximal") is not None:
        item["wgc_smoothness_verdict_complete"] = 1
    elif saw_lower_bound_tail:
        item["wgc_smoothness_evidence_incomplete"] = 1

    smooth_delay_fields = {
        "smoothTargetDelay": "smooth_target_delay_us",
        "smoothActualDelay": "smooth_actual_delay_us",
        "smoothDelayDeficit": "smooth_delay_deficit_us",
        "startupDelayDeficit": "startup_delay_deficit_us",
        "syncProtectedRepeats": "delay_sync_protected_repeats",
    }
    for field_name, key in smooth_delay_fields.items():
        value = parse_named_int_field(line, field_name)
        if value is not None:
            item[key] = value

    retained_fields = {
        "sourceFmt": ("source_format", parse_named_int_field),
        "retainedFmt": ("retained_format", parse_named_int_field),
        "compactRetained": ("compact_retained", parse_named_int_field),
        "sourceBudgetMB": ("source_budget_mb", parse_named_float_field),
        "copyBudgetMB": ("copy_budget_mb", parse_named_float_field),
        "sourceSurfaceMB": ("source_surface_mb", parse_named_float_field),
        "copySurfaceMB": ("copy_surface_mb", parse_named_float_field),
        "convertUs": ("convert_us", parse_named_int_field),
    }
    for field_name, (key, parser) in retained_fields.items():
        value = parser(line, field_name)
        if value is not None:
            item[key] = value


def parse_media_triage(media_text):
    source_starved = []
    attribution = []
    wgc_perf = []
    wgc_summary = []
    wgc_quality = []
    wgc_source_coverage = []
    wgc_cadence_events = []
    wgc_smoothness_summary = []
    inject_perf = []
    inject_summary = []
    inject_source_summary = []
    inject_quality_summary = []
    inject_repeat_pressure = []
    cfr_phase_lock_summary = []
    inject_contention = []
    app_latency_warnings = []
    final_packet_timelines = []
    final_metadata = []
    post_mux_audio_mismatches = []
    post_mux_audio_priming = []
    audio_codec_contracts = []
    audio_finalizations = []
    stop_audio_tracks = []
    stop_audio_sources = []
    stop_app_audio_latency = []
    zero_drift_warnings = []
    packet_mismatch_warnings = 0
    for line in media_text.splitlines():
        source_match = WGC_SOURCE_STARVED_RE.search(line)
        if source_match:
            source_starved.append(
                {
                    "duration_ms": parse_int(source_match.group(1)),
                    "output_ticks": parse_int(source_match.group(2)),
                    "duplicate_ticks": parse_int(source_match.group(3)),
                    "min_input_fps": parse_int(source_match.group(4)),
                    "min_delivered_fps": parse_int(source_match.group(5)),
                    "fresh_miss_pm": parse_int(source_match.group(6)),
                    "min_buffered_frames": parse_int(source_match.group(7)),
                    "line": line,
                }
            )
        attribution_match = WGC_ATTRIBUTION_RE.search(line)
        if attribution_match:
            attribution.append(parse_attribution_payload(attribution_match.group(1)))
        if WGC_PERF_RE.search(line):
            wgc_perf.append(parse_wgc_perf_line(line))
        if WGC_QUALITY_RE.search(line):
            wgc_quality.append(parse_wgc_quality_line(line))
        if WGC_SOURCE_COVERAGE_RE.search(line):
            wgc_source_coverage.append(parse_wgc_source_coverage_line(line))
        cadence_event_match = WGC_CADENCE_EVENT_RE.search(line)
        if cadence_event_match:
            event = parse_attribution_payload(cadence_event_match.group(2))
            event["mode"] = cadence_event_match.group(1)
            event["line"] = line
            wgc_cadence_events.append(event)
        if INJECT_PERF_RE.search(line):
            inject_perf.append(parse_inject_perf_line(line))
        repeat_pressure_match = INJECT_CFR_REPEAT_PRESSURE_RE.search(line)
        if repeat_pressure_match:
            values = parse_attribution_payload(repeat_pressure_match.group(1))
            inject_repeat_pressure.append(
                {
                    "duplicate": parse_int(values.get("dup")),
                    "source_limited": parse_int(values.get("srcLimited")),
                    "target_select": parse_int(values.get("targetSelect")),
                    "target_superseded": parse_int(values.get("targetSuperseded")),
                    "target_hold": parse_int(values.get("targetHold")),
                    "hold_with_candidate": parse_int(values.get("holdWithCandidate")),
                    "tick_emit": parse_int(values.get("tickEmit")),
                    "unique": parse_int(values.get("unique")),
                    "source_fps": parse_float(values.get("sourceFps")),
                    "overload_flags": parse_base0_int(values.get("overload")),
                    "timestamp_us": parse_log_timestamp_us(line),
                    "line": line,
                }
            )
        contention_match = INJECT_CONTENTION_RE.search(line)
        if contention_match:
            inject_contention.append(
                {
                    "capture_lock": parse_int(contention_match.group(1)),
                    "cpu_lease": parse_int(contention_match.group(2)),
                    "gpu_busy": parse_int(contention_match.group(3)),
                    "ring_full": parse_int(contention_match.group(4)),
                    "event_signals": parse_int(contention_match.group(5)),
                    "publication_to_ingest_avg_us": parse_int(contention_match.group(6)),
                    "publication_to_ingest_max_us": parse_int(contention_match.group(7)),
                    "timestamp_us": parse_log_timestamp_us(line),
                    "is_summary": "SUMMARY" in line,
                    "line": line,
                }
            )
        if LOG_PATTERNS["audio_app_latency_elevated"].search(line):
            app_latency_warnings.append({"timestamp_us": parse_log_timestamp_us(line), "line": line})
        summary_match = WGC_SUMMARY_RE.search(line)
        if summary_match:
            wgc_summary.append(
                {
                    "live": parse_int(summary_match.group(1)),
                    "duplicate": parse_int(summary_match.group(2)),
                    "duplicate_pct": parse_float(summary_match.group(3)),
                    "dup_src": parse_int(summary_match.group(4)),
                    "dup_def": parse_int(summary_match.group(5)),
                    "dup_timer": parse_int(summary_match.group(6)),
                    "dup_drain": parse_int(summary_match.group(7)),
                    "source_limited_repeats": parse_int(summary_match.group(8)),
                    "starved_episodes": parse_int(summary_match.group(9)),
                    "longest_ms": parse_int(summary_match.group(10)),
                    "longest_dup_ticks": parse_int(summary_match.group(11)),
                    # True contiguous freeze (held-frame run); falls back to 0 on older logs that
                    # predate the metric. longest_ms/longest_dup above are episode-scoped and overstate
                    # a freeze, so prefer this for the real worst-case held-frame duration.
                    "longest_contiguous_dup_ticks": parse_int(summary_match.group(12)),
                    "longest_contiguous_dup_ms": parse_int(summary_match.group(13)),
                    "worst_input_fps": parse_int(summary_match.group(14)),
                    "worst_delivered_fps": parse_int(summary_match.group(15)),
                    "line": line,
                }
            )
        smoothness_match = WGC_SMOOTHNESS_SUMMARY_RE.search(line)
        if smoothness_match:
            smoothness_extra = WGC_SMOOTHNESS_EXTRA_RE.search(line)
            delay_realization = WGC_DELAY_REALIZATION_RE.search(line)
            delay_raw = WGC_DELAY_RAW_RESIDUAL_RE.search(line)
            delay_relaxed = WGC_DELAY_RELAXED_RE.search(line)
            delay_repeat_rescue = WGC_DELAY_REPEAT_RESCUE_RE.search(line)
            delay_post_reject = WGC_DELAY_POST_REJECT_RE.search(line)
            smoothness_lower_bound = WGC_SMOOTHNESS_LOWER_BOUND_RE.search(line)
            wgc_smoothness_summary.append(
                {
                    "encoder_limited_drops": parse_int(smoothness_match.group(1)),
                    "live": wgc_summary[-1]["live"] if wgc_summary else 0,
                    "max_drop_ticks": parse_int(smoothness_match.group(2)),
                    "cadence_events": parse_int(smoothness_match.group(3)),
                    "phase_error_max_us": parse_int(smoothness_match.group(4)),
                    "shortfall_max_ms": parse_float(smoothness_match.group(5)),
                    "stale_debt_drops": parse_int(smoothness_match.group(6)),
                    "live_rebase_total": parse_int(smoothness_match.group(7)),
                    "live_rebase_max_ticks": parse_int(smoothness_match.group(8)),
                    "too_new_repeats": parse_int(smoothness_match.group(9)),
                    "sync_delay_holds": parse_int(smoothness_match.group(10)),
                    "too_new_lead_max_us": parse_int(smoothness_match.group(11)),
                    "av_delay_ms": parse_float(smoothness_match.group(12)),
                    "startup_delay_ms": parse_float(smoothness_match.group(13)),
                    "schedule_offset_us": parse_int(smoothness_match.group(14)),
                    "effective_delay_ms": parse_float(smoothness_match.group(15)),
                    "low_source_bypass": parse_int(smoothness_match.group(16)),
                    "mode_mismatch": parse_int(smoothness_match.group(17)),
                    "source_backtrack": parse_int(smoothness_match.group(18)),
                    "sync_delay_source_limited_holds": parse_int(smoothness_extra.group(1)) if smoothness_extra else 0,
                    "sync_delay_policy_holds": parse_int(smoothness_extra.group(2)) if smoothness_extra else 0,
                    "startup_reserve_frames": parse_int(smoothness_extra.group(3)) if smoothness_extra else 0,
                    "startup_reserve_span_us": parse_int(smoothness_extra.group(4)) if smoothness_extra else 0,
                    "startup_delay_target_us": parse_int(smoothness_extra.group(5)) if smoothness_extra else 0,
                    "startup_reserve_selected": parse_int(smoothness_extra.group(6)) if smoothness_extra else 0,
                    "startup_reserve_reason": smoothness_extra.group(7) if smoothness_extra else "",
                    "delay_reservoir_low_water_frames": parse_int(delay_realization.group(1)) if delay_realization else 0,
                    "delay_reservoir_target_frames": parse_int(delay_realization.group(2)) if delay_realization else 0,
                    "delay_reservoir_low_water_ticks": parse_int(delay_realization.group(3)) if delay_realization else 0,
                    "realized_delay_avg_us": parse_int(delay_realization.group(4)) if delay_realization else 0,
                    "realized_delay_min_us": parse_int(delay_realization.group(5)) if delay_realization else 0,
                    "realized_delay_max_us": parse_int(delay_realization.group(6)) if delay_realization else 0,
                    "delay_residual_avg_signed_us": parse_int(delay_realization.group(7)) if delay_realization else 0,
                    "delay_residual_avg_abs_us": parse_int(delay_realization.group(8)) if delay_realization else 0,
                    "delay_residual_max_us": parse_int(delay_realization.group(9)) if delay_realization else 0,
                    "delay_residual_p95_us": parse_int(delay_realization.group(10)) if delay_realization else 0,
                    "delay_residual_late_max_us": parse_int(delay_realization.group(11)) if delay_realization else 0,
                    "delay_residual_early_max_us": parse_int(delay_realization.group(12)) if delay_realization else 0,
                    "raw_residual_avg_signed_us": parse_int(delay_raw.group(1)) if delay_raw else 0,
                    "raw_residual_avg_abs_us": parse_int(delay_raw.group(2)) if delay_raw else 0,
                    "raw_residual_max_us": parse_int(delay_raw.group(3)) if delay_raw else 0,
                    "raw_residual_p95_us": parse_int(delay_raw.group(4)) if delay_raw else 0,
                    "raw_residual_late_max_us": parse_int(delay_raw.group(5)) if delay_raw else 0,
                    "raw_residual_early_max_us": parse_int(delay_raw.group(6)) if delay_raw else 0,
                    "predicted_residual_avg_signed_us": parse_int(delay_raw.group(7)) if delay_raw else 0,
                    "predicted_residual_avg_abs_us": parse_int(delay_raw.group(8)) if delay_raw else 0,
                    "predicted_residual_p95_us": parse_int(delay_raw.group(9)) if delay_raw else 0,
                    "predicted_residual_late_max_us": parse_int(delay_raw.group(10)) if delay_raw else 0,
                    "raw_minus_predicted_avg_signed_us": parse_int(delay_raw.group(11)) if delay_raw else 0,
                    "raw_minus_predicted_avg_abs_us": parse_int(delay_raw.group(12)) if delay_raw else 0,
                    "raw_minus_predicted_max_us": parse_int(delay_raw.group(13)) if delay_raw else 0,
                    "delay_relaxed_selections": parse_int(delay_relaxed.group(1)) if delay_relaxed else 0,
                    "delay_relaxed_max_us": parse_int(delay_relaxed.group(2)) if delay_relaxed else 0,
                    "delay_relaxed_rejected_sync": parse_int(delay_relaxed.group(3)) if delay_relaxed else 0,
                    "delay_repeat_cluster_pressure": parse_int(delay_relaxed.group(4)) if delay_relaxed else 0,
                    "delay_repeat_cluster_max_ticks": parse_int(delay_relaxed.group(5)) if delay_relaxed else 0,
                    "delay_relaxed_better_target": parse_int(delay_relaxed.group(6)) if delay_relaxed else 0,
                    "delay_relaxed_repeat_cluster": parse_int(delay_relaxed.group(7)) if delay_relaxed else 0,
                    "delay_relaxed_rejected_headroom": parse_int(delay_relaxed.group(8)) if delay_relaxed else 0,
                    "delay_relaxed_rejected_cost": parse_int(delay_relaxed.group(9)) if delay_relaxed else 0,
                    "delay_soft_late_rejected": parse_int(delay_relaxed.group(10)) if delay_relaxed else 0,
                    "delay_soft_late_accepted": parse_int(delay_relaxed.group(11)) if delay_relaxed else 0,
                    "delay_older_frame_avoided_repeat": parse_int(delay_relaxed.group(12)) if delay_relaxed else 0,
                    "delay_source_limited_repeats": parse_int(delay_relaxed.group(13)) if delay_relaxed else 0,
                    "delay_source_recovery_holds": parse_int(delay_relaxed.group(14)) if delay_relaxed else 0,
                    "delay_source_recovery_ticks": parse_int(delay_relaxed.group(15)) if delay_relaxed else 0,
                    "delay_repeat_rescue_success": parse_int(delay_repeat_rescue.group(1))
                    if delay_repeat_rescue
                    else 0,
                    "delay_repeat_rescue_attempts": parse_int(delay_repeat_rescue.group(2))
                    if delay_repeat_rescue
                    else 0,
                    "delay_repeat_rescue_rejected_sync": parse_int(delay_repeat_rescue.group(3))
                    if delay_repeat_rescue
                    else 0,
                    "delay_repeat_rescue_rejected_headroom": parse_int(delay_repeat_rescue.group(4))
                    if delay_repeat_rescue
                    else 0,
                    "delay_repeat_rescue_rejected_cost": parse_int(delay_repeat_rescue.group(5))
                    if delay_repeat_rescue
                    else 0,
                    "delay_repeat_promoted_before_repeat": parse_int(delay_repeat_rescue.group(6))
                    if delay_repeat_rescue
                    else 0,
                    "delay_repeat_promotion_attempts": parse_int(delay_repeat_rescue.group(7))
                    if delay_repeat_rescue
                    else 0,
                    "delay_repeat_promotion_rejected_soft": parse_int(delay_repeat_rescue.group(8))
                    if delay_repeat_rescue
                    else 0,
                    "delay_repeat_safe_after_promotion": parse_int(delay_repeat_rescue.group(9))
                    if delay_repeat_rescue
                    else 0,
                    "delay_repeat_safe_candidate": parse_int(delay_repeat_rescue.group(10))
                    if delay_repeat_rescue
                    else 0,
                    "delay_repeat_no_safe_candidate": parse_int(delay_repeat_rescue.group(11))
                    if delay_repeat_rescue
                    else 0,
                    "delay_repeat_window_healthy": parse_int(delay_repeat_rescue.group(12))
                    if delay_repeat_rescue
                    else 0,
                    "delay_repeat_window_recoverable": parse_int(delay_repeat_rescue.group(13))
                    if delay_repeat_rescue
                    else 0,
                    "delay_repeat_window_source_limited": parse_int(delay_repeat_rescue.group(14))
                    if delay_repeat_rescue
                    else 0,
                    "delay_repeat_reserve_depth_max": parse_int(delay_repeat_rescue.group(15))
                    if delay_repeat_rescue
                    else 0,
                    "delay_repeat_reserve_span_max_us": parse_int(delay_repeat_rescue.group(16))
                    if delay_repeat_rescue
                    else 0,
                    "delay_post_selection_rejected_sync": parse_int(delay_post_reject.group(1))
                    if delay_post_reject
                    else 0,
                    "delay_post_selection_rescued_sync": parse_int(smoothness_lower_bound.group(2))
                    if smoothness_lower_bound
                    else 0,
                    "source_repeat_lower_bound": parse_int(smoothness_lower_bound.group(3))
                    if smoothness_lower_bound
                    else 0,
                    "excess_repeats": parse_int(smoothness_lower_bound.group(4)) if smoothness_lower_bound else 0,
                    "policy_added_repeats": parse_int(smoothness_lower_bound.group(5))
                    if smoothness_lower_bound
                    else 0,
                    "excess_repeat_clusters": parse_int(smoothness_lower_bound.group(6))
                    if smoothness_lower_bound
                    else 0,
                    "excess_repeat_cluster_max_ticks": parse_int(smoothness_lower_bound.group(7))
                    if smoothness_lower_bound
                    else 0,
                    "smoothness_not_maximal": parse_int(smoothness_lower_bound.group(8))
                    if smoothness_lower_bound
                    else 0,
                    "line": line,
                }
            )
            update_wgc_smoothness_item_from_line(wgc_smoothness_summary[-1], line)
        elif wgc_smoothness_summary and "[WGC CFR SMOOTHNESS " in line:
            update_wgc_smoothness_item_from_line(wgc_smoothness_summary[-1], line)
        inject_summary_match = INJECT_CFR_SUMMARY_RE.search(line)
        if inject_summary_match:
            inject_summary.append(
                {
                    "live": parse_int(inject_summary_match.group(1)),
                    "duplicate": parse_int(inject_summary_match.group(2)),
                    "duplicate_pct": parse_float(inject_summary_match.group(3)),
                    "dup_src": parse_int(inject_summary_match.group(4)),
                    "dup_def": parse_int(inject_summary_match.group(5)),
                    "dup_timer": parse_int(inject_summary_match.group(6)),
                    "dup_drain": parse_int(inject_summary_match.group(7)),
                    "fresh_catchup": parse_int(inject_summary_match.group(8)),
                    "repeat_catchup": parse_int(inject_summary_match.group(9)),
                    "stale_trim": parse_int(inject_summary_match.group(10)),
                    "recovery_active": parse_int(inject_summary_match.group(11) or "0"),
                    "recovery_episodes": parse_int(inject_summary_match.group(12) or "0"),
                    "line": line,
                }
            )
        inject_source_match = INJECT_CFR_SOURCE_RE.search(line)
        if inject_source_match:
            inject_source_summary.append(
                {
                    "source_fps_min": parse_float(inject_source_match.group(1)),
                    "source_fps_max": parse_float(inject_source_match.group(2)),
                    "jitter_max_us": parse_int(inject_source_match.group(3)),
                    "selection_max_us": parse_int(inject_source_match.group(4)),
                    "line": line,
                }
            )
        inject_quality_match = INJECT_CFR_QUALITY_SUMMARY_RE.search(line)
        if inject_quality_match:
            inject_quality_summary.append(
                {
                    "target_select": parse_int(inject_quality_match.group(1)),
                    "superseded": parse_int(inject_quality_match.group(2)),
                    "target_hold": parse_int(inject_quality_match.group(3)),
                    "hold_with_candidate": parse_int(inject_quality_match.group(4)),
                    "buffer_cap_trim": parse_int(inject_quality_match.group(5)),
                    "target_residual_max_us": parse_int(inject_quality_match.group(6)),
                    "line": line,
                }
            )
        phase_lock_match = CFR_PHASE_LOCK_SUMMARY_RE.search(line)
        if phase_lock_match:
            cfr_phase_lock_summary.append(
                {
                    "backend": phase_lock_match.group(1).lower(),
                    "enabled": parse_int(phase_lock_match.group(2)),
                    "locked": parse_int(phase_lock_match.group(3)),
                    "offset_us": parse_int(phase_lock_match.group(4)),
                    "stable": parse_int(phase_lock_match.group(5)),
                    "unstable": parse_int(phase_lock_match.group(6)),
                    "acquisitions": parse_int(phase_lock_match.group(7)),
                    "rephases": parse_int(phase_lock_match.group(8)),
                    "releases": parse_int(phase_lock_match.group(9)),
                    "multiplier": parse_int(phase_lock_match.group(10)),
                    "line": line,
                }
            )
        packet_match = FINAL_PACKET_TIMELINE_RE.search(line)
        if packet_match:
            final_packet_timelines.append(
                {
                    "target_us": parse_int(packet_match.group(1)),
                    "video_end_us": parse_int(packet_match.group(2)),
                    "audio_min_end_us": parse_int(packet_match.group(3)),
                    "audio_max_end_us": parse_int(packet_match.group(4)),
                    "max_packet_delta_us": parse_int(packet_match.group(5)),
                    "audio_past_target": parse_int(packet_match.group(6)),
                    "line": line,
                }
            )
        metadata_match = FINAL_METADATA_RE.search(line)
        if metadata_match:
            final_metadata.append(
                {
                    "target_us": parse_int(metadata_match.group(1)),
                    "video_us": parse_int(metadata_match.group(2)),
                    "audio_min_us": parse_int(metadata_match.group(3)),
                    "audio_max_us": parse_int(metadata_match.group(4)),
                    "max_delta_us": parse_int(metadata_match.group(5)),
                    "encoder_overload": parse_int(metadata_match.group(6)),
                    "mux_overload": parse_int(metadata_match.group(7)),
                    "backpressure": parse_int(metadata_match.group(8)),
                    "line": line,
                }
            )
        post_mux_match = POST_MUX_AUDIO_MISMATCH_RE.search(line)
        if post_mux_match:
            post_mux_audio_mismatches.append(parse_int(post_mux_match.group(1)))
        post_mux_priming_match = POST_MUX_AUDIO_PRIMING_RE.search(line)
        if post_mux_priming_match:
            post_mux_audio_priming.append(
                {
                    "max_delta_us": parse_int(post_mux_priming_match.group(1)),
                    "priming_tolerance_us": parse_int(post_mux_priming_match.group(2)),
                    "rounding_tolerance_us": parse_int(post_mux_priming_match.group(3)),
                    "line": line,
                }
            )
        codec_contract_match = AUDIO_CODEC_CONTRACT_RE.search(line)
        if codec_contract_match:
            audio_codec_contracts.append(
                {
                    "encoder": codec_contract_match.group(1),
                    "codec_id": parse_int(codec_contract_match.group(2)),
                    "sample_format": codec_contract_match.group(3),
                    "sample_rate": parse_int(codec_contract_match.group(4)),
                    "channels": parse_int(codec_contract_match.group(5)),
                    "channel_mask": int(codec_contract_match.group(6), 16),
                    "raw_bit_depth": parse_int(codec_contract_match.group(7)),
                    "frame_size": parse_int(codec_contract_match.group(8)),
                    "capabilities": int(codec_contract_match.group(9), 16),
                    "initial_padding": parse_int(codec_contract_match.group(10)),
                    "final_policy": parse_int(codec_contract_match.group(11)),
                    "requires_codec_delay": parse_int(codec_contract_match.group(12)),
                    "requires_discard_padding": parse_int(codec_contract_match.group(13)),
                    "line": line,
                }
            )
        finalization_match = AUDIO_FINALIZATION_RE.search(line)
        if finalization_match:
            audio_finalizations.append(
                {
                    "encoder": finalization_match.group(1),
                    "stream": parse_int(finalization_match.group(2)),
                    "target_samples": parse_int(finalization_match.group(3)),
                    "input_samples": parse_int(finalization_match.group(4)),
                    "expected_silence_samples": parse_int(finalization_match.group(5)),
                    "submitted_samples": parse_int(finalization_match.group(6)),
                    "priming_samples": parse_int(finalization_match.group(7)),
                    "terminal_padding_samples": parse_int(finalization_match.group(8)),
                    "packet_endpoint_samples": parse_int(finalization_match.group(9)),
                    "expected_decoded_samples": parse_int(finalization_match.group(10)),
                    "packet_count": parse_int(finalization_match.group(11)),
                    "packet_bytes": parse_int(finalization_match.group(12)),
                    "durationless_packets": parse_int(finalization_match.group(13)),
                    "drain_eof": parse_int(finalization_match.group(14)),
                    "protocol_error": parse_int(finalization_match.group(15)),
                    "line": line,
                }
            )
        stop_track_match = STOP_AUDIO_TRACK_RE.search(line)
        if stop_track_match:
            sources = [
                parse_int(part.strip())
                for part in stop_track_match.group(6).split(",")
                if part.strip()
            ]
            stop_audio_tracks.append(
                {
                    "track": parse_int(stop_track_match.group(1)),
                    "encoded_samples": parse_int(stop_track_match.group(2)),
                    "expected_samples": parse_int(stop_track_match.group(3)),
                    "diff_samples": parse_int(stop_track_match.group(4)),
                    "diff_ms": parse_float(stop_track_match.group(5)),
                    "sources": sources,
                    "line": line,
                }
            )
        stop_source_match = STOP_AUDIO_SOURCE_RE.search(line)
        if stop_source_match:
            stop_audio_sources.append(
                {
                    "source": parse_int(stop_source_match.group(1)),
                    "track": parse_int(stop_source_match.group(2)),
                    "encoded_samples": parse_int(stop_source_match.group(3)),
                    "pad_samples": parse_int(stop_source_match.group(4)),
                    "packet_gap_samples": parse_int(stop_source_match.group(5)),
                    "late_join_suppressed_samples": parse_int(stop_source_match.group(6)),
                    "late_join_preserved_samples": parse_int(stop_source_match.group(7)),
                    "ring_peak_samples": parse_int(stop_source_match.group(8)),
                    "ring_underruns": parse_int(stop_source_match.group(9)),
                    "process": stop_source_match.group(10) or "",
                    "line": line,
                }
            )
        stop_latency_match = STOP_AUDIO_LATENCY_RE.search(line)
        if stop_latency_match:
            drain_match = re.search(r"(?:drainObservations|drainingSamples)=(\d+)/(\d+)", line)
            queue_match = re.search(r"queueOverrun=(\d+)/(\d+)", line)
            trim_match = re.search(r"trims\(lat=(\d+) normal=(\d+) cat=(\d+)/(\d+)\)", line)
            stop_app_audio_latency.append(
                {
                    "source": parse_int(stop_latency_match.group(1)),
                    "track": parse_int(stop_latency_match.group(2)),
                    "avg_ms": parse_float(stop_latency_match.group(3)),
                    "max_ms": parse_int(stop_latency_match.group(4)),
                    "target_avg_ms": parse_named_float_field(line, "targetAvg"),
                    "excess_avg_ms": parse_named_float_field(line, "excessAvg"),
                    "excess_max_ms": parse_named_int_field(line, "excessMax"),
                    "drain_observations": parse_int(drain_match.group(1)) if drain_match else 0,
                    "observation_count": parse_int(drain_match.group(2)) if drain_match else 0,
                    "live_observations": parse_named_int_field(
                        line, "liveObservations", parse_int(drain_match.group(2)) if drain_match else 0
                    ),
                    "phase_split": "liveObservations=" in line,
                    "stop_drain_observations": parse_named_int_field(line, "stopDrainObservations", 0),
                    "stop_drain_avg_ms": parse_named_float_field(line, "stopDrainAvg", 0.0),
                    "stop_drain_max_ms": parse_named_int_field(line, "stopDrainMax", 0),
                    "transitions": parse_named_int_field(line, "transitions", 0),
                    "max_comp_percent": parse_named_float_field(line, "maxComp", 0.0),
                    "queue_overrun_packets": parse_int(queue_match.group(1)) if queue_match else 0,
                    "queue_overrun_frames": parse_int(queue_match.group(2)) if queue_match else 0,
                    "underruns": parse_named_int_field(line, "underruns", 0),
                    "latency_trim_samples": parse_int(trim_match.group(1)) if trim_match else 0,
                    "normal_trim_samples": parse_int(trim_match.group(2)) if trim_match else 0,
                    "catastrophic_resync_events": parse_int(trim_match.group(3)) if trim_match else 0,
                    "catastrophic_resync_samples": parse_int(trim_match.group(4)) if trim_match else 0,
                    "line": line,
                }
            )
        zero_drift_match = ZERO_DRIFT_WARNING_RE.search(line)
        if zero_drift_match:
            zero_drift_warnings.append(
                {
                    "track": parse_int(zero_drift_match.group(1)),
                    "residual_samples": parse_int(zero_drift_match.group(2)),
                    "residual_us": parse_int(zero_drift_match.group(3)),
                    "target_samples": parse_int(zero_drift_match.group(4)),
                    "cursor_samples": parse_int(zero_drift_match.group(5)),
                    "line": line,
                }
            )
        if PACKET_MISMATCH_RE.search(line):
            packet_mismatch_warnings += 1
    return {
        "source_starved_episodes": source_starved,
        "wgc_attribution": attribution,
        "wgc_perf": wgc_perf,
        "wgc_summary": wgc_summary,
        "wgc_quality": wgc_quality,
        "wgc_source_coverage": wgc_source_coverage,
        "wgc_cadence_events": wgc_cadence_events,
        "wgc_smoothness_summary": wgc_smoothness_summary,
        "inject_perf": inject_perf,
        "inject_summary": inject_summary,
        "inject_source_summary": inject_source_summary,
        "inject_quality_summary": inject_quality_summary,
        "inject_repeat_pressure": inject_repeat_pressure,
        "cfr_phase_lock_summary": cfr_phase_lock_summary,
        "inject_contention": inject_contention,
        "app_latency_warnings": app_latency_warnings,
        "final_packet_timelines": final_packet_timelines,
        "final_metadata": final_metadata,
        "post_mux_audio_mismatch_delta_us": post_mux_audio_mismatches,
        "post_mux_audio_priming": post_mux_audio_priming,
        "audio_codec_contracts": audio_codec_contracts,
        "audio_finalizations": audio_finalizations,
        "stop_audio_tracks": stop_audio_tracks,
        "stop_audio_sources": stop_audio_sources,
        "stop_app_audio_latency": stop_app_audio_latency,
        "zero_drift_warnings": zero_drift_warnings,
        "packet_mismatch_warnings": packet_mismatch_warnings,
    }


def parse_hook_triage(session_dir):
    gaps = []
    external_overlay_lines = []
    present_stalled_lines = []
    crash_events = []
    for path in sorted(session_dir.glob("*.log")):
        if is_media_log_path(path):
            continue
        text = read_text_if_exists(path)
        for line in text.splitlines():
            gap_match = PRESENT_HEARTBEAT_GAP_RE.search(line)
            if gap_match:
                gaps.append(
                    {
                        "path": str(path),
                        "gap_ms": parse_float(gap_match.group(1)),
                        "timestamp_us": parse_log_timestamp_us(line),
                        "line": line,
                    }
                )
            if "Present STALLED" in line:
                present_stalled_lines.append({"path": str(path), "line": line})
            if EXTERNAL_OVERLAY_RE.search(line) and ("overlay" in line.lower() or "streamline" in line.lower()):
                external_overlay_lines.append({"path": str(path), "line": line})
            if CRASH_LOG_RE.search(line):
                crash_events.append({"path": str(path), "line": line})
    return {
        "present_gaps": gaps,
        "present_stalled_lines": present_stalled_lines,
        "external_overlay_lines": external_overlay_lines[:20],
        "crash_events": crash_events[:20],
    }


def parse_perf_csvs(session_dir, recording_window=None, live_source_only=False):
    summaries = []
    window_bounds = None
    if recording_window and recording_window.get("active"):
        window_bounds = (recording_window["start_qpc_us"], recording_window["end_qpc_us"])
    for path in sorted(session_dir.glob("perf_metrics_*.csv")):
        try:
            with path.open("r", encoding="utf-8", errors="replace", newline="") as handle:
                rows = list(csv.DictReader(handle))
        except OSError:
            continue
        live_source_bounds = None
        live_source_filter_kind = ""
        if live_source_only:
            phase_live_rows = [
                row_index
                for row_index, row in enumerate(rows)
                if parse_int(row.get("capture_phase"), -1) == 2
            ]
            first_live_row = phase_live_rows[0] if phase_live_rows else None
            last_live_row = phase_live_rows[-1] if phase_live_rows else None
            if phase_live_rows:
                live_source_filter_kind = "capture_phase"
            else:
                previous_source_frame = 0
                for row_index, row in enumerate(rows):
                    source_frame = parse_int(row.get("source_frame_index"), 0)
                    if source_frame <= 0:
                        continue
                    if first_live_row is None:
                        first_live_row = row_index
                        last_live_row = row_index
                    elif source_frame != previous_source_frame:
                        last_live_row = row_index
                    previous_source_frame = source_frame
                if first_live_row is not None:
                    live_source_filter_kind = "source_frame_index"
            if first_live_row is not None and last_live_row is not None:
                live_source_bounds = (first_live_row, last_live_row)
        previous_qpc = None
        max_qpc_delta_us = 0
        large_gaps = []
        max_total_us = 0
        max_capture_us = 0
        max_present_call_us = 0
        max_mux_kb = 0
        overload_rows = 0
        min_qpc_us = 0
        max_qpc_us = 0
        rows_in_window = 0
        for row_index, row in enumerate(rows):
            qpc = parse_int(row.get("qpc_us"), 0)
            if qpc > 0:
                min_qpc_us = qpc if min_qpc_us == 0 else min(min_qpc_us, qpc)
                max_qpc_us = max(max_qpc_us, qpc)
            if window_bounds and (qpc < window_bounds[0] or qpc > window_bounds[1]):
                continue
            if live_source_only and (
                live_source_bounds is None
                or row_index < live_source_bounds[0]
                or row_index > live_source_bounds[1]
            ):
                continue
            rows_in_window += 1
            if previous_qpc is None:
                # capture_phase=2 is attached to the Present that ends this interval,
                # so its first row's explicit delta is part of the live recording.
                # The legacy source-frame heuristic cannot make that guarantee.
                delta = (
                    parse_int(row.get("qpc_delta_us"), 0)
                    if live_source_filter_kind == "capture_phase"
                    else 0
                )
            elif "qpc_delta_us" in row and row.get("qpc_delta_us") not in (None, ""):
                delta = parse_int(row.get("qpc_delta_us"), 0)
            elif qpc > previous_qpc:
                delta = qpc - previous_qpc
            else:
                delta = 0
            previous_qpc = qpc if qpc > 0 else previous_qpc
            max_qpc_delta_us = max(max_qpc_delta_us, delta)
            if delta >= 100000:
                large_gaps.append(
                    {
                        "frame": parse_int(row.get("frame"), 0),
                        "qpc_us": qpc,
                        "qpc_delta_us": delta,
                        "total_us": parse_int(row.get("total_us"), 0),
                        "capture_us": parse_int(row.get("capture_us"), 0),
                        "overload_flags": row.get("overload_flags", "0"),
                    }
                )
            max_total_us = max(max_total_us, parse_int(row.get("total_us"), 0))
            max_capture_us = max(max_capture_us, parse_int(row.get("capture_us"), 0))
            max_present_call_us = max(max_present_call_us, parse_int(row.get("present_call_us"), 0))
            max_mux_kb = max(max_mux_kb, parse_int(row.get("mux_queue_kb"), 0))
            overload = row.get("overload_flags", "0")
            try:
                overload_value = int(str(overload), 0)
            except ValueError:
                overload_value = 0
            if overload_value != 0:
                overload_rows += 1
        summaries.append(
            {
                "path": str(path),
                "rows": rows_in_window if (window_bounds or live_source_only) else len(rows),
                "rows_total": len(rows),
                "min_qpc_us": min_qpc_us,
                "max_qpc_us": max_qpc_us,
                "window_start_qpc_us": window_bounds[0] if window_bounds else 0,
                "window_end_qpc_us": window_bounds[1] if window_bounds else 0,
                "live_source_filter": bool(live_source_only and live_source_bounds is not None),
                "live_source_filter_kind": live_source_filter_kind,
                "max_qpc_delta_us": max_qpc_delta_us,
                "large_qpc_gaps": large_gaps[:20],
                "max_total_us": max_total_us,
                "max_capture_us": max_capture_us,
                "max_present_call_us": max_present_call_us,
                "max_mux_queue_kb": max_mux_kb,
                "overload_rows": overload_rows,
            }
        )
    return summaries


def has_source_starvation(media_evidence):
    if media_evidence["source_starved_episodes"]:
        return True
    if any(summary["source_limited_repeats"] > 0 or summary["starved_episodes"] > 0 for summary in media_evidence["wgc_summary"]):
        return True
    return any(item["fresh_miss_pm"] >= 250 and item["min_in_250"] > 0 and item["min_in_250"] < item["min_del_250"]
               for item in media_evidence["wgc_perf"])


def summarize_wgc_source_limits(media_evidence):
    summary_rows = media_evidence["wgc_summary"]
    live_ticks = sum(row["live"] for row in summary_rows)
    duplicate_ticks = sum(row["duplicate"] for row in summary_rows)
    source_limited_repeats = sum(row["source_limited_repeats"] for row in summary_rows)
    return {
        "detail_episode_count": len(media_evidence["source_starved_episodes"]),
        "summary_starved_episodes": sum(row["starved_episodes"] for row in summary_rows),
        "summary_live": live_ticks,
        "summary_duplicate": duplicate_ticks,
        "summary_duplicate_pct": (duplicate_ticks * 100.0 / live_ticks) if live_ticks else 0.0,
        "summary_source_limited_repeats": source_limited_repeats,
        "summary_source_limited_pct": (source_limited_repeats * 100.0 / live_ticks) if live_ticks else 0.0,
        "summary_longest_ms": max((row["longest_ms"] for row in summary_rows), default=0),
        "summary_longest_dup_ticks": max((row["longest_dup_ticks"] for row in summary_rows), default=0),
        "summary_longest_contiguous_dup_ticks": max(
            (row.get("longest_contiguous_dup_ticks", 0) for row in summary_rows), default=0
        ),
        "summary_longest_contiguous_dup_ms": max(
            (row.get("longest_contiguous_dup_ms", 0) for row in summary_rows), default=0
        ),
        "summary_worst_input_fps": min(
            (row["worst_input_fps"] for row in summary_rows if row["worst_input_fps"] > 0),
            default=0,
        ),
        "summary_worst_delivered_fps": min(
            (row["worst_delivered_fps"] for row in summary_rows if row["worst_delivered_fps"] > 0),
            default=0,
        ),
    }


def summarize_inject_pacing(media_evidence):
    perf_rows = media_evidence["inject_perf"]
    summary_rows = media_evidence["inject_summary"]
    source_rows = media_evidence["inject_source_summary"]
    quality_rows = media_evidence.get("inject_quality_summary", [])
    pressure_rows = media_evidence.get("inject_repeat_pressure", [])
    matched_pressure_rows = []
    for row in pressure_rows:
        expected_fps = row.get("tick_emit", 0)
        source_fps = row.get("source_fps", 0.0)
        rate_tolerance = max(3.0, expected_fps * 0.05)
        if (
            expected_fps >= 30
            and source_fps > 0.0
            and abs(source_fps - expected_fps) <= rate_tolerance
            and row.get("hold_with_candidate", 0) > 0
            and row.get("target_superseded", 0) > 0
            and row.get("overload_flags", 0) == 0
        ):
            matched_pressure_rows.append(row)

    longest_matched_run = 0
    current_matched_run = 0
    previous_timestamp_us = -1
    matched_ids = {id(row) for row in matched_pressure_rows}
    for row in pressure_rows:
        if id(row) not in matched_ids:
            current_matched_run = 0
            previous_timestamp_us = -1
            continue
        timestamp_us = row.get("timestamp_us", -1)
        if (
            current_matched_run > 0
            and timestamp_us >= 0
            and previous_timestamp_us >= 0
            and timestamp_us - previous_timestamp_us > 7500000
        ):
            current_matched_run = 0
        current_matched_run += 1
        longest_matched_run = max(longest_matched_run, current_matched_run)
        previous_timestamp_us = timestamp_us

    return {
        "perf_rows": len(perf_rows),
        "input": sum(row["input"] for row in perf_rows),
        "queued": sum(row["queued"] for row in perf_rows),
        "drop_full": sum(row["drop_full"] for row in perf_rows),
        "drop_pace": sum(row["drop_pace"] for row in perf_rows),
        "publication_fps": max((row["publication_fps"] for row in perf_rows), default=0),
        "selection_drop": sum(row["selection_drop"] for row in perf_rows),
        "duplicate": sum(row["duplicate"] for row in perf_rows),
        "summary_live": sum(row["live"] for row in summary_rows),
        "summary_duplicate": sum(row["duplicate"] for row in summary_rows),
        "summary_dup_src": sum(row["dup_src"] for row in summary_rows),
        "summary_dup_def": sum(row["dup_def"] for row in summary_rows),
        "summary_dup_timer": sum(row["dup_timer"] for row in summary_rows),
        "summary_dup_drain": sum(row["dup_drain"] for row in summary_rows),
        "summary_stale_trim": sum(row["stale_trim"] for row in summary_rows),
        "summary_recovery_active": max((row["recovery_active"] for row in summary_rows), default=0),
        "summary_recovery_episodes": sum(row["recovery_episodes"] for row in summary_rows),
        "source_fps_min": min((row["source_fps_min"] for row in source_rows), default=0.0),
        "source_fps_max": max((row["source_fps_max"] for row in source_rows), default=0.0),
        "jitter_max_us": max((row["jitter_max_us"] for row in source_rows), default=0),
        "selection_max_us": max((row["selection_max_us"] for row in source_rows), default=0),
        "target_select": sum(row["target_select"] for row in quality_rows),
        "target_superseded": sum(row["superseded"] for row in quality_rows),
        "target_hold": sum(row["target_hold"] for row in quality_rows),
        "target_hold_with_candidate": sum(row["hold_with_candidate"] for row in quality_rows),
        "buffer_cap_trim": sum(row["buffer_cap_trim"] for row in quality_rows),
        "target_residual_max_us": max((row["target_residual_max_us"] for row in quality_rows), default=0),
        "pressure_rows": len(pressure_rows),
        "pressure_hold_with_candidate": sum(row.get("hold_with_candidate", 0) for row in pressure_rows),
        "matched_rate_pressure_rows": len(matched_pressure_rows),
        "matched_rate_hold_with_candidate": sum(
            row.get("hold_with_candidate", 0) for row in matched_pressure_rows
        ),
        "matched_rate_superseded": sum(row.get("target_superseded", 0) for row in matched_pressure_rows),
        "matched_rate_longest_run": longest_matched_run,
    }


def has_inject_capture_pacer_limit(inject_pacing):
    if (
        inject_pacing["summary_dup_src"] <= 0
        or inject_pacing["drop_pace"] <= 0
        or inject_pacing["summary_dup_def"] != 0
        or inject_pacing["summary_dup_timer"] != 0
        or inject_pacing["summary_dup_drain"] != 0
    ):
        return False

    input_frames = max(inject_pacing["input"], inject_pacing["queued"] + inject_pacing["drop_pace"], 1)
    meaningful_drop_floor = max(3, math.ceil(input_frames * 0.02))
    if inject_pacing["drop_pace"] < meaningful_drop_floor:
        return False

    publication_fps = inject_pacing["publication_fps"]
    if publication_fps > 0 and inject_pacing["source_fps_max"] >= publication_fps * 0.75:
        drop_ratio = inject_pacing["drop_pace"] / input_frames
        if drop_ratio < 0.10:
            return False

    return True


def has_stable_inject_source_rate(inject_pacing):
    source_min = inject_pacing["source_fps_min"]
    source_max = inject_pacing["source_fps_max"]
    if source_min <= 0.0 or source_max <= 0.0 or source_max < source_min:
        return False
    return (source_max - source_min) <= max(3.0, source_max * 0.05)


def has_inject_cfr_playout_churn(inject_pacing):
    duplicates = inject_pacing["summary_dup_src"]
    stale_trim = inject_pacing["summary_stale_trim"]
    if (
        duplicates < 3
        or stale_trim < 3
        or inject_pacing["summary_dup_def"] != 0
        or inject_pacing["summary_dup_timer"] != 0
        or inject_pacing["summary_dup_drain"] != 0
        or not has_stable_inject_source_rate(inject_pacing)
    ):
        return False
    return stale_trim >= math.ceil(duplicates * 0.5)


def has_inject_target_policy_hold_fault(inject_pacing):
    live = inject_pacing["summary_live"]
    hold_with_candidate = inject_pacing["target_hold_with_candidate"]
    paired_churn = min(hold_with_candidate, inject_pacing["target_superseded"])
    if live <= 0 or hold_with_candidate < max(3, math.ceil(live * 0.005)) or paired_churn < 3:
        return False
    # Session-wide min/max is invalid after a real source hitch: one slow window hides a long stable
    # segment. Require repeated per-window hold/drop pairs while the measured source rate matches the
    # output tick rate. Honest low/varying-FPS resampling therefore remains context, not a policy fault.
    return (
        inject_pacing["matched_rate_longest_run"] >= 3
        and inject_pacing["matched_rate_hold_with_candidate"] >= 6
        and inject_pacing["matched_rate_superseded"] >= 3
    )


def summarize_inject_contention_context(media_evidence, live_start_wall_us):
    all_rows = [
        item
        for item in media_evidence.get("inject_contention", [])
        if item.get("publication_to_ingest_max_us", 0) > 0
    ]
    periodic_rows = [item for item in all_rows if not item.get("is_summary")]
    rows = periodic_rows if periodic_rows else all_rows
    startup_cutoff_us = live_start_wall_us + 2000000 if live_start_wall_us >= 0 else -1
    startup_rows = [
        item
        for item in rows
        if startup_cutoff_us >= 0 and 0 <= item.get("timestamp_us", -1) < startup_cutoff_us
    ]
    settled_rows = [item for item in rows if item not in startup_rows]
    evaluated_rows = settled_rows if settled_rows else rows
    settled_starvation = any(item.get("publication_to_ingest_max_us", 0) >= 20000 for item in evaluated_rows)
    startup_backlog_only = (
        bool(settled_rows)
        and any(item.get("publication_to_ingest_max_us", 0) >= 20000 for item in startup_rows)
        and not settled_starvation
    )
    return {
        "startup_rows": len(startup_rows),
        "settled_rows": len(settled_rows),
        "startup_max_us": max((item.get("publication_to_ingest_max_us", 0) for item in startup_rows), default=0),
        "settled_max_us": max((item.get("publication_to_ingest_max_us", 0) for item in settled_rows), default=0),
        "settled_starvation": settled_starvation,
        "startup_backlog_only": startup_backlog_only,
    }


def has_encoder_or_mux_backpressure(media_evidence, perf_summaries, windowed=False):
    if any(item.get("encoder_overload") or item.get("mux_overload") or item.get("backpressure")
           for item in media_evidence["final_metadata"]):
        return True
    if any(item["overload_rows"] > 0 for item in perf_summaries):
        return True
    if windowed:
        return False
    for item in media_evidence["wgc_perf"]:
        if item["overload_flags"] != 0:
            return True
    return False


def is_post_mux_delta_codec_priming(media_evidence, delta_us):
    # Priming and discard metadata explain packet topology; they never excuse a
    # completed-file decoded endpoint mismatch. Only the muxer's one-microsecond
    # timestamp rounding can remain informational here.
    return delta_us <= 1


def has_exact_final_mux_evidence(media_evidence):
    final_packets_clean = bool(media_evidence["final_packet_timelines"]) and all(
        item["max_packet_delta_us"] <= 1 and item["audio_past_target"] == 0
        for item in media_evidence["final_packet_timelines"]
    )
    final_metadata_clean = bool(media_evidence["final_metadata"]) and all(
        item["max_delta_us"] <= 1 for item in media_evidence["final_metadata"]
    )
    no_post_mux_strict_mismatch = all(
        is_post_mux_delta_codec_priming(media_evidence, delta)
        for delta in media_evidence["post_mux_audio_mismatch_delta_us"]
    )
    return (final_packets_clean or final_metadata_clean) and no_post_mux_strict_mismatch


def parse_hex_flags(value):
    try:
        return int(str(value), 0)
    except (TypeError, ValueError):
        return 0


def parse_numeric_prefix_int(value, default=0):
    match = re.match(r"\s*(-?\d+)", str(value or ""))
    return parse_int(match.group(1), default) if match else default


def attribution_has_capacity_pressure(item):
    if item.get("fault_hint") != "ce_capacity_pressure":
        return False
    if parse_hex_flags(item.get("overload")) & 0x3:
        return True
    if parse_numeric_prefix_int(item.get("muxBp"), 0) > 0 or parse_numeric_prefix_int(item.get("waitMax"), 0) > 0:
        return True
    return False


def has_wgc_delivery_gap(media_evidence):
    return any(item.get("fault_hint") == "wgc_delivery_gap" for item in media_evidence["wgc_attribution"])


def has_wgc_framepool_pressure_attribution(media_evidence):
    pressure_hints = {"wgc_framepool_pressure", "wgc_framepool_overflow_suspected"}
    return any(
        item.get("fault_hint") in pressure_hints
        or parse_numeric_prefix_int(item.get("poolSat"), 0) > 0
        or parse_numeric_prefix_int(item.get("overwritePrevented"), 0) > 0
        or parse_numeric_prefix_int(item.get("ingressDecimated"), 0) > 0
        for item in media_evidence["wgc_attribution"]
    )


def parse_ms_ratio_value(value):
    text = str(value or "").strip().rstrip(",")
    if text.endswith("ms"):
        text = text[:-2]
    if "/" in text:
        text = text.split("/", 1)[1]
    return parse_float(text)


def has_wgc_encoder_overload_policy_fault(media_evidence, log_summary, capacity_pressure_proven=None):
    if not log_summary:
        return False

    counts = log_summary["counts"]
    if capacity_pressure_proven is False:
        return False
    overload_seen = capacity_pressure_proven is True or log_summary.get("saw_encoder_overload") or log_summary.get("saw_mux_overload")
    overload_seen = overload_seen or any(item["overload_flags"] != 0 for item in media_evidence["wgc_perf"])
    overload_seen = overload_seen or any(attribution_has_capacity_pressure(item)
                                         for item in media_evidence["wgc_attribution"])
    if not overload_seen:
        return False

    if counts.get("wgc_encoder_limited_mode_mismatch", 0) > 0:
        return True
    if counts.get("wgc_selected_source_backtrack", 0) > 0:
        return True
    if log_summary.get("max_wgc_mode_mismatch", 0) > 0 or log_summary.get("max_wgc_source_backtrack", 0) > 0:
        return True
    if any(item.get("mode_mismatch", 0) > 0 or item.get("source_backtrack", 0) > 0
           for item in media_evidence["wgc_smoothness_summary"]):
        return True

    non_encoder_pressure_events = 0
    for event in media_evidence["wgc_cadence_events"]:
        mode = str(event.get("mode", "")).lower()
        if mode not in ("normal_pressure", "scheduler_limited"):
            continue
        if (parse_hex_flags(event.get("overload")) & 0x3) == 0:
            continue
        if parse_ms_ratio_value(event.get("shortfall")) < 100.0:
            continue
        if parse_ms_ratio_value(event.get("oldest")) < 100.0 and parse_int(event.get("bufNow")) < 4:
            continue
        non_encoder_pressure_events += 1

    cadence_pressure_events = (
        counts.get("wgc_too_new_slot_repeat", 0)
        + counts.get("wgc_stale_visual_debt_drop", 0)
        + counts.get("wgc_live_scheduler_rebase", 0)
    )
    smoothness_policy_pressure = any(
        item.get("shortfall_max_ms", 0.0) >= 100.0
        and (item.get("stale_debt_drops", 0) > 0 or item.get("too_new_repeats", 0) > 0
             or item.get("live_rebase_total", 0) > 0)
        for item in media_evidence["wgc_smoothness_summary"]
    )
    if non_encoder_pressure_events > 0 and (smoothness_policy_pressure or cadence_pressure_events >= 3):
        return True

    return (
        log_summary.get("max_wgc_shortfall_ms", 0) >= 150
        and log_summary.get("max_wgc_oldest_ms", 0) >= 150
        and cadence_pressure_events >= 10
    )


def has_wgc_encoder_limited_judder(media_evidence, log_summary, capacity_pressure_proven=None):
    if not log_summary:
        return False

    counts = log_summary["counts"]
    if capacity_pressure_proven is False:
        return False
    overload_seen = capacity_pressure_proven is True or log_summary.get("saw_encoder_overload") or log_summary.get("saw_mux_overload")
    overload_seen = overload_seen or any(item["overload_flags"] != 0 for item in media_evidence["wgc_perf"])
    overload_seen = overload_seen or any(attribution_has_capacity_pressure(item)
                                         for item in media_evidence["wgc_attribution"])
    if not overload_seen:
        return False

    encoder_limited_cadence = any(
        item.get("mode", "").lower() == "encoder_limited" for item in media_evidence["wgc_cadence_events"]
    )
    smoothness_fault = any(
        item.get("too_new_repeats", 0) > 0
        or item.get("mode_mismatch", 0) > 0
        or item.get("source_backtrack", 0) > 0
        or item.get("phase_error_max_us", 0) >= 100000
        or item.get("shortfall_max_ms", 0.0) >= 100.0
        or item.get("live_rebase_max_ticks", 0) > 3
        for item in media_evidence["wgc_smoothness_summary"]
    )
    if not encoder_limited_cadence and not smoothness_fault:
        return False

    if counts.get("wgc_too_new_slot_repeat", 0) > 0:
        return True

    if counts.get("wgc_smoothness_summary", 0) > 0:
        return smoothness_fault

    return log_summary["max_wgc_shortfall_ms"] >= 100 and log_summary["max_wgc_oldest_ms"] >= 100


def has_wgc_av_sync_delay_realization_risk(media_evidence):
    for item in media_evidence["wgc_smoothness_summary"]:
        requested_delay_ms = item.get("av_delay_ms", 0.0)
        if requested_delay_ms <= 0.0:
            continue

        sync_delay_holds = item.get("sync_delay_holds", 0)
        startup_delay_ms = item.get("startup_delay_ms", 0.0)
        effective_delay_ms = item.get("effective_delay_ms", 0.0)
        smoothness_delay_ms = item.get("smoothness_buffer_delay_ms", 0.0)
        expected_effective_delay_ms = requested_delay_ms + smoothness_delay_ms

        if startup_delay_ms > 0.0 and abs(startup_delay_ms - expected_effective_delay_ms) > 5.0:
            return True
        if effective_delay_ms > 0.0 and abs(effective_delay_ms - expected_effective_delay_ms) > 5.0:
            return True

        # Old builds did not log startup/effective delay. If they had to build an active
        # video delay through repeated WGC holds, exact final durations are not enough evidence.
        if startup_delay_ms <= 0.0 and sync_delay_holds >= 10:
            return True

    return False


def wgc_active_delay_matches_request(item):
    requested_delay_ms = item.get("av_delay_ms", 0.0)
    startup_delay_ms = item.get("startup_delay_ms", 0.0)
    effective_delay_ms = item.get("effective_delay_ms", 0.0)
    smoothness_delay_ms = item.get("smoothness_buffer_delay_ms", 0.0)
    expected_effective_delay_ms = requested_delay_ms + smoothness_delay_ms
    return (
        requested_delay_ms > 0.0
        and startup_delay_ms > 0.0
        and effective_delay_ms > 0.0
        and abs(startup_delay_ms - expected_effective_delay_ms) <= 5.0
        and abs(effective_delay_ms - expected_effective_delay_ms) <= 5.0
    )


def wgc_late_residual_is_bounded(item):
    if item.get("delay_residual_avg_abs_us", 0) > 5000:
        return False
    if item.get("delay_residual_p95_us", 0) > 10000:
        return False
    late_max_us = item.get("delay_residual_late_max_us", 0)
    if late_max_us > WGC_RESIDUAL_ISOLATED_MAX_FAULT_US:
        return False
    if late_max_us <= 0 and item.get("delay_residual_max_us", 0) > WGC_RESIDUAL_ISOLATED_MAX_FAULT_US:
        return False
    if wgc_has_raw_delay_residual_evidence(item):
        if item.get("raw_residual_avg_abs_us", 0) > 5000:
            return False
        if item.get("raw_residual_p95_us", 0) > 10000:
            return False
        raw_late_max_us = item.get("raw_residual_late_max_us", 0)
        if raw_late_max_us > WGC_RESIDUAL_ISOLATED_MAX_FAULT_US:
            return False
        if (
            raw_late_max_us <= 0
            and item.get("raw_residual_max_us", 0) > WGC_RESIDUAL_ISOLATED_MAX_FAULT_US
            and item.get("raw_residual_early_max_us", 0) < item.get("raw_residual_max_us", 0)
        ):
            return False
    return True


def wgc_has_raw_delay_residual_evidence(item):
    return (
        item.get("raw_residual_avg_abs_us", 0) > 0
        or item.get("raw_residual_max_us", 0) > 0
        or item.get("raw_residual_p95_us", 0) > 0
        or item.get("raw_residual_late_max_us", 0) > 0
        or item.get("raw_residual_early_max_us", 0) > 0
    )


def wgc_has_delay_residual_evidence(item):
    return (
        item.get("realized_delay_avg_us", 0) > 0
        or item.get("delay_residual_avg_abs_us", 0) > 0
        or item.get("delay_residual_max_us", 0) > 0
        or item.get("delay_residual_p95_us", 0) > 0
        or item.get("delay_residual_late_max_us", 0) > 0
        or item.get("delay_residual_early_max_us", 0) > 0
    )


def wgc_has_source_limited_delay_context(media_evidence, item):
    source_holds = item.get("sync_delay_source_limited_holds", 0)
    policy_holds = item.get("sync_delay_policy_holds", 0)
    if source_holds > 0 and source_holds >= policy_holds:
        return True
    if source_holds > 0 and has_source_starvation(media_evidence):
        return True
    return has_source_starvation(media_evidence) and item.get("delay_reservoir_low_water_ticks", 0) > 0


def wgc_has_mixed_policy_pressure(item):
    source_holds = item.get("sync_delay_source_limited_holds", 0)
    policy_holds = item.get("sync_delay_policy_holds", 0)
    total_holds = item.get("sync_delay_holds", 0) or (source_holds + policy_holds)
    if policy_holds < WGC_ACTIVE_DELAY_POLICY_HOLD_FAULT_MIN_COUNT or total_holds <= 0:
        return False
    policy_permille = (policy_holds * 1000) // total_holds
    return policy_permille >= WGC_ACTIVE_DELAY_POLICY_HOLD_FAULT_PERMILLE or policy_holds > source_holds


def wgc_is_bounded_source_limited_active_delay(media_evidence, item):
    return (
        wgc_active_delay_matches_request(item)
        and wgc_has_delay_residual_evidence(item)
        and wgc_has_source_limited_delay_context(media_evidence, item)
        and not wgc_has_mixed_policy_pressure(item)
        and wgc_late_residual_is_bounded(item)
    )


def wgc_is_sync_protected_source_limited_ceiling(media_evidence, item):
    return (
        wgc_is_bounded_source_limited_active_delay(media_evidence, item)
        and item.get("source_repeat_lower_bound", 0) > 0
        and item.get("excess_repeats", 0) == 0
        and item.get("policy_added_repeats", 0) == 0
        and item.get("smoothness_not_maximal", 0) == 0
        and item.get("delay_post_selection_rejected_sync", 0) == 0
        and item.get("delay_soft_late_accepted", 0) == 0
        and item.get("delay_repeat_soft_safe_candidate", 0) == 0
        and item.get("delay_sync_protected_repeats", 0) > 0
    )


def has_wgc_timestamp_domain_mismatch(media_evidence):
    for item in media_evidence["wgc_smoothness_summary"]:
        if item.get("av_delay_ms", 0.0) <= 0.0 or not wgc_has_raw_delay_residual_evidence(item):
            continue
        predicted_bounded = (
            item.get("delay_residual_avg_abs_us", 0) <= 5000
            and item.get("delay_residual_p95_us", 0) <= 10000
            and item.get("delay_residual_late_max_us", 0) <= 10000
        )
        raw_unbounded = (
            item.get("raw_residual_avg_abs_us", 0) > 5000
            or item.get("raw_residual_p95_us", 0) > 10000
            or item.get("raw_residual_late_max_us", 0) > WGC_RESIDUAL_ISOLATED_MAX_FAULT_US
        )
        if predicted_bounded and raw_unbounded:
            return True
    return False


def has_wgc_active_delay_post_selection_reject(media_evidence):
    return any(item.get("delay_post_selection_rejected_sync", 0) > 0 for item in media_evidence["wgc_smoothness_summary"])


def has_wgc_av_sync_delay_residual_fault(media_evidence):
    for item in media_evidence["wgc_smoothness_summary"]:
        if item.get("av_delay_ms", 0.0) <= 0.0:
            continue

        # Newer WGC summaries log actual selected-frame delay realization. Treat
        # this as stronger evidence than startup/effective configuration parity.
        residual_logged = wgc_has_delay_residual_evidence(item)
        realized_delay_matches = wgc_active_delay_matches_request(item)
        bounded_source_limited = wgc_is_bounded_source_limited_active_delay(media_evidence, item)
        if (
            realized_delay_matches
            and item.get("sync_delay_policy_holds", 0) >= 10
            and item.get("too_new_lead_max_us", 0) > 10000
            and not bounded_source_limited
        ):
            return True
        if not residual_logged:
            continue
        if wgc_source_limited_delay_is_context(media_evidence, item):
            continue

        if item.get("delay_residual_avg_abs_us", 0) > 5000:
            return True
        if item.get("delay_residual_p95_us", 0) > 10000:
            return True
        if (
            item.get("sync_delay_policy_holds", 0) > 0
            and item.get("delay_residual_late_max_us", 0) > 10000
        ):
            return True
        if item.get("delay_residual_late_max_us", 0) > WGC_RESIDUAL_ISOLATED_MAX_FAULT_US:
            return True
        if item.get("raw_residual_avg_abs_us", 0) > 5000:
            return True
        if item.get("raw_residual_p95_us", 0) > 10000:
            return True
        if (
            item.get("sync_delay_policy_holds", 0) > 0
            and item.get("raw_residual_late_max_us", 0) > 10000
        ):
            return True
        if item.get("raw_residual_late_max_us", 0) > WGC_RESIDUAL_ISOLATED_MAX_FAULT_US:
            return True
        if (
            item.get("delay_residual_max_us", 0) > 10000
            and not (
                bounded_source_limited
                and item.get("delay_residual_early_max_us", 0) >= item.get("delay_residual_max_us", 0)
            )
        ):
            return True

    return False


def has_wgc_audio_late_risk(media_evidence):
    for item in media_evidence["wgc_smoothness_summary"]:
        if item.get("av_delay_ms", 0.0) <= 0.0:
            continue

        soft_late_accepted = item.get("delay_soft_late_accepted", 0)
        near_cap_accepted = item.get("delay_near_cap_accepted", 0)
        if wgc_source_limited_delay_is_context(media_evidence, item):
            continue
        source_limited_ceiling = wgc_is_sync_protected_source_limited_ceiling(media_evidence, item)
        if soft_late_accepted >= WGC_AUDIO_LATE_RISK_SOFT_ACCEPT_MIN_COUNT:
            return True
        if near_cap_accepted >= WGC_AUDIO_LATE_RISK_SOFT_ACCEPT_MIN_COUNT and not source_limited_ceiling:
            return True

        if (
            not source_limited_ceiling
            and (
                item.get("delay_residual_p95_us", 0) > WGC_AUDIO_LATE_RISK_P95_US
                or item.get("raw_residual_p95_us", 0) > WGC_AUDIO_LATE_RISK_P95_US
                or item.get("predicted_residual_p95_us", 0) > WGC_AUDIO_LATE_RISK_P95_US
            )
        ):
            return True

        # A single late max near the hard cap is acceptable during true source-limited
        # stalls. It becomes actionable when the selector accepted relaxed frames in
        # that region, because that can make audio feel late despite exact mux/audio
        # durations.
        accepted_relaxed_frames = soft_late_accepted + near_cap_accepted
        if accepted_relaxed_frames < WGC_AUDIO_LATE_RISK_SOFT_ACCEPT_MIN_COUNT:
            continue
        if source_limited_ceiling and soft_late_accepted == 0:
            continue
        if (
            item.get("delay_residual_late_max_us", 0) >= WGC_AUDIO_LATE_RISK_NEAR_CAP_US
            or item.get("raw_residual_late_max_us", 0) >= WGC_AUDIO_LATE_RISK_NEAR_CAP_US
            or item.get("predicted_residual_late_max_us", 0) >= WGC_AUDIO_LATE_RISK_NEAR_CAP_US
        ):
            return True

    return False


def has_wgc_sync_delay_policy_fault(media_evidence):
    for item in media_evidence["wgc_smoothness_summary"]:
        requested_delay_ms = item.get("av_delay_ms", 0.0)
        if requested_delay_ms <= 0.0:
            continue

        policy_holds = item.get("sync_delay_policy_holds", 0)
        if policy_holds >= 10:
            if (
                wgc_is_bounded_source_limited_active_delay(media_evidence, item)
                and item.get("delay_residual_late_max_us", 0) <= 10000
                and item.get("raw_residual_late_max_us", 0) <= 10000
            ):
                continue
            return True

        source_holds = item.get("sync_delay_source_limited_holds", 0)
        if source_holds > 0 or policy_holds > 0:
            continue

        # Compatibility for logs before source-limited/policy split: extreme realized-delay
        # hold clusters are a visual policy fault, but not proof that the A/V delay itself
        # was unrealized when startup/effective delay already match the request.
        realized_delay_matches = wgc_active_delay_matches_request(item)
        if not realized_delay_matches:
            continue
        sync_delay_holds = item.get("sync_delay_holds", 0)
        too_new_lead_us = item.get("too_new_lead_max_us", 0)
        if sync_delay_holds >= 120:
            return True
        if sync_delay_holds >= 30 and too_new_lead_us >= max(100000, int(requested_delay_ms * 3000.0)):
            return True

    return False


def has_wgc_cfr_smoothness_not_maximal(media_evidence):
    for item in media_evidence["wgc_smoothness_summary"]:
        if item.get("smoothness_not_maximal", 0) > 0:
            return True
        if item.get("delay_post_selection_rejected_sync", 0) > 0:
            return True
        if item.get("policy_added_repeats", 0) >= WGC_CFR_SMOOTHNESS_EXCESS_REPEAT_FAULT_MIN_COUNT:
            return True
        if (
            item.get("policy_added_repeats", 0) >= WGC_CFR_SMOOTHNESS_POLICY_REPEAT_NOTICE_MIN_COUNT
            and item.get("live", 0) > 0
            and (item.get("policy_added_repeats", 0) * 1000) // item.get("live", 1)
            >= WGC_CFR_SMOOTHNESS_POLICY_REPEAT_NOTICE_PERMILLE
        ):
            return True
        if item.get("excess_repeats", 0) >= WGC_CFR_SMOOTHNESS_EXCESS_REPEAT_FAULT_MIN_COUNT:
            return True
        if item.get("excess_repeat_cluster_max_ticks", 0) >= WGC_CFR_SMOOTHNESS_EXCESS_REPEAT_CLUSTER_FAULT_TICKS:
            return True
    return False


def has_wgc_startup_smoothness_underfilled(media_evidence):
    for item in media_evidence["wgc_smoothness_summary"]:
        smoothness_attempted = (
            item.get("smooth_target_delay_us", 0) > 0
            or item.get("smoothness_buffer_retained_frames", 0) > 0
            or item.get("smoothness_buffer_desired_frames", 0) > 0
        )
        if not smoothness_attempted:
            continue
        if item.get("smooth_delay_deficit_us", 0) >= 8000:
            return True
        if item.get("startup_delay_deficit_us", 0) >= 8000:
            return True
        reason = item.get("startup_reserve_reason", "")
        if reason in ("partial_span_timeout", "reserve_timeout", "low_water_timeout"):
            return True
    return any(
        quality.get("smooth_delay_deficit_us", 0) >= 8000
        or quality.get("startup_delay_deficit_us", 0) >= 8000
        for quality in media_evidence["wgc_quality"]
    )


def wgc_clean_source_coverage_items(media_evidence):
    return [
        item
        for item in media_evidence["wgc_source_coverage"]
        if item.get("best_effort", 0) > 0
        and item.get("source_repeat_lower_bound", 0) > 0
        and item.get("duplicates", 0) == item.get("source_repeat_lower_bound", 0)
        and item.get("excess_repeats", 0) == 0
        and item.get("policy_added_repeats", 0) == 0
        and item.get("clean_encoder_mux", 0) > 0
        and item.get("clean_pool", 0) > 0
        and item.get("clean_selection", 0) > 0
    ]


def has_wgc_clean_source_limited_coverage(media_evidence):
    return bool(wgc_clean_source_coverage_items(media_evidence))


def has_wgc_source_limited_playout_maximal(media_evidence):
    if has_wgc_clean_source_limited_coverage(media_evidence):
        return True
    coverage_items = [
        item
        for item in media_evidence["wgc_source_coverage"]
        if item.get("source_repeat_lower_bound", 0) > 0
        and item.get("policy_added_repeats", 0) == 0
        and item.get("clean_encoder_mux", 0) > 0
        and item.get("clean_pool", 0) > 0
        and parse_hex_flags(item.get("encoder_overload", "0x0")) == 0
        and item.get("mux_backpressure", 0) == 0
        and item.get("pool_pressure", 0) == 0
    ]
    if not coverage_items or has_wgc_repeat_with_safe_candidate(media_evidence):
        return False
    for item in media_evidence["wgc_smoothness_summary"]:
        excess_repeats = item.get("excess_repeats", 0)
        matching_coverage = [
            coverage
            for coverage in coverage_items
            if coverage.get("excess_repeats", 0) == excess_repeats
            and coverage.get("source_repeat_lower_bound", 0) == item.get("source_repeat_lower_bound", 0)
        ]
        live = max(
            item.get("live", 0),
            max((coverage.get("live", 0) for coverage in matching_coverage), default=0),
        )
        allowed_accounting_excess = max(5, live // 1000)
        if (
            live > 0
            and item.get("wgc_smoothness_verdict_complete", 0) > 0
            and item.get("source_repeat_lower_bound", 0) > 0
            and excess_repeats <= allowed_accounting_excess
            and item.get("policy_added_repeats", 0) == 0
            and item.get("excess_repeat_clusters", 0) == 0
            and item.get("excess_repeat_cluster_max_ticks", 0) == 0
            and item.get("smoothness_not_maximal", 0) == 0
            and item.get("mixed_policy_fault", 0) == 0
            and item.get("delay_post_selection_rejected_sync", 0) == 0
            and item.get("wgc_smoothness_evidence_incomplete", 0) == 0
            and matching_coverage
        ):
            return True
    return False


def wgc_source_delivery_period_us(media_evidence):
    worst_delivered_fps = min(
        (
            item.get("worst_delivered_fps", 0)
            for item in media_evidence["wgc_summary"]
            if item.get("worst_delivered_fps", 0) > 0
        ),
        default=0,
    )
    if worst_delivered_fps <= 0:
        return 0
    return int(math.ceil(1000000.0 / worst_delivered_fps))


def wgc_source_limited_delay_is_context(media_evidence, item):
    """True when delay variation is bounded by proven source delivery holes, not CE policy.

    A low-cadence desktop/variable-FPS source can make selected-frame age vary by its own
    delivery interval even though CFR output, A/V endpoints, and CE's lower-bound playout are
    exact. Keep genuinely actionable policy/safe-candidate/timestamp evidence strict, while
    scaling the context ceiling to the worst observed delivered-source interval.
    """
    source_period_us = wgc_source_delivery_period_us(media_evidence)
    if source_period_us <= 0:
        return False
    if not (
        has_wgc_source_limited_playout_maximal(media_evidence)
        and wgc_active_delay_matches_request(item)
        and wgc_has_source_limited_delay_context(media_evidence, item)
        and item.get("wgc_smoothness_verdict_complete", 0) > 0
        and item.get("source_repeat_lower_bound", 0) > 0
        and item.get("policy_added_repeats", 0) == 0
        and item.get("smoothness_not_maximal", 0) == 0
        and item.get("mixed_policy_fault", 0) == 0
        and item.get("sync_delay_policy_holds", 0) == 0
        and item.get("delay_post_selection_rejected_sync", 0) == 0
        and item.get("wgc_smoothness_evidence_incomplete", 0) == 0
        and item.get("delay_soft_late_accepted", 0) == 0
        and item.get("delay_near_cap_accepted", 0) < WGC_AUDIO_LATE_RISK_SOFT_ACCEPT_MIN_COUNT
        and item.get("delay_repeat_soft_safe_candidate", 0) == 0
        and item.get("delay_sync_protected_repeats", 0) > 0
    ):
        return False

    avg_limit_us = max(5000, int(math.ceil(source_period_us / 2.0)))
    p95_limit_us = max(10000, source_period_us)
    late_max_limit_us = max(WGC_RESIDUAL_ISOLATED_MAX_FAULT_US, source_period_us * 2)
    spread_limit_us = max(WGC_REALIZED_DELAY_INSTABILITY_SPREAD_US, source_period_us * 2)
    if item.get("delay_residual_avg_abs_us", 0) > avg_limit_us:
        return False
    if item.get("delay_residual_p95_us", 0) > p95_limit_us:
        return False
    if item.get("delay_residual_late_max_us", 0) > late_max_limit_us:
        return False
    if wgc_has_raw_delay_residual_evidence(item):
        if item.get("raw_residual_avg_abs_us", 0) > avg_limit_us:
            return False
        if item.get("raw_residual_p95_us", 0) > p95_limit_us:
            return False
        if item.get("raw_residual_late_max_us", 0) > late_max_limit_us:
            return False
    if item.get("predicted_residual_p95_us", 0) > p95_limit_us:
        return False
    if item.get("predicted_residual_late_max_us", 0) > late_max_limit_us:
        return False
    return wgc_realized_delay_spread_us(item) <= spread_limit_us


def wgc_realized_delay_spread_us(item):
    """Realized content-delay spread (max - min) for an active-delay smoothness summary item.

    A realized minimum of 0 is NOT "no data": it is a genuine FULL COLLAPSE of the content delay
    (the worst case -- the delay disengaged and video ran near-live), which is exactly the collapse
    half of the GPU-bound realized-delay rubber-band. Only an absent/zero MAX means no realization
    samples, so gate on delay_max alone and clamp a negative min to 0.
    """
    delay_max = item.get("realized_delay_max_us", 0)
    delay_min = item.get("realized_delay_min_us", 0)
    if delay_max <= 0:
        return 0
    if delay_min < 0:
        delay_min = 0
    if delay_max < delay_min:
        return 0
    return delay_max - delay_min


def wgc_active_delay_variation_is_source_context(media_evidence, item):
    return wgc_source_limited_delay_is_context(media_evidence, item)


def has_wgc_active_delay_realized_delay_unstable(media_evidence):
    """The realized content delay rubber-bands on an active-delay run.

    This is the GPU-bound under-delivery judder signature: the displayed content age swings by
    more than ~1.5 frame intervals while track lengths/PTS stay equal, so plain duration/sync
    checks (and the runtime's own ``smoothnessNotMaximal``) report the run as fine. Surfaced as a
    distinct verdict so the abnormal-judder condition is unambiguous in triage.
    """
    for item in media_evidence["wgc_smoothness_summary"]:
        if item.get("av_delay_ms", 0.0) <= 0.0:
            continue
        if (
            wgc_realized_delay_spread_us(item) >= WGC_REALIZED_DELAY_INSTABILITY_SPREAD_US
            and not wgc_active_delay_variation_is_source_context(media_evidence, item)
        ):
            return True
    return False


def has_wgc_source_limited_delay_variation_context(media_evidence):
    return any(
        item.get("av_delay_ms", 0.0) > 0.0
        and wgc_realized_delay_spread_us(item) >= WGC_REALIZED_DELAY_INSTABILITY_SPREAD_US
        and wgc_active_delay_variation_is_source_context(media_evidence, item)
        for item in media_evidence["wgc_smoothness_summary"]
    )


def has_wgc_source_limited_smoothness_ceiling(media_evidence):
    return any(
        wgc_is_sync_protected_source_limited_ceiling(media_evidence, item)
        for item in media_evidence["wgc_smoothness_summary"]
    )


def has_wgc_source_coverage_best_effort(media_evidence):
    return bool(wgc_clean_source_coverage_items(media_evidence))


def has_wgc_smoothness_evidence_incomplete(media_evidence):
    return any(
        item.get("wgc_smoothness_evidence_incomplete", 0) > 0
        for item in media_evidence["wgc_smoothness_summary"]
    )


def has_wgc_pool_slot_lifetime_fault(media_evidence):
    return any(item.get("pool_lease_mismatch", 0) > 0 for item in media_evidence["wgc_perf"]) or any(
        item.get("pool_lease_mismatches", 0) > 0 for item in media_evidence["wgc_smoothness_summary"]
    )


def has_wgc_pool_saturated_safe_drop(media_evidence):
    return any(item.get("pool_saturated_drops", 0) > 0 for item in media_evidence["wgc_perf"]) or any(
        item.get("pool_saturated_drops", 0) > 0 for item in media_evidence["wgc_smoothness_summary"]
    )


def has_wgc_ingress_decimated(media_evidence):
    return any(
        item.get("drop_ingress", 0) > 0 or item.get("ingress_decimated", 0) > 0
        for item in media_evidence["wgc_perf"]
    ) or any(item.get("wgc_ingress_decimated", 0) > 0 for item in media_evidence["wgc_smoothness_summary"])


def has_wgc_uniform_playout_ingress_double_decimation(media_evidence):
    for item in media_evidence["wgc_smoothness_summary"]:
        if item.get("delay_uniform_hold", 0) <= 0:
            continue
        if item.get("wgc_ingress_decimated", 0) <= 0:
            continue
        accepted = item.get("wgc_source_rolling_accepted", 0)
        cfr_ticks = item.get("wgc_source_rolling_cfr_ticks", 0)
        surplus = item.get("wgc_source_rolling_surplus", 0)
        if (accepted > 0 and cfr_ticks > 0 and accepted >= cfr_ticks) or surplus > 0:
            return True
    return False


def has_wgc_copy_pool_pressure(media_evidence):
    return (
        has_wgc_pool_saturated_safe_drop(media_evidence)
        or has_wgc_ingress_decimated(media_evidence)
        or any(item.get("smoothness_retained_cap_trim", 0) > 0 for item in media_evidence["wgc_smoothness_summary"])
    )


def has_wgc_pool_evidence_missing(media_evidence):
    summary_needs_pool_evidence = any(
        item.get("smoothness_buffer_enabled", 0) > 0
        or item.get("smoothness_buffer_retained_frames", 0) > 0
        or item.get("smoothness_buffer_pool_slots", 0) > 0
        for item in media_evidence["wgc_smoothness_summary"]
    )
    summary_has_pool_evidence = any(
        item.get("pool_lifetime_evidence", 0) > 0 for item in media_evidence["wgc_smoothness_summary"]
    )
    perf_has_pool_evidence = any(item.get("pool_lease_evidence", False) for item in media_evidence["wgc_perf"])
    return summary_needs_pool_evidence and not (summary_has_pool_evidence or perf_has_pool_evidence)


def has_wgc_repeat_with_safe_candidate(media_evidence):
    has_soft_safe_evidence = any(
        "delay_repeat_soft_safe_candidate" in item for item in media_evidence["wgc_smoothness_summary"]
    )
    if has_soft_safe_evidence:
        return any(
            item.get("delay_repeat_soft_safe_candidate", 0) > 0
            and item.get("policy_added_repeats", 0) > 0
            for item in media_evidence["wgc_smoothness_summary"]
        )
    return any(
        (
            item.get("delay_repeat_safe_after_promotion", 0) > 0
            or item.get("delay_repeat_safe_candidate", 0) > 0
        )
        and item.get("policy_added_repeats", 0) > 0
        for item in media_evidence["wgc_smoothness_summary"]
    )


def has_wgc_post_stall_recovery_fault(media_evidence):
    has_soft_safe_evidence = any(
        "delay_repeat_soft_safe_candidate" in item for item in media_evidence["wgc_smoothness_summary"]
    )
    return any(
        item.get("delay_repeat_state_post_stall", 0) > 0
        and (
            (
                item.get("delay_repeat_soft_safe_candidate", 0) > 0
                if has_soft_safe_evidence
                else (
                    item.get("delay_repeat_safe_candidate", 0) > 0
                    or item.get("delay_repeat_safe_after_promotion", 0) > 0
                )
            )
            or item.get("policy_added_repeats", 0) > 0
        )
        for item in media_evidence["wgc_smoothness_summary"]
    )


def has_wgc_sync_delay_reserve_pressure(media_evidence):
    for item in media_evidence["wgc_smoothness_summary"]:
        requested_delay_ms = item.get("av_delay_ms", 0.0)
        if requested_delay_ms <= 0.0:
            continue
        bounded_source_limited = wgc_is_bounded_source_limited_active_delay(media_evidence, item)
        if item.get("sync_delay_policy_holds", 0) > 0 and not bounded_source_limited:
            continue
        if item.get("sync_delay_source_limited_holds", 0) > 0:
            return True
        if bounded_source_limited:
            return True

        realized_delay_matches = wgc_active_delay_matches_request(item)
        if realized_delay_matches and 0 < item.get("sync_delay_holds", 0) < 120:
            return True

    return False


def summarize_stop_audio_shortfalls(media_evidence):
    short_tracks = [
        item for item in media_evidence["stop_audio_tracks"]
        if item["diff_samples"] < -48000 or item["diff_ms"] < -1000.0
    ]
    multi_source_short_tracks = [item for item in short_tracks if len(item["sources"]) > 1]
    return {
        "short_count": len(short_tracks),
        "multi_source_short_count": len(multi_source_short_tracks),
        "worst_shortfall_ms": min((item["diff_ms"] for item in short_tracks), default=0.0),
        "tracks": short_tracks,
    }


def is_sparse_app_source_silence(item):
    return (
        bool(item.get("process")) and item.get("process") != "-"
        and item.get("ring_peak_samples", 0) == 0
        and (item.get("pad_samples", 0) > 0 or item.get("ring_underruns", 0) > 0)
    )


def summarize_started_app_source_health(media_evidence, log_summary):
    counts = log_summary["counts"] if log_summary else {}
    stop_sources = media_evidence["stop_audio_sources"]
    late_join_sources = [
        item for item in stop_sources
        if item.get("late_join_suppressed_samples", 0) > 0 or item.get("late_join_preserved_samples", 0) > 0
    ]
    backlog_sources = [
        item for item in stop_sources
        if item.get("packet_gap_samples", 0) >= 48000 and item.get("late_join_suppressed_samples", 0) == 0
    ]
    underrun_sources = [item for item in stop_sources if item.get("ring_underruns", 0) > 0 or item.get("pad_samples", 0) > 0]
    sparse_silence_sources = [item for item in underrun_sources if is_sparse_app_source_silence(item)]
    active_underrun_sources = [item for item in underrun_sources if not is_sparse_app_source_silence(item)]
    return {
        "late_join_live_count": counts.get("audio_late_app_live_join", 0),
        "late_source_backlog_count": counts.get("audio_late_app_source_backlog", 0),
        "app_gap_silence_count": counts.get("audio_app_source_gap_silence", 0),
        "late_join_sources": late_join_sources,
        "backlog_sources": backlog_sources,
        "underrun_sources": underrun_sources,
        "sparse_silence_sources": sparse_silence_sources,
        "active_underrun_sources": active_underrun_sources,
    }


def summarize_app_audio_latency(media_evidence, log_summary, stop_start_wall_us=-1):
    warning_rows = media_evidence.get("app_latency_warnings", [])
    if warning_rows:
        stop_warning_count = sum(
            1
            for item in warning_rows
            if stop_start_wall_us >= 0 and item.get("timestamp_us", -1) >= stop_start_wall_us
        )
        warning_count = len(warning_rows) - stop_warning_count
    else:
        stop_warning_count = 0
        warning_count = log_summary["counts"].get("audio_app_latency_elevated", 0) if log_summary else 0
    stop_drain_only = stop_warning_count > 0 and warning_count == 0
    sources = media_evidence.get("stop_app_audio_latency", [])
    elevated_sources = []
    stop_context_sources = []
    for item in sources:
        excess_avg = item.get("excess_avg_ms")
        excess_max = item.get("excess_max_ms")
        if excess_avg is not None and excess_max is not None:
            elevated = excess_avg >= 40.0 or excess_max >= 80
        else:
            elevated = item.get("avg_ms", 0.0) >= 250.0 or item.get("max_ms", 0) >= 300
        if elevated and stop_drain_only and not item.get("phase_split", False):
            stop_context_sources.append(item)
        elif elevated:
            elevated_sources.append(item)

    queue_overrun_packets = sum(item.get("queue_overrun_packets", 0) for item in sources)
    queue_overrun_frames = sum(item.get("queue_overrun_frames", 0) for item in sources)
    underruns = sum(item.get("underruns", 0) for item in sources)
    catastrophic_resync_events = sum(item.get("catastrophic_resync_events", 0) for item in sources)
    integrity_fault = (
        queue_overrun_packets > 0
        or queue_overrun_frames > 0
        or underruns > 0
        or catastrophic_resync_events > 0
    )
    # Legacy logs without a final latency distribution cannot prove that live warnings stayed
    # inside the moving video-delay target. Preserve their strict classification. Current logs can
    # clear warning chatter only with an explicit, non-elevated, integrity-clean stop summary.
    warning_without_summary = warning_count > 0 and not sources
    fault_evidence = bool(elevated_sources) or integrity_fault or warning_without_summary
    warning_only_context = warning_count > 0 and not fault_evidence

    return {
        "warning_count": warning_count,
        "stop_drain_warning_count": stop_warning_count,
        "stop_drain_only": stop_drain_only and not elevated_sources,
        "source_count": len(sources),
        "elevated_source_count": len(elevated_sources),
        "worst_avg_ms": max((item.get("avg_ms", 0.0) for item in sources), default=0.0),
        "worst_max_ms": max((item.get("max_ms", 0) for item in sources), default=0),
        "worst_excess_avg_ms": max(
            (item.get("excess_avg_ms") for item in sources if item.get("excess_avg_ms") is not None),
            default=0.0,
        ),
        "worst_excess_max_ms": max(
            (item.get("excess_max_ms") for item in sources if item.get("excess_max_ms") is not None),
            default=0,
        ),
        "max_comp_percent": max((item.get("max_comp_percent", 0.0) for item in sources), default=0.0),
        "queue_overrun_packets": queue_overrun_packets,
        "queue_overrun_frames": queue_overrun_frames,
        "underruns": underruns,
        "catastrophic_resync_events": catastrophic_resync_events,
        "integrity_fault": integrity_fault,
        "warning_without_summary": warning_without_summary,
        "fault_evidence": fault_evidence,
        "warning_only_context": warning_only_context,
        "sources": sources,
        "elevated_sources": elevated_sources,
        "stop_context_sources": stop_context_sources,
    }


def classify_session_triage(
    session_dir, capture_path=None, recording_window=None, recording_id=None, media_log_path=None
):
    selected_recording, discovered_recordings = resolve_recording_evidence(
        session_dir, recording_id=recording_id, media_log=media_log_path
    )
    media_log = selected_recording["media_log"]
    media_text = read_text_if_exists(media_log)
    full_log_summary = analyze_log(media_log) if media_text else None
    full_media_evidence = parse_media_triage(media_text)
    hook_evidence = parse_hook_triage(session_dir)
    perf_summaries_all = parse_perf_csvs(session_dir)
    perf_summaries_live_source = parse_perf_csvs(session_dir, live_source_only=True)
    recording_window_info = build_recording_window_info(media_text, recording_window, perf_summaries_all)
    multi_recording_session = len(discovered_recordings) > 1
    if not recording_window_info and multi_recording_session:
        recording_window_info = build_full_recording_perf_window_info(media_text, perf_summaries_all)
    perf_scope_unavailable = multi_recording_session and not (
        recording_window_info and recording_window_info.get("active")
    )
    if perf_scope_unavailable:
        perf_summaries_live_source = []
        hook_evidence["present_gaps"] = []
    if media_text and recording_window and recording_window_info and recording_window_info.get("active"):
        windowed_media_text = filter_media_text_for_recording_window(media_text, recording_window_info)
        log_summary = analyze_log_text(windowed_media_text)
        media_evidence = merge_window_media_evidence(parse_media_triage(windowed_media_text), full_media_evidence)
    else:
        log_summary = full_log_summary
        media_evidence = full_media_evidence
    if perf_scope_unavailable:
        perf_summaries = []
    else:
        perf_summaries = (
            parse_perf_csvs(session_dir, recording_window_info) if recording_window_info else perf_summaries_all
        )
    manifest = parse_session_manifest(session_dir)
    recording_manifest = selected_recording.get("manifest", {})
    screen_capture_backend = resolve_screen_capture_backend(manifest, media_evidence)

    def screen_capture_diagnostic(suffix):
        return f"{screen_capture_backend}_{suffix}"

    wgc_source_limits = summarize_wgc_source_limits(media_evidence)
    inject_pacing = summarize_inject_pacing(media_evidence)
    stop_audio_shortfalls = summarize_stop_audio_shortfalls(media_evidence)
    started_app_source_health = summarize_started_app_source_health(media_evidence, log_summary)
    live_start_wall_us = parse_live_start_wall_us(media_text)
    stop_start_wall_us = parse_stop_start_wall_us(media_text)
    app_audio_latency = summarize_app_audio_latency(media_evidence, log_summary, stop_start_wall_us)
    inject_contention_context = summarize_inject_contention_context(media_evidence, live_start_wall_us)

    verdicts = []
    contexts = []
    controller_text = read_text_if_exists(session_dir / "captureengine.log")
    controller_recording_starts = len(
        re.findall(r"\[Controller\] Starting (?:audio-only )?recording\.\.\.", controller_text)
    )
    recording_evidence_incomplete = controller_recording_starts > len(discovered_recordings)
    if recording_evidence_incomplete:
        contexts.append("recording_evidence_missing_or_overwritten")
    if perf_scope_unavailable:
        contexts.append("recording_perf_evidence_unscoped")
    if recording_window_info and recording_window_info.get("active"):
        max_present_gap_ms = max((item["max_qpc_delta_us"] for item in perf_summaries), default=0) / 1000.0
        present_gap_evidence = []
        for item in perf_summaries:
            present_gap_evidence.extend(item["large_qpc_gaps"])
        present_gap_source = "perf_recording_window"
        present_gap_filter_kind = "recording_window"
    elif any(item.get("rows", 0) > 1 and item.get("live_source_filter") for item in perf_summaries_live_source):
        max_present_gap_ms = (
            max((item["max_qpc_delta_us"] for item in perf_summaries_live_source), default=0) / 1000.0
        )
        present_gap_evidence = []
        for item in perf_summaries_live_source:
            present_gap_evidence.extend(item["large_qpc_gaps"])
        present_gap_source = "perf_live_source"
        present_gap_filter_kind = next(
            (
                item.get("live_source_filter_kind", "")
                for item in perf_summaries_live_source
                if item.get("live_source_filter_kind", "")
            ),
            "",
        )
    else:
        timestamped_hook_gaps = [
            item for item in hook_evidence["present_gaps"] if item.get("timestamp_us", -1) >= 0
        ]
        live_hook_gaps = [
            item
            for item in timestamped_hook_gaps
            if live_start_wall_us >= 0
            and item["timestamp_us"] >= live_start_wall_us
            and (stop_start_wall_us < 0 or item["timestamp_us"] < stop_start_wall_us)
        ]
        selected_hook_gaps = live_hook_gaps if live_start_wall_us >= 0 and timestamped_hook_gaps else hook_evidence[
            "present_gaps"
        ]
        max_present_gap_ms = max((item["gap_ms"] for item in selected_hook_gaps), default=0.0)
        present_gap_evidence = selected_hook_gaps[:20]
        present_gap_source = "hook_live_window" if selected_hook_gaps is live_hook_gaps else "hook_logs"
        present_gap_filter_kind = "wall_clock" if selected_hook_gaps is live_hook_gaps else ""
    if max_present_gap_ms >= 100.0:
        verdicts.append("source_present_gap")
    if has_source_starvation(media_evidence):
        verdicts.append(screen_capture_diagnostic("source_starvation"))
        verdicts.append(screen_capture_diagnostic("upstream_producer_starvation"))
    dxgi_dup_missed = any(
        item.get("backend", "").lower() == "dxgiduplication" and item.get("dup_missed", 0) > 0
        for item in media_evidence["wgc_perf"]
    )
    dxgi_dup_consumer_pressure = any(
        item.get("backend", "").lower() == "dxgiduplication"
        and item.get("dup_missed", 0) > 0
        and (
            item.get("overload_flags", 0) != 0
            or item.get("pool_saturated_drops", 0) > 0
            or item.get("drop_ingress", 0) > 0
            or item.get("ingress_decimated", 0) > 0
            or (item.get("pool_lease_evidence", False) and item.get("pool_free_min", 0) == 0)
        )
        for item in media_evidence["wgc_perf"]
    )
    if dxgi_dup_consumer_pressure:
        verdicts.append("duplication_consumer_starvation")
    elif dxgi_dup_missed:
        contexts.append("dxgi_dup_delivery_gap")
    if any(item.get("gpu_busy", 0) > 0 for item in media_evidence["inject_contention"]):
        verdicts.append("capture_gpu_queue_starvation")
    if inject_contention_context["settled_starvation"]:
        verdicts.append("media_cpu_starvation")
    elif inject_contention_context["startup_backlog_only"]:
        contexts.append("inject_startup_publication_backlog")
    wgc_delivery_gap = has_wgc_delivery_gap(media_evidence)
    if wgc_delivery_gap:
        verdicts.append(screen_capture_diagnostic("delivery_gap"))
    if log_summary and log_summary["counts"].get("wgc_cfr_producer_contract_fault", 0) > 0:
        verdicts.append(screen_capture_diagnostic("producer_rate_contract_fault"))
    wgc_framepool_pressure = has_wgc_framepool_pressure_attribution(media_evidence)
    if wgc_framepool_pressure:
        verdicts.append(screen_capture_diagnostic("framepool_pressure"))
    if has_inject_capture_pacer_limit(inject_pacing):
        verdicts.append("ce_capture_pacer_limited")
    inject_cfr_playout_churn = has_inject_cfr_playout_churn(inject_pacing)
    inject_target_policy_hold_fault = has_inject_target_policy_hold_fault(inject_pacing)
    if inject_cfr_playout_churn:
        verdicts.append("inject_cfr_playout_churn")
    if inject_target_policy_hold_fault:
        verdicts.append("inject_cfr_target_policy_hold")

    audio_fault_counts = {}
    visual_fault_counts = {}
    mux_fault_counts = {}
    if log_summary:
        audio_fault_counts = {name: log_summary["counts"].get(name, 0) for name in TRIAGE_AUDIO_FAULT_EVENTS}
        visual_fault_counts = {name: log_summary["counts"].get(name, 0) for name in TRIAGE_VISUAL_FAULT_EVENTS}
        mux_fault_counts = {name: log_summary["counts"].get(name, 0) for name in TRIAGE_MUX_FAULT_EVENTS}
    strict_audio_fault_counts = dict(audio_fault_counts)
    sparse_only_app_silence = (
        started_app_source_health["app_gap_silence_count"] > 0
        and bool(started_app_source_health["sparse_silence_sources"])
        and not started_app_source_health["active_underrun_sources"]
        and started_app_source_health["late_source_backlog_count"] == 0
        and not started_app_source_health["backlog_sources"]
    )
    if sparse_only_app_silence:
        strict_audio_fault_counts["audio_underrun"] = 0
        strict_audio_fault_counts["audio_app_source_gap_silence"] = 0
    writer_sync_after_timeout = (
        log_summary is not None
        and log_summary["counts"].get("writer_finalize_timeout", 0) > 0
        and log_summary["counts"].get("writer_sync_finalize", 0) > 0
    )
    late_writer_finalize_recovered = (
        log_summary is not None
        and log_summary["counts"].get("writer_finalize_timeout", 0) > 0
        and log_summary["counts"].get("post_mux_probe_hang", 0) == 0
        and not writer_sync_after_timeout
        and has_exact_final_mux_evidence(media_evidence)
    )
    post_mux_probe_hang = log_summary is not None and log_summary["counts"].get("post_mux_probe_hang", 0) > 0
    post_mux_probe_timeout = log_summary is not None and log_summary["counts"].get("post_mux_probe_timeout", 0) > 0
    strict_mux_fault_counts = dict(mux_fault_counts)
    if late_writer_finalize_recovered:
        strict_mux_fault_counts["writer_finalize_timeout"] = 0
    strict_writer_failure = writer_sync_after_timeout or strict_mux_fault_counts.get("writer_finalize_timeout", 0) > 0
    windowed_capacity_context = recording_window_info is not None
    encoder_or_mux_backpressure = (
        has_encoder_or_mux_backpressure(media_evidence, perf_summaries, windowed=windowed_capacity_context)
        or strict_writer_failure
    )
    if encoder_or_mux_backpressure:
        verdicts.append("ce_encoder_or_mux_backpressure")
    hardware_encoder_starvation = any(item.get("encoder_overload", 0) for item in media_evidence["final_metadata"])
    hardware_encoder_starvation = hardware_encoder_starvation or any(
        item.get("overload_flags", 0) & 0x1 for item in media_evidence["wgc_perf"]
    )
    hardware_encoder_starvation = hardware_encoder_starvation or any(
        item.get("overload_flags", 0) & 0x1 for item in media_evidence["inject_perf"]
    )
    if hardware_encoder_starvation:
        verdicts.append("hardware_encoder_starvation")
    capacity_pressure_for_wgc_overload = encoder_or_mux_backpressure if windowed_capacity_context else None
    wgc_encoder_overload_policy_fault = has_wgc_encoder_overload_policy_fault(
        media_evidence, log_summary, capacity_pressure_for_wgc_overload
    )
    if wgc_encoder_overload_policy_fault:
        verdicts.append(screen_capture_diagnostic("encoder_overload_policy_fault"))
    wgc_encoder_limited_judder = has_wgc_encoder_limited_judder(
        media_evidence, log_summary, capacity_pressure_for_wgc_overload
    )
    if wgc_encoder_limited_judder:
        verdicts.append(screen_capture_diagnostic("encoder_limited_judder"))
    wgc_av_sync_delay_risk = has_wgc_av_sync_delay_realization_risk(media_evidence)
    if wgc_av_sync_delay_risk:
        verdicts.append(screen_capture_diagnostic("av_sync_delay_unrealized"))
    wgc_av_sync_delay_residual_fault = has_wgc_av_sync_delay_residual_fault(media_evidence)
    if wgc_av_sync_delay_residual_fault:
        verdicts.append(screen_capture_diagnostic("av_sync_delay_residual"))
    wgc_audio_late_risk = has_wgc_audio_late_risk(media_evidence)
    if wgc_audio_late_risk:
        verdicts.append(screen_capture_diagnostic("audio_late_risk"))
    wgc_timestamp_domain_mismatch = has_wgc_timestamp_domain_mismatch(media_evidence)
    if wgc_timestamp_domain_mismatch:
        verdicts.append(screen_capture_diagnostic("timestamp_domain_mismatch"))
    wgc_active_delay_post_selection_reject = has_wgc_active_delay_post_selection_reject(media_evidence)
    if wgc_active_delay_post_selection_reject:
        verdicts.append(screen_capture_diagnostic("active_delay_post_selection_reject"))
    wgc_sync_delay_policy_fault = has_wgc_sync_delay_policy_fault(media_evidence)
    if wgc_sync_delay_policy_fault:
        verdicts.append(screen_capture_diagnostic("sync_delay_policy_fault"))
    wgc_cfr_smoothness_not_maximal = has_wgc_cfr_smoothness_not_maximal(media_evidence)
    if wgc_cfr_smoothness_not_maximal:
        verdicts.append(screen_capture_diagnostic("cfr_smoothness_not_maximal"))
    wgc_clean_source_limited_coverage = has_wgc_clean_source_limited_coverage(media_evidence)
    wgc_source_limited_playout_maximal = has_wgc_source_limited_playout_maximal(media_evidence)
    wgc_startup_smoothness_underfilled = has_wgc_startup_smoothness_underfilled(media_evidence)
    if wgc_startup_smoothness_underfilled:
        if wgc_source_limited_playout_maximal:
            contexts.append(screen_capture_diagnostic("startup_reservoir_partial"))
        else:
            verdicts.append(screen_capture_diagnostic("startup_smoothness_underfilled"))
    wgc_active_delay_realized_delay_unstable = has_wgc_active_delay_realized_delay_unstable(media_evidence)
    if wgc_active_delay_realized_delay_unstable:
        verdicts.append(screen_capture_diagnostic("active_delay_realized_delay_unstable"))
    wgc_source_limited_delay_variation_context = has_wgc_source_limited_delay_variation_context(media_evidence)
    if wgc_source_limited_delay_variation_context:
        contexts.append(screen_capture_diagnostic("source_limited_delay_variation"))
    wgc_source_limited_smoothness_ceiling = has_wgc_source_limited_smoothness_ceiling(media_evidence)
    if wgc_source_limited_smoothness_ceiling:
        verdicts.append(screen_capture_diagnostic("source_limited_smoothness_ceiling"))
    wgc_source_coverage_best_effort = has_wgc_source_coverage_best_effort(media_evidence)
    if wgc_source_coverage_best_effort:
        verdicts.append(screen_capture_diagnostic("source_coverage_best_effort"))
    wgc_smoothness_evidence_incomplete = has_wgc_smoothness_evidence_incomplete(media_evidence)
    if wgc_smoothness_evidence_incomplete:
        verdicts.append(screen_capture_diagnostic("smoothness_evidence_incomplete"))
    wgc_pool_slot_lifetime_fault = has_wgc_pool_slot_lifetime_fault(media_evidence)
    if wgc_pool_slot_lifetime_fault:
        verdicts.append(screen_capture_diagnostic("pool_slot_lifetime_fault"))
    wgc_pool_saturated_safe_drop = has_wgc_pool_saturated_safe_drop(media_evidence)
    if wgc_pool_saturated_safe_drop:
        verdicts.append(screen_capture_diagnostic("pool_saturated_safe_drop"))
    wgc_ingress_decimated = has_wgc_ingress_decimated(media_evidence)
    if wgc_ingress_decimated:
        verdicts.append(screen_capture_diagnostic("ingress_decimated"))
    wgc_uniform_playout_ingress_double_decimation = has_wgc_uniform_playout_ingress_double_decimation(media_evidence)
    if wgc_uniform_playout_ingress_double_decimation:
        verdicts.append(screen_capture_diagnostic("uniform_playout_ingress_double_decimation"))
    wgc_copy_pool_pressure = has_wgc_copy_pool_pressure(media_evidence)
    if wgc_copy_pool_pressure:
        verdicts.append(screen_capture_diagnostic("copy_pool_pressure"))
    wgc_pool_evidence_missing = has_wgc_pool_evidence_missing(media_evidence)
    if wgc_pool_evidence_missing:
        verdicts.append(screen_capture_diagnostic("pool_evidence_missing"))
    wgc_repeat_with_safe_candidate = has_wgc_repeat_with_safe_candidate(media_evidence)
    if wgc_repeat_with_safe_candidate:
        verdicts.append(screen_capture_diagnostic("repeat_despite_safe_candidate"))
    wgc_post_stall_recovery_fault = has_wgc_post_stall_recovery_fault(media_evidence)
    if wgc_post_stall_recovery_fault:
        verdicts.append(screen_capture_diagnostic("post_stall_recovery_fault"))
    wgc_sync_delay_reserve_pressure = has_wgc_sync_delay_reserve_pressure(media_evidence)
    if (
        wgc_sync_delay_reserve_pressure
        and not wgc_sync_delay_policy_fault
        and not wgc_cfr_smoothness_not_maximal
        and not wgc_startup_smoothness_underfilled
        and not wgc_av_sync_delay_risk
        and not wgc_av_sync_delay_residual_fault
        and not wgc_audio_late_risk
        and not wgc_timestamp_domain_mismatch
        and not wgc_active_delay_post_selection_reject
        and not wgc_smoothness_evidence_incomplete
    ):
        verdicts.append(screen_capture_diagnostic("sync_delay_reserve_pressure"))
    if started_app_source_health["late_source_backlog_count"] > 0 or started_app_source_health["backlog_sources"]:
        verdicts.append("late_app_source_backlog")
    if log_summary and log_summary["counts"].get("audio_app_stop_active_no_data", 0) > 0:
        verdicts.append("app_audio_active_no_data")
    if started_app_source_health["app_gap_silence_count"] > 0:
        if sparse_only_app_silence:
            verdicts.append("sparse_app_source_silence")
        else:
            verdicts.append("started_app_source_underrun")
    if app_audio_latency["fault_evidence"]:
        verdicts.append("audio_app_latency_elevated")
    elif app_audio_latency["warning_only_context"]:
        contexts.append("app_audio_latency_within_slack")
    elif app_audio_latency["stop_drain_only"]:
        contexts.append("app_audio_stop_drain_latency")
    if post_mux_probe_hang:
        verdicts.append("post_mux_probe_hang")
    elif post_mux_probe_timeout:
        verdicts.append("post_mux_probe_timeout")
    post_mux_strict_mismatches = [
        delta for delta in media_evidence["post_mux_audio_mismatch_delta_us"]
        if not is_post_mux_delta_codec_priming(media_evidence, delta)
    ]
    final_packet_strict = [
        item for item in media_evidence["final_packet_timelines"]
        if item["max_packet_delta_us"] > 1000 or item["audio_past_target"] > 0
    ]
    exported_av_sync_ok = has_exact_final_mux_evidence(media_evidence)
    if stop_audio_shortfalls["multi_source_short_count"] > 0:
        verdicts.append("multi_app_audio_track_stall")
    timeline_audio_fault_counts = dict(strict_audio_fault_counts)
    if exported_av_sync_ok:
        for source_health_event in (
            "audio_underrun",
            "audio_source_padding_summary",
            "audio_late_app_source_backlog",
            "audio_app_source_gap_silence",
        ):
            timeline_audio_fault_counts[source_health_event] = 0
    strict_audio_timeline_fault = (
        any(timeline_audio_fault_counts.values())
        or post_mux_strict_mismatches
        or final_packet_strict
        or media_evidence["zero_drift_warnings"]
        or stop_audio_shortfalls["short_count"] > 0
    )
    source_audio_health_fault = (
        "late_app_source_backlog" in verdicts
        or "started_app_source_underrun" in verdicts
    )
    if (
        strict_audio_timeline_fault
        or (
            source_audio_health_fault
            and not exported_av_sync_ok
        )
    ):
        verdicts.append("ce_audio_timeline_fault")
    if (
        any(visual_fault_counts.values())
        or "ce_capture_pacer_limited" in verdicts
        or inject_cfr_playout_churn
        or inject_target_policy_hold_fault
        or wgc_encoder_limited_judder
        or wgc_encoder_overload_policy_fault
        or wgc_av_sync_delay_risk
        or wgc_av_sync_delay_residual_fault
        or wgc_audio_late_risk
        or wgc_timestamp_domain_mismatch
        or wgc_active_delay_post_selection_reject
        or wgc_sync_delay_policy_fault
        or wgc_cfr_smoothness_not_maximal
        or (wgc_startup_smoothness_underfilled and not wgc_source_limited_playout_maximal)
        or wgc_smoothness_evidence_incomplete
        or wgc_pool_slot_lifetime_fault
        or wgc_pool_saturated_safe_drop
        or wgc_uniform_playout_ingress_double_decimation
        or wgc_framepool_pressure
        or wgc_repeat_with_safe_candidate
        or wgc_post_stall_recovery_fault
    ):
        verdicts.append("ce_visual_timeline_fault")
    if hook_evidence["external_overlay_lines"]:
        contexts.append("external_overlay_present")
    if hook_evidence["crash_events"]:
        verdicts.append("ce_process_crash")
    if not verdicts:
        verdicts.append("unknown")

    rounding_evidence = {
        "post_mux_audio_mismatch_delta_us": media_evidence["post_mux_audio_mismatch_delta_us"],
        "post_mux_audio_priming": media_evidence["post_mux_audio_priming"],
        "post_mux_one_us_or_less_is_info": all(
            is_post_mux_delta_codec_priming(media_evidence, delta)
            for delta in media_evidence["post_mux_audio_mismatch_delta_us"]
        ),
    }
    report = {
        "schema": "ce-session-av-triage-v1",
        "session_dir": str(session_dir),
        "recording_id": selected_recording.get("recording_id"),
        "media_pid": selected_recording.get("media_pid"),
        "capture": str(capture_path) if capture_path else None,
        "recording_window": recording_window_info,
        "manifest": manifest,
        "recording_manifest": recording_manifest,
        "paths": {
            "media_log": str(media_log) if media_log.exists() else None,
            "hook_logs": [str(path) for path in sorted(session_dir.glob("*.log")) if not is_media_log_path(path)],
            "perf_csv": [item["path"] for item in perf_summaries],
            "session_manifest": str(session_dir / "session_manifest.txt") if (session_dir / "session_manifest.txt").exists() else None,
            "recording_manifest": str(selected_recording["manifest_path"])
            if selected_recording.get("manifest_path")
            else None,
        },
        "verdicts": verdicts,
        "contexts": contexts,
        "faults": {
            "encoder_or_mux_backpressure": "ce_encoder_or_mux_backpressure" in verdicts,
            "audio_timeline": "ce_audio_timeline_fault" in verdicts,
            "visual_timeline": "ce_visual_timeline_fault" in verdicts,
            "wgc_encoder_overload_policy": wgc_encoder_overload_policy_fault,
            "wgc_av_sync_delay_unrealized": wgc_av_sync_delay_risk,
            "wgc_av_sync_delay_residual": wgc_av_sync_delay_residual_fault,
            "wgc_audio_late_risk": wgc_audio_late_risk,
            "wgc_timestamp_domain_mismatch": wgc_timestamp_domain_mismatch,
            "wgc_active_delay_post_selection_reject": wgc_active_delay_post_selection_reject,
            "wgc_sync_delay_policy_fault": wgc_sync_delay_policy_fault,
            "wgc_cfr_smoothness_not_maximal": wgc_cfr_smoothness_not_maximal,
            "wgc_startup_smoothness_underfilled": wgc_startup_smoothness_underfilled,
            "wgc_active_delay_realized_delay_unstable": wgc_active_delay_realized_delay_unstable,
            "wgc_clean_source_limited_coverage": wgc_clean_source_limited_coverage,
            "wgc_source_limited_playout_maximal": wgc_source_limited_playout_maximal,
            "wgc_source_limited_delay_variation_context": wgc_source_limited_delay_variation_context,
            "wgc_source_limited_smoothness_ceiling": wgc_source_limited_smoothness_ceiling,
            "wgc_source_coverage_best_effort": wgc_source_coverage_best_effort,
            "wgc_smoothness_evidence_incomplete": wgc_smoothness_evidence_incomplete,
            "wgc_pool_slot_lifetime_fault": wgc_pool_slot_lifetime_fault,
            "wgc_pool_saturated_safe_drop": wgc_pool_saturated_safe_drop,
            "wgc_delivery_gap": wgc_delivery_gap,
            "wgc_framepool_pressure": wgc_framepool_pressure,
            "wgc_ingress_decimated": wgc_ingress_decimated,
            "wgc_uniform_playout_ingress_double_decimation": wgc_uniform_playout_ingress_double_decimation,
            "wgc_copy_pool_pressure": wgc_copy_pool_pressure,
            "wgc_pool_evidence_missing": wgc_pool_evidence_missing,
            "wgc_repeat_with_safe_candidate": wgc_repeat_with_safe_candidate,
            "wgc_post_stall_recovery_fault": wgc_post_stall_recovery_fault,
            "wgc_sync_delay_reserve_pressure": wgc_sync_delay_reserve_pressure,
            "late_app_source_backlog": "late_app_source_backlog" in verdicts,
            "started_app_source_underrun": "started_app_source_underrun" in verdicts,
            "sparse_app_source_silence": "sparse_app_source_silence" in verdicts,
            "audio_app_latency_elevated": "audio_app_latency_elevated" in verdicts,
            "inject_cfr_playout_churn": inject_cfr_playout_churn,
            "inject_cfr_target_policy_hold": inject_target_policy_hold_fault,
            "post_mux_probe_hang": post_mux_probe_hang,
            "post_mux_probe_timeout": post_mux_probe_timeout,
            "ce_process_crash": bool(hook_evidence["crash_events"]),
        },
        "evidence": {
            "screen_capture_backend": screen_capture_backend,
            "controller_recording_start_count": controller_recording_starts,
            "discovered_recording_evidence_count": len(discovered_recordings),
            "recording_evidence_incomplete": recording_evidence_incomplete,
            "recording_perf_scope_unavailable": perf_scope_unavailable,
            "discovered_recordings": [
                {
                    "recording_id": item["recording_id"],
                    "media_pid": item["media_pid"],
                    "media_log": str(item["media_log"]),
                }
                for item in discovered_recordings
            ],
            "recording_window": recording_window_info,
            "max_present_gap_ms": max_present_gap_ms,
            "present_gap_source": present_gap_source,
            "present_gap_filter_kind": present_gap_filter_kind,
            "present_gaps": present_gap_evidence[:20],
            "present_stalled_lines": hook_evidence["present_stalled_lines"][:20],
            "external_overlay_lines": hook_evidence["external_overlay_lines"],
            "inject_contention_context": inject_contention_context,
            "crash_events": hook_evidence["crash_events"],
            "wgc_source_starved_episodes": media_evidence["source_starved_episodes"],
            "wgc_source_limits": wgc_source_limits,
            "inject_pacing": inject_pacing,
            "cfr_phase_lock_summary": media_evidence["cfr_phase_lock_summary"],
            "wgc_attribution": media_evidence["wgc_attribution"],
            "wgc_summary": media_evidence["wgc_summary"],
            "wgc_quality": media_evidence["wgc_quality"],
            "wgc_source_coverage": media_evidence["wgc_source_coverage"],
            "wgc_cadence_events": media_evidence["wgc_cadence_events"][:20],
            "wgc_smoothness_summary": media_evidence["wgc_smoothness_summary"],
            "wgc_perf_worst": {
                "max_fresh_miss_pm": max((item["fresh_miss_pm"] for item in media_evidence["wgc_perf"]), default=0),
                "min_input_250_fps": min((item["min_in_250"] for item in media_evidence["wgc_perf"] if item["min_in_250"] > 0), default=0),
                "min_delivered_250_fps": min((item["min_del_250"] for item in media_evidence["wgc_perf"] if item["min_del_250"] > 0), default=0),
                "max_callback_gap_us": max((item["cb_gap_max_us"] for item in media_evidence["wgc_perf"]), default=0),
                "max_copy_us": max((item["copy_us"] for item in media_evidence["wgc_perf"]), default=0),
                "max_convert_us": max((item.get("convert_us", 0) for item in media_evidence["wgc_perf"]), default=0),
                "max_fence_us": max((item["fence_us"] for item in media_evidence["wgc_perf"]), default=0),
                "compact_retained_active": any(item.get("compact_retained", 0) for item in media_evidence["wgc_perf"]),
                "source_format": max((item.get("source_format", 0) for item in media_evidence["wgc_perf"]), default=0),
                "copy_format": max((item.get("copy_format", 0) for item in media_evidence["wgc_perf"]), default=0),
                "max_pool_lease": max((item.get("pool_lease_max", 0) for item in media_evidence["wgc_perf"]), default=0),
                "min_pool_free": min(
                    (item.get("pool_free_min", 0) for item in media_evidence["wgc_perf"]
                     if item.get("pool_lease_evidence", False)),
                    default=0,
                ),
                "pool_saturated_drops": sum(
                    item.get("pool_saturated_drops", 0) for item in media_evidence["wgc_perf"]
                ),
                "ingress_decimated": sum(
                    item.get("drop_ingress", 0) + item.get("ingress_decimated", 0)
                    for item in media_evidence["wgc_perf"]
                ),
                "ingress_accepted": sum(item.get("ingress_accepted", 0) for item in media_evidence["wgc_perf"]),
                "ingress_accepted_low_water": sum(
                    item.get("ingress_accepted_low_water", 0) for item in media_evidence["wgc_perf"]
                ),
                "ingress_accepted_recovery": sum(
                    item.get("ingress_accepted_recovery", 0) for item in media_evidence["wgc_perf"]
                ),
                "ingress_accepted_source_below": sum(
                    item.get("ingress_accepted_source_below", 0) for item in media_evidence["wgc_perf"]
                ),
                "ingress_decimated_soft_reserve": sum(
                    item.get("ingress_decimated_soft_reserve", 0) for item in media_evidence["wgc_perf"]
                ),
                "ingress_decimated_hard_reserve": sum(
                    item.get("ingress_decimated_hard_reserve", 0) for item in media_evidence["wgc_perf"]
                ),
                "ingress_decimated_credit": sum(
                    item.get("ingress_decimated_credit", 0) for item in media_evidence["wgc_perf"]
                ),
                "ingress_soft_reserve_pressure": sum(
                    item.get("ingress_soft_reserve_pressure", 0) for item in media_evidence["wgc_perf"]
                ),
                "ingress_hard_reserve_pressure": sum(
                    item.get("ingress_hard_reserve_pressure", 0) for item in media_evidence["wgc_perf"]
                ),
                "duplicate_timestamps_seen": sum(
                    item.get("duplicate_timestamps_seen", 0) for item in media_evidence["wgc_perf"]
                ),
                "duplicate_timestamps_skipped": sum(
                    item.get("duplicate_timestamps_skipped", 0) for item in media_evidence["wgc_perf"]
                ),
                "pool_overwrite_prevented": sum(
                    item.get("pool_overwrite_prevented", 0) for item in media_evidence["wgc_perf"]
                ),
                "pool_lease_mismatch": max(
                    (item.get("pool_lease_mismatch", 0) for item in media_evidence["wgc_perf"]), default=0
                ),
                "has_pool_lease_evidence": any(
                    item.get("pool_lease_evidence", False) for item in media_evidence["wgc_perf"]
                ),
            },
            "perf_csv": perf_summaries,
            "audio_fault_counts": audio_fault_counts,
            "strict_audio_fault_counts": strict_audio_fault_counts,
            "visual_fault_counts": visual_fault_counts,
            "mux_fault_counts": mux_fault_counts,
            "strict_mux_fault_counts": strict_mux_fault_counts,
            "log_counts": log_summary["counts"],
            "writer_sync_after_timeout": writer_sync_after_timeout,
            "late_writer_finalize_recovered": late_writer_finalize_recovered,
            "stop_audio_tracks": media_evidence["stop_audio_tracks"],
            "stop_audio_sources": media_evidence["stop_audio_sources"],
            "started_app_source_health": started_app_source_health,
            "app_audio_latency": app_audio_latency,
            "zero_drift_warnings": media_evidence["zero_drift_warnings"],
            "stop_audio_shortfalls": stop_audio_shortfalls,
            "exported_av_sync_ok": exported_av_sync_ok,
            "final_packet_timelines": media_evidence["final_packet_timelines"],
            "final_metadata": media_evidence["final_metadata"],
            "audio_codec_contracts": media_evidence["audio_codec_contracts"],
            "audio_finalizations": media_evidence["audio_finalizations"],
            "rounding_evidence": rounding_evidence,
        },
    }
    return report


def print_triage_report(report):
    print("session_av_triage:")
    print(f"  session_dir={report['session_dir']}")
    if report.get("recording_id"):
        print(f"  recording_id={report['recording_id']} media_pid={report.get('media_pid')}")
    if report.get("capture"):
        print(f"  capture={report['capture']}")
    if report.get("recording_window"):
        window = report["recording_window"]
        print(
            "  recording_window={spec} active={active} live_start_qpc={live} qpc_us={start}:{end} "
            "reason={reason}".format(
                spec=window.get("spec", ""),
                active=int(bool(window.get("active"))),
                live=window.get("live_start_qpc", 0),
                start=window.get("start_qpc_us", 0),
                end=window.get("end_qpc_us", 0),
                reason=window.get("reason", ""),
            )
        )
    print(f"  verdicts={','.join(report['verdicts'])}")
    if report.get("contexts"):
        print(f"  contexts={','.join(report['contexts'])}")
    print(
        "  faults encoder_or_mux={enc} audio={audio} visual={visual}".format(
            enc=int(report["faults"]["encoder_or_mux_backpressure"]),
            audio=int(report["faults"]["audio_timeline"]),
            visual=int(report["faults"]["visual_timeline"]),
        )
    )
    evidence = report["evidence"]
    screen_capture_backend = evidence.get("screen_capture_backend", "screen_capture")
    print(f"  screen_capture_backend={screen_capture_backend}")
    print(f"  exported_av_sync_ok={int(evidence.get('exported_av_sync_ok', False))}")
    print(
        "  max_present_gap_ms={gap:.3f} source={source}".format(
            gap=evidence["max_present_gap_ms"], source=evidence.get("present_gap_source", "hook_logs")
        )
    )
    wgc_source_limits = evidence["wgc_source_limits"]
    print(
        "  {backend}_source_starved_episodes={detail} summary_episodes={summary} "
        "source_limited_repeats={repeats} dup={dup}/{live} ({dup_pct:.1f}%) "
        "source_limited_pct={src_pct:.1f}% worst_contiguous_freeze={contig_dup}f/{contig_ms}ms "
        "longest_episode={longest}ms episode_dups={longest_dup} "
        "worst_fps={worst_in}/{worst_del} perf_csv={perf_count}".format(
            backend=screen_capture_backend,
            detail=wgc_source_limits["detail_episode_count"],
            summary=wgc_source_limits["summary_starved_episodes"],
            repeats=wgc_source_limits["summary_source_limited_repeats"],
            dup=wgc_source_limits["summary_duplicate"],
            live=wgc_source_limits["summary_live"],
            dup_pct=wgc_source_limits["summary_duplicate_pct"],
            src_pct=wgc_source_limits["summary_source_limited_pct"],
            contig_dup=wgc_source_limits["summary_longest_contiguous_dup_ticks"],
            contig_ms=wgc_source_limits["summary_longest_contiguous_dup_ms"],
            longest=wgc_source_limits["summary_longest_ms"],
            longest_dup=wgc_source_limits["summary_longest_dup_ticks"],
            worst_in=wgc_source_limits["summary_worst_input_fps"],
            worst_del=wgc_source_limits["summary_worst_delivered_fps"],
            perf_count=len(evidence["perf_csv"]),
        )
    )
    if evidence["wgc_quality"]:
        quality = evidence["wgc_quality"][-1]
        print(
            "  {backend}_quality dup={dup}/{live} ({dup_pct:.1f}%) worst1s={unique}/{repeats}/{emit} "
            "limiter={limiter} pool_pressure={pool} free_min={free_min} sat_drop={sat_drop} "
            "ingress_hard={hard} ingress_soft={soft} ingress_dec={ingress_dec} "
            "pool_trim={pool_trim} playout_acc={play_soft}/{play_credit} sync_protected={sync_protected} "
            "policy_added={policy_added} excess={excess} "
            "smooth_deficit={smooth_deficit:.3f}ms startup_deficit={startup_deficit:.3f}ms "
            "dup_ts={dup_ts_seen}/{dup_ts_skipped} "
            "compact_retained={compact} "
            "fmt={source_fmt}->{retained_fmt} convert_us={convert_us} final_av_sync={final_sync}".format(
                backend=screen_capture_backend,
                dup=quality["duplicates"],
                live=quality["live"],
                dup_pct=quality["duplicate_pct"],
                unique=quality["worst_1s_unique"],
                repeats=quality["worst_1s_repeats"],
                emit=quality["worst_1s_emit"],
                limiter=quality["limiter"],
                pool=quality["pool_pressure"],
                free_min=quality["free_min"],
                sat_drop=quality["pool_saturated_drops"],
                hard=quality["ingress_hard"],
                soft=quality["ingress_soft"],
                ingress_dec=quality.get("ingress_decimated", 0),
                pool_trim=quality.get("pool_pressure_trim", 0),
                play_soft=quality.get("ingress_accepted_playout_soft", 0),
                play_credit=quality.get("ingress_accepted_playout_credit", 0),
                sync_protected=quality.get("sync_protected_repeats", 0),
                policy_added=quality.get("policy_added_repeats", 0),
                excess=quality.get("excess_repeats", 0),
                smooth_deficit=quality.get("smooth_delay_deficit_us", 0) / 1000.0,
                startup_deficit=quality.get("startup_delay_deficit_us", 0) / 1000.0,
                dup_ts_seen=quality["duplicate_timestamps_seen"],
                dup_ts_skipped=quality["duplicate_timestamps_skipped"],
                compact=quality["compact_retained"],
                source_fmt=quality["source_format"],
                retained_fmt=quality["retained_format"],
                convert_us=quality["convert_us"],
                final_sync=quality["final_av_sync"],
            )
        )
    if evidence["wgc_source_coverage"]:
        coverage = evidence["wgc_source_coverage"][-1]
        print(
            "  {backend}_source_coverage coverage={coverage} reason={reason} best_effort={best_effort} "
            "dup={dup}/{live} output_fps={output_fps} lower_bound={lower_bound} "
            "sync_lower={sync_lower} delivery_lower={delivery_lower} excess={excess} "
            "policy_added={policy_added} clean={clean_encoder}/{clean_pool}/{clean_selection} "
            "encoderOverload={encoder} muxBackpressure={mux} poolPressure={pool} "
            "final_av_sync={final_sync}".format(
                backend=screen_capture_backend,
                coverage=coverage.get("coverage", ""),
                reason=coverage.get("reason", ""),
                best_effort=coverage.get("best_effort", 0),
                dup=coverage.get("duplicates", 0),
                live=coverage.get("live", 0),
                output_fps=coverage.get("output_fps", 0),
                lower_bound=coverage.get("source_repeat_lower_bound", 0),
                sync_lower=coverage.get("sync_source_repeat_lower_bound", 0),
                delivery_lower=coverage.get("delivery_repeat_lower_bound", 0),
                excess=coverage.get("excess_repeats", 0),
                policy_added=coverage.get("policy_added_repeats", 0),
                clean_encoder=coverage.get("clean_encoder_mux", 0),
                clean_pool=coverage.get("clean_pool", 0),
                clean_selection=coverage.get("clean_selection", 0),
                encoder=coverage.get("encoder_overload", "0x0"),
                mux=coverage.get("mux_backpressure", 0),
                pool=coverage.get("pool_pressure", 0),
                final_sync=coverage.get("final_av_sync", ""),
            )
        )
    stop_shortfalls = evidence["stop_audio_shortfalls"]
    if stop_shortfalls["short_count"]:
        print(
            "  stop_audio_shortfalls={count} multi_source={multi} worst_ms={worst:.3f}".format(
                count=stop_shortfalls["short_count"],
                multi=stop_shortfalls["multi_source_short_count"],
                worst=stop_shortfalls["worst_shortfall_ms"],
            )
        )
    app_health = evidence["started_app_source_health"]
    if app_health["late_source_backlog_count"] or app_health["late_join_live_count"] or app_health["app_gap_silence_count"]:
        print(
            "  app_source_health late_live_join={live} late_backlog={backlog} gap_silence={gap} "
            "sparse_silence_sources={sparse} active_underruns={active}".format(
                live=app_health["late_join_live_count"],
                backlog=app_health["late_source_backlog_count"],
                gap=app_health["app_gap_silence_count"],
                sparse=len(app_health["sparse_silence_sources"]),
                active=len(app_health["active_underrun_sources"]),
            )
        )
    app_latency = evidence["app_audio_latency"]
    if app_latency["warning_count"] or app_latency["source_count"]:
        print(
            "  app_audio_latency warnings={warnings} stop_drain_warnings={stop_warnings} "
            "sources={sources} elevated={elevated} fault={fault} warning_only={warning_only} "
            "worst_delay_avg={avg:.1f}ms worst_delay_max={max_ms}ms "
            "worst_excess_avg={excess_avg:.1f}ms worst_excess_max={excess_max}ms "
            "max_comp={comp:.4f}% queue_overrun={queue_packets}/{queue_frames} "
            "underruns={underruns} catastrophic={cat_events}".format(
                warnings=app_latency["warning_count"],
                stop_warnings=app_latency["stop_drain_warning_count"],
                sources=app_latency["source_count"],
                elevated=app_latency["elevated_source_count"],
                fault=int(app_latency["fault_evidence"]),
                warning_only=int(app_latency["warning_only_context"]),
                avg=app_latency["worst_avg_ms"],
                max_ms=app_latency["worst_max_ms"],
                excess_avg=app_latency["worst_excess_avg_ms"],
                excess_max=app_latency["worst_excess_max_ms"],
                comp=app_latency["max_comp_percent"],
                queue_packets=app_latency["queue_overrun_packets"],
                queue_frames=app_latency["queue_overrun_frames"],
                underruns=app_latency["underruns"],
                cat_events=app_latency["catastrophic_resync_events"],
            )
        )
    if report["evidence"]["crash_events"]:
        print(f"  crash_events={len(report['evidence']['crash_events'])}")
    if evidence["zero_drift_warnings"]:
        worst_residual = max(abs(item["residual_samples"]) for item in evidence["zero_drift_warnings"])
        print(f"  zero_drift_warnings={len(evidence['zero_drift_warnings'])} worst_residual_samples={worst_residual}")
    if evidence["mux_fault_counts"]:
        mux_faults = ",".join(
            f"{name}={count}" for name, count in sorted(evidence["mux_fault_counts"].items()) if count
        )
        if mux_faults:
            print(f"  mux_faults={mux_faults}")
    inject_pacing = evidence["inject_pacing"]
    if inject_pacing["perf_rows"] or inject_pacing["summary_live"] or inject_pacing["target_select"]:
        print(
            "  inject_drop_pace={drop_pace} inject_dup_src={dup_src} stale_trim={stale_trim} "
            "target_select={target_select} superseded={superseded} target_hold={target_hold} "
            "hold_with_candidate={hold_candidate} cap_trim={cap_trim} residual_max={residual}us "
            "inject_source_fps={fps_min:.2f}..{fps_max:.2f} matched_pressure={matched_rows}/{matched_run} "
            "matched_hold_drop={matched_hold}/{matched_superseded}".format(
                drop_pace=inject_pacing["drop_pace"],
                dup_src=inject_pacing["summary_dup_src"],
                stale_trim=inject_pacing["summary_stale_trim"],
                target_select=inject_pacing["target_select"],
                superseded=inject_pacing["target_superseded"],
                target_hold=inject_pacing["target_hold"],
                hold_candidate=inject_pacing["target_hold_with_candidate"],
                cap_trim=inject_pacing["buffer_cap_trim"],
                residual=inject_pacing["target_residual_max_us"],
                fps_min=inject_pacing["source_fps_min"],
                fps_max=inject_pacing["source_fps_max"],
                matched_rows=inject_pacing["matched_rate_pressure_rows"],
                matched_run=inject_pacing["matched_rate_longest_run"],
                matched_hold=inject_pacing["matched_rate_hold_with_candidate"],
                matched_superseded=inject_pacing["matched_rate_superseded"],
            )
        )
    if evidence["cfr_phase_lock_summary"]:
        phase_lock = evidence["cfr_phase_lock_summary"][-1]
        phase_lock_backend = phase_lock["backend"]
        if phase_lock_backend == "wgc" and screen_capture_backend == "dxgi_dup":
            phase_lock_backend = screen_capture_backend
        print(
            "  cfr_phase_lock backend={backend} enabled={enabled} locked={locked} offset={offset}us "
            "stable={stable} unstable={unstable} transitions={acquire}/{rephase}/{release} multiplier={multiplier}".format(
                backend=phase_lock_backend,
                enabled=phase_lock["enabled"],
                locked=phase_lock["locked"],
                offset=phase_lock["offset_us"],
                stable=phase_lock["stable"],
                unstable=phase_lock["unstable"],
                acquire=phase_lock["acquisitions"],
                rephase=phase_lock["rephases"],
                release=phase_lock["releases"],
                multiplier=phase_lock["multiplier"],
            )
        )
    if evidence["wgc_smoothness_summary"]:
        worst_sync_delay = max(evidence["wgc_smoothness_summary"], key=lambda item: item.get("sync_delay_holds", 0))
        if worst_sync_delay.get("av_delay_ms", 0.0) > 0.0:
            print(
                "  {backend}_av_delay requested={requested:.3f}ms startup={startup:.3f}ms effective={effective:.3f}ms "
                "smooth_target={smooth_target:.3f}ms smooth_actual={smooth_actual:.3f}ms "
                "smooth_deficit={smooth_deficit:.3f}ms startup_deficit={startup_deficit:.3f}ms "
                "sync_holds={holds} source_holds={source_holds} policy_holds={policy_holds} "
                "too_new_lead_us={lead} schedule_offset_us={offset} reserve={reserve_frames}/{reserve_span}us "
                "selected={reserve_selected} reason={reserve_reason} realized_avg={realized_avg:.3f}ms "
                "residual_avg={residual_avg_signed:+.3f}/{residual_avg_abs:.3f}ms "
                "residual_p95={residual_p95:.3f}ms residual_max={residual_max:.3f}ms "
                "raw_residual={raw_avg_signed:+.3f}/{raw_avg_abs:.3f}ms raw_p95={raw_p95:.3f}ms "
                "raw_late_max={raw_late_max:.3f}ms raw_minus_pred={raw_minus_pred:+.3f}ms "
                "reservoir={low_water}/{target} low_ticks={low_ticks} "
                "relaxed={relaxed} better={relaxed_better} cluster={relaxed_cluster} "
                "reject_sync={reject_sync} reject_headroom={reject_headroom} reject_cost={reject_cost} "
                "soft_late={soft_late_reject}/{soft_late_accept} older_frame={older_frame} "
                "near_cap={near_cap} hard_only={hard_only} sync_protected={sync_protected} "
                "source_limited_repeat={source_limited_repeat} "
                "repeat_rescue={repeat_rescue_success}/{repeat_rescue_attempts} "
                "repeat_promote={repeat_promote}/{repeat_promote_attempts} "
                "repeat_promote_soft_reject={repeat_promote_soft_reject} "
                "repeat_safe_after_promote={repeat_safe_after_promote} "
                "repeat_safe={repeat_safe}/{repeat_no_safe} "
                "repeat_soft_safe={repeat_soft_safe}/{repeat_no_soft_safe} "
                "repeat_class={repeat_healthy}/{repeat_recoverable}/{repeat_source_limited} "
                "repeat_state={state_healthy}/{state_recoverable}/{state_source}/{state_hard}/{state_post} "
                "post_stall_safe={post_stall_safe} "
                "repeat_reserve_max={repeat_reserve_depth}/{repeat_reserve_span}us "
                "oldest_soft_safe={oldest_soft_safe}us "
                "post_reject_sync={post_reject} post_rescue_sync={post_rescue} "
                "repeat_pressure={repeat_pressure}/{repeat_max} lower_bound={lower_bound} "
                "excess_repeats={excess_repeats} policy_added={policy_added} "
                "excess_clusters={excess_clusters}/{excess_cluster_max} smoothness_not_maximal={not_maximal} "
                "evidence_incomplete={evidence_incomplete} "
                "source_recovery={source_recovery_holds}/"
                "{source_recovery_ticks}".format(
                    backend=screen_capture_backend,
                    requested=worst_sync_delay.get("av_delay_ms", 0.0),
                    startup=worst_sync_delay.get("startup_delay_ms", 0.0),
                    effective=worst_sync_delay.get("effective_delay_ms", 0.0),
                    smooth_target=worst_sync_delay.get("smooth_target_delay_us", 0) / 1000.0,
                    smooth_actual=worst_sync_delay.get("smooth_actual_delay_us", 0) / 1000.0,
                    smooth_deficit=worst_sync_delay.get("smooth_delay_deficit_us", 0) / 1000.0,
                    startup_deficit=worst_sync_delay.get("startup_delay_deficit_us", 0) / 1000.0,
                    holds=worst_sync_delay.get("sync_delay_holds", 0),
                    source_holds=worst_sync_delay.get("sync_delay_source_limited_holds", 0),
                    policy_holds=worst_sync_delay.get("sync_delay_policy_holds", 0),
                    lead=worst_sync_delay.get("too_new_lead_max_us", 0),
                    offset=worst_sync_delay.get("schedule_offset_us", 0),
                    reserve_frames=worst_sync_delay.get("startup_reserve_frames", 0),
                    reserve_span=worst_sync_delay.get("startup_reserve_span_us", 0),
                    reserve_selected=worst_sync_delay.get("startup_reserve_selected", 0),
                    reserve_reason=worst_sync_delay.get("startup_reserve_reason", ""),
                    realized_avg=worst_sync_delay.get("realized_delay_avg_us", 0) / 1000.0,
                    residual_avg_signed=worst_sync_delay.get("delay_residual_avg_signed_us", 0) / 1000.0,
                    residual_avg_abs=worst_sync_delay.get("delay_residual_avg_abs_us", 0) / 1000.0,
                    residual_p95=worst_sync_delay.get("delay_residual_p95_us", 0) / 1000.0,
                    residual_max=worst_sync_delay.get("delay_residual_max_us", 0) / 1000.0,
                    raw_avg_signed=worst_sync_delay.get("raw_residual_avg_signed_us", 0) / 1000.0,
                    raw_avg_abs=worst_sync_delay.get("raw_residual_avg_abs_us", 0) / 1000.0,
                    raw_p95=worst_sync_delay.get("raw_residual_p95_us", 0) / 1000.0,
                    raw_late_max=worst_sync_delay.get("raw_residual_late_max_us", 0) / 1000.0,
                    raw_minus_pred=worst_sync_delay.get("raw_minus_predicted_avg_signed_us", 0) / 1000.0,
                    low_water=worst_sync_delay.get("delay_reservoir_low_water_frames", 0),
                    target=worst_sync_delay.get("delay_reservoir_target_frames", 0),
                    low_ticks=worst_sync_delay.get("delay_reservoir_low_water_ticks", 0),
                    relaxed=worst_sync_delay.get("delay_relaxed_selections", 0),
                    relaxed_better=worst_sync_delay.get("delay_relaxed_better_target", 0),
                    relaxed_cluster=worst_sync_delay.get("delay_relaxed_repeat_cluster", 0),
                    reject_sync=worst_sync_delay.get("delay_relaxed_rejected_sync", 0),
                    reject_headroom=worst_sync_delay.get("delay_relaxed_rejected_headroom", 0),
                    reject_cost=worst_sync_delay.get("delay_relaxed_rejected_cost", 0),
                    soft_late_reject=worst_sync_delay.get("delay_soft_late_rejected", 0),
                    soft_late_accept=worst_sync_delay.get("delay_soft_late_accepted", 0),
                    older_frame=worst_sync_delay.get("delay_older_frame_avoided_repeat", 0),
                    near_cap=worst_sync_delay.get("delay_near_cap_accepted", 0),
                    hard_only=worst_sync_delay.get("delay_hard_only_candidates", 0),
                    sync_protected=worst_sync_delay.get("delay_sync_protected_repeats", 0),
                    source_limited_repeat=worst_sync_delay.get("delay_source_limited_repeats", 0),
                    repeat_rescue_success=worst_sync_delay.get("delay_repeat_rescue_success", 0),
                    repeat_rescue_attempts=worst_sync_delay.get("delay_repeat_rescue_attempts", 0),
                    repeat_promote=worst_sync_delay.get("delay_repeat_promoted_before_repeat", 0),
                    repeat_promote_attempts=worst_sync_delay.get("delay_repeat_promotion_attempts", 0),
                    repeat_promote_soft_reject=worst_sync_delay.get("delay_repeat_promotion_rejected_soft", 0),
                    repeat_safe_after_promote=worst_sync_delay.get("delay_repeat_safe_after_promotion", 0),
                    repeat_safe=worst_sync_delay.get("delay_repeat_safe_candidate", 0),
                    repeat_no_safe=worst_sync_delay.get("delay_repeat_no_safe_candidate", 0),
                    repeat_soft_safe=worst_sync_delay.get("delay_repeat_soft_safe_candidate", 0),
                    repeat_no_soft_safe=worst_sync_delay.get("delay_repeat_no_soft_safe_candidate", 0),
                    repeat_healthy=worst_sync_delay.get("delay_repeat_window_healthy", 0),
                    repeat_recoverable=worst_sync_delay.get("delay_repeat_window_recoverable", 0),
                    repeat_source_limited=worst_sync_delay.get("delay_repeat_window_source_limited", 0),
                    state_healthy=worst_sync_delay.get("delay_repeat_state_healthy", 0),
                    state_recoverable=worst_sync_delay.get("delay_repeat_state_recoverable", 0),
                    state_source=worst_sync_delay.get("delay_repeat_state_source_limited", 0),
                    state_hard=worst_sync_delay.get("delay_repeat_state_hard_stall", 0),
                    state_post=worst_sync_delay.get("delay_repeat_state_post_stall", 0),
                    post_stall_safe=worst_sync_delay.get("delay_post_stall_safe_frames", 0),
                    repeat_reserve_depth=worst_sync_delay.get("delay_repeat_reserve_depth_max", 0),
                    repeat_reserve_span=worst_sync_delay.get("delay_repeat_reserve_span_max_us", 0),
                    oldest_soft_safe=worst_sync_delay.get("delay_oldest_soft_safe_age_max_us", 0),
                    post_reject=worst_sync_delay.get("delay_post_selection_rejected_sync", 0),
                    post_rescue=worst_sync_delay.get("delay_post_selection_rescued_sync", 0),
                    repeat_pressure=worst_sync_delay.get("delay_repeat_cluster_pressure", 0),
                    repeat_max=worst_sync_delay.get("delay_repeat_cluster_max_ticks", 0),
                    lower_bound=worst_sync_delay.get("source_repeat_lower_bound", 0),
                    excess_repeats=worst_sync_delay.get("excess_repeats", 0),
                    policy_added=worst_sync_delay.get("policy_added_repeats", 0),
                    excess_clusters=worst_sync_delay.get("excess_repeat_clusters", 0),
                    excess_cluster_max=worst_sync_delay.get("excess_repeat_cluster_max_ticks", 0),
                    not_maximal=worst_sync_delay.get("smoothness_not_maximal", 0),
                    evidence_incomplete=worst_sync_delay.get("wgc_smoothness_evidence_incomplete", 0),
                    source_recovery_holds=worst_sync_delay.get("delay_source_recovery_holds", 0),
                    source_recovery_ticks=worst_sync_delay.get("delay_source_recovery_ticks", 0),
                )
            )
    rounding = evidence["rounding_evidence"]
    if rounding["post_mux_audio_mismatch_delta_us"]:
        print(
            "  post_mux_rounding_delta_us={deltas} informational={info}".format(
                deltas=",".join(str(value) for value in rounding["post_mux_audio_mismatch_delta_us"]),
                info=int(rounding["post_mux_one_us_or_less_is_info"]),
            )
        )
    completed = report.get("completed_capture")
    if completed:
        video = completed["video"]
        print(
            "  completed_capture passed={passed} cfr={cfr} frames={frames} fps={fps} "
            "endpoints_identical={identical} decoder_clean={decoder}".format(
                passed=int(completed["passed"]),
                cfr=int(completed["cfr_packet_coverage_exact"]),
                frames=video["frame_count"],
                fps=video["fps"],
                identical=int(completed["endpoint_durations_identical"]),
                decoder=int(completed["decoder_clean"]),
            )
        )
        for track in completed["tracks"]:
            print(
                "    a:{ordinal} codec={codec} rate={rate} decoded={decoded} expected={expected} "
                "delta={delta:+d} lattice={lattice} exact={exact} decoder={decoder} "
                "first={first} last={last} tail_ms={tail}".format(
                    ordinal=track["audio_ordinal"],
                    codec=track["codec"],
                    rate=track["sample_rate"],
                    decoded=track["decoded_samples"],
                    expected=track["expected_samples"],
                    delta=track["sample_delta"],
                    lattice=int(track["lattice_representable"]),
                    exact=int(track["endpoint_exact"]),
                    decoder=int(track["decoder_clean"]),
                    first=track["first_content_sample"],
                    last=track["last_content_sample"],
                    tail=track["tail_silence_ms"],
                )
            )


def print_top_histogram(name, histogram, limit=6):
    print(f"{name}:")
    if not histogram:
        print("  none")
        return
    for key, count in histogram.most_common(limit):
        print(f"  {key}: {count}")


def analyze_window(ffprobe, ffmpeg, capture_path, audio_streams, start_time, duration, framehash, framehash_width,
                   nominal_fps):
    read_interval = build_read_interval(start_time, duration)
    video_timing = analyze_video_timing(ffprobe, capture_path, read_interval=read_interval, nominal_fps=nominal_fps)
    audio_tracks = [
        analyze_audio_stream(ffprobe, capture_path, audio_ordinal, stream_info, read_interval=read_interval)
        for audio_ordinal, stream_info in enumerate(audio_streams)
    ]
    duplicate_runs = None
    if framehash and duration > 0.0:
        duplicate_runs = analyze_video_duplicate_runs_segment(
            ffmpeg, capture_path, framehash_width, start_time, duration
        )

    video_duration = video_timing["duration"]
    audio_lengths = [track["decoded_duration"] for track in audio_tracks]
    audio_duration_spread = (max(audio_lengths) - min(audio_lengths)) if audio_lengths else 0.0
    video_audio_max_delta = (
        max(abs(track["decoded_duration"] - video_duration) for track in audio_tracks) if audio_tracks else 0.0
    )
    return {
        "start_time": start_time,
        "duration": duration,
        "video": video_timing,
        "audio": audio_tracks,
        "duplicate_runs": duplicate_runs,
        "audio_duration_spread": audio_duration_spread,
        "video_audio_max_delta": video_audio_max_delta,
    }


def analyze_video_duplicate_runs_segment(ffmpeg, capture_path, scale_width, start_time, duration):
    result = run_command(
        [
            str(ffmpeg),
            "-v",
            "error",
            "-ss",
            f"{max(0.0, start_time):.6f}",
            "-t",
            f"{duration:.6f}",
            "-i",
            str(capture_path),
            "-map",
            "0:v:0",
            "-an",
            "-sn",
            "-dn",
            "-vf",
            f"scale={scale_width}:-2:flags=fast_bilinear,format=gray",
            "-f",
            "framemd5",
            "-",
        ]
    )
    hashes = []
    for line in result.stdout.splitlines():
        if not line or line.startswith("#"):
            continue
        parts = [part.strip() for part in line.split(",")]
        if len(parts) < 6:
            continue
        hashes.append(parts[-1])

    run_lengths = []
    if hashes:
        current_run = 1
        for index in range(1, len(hashes)):
            if hashes[index] == hashes[index - 1]:
                current_run += 1
            else:
                run_lengths.append(current_run)
                current_run = 1
        run_lengths.append(current_run)

    repeated_runs = [run for run in run_lengths if run > 1]
    return {
        "framehash_count": len(hashes),
        "run_count": len(run_lengths),
        "repeated_run_count": len(repeated_runs),
        "repeated_frame_count": sum(run - 1 for run in repeated_runs),
        "longest_run": max(run_lengths) if run_lengths else 0,
        "repeated_histogram": collections.Counter(repeated_runs),
    }


def print_window_summary(name, window):
    print(f"window_{name}:")
    print(
        "  start={start:.6f} duration={duration:.6f} video_frames={frames} video_duration={video_duration:.6f}".format(
            start=window["start_time"],
            duration=window["duration"],
            frames=window["video"]["frame_count"],
            video_duration=window["video"]["duration"],
        )
    )
    print(
        "  delta_mean={mean:.6f} delta_min={delta_min:.6f} delta_max={delta_max:.6f} delta_stdev={stdev:.6f}".format(
            mean=window["video"]["delta_mean"],
            delta_min=window["video"]["delta_min"],
            delta_max=window["video"]["delta_max"],
            stdev=window["video"]["delta_stdev"],
        )
    )
    print(
        f"  audio_duration_spread={window['audio_duration_spread']:.6f} max_video_audio_duration_delta={window['video_audio_max_delta']:.6f}"
    )
    if window["duplicate_runs"] is None:
        print("  duplicate_runs=skipped")
    else:
        print(
            "  duplicate_runs framehash_frames={framehash_count} repeated_runs={repeated_runs} repeated_frames={repeated_frames} longest_run={longest}".format(
                framehash_count=window["duplicate_runs"]["framehash_count"],
                repeated_runs=window["duplicate_runs"]["repeated_run_count"],
                repeated_frames=window["duplicate_runs"]["repeated_frame_count"],
                longest=window["duplicate_runs"]["longest_run"],
            )
        )
    for track in window["audio"]:
        print(
            "  a:{ordinal} samples={samples} duration={duration:.6f} start={start:.6f} end={end:.6f}".format(
                ordinal=track["audio_ordinal"],
                samples=track["sample_total"],
                duration=track["decoded_duration"],
                start=track["frame_start"],
                end=track["frame_end"],
            )
        )


def self_test():
    complete_coverage = summarize_cfr_packet_coverage([0.0, 1 / 120, 2 / 120], [1 / 120] * 3, 120.0)
    assert complete_coverage["complete"]
    assert complete_coverage["expected_packets"] == 3
    sparse_coverage = summarize_cfr_packet_coverage([0.0, 1 / 120, 6 / 120], [1 / 120] * 3, 120.0)
    assert not sparse_coverage["complete"]
    assert sparse_coverage["expected_packets"] == 7
    assert sparse_coverage["missing_packets"] == 4
    assert math.isclose(sparse_coverage["max_gap_ticks"], 5.0, abs_tol=1e-9)
    assert sparse_coverage["packet_count"] == 3
    assert Fraction(sparse_coverage["expected_packets"], 120) * 48000 == 2800

    manifest_warning = (
        "(process:42): GLib-GIO-WARNING **: Failed to open application manifest "
        "`C:\\Windows\\SystemApps\\Example\\AppxManifest.xml' for package #1: error code 0x2"
    )
    actionable_stderr, ignored_stderr = split_decoder_stderr(manifest_warning)
    assert actionable_stderr == ""
    assert ignored_stderr == manifest_warning
    actionable_stderr, ignored_stderr = split_decoder_stderr(manifest_warning + "\n[aac] invalid data")
    assert actionable_stderr == "[aac] invalid data"
    assert ignored_stderr == manifest_warning

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)

        def make_session(name, media="", hook="", perf="", capture_method="wgc"):
            session = root / name
            session.mkdir()
            (session / "session_manifest.txt").write_text(
                f"build_version=test\ncapture_method={capture_method}\nnotes=test fixture\n", encoding="utf-8"
            )
            (session / "media.log").write_text(media, encoding="utf-8")
            if hook:
                (session / "hook_debug.log").write_text(hook, encoding="utf-8")
            if perf:
                (session / "perf_metrics_1.csv").write_text(perf, encoding="utf-8")
            return session

        finite_wgc_producer = make_session(
            "finite_wgc_producer",
            media=(
                "[Cadence Health] Live=120 Unique=72 Dup=48 WgcThr=120 Adj=1 SrcFps=72.00\n"
                "[WGC CFR] Producer contract: backend=wgc outputFps=120 producerTargetFps=120 "
                "minUpdateInterval100ns=83333 policy=finite localThrottleFps=0\n"
            ),
        )
        report = classify_session_triage(finite_wgc_producer)
        assert report["evidence"]["log_counts"]["wgc_cfr_producer_contract_fault"] == 2
        assert "wgc_producer_rate_contract_fault" in report["verdicts"]
        assert "ce_visual_timeline_fault" in report["verdicts"]

        app_active_no_data = make_session(
            "app_active_no_data",
            media=(
                "[AppAudioCapture] Capture mode contract: selected=polling preference=polling-first "
                "eventFallbackAllowed=1\n"
                "[STOP AUDIO] Source 1 (app-active-no-data): track=2 process=brave.exe\n"
            ),
        )
        report = classify_session_triage(app_active_no_data)
        assert "app_audio_active_no_data" in report["verdicts"]
        assert report["evidence"]["log_counts"]["audio_app_capture_mode_contract"] == 1

        source_gap = make_session(
            "source_gap",
            media="[VideoEncoder] Final metadata durations: target=1000 us video=1000 us audioMin=1000 us audioMax=1000 us maxDelta=0 us streams(v=1 a=2) overload(encoder=0 mux=0) backpressure=0 peakMux=0KB peakPkts=0\n",
            hook="DetourPresent: heartbeat #3514 gap=299ms presentOwner=0x0000 depth=0 slFG=0 tid=0x1234\n",
            perf=(
                "frame,qpc_us,total_us,capture_us,present_call_us,mux_queue_kb,overload_flags,api\n"
                "1,1000000,100,20,40,0,0,DX12\n2,1299402,196,64,50,0,0,DX12\n"
            ),
        )
        report = classify_session_triage(source_gap)
        assert "source_present_gap" in report["verdicts"]
        assert "ce_encoder_or_mux_backpressure" not in report["verdicts"]

        pre_live_gap = make_session(
            "pre_live_gap",
            media=(
                "[2026-07-17 16:52:14.000] [INFO] [EncoderThread] Recording live (inject)\n"
                "[2026-07-17 16:52:16.000] [INFO] [Media] Stopping recording...\n"
            ),
            perf=(
                "frame,qpc_us,total_us,capture_us,present_call_us,mux_queue_kb,overload_flags,api,"
                "source_frame_index,qpc_delta_us\n"
                "1,1000000,100,20,40,0,0,DX12,0,0\n"
                "2,1500000,100,20,40,0,0,DX12,0,500000\n"
                "3,2000000,100,20,40,0,0,DX12,1,500000\n"
                "4,2010000,100,20,40,0,0,DX12,2,10000\n"
                "5,2020000,100,20,40,0,0,DX12,3,10000\n"
                "6,2520000,100,20,40,0,0,DX12,3,500000\n"
            ),
        )
        report = classify_session_triage(pre_live_gap)
        assert "source_present_gap" not in report["verdicts"]
        assert report["evidence"]["present_gap_source"] == "perf_live_source"
        assert report["evidence"]["max_present_gap_ms"] == 10.0

        live_source_gap = make_session(
            "live_source_gap",
            media="[2026-07-17 16:52:14.000] [INFO] [EncoderThread] Recording live (inject)\n",
            perf=(
                "frame,qpc_us,total_us,capture_us,present_call_us,mux_queue_kb,overload_flags,api,"
                "source_frame_index,qpc_delta_us\n"
                "1,1000000,100,20,40,0,0,DX12,0,0\n"
                "2,1500000,100,20,40,0,0,DX12,1,500000\n"
                "3,1510000,100,20,40,0,0,DX12,2,10000\n"
                "4,1710000,100,20,40,0,0,DX12,3,200000\n"
            ),
        )
        report = classify_session_triage(live_source_gap)
        assert "source_present_gap" in report["verdicts"]
        assert report["evidence"]["max_present_gap_ms"] == 200.0

        contention_attribution = make_session(
            "contention_attribution",
            media=(
                "[WGC CFR] Source-starved episode: duration=100ms out=12 dup=6 minIn=40 minDel=40 "
                "freshMiss=500pm minBuf=0\n"
                "[WGC Perf] Input: 100 | Queued: 90 | MinIn250/500: 80/90 | MinDel250/500: 80/90 | "
                "FreshMiss: 100pm | Backend: DxgiDuplication DupMissed: 7 | Overload: 0x1\n"
                "[Inject Contention SUMMARY] CaptureLock=1 CpuLease=2 GpuBusy=3 RingFull=4 EventSignals=50 "
                "PubToIngest=2000/25000us\n"
                "[VideoEncoder] Final metadata durations: target=1000 us video=1000 us audioMin=1000 us "
                "audioMax=1000 us maxDelta=0 us streams(v=1 a=1) overload(encoder=1 mux=0) backpressure=0\n"
            ),
        )
        contention_report = classify_session_triage(contention_attribution)
        for verdict in (
            "wgc_upstream_producer_starvation",
            "duplication_consumer_starvation",
            "capture_gpu_queue_starvation",
            "hardware_encoder_starvation",
            "media_cpu_starvation",
        ):
            assert verdict in contention_report["verdicts"]

        startup_contention = make_session(
            "startup_contention",
            media=(
                "[2026-07-17 15:54:31.784] [INFO] [EncoderThread] Recording live (inject)\n"
                "[2026-07-17 15:54:32.540] [INFO] [Inject Contention] CaptureLock=0 CpuLease=1 GpuBusy=0 "
                "RingFull=0 EventSignals=78 PubToIngest=16237/291415us\n"
                "[2026-07-17 15:54:34.541] [INFO] [Inject Contention] CaptureLock=0 CpuLease=1 GpuBusy=0 "
                "RingFull=0 EventSignals=309 PubToIngest=33/59us\n"
            ),
        )
        report = classify_session_triage(startup_contention)
        assert "media_cpu_starvation" not in report["verdicts"]
        assert "inject_startup_publication_backlog" in report["contexts"]
        assert report["evidence"]["inject_contention_context"]["startup_max_us"] == 291415
        assert report["evidence"]["inject_contention_context"]["settled_max_us"] == 59

        overlay_context = make_session(
            "overlay_context",
            media="[VideoEncoder] Final packet timeline: target=1000 us videoEnd=1000 us audioMinEnd=1000 us "
            "audioMaxEnd=1000 us maxPacketDelta=0 us streams(v=1 a=1) audioPastTarget=0\n",
            hook="[2026-07-17 15:54:31.000] Steam overlay module detected\n",
        )
        report = classify_session_triage(overlay_context)
        assert "external_overlay_context" not in report["verdicts"]
        assert "external_overlay_present" in report["contexts"]

        smooth_buffer_item = {}
        update_wgc_smoothness_item_from_line(
            smooth_buffer_item,
            "[WGC CFR SMOOTHNESS SUMMARY] smoothBuf=1 smoothTargetMs=250 smoothFrames=16/16/30 "
            "smoothDelay=133.3ms smoothPoolSlots=20 smoothVramMB=2025.0 smoothCapLimited=1 "
            "smoothReason=vram_cap_limited",
        )
        assert smooth_buffer_item["smoothness_buffer_reason"] == "vram_cap_limited"
        assert smooth_buffer_item["smoothness_buffer_vram_mb"] == 2025.0

        smooth_buffer_with_pool_item = {}
        update_wgc_smoothness_item_from_line(
            smooth_buffer_with_pool_item,
            "[WGC CFR SMOOTHNESS SUMMARY] smoothBuf=1 smoothTargetMs=250 smoothFrames=16/16/30 "
            "smoothDelay=133.3ms smoothPoolSlots=24 sourceFramePoolBuffers=8 budgetSurfaces=32 "
            "syncFrames=4 safetySlots=4 leasedMax=22 freeMin=2 poolSaturatedDrops=0 "
            "overwritePrevented=9 leaseMismatches=0 smoothVramMB=2025.0 smoothCapLimited=1 "
            "smoothReason=vram_cap_limited",
        )
        assert smooth_buffer_with_pool_item["pool_lifetime_evidence"] == 1
        assert smooth_buffer_with_pool_item["smoothness_source_frame_pool_buffers"] == 8
        assert smooth_buffer_with_pool_item["smoothness_buffer_pool_slots"] == 24
        assert smooth_buffer_with_pool_item["smoothness_budget_surfaces"] == 32
        assert smooth_buffer_with_pool_item["pool_lease_max"] == 22
        assert smooth_buffer_with_pool_item["pool_overwrite_prevented"] == 9

        # Smoothness FLOOR line (video-only / low-confidence path): parsed into the same item for
        # triage visibility. Additive only -- no failure gate depends on it.
        smooth_floor_item = {}
        update_wgc_smoothness_item_from_line(
            smooth_floor_item,
            "[WGC CFR SMOOTHNESS FLOOR] smoothFloorSource=auto smoothFloorConfigured=1 smoothFloorMs=0 "
            "smoothFloorRequestedUs=42000 smoothFloorDelayUs=42000 smoothFloorClampedBy=none "
            "smoothFloorRealizedTargetUs=42000 measuredDeliveryGapUs(avg/max)=9000/50000 "
            "measuredSourceJitterUs(avg/max)=800/1200 realizedDelay(min/avg/max)Us=38000/41000/44000 "
            "residualLateMaxUs=3000 avContentDelayActive=0",
        )
        assert smooth_floor_item["smooth_floor_source"] == "auto"
        assert smooth_floor_item["smooth_floor_delay_us"] == 42000
        assert smooth_floor_item["smooth_floor_clamped_by"] == "none"
        assert smooth_floor_item["smooth_floor_delivery_gap_max_us"] == 50000
        assert smooth_floor_item["smooth_floor_realized_min_us"] == 38000
        assert smooth_floor_item["smooth_floor_av_content_delay_active"] == 0

        # WGC SUMMARY: new longestContiguousDup field parses, and worstIn/worstDel stay correct after
        # the group renumbering. This is the true visible-freeze metric (episode metrics overstate it).
        new_summary = WGC_SUMMARY_RE.search(
            "[WGC CFR SUMMARY] Live=23460 Dup=3083 DupPct=13.1% NoFresh=179pm NoReserve=206pm "
            "DupReason(src=3072 def=0 timer=11 drain=0) SourceLimitedRepeats=3072 StarvedEpisodes=1451 "
            "longest=4110ms longestDup=292 longestContiguousDup=115 (958ms) worstIn=4 worstDel=4"
        )
        assert new_summary is not None
        assert parse_int(new_summary.group(10)) == 4110  # episode ms (overstates freeze)
        assert parse_int(new_summary.group(11)) == 292  # episode dups (overstates freeze)
        assert parse_int(new_summary.group(12)) == 115  # true contiguous freeze frames
        assert parse_int(new_summary.group(13)) == 958  # true contiguous freeze ms
        assert parse_int(new_summary.group(14)) == 4  # worstIn still correct
        assert parse_int(new_summary.group(15)) == 4  # worstDel still correct
        # Backward-compat: older logs without the field still parse; contiguous defaults to 0.
        old_summary = WGC_SUMMARY_RE.search(
            "[WGC CFR SUMMARY] Live=1000 Dup=120 DupPct=12.0% NoFresh=120pm NoReserve=0pm "
            "DupReason(src=120 def=0 timer=0 drain=0) SourceLimitedRepeats=120 StarvedEpisodes=5 "
            "longest=400ms longestDup=40 worstIn=84 worstDel=84"
        )
        assert old_summary is not None
        assert parse_int(old_summary.group(12)) == 0  # absent -> 0
        assert parse_int(old_summary.group(14)) == 84  # worstIn still correct on old format
        assert parse_int(old_summary.group(15)) == 84

        wgc_pool_safe_drop = make_session(
            "wgc_pool_safe_drop",
            media=(
                "[WGC Perf] Input: 120 | Queued: 118 | DropFull: 0 | DropPace: 0 | DropThrottle: 0 | "
                "DropStale: 0 (DupTs=0 OOO=0) | DropCursor: 0 | DropPool: 2 | HostQ: 20 | EncQ: 22 | Dup: 2 | "
                "Late: 0 | SrcAvg: 8333us | JitAvg: 200us | JitMax: 900us | Src->Copy: 40/80us | Deliv: 118 | "
                "MinIn250/500: 118/119 | MinDel250/500: 118/119 | FreshMiss: 0pm | BufAvg: 900pm | BufMin: 4 | "
                "NoFresh: 0 | NoReserve: 0 | SchedSelAvg: 0us SchedSelBias: 0us | WgcSelAvg: 0us WgcSelBias: 0us | "
                "CbGap: 8333/9000us CbProc: 50/90us CbDrainMax: 0 | Copy: 50us | "
                "SlotAge: 8200us FastSlot: 0 | PoolLease: max=24 freeMin=0 satDrop=2 overwritePrevented=24 "
                "mismatch=0 sourceFramePoolBuffers=8 copyPoolSlots=24 budgetSurfaces=32 syncFrames=4 "
                "extraFrames=16 safetySlots=4 | KMFail: 0/0 | Flush: 0/0 | Dedicated: 1 | Encode: 300us | "
                "Fence: 0us | Throttle: 120 | Mux: 0KB | Overload: 0x0\n"
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=0 "
                "phaseErrorMax=0us shortfallMax=0.0ms staleDebtDrops=0 liveRebase=0/0 tooNewRepeats=0 "
                "syncDelayHolds=0 tooNewLeadMax=0us avDelay=33.3ms startupDelay=166.6ms "
                "scheduleOffset=0us effectiveDelay=166.6ms lowSourceBypass=0 modeMismatch=0 "
                "sourceBacktrack=0 syncDelaySourceLimitedHolds=0 syncDelayPolicyHolds=0 "
                "startupReserveFrames=24 startupReserveSpan=166000us startupDelayTarget=166600us "
                "startupReserveSelected=1 startupReserveReason=selected smoothBuf=1 smoothTargetMs=250 "
                "smoothFrames=16/16/30 smoothDelay=133.3ms smoothPoolSlots=24 sourceFramePoolBuffers=8 "
                "budgetSurfaces=32 syncFrames=4 safetySlots=4 leasedMax=24 freeMin=0 "
                "poolSaturatedDrops=2 overwritePrevented=24 leaseMismatches=0 smoothVramMB=2025.0 "
                "smoothCapLimited=1 smoothReason=vram_cap_limited\n"
                "[VideoEncoder] Final metadata durations: target=1000 us video=1000 us audioMin=1000 "
                "us audioMax=1000 us maxDelta=0 us streams(v=1 a=2) overload(encoder=0 mux=0) "
                "backpressure=0 peakMux=0KB peakPkts=0\n"
            ),
        )
        report = classify_session_triage(wgc_pool_safe_drop)
        assert "wgc_pool_saturated_safe_drop" in report["verdicts"]
        assert "wgc_copy_pool_pressure" in report["verdicts"]
        assert "wgc_pool_slot_lifetime_fault" not in report["verdicts"]
        assert "ce_encoder_or_mux_backpressure" not in report["verdicts"]
        assert "ce_visual_timeline_fault" in report["verdicts"]
        assert report["faults"]["wgc_pool_saturated_safe_drop"]
        assert not report["faults"]["wgc_pool_slot_lifetime_fault"]

        wgc_ingress_decimated = make_session(
            "wgc_ingress_decimated",
            media=(
                "[WGC Perf] Input: 144 | Queued: 120 | DropFull: 0 | DropPace: 0 | DropThrottle: 0 | "
                "DropStale: 0 (DupTs=0 OOO=0) | DropCursor: 0 | DropPool: 0 | DropIngress: 24 | "
                "HostQ: 18 | EncQ: 1 | Dup: 0 | Late: 0 | SrcAvg: 6944us | JitAvg: 100us | JitMax: 500us | "
                "Src->Copy: 40/80us | Deliv: 120 | MinIn250/500: 144/144 | MinDel250/500: 120/120 | "
                "FreshMiss: 0pm | BufAvg: 900pm | BufMin: 18 | NoFresh: 0 | NoReserve: 0 | "
                "SchedSelAvg: 0us SchedSelBias: 0us | WgcSelAvg: 0us WgcSelBias: 0us | "
                "CbGap: 6944/7100us CbProc: 50/90us CbDrainMax: 0 | Copy: 50us | "
                "SlotAge: 8200us FastSlot: 0 | PoolLease: max=20 freeMin=4 satDrop=0 overwritePrevented=0 "
                "mismatch=0 sourceFramePoolBuffers=8 copyPoolSlots=24 budgetSurfaces=32 syncFrames=4 "
                "extraFrames=13 retainedCap=18 reservedFree=6 safetySlots=4 | "
                "Ingress: accepted=120 decimated=24 retained=18/18 lowWater=17 reason=wgc_ingress_decimated | "
                "KMFail: 0/0 | Flush: 0/0 | Dedicated: 1 | Encode: 300us | Fence: 0us | Throttle: 0 | "
                "Mux: 0KB | Overload: 0x0\n"
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=0 "
                "phaseErrorMax=0us shortfallMax=0.0ms staleDebtDrops=0 liveRebase=0/0 tooNewRepeats=0 "
                "syncDelayHolds=0 tooNewLeadMax=0us avDelay=33.3ms startupDelay=141.6ms "
                "scheduleOffset=0us effectiveDelay=141.6ms lowSourceBypass=0 modeMismatch=0 "
                "sourceBacktrack=0 syncDelaySourceLimitedHolds=0 syncDelayPolicyHolds=0 "
                "startupReserveFrames=18 startupReserveSpan=141000us startupDelayTarget=141600us "
                "startupReserveSelected=1 startupReserveReason=selected smoothBuf=1 smoothTargetMs=250 "
                "smoothFrames=13/13/30 smoothDelay=108.3ms smoothPoolSlots=24 sourceFramePoolBuffers=8 "
                "budgetSurfaces=32 syncFrames=4 extraFrames=13 retainedCap=18 reservedFreeSlots=6 safetySlots=4 "
                "retainedCapTrim=0 ingressAccepted=120 ingressDecimated=24 ingressRetained=18/18 "
                "ingressLowWater=17 leasedMax=20 freeMin=4 poolSaturatedDrops=0 overwritePrevented=0 "
                "leaseMismatches=0 smoothVramMB=2025.0 smoothCapLimited=1 smoothReason=vram_cap_limited\n"
                "[WGC CFR SMOOTHNESS DELAY] delayReservoirLowWaterFrames=17 delayReservoirTargetFrames=18 "
                "delayReservoirLowWaterTicks=0 realizedDelayAvg=141600us realizedDelayMin=141000us "
                "realizedDelayMax=142000us delayResidualAvg=0/1000us delayResidualMax=2000us "
                "delayResidualP95=1000us delayResidualLateMax=1000us delayResidualEarlyMax=1000us "
                "rawResidualAvg=0/1000us rawResidualMax=2000us rawResidualP95=1000us "
                "rawResidualLateMax=1000us rawResidualEarlyMax=1000us predictedResidualAvg=0/1000us "
                "predictedResidualP95=1000us predictedResidualLateMax=1000us rawMinusPredictedAvg=0/0us "
                "rawMinusPredictedMax=0us\n"
                "[VideoEncoder] Final packet timeline: target=1000 us videoEnd=1000 us audioMinEnd=1000 us "
                "audioMaxEnd=1000 us maxPacketDelta=0 us audioPastTarget=0\n"
                "[VideoEncoder] Final metadata durations: target=1000 us video=1000 us audioMin=1000 "
                "us audioMax=1000 us maxDelta=0 us streams(v=1 a=2) overload(encoder=0 mux=0) "
                "backpressure=0 peakMux=0KB peakPkts=0\n"
            ),
        )
        report = classify_session_triage(wgc_ingress_decimated)
        assert "wgc_ingress_decimated" in report["verdicts"]
        assert "wgc_copy_pool_pressure" in report["verdicts"]
        assert "wgc_pool_saturated_safe_drop" not in report["verdicts"]
        assert "ce_encoder_or_mux_backpressure" not in report["verdicts"]
        assert not report["faults"]["visual_timeline"]

        wgc_094302_pool_pressure = make_session(
            "wgc_094302_pool_pressure",
            media=(
                "[WGC Perf] Input: 120 | Queued: 84 | DropFull: 0 | DropPace: 0 | DropThrottle: 0 | "
                "DropStale: 0 (DupTs=0 OOO=0) | DropCursor: 0 | DropPool: 36 | HostQ: 24 | EncQ: 1 | "
                "Dup: 65 | Late: 0 | SrcAvg: 8333us | JitAvg: 500us | JitMax: 1200us | Src->Copy: 40/80us | "
                "Deliv: 84 | MinIn250/500: 120/120 | MinDel250/500: 84/84 | FreshMiss: 0pm | BufAvg: 900pm | "
                "BufMin: 0 | NoFresh: 0 | NoReserve: 0 | SchedSelAvg: 0us SchedSelBias: 0us | "
                "WgcSelAvg: 0us WgcSelBias: 0us | CbGap: 8333/9000us CbProc: 50/90us CbDrainMax: 0 | "
                "Copy: 50us | SlotAge: 8200us FastSlot: 0 | PoolLease: max=24 freeMin=0 satDrop=36 "
                "overwritePrevented=4000 mismatch=0 sourceFramePoolBuffers=8 copyPoolSlots=24 budgetSurfaces=32 "
                "syncFrames=4 extraFrames=16 safetySlots=4 | KMFail: 0/0 | Flush: 0/0 | Dedicated: 1 | "
                "Encode: 300us | Fence: 0us | Throttle: 0 | Mux: 0KB | Overload: 0x0\n"
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=1 "
                "phaseErrorMax=0us shortfallMax=0.0ms staleDebtDrops=0 liveRebase=0/0 tooNewRepeats=0 "
                "syncDelayHolds=0 tooNewLeadMax=0us avDelay=31.2ms startupDelay=163.5ms "
                "scheduleOffset=102699us effectiveDelay=163.5ms lowSourceBypass=0 modeMismatch=0 "
                "sourceBacktrack=0 syncDelaySourceLimitedHolds=0 syncDelayPolicyHolds=0 "
                "startupReserveFrames=24 startupReserveSpan=163489us startupDelayTarget=163489us "
                "startupReserveSelected=1 startupReserveReason=selected smoothBuf=1 smoothTargetMs=250 "
                "smoothFrames=16/16/30 smoothDelay=132.3ms smoothPoolSlots=24 sourceFramePoolBuffers=8 "
                "budgetSurfaces=32 syncFrames=4 safetySlots=4 leasedMax=24 freeMin=0 poolSaturatedDrops=766 "
                "overwritePrevented=117761 leaseMismatches=0 smoothVramMB=2025.0 smoothCapLimited=1 "
                "smoothReason=vram_cap_limited\n"
                "[VideoEncoder] Final packet timeline: target=111108333 us videoEnd=111108333 us "
                "audioMinEnd=111108333 us audioMaxEnd=111108333 us maxPacketDelta=0 us audioPastTarget=0\n"
                "[VideoEncoder] Final metadata durations: target=111108333 us video=111108333 us "
                "audioMin=111108333 us audioMax=111108333 us maxDelta=0 us streams(v=1 a=2) "
                "overload(encoder=0 mux=0) backpressure=0 peakMux=0KB peakPkts=0\n"
                "[VideoEncoder] Post-mux audio duration mismatch maxDelta=21333\n"
                "[VideoEncoder] Post-mux audio codec priming evidence (target=111108333 audioMinEnd=111129666 "
                "audioMaxEnd=111129666 maxDelta=21333 primingTolerance=21355 roundingTolerance=21)\n"
            ),
        )
        report = classify_session_triage(wgc_094302_pool_pressure)
        assert "wgc_pool_saturated_safe_drop" in report["verdicts"]
        assert "wgc_copy_pool_pressure" in report["verdicts"]
        assert "ce_encoder_or_mux_backpressure" not in report["verdicts"]
        assert "ce_audio_timeline_fault" in report["verdicts"]
        assert not report["evidence"]["rounding_evidence"]["post_mux_one_us_or_less_is_info"]

        wgc_quality_summary = make_session(
            "wgc_quality_summary",
            media=(
                "[WGC CFR QUALITY] duplicatePct=19.4 duplicates=1399/7203 worst1sUnique=71 "
                "worst1sRepeats=49 worst1sEmit=120 limiter=wgc_pool_pressure sourceLimitedRepeats=1399 "
                "poolPressure=1 freeMin=0 poolSaturatedDrops=6 ingressHard=103 ingressSoft=500 "
                "overwritePrevented=136402 dupTsSeen=492 dupTsSkipped=489 encoderOverload=0x0 "
                "muxBackpressure=0 compactRetained=1 sourceFmt=10 retainedFmt=24 convertUs=620 "
                "finalAvSync=exported_tracks_authoritative\n"
                "[VideoEncoder] Final packet timeline: target=60000000 us videoEnd=60000000 us "
                "audioMinEnd=60000000 us audioMaxEnd=60000000 us maxPacketDelta=0 us "
                "streams(v=1 a=3) audioPastTarget=0\n"
                "[VideoEncoder] Final metadata durations: target=60000000 us video=60000000 us "
                "audioMin=60000000 us audioMax=60000000 us maxDelta=0 us streams(v=1 a=3) "
                "overload(encoder=0 mux=0) backpressure=0\n"
            ),
        )
        report = classify_session_triage(wgc_quality_summary)
        quality = report["evidence"]["wgc_quality"][0]
        assert quality["limiter"] == "wgc_pool_pressure"
        assert quality["worst_1s_unique"] == 71
        assert quality["worst_1s_repeats"] == 49
        assert quality["compact_retained"] == 1
        assert quality["source_format"] == 10
        assert quality["retained_format"] == 24
        assert quality["duplicate_timestamps_seen"] == 492
        assert quality["duplicate_timestamps_skipped"] == 489
        assert report["evidence"]["exported_av_sync_ok"]
        assert "ce_audio_timeline_fault" not in report["verdicts"]

        wgc_source_coverage_best_effort = make_session(
            "wgc_source_coverage_best_effort",
            media=(
                "[WGC CFR SOURCE COVERAGE] coverage=limited reason=delivery_holes bestEffort=1 "
                "outputFps=120 duplicates=2976/16409 sourceLimitedRepeats=2976 "
                "sourceRepeatLowerBound=2976 syncSourceRepeatLowerBound=0 deliveryRepeatLowerBound=2976 "
                "excessRepeats=0 policyAddedRepeats=0 policyNoSourceRepeats=0 cleanEncoderMux=1 "
                "cleanPool=1 cleanSelection=1 encoderOverload=0x0 muxBackpressure=0 poolPressure=0 "
                "poolFreeMin=51 finalAvSync=exported_tracks_authoritative "
                "note=surplus_source_frames_are_dropped_when_available_repeats_mean_cfr_coverage_holes\n"
                "[VideoEncoder] Final metadata durations: target=1000 us video=1000 us audioMin=1000 "
                "us audioMax=1000 us maxDelta=0 us streams(v=1 a=2) overload(encoder=0 mux=0) "
                "backpressure=0 peakMux=0KB peakPkts=0\n"
            ),
        )
        report = classify_session_triage(wgc_source_coverage_best_effort)
        coverage = report["evidence"]["wgc_source_coverage"][0]
        assert coverage["coverage"] == "limited"
        assert coverage["reason"] == "delivery_holes"
        assert coverage["best_effort"] == 1
        assert coverage["source_repeat_lower_bound"] == 2976
        assert coverage["delivery_repeat_lower_bound"] == 2976
        assert coverage["excess_repeats"] == 0
        assert "wgc_source_coverage_best_effort" in report["verdicts"]
        assert "ce_audio_timeline_fault" not in report["verdicts"]
        assert "ce_visual_timeline_fault" not in report["verdicts"]

        dxgi_variable_fps_source_limited = make_session(
            "dxgi_variable_fps_source_limited",
            capture_method="dxgi_dup",
            media=(
                "[WGC Perf] Input: 15035 | Queued: 15035 | MinIn250/500: 12/48 | "
                "MinDel250/500: 12/48 | FreshMiss: 208pm | PoolLease: max=42 freeMin=22 "
                "satDrop=0 overwritePrevented=0 mismatch=0 | Backend: DxgiDuplication DupMissed: 22 | "
                "Overload: 0x0\n"
                "[WGC CFR SUMMARY] Live=12728 Dup=85 DupPct=0.6% NoFresh=6pm NoReserve=10pm "
                "DupReason(src=85 def=0 timer=0 drain=0) SourceLimitedRepeats=85 StarvedEpisodes=281 "
                "AntiFreezeFloor=0 AntiFreezeFloorSkippedSync=14 BiasClampCount=0 "
                "longest=938ms longestDup=10 longestContiguousDup=24 (200ms) worstIn=56 worstDel=56\n"
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=10 "
                "phaseErrorMax=54170us shortfallMax=0.0ms staleDebtDrops=143 liveRebase=0/0 "
                "tooNewRepeats=37 syncDelayHolds=37 tooNewLeadMax=0us avDelay=28.7ms "
                "startupDelay=48.6ms scheduleOffset=19908us effectiveDelay=48.6ms "
                "lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0 "
                "syncDelaySourceLimitedHolds=37 syncDelayPolicyHolds=0 startupReserveFrames=40 "
                "startupReserveSpan=270894us startupDelayTarget=328710us startupReserveSelected=0 "
                "startupReserveReason=partial_span_timeout smoothBuf=1 smoothTargetMs=300 "
                "smoothFrames=2/45/45 smoothDelay=19.9ms smoothPoolSlots=64 sourceFramePoolBuffers=0 "
                "budgetSurfaces=94 syncFrames=5 extraFrames=45 retainedCap=58 reservedFreeSlots=6 "
                "safetySlots=4 retainedCapTrim=0 ingressAccepted=15035 ingressDecimated=0 "
                "ingressPlaySoft=0 ingressPlayCredit=0 ingressRetained=6/58 ingressLowWater=6 "
                "leasedMax=42 freeNow=64 freeMin=22 poolPressureTrim=0 poolSaturatedDrops=0 "
                "overwritePrevented=0 leaseMismatches=0 smoothVramMB=2025.0 smoothCapLimited=0 "
                "smoothReason=startup_attempt\n"
                "[WGC CFR SMOOTHNESS BUFFER] smoothTargetDelay=299998us smoothActualDelay=19908us "
                "smoothDelayDeficit=280090us startupDelayTarget=328710us effectiveDelay=48619us "
                "startupDelayDeficit=280091us finalAvSync=exported_tracks_authoritative\n"
                "[WGC CFR SMOOTHNESS DELAY] delayReservoirLowWaterFrames=6 delayReservoirTargetFrames=7 "
                "delayReservoirLowWaterTicks=130 realizedDelayAvg=48601us realizedDelayMin=38874us "
                "realizedDelayMax=53330us delayResidualAvg=17/1793us delayResidualMax=9745us "
                "delayResidualP95=3000us delayResidualLateMax=9745us delayResidualEarlyMax=4711us "
                "rawResidualAvg=-2836/3799us rawResidualMax=10710us rawResidualP95=7000us "
                "rawResidualLateMax=10710us rawResidualEarlyMax=9800us predictedResidualAvg=17/1793us "
                "predictedResidualP95=3000us predictedResidualLateMax=9745us "
                "rawMinusPredictedAvg=-2854/2854us rawMinusPredictedMax=6250us\n"
                "[WGC CFR SMOOTHNESS REPEAT] delaySoftLateAccepted=0 delayRepeatSoftSafeCandidate=0 "
                "delaySyncProtectedRepeats=37 delayPostSelectionRejectedSync=0\n"
                "[WGC CFR SMOOTHNESS VERDICT] delayPostSelectionRejectedSync=0 "
                "delayPostSelectionRescuedSync=0 sourceRepeatLowerBound=85 excessRepeats=0 "
                "policyAddedRepeats=0 excessRepeatClusters=0 excessRepeatClusterMax=0 "
                "smoothnessNotMaximal=0 mixedPolicyFault=0 syncSourceRepeatLowerBound=37 "
                "deliveryRepeatLowerBound=85 policyNoSourceRepeats=0\n"
                "[WGC CFR QUALITY] duplicatePct=0.6 duplicates=85/12728 worst1sUnique=95 "
                "worst1sRepeats=25 worst1sEmit=120 limiter=source_limited sourceLimitedRepeats=85 "
                "poolPressure=0 freeMin=22 poolSaturatedDrops=0 ingressHard=0 ingressSoft=0 "
                "ingressDecimated=0 policyAddedRepeats=0 excessRepeats=0 "
                "smoothDelayDeficitUs=280090 startupDelayDeficitUs=280091 encoderOverload=0x0 "
                "muxBackpressure=0 backend=dxgi_dup finalAvSync=exported_tracks_authoritative\n"
                "[WGC CFR SOURCE COVERAGE] coverage=limited reason=source_and_delivery_holes bestEffort=1 "
                "outputFps=120 duplicates=85/12728 sourceLimitedRepeats=85 sourceRepeatLowerBound=85 "
                "syncSourceRepeatLowerBound=37 deliveryRepeatLowerBound=85 excessRepeats=0 "
                "policyAddedRepeats=0 policyNoSourceRepeats=0 cleanEncoderMux=1 cleanPool=1 "
                "cleanSelection=1 encoderOverload=0x0 muxBackpressure=0 poolPressure=0 poolFreeMin=22 "
                "finalAvSync=exported_tracks_authoritative\n"
                "[VideoEncoder] Final packet timeline: target=106075000 us videoEnd=106075000 us "
                "audioMinEnd=106074999 us audioMaxEnd=106074999 us maxPacketDelta=1 us "
                "streams(v=1 a=3) audioPastTarget=0\n"
                "[VideoEncoder] Final metadata durations: target=106075000 us video=106075000 us "
                "audioMin=106074999 us audioMax=106074999 us maxDelta=1 us streams(v=1 a=3) "
                "overload(encoder=0 mux=0) backpressure=0\n"
            ),
            hook="DetourPresent: heartbeat #1 gap=757ms presentOwner=0x0 depth=0 slFG=0 tid=0x1\n",
            perf=(
                "frame,qpc_us,total_us,capture_us,present_call_us,mux_queue_kb,overload_flags,api,"
                "source_frame_index,capture_phase,qpc_delta_us\n"
                "1,1000000,100,0,0,0,0,DX12,0,0,0\n"
                "2,1757000,100,0,0,0,0,DX12,0,0,757000\n"
                "3,1765000,100,0,0,0,0,DX12,0,1,8000\n"
                "4,1993852,100,0,0,0,0,DX12,0,2,228852\n"
                "5,2002185,100,0,0,0,0,DX12,0,2,8333\n"
            ),
        )
        report = classify_session_triage(dxgi_variable_fps_source_limited)
        assert report["evidence"]["screen_capture_backend"] == "dxgi_dup"
        assert report["evidence"]["present_gap_source"] == "perf_live_source"
        assert report["evidence"]["max_present_gap_ms"] == 228.852
        assert report["evidence"]["present_gap_filter_kind"] == "capture_phase"
        assert "dxgi_dup_source_starvation" in report["verdicts"]
        assert "dxgi_dup_source_coverage_best_effort" in report["verdicts"]
        assert not any(verdict.startswith("wgc_") for verdict in report["verdicts"])
        assert "duplication_consumer_starvation" not in report["verdicts"]
        assert "dxgi_dup_delivery_gap" in report["contexts"]
        assert "dxgi_dup_startup_reservoir_partial" in report["contexts"]
        assert "dxgi_dup_source_limited_delay_variation" in report["contexts"]
        assert "dxgi_dup_av_sync_delay_residual" not in report["verdicts"]
        assert "dxgi_dup_timestamp_domain_mismatch" not in report["verdicts"]
        assert "dxgi_dup_active_delay_realized_delay_unstable" not in report["verdicts"]
        assert "ce_visual_timeline_fault" not in report["verdicts"]
        assert not report["faults"]["visual_timeline"]
        assert report["faults"]["wgc_clean_source_limited_coverage"]
        assert report["faults"]["wgc_source_limited_delay_variation_context"]

        dxgi_desktop_source_limited = make_session(
            "dxgi_desktop_source_limited",
            capture_method="dxgi_dup",
            media=(
                "[WGC CFR SUMMARY] Live=47626 Dup=31350 DupPct=65.8% NoFresh=658pm NoReserve=972pm "
                "DupReason(src=31347 def=0 timer=3 drain=0) SourceLimitedRepeats=31347 StarvedEpisodes=8 "
                "AntiFreezeFloor=1 AntiFreezeFloorSkippedSync=6130 BiasClampCount=0 "
                "longest=236828ms longestDup=19031 longestContiguousDup=15 (125ms) worstIn=8 worstDel=8\n"
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=394 "
                "phaseErrorMax=274760us shortfallMax=0.0ms staleDebtDrops=32 liveRebase=0/0 "
                "tooNewRepeats=31350 syncDelayHolds=31350 tooNewLeadMax=0us avDelay=28.5ms "
                "startupDelay=256.5ms scheduleOffset=227966us effectiveDelay=256.5ms "
                "lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0 "
                "syncDelaySourceLimitedHolds=31350 syncDelayPolicyHolds=0 startupReserveFrames=11 "
                "startupReserveSpan=256479us startupDelayTarget=328511us startupReserveSelected=0 "
                "startupReserveReason=partial_span_timeout smoothBuf=1 smoothTargetMs=300 "
                "smoothFrames=27/45/45 smoothDelay=228.0ms smoothPoolSlots=64 sourceFramePoolBuffers=0 "
                "budgetSurfaces=94 syncFrames=5 extraFrames=45 retainedCap=58 reservedFreeSlots=6 "
                "safetySlots=4 retainedCapTrim=0 ingressAccepted=16745 ingressDecimated=0 "
                "ingressPlaySoft=0 ingressPlayCredit=0 ingressRetained=30/58 ingressLowWater=31 "
                "leasedMax=39 freeNow=64 freeMin=25 poolPressureTrim=0 poolSaturatedDrops=0 "
                "overwritePrevented=0 leaseMismatches=0 smoothVramMB=2025.0 smoothCapLimited=0 "
                "smoothReason=startup_attempt_source_rate_low\n"
                "[WGC CFR SMOOTHNESS BUFFER] smoothTargetDelay=299998us smoothActualDelay=227966us "
                "smoothDelayDeficit=72032us startupDelayTarget=328511us effectiveDelay=256479us "
                "startupDelayDeficit=72032us finalAvSync=exported_tracks_authoritative\n"
                "[WGC CFR SMOOTHNESS DELAY] delayReservoirLowWaterFrames=31 delayReservoirTargetFrames=32 "
                "delayReservoirLowWaterTicks=46304 realizedDelayAvg=253512us realizedDelayMin=229874us "
                "realizedDelayMax=261339us delayResidualAvg=2966/3255us delayResidualMax=26605us "
                "delayResidualP95=7000us delayResidualLateMax=26605us delayResidualEarlyMax=4860us "
                "rawResidualAvg=3360/5376us rawResidualMax=29286us rawResidualP95=11000us "
                "rawResidualLateMax=29286us rawResidualEarlyMax=10746us "
                "predictedResidualAvg=2966/3255us predictedResidualP95=7000us "
                "predictedResidualLateMax=26605us rawMinusPredictedAvg=393/393us "
                "rawMinusPredictedMax=6250us\n"
                "[WGC CFR SMOOTHNESS REPEAT] delaySoftLateAccepted=0 delayNearCapAccepted=1 "
                "delayRepeatSoftSafeCandidate=0 delaySyncProtectedRepeats=31350 "
                "delayPostSelectionRejectedSync=0\n"
                "[WGC CFR SMOOTHNESS VERDICT] delayPostSelectionRejectedSync=0 "
                "delayPostSelectionRescuedSync=0 sourceRepeatLowerBound=31350 excessRepeats=0 "
                "policyAddedRepeats=0 excessRepeatClusters=0 excessRepeatClusterMax=0 "
                "smoothnessNotMaximal=0 mixedPolicyFault=0 syncSourceRepeatLowerBound=31350 "
                "deliveryRepeatLowerBound=31347 policyNoSourceRepeats=0\n"
                "[WGC CFR QUALITY] duplicatePct=65.8 duplicates=31350/47626 worst1sUnique=10 "
                "worst1sRepeats=122 worst1sEmit=132 limiter=source_limited sourceLimitedRepeats=31347 "
                "poolPressure=0 freeMin=25 poolSaturatedDrops=0 ingressHard=0 ingressSoft=0 "
                "ingressDecimated=0 policyAddedRepeats=0 excessRepeats=0 smoothDelayDeficitUs=72032 "
                "startupDelayDeficitUs=72032 encoderOverload=0x0 muxBackpressure=0 backend=dxgi_dup "
                "finalAvSync=exported_tracks_authoritative\n"
                "[WGC CFR SOURCE COVERAGE] coverage=limited reason=source_and_delivery_holes bestEffort=1 "
                "outputFps=120 duplicates=31350/47626 sourceLimitedRepeats=31347 "
                "sourceRepeatLowerBound=31350 syncSourceRepeatLowerBound=31350 "
                "deliveryRepeatLowerBound=31347 excessRepeats=0 policyAddedRepeats=0 "
                "policyNoSourceRepeats=0 cleanEncoderMux=1 cleanPool=1 cleanSelection=1 "
                "encoderOverload=0x0 muxBackpressure=0 poolPressure=0 poolFreeMin=25 "
                "finalAvSync=exported_tracks_authoritative\n"
                "[VideoEncoder] Final packet timeline: target=396891667 us videoEnd=396891667 us "
                "audioMinEnd=396891666 us audioMaxEnd=396891666 us maxPacketDelta=1 us "
                "streams(v=1 a=3) audioPastTarget=0\n"
                "[VideoEncoder] Final metadata durations: target=396891667 us video=396891667 us "
                "audioMin=396891666 us audioMax=396891666 us maxDelta=1 us streams(v=1 a=3) "
                "overload(encoder=0 mux=0) backpressure=0\n"
            ),
        )
        report = classify_session_triage(dxgi_desktop_source_limited)
        assert "dxgi_dup_source_starvation" in report["verdicts"]
        assert "dxgi_dup_source_coverage_best_effort" in report["verdicts"]
        assert "dxgi_dup_source_limited_delay_variation" in report["contexts"]
        assert "dxgi_dup_av_sync_delay_residual" not in report["verdicts"]
        assert "dxgi_dup_audio_late_risk" not in report["verdicts"]
        assert "dxgi_dup_active_delay_realized_delay_unstable" not in report["verdicts"]
        assert "ce_visual_timeline_fault" not in report["verdicts"]
        assert not report["faults"]["visual_timeline"]
        assert report["faults"]["wgc_clean_source_limited_coverage"]
        assert report["faults"]["wgc_source_limited_delay_variation_context"]

        near_lower_bound_evidence = parse_media_triage(
            read_text_if_exists(dxgi_variable_fps_source_limited / "media.log")
        )
        near_lower_bound_summary = near_lower_bound_evidence["wgc_smoothness_summary"][0]
        near_lower_bound_summary.update(
            {
                "live": 0,
                "source_repeat_lower_bound": 107,
                "excess_repeats": 5,
                "realized_delay_min_us": 42497,
                "realized_delay_max_us": 56283,
            }
        )
        near_lower_bound_coverage = near_lower_bound_evidence["wgc_source_coverage"][0]
        near_lower_bound_coverage.update(
            {
                "best_effort": 0,
                "duplicates": 112,
                "live": 16117,
                "source_repeat_lower_bound": 107,
                "excess_repeats": 5,
                "clean_selection": 0,
            }
        )
        assert not has_wgc_clean_source_limited_coverage(near_lower_bound_evidence)
        assert has_wgc_source_limited_playout_maximal(near_lower_bound_evidence)
        assert has_wgc_source_limited_delay_variation_context(near_lower_bound_evidence)
        assert not has_wgc_active_delay_realized_delay_unstable(near_lower_bound_evidence)

        wgc_pool_lifetime_fault = make_session(
            "wgc_pool_lifetime_fault",
            media=(
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=0 "
                "phaseErrorMax=0us shortfallMax=0.0ms staleDebtDrops=0 liveRebase=0/0 tooNewRepeats=0 "
                "syncDelayHolds=0 tooNewLeadMax=0us avDelay=33.3ms startupDelay=166.6ms "
                "scheduleOffset=0us effectiveDelay=166.6ms lowSourceBypass=0 modeMismatch=0 "
                "sourceBacktrack=0 syncDelaySourceLimitedHolds=0 syncDelayPolicyHolds=0 "
                "startupReserveFrames=24 startupReserveSpan=166000us startupDelayTarget=166600us "
                "startupReserveSelected=1 startupReserveReason=selected smoothBuf=1 smoothTargetMs=250 "
                "smoothFrames=16/16/30 smoothDelay=133.3ms smoothPoolSlots=24 sourceFramePoolBuffers=8 "
                "budgetSurfaces=32 syncFrames=4 safetySlots=4 leasedMax=20 freeMin=4 "
                "poolSaturatedDrops=0 overwritePrevented=0 leaseMismatches=1 smoothVramMB=2025.0 "
                "smoothCapLimited=1 smoothReason=vram_cap_limited\n"
            ),
        )
        report = classify_session_triage(wgc_pool_lifetime_fault)
        assert "wgc_pool_slot_lifetime_fault" in report["verdicts"]
        assert "ce_visual_timeline_fault" in report["verdicts"]
        assert report["faults"]["wgc_pool_slot_lifetime_fault"]

        wgc_pool_evidence_missing = make_session(
            "wgc_pool_evidence_missing",
            media=(
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=0 "
                "phaseErrorMax=0us shortfallMax=0.0ms staleDebtDrops=0 liveRebase=0/0 tooNewRepeats=0 "
                "syncDelayHolds=0 tooNewLeadMax=0us avDelay=33.3ms startupDelay=166.6ms "
                "scheduleOffset=0us effectiveDelay=166.6ms lowSourceBypass=0 modeMismatch=0 "
                "sourceBacktrack=0 syncDelaySourceLimitedHolds=0 syncDelayPolicyHolds=0 "
                "startupReserveFrames=20 startupReserveSpan=166000us startupDelayTarget=166600us "
                "startupReserveSelected=1 startupReserveReason=selected delayReservoirLowWaterFrames=4 "
                "delayReservoirTargetFrames=5 delayReservoirLowWaterTicks=0 realizedDelayAvg=166600us "
                "realizedDelayMin=165000us realizedDelayMax=168000us delayResidualAvg=0/1000us "
                "delayResidualMax=2000us delayResidualP95=1000us delayResidualLateMax=1000us "
                "delayResidualEarlyMax=1000us smoothBuf=1 smoothTargetMs=250 "
                "smoothFrames=16/16/30 smoothDelay=133.3ms smoothPoolSlots=20 smoothVramMB=2025.0 "
                "smoothCapLimited=1 smoothReason=vram_cap_limited\n"
                "[VideoEncoder] Final metadata durations: target=1000 us video=1000 us audioMin=1000 "
                "us audioMax=1000 us maxDelta=0 us streams(v=1 a=2) overload(encoder=0 mux=0) "
                "backpressure=0 peakMux=0KB peakPkts=0\n"
            ),
        )
        report = classify_session_triage(wgc_pool_evidence_missing)
        assert "wgc_pool_evidence_missing" in report["verdicts"]
        assert "ce_encoder_or_mux_backpressure" not in report["verdicts"]

        source_gap_windowed = make_session(
            "source_gap_windowed",
            media=(
                "[AVSyncApply] wgc_schedule_anchor: videoQpc=9700000 audioAnchorQpc=9700000 "
                "liveStartQpc=10000000 requestedDelayUs=0 startupDelayUs=0 scheduleOffsetUs=0\n"
                "[VideoEncoder] Final metadata durations: target=1000 us video=1000 us audioMin=1000 "
                "us audioMax=1000 us maxDelta=0 us streams(v=1 a=2) overload(encoder=0 mux=0) "
                "backpressure=0 peakMux=0KB peakPkts=0\n"
            ),
            hook="DetourPresent: heartbeat #1 gap=299ms presentOwner=0x0000 depth=0 slFG=0 tid=0x1234\n",
            perf=(
                "frame,qpc_us,total_us,capture_us,present_call_us,mux_queue_kb,overload_flags,api,qpc_delta_us\n"
                "1,25990000,100,20,40,0,0,DX12,0\n"
                "2,26000000,100,20,40,0,0,DX12,10000\n"
                "3,26010000,100,20,40,0,0,DX12,10000\n"
            ),
        )
        report = classify_session_triage(source_gap_windowed, recording_window="25:45")
        assert "source_present_gap" not in report["verdicts"]
        assert report["evidence"]["present_gap_source"] == "perf_recording_window"
        assert report["evidence"]["max_present_gap_ms"] <= 11.0

        window_audio_noise = make_session(
            "window_audio_noise",
            media=(
                "[2026-01-01 00:00:00.000] [INFO] [EncoderThread] Recording live "
                "(WGC, hiddenFrames=0, bufferedInject=0)\n"
                "[2026-01-01 00:00:00.100] [INFO] [AVSyncApply] wgc_schedule_anchor: "
                "videoQpc=99900000 audioAnchorQpc=100000000 liveStartQpc=100000000 "
                "requestedDelayUs=1000 startupDelayUs=1000 scheduleOffsetUs=0 "
                "selectionOffsetUs=0 renderDelayUs=1000 smoothExtraDelayUs=0 confidence=high reason=test\n"
                "[2026-01-01 00:00:02.500] [INFO] [Media] [WGC CFR] Source-starved episode: "
                "duration=120ms out=14 dup=12 minIn=60 minDel=120 freshMiss=1000pm minBuf=0\n"
                "[2026-01-01 00:00:05.500] [INFO] [Media] [PullAudio] WARNING: Source underrun - "
                "src 4 padding 400 samples with silence (available=0 needed=400 forceDrain=0)\n"
                "[2026-01-01 00:00:05.600] [INFO] [Media] [A/V ZERO DRIFT WARNING] Track 1 "
                "residual_samples=-480 residual_us=-10000 target_samples=4800 cursor_samples=4320\n"
                "[2026-01-01 00:00:06.000] [INFO] [Media] [VideoEncoder] Final packet timeline: "
                "target=6000000 us videoEnd=6000000 us audioMinEnd=6000000 us audioMaxEnd=6000000 us "
                "maxPacketDelta=0 us streams(v=1 a=1) audioPastTarget=0\n"
                "[2026-01-01 00:00:06.000] [INFO] [Media] [VideoEncoder] Final metadata durations: "
                "target=6000000 us video=6000000 us audioMin=6000000 us audioMax=6000000 us "
                "maxDelta=0 us streams(v=1 a=1) overload(encoder=0 mux=0) backpressure=0\n"
            ),
            perf=(
                "frame,qpc_us,total_us,capture_us,present_call_us,mux_queue_kb,overload_flags,api\n"
                "1,102000000,100,20,40,0,0,DX12\n"
                "2,103000000,100,20,40,0,0,DX12\n"
            ),
        )
        report = classify_session_triage(window_audio_noise, recording_window="2:4")
        assert report["evidence"]["recording_window"]["active"]
        assert "wgc_source_starvation" in report["verdicts"]
        assert "started_app_source_underrun" not in report["verdicts"]
        assert "ce_audio_timeline_fault" not in report["verdicts"]
        assert report["evidence"]["final_packet_timelines"][0]["max_packet_delta_us"] == 0

        wgc_starved = make_session(
            "wgc_starved",
            media=(
                "[WGC CFR] Source-starved episode: duration=1016ms out=121 dup=34 minIn=4 minDel=4 freshMiss=465pm minBuf=0\n"
                "[WGC CFR SUMMARY] Live=5791 Dup=151 DupPct=2.6% NoFresh=10pm NoReserve=0pm DupReason(src=151 def=0 timer=0 drain=0) SourceLimitedRepeats=151 StarvedEpisodes=319 longest=1109ms longestDup=34 worstIn=4 worstDel=4\n"
                "[VideoEncoder] Final metadata durations: target=1000 us video=1000 us audioMin=1000 us audioMax=1000 us maxDelta=0 us streams(v=1 a=2) overload(encoder=0 mux=0) backpressure=0 peakMux=0KB peakPkts=0\n"
            ),
        )
        report = classify_session_triage(wgc_starved)
        assert "wgc_source_starvation" in report["verdicts"]
        assert "ce_encoder_or_mux_backpressure" not in report["verdicts"]
        assert report["evidence"]["wgc_source_limits"]["detail_episode_count"] == 1
        assert report["evidence"]["wgc_source_limits"]["summary_starved_episodes"] == 319
        assert report["evidence"]["wgc_source_limits"]["summary_source_limited_repeats"] == 151

        wgc_false_capacity_label = make_session(
            "wgc_false_capacity_label",
            media=(
                "[WGC CFR ATTRIBUTION] fault_hint=ce_capacity_pressure qpc=1..2 duration=922ms out=110 dup=12 "
                "minIn=84 minDel=84 freshMiss=1000pm minBuf=1 cbGapMax=110189us encEmaMax=0.31ms "
                "muxBp=0 waitMax=0us muxMax=0KB overload=0x0 copyMax=1us copyHealth=ok fenceMax=0us "
                "fenceHealth=ok\n"
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=4 maxDropTicks=1 cadenceEvents=1 "
                "phaseErrorMax=0us shortfallMax=50.0ms staleDebtDrops=0 liveRebase=0/4 tooNewRepeats=0 "
                "syncDelayHolds=0 tooNewLeadMax=0us avDelay=29.4ms startupDelay=29.4ms "
                "scheduleOffset=0us effectiveDelay=29.4ms lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0 "
                "syncDelaySourceLimitedHolds=0 syncDelayPolicyHolds=0 startupReserveFrames=4 "
                "startupReserveSpan=29000us startupDelayTarget=29400us startupReserveSelected=1 "
                "startupReserveReason=selected\n"
                "[VideoEncoder] Final metadata durations: target=1000 us video=1000 us audioMin=1000 "
                "us audioMax=1000 us maxDelta=0 us streams(v=1 a=2) overload(encoder=0 mux=0) "
                "backpressure=0 peakMux=0KB peakPkts=0\n"
            ),
        )
        report = classify_session_triage(wgc_false_capacity_label)
        assert "ce_encoder_or_mux_backpressure" not in report["verdicts"]
        assert "wgc_encoder_limited_judder" not in report["verdicts"]

        wgc_delivery_gap = make_session(
            "wgc_delivery_gap",
            media=(
                "[WGC CFR ATTRIBUTION] fault_hint=wgc_delivery_gap qpc=1..2 duration=250ms out=30 dup=12 "
                "minIn=120 minDel=84 freshMiss=400pm minBuf=2 cbGapMax=48000us encEmaMax=0.31ms "
                "muxBp=0 waitMax=0us muxMax=0KB overload=0x0 copyMax=1us copyHealth=ok fenceMax=0us "
                "fenceHealth=ok poolSat=0 overwritePrevented=0 ingressDecimated=0\n"
                "[VideoEncoder] Final metadata durations: target=1000 us video=1000 us audioMin=1000 "
                "us audioMax=1000 us maxDelta=0 us streams(v=1 a=2) overload(encoder=0 mux=0) "
                "backpressure=0 peakMux=0KB peakPkts=0\n"
            ),
        )
        report = classify_session_triage(wgc_delivery_gap)
        assert "wgc_delivery_gap" in report["verdicts"]
        assert "wgc_framepool_pressure" not in report["verdicts"]
        assert "ce_encoder_or_mux_backpressure" not in report["verdicts"]
        assert "ce_visual_timeline_fault" not in report["verdicts"]

        wgc_framepool_pressure = make_session(
            "wgc_framepool_pressure",
            media=(
                "[WGC CFR ATTRIBUTION] fault_hint=wgc_framepool_pressure qpc=1..2 duration=250ms out=30 dup=12 "
                "minIn=120 minDel=84 freshMiss=400pm minBuf=2 cbGapMax=48000us encEmaMax=0.31ms "
                "muxBp=0 waitMax=0us muxMax=0KB overload=0x0 copyMax=1us copyHealth=ok fenceMax=0us "
                "fenceHealth=ok poolSat=1 overwritePrevented=0 ingressDecimated=0\n"
                "[VideoEncoder] Final metadata durations: target=1000 us video=1000 us audioMin=1000 "
                "us audioMax=1000 us maxDelta=0 us streams(v=1 a=2) overload(encoder=0 mux=0) "
                "backpressure=0 peakMux=0KB peakPkts=0\n"
            ),
        )
        report = classify_session_triage(wgc_framepool_pressure)
        assert "wgc_framepool_pressure" in report["verdicts"]
        assert "ce_visual_timeline_fault" in report["verdicts"]
        assert "ce_encoder_or_mux_backpressure" not in report["verdicts"]

        wgc_delivery_limited_lower_bound = make_session(
            "wgc_delivery_limited_lower_bound",
            media=(
                "[WGC CFR SUMMARY] Live=1000 Dup=120 DupPct=12.0% NoFresh=120pm NoReserve=0pm "
                "DupReason(src=120 def=0 timer=0 drain=0) SourceLimitedRepeats=120 StarvedEpisodes=0 "
                "longest=0ms longestDup=0 worstIn=120 worstDel=84\n"
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=0 "
                "phaseErrorMax=0us shortfallMax=0.0ms staleDebtDrops=0 liveRebase=0/0 tooNewRepeats=0 "
                "syncDelayHolds=0 tooNewLeadMax=0us avDelay=0.0ms startupDelay=0.0ms "
                "scheduleOffset=0us effectiveDelay=0.0ms lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0 "
                "syncDelaySourceLimitedHolds=0 syncDelayPolicyHolds=0 startupReserveFrames=0 "
                "startupReserveSpan=0us startupDelayTarget=0us startupReserveSelected=0 "
                "startupReserveReason=inactive\n"
                "[WGC CFR SMOOTHNESS VERDICT] delayPostSelectionRejectedSync=0 "
                "delayPostSelectionRescuedSync=0 sourceRepeatLowerBound=120 excessRepeats=0 "
                "policyAddedRepeats=0 excessRepeatClusters=0 excessRepeatClusterMax=0 "
                "smoothnessNotMaximal=0 mixedPolicyFault=0 syncSourceRepeatLowerBound=0 "
                "deliveryRepeatLowerBound=120\n"
                "[VideoEncoder] Final metadata durations: target=1000 us video=1000 us audioMin=1000 "
                "us audioMax=1000 us maxDelta=0 us streams(v=1 a=2) overload(encoder=0 mux=0) "
                "backpressure=0 peakMux=0KB peakPkts=0\n"
            ),
        )
        report = classify_session_triage(wgc_delivery_limited_lower_bound)
        smoothness = report["evidence"]["wgc_smoothness_summary"][0]
        assert smoothness["source_repeat_lower_bound"] == 120
        assert smoothness["sync_source_repeat_lower_bound"] == 0
        assert smoothness["delivery_repeat_lower_bound"] == 120
        assert smoothness["policy_added_repeats"] == 0
        assert "wgc_cfr_smoothness_not_maximal" not in report["verdicts"]
        assert "ce_visual_timeline_fault" not in report["verdicts"]

        inject_pacer = make_session(
            "inject_pacer",
            media=(
                "[Inject Perf] Input: 160 | Queued: 75 | DropFull: 0 | DropPace: 85 | PubFps: 240 | HostQ: 0 | "
                "EncQ: 0 | Dup: 5 | Late: 0 | Trim: 0 | SelDrop: 20 | Def: 0 | Encode: 308us | Fence: 2us | "
                "Mux: 0KB | Overload: 0x0\n"
                "[Inject CFR SUMMARY] Live=582 Dup=48 DupPct=8.2% DupReason(src=48 def=0 timer=0 drain=0) "
                "FreshCatchup=3 RepeatCatchup=1 StaleTrim=7 Recovery=0/1 PathMismatch=0/0 DefRequeued=0 "
                "DefDropped=0\n"
                "[Inject CFR SUMMARY] SourceFps=71.13..79.26 JitterMax=1373us SelMax=67148us EncEmaMax=0.55ms "
                "SustainMin=1818.8fps\n"
            ),
        )
        report = classify_session_triage(inject_pacer)
        assert "ce_capture_pacer_limited" in report["verdicts"]
        assert "ce_visual_timeline_fault" in report["verdicts"]
        assert report["faults"]["visual_timeline"]
        assert report["evidence"]["inject_pacing"]["drop_pace"] == 85
        assert report["evidence"]["inject_pacing"]["publication_fps"] == 240
        assert report["evidence"]["inject_pacing"]["summary_dup_src"] == 48
        assert report["evidence"]["inject_pacing"]["summary_recovery_episodes"] == 1

        inject_recovery_stalled = make_session(
            "inject_recovery_stalled",
            media="[Inject CFR] Recovery still active: duration=5000ms debt=36 start=36 best=35\n",
        )
        report = classify_session_triage(inject_recovery_stalled)
        assert report["evidence"]["visual_fault_counts"]["inject_cfr_recovery_stalled"] == 1
        assert report["faults"]["visual_timeline"]

        inject_planned_source_stall = make_session(
            "inject_planned_source_stall",
            media=(
                "[Inject Perf] Input: 2005 | Queued: 2004 | DropFull: 0 | DropPace: 1 | PubFps: 240 | HostQ: 0 | "
                "EncQ: 1 | Dup: 16 | Late: 0 | Trim: 1 | SelDrop: 1500 | Def: 0 | Encode: 308us | Fence: 2us | "
                "Mux: 0KB | Overload: 0x0\n"
                "[Inject CFR SUMMARY] Live=574 Dup=16 DupPct=2.7% DupReason(src=16 def=0 timer=0 drain=0) "
                "FreshCatchup=0 RepeatCatchup=0 StaleTrim=4 PathMismatch=0/0 DefRequeued=0 DefDropped=0\n"
                "[Inject CFR SUMMARY] SourceFps=24.00..240.83 JitterMax=22684us SelMax=528us EncEmaMax=0.59ms "
                "SustainMin=1701.7fps\n"
            ),
        )
        report = classify_session_triage(inject_planned_source_stall)
        assert "ce_capture_pacer_limited" not in report["verdicts"]
        assert "ce_visual_timeline_fault" not in report["verdicts"]

        inject_playout_churn = make_session(
            "inject_playout_churn",
            media=(
                "[Inject CFR SUMMARY] Live=14239 Dup=882 DupPct=6.1% "
                "DupReason(src=882 def=0 timer=0 drain=0) FreshCatchup=0 RepeatCatchup=0 "
                "StaleTrim=783 Recovery=0/0\n"
                "[Inject CFR SUMMARY] SourceFps=116.20..122.08 JitterMax=8000us SelMax=7000us\n"
            ),
        )
        report = classify_session_triage(inject_playout_churn)
        assert "inject_cfr_playout_churn" in report["verdicts"]
        assert "ce_visual_timeline_fault" in report["verdicts"]

        inject_target_quality = make_session(
            "inject_target_quality",
            media=(
                "[Inject CFR SUMMARY] Live=12000 Dup=0 DupPct=0.0% "
                "DupReason(src=0 def=0 timer=0 drain=0) FreshCatchup=0 RepeatCatchup=0 "
                "StaleTrim=0 Recovery=0/0\n"
                "[Inject CFR SUMMARY] SourceFps=119.80..120.20 JitterMax=300us SelMax=100us\n"
                "[Inject CFR QUALITY SUMMARY] TargetSelect=12000 Superseded=0 TargetHold=0 "
                "HoldWithCandidate=0 BufferCapTrim=0 TargetResidualMax=100us\n"
            ),
        )
        report = classify_session_triage(inject_target_quality)
        assert "inject_cfr_target_policy_hold" not in report["verdicts"]
        assert report["evidence"]["inject_pacing"]["target_select"] == 12000
        assert report["evidence"]["inject_pacing"]["target_residual_max_us"] == 100

        inject_post_hitch_phase_churn = make_session(
            "inject_post_hitch_phase_churn",
            media=(
                "[Inject CFR SUMMARY] Live=13545 Dup=354 DupPct=2.6% "
                "DupReason(src=354 def=0 timer=0 drain=0) FreshCatchup=0 RepeatCatchup=0 "
                "StaleTrim=0 Recovery=0/0\n"
                "[Inject CFR SUMMARY] SourceFps=98.08..122.39 JitterMax=796us SelMax=33907us\n"
                "[Inject CFR QUALITY SUMMARY] TargetSelect=13190 Superseded=255 TargetHold=354 "
                "HoldWithCandidate=267 BufferCapTrim=0 TargetResidualMax=220577us\n"
                "[2026-07-17 16:53:45.000] [INFO] [Inject CFR] Repeat pressure: dup=5 srcLimited=5 "
                "targetSelect=115 targetSuperseded=5 targetHold=5 holdWithCandidate=5 tickEmit=120 "
                "unique=115 sourceFps=119.95 overload=0x0\n"
                "[2026-07-17 16:53:50.000] [INFO] [Inject CFR] Repeat pressure: dup=6 srcLimited=6 "
                "targetSelect=114 targetSuperseded=5 targetHold=6 holdWithCandidate=6 tickEmit=120 "
                "unique=114 sourceFps=120.78 overload=0x0\n"
                "[2026-07-17 16:53:55.000] [INFO] [Inject CFR] Repeat pressure: dup=4 srcLimited=4 "
                "targetSelect=116 targetSuperseded=4 targetHold=4 holdWithCandidate=4 tickEmit=120 "
                "unique=116 sourceFps=121.22 overload=0x0\n"
                "[CFR PHASE LOCK SUMMARY] Backend=inject Enabled=1 Locked=1 Offset=4012us Stable=900 "
                "Unstable=0 Acquire=1 Rephase=1 Release=0 Multiplier=1\n"
            ),
        )
        report = classify_session_triage(inject_post_hitch_phase_churn)
        assert "inject_cfr_target_policy_hold" in report["verdicts"]
        assert report["evidence"]["inject_pacing"]["matched_rate_longest_run"] == 3
        assert report["evidence"]["cfr_phase_lock_summary"][0]["offset_us"] == 4012

        inject_variable_rate_resampling = make_session(
            "inject_variable_rate_resampling",
            media=(
                "[Inject CFR SUMMARY] Live=12000 Dup=2200 DupPct=18.3% "
                "DupReason(src=2200 def=0 timer=0 drain=0) FreshCatchup=0 RepeatCatchup=0 "
                "StaleTrim=0 Recovery=0/0\n"
                "[Inject CFR SUMMARY] SourceFps=72.00..119.00 JitterMax=5000us SelMax=8000us\n"
                "[Inject CFR QUALITY SUMMARY] TargetSelect=9800 Superseded=1800 TargetHold=2200 "
                "HoldWithCandidate=1800 BufferCapTrim=0 TargetResidualMax=8000us\n"
                "[2026-07-17 16:53:45.000] [INFO] [Inject CFR] Repeat pressure: dup=35 srcLimited=35 "
                "targetSelect=85 targetSuperseded=20 targetHold=35 holdWithCandidate=20 tickEmit=120 "
                "unique=85 sourceFps=85.00 overload=0x0\n"
                "[2026-07-17 16:53:50.000] [INFO] [Inject CFR] Repeat pressure: dup=20 srcLimited=20 "
                "targetSelect=100 targetSuperseded=15 targetHold=20 holdWithCandidate=15 tickEmit=120 "
                "unique=100 sourceFps=100.00 overload=0x0\n"
                "[2026-07-17 16:53:55.000] [INFO] [Inject CFR] Repeat pressure: dup=12 srcLimited=12 "
                "targetSelect=108 targetSuperseded=8 targetHold=12 holdWithCandidate=8 tickEmit=120 "
                "unique=108 sourceFps=108.00 overload=0x0\n"
            ),
        )
        report = classify_session_triage(inject_variable_rate_resampling)
        assert "inject_cfr_target_policy_hold" not in report["verdicts"]
        assert report["evidence"]["inject_pacing"]["matched_rate_pressure_rows"] == 0

        audio_worker_scheduling_stall = make_session(
            "audio_worker_scheduling_stall",
            media="[AudioLoop] Scheduling summary: events=2 maxGap=38124us threshold=25000us\n",
        )
        report = classify_session_triage(audio_worker_scheduling_stall)
        assert report["evidence"]["log_counts"]["audio_worker_scheduling_stall"] == 1
        assert not report["faults"]["audio_timeline"]

        mux_overload = make_session(
            "mux_overload",
            media="[VideoEncoder] Final metadata durations: target=1000 us video=1000 us audioMin=1000 us audioMax=1000 us maxDelta=0 us streams(v=1 a=2) overload(encoder=0 mux=1) backpressure=3 peakMux=20000KB peakPkts=50\n",
        )
        report = classify_session_triage(mux_overload)
        assert "ce_encoder_or_mux_backpressure" in report["verdicts"]

        writer_timeout = make_session(
            "writer_timeout",
            media=(
                "[VideoEncoder] Stop: WARNING - Writer thread did not finish in 5s (result=258), detaching.\n"
                "[VideoEncoder] Sync Stop: Finalizing file...\n"
            ),
        )
        report = classify_session_triage(writer_timeout)
        assert "ce_encoder_or_mux_backpressure" in report["verdicts"]
        assert report["evidence"]["mux_fault_counts"]["writer_finalize_timeout"] == 1
        assert report["evidence"]["writer_sync_after_timeout"]

        writer_post_mux_hang = make_session(
            "writer_post_mux_hang",
            media=(
                "[VideoEncoder] Stop: ERROR writer_finalize_timeout result=258 phase=post_mux_probe elapsed=30000ms "
                "queueBytes=0 queuePackets=0; async writer retains FFmpeg context, skipping synchronous finalize\n"
                "[VideoEncoder] Final packet timeline: target=1000 us videoEnd=1000 us audioMinEnd=1000 us "
                "audioMaxEnd=1000 us maxPacketDelta=0 us audioPastTarget=0\n"
            ),
        )
        report = classify_session_triage(writer_post_mux_hang)
        assert "post_mux_probe_hang" in report["verdicts"]
        assert "ce_encoder_or_mux_backpressure" in report["verdicts"]
        assert not report["evidence"]["late_writer_finalize_recovered"]
        assert report["faults"]["post_mux_probe_hang"]

        writer_probe_timeout = make_session(
            "writer_probe_timeout",
            media=(
                "[VideoEncoder] mux_closed file='x.mkv' finalDurationUs=1000\n"
                "[VideoEncoder] post_mux_probe_start file='x.mkv' target=1000 timeout=5000ms\n"
                "[VideoEncoder] post_mux_probe_timeout file='x.mkv' elapsed=5000ms; cancelling validation probe\n"
                "[VideoEncoder] post_mux_probe_cancelled file='x.mkv' detached=1\n"
                "[VideoEncoder] Final packet timeline: target=1000 us videoEnd=1000 us audioMinEnd=1000 us "
                "audioMaxEnd=1000 us maxPacketDelta=0 us audioPastTarget=0\n"
            ),
        )
        report = classify_session_triage(writer_probe_timeout)
        assert "post_mux_probe_timeout" in report["verdicts"]
        assert "ce_encoder_or_mux_backpressure" not in report["verdicts"]
        assert report["faults"]["post_mux_probe_timeout"]

        wgc_encoder_judder = make_session(
            "wgc_encoder_judder",
            media=(
                "[Cadence Health] Phase=Live | WgcSelBias=260000us | Shortfall=32/266.7ms "
                "LeadExcess=116.0ms | Oldest=250.0ms BufNow=30 | WgcLiveRebase=1/120/1 | Over=0x1\n"
                "[WGC CFR CADENCE EVENT] mode=encoder_limited shortfall=32/266.7ms phaseErrorAvg=260000us "
                "phaseErrorMax=260000us rebaseWindow=1 encoderDropWindow=1 encoderDropTotal=4 "
                "tooNewRepeat=1 staleDrop=4 freshMiss=0pm bufNow=30 oldest=250.0ms enc=10.0ms "
                "sustain=80.0fps overload=0x1 cause=S0/D0/E1\n"
                "[EncoderThread] WGC CFR slot repeat: buffered frame is too new for scheduled slot "
                "(lead=28000us targetQpc=1000 firstQpc=1280 buffered=30 shortfall=32)\n"
                "[EncoderThread] WGC CFR stale visual debt drop: reason=live-buffer mode=encoder_limited dropped=4 "
                "floorQpc=1200 liveNowQpc=1600 maxDebt=1000us remaining=20 shortfall=32\n"
                "[EncoderThread] WGC CFR live scheduler rebase: mode=encoder_limited skippedTicks=1 excessTicks=2 "
                "requestedTicks=32 shortfallBefore=32 nextQpc=1000 nextAfterQpc=1100 liveNowQpc=1600 "
                "timelineCovered=42\n"
            ),
        )
        report = classify_session_triage(wgc_encoder_judder)
        assert "wgc_encoder_limited_judder" in report["verdicts"]
        assert "wgc_encoder_overload_policy_fault" not in report["verdicts"]
        assert "ce_visual_timeline_fault" in report["verdicts"]
        assert "ce_audio_timeline_fault" not in report["verdicts"]

        wgc_overload_policy_fault = make_session(
            "wgc_overload_policy_fault",
            media=(
                "[Cadence Health] Phase=Live | WgcSelBias=260000us | Shortfall=32/266.7ms "
                "LeadExcess=116.0ms | Oldest=250.0ms BufNow=30 | WgcLiveRebase=1/120/1 | Over=0x1\n"
                "[WGC CFR CADENCE EVENT] mode=normal_pressure shortfall=32/266.7ms phaseErrorAvg=260000us "
                "phaseErrorMax=260000us rebaseWindow=1 encoderDropWindow=0 encoderDropTotal=0 "
                "tooNewRepeat=1 staleDrop=4 freshMiss=0pm bufNow=30 oldest=250.0ms enc=10.0ms "
                "sustain=80.0fps overload=0x1 cause=S0/D0/E1\n"
                "[EncoderThread] WGC CFR slot repeat: buffered frame is too new for scheduled slot "
                "(lead=28000us targetQpc=1000 firstQpc=1280 buffered=30 shortfall=32)\n"
                "[EncoderThread] WGC CFR stale visual debt drop: reason=live-buffer mode=normal dropped=4 "
                "floorQpc=1200 liveNowQpc=1600 maxDebt=1000us remaining=20 shortfall=32\n"
                "[EncoderThread] WGC CFR live scheduler rebase: mode=normal skippedTicks=1 excessTicks=2 "
                "requestedTicks=32 shortfallBefore=32 nextQpc=1000 nextAfterQpc=1100 liveNowQpc=1600 "
                "timelineCovered=42\n"
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=1 "
                "phaseErrorMax=260000us shortfallMax=266.7ms staleDebtDrops=4 liveRebase=1/1 "
                "tooNewRepeats=1\n"
            ),
        )
        report = classify_session_triage(wgc_overload_policy_fault)
        assert "wgc_encoder_overload_policy_fault" in report["verdicts"]
        assert "ce_visual_timeline_fault" in report["verdicts"]
        assert "ce_audio_timeline_fault" not in report["verdicts"]

        wgc_sync_delay_smoothness_summary = make_session(
            "wgc_sync_delay_smoothness_summary",
            media=(
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=1 "
                "phaseErrorMax=9000us shortfallMax=16.7ms staleDebtDrops=0 liveRebase=0/0 "
                "tooNewRepeats=12 syncDelayHolds=10 tooNewLeadMax=42000us avDelay=35.0ms "
                "lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0\n"
            ),
        )
        sync_delay_report = classify_session_triage(wgc_sync_delay_smoothness_summary)
        sync_delay_summary = sync_delay_report["evidence"]["wgc_smoothness_summary"][0]
        assert sync_delay_summary["too_new_repeats"] == 12
        assert sync_delay_summary["sync_delay_holds"] == 10
        assert sync_delay_summary["too_new_lead_max_us"] == 42000
        assert sync_delay_summary["av_delay_ms"] == 35.0
        assert "wgc_av_sync_delay_unrealized" in sync_delay_report["verdicts"]
        assert "ce_visual_timeline_fault" in sync_delay_report["verdicts"]

        wgc_sync_delay_realized = make_session(
            "wgc_sync_delay_realized",
            media=(
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=0 "
                "phaseErrorMax=4000us shortfallMax=0.0ms staleDebtDrops=0 liveRebase=0/0 "
                "tooNewRepeats=0 syncDelayHolds=0 tooNewLeadMax=0us avDelay=35.0ms "
                "startupDelay=35.0ms scheduleOffset=12000us effectiveDelay=35.0ms "
                "lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0\n"
            ),
        )
        realized_report = classify_session_triage(wgc_sync_delay_realized)
        realized_summary = realized_report["evidence"]["wgc_smoothness_summary"][0]
        assert realized_summary["startup_delay_ms"] == 35.0
        assert realized_summary["effective_delay_ms"] == 35.0
        assert realized_summary["schedule_offset_us"] == 12000
        assert "wgc_av_sync_delay_unrealized" not in realized_report["verdicts"]
        assert "ce_visual_timeline_fault" not in realized_report["verdicts"]

        wgc_sync_delay_residual_fault = make_session(
            "wgc_sync_delay_residual_fault",
            media=(
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=18 "
                "phaseErrorMax=12047us shortfallMax=8.3ms staleDebtDrops=2 liveRebase=0/0 "
                "tooNewRepeats=105 syncDelayHolds=105 tooNewLeadMax=35632us avDelay=32.0ms "
                "startupDelay=32.0ms scheduleOffset=313291us effectiveDelay=32.0ms "
                "lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0 "
                "syncDelaySourceLimitedHolds=20 syncDelayPolicyHolds=105 startupReserveFrames=26 "
                "startupReserveSpan=191938us startupDelayTarget=32000us startupReserveSelected=1 "
                "startupReserveReason=selected delayReservoirLowWaterFrames=4 delayReservoirTargetFrames=5 "
                "delayReservoirLowWaterTicks=27 realizedDelayAvg=23000us realizedDelayMin=16000us "
                "realizedDelayMax=32000us delayResidualAvg=9000/9000us delayResidualMax=16000us "
                "delayResidualP95=12000us delayResidualLateMax=16000us delayResidualEarlyMax=1000us\n"
            ),
        )
        residual_fault_report = classify_session_triage(wgc_sync_delay_residual_fault)
        residual_fault_summary = residual_fault_report["evidence"]["wgc_smoothness_summary"][0]
        assert residual_fault_summary["delay_residual_avg_abs_us"] == 9000
        assert residual_fault_summary["delay_residual_max_us"] == 16000
        assert "wgc_av_sync_delay_unrealized" not in residual_fault_report["verdicts"]
        assert "wgc_av_sync_delay_residual" in residual_fault_report["verdicts"]
        assert "wgc_sync_delay_policy_fault" in residual_fault_report["verdicts"]
        assert "ce_visual_timeline_fault" in residual_fault_report["verdicts"]

        wgc_sync_delay_residual_bounded = make_session(
            "wgc_sync_delay_residual_bounded",
            media=(
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=2 "
                "phaseErrorMax=5000us shortfallMax=0.0ms staleDebtDrops=0 liveRebase=0/0 "
                "tooNewRepeats=3 syncDelayHolds=3 tooNewLeadMax=7000us avDelay=32.0ms "
                "startupDelay=32.0ms scheduleOffset=24000us effectiveDelay=32.0ms "
                "lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0 "
                "syncDelaySourceLimitedHolds=3 syncDelayPolicyHolds=0 startupReserveFrames=8 "
                "startupReserveSpan=64000us startupDelayTarget=32000us startupReserveSelected=1 "
                "startupReserveReason=selected delayReservoirLowWaterFrames=4 delayReservoirTargetFrames=5 "
                "delayReservoirLowWaterTicks=0 realizedDelayAvg=30000us realizedDelayMin=26000us "
                "realizedDelayMax=34000us delayResidualAvg=2000/2000us delayResidualMax=6000us "
                "delayResidualP95=4000us delayResidualLateMax=6000us delayResidualEarlyMax=2000us\n"
            ),
        )
        residual_bounded_report = classify_session_triage(wgc_sync_delay_residual_bounded)
        assert "wgc_av_sync_delay_residual" not in residual_bounded_report["verdicts"]
        assert "wgc_av_sync_delay_unrealized" not in residual_bounded_report["verdicts"]
        assert "ce_visual_timeline_fault" not in residual_bounded_report["verdicts"]
        # An 8 ms realized-delay spread is bounded: the instability classifier must stay quiet.
        assert "wgc_active_delay_realized_delay_unstable" not in residual_bounded_report["verdicts"]

        # GPU-bound under-delivery judder signature (Strange Brigade sbwgcjudder shape): residuals
        # and holds look bounded and the runtime reports smoothnessNotMaximal=0, but the realized
        # content delay rubber-bands 22.9..44.4 ms. The legacy analyzer trusted the runtime verdict
        # and called this "fine"; the instability classifier must now flag it distinctly.
        wgc_realized_delay_unstable = make_session(
            "wgc_realized_delay_unstable",
            media=(
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=101 "
                "phaseErrorMax=3260us shortfallMax=58.3ms staleDebtDrops=13 liveRebase=0/0 "
                "tooNewRepeats=544 syncDelayHolds=544 tooNewLeadMax=40208us avDelay=31.1ms "
                "startupDelay=31.1ms scheduleOffset=99714us effectiveDelay=31.1ms "
                "lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0 "
                "syncDelaySourceLimitedHolds=544 syncDelayPolicyHolds=0 startupReserveFrames=34 "
                "startupReserveSpan=229296us startupDelayTarget=31111us startupReserveSelected=1 "
                "startupReserveReason=selected delayReservoirLowWaterFrames=4 delayReservoirTargetFrames=5 "
                "delayReservoirLowWaterTicks=1279 realizedDelayAvg=30756us realizedDelayMin=22875us "
                "realizedDelayMax=44419us delayResidualAvg=354/2199us delayResidualMax=13308us "
                "delayResidualP95=5000us delayResidualLateMax=8236us delayResidualEarlyMax=13308us\n"
                "[WGC CFR SMOOTHNESS VERDICT] delayPostSelectionRejectedSync=0 "
                "delayPostSelectionRescuedSync=0 sourceRepeatLowerBound=544 excessRepeats=0 "
                "policyAddedRepeats=0 excessRepeatClusters=0 excessRepeatClusterMax=0 "
                "smoothnessNotMaximal=0 mixedPolicyFault=0\n"
            ),
        )
        realized_unstable_report = classify_session_triage(wgc_realized_delay_unstable)
        realized_unstable_summary = realized_unstable_report["evidence"]["wgc_smoothness_summary"][0]
        assert realized_unstable_summary["realized_delay_min_us"] == 22875
        assert realized_unstable_summary["realized_delay_max_us"] == 44419
        # The runtime self-classified the run as fine; the spread must still be flagged.
        assert "wgc_cfr_smoothness_not_maximal" not in realized_unstable_report["verdicts"]
        assert "wgc_active_delay_realized_delay_unstable" in realized_unstable_report["verdicts"]

        # Full-collapse shape (real 20260625_135221 session): the realized content delay both inflated
        # (max 248 ms vs 31.6 ms target) AND fully collapsed to 0 (live-recovery latched and disabled
        # the delay for tens of seconds). A realizedDelayMin of 0 is the WORST case, but the legacy
        # spread guard (`delay_min <= 0`) treated it as "no data" and returned 0, silently hiding the
        # most severe instability. The classifier must now fire on min=0 with a valid large max.
        wgc_realized_delay_collapse = make_session(
            "wgc_realized_delay_collapse",
            media=(
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=10 "
                "phaseErrorMax=159206us shortfallMax=66.7ms staleDebtDrops=58 liveRebase=0/0 "
                "tooNewRepeats=13 syncDelayHolds=0 tooNewLeadMax=71540us avDelay=31.6ms "
                "startupDelay=31.6ms scheduleOffset=347201us effectiveDelay=31.6ms "
                "lowSourceBypass=1 modeMismatch=0 sourceBacktrack=0 "
                "syncDelaySourceLimitedHolds=0 syncDelayPolicyHolds=0 startupReserveFrames=32 "
                "startupReserveSpan=219477us startupDelayTarget=31558us startupReserveSelected=1 "
                "startupReserveReason=selected delayReservoirLowWaterFrames=4 delayReservoirTargetFrames=5 "
                "delayReservoirLowWaterTicks=735 realizedDelayAvg=64372us realizedDelayMin=0us "
                "realizedDelayMax=248196us delayResidualAvg=-32813/35303us delayResidualMax=216638us "
                "delayResidualP95=141000us delayResidualLateMax=37181us delayResidualEarlyMax=216638us\n"
            ),
        )
        realized_collapse_report = classify_session_triage(wgc_realized_delay_collapse)
        realized_collapse_summary = realized_collapse_report["evidence"]["wgc_smoothness_summary"][0]
        assert realized_collapse_summary["realized_delay_min_us"] == 0
        assert realized_collapse_summary["realized_delay_max_us"] == 248196
        assert wgc_realized_delay_spread_us(realized_collapse_summary) == 248196
        assert "wgc_active_delay_realized_delay_unstable" in realized_collapse_report["verdicts"]

        wgc_sync_delay_realized_source_pressure = make_session(
            "wgc_sync_delay_realized_source_pressure",
            media=(
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=12 "
                "phaseErrorMax=12047us shortfallMax=8.3ms staleDebtDrops=10 liveRebase=0/0 "
                "tooNewRepeats=48 syncDelayHolds=48 tooNewLeadMax=36084us avDelay=34.8ms "
                "startupDelay=34.8ms scheduleOffset=54224us effectiveDelay=34.8ms "
                "lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0 "
                "syncDelaySourceLimitedHolds=48 syncDelayPolicyHolds=0 startupReserveFrames=6 "
                "startupReserveSpan=42000us startupDelayTarget=34800us startupReserveSelected=1 "
                "startupReserveReason=selected\n"
            ),
        )
        source_pressure_report = classify_session_triage(wgc_sync_delay_realized_source_pressure)
        source_pressure_summary = source_pressure_report["evidence"]["wgc_smoothness_summary"][0]
        assert source_pressure_summary["sync_delay_source_limited_holds"] == 48
        assert source_pressure_summary["sync_delay_policy_holds"] == 0
        assert source_pressure_summary["startup_reserve_selected"] == 1
        assert "wgc_av_sync_delay_unrealized" not in source_pressure_report["verdicts"]
        assert "wgc_sync_delay_reserve_pressure" in source_pressure_report["verdicts"]
        assert "ce_visual_timeline_fault" not in source_pressure_report["verdicts"]

        wgc_sync_delay_realized_legacy_pressure = make_session(
            "wgc_sync_delay_realized_legacy_pressure",
            media=(
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=12 "
                "phaseErrorMax=12047us shortfallMax=8.3ms staleDebtDrops=10 liveRebase=0/0 "
                "tooNewRepeats=48 syncDelayHolds=48 tooNewLeadMax=36084us avDelay=34.8ms "
                "startupDelay=34.8ms scheduleOffset=54224us effectiveDelay=34.8ms "
                "lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0\n"
            ),
        )
        legacy_pressure_report = classify_session_triage(wgc_sync_delay_realized_legacy_pressure)
        assert "wgc_av_sync_delay_unrealized" not in legacy_pressure_report["verdicts"]
        assert "wgc_sync_delay_reserve_pressure" in legacy_pressure_report["verdicts"]
        assert "ce_visual_timeline_fault" not in legacy_pressure_report["verdicts"]

        wgc_sync_delay_goodish_source_limited = make_session(
            "wgc_sync_delay_goodish_source_limited",
            media=(
                "[WGC CFR SUMMARY] Live=13965 Dup=3006 DupPct=21.5% NoFresh=261pm NoReserve=296pm "
                "DupReason(src=3006 def=0 timer=0 drain=0) SourceLimitedRepeats=3006 StarvedEpisodes=281 "
                "longest=6469ms longestDup=401 worstIn=4 worstDel=4\n"
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=114 "
                "phaseErrorMax=103462us shortfallMax=0.0ms staleDebtDrops=1 liveRebase=0/0 "
                "tooNewRepeats=1676 syncDelayHolds=1676 tooNewLeadMax=160855us avDelay=32.0ms "
                "startupDelay=32.0ms scheduleOffset=102131us effectiveDelay=32.0ms "
                "lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0 "
                "syncDelaySourceLimitedHolds=1291 syncDelayPolicyHolds=385 startupReserveFrames=2 "
                "startupReserveSpan=7961us startupDelayTarget=31995us startupReserveSelected=0 "
                "startupReserveReason=reserve_timeout delayReservoirLowWaterFrames=4 "
                "delayReservoirTargetFrames=5 delayReservoirLowWaterTicks=4140 realizedDelayAvg=31500us "
                "realizedDelayMin=22009us realizedDelayMax=237273us delayResidualAvg=494/2444us "
                "delayResidualMax=205278us delayResidualP95=5000us delayResidualLateMax=9986us "
                "delayResidualEarlyMax=205278us\n"
            ),
        )
        goodish_report = classify_session_triage(wgc_sync_delay_goodish_source_limited)
        assert "wgc_source_starvation" in goodish_report["verdicts"]
        assert "wgc_sync_delay_reserve_pressure" in goodish_report["verdicts"]
        assert "wgc_av_sync_delay_residual" not in goodish_report["verdicts"]
        assert "wgc_sync_delay_policy_fault" not in goodish_report["verdicts"]
        assert "ce_visual_timeline_fault" not in goodish_report["verdicts"]
        assert "ce_audio_timeline_fault" not in goodish_report["verdicts"]

        wgc_sync_delay_mixed_policy_pressure = make_session(
            "wgc_sync_delay_mixed_policy_pressure",
            media=(
                "[WGC CFR SUMMARY] Live=10869 Dup=2072 DupPct=19.0% NoFresh=230pm NoReserve=256pm "
                "DupReason(src=2072 def=0 timer=0 drain=0) SourceLimitedRepeats=2072 StarvedEpisodes=235 "
                "longest=4359ms longestDup=214 worstIn=4 worstDel=4\n"
                "[WGC CFR CADENCE EVENT] mode=normal_pressure shortfall=0/0.0ms phaseErrorAvg=427us "
                "phaseErrorMax=6782us rebaseWindow=0 encoderDropWindow=0 encoderDropTotal=0 "
                "tooNewRepeat=11 syncDelayHold=11 syncDelaySourceHold=1 syncDelayPolicyHold=10 "
                "tooNewLeadMax=12459us staleDrop=0 freshMiss=117pm bufNow=5 oldest=24.9ms enc=0.41ms "
                "sustain=2428.8fps overload=0x0 lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0 "
                "avDelay=31.0ms delayResidualAvg=382/2320us delayResidualMax=76400us "
                "delayResidualP95=5000us delayResidualLateMax=9990us reservoir=4/5 lowTicks=14 cause=S0/D0/E0\n"
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=88 "
                "phaseErrorMax=34457us shortfallMax=0.0ms staleDebtDrops=42 liveRebase=0/0 "
                "tooNewRepeats=1104 syncDelayHolds=1104 tooNewLeadMax=165483us avDelay=31.0ms "
                "startupDelay=31.0ms scheduleOffset=300848us effectiveDelay=31.0ms "
                "lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0 "
                "syncDelaySourceLimitedHolds=782 syncDelayPolicyHolds=322 startupReserveFrames=18 "
                "startupReserveSpan=243048us startupDelayTarget=31003us startupReserveSelected=1 "
                "startupReserveReason=selected delayReservoirLowWaterFrames=4 "
                "delayReservoirTargetFrames=5 delayReservoirLowWaterTicks=2783 realizedDelayAvg=30618us "
                "realizedDelayMin=21013us realizedDelayMax=107403us delayResidualAvg=384/2319us "
                "delayResidualMax=76400us delayResidualP95=5000us delayResidualLateMax=9990us "
                "delayResidualEarlyMax=76400us\n"
            ),
        )
        mixed_policy_report = classify_session_triage(wgc_sync_delay_mixed_policy_pressure)
        assert "wgc_source_starvation" in mixed_policy_report["verdicts"]
        assert "wgc_av_sync_delay_residual" in mixed_policy_report["verdicts"]
        assert "wgc_sync_delay_policy_fault" in mixed_policy_report["verdicts"]
        assert "ce_visual_timeline_fault" in mixed_policy_report["verdicts"]
        assert "ce_audio_timeline_fault" not in mixed_policy_report["verdicts"]

        wgc_sync_delay_fortidelay2_mixed_pressure = make_session(
            "wgc_sync_delay_fortidelay2_mixed_pressure",
            media=(
                "[WGC CFR SUMMARY] Live=8709 Dup=1545 DupPct=17.7% NoFresh=227pm NoReserve=258pm "
                "DupReason(src=1545 def=0 timer=0 drain=0) SourceLimitedRepeats=1545 StarvedEpisodes=184 "
                "longest=4250ms longestDup=200 worstIn=4 worstDel=4\n"
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=72 "
                "phaseErrorMax=5382us shortfallMax=0.0ms staleDebtDrops=32 liveRebase=0/0 "
                "tooNewRepeats=893 syncDelayHolds=893 tooNewLeadMax=163581us avDelay=27.5ms "
                "startupDelay=27.5ms scheduleOffset=308784us effectiveDelay=27.5ms "
                "lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0 "
                "syncDelaySourceLimitedHolds=603 syncDelayPolicyHolds=290 startupReserveFrames=14 "
                "startupReserveSpan=152803us startupDelayTarget=27474us startupReserveSelected=1 "
                "startupReserveReason=selected delayReservoirLowWaterFrames=4 "
                "delayReservoirTargetFrames=5 delayReservoirLowWaterTicks=2254 realizedDelayAvg=26815us "
                "realizedDelayMin=17664us realizedDelayMax=220633us delayResidualAvg=658/2397us "
                "delayResidualMax=193159us delayResidualP95=5000us delayResidualLateMax=9810us "
                "delayResidualEarlyMax=193159us delayResidualRelaxedSelections=462 "
                "delayResidualRelaxedMax=9810us delayResidualRelaxedRejectedSync=41577 "
                "delayRepeatClusterPressure=175 delayRepeatClusterMax=92 mixedPolicyFault=1\n"
                "[STOP AUDIO TRACK] Track 1: encoded=3484000 expected=3484000 diff=+0 (+0.000 ms) "
                "sources=[1,3,5,7]\n"
                "[STOP AUDIO TRACK] Track 2: encoded=3484000 expected=3484000 diff=+0 (+0.000 ms) "
                "sources=[4,6,8]\n"
                "[STOP AUDIO TRACK] Track 3: encoded=3484000 expected=3484000 diff=+0 (+0.000 ms) "
                "sources=[0,2]\n"
                "[VideoEncoder] Final packet timeline: target=72583333 us videoEnd=72583333 us "
                "audioMinEnd=72583333 us audioMaxEnd=72583333 us maxPacketDelta=0 us streams(v=1 a=3) "
                "audioPastTarget=0\n"
                "[VideoEncoder] Final metadata durations: target=72583333 us video=72583333 us "
                "audioMin=72583333 us audioMax=72583333 us maxDelta=0 us streams(v=1 a=3) "
                "overload(encoder=0 mux=0) backpressure=0\n"
            ),
        )
        fortidelay2_report = classify_session_triage(wgc_sync_delay_fortidelay2_mixed_pressure)
        fortidelay2_summary = fortidelay2_report["evidence"]["wgc_smoothness_summary"][0]
        assert fortidelay2_summary["delay_relaxed_rejected_sync"] == 41577
        assert fortidelay2_summary["delay_repeat_cluster_pressure"] == 175
        assert fortidelay2_summary["delay_repeat_cluster_max_ticks"] == 92
        assert "wgc_source_starvation" in fortidelay2_report["verdicts"]
        assert "wgc_av_sync_delay_residual" in fortidelay2_report["verdicts"]
        assert "wgc_sync_delay_policy_fault" in fortidelay2_report["verdicts"]
        assert "ce_visual_timeline_fault" in fortidelay2_report["verdicts"]
        assert "ce_audio_timeline_fault" not in fortidelay2_report["verdicts"]

        wgc_sync_delay_20260620_044434_unstable_fps = make_session(
            "wgc_sync_delay_20260620_044434_unstable_fps",
            media=(
                "[WGC CFR SUMMARY] Live=29098 Dup=3369 DupPct=11.5% NoFresh=161pm NoReserve=183pm "
                "DupReason(src=3369 def=0 timer=0 drain=0) SourceLimitedRepeats=3369 StarvedEpisodes=757 "
                "longest=9313ms longestDup=573 worstIn=4 worstDel=4\n"
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=242 "
                "phaseErrorMax=110628us shortfallMax=0.0ms staleDebtDrops=1 liveRebase=0/0 "
                "tooNewRepeats=2927 syncDelayHolds=2927 tooNewLeadMax=95075us avDelay=28.3ms "
                "startupDelay=28.3ms scheduleOffset=404156us effectiveDelay=28.3ms "
                "lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0 "
                "syncDelaySourceLimitedHolds=2159 syncDelayPolicyHolds=768 startupReserveFrames=1 "
                "startupReserveSpan=0us startupDelayTarget=28334us startupReserveSelected=0 "
                "startupReserveReason=source_startup_underfeed delayReservoirLowWaterFrames=4 "
                "delayReservoirTargetFrames=5 delayReservoirLowWaterTicks=5346 realizedDelayAvg=27764us "
                "realizedDelayMin=18412us realizedDelayMax=78381us delayResidualAvg=569/2322us "
                "delayResidualMax=50047us delayResidualP95=5000us delayResidualLateMax=9922us "
                "delayResidualEarlyMax=50047us rawResidualAvg=676/2502us rawResidualMax=63403us "
                "rawResidualP95=6000us rawResidualLateMax=9996us rawResidualEarlyMax=63403us "
                "predictedResidualAvg=569/2322us predictedResidualP95=5000us "
                "predictedResidualLateMax=9922us rawMinusPredictedAvg=107/107us "
                "rawMinusPredictedMax=66811us delayResidualRelaxedSelections=1447 "
                "delayResidualRelaxedMax=9922us delayResidualRelaxedRejectedSync=335410 "
                "delayRepeatClusterPressure=429 delayRepeatClusterMax=34 "
                "delayResidualRelaxedBetter=1445 delayResidualRelaxedCluster=1 "
                "delayResidualRelaxedRejectedHeadroom=103 delayResidualRelaxedRejectedCost=11644 "
                "delaySourceRecoveryHolds=841 delaySourceRecoveryTicks=14564 "
                "delayPostSelectionRejectedSync=0 delayPostSelectionRescuedSync=0 "
                "sourceRepeatLowerBound=2426 excessRepeats=508 policyAddedRepeats=508 "
                "excessRepeatClusters=0 excessRepeatClusterMax=0 smoothnessNotMaximal=1 "
                "mixedPolicyFault=1\n"
                "[STOP AUDIO TRACK] Track 1: encoded=11640400 expected=11640400 diff=+0 (+0.000 ms) "
                "sources=[1,3,5,7]\n"
                "[STOP AUDIO TRACK] Track 2: encoded=11640400 expected=11640400 diff=+0 (+0.000 ms) "
                "sources=[4,6,8]\n"
                "[STOP AUDIO TRACK] Track 3: encoded=11640400 expected=11640400 diff=+0 (+0.000 ms) "
                "sources=[0,2]\n"
                "[VideoEncoder] Final packet timeline: target=242508333 us videoEnd=242508333 us "
                "audioMinEnd=242508333 us audioMaxEnd=242508333 us maxPacketDelta=0 us streams(v=1 a=3) "
                "audioPastTarget=0\n"
                "[VideoEncoder] Final metadata durations: target=242508333 us video=242508333 us "
                "audioMin=242508333 us audioMax=242508333 us maxDelta=0 us streams(v=1 a=3) "
                "overload(encoder=0 mux=0) backpressure=0\n"
            ),
        )
        delay_20260620_report = classify_session_triage(wgc_sync_delay_20260620_044434_unstable_fps)
        delay_20260620_summary = delay_20260620_report["evidence"]["wgc_smoothness_summary"][0]
        assert delay_20260620_summary["source_repeat_lower_bound"] == 2426
        assert delay_20260620_summary["policy_added_repeats"] == 508
        assert delay_20260620_summary["delay_repeat_cluster_pressure"] == 429
        assert "wgc_source_starvation" in delay_20260620_report["verdicts"]
        assert "wgc_av_sync_delay_residual" in delay_20260620_report["verdicts"]
        assert "wgc_sync_delay_policy_fault" in delay_20260620_report["verdicts"]
        assert "wgc_cfr_smoothness_not_maximal" in delay_20260620_report["verdicts"]
        assert "ce_visual_timeline_fault" in delay_20260620_report["verdicts"]
        assert "ce_audio_timeline_fault" not in delay_20260620_report["verdicts"]

        wgc_sync_delay_fortineu_safe_candidate_pressure = make_session(
            "wgc_sync_delay_fortineu_safe_candidate_pressure",
            media=(
                "[WGC CFR SUMMARY] Live=31799 Dup=3055 DupPct=9.6% NoFresh=130pm NoReserve=158pm "
                "DupReason(src=3055 def=0 timer=0 drain=0) SourceLimitedRepeats=3055 StarvedEpisodes=663 "
                "longest=2703ms longestDup=172 worstIn=4 worstDel=4\n"
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=210 "
                "phaseErrorMax=214038us shortfallMax=8.3ms staleDebtDrops=0 liveRebase=0/0 "
                "tooNewRepeats=2644 syncDelayHolds=2644 tooNewLeadMax=110473us avDelay=33.6ms "
                "startupDelay=33.6ms scheduleOffset=-169584us effectiveDelay=33.6ms "
                "lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0 "
                "syncDelaySourceLimitedHolds=1709 syncDelayPolicyHolds=935 startupReserveFrames=1 "
                "startupReserveSpan=0us startupDelayTarget=33600us startupReserveSelected=0 "
                "startupReserveReason=source_startup_underfeed delayReservoirLowWaterFrames=5 "
                "delayReservoirTargetFrames=6 delayReservoirLowWaterTicks=5043 realizedDelayAvg=33175us "
                "realizedDelayMin=22000us realizedDelayMax=79000us delayResidualAvg=405/2210us "
                "delayResidualMax=45594us delayResidualP95=5000us delayResidualLateMax=9997us "
                "delayResidualEarlyMax=45594us rawResidualAvg=504/2407us rawResidualMax=50000us "
                "rawResidualP95=5000us rawResidualLateMax=9958us rawResidualEarlyMax=50000us "
                "predictedResidualAvg=405/2210us predictedResidualP95=5000us "
                "predictedResidualLateMax=9997us rawMinusPredictedAvg=98/100us "
                "rawMinusPredictedMax=1000us delayResidualRelaxedSelections=1218 "
                "delayResidualRelaxedMax=9958us delayResidualRelaxedRejectedSync=479466 "
                "delayRepeatClusterPressure=295 delayRepeatClusterMax=56 "
                "delayResidualRelaxedBetter=1217 delayResidualRelaxedCluster=0 "
                "delayResidualRelaxedRejectedHeadroom=7584 delayResidualRelaxedRejectedCost=9124 "
                "delaySoftLateRejected=7496 delaySoftLateAccepted=1978 "
                "delayOlderFrameAvoidedRepeat=27484 delaySourceLimitedRepeats=1945 "
                "delayRepeatRescue=0/935 delayRepeatRescueRejected=0/0/935 "
                "delayRepeatSafeCandidate=699 delayRepeatNoSafeCandidate=236 "
                "delayRepeatWindowClass=0/699/236 delayRepeatReserveMax=6/50000us "
                "delaySourceRecoveryHolds=675 delaySourceRecoveryTicks=12971 "
                "delayPostSelectionRejectedSync=0 delayPostSelectionRescuedSync=0 "
                "sourceRepeatLowerBound=1945 excessRepeats=699 policyAddedRepeats=699 "
                "excessRepeatClusters=12 excessRepeatClusterMax=1 smoothnessNotMaximal=1 "
                "mixedPolicyFault=1\n"
                "[STOP AUDIO TRACK] Track 1: encoded=12720000 expected=12720000 diff=+0 (+0.000 ms) "
                "sources=[1,3,5,7]\n"
                "[VideoEncoder] Final packet timeline: target=265000000 us videoEnd=265000000 us "
                "audioMinEnd=265000000 us audioMaxEnd=265000000 us maxPacketDelta=0 us streams(v=1 a=3) "
                "audioPastTarget=0\n"
                "[VideoEncoder] Final metadata durations: target=265000000 us video=265000000 us "
                "audioMin=265000000 us audioMax=265000000 us maxDelta=0 us streams(v=1 a=3) "
                "overload(encoder=0 mux=0) backpressure=0\n"
            ),
        )
        fortineu_report = classify_session_triage(wgc_sync_delay_fortineu_safe_candidate_pressure)
        fortineu_summary = fortineu_report["evidence"]["wgc_smoothness_summary"][0]
        assert fortineu_summary["delay_repeat_safe_candidate"] == 699
        assert fortineu_summary["delay_repeat_rescue_attempts"] == 935
        assert "wgc_repeat_despite_safe_candidate" in fortineu_report["verdicts"]
        assert "wgc_cfr_smoothness_not_maximal" in fortineu_report["verdicts"]
        assert "ce_visual_timeline_fault" in fortineu_report["verdicts"]
        assert "ce_audio_timeline_fault" not in fortineu_report["verdicts"]

        wgc_sync_delay_20260620_171522_safe_candidate_pressure = make_session(
            "wgc_sync_delay_20260620_171522_safe_candidate_pressure",
            media=(
                "[WGC CFR SUMMARY] Live=12599 Dup=2501 DupPct=19.8% NoFresh=224pm NoReserve=269pm "
                "DupReason(src=2501 def=0 timer=0 drain=0) SourceLimitedRepeats=2501 StarvedEpisodes=162 "
                "longest=6703ms longestDup=404 worstIn=4 worstDel=4\n"
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=102 "
                "phaseErrorMax=249880us shortfallMax=0.0ms staleDebtDrops=25 liveRebase=0/0 "
                "tooNewRepeats=1617 syncDelayHolds=1617 tooNewLeadMax=229934us avDelay=32.4ms "
                "startupDelay=32.4ms scheduleOffset=519552us effectiveDelay=32.4ms "
                "lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0 "
                "syncDelaySourceLimitedHolds=1363 syncDelayPolicyHolds=254 startupReserveFrames=23 "
                "startupReserveSpan=205902us startupDelayTarget=32399us startupReserveSelected=1 "
                "startupReserveReason=partial_span_timeout delayReservoirLowWaterFrames=4 "
                "delayReservoirTargetFrames=5 delayReservoirLowWaterTicks=3399 realizedDelayAvg=32057us "
                "realizedDelayMin=22425us realizedDelayMax=219012us delayResidualAvg=341/2366us "
                "delayResidualMax=186613us delayResidualP95=5000us delayResidualLateMax=9974us "
                "delayResidualEarlyMax=186613us rawResidualAvg=449/2423us rawResidualMax=32695us "
                "rawResidualP95=5000us rawResidualLateMax=9993us rawResidualEarlyMax=32695us "
                "predictedResidualAvg=341/2366us predictedResidualP95=5000us "
                "predictedResidualLateMax=9974us rawMinusPredictedAvg=108/108us "
                "rawMinusPredictedMax=193046us delayResidualRelaxedSelections=504 "
                "delayResidualRelaxedMax=9974us delayResidualRelaxedRejectedSync=164724 "
                "delayRepeatClusterPressure=562 delayRepeatClusterMax=167 "
                "delayResidualRelaxedBetter=504 delayResidualRelaxedCluster=0 "
                "delayResidualRelaxedRejectedHeadroom=3119 delayResidualRelaxedRejectedCost=3210 "
                "delaySoftLateRejected=2976 delaySoftLateAccepted=646 "
                "delayOlderFrameAvoidedRepeat=9548 delaySourceLimitedRepeats=1438 "
                "delayRepeatRescue=29/1646 delayRepeatRescueRejected=265/4498/357 "
                "delayRepeatPromoted=29/1646 delayRepeatPromoteRejectedSoft=2976 "
                "delayRepeatSafeAfterPromote=569 delayRepeatSafeCandidate=569 "
                "delayRepeatNoSafeCandidate=1048 delayRepeatWindowClass=65/246/1306 "
                "delayRepeatReserveMax=8/87055us delaySourceRecoveryHolds=131 "
                "delaySourceRecoveryTicks=5995 delayPostSelectionRejectedSync=0 "
                "delayPostSelectionRescuedSync=0 sourceRepeatLowerBound=1438 excessRepeats=254 "
                "policyAddedRepeats=254 excessRepeatClusters=5 excessRepeatClusterMax=1 "
                "smoothnessNotMaximal=1 mixedPolicyFault=0\n"
                "[STOP AUDIO TRACK] Track 1: encoded=5040000 expected=5040000 diff=+0 (+0.000 ms) "
                "sources=[1,3,5,7]\n"
                "[STOP AUDIO TRACK] Track 2: encoded=5040000 expected=5040000 diff=+0 (+0.000 ms) "
                "sources=[4,6,8]\n"
                "[STOP AUDIO TRACK] Track 3: encoded=5040000 expected=5040000 diff=+0 (+0.000 ms) "
                "sources=[0,2]\n"
                "[VideoEncoder] Final packet timeline: target=105000000 us videoEnd=105000000 us "
                "audioMinEnd=105000000 us audioMaxEnd=105000000 us maxPacketDelta=0 us streams(v=1 a=3) "
                "audioPastTarget=0\n"
                "[VideoEncoder] Final metadata durations: target=105000000 us video=105000000 us "
                "audioMin=105000000 us audioMax=105000000 us maxDelta=0 us streams(v=1 a=3) "
                "overload(encoder=0 mux=0) backpressure=0\n"
            ),
        )
        latest_safe_candidate_report = classify_session_triage(
            wgc_sync_delay_20260620_171522_safe_candidate_pressure
        )
        latest_safe_candidate_summary = latest_safe_candidate_report["evidence"]["wgc_smoothness_summary"][0]
        assert latest_safe_candidate_summary["policy_added_repeats"] == 254
        assert latest_safe_candidate_summary["delay_repeat_safe_after_promotion"] == 569
        assert latest_safe_candidate_summary["delay_repeat_promoted_before_repeat"] == 29
        assert "wgc_repeat_despite_safe_candidate" in latest_safe_candidate_report["verdicts"]
        assert "wgc_cfr_smoothness_not_maximal" in latest_safe_candidate_report["verdicts"]
        assert "ce_visual_timeline_fault" in latest_safe_candidate_report["verdicts"]
        assert "ce_audio_timeline_fault" not in latest_safe_candidate_report["verdicts"]

        wgc_sync_delay_20260620_173427_truncated_smoothness = make_session(
            "wgc_sync_delay_20260620_173427_truncated_smoothness",
            media=(
                "[WGC CFR SUMMARY] Live=12371 Dup=2251 DupPct=18.1% NoFresh=214pm NoReserve=246pm "
                "DupReason(src=2251 def=0 timer=0 drain=0) SourceLimitedRepeats=2251 StarvedEpisodes=229 "
                "longest=4672ms longestDup=194 worstIn=4 worstDel=4\n"
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=101 "
                "phaseErrorMax=245788us shortfallMax=0.0ms staleDebtDrops=2 liveRebase=0/0 "
                "tooNewRepeats=1314 syncDelayHolds=1314 tooNewLeadMax=165307us avDelay=32.3ms "
                "startupDelay=32.3ms scheduleOffset=292441us effectiveDelay=32.3ms "
                "lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0 "
                "syncDelaySourceLimitedHolds=1093 syncDelayPolicyHolds=221 startupReserveFrames=15 "
                "startupReserveSpan=107502us startupDelayTarget=32281us startupReserveSelected=1 "
                "startupReserveReason=partial_span_timeout delayReservoirLowWaterFrames=4 "
                "delayReservoirTargetFrames=5 delayReservoirLowWaterTicks=3049 realizedDelayAvg=31990us "
                "realizedDelayMin=22427us realizedDelayMax=193734us delayResidualAvg=290/2377us "
                "delayResidualMax=161453us delayResidualP95=5000us delayResidualLateMax=9854us "
                "delayResidualEarlyMax=161453us rawResidualAvg=370/2403us rawResidualMax=79471us "
                "rawResidualP95=5000us rawResidualLateMax=9958us rawResidualEarlyMax=79471us "
                "predictedResidualAvg=290/2377us predictedResidualP95=5000us "
                "predictedResidualLateMax=9854us rawMinusPredictedAvg=80/80us "
                "rawMinusPredictedMax=163331us delayResidualRelaxedSelections=515 "
                "delayResidualRelaxedMax=9854us delayResidualRelaxedRejectedSync=156642 "
                "delayRepeatClusterPressure=375 delayRepeatClusterMax=110 "
                "delayResidualRelaxedBetter=465 delayResidualRelaxedCluster=1 "
                "delayResidualRelaxedRejectedHeadroom=3032 delayResidualRelaxedRejectedCost=3006 "
                "delaySoftLateRejected=2804 delaySoftLateAccepted=496 "
                "delayOlderFrameAvoidedRepeat=9545 delaySourceLimitedRepeats=1097 "
                "delayRepeatRescue=0/1314 delayRepeatRescueRejected=3893/325/335 "
                "delayRepeatPromoted=0/1314 delayRepeatPromoteRejectedSoft=273 "
                "delayRepeatSafeAfterPromote=593 delayRepeatSafeCandidate=593 "
                "delayRepeatNoSafeCandidate=721 delayRepeatWindowClass=65/204/1045 "
                "delayRepeatReserveMax=8/107104us delaySourceRecoveryHolds=251 "
                "delaySourceRecoveryTicks=6845 delayPostSelectionRejectedSync=0 "
                "delayPostSelectionRescuedSync=0 sourceRepeatLowerBound=1097 excessRepeats=221 "
                "policyAddedRepeats=221 excessRe\n"
                "[STOP AUDIO TRACK] Track 1: encoded=4948800 expected=4948800 diff=+0 (+0.000 ms) "
                "sources=[1,3,5,7]\n"
                "[STOP AUDIO TRACK] Track 2: encoded=4948800 expected=4948800 diff=+0 (+0.000 ms) "
                "sources=[4,6,8]\n"
                "[STOP AUDIO TRACK] Track 3: encoded=4948800 expected=4948800 diff=+0 (+0.000 ms) "
                "sources=[0,2]\n"
                "[VideoEncoder] Final packet timeline: target=103100000 us videoEnd=103100000 us "
                "audioMinEnd=103100000 us audioMaxEnd=103100000 us maxPacketDelta=0 us streams(v=1 a=3) "
                "audioPastTarget=0\n"
                "[VideoEncoder] Final metadata durations: target=103100000 us video=103100000 us "
                "audioMin=103100000 us audioMax=103100000 us maxDelta=0 us streams(v=1 a=3) "
                "overload(encoder=0 mux=0) backpressure=0\n"
            ),
        )
        truncated_smoothness_report = classify_session_triage(
            wgc_sync_delay_20260620_173427_truncated_smoothness
        )
        truncated_smoothness_summary = truncated_smoothness_report["evidence"]["wgc_smoothness_summary"][0]
        assert truncated_smoothness_summary["policy_added_repeats"] == 221
        assert truncated_smoothness_summary["wgc_smoothness_evidence_incomplete"] == 1
        assert "wgc_smoothness_evidence_incomplete" in truncated_smoothness_report["verdicts"]
        assert "wgc_cfr_smoothness_not_maximal" in truncated_smoothness_report["verdicts"]
        assert "ce_visual_timeline_fault" in truncated_smoothness_report["verdicts"]
        assert "ce_audio_timeline_fault" not in truncated_smoothness_report["verdicts"]

        wgc_sync_delay_20260620_173427_split_smoothness = make_session(
            "wgc_sync_delay_20260620_173427_split_smoothness",
            media=(
                "[WGC CFR SUMMARY] Live=12371 Dup=2251 DupPct=18.1% NoFresh=214pm NoReserve=246pm "
                "DupReason(src=2251 def=0 timer=0 drain=0) SourceLimitedRepeats=2251 StarvedEpisodes=229 "
                "longest=4672ms longestDup=194 worstIn=4 worstDel=4\n"
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=101 "
                "phaseErrorMax=245788us shortfallMax=0.0ms staleDebtDrops=2 liveRebase=0/0 "
                "tooNewRepeats=1314 syncDelayHolds=1314 tooNewLeadMax=165307us avDelay=32.3ms "
                "startupDelay=32.3ms scheduleOffset=292441us effectiveDelay=32.3ms "
                "lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0 "
                "syncDelaySourceLimitedHolds=1093 syncDelayPolicyHolds=221 startupReserveFrames=15 "
                "startupReserveSpan=107502us startupDelayTarget=32281us startupReserveSelected=1 "
                "startupReserveReason=partial_span_timeout\n"
                "[WGC CFR SMOOTHNESS DELAY] delayReservoirLowWaterFrames=4 delayReservoirTargetFrames=5 "
                "delayReservoirLowWaterTicks=3049 realizedDelayAvg=31990us realizedDelayMin=22427us "
                "realizedDelayMax=193734us delayResidualAvg=290/2377us delayResidualMax=161453us "
                "delayResidualP95=5000us delayResidualLateMax=9854us delayResidualEarlyMax=161453us "
                "rawResidualAvg=370/2403us rawResidualMax=79471us rawResidualP95=5000us "
                "rawResidualLateMax=9958us rawResidualEarlyMax=79471us "
                "predictedResidualAvg=290/2377us predictedResidualP95=5000us "
                "predictedResidualLateMax=9854us rawMinusPredictedAvg=80/80us "
                "rawMinusPredictedMax=163331us\n"
                "[WGC CFR SMOOTHNESS REPEAT] delayResidualRelaxedSelections=515 "
                "delayResidualRelaxedMax=9854us delayResidualRelaxedRejectedSync=156642 "
                "delayRepeatClusterPressure=375 delayRepeatClusterMax=110 "
                "delayResidualRelaxedBetter=465 delayResidualRelaxedCluster=1 "
                "delayResidualRelaxedRejectedHeadroom=3032 delayResidualRelaxedRejectedCost=3006 "
                "delaySoftLateRejected=2804 delaySoftLateAccepted=496 "
                "delayOlderFrameAvoidedRepeat=9545 delaySourceLimitedRepeats=1097 "
                "delayRepeatRescue=0/1314 delayRepeatRescueRejected=3893/325/335 "
                "delayRepeatPromoted=0/1314 delayRepeatPromoteRejectedSoft=273 "
                "delayRepeatSafeAfterPromote=593 delayRepeatSafeCandidate=593 "
                "delayRepeatNoSafeCandidate=721 delayRepeatWindowClass=65/204/1045 "
                "delayRepeatReserveMax=8/107104us delaySourceRecoveryHolds=251 "
                "delaySourceRecoveryTicks=6845\n"
                "[WGC CFR SMOOTHNESS VERDICT] delayPostSelectionRejectedSync=0 "
                "delayPostSelectionRescuedSync=0 sourceRepeatLowerBound=1097 excessRepeats=221 "
                "policyAddedRepeats=221 excessRepeatClusters=0 excessRepeatClusterMax=0 "
                "smoothnessNotMaximal=1 mixedPolicyFault=0\n"
                "[STOP AUDIO TRACK] Track 1: encoded=4948800 expected=4948800 diff=+0 (+0.000 ms) "
                "sources=[1,3,5,7]\n"
                "[VideoEncoder] Final packet timeline: target=103100000 us videoEnd=103100000 us "
                "audioMinEnd=103100000 us audioMaxEnd=103100000 us maxPacketDelta=0 us streams(v=1 a=3) "
                "audioPastTarget=0\n"
            ),
        )
        split_smoothness_report = classify_session_triage(wgc_sync_delay_20260620_173427_split_smoothness)
        split_smoothness_summary = split_smoothness_report["evidence"]["wgc_smoothness_summary"][0]
        assert split_smoothness_summary["policy_added_repeats"] == 221
        assert split_smoothness_summary.get("wgc_smoothness_evidence_incomplete", 0) == 0
        assert "wgc_smoothness_evidence_incomplete" not in split_smoothness_report["verdicts"]
        assert "wgc_cfr_smoothness_not_maximal" in split_smoothness_report["verdicts"]
        assert "ce_visual_timeline_fault" in split_smoothness_report["verdicts"]
        assert "ce_audio_timeline_fault" not in split_smoothness_report["verdicts"]

        wgc_sync_delay_20260619_162335 = make_session(
            "wgc_sync_delay_20260619_162335",
            media=(
                "[WGC CFR SUMMARY] Live=10617 Dup=1814 DupPct=17.0% NoFresh=239pm NoReserve=260pm "
                "DupReason(src=1814 def=0 timer=0 drain=0) SourceLimitedRepeats=1814 StarvedEpisodes=227 "
                "longest=3156ms longestDup=189 worstIn=4 worstDel=4\n"
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=86 "
                "phaseErrorMax=6773us shortfallMax=0.0ms staleDebtDrops=41 liveRebase=0/0 "
                "tooNewRepeats=1213 syncDelayHolds=1213 tooNewLeadMax=140542us avDelay=32.4ms "
                "startupDelay=32.4ms scheduleOffset=303131us effectiveDelay=32.4ms "
                "lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0 "
                "syncDelaySourceLimitedHolds=972 syncDelayPolicyHolds=241 startupReserveFrames=26 "
                "startupReserveSpan=189883us startupDelayTarget=32432us startupReserveSelected=1 "
                "startupReserveReason=selected delayReservoirLowWaterFrames=4 "
                "delayReservoirTargetFrames=5 delayReservoirLowWaterTicks=2528 realizedDelayAvg=31820us "
                "realizedDelayMin=20115us realizedDelayMax=199459us delayResidualAvg=611/2420us "
                "delayResidualMax=167027us delayResidualP95=5000us delayResidualLateMax=12317us "
                "delayResidualEarlyMax=167027us delayResidualRelaxedSelections=585 "
                "delayResidualRelaxedMax=12317us delayResidualRelaxedRejectedSync=66052 "
                "delayRepeatClusterPressure=218 delayRepeatClusterMax=189 "
                "delayResidualRelaxedBetter=580 delayResidualRelaxedCluster=3 "
                "delayResidualRelaxedRejectedHeadroom=391 delayResidualRelaxedRejectedCost=5314 "
                "delaySourceRecoveryHolds=379 delaySourceRecoveryTicks=6658 mixedPolicyFault=0\n"
                "[STOP AUDIO TRACK] Track 1: encoded=4246800 expected=4246800 diff=+0 (+0.000 ms) "
                "sources=[1,3,5,7]\n"
                "[STOP AUDIO TRACK] Track 2: encoded=4246800 expected=4246800 diff=+0 (+0.000 ms) "
                "sources=[4,6,8]\n"
                "[STOP AUDIO TRACK] Track 3: encoded=4246800 expected=4246800 diff=+0 (+0.000 ms) "
                "sources=[0,2]\n"
                "[VideoEncoder] Final packet timeline: target=88475000 us videoEnd=88475000 us "
                "audioMinEnd=88475000 us audioMaxEnd=88475000 us maxPacketDelta=0 us streams(v=1 a=3) "
                "audioPastTarget=0\n"
                "[VideoEncoder] Final metadata durations: target=88475000 us video=88475000 us "
                "audioMin=88475000 us audioMax=88475000 us maxDelta=0 us streams(v=1 a=3) "
                "overload(encoder=0 mux=0) backpressure=0\n"
            ),
        )
        delay_162335_report = classify_session_triage(wgc_sync_delay_20260619_162335)
        assert "wgc_source_starvation" in delay_162335_report["verdicts"]
        assert "wgc_av_sync_delay_residual" in delay_162335_report["verdicts"]
        assert "wgc_sync_delay_policy_fault" in delay_162335_report["verdicts"]
        assert "ce_visual_timeline_fault" in delay_162335_report["verdicts"]
        assert "ce_audio_timeline_fault" not in delay_162335_report["verdicts"]

        wgc_raw_timestamp_domain_mismatch = make_session(
            "wgc_raw_timestamp_domain_mismatch",
            media=(
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=4 "
                "phaseErrorMax=6000us shortfallMax=0.0ms staleDebtDrops=0 liveRebase=0/0 "
                "tooNewRepeats=12 syncDelayHolds=12 tooNewLeadMax=9000us avDelay=32.0ms "
                "startupDelay=32.0ms scheduleOffset=12000us effectiveDelay=32.0ms "
                "lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0 "
                "syncDelaySourceLimitedHolds=12 syncDelayPolicyHolds=0 startupReserveFrames=5 "
                "startupReserveSpan=42000us startupDelayTarget=32000us startupReserveSelected=1 "
                "startupReserveReason=selected delayReservoirLowWaterFrames=4 "
                "delayReservoirTargetFrames=5 delayReservoirLowWaterTicks=0 realizedDelayAvg=31500us "
                "realizedDelayMin=29000us realizedDelayMax=34000us delayResidualAvg=500/2200us "
                "delayResidualMax=6000us delayResidualP95=5000us delayResidualLateMax=6000us "
                "delayResidualEarlyMax=2000us rawResidualAvg=6200/6200us rawResidualMax=14000us "
                "rawResidualP95=12000us rawResidualLateMax=14000us rawResidualEarlyMax=1000us "
                "predictedResidualAvg=500/2200us predictedResidualP95=5000us "
                "predictedResidualLateMax=6000us rawMinusPredictedAvg=5700/5700us "
                "rawMinusPredictedMax=11000us delayResidualRelaxedSelections=4 "
                "delayResidualRelaxedMax=6000us delayResidualRelaxedRejectedSync=0 "
                "delayRepeatClusterPressure=0 delayRepeatClusterMax=0 "
                "delayResidualRelaxedBetter=4 delayResidualRelaxedCluster=0 "
                "delayResidualRelaxedRejectedHeadroom=0 delayResidualRelaxedRejectedCost=0 "
                "delaySourceRecoveryHolds=0 delaySourceRecoveryTicks=0 delayPostSelectionRejectedSync=0 "
                "mixedPolicyFault=0\n"
            ),
        )
        raw_domain_report = classify_session_triage(wgc_raw_timestamp_domain_mismatch)
        raw_domain_summary = raw_domain_report["evidence"]["wgc_smoothness_summary"][0]
        assert raw_domain_summary["raw_residual_late_max_us"] == 14000
        assert "wgc_timestamp_domain_mismatch" in raw_domain_report["verdicts"]
        assert "wgc_av_sync_delay_residual" in raw_domain_report["verdicts"]
        assert "ce_visual_timeline_fault" in raw_domain_report["verdicts"]

        wgc_post_selection_reject = make_session(
            "wgc_post_selection_reject",
            media=(
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=4 "
                "phaseErrorMax=6000us shortfallMax=0.0ms staleDebtDrops=0 liveRebase=0/0 "
                "tooNewRepeats=14 syncDelayHolds=14 tooNewLeadMax=11000us avDelay=32.0ms "
                "startupDelay=32.0ms scheduleOffset=12000us effectiveDelay=32.0ms "
                "lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0 "
                "syncDelaySourceLimitedHolds=0 syncDelayPolicyHolds=14 startupReserveFrames=5 "
                "startupReserveSpan=42000us startupDelayTarget=32000us startupReserveSelected=1 "
                "startupReserveReason=selected delayReservoirLowWaterFrames=4 "
                "delayReservoirTargetFrames=5 delayReservoirLowWaterTicks=0 realizedDelayAvg=31500us "
                "realizedDelayMin=29000us realizedDelayMax=34000us delayResidualAvg=500/2200us "
                "delayResidualMax=6000us delayResidualP95=5000us delayResidualLateMax=6000us "
                "delayResidualEarlyMax=2000us rawResidualAvg=500/2200us rawResidualMax=6000us "
                "rawResidualP95=5000us rawResidualLateMax=6000us rawResidualEarlyMax=2000us "
                "predictedResidualAvg=500/2200us predictedResidualP95=5000us "
                "predictedResidualLateMax=6000us rawMinusPredictedAvg=0/0us rawMinusPredictedMax=0us "
                "delayResidualRelaxedSelections=4 delayResidualRelaxedMax=6000us "
                "delayResidualRelaxedRejectedSync=3 delayRepeatClusterPressure=2 delayRepeatClusterMax=3 "
                "delayResidualRelaxedBetter=4 delayResidualRelaxedCluster=0 "
                "delayResidualRelaxedRejectedHeadroom=0 delayResidualRelaxedRejectedCost=0 "
                "delaySourceRecoveryHolds=0 delaySourceRecoveryTicks=0 delayPostSelectionRejectedSync=3 "
                "mixedPolicyFault=0\n"
            ),
        )
        post_reject_report = classify_session_triage(wgc_post_selection_reject)
        assert "wgc_active_delay_post_selection_reject" in post_reject_report["verdicts"]
        assert "wgc_cfr_smoothness_not_maximal" in post_reject_report["verdicts"]
        assert "wgc_sync_delay_policy_fault" in post_reject_report["verdicts"]
        assert "ce_visual_timeline_fault" in post_reject_report["verdicts"]

        wgc_source_limited_lower_bound = make_session(
            "wgc_source_limited_lower_bound",
            media=(
                "[WGC CFR SUMMARY] Live=2400 Dup=260 DupPct=10.8% NoFresh=160pm NoReserve=140pm "
                "DupReason(src=260 def=0 timer=0 drain=0) SourceLimitedRepeats=260 StarvedEpisodes=60 "
                "longest=900ms longestDup=18 worstIn=90 worstDel=90\n"
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=12 "
                "phaseErrorMax=7000us shortfallMax=0.0ms staleDebtDrops=0 liveRebase=0/0 "
                "tooNewRepeats=260 syncDelayHolds=260 tooNewLeadMax=40000us avDelay=31.7ms "
                "startupDelay=31.7ms scheduleOffset=40000us effectiveDelay=31.7ms "
                "lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0 "
                "syncDelaySourceLimitedHolds=260 syncDelayPolicyHolds=0 startupReserveFrames=5 "
                "startupReserveSpan=41000us startupDelayTarget=31700us startupReserveSelected=1 "
                "startupReserveReason=selected delayReservoirLowWaterFrames=4 "
                "delayReservoirTargetFrames=5 delayReservoirLowWaterTicks=120 realizedDelayAvg=31000us "
                "realizedDelayMin=25000us realizedDelayMax=36000us delayResidualAvg=700/2300us "
                "delayResidualMax=7000us delayResidualP95=5000us delayResidualLateMax=7000us "
                "delayResidualEarlyMax=4000us rawResidualAvg=800/2400us rawResidualMax=8000us "
                "rawResidualP95=5000us rawResidualLateMax=8000us rawResidualEarlyMax=4000us "
                "predictedResidualAvg=700/2300us predictedResidualP95=5000us "
                "predictedResidualLateMax=7000us rawMinusPredictedAvg=100/500us "
                "rawMinusPredictedMax=1000us delayResidualRelaxedSelections=80 "
                "delayResidualRelaxedMax=7000us delayResidualRelaxedRejectedSync=0 "
                "delayRepeatClusterPressure=0 delayRepeatClusterMax=0 "
                "delayResidualRelaxedBetter=40 delayResidualRelaxedCluster=40 "
                "delayResidualRelaxedRejectedHeadroom=0 delayResidualRelaxedRejectedCost=0 "
                "delaySoftLateRejected=0 delaySoftLateAccepted=0 "
                "delayOlderFrameAvoidedRepeat=80 delaySourceLimitedRepeats=260 "
                "delaySourceRecoveryHolds=80 delaySourceRecoveryTicks=400 "
                "delayPostSelectionRejectedSync=0 delayPostSelectionRescuedSync=0 "
                "sourceRepeatLowerBound=260 excessRepeats=0 policyAddedRepeats=0 "
                "excessRepeatClusters=0 excessRepeatClusterMax=0 smoothnessNotMaximal=0 "
                "mixedPolicyFault=0\n"
            ),
        )
        lower_bound_report = classify_session_triage(wgc_source_limited_lower_bound)
        lower_bound_summary = lower_bound_report["evidence"]["wgc_smoothness_summary"][0]
        assert lower_bound_summary["source_repeat_lower_bound"] == 260
        assert lower_bound_summary["excess_repeats"] == 0
        assert lower_bound_summary["delay_older_frame_avoided_repeat"] == 80
        assert lower_bound_summary["delay_source_limited_repeats"] == 260
        assert "wgc_source_starvation" in lower_bound_report["verdicts"]
        assert "wgc_sync_delay_reserve_pressure" in lower_bound_report["verdicts"]
        assert "wgc_cfr_smoothness_not_maximal" not in lower_bound_report["verdicts"]
        assert "ce_visual_timeline_fault" not in lower_bound_report["verdicts"]

        wgc_small_policy_repeat_not_optimal = make_session(
            "wgc_small_policy_repeat_not_optimal",
            media=(
                "[WGC CFR SUMMARY] Live=7000 Dup=320 DupPct=4.6% NoFresh=45pm NoReserve=20pm "
                "DupReason(src=238 def=0 timer=0 drain=0) SourceLimitedRepeats=238 StarvedEpisodes=8 "
                "longest=120ms longestDup=9 worstIn=102 worstDel=102\n"
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=25 "
                "phaseErrorMax=6000us shortfallMax=0.0ms staleDebtDrops=0 liveRebase=0/0 "
                "tooNewRepeats=320 syncDelayHolds=320 tooNewLeadMax=7000us avDelay=30.0ms "
                "startupDelay=230.0ms scheduleOffset=0us effectiveDelay=230.0ms "
                "lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0 "
                "syncDelaySourceLimitedHolds=238 syncDelayPolicyHolds=82 startupReserveFrames=24 "
                "startupReserveSpan=200000us startupDelayTarget=270000us startupReserveSelected=0 "
                "startupReserveReason=partial_span_timeout\n"
                "[WGC CFR SMOOTHNESS VERDICT] delayPostSelectionRejectedSync=0 "
                "delayPostSelectionRescuedSync=0 sourceRepeatLowerBound=238 excessRepeats=82 "
                "policyAddedRepeats=82 excessRepeatClusters=0 excessRepeatClusterMax=4 "
                "smoothnessNotMaximal=0 mixedPolicyFault=0\n"
            ),
        )
        small_policy_report = classify_session_triage(wgc_small_policy_repeat_not_optimal)
        assert "wgc_cfr_smoothness_not_maximal" in small_policy_report["verdicts"]

        wgc_post_stall_recovery_repeat = make_session(
            "wgc_post_stall_recovery_repeat",
            media=(
                "[WGC CFR SUMMARY] Live=1200 Dup=160 DupPct=13.3% NoFresh=130pm NoReserve=90pm "
                "DupReason(src=160 def=0 timer=0 drain=0) SourceLimitedRepeats=120 StarvedEpisodes=8 "
                "longest=640ms longestDup=16 worstIn=70 worstDel=70\n"
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=12 "
                "phaseErrorMax=7000us shortfallMax=0.0ms staleDebtDrops=0 liveRebase=0/0 "
                "tooNewRepeats=160 syncDelayHolds=160 tooNewLeadMax=38000us avDelay=32.0ms "
                "startupDelay=32.0ms scheduleOffset=0us effectiveDelay=32.0ms "
                "lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0 "
                "syncDelaySourceLimitedHolds=120 syncDelayPolicyHolds=40 startupReserveFrames=5 "
                "startupReserveSpan=42000us startupDelayTarget=32000us startupReserveSelected=1 "
                "startupReserveReason=selected\n"
                "[WGC CFR SMOOTHNESS DELAY] delayReservoirLowWaterFrames=4 delayReservoirTargetFrames=5 "
                "delayReservoirLowWaterTicks=80 realizedDelayAvg=32000us realizedDelayMin=28000us "
                "realizedDelayMax=39000us delayResidualAvg=600/2100us delayResidualMax=7000us "
                "delayResidualP95=5000us delayResidualLateMax=7000us delayResidualEarlyMax=4000us "
                "rawResidualAvg=700/2200us rawResidualMax=8000us rawResidualP95=5000us "
                "rawResidualLateMax=8000us rawResidualEarlyMax=4000us predictedResidualAvg=600/2100us "
                "predictedResidualP95=5000us predictedResidualLateMax=7000us "
                "rawMinusPredictedAvg=100/300us rawMinusPredictedMax=900us\n"
                "[WGC CFR SMOOTHNESS REPEAT] delayResidualRelaxedSelections=40 "
                "delayResidualRelaxedMax=6000us delayResidualRelaxedRejectedSync=0 "
                "delayRepeatClusterPressure=40 delayRepeatClusterMax=8 delayResidualRelaxedBetter=20 "
                "delayResidualRelaxedCluster=20 delayResidualRelaxedRejectedHeadroom=0 "
                "delayResidualRelaxedRejectedCost=0 delaySoftLateRejected=0 delaySoftLateAccepted=0 "
                "delayOlderFrameAvoidedRepeat=20 delaySourceLimitedRepeats=120 "
                "delayRepeatRescue=0/40 delayRepeatRescueRejected=0/0/40 "
                "delayRepeatPromoted=0/40 delayRepeatPromoteRejectedSoft=0 "
                "delayRepeatSafeAfterPromote=40 delayRepeatSafeCandidate=40 delayRepeatNoSafeCandidate=120 "
                "delayRepeatSoftSafeCandidate=40 delayRepeatNoSoftSafeCandidate=120 "
                "delayRepeatWindowClass=0/40/120 delayRepeatWindowState=0/0/120/120/40 "
                "delayPostStallSafeFrames=60 delayRepeatReserveMax=5/42000us "
                "delaySourceRecoveryHolds=120 delaySourceRecoveryTicks=600\n"
                "[WGC CFR SMOOTHNESS VERDICT] delayPostSelectionRejectedSync=0 "
                "delayPostSelectionRescuedSync=0 sourceRepeatLowerBound=120 excessRepeats=40 "
                "policyAddedRepeats=40 excessRepeatClusters=8 excessRepeatClusterMax=8 "
                "smoothnessNotMaximal=0 mixedPolicyFault=0\n"
            ),
        )
        post_stall_report = classify_session_triage(wgc_post_stall_recovery_repeat)
        post_stall_summary = post_stall_report["evidence"]["wgc_smoothness_summary"][0]
        assert post_stall_summary["delay_repeat_state_post_stall"] == 40
        assert post_stall_summary["delay_post_stall_safe_frames"] == 60
        assert post_stall_summary["delay_repeat_soft_safe_candidate"] == 40
        assert "wgc_post_stall_recovery_fault" in post_stall_report["verdicts"]
        assert "ce_visual_timeline_fault" in post_stall_report["verdicts"]

        wgc_hard_only_repeat_not_soft_safe = make_session(
            "wgc_hard_only_repeat_not_soft_safe",
            media=(
                "[WGC CFR SUMMARY] Live=1200 Dup=160 DupPct=13.3% NoFresh=130pm NoReserve=90pm "
                "DupReason(src=160 def=0 timer=0 drain=0) SourceLimitedRepeats=160 StarvedEpisodes=8 "
                "longest=640ms longestDup=16 worstIn=70 worstDel=70\n"
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=12 "
                "phaseErrorMax=7000us shortfallMax=0.0ms staleDebtDrops=0 liveRebase=0/0 "
                "tooNewRepeats=160 syncDelayHolds=160 tooNewLeadMax=38000us avDelay=32.0ms "
                "startupDelay=32.0ms scheduleOffset=0us effectiveDelay=32.0ms "
                "lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0 "
                "syncDelaySourceLimitedHolds=160 syncDelayPolicyHolds=0 startupReserveFrames=5 "
                "startupReserveSpan=42000us startupDelayTarget=32000us startupReserveSelected=1 "
                "startupReserveReason=selected\n"
                "[WGC CFR SMOOTHNESS DELAY] delayReservoirLowWaterFrames=4 delayReservoirTargetFrames=5 "
                "delayReservoirLowWaterTicks=80 realizedDelayAvg=32000us realizedDelayMin=28000us "
                "realizedDelayMax=39000us delayResidualAvg=600/2100us delayResidualMax=7000us "
                "delayResidualP95=5000us delayResidualLateMax=7000us delayResidualEarlyMax=4000us "
                "rawResidualAvg=700/2200us rawResidualMax=8000us rawResidualP95=5000us "
                "rawResidualLateMax=8000us rawResidualEarlyMax=4000us predictedResidualAvg=600/2100us "
                "predictedResidualP95=5000us predictedResidualLateMax=7000us "
                "rawMinusPredictedAvg=100/300us rawMinusPredictedMax=900us\n"
                "[WGC CFR SMOOTHNESS REPEAT] delayResidualRelaxedSelections=0 "
                "delayResidualRelaxedMax=0us delayResidualRelaxedRejectedSync=0 "
                "delayRepeatClusterPressure=0 delayRepeatClusterMax=0 delayResidualRelaxedBetter=0 "
                "delayResidualRelaxedCluster=0 delayResidualRelaxedRejectedHeadroom=0 "
                "delayResidualRelaxedRejectedCost=0 delaySoftLateRejected=40 delaySoftLateAccepted=0 "
                "delayOlderFrameAvoidedRepeat=0 delaySourceLimitedRepeats=160 "
                "delayRepeatRescue=0/160 delayRepeatRescueRejected=0/40/0 "
                "delayRepeatPromoted=0/160 delayRepeatPromoteRejectedSoft=40 "
                "delayRepeatSafeAfterPromote=40 delayRepeatSafeCandidate=40 delayRepeatNoSafeCandidate=120 "
                "delayRepeatSoftSafeCandidate=0 delayRepeatNoSoftSafeCandidate=160 "
                "delayRepeatWindowClass=0/40/120 delayRepeatWindowState=0/0/120/120/40 "
                "delayPostStallSafeFrames=0 delayRepeatReserveMax=5/42000us "
                "delaySourceRecoveryHolds=120 delaySourceRecoveryTicks=600\n"
                "[WGC CFR SMOOTHNESS VERDICT] delayPostSelectionRejectedSync=0 "
                "delayPostSelectionRescuedSync=0 sourceRepeatLowerBound=160 excessRepeats=0 "
                "policyAddedRepeats=0 excessRepeatClusters=0 excessRepeatClusterMax=0 "
                "smoothnessNotMaximal=0 mixedPolicyFault=0\n"
            ),
        )
        hard_only_report = classify_session_triage(wgc_hard_only_repeat_not_soft_safe)
        hard_only_summary = hard_only_report["evidence"]["wgc_smoothness_summary"][0]
        assert hard_only_summary["delay_repeat_safe_candidate"] == 40
        assert hard_only_summary["delay_repeat_soft_safe_candidate"] == 0
        assert "wgc_repeat_despite_safe_candidate" not in hard_only_report["verdicts"]
        assert "wgc_post_stall_recovery_fault" not in hard_only_report["verdicts"]

        wgc_sync_delay_fortibad_audio_late_risk = make_session(
            "wgc_sync_delay_fortibad_audio_late_risk",
            media=(
                "[WGC CFR SUMMARY] Live=25926 Dup=2489 DupPct=9.6% NoFresh=120pm NoReserve=140pm "
                "DupReason(src=2489 def=0 timer=0 drain=0) SourceLimitedRepeats=2489 StarvedEpisodes=579 "
                "longest=1922ms longestDup=190 worstIn=4 worstDel=4\n"
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=221 "
                "phaseErrorMax=83180us shortfallMax=0.0ms staleDebtDrops=1 liveRebase=0/0 "
                "tooNewRepeats=2147 syncDelayHolds=2147 tooNewLeadMax=150000us avDelay=29.5ms "
                "startupDelay=29.5ms scheduleOffset=0us effectiveDelay=29.5ms "
                "lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0 "
                "syncDelaySourceLimitedHolds=2147 syncDelayPolicyHolds=0 startupReserveFrames=8 "
                "startupReserveSpan=58000us startupDelayTarget=29500us startupReserveSelected=1 "
                "startupReserveReason=selected\n"
                "[WGC CFR SMOOTHNESS DELAY] delayReservoirLowWaterFrames=4 delayReservoirTargetFrames=5 "
                "delayReservoirLowWaterTicks=1600 realizedDelayAvg=29100us realizedDelayMin=21000us "
                "realizedDelayMax=112680us delayResidualAvg=351/2249us delayResidualMax=83180us "
                "delayResidualP95=5000us delayResidualLateMax=9858us delayResidualEarlyMax=83180us "
                "rawResidualAvg=389/2344us rawResidualMax=83180us rawResidualP95=5000us "
                "rawResidualLateMax=9858us rawResidualEarlyMax=83180us predictedResidualAvg=351/2249us "
                "predictedResidualP95=5000us predictedResidualLateMax=9858us "
                "rawMinusPredictedAvg=38/300us rawMinusPredictedMax=1000us\n"
                "[WGC CFR SMOOTHNESS REPEAT] delayResidualRelaxedSelections=14717 "
                "delayResidualRelaxedMax=9858us delayResidualRelaxedRejectedSync=0 "
                "delayRepeatClusterPressure=0 delayRepeatClusterMax=0 delayResidualRelaxedBetter=12000 "
                "delayResidualRelaxedCluster=2717 delayResidualRelaxedRejectedHeadroom=0 "
                "delayResidualRelaxedRejectedCost=0 delaySoftLateRejected=14717 delaySoftLateAccepted=2211 "
                "delayOlderFrameAvoidedRepeat=16 delaySourceLimitedRepeats=2147 "
                "delayRepeatRescue=16/2163 delayRepeatRescueRejected=0/1233/0 "
                "delayRepeatPromoted=16/2163 delayRepeatPromoteRejectedSoft=1233 "
                "delayRepeatSafeAfterPromote=0 delayRepeatSafeCandidate=1329 delayRepeatNoSafeCandidate=818 "
                "delayRepeatSoftSafeCandidate=0 delayRepeatNoSoftSafeCandidate=2147 "
                "delayRepeatWindowClass=0/0/2147 delayRepeatWindowState=0/0/2147/2147/0 "
                "delayPostStallSafeFrames=0 delayRepeatReserveMax=5/58000us "
                "delaySourceRecoveryHolds=0 delaySourceRecoveryTicks=0 "
                "delayNearCapAccepted=2211 delayHardOnlyCandidates=1329 "
                "delaySyncProtectedRepeats=2147 delayOldestSoftSafeAgeMax=0us\n"
                "[WGC CFR SMOOTHNESS VERDICT] delayPostSelectionRejectedSync=0 "
                "delayPostSelectionRescuedSync=0 sourceRepeatLowerBound=2147 excessRepeats=0 "
                "policyAddedRepeats=0 excessRepeatClusters=0 excessRepeatClusterMax=0 "
                "smoothnessNotMaximal=0 mixedPolicyFault=0\n"
                "[STOP AUDIO TRACK] Track 1: encoded=1000 expected=1000 diff=+0 (+0.000 ms) sources=[1]\n"
                "[VideoEncoder] Final packet timeline: target=1000 us videoEnd=1000 us "
                "audioMinEnd=1000 us audioMaxEnd=1000 us maxPacketDelta=0 us streams(v=1 a=1) audioPastTarget=0\n"
                "[VideoEncoder] Final metadata durations: target=1000 us video=1000 us "
                "audioMin=1000 us audioMax=1000 us maxDelta=0 us streams(v=1 a=1) "
                "overload(encoder=0 mux=0) backpressure=0\n"
            ),
        )
        fortibad_risk_report = classify_session_triage(wgc_sync_delay_fortibad_audio_late_risk)
        fortibad_risk_summary = fortibad_risk_report["evidence"]["wgc_smoothness_summary"][0]
        assert fortibad_risk_summary["delay_soft_late_accepted"] == 2211
        assert fortibad_risk_summary["delay_near_cap_accepted"] == 2211
        assert fortibad_risk_summary["policy_added_repeats"] == 0
        assert "wgc_audio_late_risk" in fortibad_risk_report["verdicts"]
        assert "wgc_cfr_smoothness_not_maximal" not in fortibad_risk_report["verdicts"]
        assert "ce_visual_timeline_fault" in fortibad_risk_report["verdicts"]
        assert "ce_audio_timeline_fault" not in fortibad_risk_report["verdicts"]

        wgc_sync_delay_source_limited_ceiling = make_session(
            "wgc_sync_delay_source_limited_ceiling",
            media=(
                "[WGC CFR SUMMARY] Live=131396 Dup=11732 DupPct=8.9% NoFresh=119pm NoReserve=123pm "
                "DupReason(src=11731 def=0 timer=1 drain=0) SourceLimitedRepeats=11731 StarvedEpisodes=3084 "
                "longest=7547ms longestDup=319 worstIn=24 worstDel=24\n"
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=1069 "
                "phaseErrorMax=7246us shortfallMax=0.0ms staleDebtDrops=12 liveRebase=0/0 "
                "tooNewRepeats=11302 syncDelayHolds=11302 tooNewLeadMax=140610us avDelay=33.0ms "
                "startupDelay=33.0ms scheduleOffset=81368us effectiveDelay=33.0ms "
                "lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0 "
                "syncDelaySourceLimitedHolds=11302 syncDelayPolicyHolds=0 startupReserveFrames=33 "
                "startupReserveSpan=228518us startupDelayTarget=33045us startupReserveSelected=1 "
                "startupReserveReason=selected\n"
                "[WGC CFR SMOOTHNESS DELAY] delayReservoirLowWaterFrames=4 delayReservoirTargetFrames=5 "
                "delayReservoirLowWaterTicks=16239 realizedDelayAvg=32792us realizedDelayMin=23269us "
                "realizedDelayMax=195397us delayResidualAvg=252/2164us delayResidualMax=162352us "
                "delayResidualP95=5000us delayResidualLateMax=9776us delayResidualEarlyMax=162352us "
                "rawResidualAvg=264/2276us rawResidualMax=49465us rawResidualP95=5000us "
                "rawResidualLateMax=9981us rawResidualEarlyMax=49465us predictedResidualAvg=252/2164us "
                "predictedResidualP95=5000us predictedResidualLateMax=9776us "
                "rawMinusPredictedAvg=11/11us rawMinusPredictedMax=167508us\n"
                "[WGC CFR SMOOTHNESS REPEAT] delayResidualRelaxedSelections=4141 "
                "delayResidualRelaxedMax=9776us delayResidualRelaxedRejectedSync=1916996 "
                "delayRepeatClusterPressure=641 delayRepeatClusterMax=176 delayResidualRelaxedBetter=3126 "
                "delayResidualRelaxedCluster=1 delayResidualRelaxedRejectedHeadroom=97704 "
                "delayResidualRelaxedRejectedCost=2108 delaySoftLateRejected=97640 delaySoftLateAccepted=0 "
                "delayOlderFrameAvoidedRepeat=115574 delaySourceLimitedRepeats=11328 "
                "delayRepeatRescue=49/11351 delayRepeatRescueRejected=42801/7965/212 "
                "delayRepeatPromoted=49/11351 delayRepeatPromoteRejectedSoft=7960 "
                "delayRepeatSafeAfterPromote=7531 delayRepeatSafeCandidate=7531 "
                "delayRepeatNoSafeCandidate=3771 delayRepeatSoftSafeCandidate=0 "
                "delayRepeatNoSoftSafeCandidate=11302 delayRepeatWindowClass=1012/6329/3961 "
                "delayRepeatWindowState=1012/6329/3961/3771/3082 delayPostStallSafeFrames=45288 "
                "delayRepeatReserveMax=9/56199us delaySourceRecoveryHolds=3804 "
                "delaySourceRecoveryTicks=50346 delayNearCapAccepted=2104 delayHardOnlyCandidates=7531 "
                "delaySyncProtectedRepeats=11302 delayOldestSoftSafeAgeMax=0us\n"
                "[WGC CFR SMOOTHNESS VERDICT] delayPostSelectionRejectedSync=0 "
                "delayPostSelectionRescuedSync=0 sourceRepeatLowerBound=11328 excessRepeats=0 "
                "policyAddedRepeats=0 excessRepeatClusters=0 excessRepeatClusterMax=0 "
                "smoothnessNotMaximal=0 mixedPolicyFault=0\n"
                "[STOP AUDIO TRACK] Track 1: encoded=52558800 expected=52558800 diff=+0 (+0.000 ms) sources=[1]\n"
                "[VideoEncoder] Final packet timeline: target=1094975000 us videoEnd=1094975000 us "
                "audioMinEnd=1094975000 us audioMaxEnd=1094975000 us maxPacketDelta=0 us streams(v=1 a=1) audioPastTarget=0\n"
                "[VideoEncoder] Final metadata durations: target=1094975000 us video=1094975000 us "
                "audioMin=1094975000 us audioMax=1094975000 us maxDelta=0 us streams(v=1 a=1) "
                "overload(encoder=0 mux=0) backpressure=0\n"
            ),
        )
        ceiling_report = classify_session_triage(wgc_sync_delay_source_limited_ceiling)
        ceiling_limits = ceiling_report["evidence"]["wgc_source_limits"]
        assert ceiling_limits["summary_duplicate"] == 11732
        assert ceiling_limits["summary_longest_dup_ticks"] == 319
        assert "wgc_source_limited_smoothness_ceiling" in ceiling_report["verdicts"]
        assert "wgc_audio_late_risk" not in ceiling_report["verdicts"]
        assert "wgc_cfr_smoothness_not_maximal" not in ceiling_report["verdicts"]
        assert "ce_visual_timeline_fault" not in ceiling_report["verdicts"]
        assert "ce_audio_timeline_fault" not in ceiling_report["verdicts"]

        wgc_sync_delay_20260619_214948 = make_session(
            "wgc_sync_delay_20260619_214948",
            media=(
                "[WGC CFR SUMMARY] Live=39375 Dup=4230 DupPct=9.7% NoFresh=180pm NoReserve=190pm "
                "DupReason(src=4230 def=0 timer=0 drain=0) SourceLimitedRepeats=4230 StarvedEpisodes=980 "
                "longest=6300ms longestDup=178 worstIn=4 worstDel=4\n"
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=221 "
                "phaseErrorMax=100000us shortfallMax=0.0ms staleDebtDrops=1 liveRebase=0/0 "
                "tooNewRepeats=3433 syncDelayHolds=3433 tooNewLeadMax=170000us avDelay=31.7ms "
                "startupDelay=31.7ms scheduleOffset=300000us effectiveDelay=31.7ms "
                "lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0 "
                "syncDelaySourceLimitedHolds=2299 syncDelayPolicyHolds=1134 startupReserveFrames=20 "
                "startupReserveSpan=180000us startupDelayTarget=31700us startupReserveSelected=1 "
                "startupReserveReason=selected delayReservoirLowWaterFrames=4 "
                "delayReservoirTargetFrames=5 delayReservoirLowWaterTicks=6344 realizedDelayAvg=31183us "
                "realizedDelayMin=20000us realizedDelayMax=190000us delayResidualAvg=519/2333us "
                "delayResidualMax=160000us delayResidualP95=5000us delayResidualLateMax=9978us "
                "delayResidualEarlyMax=160000us rawResidualAvg=588/2450us rawResidualMax=170000us "
                "rawResidualP95=5000us rawResidualLateMax=9999us rawResidualEarlyMax=170000us "
                "predictedResidualAvg=519/2333us predictedResidualP95=5000us "
                "predictedResidualLateMax=9978us rawMinusPredictedAvg=69/600us "
                "rawMinusPredictedMax=1000us delayResidualRelaxedSelections=1719 "
                "delayResidualRelaxedMax=9978us delayResidualRelaxedRejectedSync=476062 "
                "delayRepeatClusterPressure=483 delayRepeatClusterMax=178 "
                "delayResidualRelaxedBetter=1713 delayResidualRelaxedCluster=3 "
                "delayResidualRelaxedRejectedHeadroom=252 delayResidualRelaxedRejectedCost=14635 "
                "delaySourceRecoveryHolds=874 delaySourceRecoveryTicks=15609 "
                "delayPostSelectionRejectedSync=1 delayPostSelectionRescuedSync=0 "
                "sourceRepeatLowerBound=2299 excessRepeats=1134 policyAddedRepeats=1134 "
                "excessRepeatClusters=483 excessRepeatClusterMax=178 smoothnessNotMaximal=1 "
                "mixedPolicyFault=1\n"
                "[STOP AUDIO TRACK] Track 1: encoded=1000 expected=1000 diff=+0 (+0.000 ms) sources=[1]\n"
                "[VideoEncoder] Final packet timeline: target=1000 us videoEnd=1000 us "
                "audioMinEnd=1000 us audioMaxEnd=1000 us maxPacketDelta=0 us streams(v=1 a=1) audioPastTarget=0\n"
                "[VideoEncoder] Final metadata durations: target=1000 us video=1000 us "
                "audioMin=1000 us audioMax=1000 us maxDelta=0 us streams(v=1 a=1) "
                "overload(encoder=0 mux=0) backpressure=0\n"
            ),
        )
        delay_214948_report = classify_session_triage(wgc_sync_delay_20260619_214948)
        delay_214948_summary = delay_214948_report["evidence"]["wgc_smoothness_summary"][0]
        assert delay_214948_summary["policy_added_repeats"] == 1134
        assert delay_214948_summary["excess_repeat_cluster_max_ticks"] == 178
        assert "wgc_source_starvation" in delay_214948_report["verdicts"]
        assert "wgc_active_delay_post_selection_reject" in delay_214948_report["verdicts"]
        assert "wgc_cfr_smoothness_not_maximal" in delay_214948_report["verdicts"]
        assert "wgc_sync_delay_policy_fault" in delay_214948_report["verdicts"]
        assert "ce_visual_timeline_fault" in delay_214948_report["verdicts"]
        assert "ce_audio_timeline_fault" not in delay_214948_report["verdicts"]

        wgc_sync_delay_long_run_p5_mixed_pressure = make_session(
            "wgc_sync_delay_long_run_p5_mixed_pressure",
            media=(
                "[WGC CFR SUMMARY] Live=150230 Dup=13764 DupPct=9.1% NoFresh=142pm NoReserve=148pm "
                "DupReason(src=13764 def=0 timer=0 drain=0) SourceLimitedRepeats=13764 StarvedEpisodes=3743 "
                "longest=11406ms longestDup=528 worstIn=24 worstDel=24\n"
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=1235 "
                "phaseErrorMax=279304us shortfallMax=0.0ms staleDebtDrops=1 liveRebase=0/0 "
                "tooNewRepeats=13140 syncDelayHolds=13140 tooNewLeadMax=195524us avDelay=32.3ms "
                "startupDelay=32.3ms scheduleOffset=-236210us effectiveDelay=32.3ms "
                "lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0 "
                "syncDelaySourceLimitedHolds=5859 syncDelayPolicyHolds=7281 startupReserveFrames=1 "
                "startupReserveSpan=0us startupDelayTarget=32276us startupReserveSelected=0 "
                "startupReserveReason=reserve_timeout delayReservoirLowWaterFrames=4 "
                "delayReservoirTargetFrames=5 delayReservoirLowWaterTicks=22377 realizedDelayAvg=31715us "
                "realizedDelayMin=18476us realizedDelayMax=62767us delayResidualAvg=560/2297us "
                "delayResidualMax=30491us delayResidualP95=5000us delayResidualLateMax=13800us "
                "delayResidualEarlyMax=30491us delayResidualRelaxedSelections=7917 "
                "delayResidualRelaxedMax=13800us delayResidualRelaxedRejectedSync=44 "
                "delayRepeatClusterPressure=7281 delayRepeatClusterMax=120 "
                "delayResidualRelaxedBetter=2400 delayResidualRelaxedCluster=5517 "
                "delayResidualRelaxedRejectedHeadroom=120 delayResidualRelaxedRejectedCost=64 "
                "delaySourceRecoveryHolds=3200 delaySourceRecoveryTicks=8000 mixedPolicyFault=1\n"
                "[PullAudio] App source gap silence: track=1 src=5 buffered=0 requested=400 "
                "target=219800ms encoded=10550000. Source contributes silence for missing range.\n"
                "[PullAudio] WARNING: Source underrun - src 5 padding 400 samples with silence "
                "(available=0 needed=400 forceDrain=0)\n"
                "[STOP AUDIO TRACK] Track 1: encoded=60092400 expected=60092400 diff=+0 (+0.000 ms) "
                "sources=[1,3,5,7]\n"
                "[STOP AUDIO] Source 5: track=1 encoded=60092400 trim=cov:0 latTotal:0 liveUncat:0 "
                "pad:49648177 qgap:480 qjoin:5232636 qjoinKeep:480 ringPeak=0 ringUnderruns=124120 "
                "process=Brave.exe\n"
                "[VideoEncoder] Final packet timeline: target=1251925000 us videoEnd=1251925000 us "
                "audioMinEnd=1251925000 us audioMaxEnd=1251925000 us maxPacketDelta=0 us "
                "streams(v=1 a=3) audioPastTarget=0\n"
                "[VideoEncoder] Final metadata durations: target=1251925000 us video=1251925000 us "
                "audioMin=1251925000 us audioMax=1251925000 us maxDelta=0 us streams(v=1 a=3) "
                "overload(encoder=0 mux=0) backpressure=0\n"
            ),
        )
        long_run_report = classify_session_triage(wgc_sync_delay_long_run_p5_mixed_pressure)
        long_run_summary = long_run_report["evidence"]["wgc_smoothness_summary"][0]
        assert long_run_summary["delay_relaxed_rejected_sync"] == 44
        assert long_run_summary["delay_repeat_cluster_pressure"] == 7281
        assert long_run_summary["delay_repeat_cluster_max_ticks"] == 120
        assert long_run_summary["delay_relaxed_better_target"] == 2400
        assert long_run_summary["delay_relaxed_repeat_cluster"] == 5517
        assert long_run_summary["delay_relaxed_rejected_headroom"] == 120
        assert long_run_summary["delay_relaxed_rejected_cost"] == 64
        assert long_run_summary["delay_source_recovery_holds"] == 3200
        assert long_run_summary["delay_source_recovery_ticks"] == 8000
        assert "wgc_source_starvation" in long_run_report["verdicts"]
        assert "wgc_av_sync_delay_residual" in long_run_report["verdicts"]
        assert "wgc_sync_delay_policy_fault" in long_run_report["verdicts"]
        assert "sparse_app_source_silence" in long_run_report["verdicts"]
        assert "ce_visual_timeline_fault" in long_run_report["verdicts"]
        assert "ce_audio_timeline_fault" not in long_run_report["verdicts"]
        assert long_run_report["evidence"]["exported_av_sync_ok"]

        wgc_sync_delay_policy_fault = make_session(
            "wgc_sync_delay_policy_fault",
            media=(
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=90 "
                "phaseErrorMax=200000us shortfallMax=41.7ms staleDebtDrops=0 liveRebase=0/0 "
                "tooNewRepeats=1280 syncDelayHolds=1280 tooNewLeadMax=261706us avDelay=34.8ms "
                "startupDelay=34.8ms scheduleOffset=58559us effectiveDelay=34.8ms "
                "lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0\n"
            ),
        )
        policy_fault_report = classify_session_triage(wgc_sync_delay_policy_fault)
        assert "wgc_av_sync_delay_unrealized" not in policy_fault_report["verdicts"]
        assert "wgc_sync_delay_policy_fault" in policy_fault_report["verdicts"]
        assert "ce_visual_timeline_fault" in policy_fault_report["verdicts"]

        source_starved_with_overload = make_session(
            "source_starved_with_overload",
            media=(
                "[WGC CFR] Source-starved episode: duration=240ms out=29 dup=29 minIn=0 minDel=120 "
                "freshMiss=1000pm minBuf=0\n"
                "[Cadence Health] Phase=Live | WgcSelBias=260000us | Shortfall=32/266.7ms "
                "LeadExcess=116.0ms | Oldest=250.0ms BufNow=0 | WgcLiveRebase=1/120/1 | Over=0x1\n"
                "[EncoderThread] WGC CFR slot repeat: buffered frame is too new for scheduled slot "
                "(lead=28000us targetQpc=1000 firstQpc=1280 buffered=0 shortfall=32)\n"
            ),
        )
        report = classify_session_triage(source_starved_with_overload)
        assert "wgc_source_starvation" in report["verdicts"]
        assert "wgc_encoder_limited_judder" not in report["verdicts"]

        multi_app_stall = make_session(
            "multi_app_stall",
            media=(
                "[STOP AUDIO TRACK] Track 1: encoded=1360000 expected=12490400 "
                "diff=-11130400 (-231883.333 ms) sources=[1,3,5,7,9]\n"
                "[STOP AUDIO TRACK] Track 2: encoded=1360000 expected=12490400 "
                "diff=-11130400 (-231883.333 ms) sources=[4,6,8,10]\n"
                "[STOP AUDIO TRACK] Track 3: encoded=12490400 expected=12490400 diff=0 (0.000 ms) sources=[2]\n"
            ),
        )
        report = classify_session_triage(multi_app_stall)
        assert "multi_app_audio_track_stall" in report["verdicts"]
        assert "ce_audio_timeline_fault" in report["verdicts"]
        assert report["evidence"]["stop_audio_shortfalls"]["multi_source_short_count"] == 2

        late_app_backlog = make_session(
            "late_app_backlog",
            media=(
                "[PullAudio] Source primed - src=9 track=1 buffered=359086 realBuffered=358483 needed=1200 "
                "lateStart=7459ms\n"
                "[PullAudio] App source gap silence - src 9 added 480 samples to track 1\n"
                "[PullAudio] WARNING: Source underrun - src 9 padding 480 samples with silence "
                "(available=0 needed=480 forceDrain=0)\n"
            ),
        )
        report = classify_session_triage(late_app_backlog)
        assert "late_app_source_backlog" in report["verdicts"]
        assert "started_app_source_underrun" in report["verdicts"]
        assert "ce_audio_timeline_fault" in report["verdicts"]
        assert report["faults"]["late_app_source_backlog"]

        exported_sync_with_app_underrun = make_session(
            "exported_sync_with_app_underrun",
            media=(
                "[PullAudio] WARNING: Source underrun - src 9 padding 480 samples with silence "
                "(available=0 needed=480 forceDrain=0)\n"
                "[PullAudio] App source gap silence - src 9 added 480 samples to track 1\n"
                "[STOP AUDIO] Source 9: track=1 encoded=48000 trim=cov:0 latTotal:0 liveUncat:0 "
                "pad:480 qgap:0 qjoin:0 qjoinKeep:0 ringPeak=1200 ringUnderruns=1 process=game.exe\n"
                "[VideoEncoder] Final packet timeline: target=60000000 us videoEnd=60000000 us "
                "audioMinEnd=60000000 us audioMaxEnd=60000000 us maxPacketDelta=0 us audioPastTarget=0\n"
                "[VideoEncoder] Final metadata durations: target=60000000 us video=60000000 us "
                "audioMin=60000000 us audioMax=60000000 us maxDelta=0 us streams(v=1 a=3) "
                "overload(encoder=0 mux=0) backpressure=0\n"
            ),
        )
        report = classify_session_triage(exported_sync_with_app_underrun)
        assert "started_app_source_underrun" in report["verdicts"]
        assert "ce_audio_timeline_fault" not in report["verdicts"]
        assert report["evidence"]["exported_av_sync_ok"]

        late_app_live_join = make_session(
            "late_app_live_join",
            media=(
                "[AudioLoop] Late app source live join src=9 track=1 process=dx12_av_sync_late.exe "
                "packetStart=358003 trackCursor=358483 joinCursor=358483 suppressedGap=358003 "
                "preservedGap=0 qpcStart=123\n"
                "[PullAudio] Source primed - src=9 track=1 buffered=1200 realBuffered=1200 needed=1200 "
                "lateStart=7459ms\n"
                "[STOP AUDIO] Source 9: track=1 encoded=48000 trim=cov:0 latTotal:0 liveUncat:0 "
                "pad:0 qgap:0 qjoin:358003 qjoinKeep:0 ringPeak=1200 ringUnderruns=0 process=dx12_av_sync_late.exe\n"
            ),
        )
        report = classify_session_triage(late_app_live_join)
        assert "late_app_source_backlog" not in report["verdicts"]
        assert "ce_audio_timeline_fault" not in report["verdicts"]
        assert report["evidence"]["started_app_source_health"]["late_join_live_count"] == 1

        sparse_app_source_silence = make_session(
            "sparse_app_source_silence",
            media=(
                "[PullAudio] App source gap silence: track=1 src=5 buffered=0 requested=400 "
                "target=219800ms encoded=10550000. Source contributes silence for missing range.\n"
                "[PullAudio] WARNING: Source underrun - src 5 padding 400 samples with silence "
                "(available=0 needed=400 forceDrain=0)\n"
                "[STOP AUDIO TRACK] Track 1: encoded=60092400 expected=60092400 diff=+0 (+0.000 ms) "
                "sources=[1,3,5,7]\n"
                "[STOP AUDIO] Source 5: track=1 encoded=60092400 trim=cov:0 latTotal:0 liveUncat:0 "
                "pad:49648177 qgap:480 qjoin:5232636 qjoinKeep:480 ringPeak=0 ringUnderruns=124120 "
                "process=Brave.exe\n"
                "[VideoEncoder] Final packet timeline: target=1251925000 us videoEnd=1251925000 us "
                "audioMinEnd=1251925000 us audioMaxEnd=1251925000 us maxPacketDelta=0 us "
                "streams(v=1 a=3) audioPastTarget=0\n"
            ),
        )
        report = classify_session_triage(sparse_app_source_silence)
        assert "sparse_app_source_silence" in report["verdicts"]
        assert "started_app_source_underrun" not in report["verdicts"]
        assert "ce_audio_timeline_fault" not in report["verdicts"]
        assert report["evidence"]["audio_fault_counts"]["audio_underrun"] == 1
        assert report["evidence"]["strict_audio_fault_counts"]["audio_underrun"] == 0
        assert len(report["evidence"]["started_app_source_health"]["sparse_silence_sources"]) == 1

        app_latency_new_summary = make_session(
            "app_latency_new_summary",
            media=(
                "[STOP AUDIO LATENCY] Source 5 track=1 appAudioDelay avg=226ms max=332ms "
                "targetAvg=141ms excessAvg=85ms excessMax=191ms "
                "buckets(<50/50-150/150-300/300-600/>600ms)=0%/0%/100%/0%/0% "
                ">=150ms=100% drainObservations=60/120 transitions=2 maxComp=0.5000% "
                "queueOverrun=0/0 underruns=0 trims(lat=0 normal=0 cat=0/0). "
                "Lower/more-uniform excess is better; high excess means audio content ran behind video.\n"
                "[VideoEncoder] Final packet timeline: target=51441667 us videoEnd=51441667 us "
                "audioMinEnd=51441667 us audioMaxEnd=51441667 us maxPacketDelta=0 us "
                "streams(v=1 a=3) audioPastTarget=0\n"
            ),
        )
        report = classify_session_triage(app_latency_new_summary)
        assert "audio_app_latency_elevated" in report["verdicts"]
        assert "ce_audio_timeline_fault" not in report["verdicts"]
        assert report["faults"]["audio_app_latency_elevated"]
        assert report["evidence"]["app_audio_latency"]["elevated_source_count"] == 1
        assert report["evidence"]["app_audio_latency"]["worst_excess_avg_ms"] == 85.0

        app_latency_warning_within_slack = make_session(
            "app_latency_warning_within_slack",
            media=(
                "[2026-07-18 23:30:00.000] [INFO] [AppLatency] WARNING: app audio src=9 track=1 "
                "delayMs=289 targetMs=299 excessMs=0 rbAvail=13912 drain=0 reason=within_slack "
                "compDelta=0 comp=0.0000% rateCompActive=0 underruns=0 queuePending=0 "
                "queueOverrun=0/0. Content backlog should drain toward the video target without trims.\n"
                "[STOP AUDIO LATENCY] Source 9 track=1 appAudioDelay avg=243ms max=289ms "
                "targetAvg=292ms excessAvg=1ms excessMax=77ms "
                "buckets(<50/50-150/150-300/300-600/>600ms)=0%/0%/100%/0%/0% "
                ">=150ms=100% drainObservations=1267/47620 transitions=529 maxComp=0.3125% "
                "liveObservations=47620 stopDrainObservations=1 stopDrainAvg=1131ms "
                "stopDrainMax=1131ms queueOverrun=0/0 underruns=0 trims(lat=0 normal=0 cat=0/0). "
                "Lower/more-uniform excess is better; high excess means audio content ran behind video.\n"
                "[VideoEncoder] Final packet timeline: target=396891667 us videoEnd=396891667 us "
                "audioMinEnd=396891666 us audioMaxEnd=396891666 us maxPacketDelta=1 us "
                "streams(v=1 a=3) audioPastTarget=0\n"
            ),
        )
        report = classify_session_triage(app_latency_warning_within_slack)
        assert "audio_app_latency_elevated" not in report["verdicts"]
        assert "app_audio_latency_within_slack" in report["contexts"]
        assert not report["faults"]["audio_app_latency_elevated"]
        assert report["evidence"]["app_audio_latency"]["warning_count"] == 1
        assert report["evidence"]["app_audio_latency"]["elevated_source_count"] == 0
        assert report["evidence"]["app_audio_latency"]["warning_only_context"]
        assert not report["evidence"]["app_audio_latency"]["fault_evidence"]

        app_latency_integrity_fault = make_session(
            "app_latency_integrity_fault",
            media=read_text_if_exists(app_latency_warning_within_slack / "media.log").replace(
                "queueOverrun=0/0 underruns=0 trims(lat=0 normal=0 cat=0/0)",
                "queueOverrun=1/480 underruns=0 trims(lat=0 normal=0 cat=0/0)",
            ),
        )
        report = classify_session_triage(app_latency_integrity_fault)
        assert "audio_app_latency_elevated" in report["verdicts"]
        assert "app_audio_latency_within_slack" not in report["contexts"]
        assert report["faults"]["audio_app_latency_elevated"]
        assert report["evidence"]["app_audio_latency"]["integrity_fault"]
        assert report["evidence"]["app_audio_latency"]["queue_overrun_packets"] == 1
        assert report["evidence"]["app_audio_latency"]["queue_overrun_frames"] == 480

        app_latency_old_fallback = make_session(
            "app_latency_old_fallback",
            media=(
                "[AppLatency] WARNING: app audio src=6 track=2 is 351ms behind video "
                "(rbAvail=16874 samples, draining=0). Read-stall backlog; should drain toward live.\n"
                "[STOP AUDIO LATENCY] Source 6 track=2 appAudioDelay avg=329ms max=527ms "
                "buckets(<50/50-150/150-300/300-600/>600ms)=0%/0%/0%/100%/0% "
                ">=150ms=100% drainingSamples=0/100. Lower/more-uniform is better; high 300-600ms "
                "means audio ran noticeably behind video.\n"
                "[VideoEncoder] Final packet timeline: target=51441667 us videoEnd=51441667 us "
                "audioMinEnd=51441667 us audioMaxEnd=51441667 us maxPacketDelta=0 us "
                "streams(v=1 a=3) audioPastTarget=0\n"
            ),
        )
        report = classify_session_triage(app_latency_old_fallback)
        assert "audio_app_latency_elevated" in report["verdicts"]
        assert "ce_audio_timeline_fault" not in report["verdicts"]
        assert report["evidence"]["app_audio_latency"]["warning_count"] == 1
        assert report["evidence"]["app_audio_latency"]["elevated_source_count"] == 1

        app_latency_stop_drain_context = make_session(
            "app_latency_stop_drain_context",
            media=(
                "[2026-07-17 15:55:00.000] [INFO] [EncoderThread] Recording live (inject)\n"
                "[2026-07-17 15:56:30.690] [INFO] [Media] Stopping recording...\n"
                "[2026-07-17 15:56:31.000] [WARN] [AppLatency] WARNING: app audio src=6 track=2 "
                "delayMs=351 targetMs=60 excessMs=291\n"
                "[2026-07-17 15:56:31.033] [INFO] [STOP AUDIO LATENCY] Source 6 track=2 "
                "appAudioDelay avg=329ms max=527ms buckets(<50/50-150/150-300/300-600/>600ms)="
                "0%/0%/0%/100%/0% >=150ms=100% drainingSamples=0/100\n"
                "[VideoEncoder] Final packet timeline: target=51441667 us videoEnd=51441667 us "
                "audioMinEnd=51441667 us audioMaxEnd=51441667 us maxPacketDelta=0 us "
                "streams(v=1 a=3) audioPastTarget=0\n"
            ),
        )
        report = classify_session_triage(app_latency_stop_drain_context)
        assert "audio_app_latency_elevated" not in report["verdicts"]
        assert "app_audio_stop_drain_latency" in report["contexts"]
        assert report["evidence"]["app_audio_latency"]["warning_count"] == 0
        assert report["evidence"]["app_audio_latency"]["stop_drain_warning_count"] == 1

        crash_session = make_session(
            "crash_session",
            media="[VideoEncoder] Final metadata durations: target=1000 us video=1000 us audioMin=1000 us audioMax=1000 us maxDelta=0 us streams(v=1 a=2) overload(encoder=0 mux=0) backpressure=0 peakMux=0KB peakPkts=0\n",
        )
        (crash_session / "crash.log").write_text("[03:34:44] CRASH DETECTED - Handling exception\n", encoding="utf-8")
        report = classify_session_triage(crash_session)
        assert "ce_process_crash" in report["verdicts"]
        assert report["faults"]["ce_process_crash"]

        audio_fault = make_session("audio_fault", media="[PullAudio] WARNING: Source underrun track=1\n")
        report = classify_session_triage(audio_fault)
        assert "ce_audio_timeline_fault" in report["verdicts"]

        stop_tail_padding = make_session(
            "stop_tail_padding",
            media=(
                "[PullAudio] WARNING: Source underrun - src 2 padding 800 samples with silence "
                "(available=0 needed=800 forceDrain=1)\n"
            ),
        )
        report = classify_session_triage(stop_tail_padding)
        assert "ce_audio_timeline_fault" not in report["verdicts"]
        assert report["evidence"]["log_counts"]["audio_stop_tail_padding"] == 1

        zero_drift = make_session(
            "zero_drift",
            media=(
                "[A/V ZERO DRIFT WARNING] Track 1 residual_samples=+1 residual_us=+21 "
                "target_samples=96000 cursor_samples=96001 target_us=2000000 cursor_us=2000021\n"
            ),
        )
        report = classify_session_triage(zero_drift)
        assert "ce_audio_timeline_fault" in report["verdicts"]
        assert report["evidence"]["zero_drift_warnings"][0]["residual_samples"] == 1

        one_us = make_session(
            "one_us",
            media="[VideoEncoder] WARNING: Post-mux audio duration mismatch (target=48266667 audioMinEnd=48266666 audioMaxEnd=48266666 maxDelta=1)\n",
        )
        report = classify_session_triage(one_us)
        assert "ce_audio_timeline_fault" not in report["verdicts"]
        assert report["evidence"]["rounding_evidence"]["post_mux_one_us_or_less_is_info"]

        multi_recording = root / "multi_recording"
        multi_recording.mkdir()
        (multi_recording / "captureengine.log").write_text(
            "[Controller] Starting recording...\n[Controller] Starting recording...\n", encoding="utf-8"
        )
        for recording_id, pid in (("r0001", 101), ("r0002", 202)):
            media_name = f"media_{recording_id}_{pid}.log"
            media_text = "[Media] Starting recording...\n"
            if recording_id == "r0001":
                media_text += (
                    "[2026-07-18 12:00:00.000] [INFO] [AVSyncApply] liveStartQpc=1000000\n"
                    "[2026-07-18 12:00:02.000] [INFO] [Media] Stopping recording...\n"
                )
            (multi_recording / media_name).write_text(media_text, encoding="utf-8")
            (multi_recording / f"recording_{recording_id}_{pid}.manifest").write_text(
                f"recording_id={recording_id}\nmedia_pid={pid}\nmedia_log={media_name}\n", encoding="utf-8"
            )
        (multi_recording / "perf_metrics_42.csv").write_text(
            "qpc_us,qpc_delta_us,total_us,capture_us,present_call_us,mux_queue_kb,overload_flags\n"
            "1100000,1000,0,0,0,0,0\n"
            "1500000,400000,0,0,0,0,0\n"
            "5000000,3500000,0,0,0,0,0\n",
            encoding="utf-8",
        )
        try:
            classify_session_triage(multi_recording)
            raise AssertionError("ambiguous multi-recording session was silently accepted")
        except ValueError as exc:
            assert "multiple recordings" in str(exc)
        report = classify_session_triage(multi_recording, recording_id="r0001")
        assert report["recording_id"] == "r0001"
        assert report["media_pid"] == 101
        assert not report["evidence"]["recording_evidence_incomplete"]
        assert report["recording_window"]["reason"] == "derived_selected_recording_bounds"
        assert report["evidence"]["max_present_gap_ms"] == 400.0

        overwritten_legacy = root / "overwritten_legacy"
        overwritten_legacy.mkdir()
        (overwritten_legacy / "captureengine.log").write_text(
            "[Controller] Starting recording...\n[Controller] Starting recording...\n", encoding="utf-8"
        )
        (overwritten_legacy / "media.log").write_text("[Media] Starting recording...\n", encoding="utf-8")
        report = classify_session_triage(overwritten_legacy)
        assert "recording_evidence_missing_or_overwritten" in report["contexts"]
        assert report["evidence"]["controller_recording_start_count"] == 2
        assert report["evidence"]["discovered_recording_evidence_count"] == 1

    print("self-test: PASS")


def main():
    parser = argparse.ArgumentParser(
        description="Analyze a capture file for CFR timing, duplicate-frame runs, and exact audio track alignment."
    )
    parser.add_argument("capture", nargs="?", type=Path, help="Capture file to analyze")
    parser.add_argument("--capture", dest="capture_option", type=Path, help="Capture file to attach to session triage")
    parser.add_argument("--session-dir", type=Path, help="Analyze a CE logs session for stutter attribution")
    parser.add_argument("--recording-id", help="Select one immutable recording within a multi-recording session")
    parser.add_argument("--media-log", type=Path, help="Select an exact media log within a session")
    parser.add_argument(
        "--all-recordings",
        action="store_true",
        help="Analyze every preserved recording in the session (cannot attach one capture file)",
    )
    parser.add_argument(
        "--recording-window",
        help="Restrict session perf/present-gap triage to live recording seconds START:END, for example 25:45",
    )
    parser.add_argument("--json-out", type=Path, help="Write JSON report")
    parser.add_argument("--log", type=Path, help="Optional media.log to summarize alongside the capture")
    parser.add_argument("--ffprobe", type=Path, default=Path("ffprobe"), help="Path to ffprobe executable")
    parser.add_argument("--ffmpeg", type=Path, default=Path("ffmpeg"), help="Path to ffmpeg executable")
    parser.add_argument(
        "--framehash",
        action="store_true",
        help="Run an additional full-length ffmpeg framemd5 pass to quantify repeated visual frames",
    )
    parser.add_argument(
        "--full-scan",
        dest="full_scan",
        action="store_true",
        help="Use authoritative frame/packet scans (default)",
    )
    parser.add_argument(
        "--metadata-only",
        dest="full_scan",
        action="store_false",
        help="Use faster informational stream metadata instead of exact decoded audio sample totals",
    )
    parser.set_defaults(full_scan=True)
    parser.add_argument(
        "--decode-check",
        action="store_true",
        help="Decode every audio stream with ffmpeg -v error and fail on stderr or nonzero exit",
    )
    parser.add_argument(
        "--waveform-tail-scan",
        action="store_true",
        help="Decode every audio stream to float PCM and report the last sample above --tail-threshold",
    )
    parser.add_argument(
        "--tail-threshold",
        type=float,
        default=1e-4,
        help="Absolute sample threshold for --waveform-tail-scan marker detection (default: 1e-4)",
    )
    parser.add_argument(
        "--framehash-width",
        type=int,
        default=320,
        help="Downscale width for duplicate-run frame hashing when --framehash is enabled (default: 320)",
    )
    parser.add_argument(
        "--window-seconds",
        type=float,
        default=10.0,
        help="Analyze first/middle/last windows of this many seconds using ffprobe frame scans (default: 10)",
    )
    parser.add_argument(
        "--max-audio-spread-ms",
        type=float,
        help="Fail if decoded audio track durations differ by more than this many milliseconds",
    )
    parser.add_argument(
        "--max-video-audio-delta-ms",
        type=float,
        help="Fail if any decoded audio track differs from decoded video duration by more than this many milliseconds",
    )
    parser.add_argument(
        "--max-audio-tail-marker-spread-ms",
        type=float,
        help="Fail if last non-silent audio markers differ by more than this many milliseconds",
    )
    parser.add_argument(
        "--max-mean-frame-delta-error-us",
        type=float,
        help="Fail if mean video frame spacing differs from nominal CFR spacing by more than this many microseconds",
    )
    parser.add_argument(
        "--max-longest-duplicate-run",
        type=int,
        help="Fail if the longest visual duplicate run exceeds this many frames (requires --framehash)",
    )
    parser.add_argument(
        "--max-repeated-frames",
        type=int,
        help="Fail if the total repeated visual frames exceed this count (requires --framehash)",
    )
    parser.add_argument(
        "--max-log-event",
        action="append",
        default=[],
        metavar="NAME=COUNT",
        help="Fail if the named log event count exceeds COUNT. Valid names match LOG_PATTERNS.",
    )
    parser.add_argument(
        "--strict-sync-events",
        action="store_true",
        help="Fail on any known audio trim/drop/underrun event or stale CFR catch-up event in the log.",
    )
    parser.add_argument(
        "--max-cadence-metric",
        action="append",
        default=[],
        metavar="NAME=COUNT",
        help=(
            "Fail if the named cadence summary metric exceeds COUNT. Valid names: age_max_us, sel_miss, "
            "stale_unique, ancient, rep_no_fresh, wgc_sel_bias_abs_us, wgc_shortfall_ms, "
            "wgc_lead_excess_ms, wgc_oldest_ms, wgc_buffered_frames, wgc_live_rebase_max_ticks, "
            "wgc_startup_frame_age_us."
        ),
    )
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        self_test()
        return

    effective_capture = args.capture_option or args.capture

    if args.session_dir:
        if not args.session_dir.exists():
            fail(f"session dir not found: {args.session_dir}")
        if effective_capture and not effective_capture.exists():
            fail(f"capture file not found: {effective_capture}")
        if args.all_recordings and (args.recording_id or args.media_log):
            fail("--all-recordings cannot be combined with --recording-id or --media-log")
        if args.all_recordings and effective_capture:
            fail("--all-recordings cannot attach one --capture to multiple recordings")
        try:
            if args.all_recordings:
                recordings = discover_recording_evidence(args.session_dir)
                if not recordings:
                    fail(f"no media recording evidence found in session: {args.session_dir}")
                reports = [
                    classify_session_triage(
                        args.session_dir,
                        recording_window=args.recording_window,
                        media_log_path=item["media_log"],
                    )
                    for item in recordings
                ]
            else:
                reports = [
                    classify_session_triage(
                        args.session_dir,
                        effective_capture,
                        args.recording_window,
                        recording_id=args.recording_id,
                        media_log_path=args.media_log,
                    )
                ]
        except ValueError as exc:
            fail(str(exc))
        if effective_capture:
            attach_completed_capture_report(
                reports[0],
                analyze_completed_capture_exact(
                    args.ffprobe, args.ffmpeg, effective_capture, args.tail_threshold
                ),
            )
        for index, report in enumerate(reports):
            if index:
                print()
            print_triage_report(report)
        if args.json_out:
            args.json_out.parent.mkdir(parents=True, exist_ok=True)
            json_report = (
                {"schema": "ce-session-av-triage-set-v1", "session_dir": str(args.session_dir), "reports": reports}
                if args.all_recordings
                else reports[0]
            )
            args.json_out.write_text(json.dumps(json_report, indent=2), encoding="utf-8")
        return

    if args.capture_option:
        fail("--capture is only supported with --session-dir; pass the capture as a positional argument otherwise")
    if not effective_capture:
        fail("capture is required unless --session-dir or --self-test is used")
    if not effective_capture.exists():
        fail(f"capture file not found: {effective_capture}")
    if args.log and not args.log.exists():
        fail(f"log file not found: {args.log}")

    args.capture = effective_capture
    format_info, video_streams, audio_streams = analyze_streams(args.ffprobe, args.capture)
    video_stream = video_streams[0]
    format_duration = parse_float(format_info.get("duration"))
    nominal_fps_text = video_stream.get("avg_frame_rate") or video_stream.get("r_frame_rate")
    nominal_fps_fraction = parse_ratio_fraction(nominal_fps_text)
    nominal_fps = float(nominal_fps_fraction) if nominal_fps_fraction > 0 else 0.0
    video_timing = (
        analyze_video_timing(args.ffprobe, args.capture, nominal_fps=nominal_fps)
        if args.full_scan
        else analyze_video_stream_metadata(video_stream, format_duration)
    )
    packet_coverage = analyze_cfr_packet_coverage(args.ffprobe, args.capture, nominal_fps)
    duplicate_runs = None
    if args.framehash:
        duplicate_runs = analyze_video_duplicate_runs(args.ffmpeg, args.capture, args.framehash_width)
    audio_tracks = []
    for audio_ordinal, stream_info in enumerate(audio_streams):
        if args.full_scan:
            audio_tracks.append(analyze_audio_stream(args.ffprobe, args.capture, audio_ordinal, stream_info))
        else:
            audio_tracks.append(analyze_audio_stream_metadata(audio_ordinal, stream_info, format_duration))
    decode_results = []
    if args.decode_check:
        decode_results = [
            analyze_audio_decode(args.ffmpeg, args.capture, audio_ordinal)
            for audio_ordinal, _stream_info in enumerate(audio_streams)
        ]
    tail_results = []
    if args.full_scan or args.waveform_tail_scan or args.max_audio_tail_marker_spread_ms is not None:
        tail_results = [
            analyze_audio_tail_marker(args.ffmpeg, args.capture, audio_ordinal, stream_info, args.tail_threshold)
            for audio_ordinal, stream_info in enumerate(audio_streams)
        ]
    if args.full_scan:
        for track, decoded in zip(audio_tracks, tail_results):
            track["source"] = "decoded-pcm-f32"
            track["sample_total"] = decoded["samples"]
            track["decoded_duration"] = (
                decoded["samples"] / decoded["sample_rate"] if decoded["sample_rate"] > 0 else 0.0
            )
            track["frame_start"] = 0.0
            track["frame_end"] = track["decoded_duration"]
    inter_track_correlations = analyze_inter_track_correlations(tail_results)
    log_summary = analyze_log(args.log)

    video_duration = video_timing["duration"]
    cfr_target_duration = (
        Fraction(video_timing["frame_count"], 1) / nominal_fps_fraction
        if nominal_fps_fraction > 0
        else Fraction(0, 1)
    )
    audio_lengths = [track["decoded_duration"] for track in audio_tracks]
    audio_duration_spread = (max(audio_lengths) - min(audio_lengths)) if audio_lengths else 0.0
    video_audio_max_delta = (
        max(abs(track["decoded_duration"] - video_duration) for track in audio_tracks) if audio_tracks else 0.0
    )
    tail_marker_times = [
        result["last_marker_time"] for result in tail_results if result.get("last_marker_time") is not None
    ]
    audio_tail_marker_spread = (
        max(tail_marker_times) - min(tail_marker_times) if len(tail_marker_times) >= 2 else 0.0
    )
    window_duration = max(0.0, min(args.window_seconds, format_duration if format_duration > 0.0 else args.window_seconds))
    windows = {}
    if window_duration > 0.0 and format_duration > 0.0:
        middle_start = max(0.0, (format_duration / 2.0) - (window_duration / 2.0))
        last_start = max(0.0, format_duration - window_duration)
        window_specs = {
            "first": 0.0,
            "middle": middle_start,
            "last": last_start,
        }
        for name, start_time in window_specs.items():
            windows[name] = analyze_window(
                args.ffprobe,
                args.ffmpeg,
                args.capture,
                audio_streams,
                start_time,
                window_duration,
                args.framehash,
                args.framehash_width,
                nominal_fps,
            )

    print(f"capture: {args.capture}")
    print(f"container_duration: {format_seconds(format_duration)}")
    print()

    print("video:")
    print(f"  timing_source={video_timing['source']}")
    print(
        "  codec={codec} resolution={width}x{height} nominal_fps={fps:.6f}".format(
            codec=video_stream.get("codec_name", ""),
            width=parse_int(video_stream.get("width")),
            height=parse_int(video_stream.get("height")),
            fps=nominal_fps,
        )
    )
    print(
        "  frames={frames} first_pts={first_pts:.6f} last_pts={last_pts:.6f} duration={duration:.6f}".format(
            frames=video_timing["frame_count"],
            first_pts=video_timing["first_pts"],
            last_pts=video_timing["last_pts"],
            duration=video_duration,
        )
    )
    print(
        "  cfr_packet_coverage actual={actual} expected={expected} missing={missing} "
        "max_gap_ticks={max_gap:.3f} complete={complete}".format(
            actual=packet_coverage["packet_count"],
            expected=packet_coverage["expected_packets"],
            missing=packet_coverage["missing_packets"],
            max_gap=packet_coverage["max_gap_ticks"],
            complete="yes" if packet_coverage["complete"] else "no",
        )
    )
    print(
        "  delta_mean={mean:.6f} delta_min={delta_min:.6f} delta_max={delta_max:.6f} delta_stdev={stdev:.6f}".format(
            mean=video_timing["delta_mean"],
            delta_min=video_timing["delta_min"],
            delta_max=video_timing["delta_max"],
            stdev=video_timing["delta_stdev"],
        )
    )
    if args.full_scan:
        print_top_histogram("  frame_delta_histogram", video_timing["delta_histogram"])
    else:
        print("  frame_delta_histogram: skipped (pass --full-scan for per-frame timing)")
    print(
        "  duplicate_runs={status}".format(
            status=(
                "skipped (pass --framehash for full visual duplicate scan)"
                if duplicate_runs is None
                else "enabled"
            )
        )
    )
    if duplicate_runs is not None:
        print(
            (
                "  duplicate_runs framehash_frames={framehash_count} repeated_runs={repeated_runs} "
                "repeated_frames={repeated_frames} longest_run={longest}"
            ).format(
                framehash_count=duplicate_runs["framehash_count"],
                repeated_runs=duplicate_runs["repeated_run_count"],
                repeated_frames=duplicate_runs["repeated_frame_count"],
                longest=duplicate_runs["longest_run"],
            ),
        )
        print_top_histogram("  duplicate_run_histogram", duplicate_runs["repeated_histogram"])
    print()

    print("audio:")
    if not audio_tracks:
        print("  no audio streams")
    for track in audio_tracks:
        print(
            (
                "  a:{ordinal} stream={stream_index} codec={codec} rate={rate}Hz ch={channels} "
                "samples={samples} duration={duration:.6f} start={start:.6f} "
                "end={end:.6f} source={source}"
            ).format(
                ordinal=track["audio_ordinal"],
                stream_index=track["stream_index"],
                codec=track["codec"],
                rate=track["sample_rate"],
                channels=track["channels"],
                samples=track["sample_total"],
                duration=track["decoded_duration"],
                start=track["frame_start"],
                end=track["frame_end"],
                source=track["source"],
            ),
        )
    if audio_tracks:
        print(f"  audio_duration_spread={audio_duration_spread:.6f}")
        print(f"  max_video_audio_duration_delta={video_audio_max_delta:.6f}")
    print()

    if decode_results:
        print("audio_decode:")
        for result in decode_results:
            stderr = result["stderr"].replace("\n", " | ")
            print(
                "  a:{ordinal} returncode={returncode} stderr={stderr}".format(
                    ordinal=result["audio_ordinal"],
                    returncode=result["returncode"],
                    stderr=stderr if stderr else "(empty)",
                )
            )
        print()

    if tail_results:
        print("audio_decoded_pcm:")
        for result in tail_results:
            first_marker = result["first_marker_sample"]
            marker = result["last_marker_sample"]
            first_marker_text = "none" if first_marker is None else str(first_marker)
            marker_text = "none" if marker is None else str(marker)
            time_text = "none" if result["last_marker_time"] is None else f"{result['last_marker_time']:.6f}"
            silence_text = "none" if result["tail_silence_ms"] is None else f"{result['tail_silence_ms']:.3f}ms"
            print(
                "  a:{ordinal} samples={samples} first_marker_sample={first_marker} last_marker_sample={marker} "
                "last_marker_time={time} tail_silence={silence} peak={peak:.7f} clipping={clipping} "
                "silent={silent} longest_silence={longest} discontinuities={discontinuities} "
                "identical_channel_frames={identical} decoder_rc={returncode} threshold={threshold:g}".format(
                    ordinal=result["audio_ordinal"],
                    samples=result["samples"],
                    first_marker=first_marker_text,
                    marker=marker_text,
                    time=time_text,
                    silence=silence_text,
                    peak=result["peak"],
                    clipping=result["clipping_samples"],
                    silent=result["silent_samples"],
                    longest=result["longest_silence_samples"],
                    discontinuities=result["discontinuities"],
                    identical=result["identical_channel_frames"],
                    returncode=result["returncode"],
                    threshold=args.tail_threshold,
                )
            )
            if result["stderr"]:
                print(f"    stderr={result['stderr'].replace(chr(10), ' | ')}")
        print(f"  audio_tail_marker_spread={audio_tail_marker_spread:.6f}")
        for correlation in inter_track_correlations:
            print(
                "  correlation a:{left}<->a:{right} coefficient={coefficient:.6f} offset={offset:+.3f}ms".format(
                    left=correlation["left"],
                    right=correlation["right"],
                    coefficient=correlation["correlation"],
                    offset=correlation["offset_ms"],
                )
            )
        print()

    if log_summary:
        print("log_summary:")
        for name, count in sorted(log_summary["counts"].items()):
            print(f"  {name}={count}")
        print(
            (
                "  cadence_windows={windows} max_age_max_us={age_max} max_sel_miss={sel_miss} "
                "max_stale_unique={stale_unique} max_ancient={ancient} "
                "max_rep_no_fresh={rep_no_fresh} max_wgc_sel_bias_abs_us={wgc_bias} "
                "max_wgc_shortfall_ms={shortfall} max_wgc_lead_excess_ms={lead_excess} "
                "max_wgc_oldest_ms={oldest} max_wgc_buffered_frames={buffered} "
                "max_wgc_live_rebase_ticks={live_rebase} max_wgc_startup_frame_age_us={startup_age} "
                "max_wgc_encoder_limited_drops={encoder_drops} max_wgc_phase_error_us={phase_error}"
            ).format(
                windows=log_summary["cadence_windows"],
                age_max=log_summary["max_age_max_us"],
                sel_miss=log_summary["max_sel_miss"],
                stale_unique=log_summary["max_stale_unique"],
                ancient=log_summary["max_ancient"],
                rep_no_fresh=log_summary["max_rep_no_fresh"],
                wgc_bias=log_summary["max_wgc_sel_bias_abs_us"],
                shortfall=log_summary["max_wgc_shortfall_ms"],
                lead_excess=log_summary["max_wgc_lead_excess_ms"],
                oldest=log_summary["max_wgc_oldest_ms"],
                buffered=log_summary["max_wgc_buffered_frames"],
                live_rebase=log_summary["max_wgc_live_rebase_ticks"],
                startup_age=log_summary["max_wgc_startup_frame_age_us"],
                encoder_drops=log_summary["max_wgc_encoder_limited_drops"],
                phase_error=log_summary["max_wgc_phase_error_us"],
            ),
        )
        print(
            "  saw_encoder_overload={enc} saw_mux_overload={mux}".format(
                enc=int(log_summary["saw_encoder_overload"]),
                mux=int(log_summary["saw_mux_overload"]),
            )
        )
        print()

    if windows:
        print("windows:")
        for name in ("first", "middle", "last"):
            print_window_summary(name, windows[name])
        print()

    checks, mean_frame_delta_error_us = evaluate_thresholds(
        args,
        nominal_fps,
        video_timing,
        duplicate_runs,
        audio_duration_spread,
        video_audio_max_delta,
        log_summary,
    )
    checks.append(
        {
            "name": "cfr_packet_coverage",
            "passed": packet_coverage["complete"],
            "actual": "{actual}/{expected} packets max_gap={gap:.3f} ticks".format(
                actual=packet_coverage["packet_count"],
                expected=packet_coverage["expected_packets"],
                gap=packet_coverage["max_gap_ticks"],
            ),
            "expected": "all CFR ticks represented; max gap <= 1.01 ticks",
        }
    )
    if args.full_scan:
        decoded_endpoints = []
        for result in tail_results:
            exact_target = cfr_target_duration * result["sample_rate"]
            lattice_representable = exact_target.denominator == 1
            expected_samples = exact_target.numerator if lattice_representable else round_fraction(exact_target)
            decoded_endpoints.append((result["samples"], expected_samples, result["sample_rate"]))
            checks.append(
                {
                    "name": f"audio_decoded_exact.a:{result['audio_ordinal']}",
                    "passed": result["returncode"] == 0
                    and result["stderr"] == ""
                    and lattice_representable
                    and result["samples"] == expected_samples,
                    "actual": (
                        f"samples={result['samples']} expected={expected_samples} rate={result['sample_rate']} "
                        f"lattice={int(lattice_representable)} returncode={result['returncode']} "
                        f"stderr={'empty' if result['stderr'] == '' else 'nonempty'}"
                    ),
                    "expected": "completed-file decoded PCM equals the CFR-derived sample target exactly",
                }
            )
        comparable_counts = {samples for samples, _expected, rate in decoded_endpoints if rate == 48000}
        if comparable_counts:
            checks.append(
                {
                    "name": "audio_48000_track_endpoints_identical",
                    "passed": len(comparable_counts) == 1,
                    "actual": sorted(comparable_counts),
                    "expected": "identical decoded sample counts for all 48 kHz tracks",
                }
            )
        endpoint_durations = {
            Fraction(samples, rate) for samples, _expected, rate in decoded_endpoints if rate > 0
        }
        checks.append(
            {
                "name": "audio_track_endpoint_durations_identical",
                "passed": len(endpoint_durations) <= 1,
                "actual": [f"{value.numerator}/{value.denominator}" for value in sorted(endpoint_durations)],
                "expected": "all decoded tracks end at the same exact rational duration",
            }
        )
    if args.decode_check:
        for result in decode_results:
            checks.append(
                {
                    "name": f"audio_decode.a:{result['audio_ordinal']}",
                    "passed": result["returncode"] == 0 and result["stderr"] == "",
                    "actual": f"returncode={result['returncode']} stderr={'empty' if result['stderr'] == '' else 'nonempty'}",
                    "expected": "returncode=0 stderr=empty",
                }
            )
    if args.max_audio_tail_marker_spread_ms is not None:
        marker_missing_mismatch = bool(tail_results) and len(tail_marker_times) not in (0, len(tail_results))
        checks.append(
            {
                "name": "audio_tail_marker_presence",
                "passed": not marker_missing_mismatch,
                "actual": f"{len(tail_marker_times)}/{len(tail_results)} tracks have marker",
                "expected": "all or none",
            }
        )
        checks.append(
            make_upper_bound_check(
                "audio_tail_marker_spread",
                audio_tail_marker_spread * 1000.0,
                args.max_audio_tail_marker_spread_ms,
                "ms",
                tolerance=0.0005,
            )
        )

    print("summary:")
    if mean_frame_delta_error_us is None and video_timing["frame_count"] > 1 and nominal_fps > 0.0:
        expected_delta = 1.0 / nominal_fps
        mean_frame_delta_error_us = abs(video_timing["delta_mean"] - expected_delta) * 1_000_000.0
    if mean_frame_delta_error_us is not None:
        print(f"  mean_frame_delta_error_us={mean_frame_delta_error_us:.3f}")
    print(f"  exact_audio_length_match={'yes' if math.isclose(audio_duration_spread, 0.0, abs_tol=1e-6) else 'no'}")
    print(f"  cfr_packet_coverage={'yes' if packet_coverage['complete'] else 'no'}")
    if tail_results:
        print(f"  audio_tail_marker_spread_ms={audio_tail_marker_spread * 1000.0:.3f}")
    print(
        "  all_audio_tracks_match_video_length={value}".format(
            value="yes" if math.isclose(video_audio_max_delta, 0.0, abs_tol=1e-3) else "no"
        )
    )
    print()
    print_checks(checks)

    if args.json_out:
        standalone_report = {
            "schema": "ce-completed-capture-av-v2",
            "capture": str(args.capture),
            "video": {
                "codec": video_stream.get("codec_name", ""),
                "fps": nominal_fps_text,
                "frame_count": video_timing["frame_count"],
                "duration": video_duration,
                "packet_coverage": packet_coverage,
            },
            "audio_tracks": audio_tracks,
            "decoded_pcm": [
                {key: value for key, value in result.items() if key != "signature"}
                for result in tail_results
            ],
            "correlations": inter_track_correlations,
            "checks": checks,
            "passed": all(check["passed"] for check in checks),
        }
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(json.dumps(standalone_report, indent=2), encoding="utf-8")

    if any(not check["passed"] for check in checks):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
