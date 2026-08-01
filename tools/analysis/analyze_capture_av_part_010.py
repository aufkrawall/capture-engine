

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
    recording_status = recording_manifest.get("status", "")
    recording_finalization_complete = parse_int(recording_manifest.get("finalization_complete"), 0) == 1
    recording_output_saved = None
    if "output_saved" in recording_manifest:
        recording_output_saved = parse_int(recording_manifest.get("output_saved"), 0) == 1
    recording_finalization_failed = recording_finalization_complete and (
        recording_status == "recording_failed"
        or (recording_status != "recording_canceled" and recording_output_saved is False)
    )
    screen_capture_backend_history = resolve_screen_capture_backend_history(manifest, media_evidence)
    screen_capture_backend = resolve_screen_capture_backend(manifest, media_evidence)
    screen_capture_diagnostic_prefix = (
        screen_capture_backend_history[0]
        if len(screen_capture_backend_history) == 1
        else "screen_capture"
    )

    def screen_capture_diagnostic(suffix):
        return f"{screen_capture_diagnostic_prefix}_{suffix}"

    wgc_source_limits = summarize_wgc_source_limits(media_evidence)
    inject_pacing = summarize_inject_pacing(media_evidence)
    stop_audio_shortfalls = summarize_stop_audio_shortfalls(media_evidence)
    audio_ingest_starvation = summarize_audio_ingest_starvation(media_evidence)
    started_app_source_health = summarize_started_app_source_health(media_evidence, log_summary)
    live_start_wall_us = parse_live_start_wall_us(media_text)
    stop_start_wall_us = parse_stop_start_wall_us(media_text)
    app_audio_latency = summarize_app_audio_latency(media_evidence, log_summary, stop_start_wall_us)
    inject_contention_context = summarize_inject_contention_context(media_evidence, live_start_wall_us)

    verdicts = []
    contexts = []
    recording_health_summary = summarize_recording_health_attribution(
        media_evidence, recording_manifest, log_summary
    )
    recording_health = recording_health_summary["health"]
    hard_wgc_pool_pressure = recording_health_summary["hard_wgc_pool_pressure"]
    recording_health_degraded = recording_health_summary["degraded"]
    recording_health_encoder_cause = recording_health_summary["encoder_cause"]
    recording_health_mux_cause = recording_health_summary["mux_cause"]
    wgc_capacity_debt_history_loss = recording_health_summary["capacity_debt_history_loss"]
    if recording_health_summary["inferred"]:
        contexts.append(screen_capture_diagnostic("recording_health_inferred_from_legacy_evidence"))
    controller_text = read_text_if_exists(session_dir / "captureengine.log")
    controller_recording_starts = len(
        re.findall(r"\[Controller\] Starting (?:audio-only )?recording\.\.\.", controller_text)
    )
    recording_evidence_incomplete = controller_recording_starts > len(discovered_recordings)
    if recording_evidence_incomplete:
        contexts.append("recording_evidence_missing_or_overwritten")
    if perf_scope_unavailable:
        contexts.append("recording_perf_evidence_unscoped")
    if len(screen_capture_backend_history) > 1:
        contexts.append("screen_capture_backend_transition")
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
        if wgc_capacity_debt_history_loss:
            contexts.append(screen_capture_diagnostic("source_starvation_after_capacity_debt"))
        else:
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
        if wgc_capacity_debt_history_loss and not hard_wgc_pool_pressure:
            contexts.append("duplication_delivery_loss_after_capacity_debt")
        else:
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
        if wgc_capacity_debt_history_loss:
            contexts.append(screen_capture_diagnostic("delivery_gap_after_capacity_debt"))
        else:
            verdicts.append(screen_capture_diagnostic("delivery_gap"))
    if log_summary and log_summary["counts"].get("wgc_cfr_producer_contract_fault", 0) > 0:
        verdicts.append(screen_capture_diagnostic("producer_rate_contract_fault"))
    wgc_framepool_pressure_evidence = has_wgc_framepool_pressure_attribution(media_evidence)
    wgc_framepool_pressure = wgc_framepool_pressure_evidence and not (
        wgc_capacity_debt_history_loss and not hard_wgc_pool_pressure
    )
    if wgc_framepool_pressure:
        verdicts.append(screen_capture_diagnostic("framepool_pressure"))
    elif wgc_framepool_pressure_evidence:
        contexts.append(screen_capture_diagnostic("framepool_pressure_after_capacity_debt"))
    wgc_pool_lease_contention = any(
        item.get("fault_hint") == "wgc_pool_lease_contention"
        or parse_numeric_prefix_int(item.get("overwritePrevented"), 0) > 0
        for item in media_evidence["wgc_attribution"]
    )
    if wgc_pool_lease_contention and not wgc_framepool_pressure:
        contexts.append(screen_capture_diagnostic("pool_lease_contention"))
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
        or (recording_health_degraded and recording_health_encoder_cause)
        or (recording_health_degraded and recording_health_mux_cause)
    )
    if encoder_or_mux_backpressure:
        verdicts.append("ce_encoder_or_mux_backpressure")
    hardware_encoder_starvation = recording_health_degraded and recording_health_encoder_cause
    hardware_encoder_starvation = hardware_encoder_starvation or any(
        item.get("encoder_overload", 0) for item in media_evidence["final_metadata"]
    )
    hardware_encoder_starvation = hardware_encoder_starvation or any(
        item.get("overload_flags", 0) & 0x1 for item in media_evidence["wgc_perf"]
    )
    hardware_encoder_starvation = hardware_encoder_starvation or any(
        item.get("overload_flags", 0) & 0x1 for item in media_evidence["inject_perf"]
    )
    if hardware_encoder_starvation:
        verdicts.append("hardware_encoder_starvation")
    if recording_health_degraded and recording_health_encoder_cause:
        verdicts.append(screen_capture_diagnostic("encoder_timeline_debt"))
    if recording_health_degraded and recording_health_mux_cause:
        verdicts.append(screen_capture_diagnostic("mux_timeline_debt"))
    if not recording_health_degraded and recording_health_encoder_cause:
        contexts.append("recording_encoder_capacity_pressure_observed")
    if not recording_health_degraded and recording_health_mux_cause:
        contexts.append("recording_mux_capacity_pressure_observed")
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
    wgc_clean_source_limited_coverage = has_wgc_clean_source_limited_coverage(media_evidence)
    wgc_source_limited_playout_maximal = has_wgc_source_limited_playout_maximal(media_evidence)
    wgc_backend_transition_source_limited_playout_maximal = (
        has_wgc_backend_transition_source_limited_playout_maximal(
            media_evidence, screen_capture_backend_history
        )
    )
    wgc_source_limited_playout_maximal = (
        wgc_source_limited_playout_maximal
        or wgc_backend_transition_source_limited_playout_maximal
    )
    wgc_av_sync_delay_risk = has_wgc_av_sync_delay_realization_risk(media_evidence)
    if wgc_av_sync_delay_risk:
        verdicts.append(screen_capture_diagnostic("av_sync_delay_unrealized"))
    wgc_av_sync_delay_residual_fault = has_wgc_av_sync_delay_residual_fault(
        media_evidence, wgc_source_limited_playout_maximal
    )
    if wgc_av_sync_delay_residual_fault:
        verdicts.append(screen_capture_diagnostic("av_sync_delay_residual"))
    wgc_audio_late_risk = has_wgc_audio_late_risk(
        media_evidence, wgc_source_limited_playout_maximal
    )
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
    wgc_startup_smoothness_underfilled = has_wgc_startup_smoothness_underfilled(media_evidence)
    if wgc_startup_smoothness_underfilled:
        if wgc_source_limited_playout_maximal:
            contexts.append(screen_capture_diagnostic("startup_reservoir_partial"))
        else:
            verdicts.append(screen_capture_diagnostic("startup_smoothness_underfilled"))
    wgc_active_delay_realized_delay_unstable = has_wgc_active_delay_realized_delay_unstable(
        media_evidence, wgc_source_limited_playout_maximal
    )
    if wgc_active_delay_realized_delay_unstable:
        verdicts.append(screen_capture_diagnostic("active_delay_realized_delay_unstable"))
    wgc_near_cap_window = wgc_near_cap_window_pressure(media_evidence)
    wgc_source_limited_delay_variation_context = has_wgc_source_limited_delay_variation_context(
        media_evidence, wgc_source_limited_playout_maximal
    )
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
        if wgc_capacity_debt_history_loss and not hard_wgc_pool_pressure:
            contexts.append(screen_capture_diagnostic("ingress_decimated_after_capacity_debt"))
        else:
            verdicts.append(screen_capture_diagnostic("ingress_decimated"))
    wgc_uniform_playout_ingress_double_decimation = has_wgc_uniform_playout_ingress_double_decimation(media_evidence)
    if wgc_uniform_playout_ingress_double_decimation:
        verdicts.append(screen_capture_diagnostic("uniform_playout_ingress_double_decimation"))
    wgc_copy_pool_pressure = has_wgc_copy_pool_pressure(media_evidence)
    if wgc_copy_pool_pressure:
        if wgc_capacity_debt_history_loss and not hard_wgc_pool_pressure:
            contexts.append(screen_capture_diagnostic("copy_pool_pressure_after_capacity_debt"))
        else:
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
    exported_av_sync_ok = has_exact_final_mux_evidence(media_evidence)
    encoder_debt_audio_backlog_recovered = (
        recording_health_degraded
        and recording_health_encoder_cause
        and exported_av_sync_ok
        and app_audio_latency["queue_overrun_packets"] == 0
        and app_audio_latency["queue_overrun_frames"] == 0
        and app_audio_latency["underruns"] == 0
        and app_audio_latency["catastrophic_resync_events"] == 0
        and audio_ingest_starvation["destroyed_samples"] == 0
        and stop_audio_shortfalls["short_count"] == 0
    )
    if app_audio_latency["fault_evidence"]:
        if encoder_debt_audio_backlog_recovered:
            contexts.append("app_audio_latency_following_encoder_debt")
        else:
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
    if log_summary and log_summary["counts"].get("audio_stop_force_drain_backlog", 0) > 0:
        contexts.append("audio_stop_force_drain_backlog")
    if stop_audio_shortfalls["multi_source_short_count"] > 0:
        verdicts.append("multi_app_audio_track_stall")
    # Consumer overrun destroys captured audio while leaving track lengths, packet timing,
    # and drift residuals perfect, so it must be its own strict verdict rather than being
    # inferred from any duration-based check.
    if audio_ingest_starvation["destroyed_samples"] > 0:
        verdicts.append("audio_ingest_starvation")
    if audio_ingest_starvation["resync_events"] > 0:
        verdicts.append("audio_ingest_starvation_resync")
    timeline_audio_fault_counts = dict(strict_audio_fault_counts)
    if encoder_debt_audio_backlog_recovered:
        timeline_audio_fault_counts["audio_extreme_drift"] = 0
        if strict_audio_fault_counts.get("audio_extreme_drift", 0) > 0:
            contexts.append("audio_drift_backlog_recovered_after_encoder_debt")
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
        or audio_ingest_starvation["destroyed_samples"] > 0
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
        or recording_health_degraded
    ):
        verdicts.append("ce_visual_timeline_fault")
    if hook_evidence["external_overlay_lines"]:
        contexts.append("external_overlay_present")
    if hook_evidence["crash_events"]:
        verdicts.append("ce_process_crash")
    if recording_finalization_failed:
        verdicts.append("ce_recording_output_not_saved")
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
            "session_manifest": (
                str(session_dir / "session_manifest.txt")
                if (session_dir / "session_manifest.txt").exists()
                else None
            ),
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
            "recording_health_degraded": recording_health_degraded,
            "recording_encoder_timeline_debt": recording_health_degraded and recording_health_encoder_cause,
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
            "wgc_backend_transition_source_limited_playout_maximal": (
                wgc_backend_transition_source_limited_playout_maximal
            ),
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
            "recording_output_not_saved": recording_finalization_failed,
            "ce_process_crash": bool(hook_evidence["crash_events"]),
        },
        "evidence": {
            "screen_capture_backend": screen_capture_backend,
            "screen_capture_backend_history": screen_capture_backend_history,
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
            "recording_health": recording_health,
            "recording_finalization": {
                "status": recording_status,
                "complete": recording_finalization_complete,
                "output_saved": recording_output_saved,
                "failed": recording_finalization_failed,
            },
            "wgc_source_coverage": media_evidence["wgc_source_coverage"],
            "wgc_cadence_events": media_evidence["wgc_cadence_events"][:20],
            "wgc_near_cap_window_pressure": wgc_near_cap_window,
            "wgc_smoothness_summary": media_evidence["wgc_smoothness_summary"],
            "wgc_perf_worst": {
                "max_fresh_miss_pm": max((item["fresh_miss_pm"] for item in media_evidence["wgc_perf"]), default=0),
                "min_input_250_fps": min(
                    (item["min_in_250"] for item in media_evidence["wgc_perf"] if item["min_in_250"] > 0),
                    default=0,
                ),
                "min_delivered_250_fps": min(
                    (item["min_del_250"] for item in media_evidence["wgc_perf"] if item["min_del_250"] > 0),
                    default=0,
                ),
                "max_callback_gap_us": max((item["cb_gap_max_us"] for item in media_evidence["wgc_perf"]), default=0),
                "max_copy_us": max((item["copy_us"] for item in media_evidence["wgc_perf"]), default=0),
                "max_convert_us": max((item.get("convert_us", 0) for item in media_evidence["wgc_perf"]), default=0),
                "max_fence_us": max((item["fence_us"] for item in media_evidence["wgc_perf"]), default=0),
                "compact_retained_active": any(item.get("compact_retained", 0) for item in media_evidence["wgc_perf"]),
                "source_format": max((item.get("source_format", 0) for item in media_evidence["wgc_perf"]), default=0),
                "copy_format": max((item.get("copy_format", 0) for item in media_evidence["wgc_perf"]), default=0),
                "max_pool_lease": max(
                    (item.get("pool_lease_max", 0) for item in media_evidence["wgc_perf"]), default=0
                ),
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
            "audio_ingest_starvation": audio_ingest_starvation,
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
