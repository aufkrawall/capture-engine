

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
    backend_history = evidence.get("screen_capture_backend_history", [])
    if len(backend_history) > 1:
        print(f"  screen_capture_backend_history={'->'.join(backend_history)}")
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
        "source_limited_pct={src_pct:.1f}% longest_contiguous_hold={contig_dup}f/{contig_ms}ms "
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
    ingest_starvation = evidence.get("audio_ingest_starvation")
    if ingest_starvation and (
        ingest_starvation["destroyed_samples"]
        or ingest_starvation["resync_events"]
        or ingest_starvation["reservoir_peak_ms"]
    ):
        print(
            "  audio_ingest destroyed={destroyed} samples ({destroyed_ms:.1f} ms) sources={sources} "
            "resync={resync_events}/{resync_samples} reservoir_peak_ms={peak}".format(
                destroyed=ingest_starvation["destroyed_samples"],
                destroyed_ms=ingest_starvation["destroyed_ms"],
                sources=len(ingest_starvation["affected_sources"]),
                resync_events=ingest_starvation["resync_events"],
                resync_samples=ingest_starvation["resync_samples"],
                peak=ingest_starvation["reservoir_peak_ms"],
            )
        )
    app_health = evidence["started_app_source_health"]
    if (
        app_health["late_source_backlog_count"]
        or app_health["late_join_live_count"]
        or app_health["app_gap_silence_count"]
    ):
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
        if len(backend_history) > 1:
            phase_lock_backend = screen_capture_backend
        elif phase_lock_backend == "wgc" and screen_capture_backend == "dxgi_dup":
            phase_lock_backend = screen_capture_backend
        print(
            "  cfr_phase_lock backend={backend} enabled={enabled} locked={locked} offset={offset}us "
            "stable={stable} unstable={unstable} transitions={acquire}/{rephase}/{release} "
            "multiplier={multiplier}".format(
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
                "near_cap={near_cap} near_cap_10s={near_cap_window}/{near_cap_accounted} "
                "hard_only={hard_only} sync_protected={sync_protected} "
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
                    near_cap_window=evidence["wgc_near_cap_window_pressure"]["max_accepted"],
                    near_cap_accounted=evidence["wgc_near_cap_window_pressure"]["accepted_total"],
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
        if not completed.get("authoritative", True):
            print(
                "  completed_capture mode=metadata authoritative=0 container={duration:.6f}s "
                "video={codec} fps={fps} estimated_frames={frames} audio_tracks={tracks}".format(
                    duration=completed["container_duration"],
                    codec=video["codec"],
                    fps=video["fps"],
                    frames=video["frame_count"],
                    tracks=len(completed["tracks"]),
                )
            )
            return
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
        f"  audio_duration_spread={window['audio_duration_spread']:.6f} "
        f"max_video_audio_duration_delta={window['video_audio_max_delta']:.6f}"
    )
    if window["duplicate_runs"] is None:
        print("  duplicate_runs=skipped")
    else:
        print(
            "  duplicate_runs framehash_frames={framehash_count} repeated_runs={repeated_runs} "
            "repeated_frames={repeated_frames} longest_run={longest}".format(
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
