

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
    if any(
        summary["source_limited_repeats"] > 0 or summary["starved_episodes"] > 0
        for summary in media_evidence["wgc_summary"]
    ):
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
    clean_retention_rows = []
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
        if (
            expected_fps >= 30
            and row.get("overload_flags", 0) == 0
            and row.get("hold_with_candidate", 0) >= max(6, math.ceil(expected_fps * 0.20))
        ):
            clean_retention_rows.append(row)

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
        "phase_reserve_peak": max((row.get("phase_reserve_peak", 0) for row in quality_rows), default=0),
        "phase_shift_max_us": max((row.get("phase_shift_max_us", 0) for row in quality_rows), default=0),
        "preserve_front_trim": sum(row.get("preserve_front_trim", 0) for row in quality_rows),
        "display_path_transitions": sum(
            row.get("display_path_transitions", 0) for row in quality_rows
        ),
        "pressure_rows": len(pressure_rows),
        "pressure_hold_with_candidate": sum(row.get("hold_with_candidate", 0) for row in pressure_rows),
        "matched_rate_pressure_rows": len(matched_pressure_rows),
        "matched_rate_hold_with_candidate": sum(
            row.get("hold_with_candidate", 0) for row in matched_pressure_rows
        ),
        "matched_rate_superseded": sum(row.get("target_superseded", 0) for row in matched_pressure_rows),
        "matched_rate_longest_run": longest_matched_run,
        "clean_retention_pressure_rows": len(clean_retention_rows),
        "clean_retention_hold_with_candidate": sum(
            row.get("hold_with_candidate", 0) for row in clean_retention_rows
        ),
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


def has_inject_timestamp_retention_fault(inject_pacing):
    live = inject_pacing["summary_live"]
    hold_with_candidate = inject_pacing["target_hold_with_candidate"]
    cap_trim = inject_pacing["buffer_cap_trim"]
    if live <= 0:
        return False
    fault_floor = max(12, math.ceil(live * 0.02))
    return (
        min(hold_with_candidate, cap_trim) >= fault_floor
        and inject_pacing["clean_retention_pressure_rows"] >= 1
        and inject_pacing["clean_retention_hold_with_candidate"] >= fault_floor
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
    coverage = media_evidence["wgc_source_coverage"]
    final_pool_clean = bool(coverage) and all(item.get("clean_pool", 0) > 0 and item.get("pool_pressure", 0) == 0
                                              for item in coverage)
    for item in media_evidence["wgc_attribution"]:
        hint = item.get("fault_hint")
        overwrite_prevented = parse_numeric_prefix_int(item.get("overwritePrevented"), 0) > 0
        lossy = any(parse_numeric_prefix_int(item.get(key), 0) > 0 for key in ("poolSat", "ingressDecimated"))
        legacy_pressure = hint == "wgc_framepool_pressure" and not (overwrite_prevented and final_pool_clean)
        if lossy or legacy_pressure or hint == "wgc_framepool_overflow_suspected":
            return True
    return False


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
    overload_seen = (
        capacity_pressure_proven is True
        or log_summary.get("saw_encoder_overload")
        or log_summary.get("saw_mux_overload")
    )
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
    overload_seen = (
        capacity_pressure_proven is True
        or log_summary.get("saw_encoder_overload")
        or log_summary.get("saw_mux_overload")
    )
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
