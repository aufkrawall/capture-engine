

def parse_media_triage(media_text):
    screen_capture_backend_events = []
    source_starved = []
    attribution = []
    wgc_perf = []
    wgc_summary = []
    wgc_quality = []
    recording_health = []
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
    stop_audio_ingest = []
    stop_audio_overlap = []
    stop_app_audio_latency = []
    zero_drift_warnings = []
    packet_mismatch_warnings = 0
    for line in media_text.splitlines():
        if not CFR_PHASE_LOCK_LINE_RE.search(line):
            for backend_match in SCREEN_CAPTURE_BACKEND_TOKEN_RE.finditer(line):
                backend = normalize_screen_capture_backend(backend_match.group(1))
                if backend and (
                    not screen_capture_backend_events
                    or screen_capture_backend_events[-1]["backend"] != backend
                ):
                    screen_capture_backend_events.append(
                        {
                            "backend": backend,
                            "timestamp_us": parse_log_timestamp_us(line),
                            "line": line,
                        }
                    )
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
        if RECORDING_HEALTH_RE.search(line):
            recording_health.append(parse_recording_health_line(line))
        if WGC_SOURCE_COVERAGE_RE.search(line):
            wgc_source_coverage.append(parse_wgc_source_coverage_line(line))
        cadence_event_match = WGC_CADENCE_EVENT_RE.search(line)
        if cadence_event_match:
            event = parse_attribution_payload(cadence_event_match.group(2))
            event["mode"] = cadence_event_match.group(1)
            event["timestamp_us"] = parse_log_timestamp_us(line)
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
                    "delay_reservoir_low_water_frames": (
                        parse_int(delay_realization.group(1)) if delay_realization else 0
                    ),
                    "delay_reservoir_target_frames": parse_int(delay_realization.group(2)) if delay_realization else 0,
                    "delay_reservoir_low_water_ticks": (
                        parse_int(delay_realization.group(3)) if delay_realization else 0
                    ),
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
        stop_detail_overlap_match = STOP_AUDIO_DETAIL_OVERLAP_RE.search(line)
        if stop_detail_overlap_match:
            stop_audio_overlap.append(
                {
                    "source": parse_int(stop_detail_overlap_match.group(1)),
                    "overlap_samples": parse_int(stop_detail_overlap_match.group(2)),
                    "line": line,
                }
            )
        stop_ingest_match = STOP_AUDIO_INGEST_RE.search(line)
        if stop_ingest_match:
            stop_audio_ingest.append(
                {
                    "source": parse_int(stop_ingest_match.group(1)),
                    "track": parse_int(stop_ingest_match.group(2)),
                    "starved_samples": parse_int(stop_ingest_match.group(3)),
                    "resync_samples": parse_int(stop_ingest_match.group(4)),
                    "resync_events": parse_int(stop_ingest_match.group(5)),
                    "reservoir_peak_ms": parse_int(stop_ingest_match.group(6)),
                    "process": stop_ingest_match.group(7) or "",
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
        "screen_capture_backend_events": screen_capture_backend_events,
        "source_starved_episodes": source_starved,
        "wgc_attribution": attribution,
        "wgc_perf": wgc_perf,
        "wgc_summary": wgc_summary,
        "wgc_quality": wgc_quality,
        "recording_health": recording_health,
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
        "stop_audio_ingest": stop_audio_ingest,
        "stop_audio_overlap": stop_audio_overlap,
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
