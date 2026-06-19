#!/usr/bin/env python3

import argparse
import collections
import csv
import json
import math
import os
import re
import statistics
import struct
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import NoReturn


LOG_PATTERNS = {
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
PRESENT_HEARTBEAT_GAP_RE = re.compile(r"DetourPresent: heartbeat #\d+ gap=([0-9.]+)ms", re.IGNORECASE)
WGC_SOURCE_STARVED_RE = re.compile(
    r"\[WGC CFR\] Source-starved episode: duration=(\d+)ms out=(\d+) dup=(\d+) minIn=(\d+) minDel=(\d+) "
    r"freshMiss=(\d+)pm minBuf=(\d+)",
    re.IGNORECASE,
)
WGC_ATTRIBUTION_RE = re.compile(r"\[WGC CFR ATTRIBUTION\]\s*(.*)", re.IGNORECASE)
WGC_CADENCE_EVENT_RE = re.compile(r"\[WGC CFR CADENCE EVENT\]\s*mode=([A-Za-z_]+)\s*(.*)", re.IGNORECASE)
WGC_SUMMARY_RE = re.compile(
    r"\[WGC CFR SUMMARY\].*Live=(\d+) Dup=(\d+).*DupReason\(src=(\d+) def=(\d+) timer=(\d+) drain=(\d+)\).*"
    r"SourceLimitedRepeats=(\d+) StarvedEpisodes=(\d+) longest=(\d+)ms",
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
WGC_DELAY_REALIZATION_RE = re.compile(
    r"delayReservoirLowWaterFrames=(\d+) delayReservoirTargetFrames=(\d+) "
    r"delayReservoirLowWaterTicks=(\d+) realizedDelayAvg=(\d+)us realizedDelayMin=(\d+)us "
    r"realizedDelayMax=(\d+)us delayResidualAvg=([+-]?\d+)/(\d+)us "
    r"delayResidualMax=(\d+)us delayResidualP95=(\d+)us delayResidualLateMax=(\d+)us "
    r"delayResidualEarlyMax=(\d+)us",
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
    r"delaySourceRecoveryHolds=(\d+) delaySourceRecoveryTicks=(\d+))?)?",
    re.IGNORECASE,
)
WGC_DELAY_POST_REJECT_RE = re.compile(r"delayPostSelectionRejectedSync=(\d+)", re.IGNORECASE)
WGC_PERF_RE = re.compile(r"\[WGC Perf\].*", re.IGNORECASE)
INJECT_PERF_RE = re.compile(r"\[Inject Perf\].*", re.IGNORECASE)
INJECT_CFR_SUMMARY_RE = re.compile(
    r"\[Inject CFR SUMMARY\].*Live=(\d+) Dup=(\d+) DupPct=([0-9.]+)% "
    r"DupReason\(src=(\d+) def=(\d+) timer=(\d+) drain=(\d+)\).*"
    r"FreshCatchup=(\d+) RepeatCatchup=(\d+) StaleTrim=(\d+)",
    re.IGNORECASE,
)
INJECT_CFR_SOURCE_RE = re.compile(
    r"\[Inject CFR SUMMARY\] SourceFps=([0-9.]+)\.\.([0-9.]+) JitterMax=(\d+)us SelMax=(\d+)us",
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
PACKET_MISMATCH_RE = re.compile(r"Packet-level A/V duration mismatch", re.IGNORECASE)
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

TRIAGE_AUDIO_FAULT_EVENTS = {
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
    "wgc_fresh_catchup",
    "wgc_stop_drain_aborted",
    "wgc_stop_hold_repeats",
    "wgc_drain_duplicate_summary",
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
    return {
        "audio_ordinal": audio_ordinal,
        "returncode": result.returncode,
        "stderr": result.stderr.strip(),
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
    last_marker_sample = None
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
        values = struct.unpack("<" + "f" * (usable // 4), payload)
        frames = usable // frame_bytes
        for frame in range(frames):
            base = frame * channels
            peak = max(abs(values[base + channel]) for channel in range(channels))
            if peak > threshold:
                last_marker_sample = total_samples + frame
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
    return {
        "audio_ordinal": audio_ordinal,
        "sample_rate": sample_rate,
        "channels": channels,
        "samples": total_samples,
        "last_marker_sample": last_marker_sample,
        "last_marker_time": last_marker_time,
        "tail_silence_ms": tail_silence_ms,
        "stderr": stderr.decode("utf-8", errors="replace").strip(),
        "returncode": returncode,
    }


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


def parse_session_manifest(session_dir):
    manifest = {}
    path = session_dir / "session_manifest.txt"
    for line in read_text_if_exists(path).splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        manifest[key.strip()] = value.strip()
    return manifest


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
        "input": find_int(r"Input:\s*(\d+)"),
        "queued": find_int(r"Queued:\s*(\d+)"),
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


def parse_media_triage(media_text):
    source_starved = []
    attribution = []
    wgc_perf = []
    wgc_summary = []
    wgc_cadence_events = []
    wgc_smoothness_summary = []
    inject_perf = []
    inject_summary = []
    inject_source_summary = []
    final_packet_timelines = []
    final_metadata = []
    post_mux_audio_mismatches = []
    stop_audio_tracks = []
    stop_audio_sources = []
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
        cadence_event_match = WGC_CADENCE_EVENT_RE.search(line)
        if cadence_event_match:
            event = parse_attribution_payload(cadence_event_match.group(2))
            event["mode"] = cadence_event_match.group(1)
            event["line"] = line
            wgc_cadence_events.append(event)
        if INJECT_PERF_RE.search(line):
            inject_perf.append(parse_inject_perf_line(line))
        summary_match = WGC_SUMMARY_RE.search(line)
        if summary_match:
            wgc_summary.append(
                {
                    "live": parse_int(summary_match.group(1)),
                    "duplicate": parse_int(summary_match.group(2)),
                    "dup_src": parse_int(summary_match.group(3)),
                    "dup_def": parse_int(summary_match.group(4)),
                    "dup_timer": parse_int(summary_match.group(5)),
                    "dup_drain": parse_int(summary_match.group(6)),
                    "source_limited_repeats": parse_int(summary_match.group(7)),
                    "starved_episodes": parse_int(summary_match.group(8)),
                    "longest_ms": parse_int(summary_match.group(9)),
                    "line": line,
                }
            )
        smoothness_match = WGC_SMOOTHNESS_SUMMARY_RE.search(line)
        if smoothness_match:
            smoothness_extra = WGC_SMOOTHNESS_EXTRA_RE.search(line)
            delay_realization = WGC_DELAY_REALIZATION_RE.search(line)
            delay_raw = WGC_DELAY_RAW_RESIDUAL_RE.search(line)
            delay_relaxed = WGC_DELAY_RELAXED_RE.search(line)
            delay_post_reject = WGC_DELAY_POST_REJECT_RE.search(line)
            wgc_smoothness_summary.append(
                {
                    "encoder_limited_drops": parse_int(smoothness_match.group(1)),
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
                    "delay_source_recovery_holds": parse_int(delay_relaxed.group(10)) if delay_relaxed else 0,
                    "delay_source_recovery_ticks": parse_int(delay_relaxed.group(11)) if delay_relaxed else 0,
                    "delay_post_selection_rejected_sync": parse_int(delay_post_reject.group(1))
                    if delay_post_reject
                    else 0,
                    "line": line,
                }
            )
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
        "wgc_cadence_events": wgc_cadence_events,
        "wgc_smoothness_summary": wgc_smoothness_summary,
        "inject_perf": inject_perf,
        "inject_summary": inject_summary,
        "inject_source_summary": inject_source_summary,
        "final_packet_timelines": final_packet_timelines,
        "final_metadata": final_metadata,
        "post_mux_audio_mismatch_delta_us": post_mux_audio_mismatches,
        "stop_audio_tracks": stop_audio_tracks,
        "stop_audio_sources": stop_audio_sources,
        "zero_drift_warnings": zero_drift_warnings,
        "packet_mismatch_warnings": packet_mismatch_warnings,
    }


def parse_hook_triage(session_dir):
    gaps = []
    external_overlay_lines = []
    present_stalled_lines = []
    crash_events = []
    for path in sorted(session_dir.glob("*.log")):
        if path.name.lower() == "media.log":
            continue
        text = read_text_if_exists(path)
        for line in text.splitlines():
            gap_match = PRESENT_HEARTBEAT_GAP_RE.search(line)
            if gap_match:
                gaps.append({"path": str(path), "gap_ms": parse_float(gap_match.group(1)), "line": line})
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


def parse_perf_csvs(session_dir):
    summaries = []
    for path in sorted(session_dir.glob("perf_metrics_*.csv")):
        try:
            with path.open("r", encoding="utf-8", errors="replace", newline="") as handle:
                rows = list(csv.DictReader(handle))
        except OSError:
            continue
        previous_qpc = None
        max_qpc_delta_us = 0
        large_gaps = []
        max_total_us = 0
        max_capture_us = 0
        max_present_call_us = 0
        max_mux_kb = 0
        overload_rows = 0
        for row in rows:
            qpc = parse_int(row.get("qpc_us"), 0)
            if "qpc_delta_us" in row and row.get("qpc_delta_us") not in (None, ""):
                delta = parse_int(row.get("qpc_delta_us"), 0)
            elif previous_qpc and qpc > previous_qpc:
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
                "rows": len(rows),
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
    return {
        "detail_episode_count": len(media_evidence["source_starved_episodes"]),
        "summary_starved_episodes": sum(row["starved_episodes"] for row in summary_rows),
        "summary_source_limited_repeats": sum(row["source_limited_repeats"] for row in summary_rows),
        "summary_longest_ms": max((row["longest_ms"] for row in summary_rows), default=0),
    }


def summarize_inject_pacing(media_evidence):
    perf_rows = media_evidence["inject_perf"]
    summary_rows = media_evidence["inject_summary"]
    source_rows = media_evidence["inject_source_summary"]
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
        "source_fps_min": min((row["source_fps_min"] for row in source_rows), default=0.0),
        "source_fps_max": max((row["source_fps_max"] for row in source_rows), default=0.0),
        "jitter_max_us": max((row["jitter_max_us"] for row in source_rows), default=0),
        "selection_max_us": max((row["selection_max_us"] for row in source_rows), default=0),
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


def has_encoder_or_mux_backpressure(media_evidence, perf_summaries):
    if any(item.get("encoder_overload") or item.get("mux_overload") or item.get("backpressure")
           for item in media_evidence["final_metadata"]):
        return True
    if any(item["overload_rows"] > 0 for item in perf_summaries):
        return True
    for item in media_evidence["wgc_perf"]:
        if item["overload_flags"] != 0:
            return True
    return False


def has_exact_final_mux_evidence(media_evidence):
    final_packets_clean = bool(media_evidence["final_packet_timelines"]) and all(
        item["max_packet_delta_us"] <= 1 and item["audio_past_target"] == 0
        for item in media_evidence["final_packet_timelines"]
    )
    final_metadata_clean = bool(media_evidence["final_metadata"]) and all(
        item["max_delta_us"] <= 1 for item in media_evidence["final_metadata"]
    )
    no_post_mux_strict_mismatch = all(delta <= 1 for delta in media_evidence["post_mux_audio_mismatch_delta_us"])
    return (final_packets_clean or final_metadata_clean) and no_post_mux_strict_mismatch


def parse_hex_flags(value):
    try:
        return int(str(value), 0)
    except (TypeError, ValueError):
        return 0


def parse_ms_ratio_value(value):
    text = str(value or "").strip().rstrip(",")
    if text.endswith("ms"):
        text = text[:-2]
    if "/" in text:
        text = text.split("/", 1)[1]
    return parse_float(text)


def has_wgc_encoder_overload_policy_fault(media_evidence, log_summary):
    if not log_summary:
        return False

    counts = log_summary["counts"]
    overload_seen = log_summary.get("saw_encoder_overload") or log_summary.get("saw_mux_overload")
    overload_seen = overload_seen or any(item["overload_flags"] != 0 for item in media_evidence["wgc_perf"])
    overload_seen = overload_seen or any(
        item.get("fault_hint") == "ce_capacity_pressure" for item in media_evidence["wgc_attribution"]
    )
    overload_seen = overload_seen or any(item.get("encoder_limited_drops", 0) > 0
                                         for item in media_evidence["wgc_smoothness_summary"])
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


def has_wgc_encoder_limited_judder(media_evidence, log_summary):
    if not log_summary:
        return False

    counts = log_summary["counts"]
    overload_seen = log_summary.get("saw_encoder_overload") or log_summary.get("saw_mux_overload")
    overload_seen = overload_seen or any(item["overload_flags"] != 0 for item in media_evidence["wgc_perf"])
    overload_seen = overload_seen or any(
        item.get("fault_hint") == "ce_capacity_pressure" for item in media_evidence["wgc_attribution"]
    )
    overload_seen = overload_seen or any(item.get("encoder_limited_drops", 0) > 0
                                         for item in media_evidence["wgc_smoothness_summary"])
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

        if startup_delay_ms > 0.0 and abs(startup_delay_ms - requested_delay_ms) > 5.0:
            return True
        if effective_delay_ms > 0.0 and abs(effective_delay_ms - requested_delay_ms) > 5.0:
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
    return (
        requested_delay_ms > 0.0
        and startup_delay_ms > 0.0
        and effective_delay_ms > 0.0
        and abs(startup_delay_ms - requested_delay_ms) <= 5.0
        and abs(effective_delay_ms - requested_delay_ms) <= 5.0
    )


def wgc_late_residual_is_bounded(item):
    if item.get("delay_residual_avg_abs_us", 0) > 5000:
        return False
    if item.get("delay_residual_p95_us", 0) > 10000:
        return False
    late_max_us = item.get("delay_residual_late_max_us", 0)
    if late_max_us > 10000:
        return False
    if late_max_us <= 0 and item.get("delay_residual_max_us", 0) > 10000:
        return False
    if wgc_has_raw_delay_residual_evidence(item):
        if item.get("raw_residual_avg_abs_us", 0) > 5000:
            return False
        if item.get("raw_residual_p95_us", 0) > 10000:
            return False
        raw_late_max_us = item.get("raw_residual_late_max_us", 0)
        if raw_late_max_us > 10000:
            return False
        if (
            raw_late_max_us <= 0
            and item.get("raw_residual_max_us", 0) > 10000
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
            or item.get("raw_residual_late_max_us", 0) > 10000
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

        if item.get("delay_residual_avg_abs_us", 0) > 5000:
            return True
        if item.get("delay_residual_p95_us", 0) > 10000:
            return True
        if item.get("delay_residual_late_max_us", 0) > 10000:
            return True
        if item.get("raw_residual_avg_abs_us", 0) > 5000:
            return True
        if item.get("raw_residual_p95_us", 0) > 10000:
            return True
        if item.get("raw_residual_late_max_us", 0) > 10000:
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


def has_wgc_sync_delay_policy_fault(media_evidence):
    for item in media_evidence["wgc_smoothness_summary"]:
        requested_delay_ms = item.get("av_delay_ms", 0.0)
        if requested_delay_ms <= 0.0:
            continue

        policy_holds = item.get("sync_delay_policy_holds", 0)
        if policy_holds >= 10:
            if wgc_is_bounded_source_limited_active_delay(media_evidence, item):
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


def classify_session_triage(session_dir, capture_path=None):
    media_log = session_dir / "media.log"
    media_text = read_text_if_exists(media_log)
    log_summary = analyze_log(media_log) if media_text else None
    media_evidence = parse_media_triage(media_text)
    hook_evidence = parse_hook_triage(session_dir)
    perf_summaries = parse_perf_csvs(session_dir)
    manifest = parse_session_manifest(session_dir)
    wgc_source_limits = summarize_wgc_source_limits(media_evidence)
    inject_pacing = summarize_inject_pacing(media_evidence)
    stop_audio_shortfalls = summarize_stop_audio_shortfalls(media_evidence)
    started_app_source_health = summarize_started_app_source_health(media_evidence, log_summary)

    verdicts = []
    max_present_gap_ms = max((item["gap_ms"] for item in hook_evidence["present_gaps"]), default=0.0)
    if max_present_gap_ms >= 100.0:
        verdicts.append("source_present_gap")
    if has_source_starvation(media_evidence):
        verdicts.append("wgc_source_starvation")
    if has_inject_capture_pacer_limit(inject_pacing):
        verdicts.append("ce_capture_pacer_limited")

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
    if (
        has_encoder_or_mux_backpressure(media_evidence, perf_summaries)
        or any(strict_mux_fault_counts.values())
        or writer_sync_after_timeout
    ):
        verdicts.append("ce_encoder_or_mux_backpressure")
    wgc_encoder_overload_policy_fault = has_wgc_encoder_overload_policy_fault(media_evidence, log_summary)
    if wgc_encoder_overload_policy_fault:
        verdicts.append("wgc_encoder_overload_policy_fault")
    wgc_encoder_limited_judder = has_wgc_encoder_limited_judder(media_evidence, log_summary)
    if wgc_encoder_limited_judder:
        verdicts.append("wgc_encoder_limited_judder")
    wgc_av_sync_delay_risk = has_wgc_av_sync_delay_realization_risk(media_evidence)
    if wgc_av_sync_delay_risk:
        verdicts.append("wgc_av_sync_delay_unrealized")
    wgc_av_sync_delay_residual_fault = has_wgc_av_sync_delay_residual_fault(media_evidence)
    if wgc_av_sync_delay_residual_fault:
        verdicts.append("wgc_av_sync_delay_residual")
    wgc_timestamp_domain_mismatch = has_wgc_timestamp_domain_mismatch(media_evidence)
    if wgc_timestamp_domain_mismatch:
        verdicts.append("wgc_timestamp_domain_mismatch")
    wgc_active_delay_post_selection_reject = has_wgc_active_delay_post_selection_reject(media_evidence)
    if wgc_active_delay_post_selection_reject:
        verdicts.append("wgc_active_delay_post_selection_reject")
    wgc_sync_delay_policy_fault = has_wgc_sync_delay_policy_fault(media_evidence)
    if wgc_sync_delay_policy_fault:
        verdicts.append("wgc_sync_delay_policy_fault")
    wgc_sync_delay_reserve_pressure = has_wgc_sync_delay_reserve_pressure(media_evidence)
    if (
        wgc_sync_delay_reserve_pressure
        and not wgc_sync_delay_policy_fault
        and not wgc_av_sync_delay_risk
        and not wgc_av_sync_delay_residual_fault
        and not wgc_timestamp_domain_mismatch
        and not wgc_active_delay_post_selection_reject
    ):
        verdicts.append("wgc_sync_delay_reserve_pressure")
    if started_app_source_health["late_source_backlog_count"] > 0 or started_app_source_health["backlog_sources"]:
        verdicts.append("late_app_source_backlog")
    if started_app_source_health["app_gap_silence_count"] > 0:
        if sparse_only_app_silence:
            verdicts.append("sparse_app_source_silence")
        else:
            verdicts.append("started_app_source_underrun")
    if post_mux_probe_hang:
        verdicts.append("post_mux_probe_hang")
    elif post_mux_probe_timeout:
        verdicts.append("post_mux_probe_timeout")
    post_mux_strict_mismatches = [
        delta for delta in media_evidence["post_mux_audio_mismatch_delta_us"] if delta > 1
    ]
    final_packet_strict = [
        item for item in media_evidence["final_packet_timelines"]
        if item["max_packet_delta_us"] > 1000 or item["audio_past_target"] > 0
    ]
    if stop_audio_shortfalls["multi_source_short_count"] > 0:
        verdicts.append("multi_app_audio_track_stall")
    if (
        any(strict_audio_fault_counts.values())
        or post_mux_strict_mismatches
        or final_packet_strict
        or media_evidence["zero_drift_warnings"]
        or stop_audio_shortfalls["short_count"] > 0
        or "late_app_source_backlog" in verdicts
        or "started_app_source_underrun" in verdicts
    ):
        verdicts.append("ce_audio_timeline_fault")
    if (
        any(visual_fault_counts.values())
        or "ce_capture_pacer_limited" in verdicts
        or wgc_encoder_limited_judder
        or wgc_encoder_overload_policy_fault
        or wgc_av_sync_delay_risk
        or wgc_av_sync_delay_residual_fault
        or wgc_timestamp_domain_mismatch
        or wgc_active_delay_post_selection_reject
        or wgc_sync_delay_policy_fault
    ):
        verdicts.append("ce_visual_timeline_fault")
    if hook_evidence["external_overlay_lines"]:
        verdicts.append("external_overlay_context")
    if hook_evidence["crash_events"]:
        verdicts.append("ce_process_crash")
    if not verdicts:
        verdicts.append("unknown")

    rounding_evidence = {
        "post_mux_audio_mismatch_delta_us": media_evidence["post_mux_audio_mismatch_delta_us"],
        "post_mux_one_us_or_less_is_info": all(
            delta <= 1 for delta in media_evidence["post_mux_audio_mismatch_delta_us"]
        ),
    }
    report = {
        "schema": "ce-session-av-triage-v1",
        "session_dir": str(session_dir),
        "capture": str(capture_path) if capture_path else None,
        "manifest": manifest,
        "paths": {
            "media_log": str(media_log) if media_log.exists() else None,
            "hook_logs": [str(path) for path in sorted(session_dir.glob("*.log")) if path.name.lower() != "media.log"],
            "perf_csv": [item["path"] for item in perf_summaries],
            "session_manifest": str(session_dir / "session_manifest.txt") if (session_dir / "session_manifest.txt").exists() else None,
        },
        "verdicts": verdicts,
        "faults": {
            "encoder_or_mux_backpressure": "ce_encoder_or_mux_backpressure" in verdicts,
            "audio_timeline": "ce_audio_timeline_fault" in verdicts,
            "visual_timeline": "ce_visual_timeline_fault" in verdicts,
            "wgc_encoder_overload_policy": wgc_encoder_overload_policy_fault,
            "wgc_av_sync_delay_unrealized": wgc_av_sync_delay_risk,
            "wgc_av_sync_delay_residual": wgc_av_sync_delay_residual_fault,
            "wgc_timestamp_domain_mismatch": wgc_timestamp_domain_mismatch,
            "wgc_active_delay_post_selection_reject": wgc_active_delay_post_selection_reject,
            "wgc_sync_delay_policy_fault": wgc_sync_delay_policy_fault,
            "wgc_sync_delay_reserve_pressure": wgc_sync_delay_reserve_pressure,
            "late_app_source_backlog": "late_app_source_backlog" in verdicts,
            "started_app_source_underrun": "started_app_source_underrun" in verdicts,
            "sparse_app_source_silence": "sparse_app_source_silence" in verdicts,
            "post_mux_probe_hang": post_mux_probe_hang,
            "post_mux_probe_timeout": post_mux_probe_timeout,
            "ce_process_crash": bool(hook_evidence["crash_events"]),
        },
        "evidence": {
            "max_present_gap_ms": max_present_gap_ms,
            "present_gaps": hook_evidence["present_gaps"][:20],
            "present_stalled_lines": hook_evidence["present_stalled_lines"][:20],
            "external_overlay_lines": hook_evidence["external_overlay_lines"],
            "crash_events": hook_evidence["crash_events"],
            "wgc_source_starved_episodes": media_evidence["source_starved_episodes"],
            "wgc_source_limits": wgc_source_limits,
            "inject_pacing": inject_pacing,
            "wgc_attribution": media_evidence["wgc_attribution"],
            "wgc_summary": media_evidence["wgc_summary"],
            "wgc_cadence_events": media_evidence["wgc_cadence_events"][:20],
            "wgc_smoothness_summary": media_evidence["wgc_smoothness_summary"],
            "wgc_perf_worst": {
                "max_fresh_miss_pm": max((item["fresh_miss_pm"] for item in media_evidence["wgc_perf"]), default=0),
                "min_input_250_fps": min((item["min_in_250"] for item in media_evidence["wgc_perf"] if item["min_in_250"] > 0), default=0),
                "min_delivered_250_fps": min((item["min_del_250"] for item in media_evidence["wgc_perf"] if item["min_del_250"] > 0), default=0),
                "max_callback_gap_us": max((item["cb_gap_max_us"] for item in media_evidence["wgc_perf"]), default=0),
                "max_copy_us": max((item["copy_us"] for item in media_evidence["wgc_perf"]), default=0),
                "max_fence_us": max((item["fence_us"] for item in media_evidence["wgc_perf"]), default=0),
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
            "zero_drift_warnings": media_evidence["zero_drift_warnings"],
            "stop_audio_shortfalls": stop_audio_shortfalls,
            "final_packet_timelines": media_evidence["final_packet_timelines"],
            "final_metadata": media_evidence["final_metadata"],
            "rounding_evidence": rounding_evidence,
        },
    }
    return report


def print_triage_report(report):
    print("session_av_triage:")
    print(f"  session_dir={report['session_dir']}")
    if report.get("capture"):
        print(f"  capture={report['capture']}")
    print(f"  verdicts={','.join(report['verdicts'])}")
    print(
        "  faults encoder_or_mux={enc} audio={audio} visual={visual}".format(
            enc=int(report["faults"]["encoder_or_mux_backpressure"]),
            audio=int(report["faults"]["audio_timeline"]),
            visual=int(report["faults"]["visual_timeline"]),
        )
    )
    evidence = report["evidence"]
    print(f"  max_present_gap_ms={evidence['max_present_gap_ms']:.3f}")
    wgc_source_limits = evidence["wgc_source_limits"]
    print(
        "  wgc_source_starved_episodes={detail} summary_episodes={summary} "
        "source_limited_repeats={repeats} longest_ms={longest} perf_csv={perf_count}".format(
            detail=wgc_source_limits["detail_episode_count"],
            summary=wgc_source_limits["summary_starved_episodes"],
            repeats=wgc_source_limits["summary_source_limited_repeats"],
            longest=wgc_source_limits["summary_longest_ms"],
            perf_count=len(evidence["perf_csv"]),
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
    if inject_pacing["perf_rows"] or inject_pacing["summary_duplicate"]:
        print(
            "  inject_drop_pace={drop_pace} inject_dup_src={dup_src} "
            "inject_source_fps={fps_min:.2f}..{fps_max:.2f}".format(
                drop_pace=inject_pacing["drop_pace"],
                dup_src=inject_pacing["summary_dup_src"],
                fps_min=inject_pacing["source_fps_min"],
                fps_max=inject_pacing["source_fps_max"],
            )
        )
    if evidence["wgc_smoothness_summary"]:
        worst_sync_delay = max(evidence["wgc_smoothness_summary"], key=lambda item: item.get("sync_delay_holds", 0))
        if worst_sync_delay.get("av_delay_ms", 0.0) > 0.0:
            print(
                "  wgc_av_delay requested={requested:.3f}ms startup={startup:.3f}ms effective={effective:.3f}ms "
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
                "post_reject_sync={post_reject} repeat_pressure={repeat_pressure}/{repeat_max} "
                "source_recovery={source_recovery_holds}/"
                "{source_recovery_ticks}".format(
                    requested=worst_sync_delay.get("av_delay_ms", 0.0),
                    startup=worst_sync_delay.get("startup_delay_ms", 0.0),
                    effective=worst_sync_delay.get("effective_delay_ms", 0.0),
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
                    post_reject=worst_sync_delay.get("delay_post_selection_rejected_sync", 0),
                    repeat_pressure=worst_sync_delay.get("delay_repeat_cluster_pressure", 0),
                    repeat_max=worst_sync_delay.get("delay_repeat_cluster_max_ticks", 0),
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
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)

        def make_session(name, media="", hook="", perf=""):
            session = root / name
            session.mkdir()
            (session / "session_manifest.txt").write_text(
                "build_version=test\ncapture_method=wgc\nnotes=test fixture\n", encoding="utf-8"
            )
            (session / "media.log").write_text(media, encoding="utf-8")
            if hook:
                (session / "hook_debug.log").write_text(hook, encoding="utf-8")
            if perf:
                (session / "perf_metrics_1.csv").write_text(perf, encoding="utf-8")
            return session

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

        inject_pacer = make_session(
            "inject_pacer",
            media=(
                "[Inject Perf] Input: 160 | Queued: 75 | DropFull: 0 | DropPace: 85 | PubFps: 240 | HostQ: 0 | "
                "EncQ: 0 | Dup: 5 | Late: 0 | Trim: 0 | SelDrop: 20 | Def: 0 | Encode: 308us | Fence: 2us | "
                "Mux: 0KB | Overload: 0x0\n"
                "[Inject CFR SUMMARY] Live=582 Dup=48 DupPct=8.2% DupReason(src=48 def=0 timer=0 drain=0) "
                "FreshCatchup=0 RepeatCatchup=0 StaleTrim=7 PathMismatch=0/0 DefRequeued=0 DefDropped=0\n"
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
        assert "wgc_sync_delay_policy_fault" in post_reject_report["verdicts"]
        assert "ce_visual_timeline_fault" in post_reject_report["verdicts"]

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

    print("self-test: PASS")


def main():
    parser = argparse.ArgumentParser(
        description="Analyze a capture file for CFR timing, duplicate-frame runs, and exact audio track alignment."
    )
    parser.add_argument("capture", nargs="?", type=Path, help="Capture file to analyze")
    parser.add_argument("--capture", dest="capture_option", type=Path, help="Capture file to attach to session triage")
    parser.add_argument("--session-dir", type=Path, help="Analyze a CE logs session for stutter attribution")
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
        action="store_true",
        help="Use ffprobe frame-by-frame scans for exact frame deltas and decoded audio sample totals",
    )
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
        report = classify_session_triage(args.session_dir, effective_capture)
        print_triage_report(report)
        if args.json_out:
            args.json_out.parent.mkdir(parents=True, exist_ok=True)
            args.json_out.write_text(json.dumps(report, indent=2), encoding="utf-8")
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
    nominal_fps = parse_ratio(video_stream.get("avg_frame_rate") or video_stream.get("r_frame_rate"))
    video_timing = (
        analyze_video_timing(args.ffprobe, args.capture, nominal_fps=nominal_fps)
        if args.full_scan
        else analyze_video_stream_metadata(video_stream, format_duration)
    )
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
    if args.waveform_tail_scan or args.max_audio_tail_marker_spread_ms is not None:
        tail_results = [
            analyze_audio_tail_marker(args.ffmpeg, args.capture, audio_ordinal, stream_info, args.tail_threshold)
            for audio_ordinal, stream_info in enumerate(audio_streams)
        ]
    log_summary = analyze_log(args.log)

    video_duration = video_timing["duration"]
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
        print("audio_tail_markers:")
        for result in tail_results:
            marker = result["last_marker_sample"]
            marker_text = "none" if marker is None else str(marker)
            time_text = "none" if result["last_marker_time"] is None else f"{result['last_marker_time']:.6f}"
            silence_text = "none" if result["tail_silence_ms"] is None else f"{result['tail_silence_ms']:.3f}ms"
            print(
                "  a:{ordinal} samples={samples} last_marker_sample={marker} last_marker_time={time} "
                "tail_silence={silence} threshold={threshold:g}".format(
                    ordinal=result["audio_ordinal"],
                    samples=result["samples"],
                    marker=marker_text,
                    time=time_text,
                    silence=silence_text,
                    threshold=args.tail_threshold,
                )
            )
            if result["stderr"]:
                print(f"    stderr={result['stderr'].replace(chr(10), ' | ')}")
        print(f"  audio_tail_marker_spread={audio_tail_marker_spread:.6f}")
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
    if tail_results:
        print(f"  audio_tail_marker_spread_ms={audio_tail_marker_spread * 1000.0:.3f}")
    print(
        "  all_audio_tracks_match_video_length={value}".format(
            value="yes" if math.isclose(video_audio_max_delta, 0.0, abs_tol=1e-3) else "no"
        )
    )
    print()
    print_checks(checks)

    if any(not check["passed"] for check in checks):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
