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
    # Real captured audio destroyed because the exported cursor ran past the live capture edge.
    "audio_ingest_starvation": re.compile(
        r"\[AudioLoop\] WARNING: source ingest starvation", re.IGNORECASE
    ),
    "audio_ingest_starvation_resync": re.compile(
        r"\[AudioLoop\] WARNING: unrecoverable ingest starvation", re.IGNORECASE
    ),
    "audio_ingest_reservoir_deepened": re.compile(
        r"\[PullAudio\] Ingest reservoir deepened", re.IGNORECASE
    ),
    "audio_late_live_source_hold": re.compile(r"\[PullAudio\] Late live source hold", re.IGNORECASE),
    "audio_epoch_bootstrap_recovery": re.compile(
        r"\[AudioEpoch\] WARNING: restored recording-sticky source bootstrap eligibility", re.IGNORECASE
    ),
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
    "audio_stop_force_drain_backlog": re.compile(
        r"\[PullAudio\] Stop force-drain backlog:", re.IGNORECASE
    ),
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
    "audio_ingest_starvation",
    "audio_ingest_starvation_resync",
    "audio_epoch_bootstrap_recovery",
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
RECORDING_HEALTH_RE = re.compile(r"\[RECORDING HEALTH\]\s*(.*)", re.IGNORECASE)
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
    r"HoldWithCandidate=(\d+) BufferCapTrim=(\d+) TargetResidualMax=(\d+)us"
    r"(?: PhaseReservePeak=(\d+) PhaseShiftMax=(-?\d+)us PreserveFrontTrim=(\d+) "
    r"DisplayPathTransitions=(\d+))?",
    re.IGNORECASE,
)
INJECT_CFR_REPEAT_PRESSURE_RE = re.compile(r"\[Inject CFR\] Repeat pressure:\s*(.*)", re.IGNORECASE)
CFR_PHASE_LOCK_SUMMARY_RE = re.compile(
    r"\[CFR PHASE LOCK SUMMARY\] Backend=(\w+) Enabled=(\d+) Locked=(\d+) Offset=(-?\d+)us "
    r"Stable=(\d+) Unstable=(\d+) Acquire=(\d+) Rephase=(\d+) Release=(\d+) Multiplier=(\d+)",
    re.IGNORECASE,
)
CFR_PHASE_LOCK_LINE_RE = re.compile(r"\[CFR PHASE\s*LOCK(?: SUMMARY)?\]", re.IGNORECASE)
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
# Consumer-overrun evidence. `starve` counts real captured samples destroyed because the
# exported cursor ran past the live capture edge; the recording still reports exact track
# lengths and perfect packet timing, so this is the only direct proof of the failure.
STOP_AUDIO_INGEST_RE = re.compile(
    r"\[STOP AUDIO INGEST\] Source (\d+): track=(\d+) starve=(\d+) resync=(\d+)/(\d+) "
    r"reservoirPeakMs=(-?\d+)(?: process=([^\s]+))?",
    re.IGNORECASE,
)
# Legacy fallback for logs written before the destroyed-sample counter existed. A source that
# trimmed a large fraction of its timeline as packet overlap was being starved by the consumer;
# ordinary boundary de-duplication is a few hundred samples per recording.
STOP_AUDIO_DETAIL_OVERLAP_RE = re.compile(
    r"\[STOP AUDIO DETAIL\] Source (\d+):.*?overlap=(\d+)",
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
AUDIO_CORRELATION_FOCUS_CURSOR_RE = re.compile(
    r"\[(?:AudioLoop|AudioEpoch)\] (?:Late app source live join|"
    r"Capture owner accepted acknowledged transition).*?\btrackCursor=(\d+)",
    re.IGNORECASE,
)
LATE_APP_PRIMED_SRC_RE = re.compile(
    r"\[PullAudio\] Source primed\s+-\s+src=(\d+).*?lateStart=(\d+)ms(?:\s+app=([01]))?",
    re.IGNORECASE,
)
APP_DRAIN_STATE_RE = re.compile(
    r"\[AppDrain\] state src=(\d+).*?\bforceDrain=([01])", re.IGNORECASE
)
AUDIO_EXTREME_DRIFT_SRC_RE = re.compile(
    r"\[PullAudio\] WARNING: Extreme drift detected \([^)]*?\bsrc=(\d+)\)"
    r"(?:.*?\bforceDrain=([01]))?",
    re.IGNORECASE,
)
SCREEN_CAPTURE_BACKEND_TOKEN_RE = re.compile(
    r"(?:\bBackend:\s*|\bbackend=)(DxgiDuplication|dxgi_duplication|dxgi_dup|WGC)\b",
    re.IGNORECASE,
)

WGC_ACTIVE_DELAY_POLICY_HOLD_FAULT_MIN_COUNT = 120
WGC_ACTIVE_DELAY_POLICY_HOLD_FAULT_PERMILLE = 250
WGC_CFR_SMOOTHNESS_EXCESS_REPEAT_FAULT_MIN_COUNT = 120
WGC_CFR_SMOOTHNESS_POLICY_REPEAT_NOTICE_MIN_COUNT = 24
WGC_CFR_SMOOTHNESS_POLICY_REPEAT_NOTICE_PERMILLE = 5
WGC_CFR_SMOOTHNESS_EXCESS_REPEAT_CLUSTER_FAULT_TICKS = 24
WGC_AUDIO_LATE_RISK_SOFT_ACCEPT_MIN_COUNT = 10
WGC_AUDIO_LATE_RISK_WINDOW_US = 10 * 1000 * 1000
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
    "audio_ingest_starvation",
    "audio_ingest_starvation_resync",
    "audio_epoch_bootstrap_recovery",
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
