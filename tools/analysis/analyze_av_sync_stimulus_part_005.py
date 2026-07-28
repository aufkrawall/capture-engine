

def self_test():
    reference = [{"to": 1, "time": 1.0}, {"to": 2, "time": 2.0}, {"to": 3, "time": 3.0}]
    candidate = [{"to": 1, "time": 1.012}, {"to": 2, "time": 2.015}, {"to": 3, "time": 2.990}]
    offsets, missing = match_transition_offsets(reference, candidate, 0.1)
    assert missing == 0
    assert round(max(abs(offset) for offset in offsets), 3) == 0.015
    points, missing = match_transition_offset_points(reference, candidate, 0.1)
    assert missing == 0
    assert round(compute_offset_slope_ms_per_minute(points), 3) == -660.0
    extra_candidate = candidate + [{"to": 3, "time": 3.4}]
    offsets, missing = match_transition_offsets(reference, extra_candidate, 0.1)
    assert missing == 0
    missing_candidate = candidate[:2]
    offsets, missing = match_transition_offsets(reference, missing_candidate, 0.1)
    assert missing == 1
    stats = summarize_offsets_ms(offsets)
    assert stats["matched"] == 2
    assert round(stats["mean_signed"], 3) == 13.5
    assert round(stats["max_abs"], 3) == 15.0
    assert round(stats["span"], 3) == 3.0
    assert offset_slope_is_acceptable(offsets, stats, 120.0, 30.0, 5.0)
    assert not offset_slope_is_acceptable([0.0, 0.020], {"span": 20.0}, 120.0, 30.0, 5.0)
    one_frame_wgc_offsets = [0.0132, 0.0131, 0.0130, 0.0, 0.0, 0.0]
    one_frame_wgc_stats = summarize_offsets_ms(one_frame_wgc_offsets)
    assert offset_slope_is_acceptable(
        one_frame_wgc_offsets,
        one_frame_wgc_stats,
        -130.0,
        30.0,
        12.0,
        1000.0 / 60.0,
        25.0,
        15.0,
    )
    one_frame_wgc_stats["max_abs"] = 26.0
    assert not offset_slope_is_acceptable(
        one_frame_wgc_offsets,
        one_frame_wgc_stats,
        -130.0,
        30.0,
        12.0,
        1000.0 / 60.0,
        25.0,
        15.0,
    )
    repeated_reference = [
        {"to": 1, "time": 1.0},
        {"to": 2, "time": 2.0},
        {"to": 1, "time": 17.0},
        {"to": 2, "time": 18.0},
    ]
    repeated_candidate = [
        {"to": 1, "time": 1.004},
        {"to": 2, "time": 2.004},
        {"to": 1, "time": 17.004},
        {"to": 2, "time": 18.004},
    ]
    points, missing = match_transition_offset_points(repeated_reference, repeated_candidate, 0.1)
    assert missing == 0
    assert len(points) == 4
    segments, transitions = compress_states([(0.0, 1), (0.1, 1), (0.2, 2), (0.3, 2)], min_duration=0.05)
    assert len(segments) == 2
    assert transitions[0]["to"] == 2
    audio = array.array("f")
    phase = 0.0
    rate = 48000
    freqs = [440.0, 660.0]
    for sample_index in range(rate * 2):
        freq = freqs[0] if sample_index < rate else freqs[1]
        audio.append(0.25 * math.sin(phase))
        phase += 2.0 * math.pi * freq / rate
        if phase > 2.0 * math.pi:
            phase -= 2.0 * math.pi
    _, audio_transitions = detect_audio_states(audio, rate, freqs)
    assert audio_transitions and abs(audio_transitions[0]["time"] - 1.0) < 0.01
    assert "coarse_time" in audio_transitions[0]
    assert audio_refinement_is_plausible(1.0, 1.011)
    assert not audio_refinement_is_plausible(1.0, 1.014)
    detector_biases = calibrate_audio_detector_biases(48000, [440.0, 550.0, 660.0, 770.0])
    assert detector_biases
    biased_transitions = []
    reference_transitions = []
    for event_index in range(1, 9):
        to_index = event_index % 4
        from_index = (to_index - 1) % 4
        bias = detector_biases[(from_index, to_index)]
        biased_transitions.append({"from": from_index, "to": to_index, "time": event_index + bias})
        reference_transitions.append({"from": from_index, "to": to_index, "time": float(event_index)})
    corrected_transitions = apply_audio_detector_bias_correction(biased_transitions, detector_biases)
    corrected_offsets, corrected_missing = match_transition_offsets(reference_transitions, corrected_transitions, 0.05)
    assert corrected_missing == 0
    assert max(abs(offset) for offset in corrected_offsets) < 0.000001
    clean_counts = analyze_ce_log_text(
        "[PullAudio] Track 1 bootstrap complete - target=3ms samples=160 forced=0 trimmed=0 protected=1029\n"
        "[A/V SYNC CHECK] Track 1: RetainTrim=0, CoverageTrim=0, Tier2Trim=0, BootstrapTrim=0 residual_samples=+0\n"
    )
    assert clean_counts["audio_trim"] == 0
    assert clean_counts["audio_zero_drift_residual"] == 0
    bad_counts = analyze_ce_log_text(
        "[PullAudio] Track 1 bootstrap complete - target=3ms samples=160 forced=0 trimmed=4 protected=1029\n"
        "[PullAudio] Audio latency cap: src 0 ahead by 1200 samples - trimming 240\n"
        "[PullAudio] WARNING: Source underrun - src 1 padding 480 samples with silence "
        "(available=0 needed=480 forceDrain=0)\n"
        "[PullAudio] WARNING: Source underrun - src 2 padding 800 samples with silence "
        "(available=0 needed=800 forceDrain=1)\n"
        "[PullAudio] Source primed - src=9 track=1 buffered=359086 realBuffered=358483 needed=1200 lateStart=7459ms\n"
        "[PullAudio] App source gap silence - src 9 added 480 samples to track 1\n"
        "[VideoEncoder] Stop: ERROR writer_finalize_timeout result=258 phase=post_mux_probe elapsed=30000ms\n"
        "[STOP AUDIO] Source 3: track=2 encoded=1000 trim=cov:0 latTotal:0 liveUncat:0 pad:42 qgap:0\n"
        "[A/V ZERO DRIFT WARNING] Track 1 residual_samples=+1 residual_us=+21\n"
    )
    assert bad_counts["audio_trim"] == 2
    assert bad_counts["audio_underrun"] == 1
    assert bad_counts["audio_stop_tail_padding"] == 1
    assert bad_counts["audio_strict_source_padding"] == 1
    assert bad_counts["audio_late_app_source_backlog"] == 1
    assert bad_counts["audio_app_source_gap_silence"] == 1
    assert bad_counts["audio_zero_drift_residual"] == 1
    assert bad_counts["post_mux_probe_hang"] == 1
    live_join_counts = analyze_ce_log_text(
        "[AudioLoop] Late app source live join src=9 track=1 process=dx12_av_sync_late.exe "
        "packetStart=358003 trackCursor=358483 joinCursor=358483 suppressedGap=358003 "
        "preservedGap=0 qpcStart=123\n"
        "[PullAudio] Source primed - src=9 track=1 buffered=1200 realBuffered=1200 needed=1200 lateStart=7459ms\n"
    )
    assert live_join_counts["audio_late_app_source_backlog"] == 0
    typed_source_counts = analyze_ce_log_text(
        "[PullAudio] Source primed - src=0 realBuffered=1200 samples "
        "synthetic(ring=0 inflight=0 post=0) lateStart=4600ms\n"
        "[AudioLoop] Late app source live join src=13 track=1 process=game.exe "
        "packetStart=191057 trackCursor=191520 joinCursor=191520 suppressedGap=191057 "
        "preservedGap=0 qpcStart=123\n"
        "[PullAudio] Source primed - src=13 realBuffered=1200 samples "
        "synthetic(ring=0 inflight=0 post=0) lateStart=3981ms\n"
        "[STOP AUDIO] Source 0: track=3 encoded=48000 trim=cov:0 latTotal:0 liveUncat:0 "
        "cat:0 normal:0 pad:0 qgap:1695 qjoin:0 qjoinKeep:0 ringPeak=20738 "
        "ringUnderruns=0 process=-\n"
        "[STOP AUDIO] Source 13: track=1 encoded=48000 trim=cov:0 latTotal:0 liveUncat:0 "
        "cat:0 normal:0 pad:0 qgap:945 qjoin:191057 qjoinKeep:0 ringPeak=30545 "
        "ringUnderruns=0 process=game.exe\n"
    )
    assert typed_source_counts["audio_late_app_source_backlog"] == 0
    explicit_app_counts = analyze_ce_log_text(
        "[PullAudio] Source primed - src=9 realBuffered=1200 samples "
        "synthetic(ring=0 inflight=0 post=0) lateStart=7459ms app=1\n"
    )
    assert explicit_app_counts["audio_late_app_source_backlog"] == 1
    historical_force_drain_counts = analyze_ce_log_text(
        "[AppDrain] state src=11 track=1 active=0 reason=force_drain delayMs=2830 "
        "targetMs=142 excessMs=2688 rb=135868 target=6816 delta=0 comp=0.0000% "
        "forceDrain=1 startupSettled=1 startupProtected=0\n"
        "[PullAudio] WARNING: Extreme drift detected (129052 samples src=11) - may indicate sync issue\n"
    )
    assert historical_force_drain_counts["audio_extreme_drift"] == 0
    assert historical_force_drain_counts["audio_stop_force_drain_backlog"] == 1
    live_drift_counts = analyze_ce_log_text(
        "[PullAudio] WARNING: Extreme drift detected (129052 samples src=11) "
        "forceDrain=0 - may indicate sync issue\n"
    )
    assert live_drift_counts["audio_extreme_drift"] == 1
    assert live_drift_counts["audio_stop_force_drain_backlog"] == 0
    legacy_unstructured_drift_counts = analyze_ce_log_text(
        "[PullAudio] WARNING: Extreme drift detected - legacy diagnostic\n"
    )
    assert legacy_unstructured_drift_counts["audio_extreme_drift"] == 1
    assert legacy_unstructured_drift_counts["audio_stop_force_drain_backlog"] == 0
    current_force_drain_counts = analyze_ce_log_text(
        "[PullAudio] Stop force-drain backlog: drift=129052 samples src=11 forceDrain=1 "
        "(post-target backlog is excluded from output)\n"
    )
    assert current_force_drain_counts["audio_extreme_drift"] == 0
    assert current_force_drain_counts["audio_stop_force_drain_backlog"] == 1
    clean_app_counts = analyze_app_log_text(
        "AVSYNC FRAME_TIMING planned_source_gap frameId=10 deltaMs=305.0 thresholdMs=33.3\n"
        "AVSYNC SUMMARY frame_timing targetFps=60 spikes=0\n"
    )
    assert clean_app_counts["frame_pacing_spike"] == 0
    bad_app_counts = analyze_app_log_text(
        "AVSYNC WARNING frame pacing spike frameId=42 deltaMs=50.000 thresholdMs=33.333\n"
        "AVSYNC WARNING audio event timeout count=1\n"
    )
    assert bad_app_counts["frame_pacing_spike"] == 1
    assert bad_app_counts["audio_event_timeout"] == 1
    manifest = {
        "source_stalls": [
            {
                "index": 0,
                "actual_start_seconds": 8.0,
                "actual_end_seconds": 8.3,
                "expected_repeat_span_seconds": 0.3,
                "tolerance_seconds": 0.05,
                "suppressed_present_count": 18,
            }
        ]
    }
    clusters = [{"start_pts": 8.02, "end_pts": 8.30, "frames": 18, "repeated_frames": 17, "marker": 99}]
    planned, source_fps_limited, unplanned, missing = classify_repeat_clusters(clusters, manifest)
    assert len(planned) == 1
    assert not source_fps_limited
    assert not unplanned
    assert not missing
    clusters = [{"start_pts": 4.0, "end_pts": 4.2, "frames": 12, "repeated_frames": 11, "marker": 100}]
    planned, source_fps_limited, unplanned, missing = classify_repeat_clusters(clusters, manifest)
    assert not planned
    assert not source_fps_limited
    assert len(unplanned) == 1
    low_fps_manifest = {"target_fps": 45}
    low_fps_clusters = [{"start_pts": 1.0, "end_pts": 1.016, "frames": 2, "repeated_frames": 1, "marker": 100}]
    planned, source_fps_limited, unplanned, missing = classify_repeat_clusters(
        low_fps_clusters, low_fps_manifest, stalls=[], nominal_output_fps=60.0
    )
    assert not planned
    assert len(source_fps_limited) == 1
    assert not unplanned
    low_fps_stall_manifest = {**manifest, "target_fps": 45}
    low_fps_near_stall = [{"start_pts": 8.32, "end_pts": 8.34, "frames": 2, "repeated_frames": 1, "marker": 101}]
    planned, source_fps_limited, unplanned, missing = classify_repeat_clusters(
        low_fps_near_stall, low_fps_stall_manifest, nominal_output_fps=60.0
    )
    assert not planned
    assert len(source_fps_limited) == 1
    assert source_fps_limited[0]["classification"] == "planned_source_stall_boundary_repeat"
    assert not unplanned
    assert len(missing) == 1
    stall_tail_120fps = [{"start_pts": 8.32, "end_pts": 8.336, "frames": 3, "repeated_frames": 2, "marker": 102}]
    planned, source_fps_limited, unplanned, missing = classify_repeat_clusters(
        stall_tail_120fps, manifest, nominal_output_fps=120.0
    )
    assert not planned
    assert len(source_fps_limited) == 1
    assert source_fps_limited[0]["classification"] == "planned_source_stall_boundary_repeat"
    assert not unplanned
    assert len(missing) == 1
    filter_summary = {
        "timing": {"capture_to_stimulus_offset_seconds": 0.75},
        "analysis_start_seconds": 2.0,
        "source_stalls": [
            {
                "stimulus_start": 8.0,
                "stimulus_end": 8.3,
                "tolerance": 0.05,
            }
        ],
    }
    audio_filter_input = [
        {"to": 1, "time": 0.15},
        {"to": 2, "time": 1.19},
        {"to": 8, "time": 7.19},
        {"to": 9, "time": 8.19},
    ]
    filtered = filter_transitions_for_analysis(audio_filter_input, filter_summary, stimulus_time_adjust=0.075)
    assert [item["to"] for item in filtered] == [2, 9]
    deduped = dedupe_transitions_by_target_state(
        [{"to": 7, "time": 7.0}, {"to": 7, "time": 7.8}, {"to": 8, "time": 8.0}]
    )
    assert [(item["to"], item["time"]) for item in deduped] == [(7, 7.0), (8, 8.0)]
    interval_reference = use_video_transition_uncertainty_intervals(
        [{"to": 2, "time": 2.055, "display_time": 2.067, "time_mode": "source_marker_interpolated"}]
    )
    assert interval_reference[0]["time"] == 2.055
    assert interval_reference[0]["time_min"] == 2.055
    assert interval_reference[0]["time_max"] == 2.067
    assert interval_reference[0]["time_mode_for_audio"] == "source_to_visible_interval"
    assert transition_signed_offset({"time": 2.060}, interval_reference[0]) == 0.0
    assert round(transition_signed_offset({"time": 2.077}, interval_reference[0]), 3) == 0.010
    assert round(transition_signed_offset({"time": 2.050}, interval_reference[0]), 3) == -0.005

    class Args:
        max_timing_anchor_error_ms = 50.0
        max_corrupt_frames = 0
        max_longest_repeat = 1
        max_motion_stall = 5
        max_motion_error_frames = 5
        min_video_transitions = 2
        min_audio_transitions = 2
        transition_match_window_ms = 100.0
        max_missing_transition_matches = 0
        max_av_offset_ms = 10.0
        max_mean_av_offset_ms = 5.0
        max_offset_slope_ms_per_min = 5000.0
        min_offset_slope_excursion_ms = 5.0
        max_track_spread_ms = 5.0
        non_strict_audio_ordinals_set = {2}

    strict_spread_video = {
        "timing": {"anchor_error_seconds": 0.0},
        "corrupt_frames": 0,
        "longest_unplanned_marker_repeat": 0,
        "missing_planned_source_stalls": [],
        "longest_motion_stall": 0,
        "motion_error_frames": 0,
        "out_of_order_markers": 0,
        "transitions": [{"to": 1, "time": 1.0}, {"to": 2, "time": 2.0}, {"to": 3, "time": 3.0}],
    }
    strict_spread_audio = [
        {"ordinal": 0, "codec": "aac", "sample_rate": 48000, "transitions": strict_spread_video["transitions"]},
        {
            "ordinal": 1,
            "codec": "aac",
            "sample_rate": 48000,
            "transitions": [{"to": 1, "time": 1.003}, {"to": 2, "time": 2.003}, {"to": 3, "time": 3.003}],
        },
        {"ordinal": 2, "codec": "aac", "sample_rate": 48000, "transitions": []},
    ]
    strict_spread_checks = evaluate(Args(), strict_spread_video, strict_spread_audio, {}, {})
    spread_checks = [check for check in strict_spread_checks if check["name"] == "audio.inter_track_spread_ms"]
    assert spread_checks and spread_checks[0]["passed"]
    assert all("opportunistic" not in check["name"] or check["passed"] for check in strict_spread_checks)
    strict_spread_extra_audio_transition = [
        {"ordinal": 0, "codec": "aac", "sample_rate": 48000, "transitions": strict_spread_video["transitions"]},
        {
            "ordinal": 1,
            "codec": "aac",
            "sample_rate": 48000,
            "transitions": [
                {"to": 1, "time": 1.003},
                {"to": 2, "time": 2.003},
                {"to": 3, "time": 3.003},
                {"to": 4, "time": 4.003},
            ],
        },
    ]
    strict_spread_extra_checks = evaluate(Args(), strict_spread_video, strict_spread_extra_audio_transition, {}, {})
    extra_spread_checks = [
        check for check in strict_spread_extra_checks if check["name"] == "audio.inter_track_spread_ms"
    ]
    assert extra_spread_checks and extra_spread_checks[0]["passed"]
    assert extra_spread_checks[0]["actual"]["shared_events"] == 3

    frames = [
        {"pts": 0.00, "palette": 1},
        {"pts": 0.20, "palette": 1},
        {"pts": 0.52, "palette": 1},
        {"pts": 0.54, "palette": 2},
        {"pts": 0.90, "palette": 2},
        {"pts": 1.52, "palette": 2},
        {"pts": 1.54, "palette": 3},
        {"pts": 1.90, "palette": 3},
        {"pts": 2.52, "palette": 3},
        {"pts": 2.54, "palette": 4},
        {"pts": 2.90, "palette": 4},
    ]
    timing_manifest = {
        "events": [{"palette": index} for index in range(16)],
        "duration_seconds": 10,
        "event_period_seconds": 1.0,
    }
    timing = infer_capture_timing(frames, timing_manifest)
    assert abs(timing["capture_to_stimulus_offset_seconds"] - 1.46) < 0.01
    truncated_pts = [index / 60.0 for index in range(578)]
    assert abs(infer_pts_delta(truncated_pts) - (1.0 / 60.0)) < 0.0001
    assert abs(frame_pts_for_index(truncated_pts, 578, infer_pts_delta(truncated_pts)) - (578.0 / 60.0)) < 0.0001
    assert abs(parse_frame_rate_ratio("120/1") - 120.0) < 0.001
    assert abs(parse_frame_rate_ratio("60000/1001") - 59.94005994) < 0.001
    assert abs(declared_video_frame_delta({"avg_frame_rate": "120/1"}) - (1.0 / 120.0)) < 0.000001
    assert declared_video_frame_delta({"avg_frame_rate": "0/0", "r_frame_rate": ""}) == 0.0
    wrap_frames = [
        {"pts": 0.00, "palette": 0},
        {"pts": 0.20, "palette": 0},
        {"pts": 0.24, "palette": 1},
        {"pts": 0.80, "palette": 1},
        {"pts": 1.24, "palette": 2},
        {"pts": 1.80, "palette": 2},
        {"pts": 2.24, "palette": 3},
        {"pts": 2.80, "palette": 3},
    ]
    wrap_manifest = {
        "events": [{"palette": index} for index in range(16)],
        "duration_seconds": 16,
        "event_period_seconds": 1.0,
    }
    wrap_timing = infer_capture_timing(wrap_frames, wrap_manifest)
    assert 0.0 <= wrap_timing["capture_to_stimulus_offset_seconds"] < 2.0
    assert wrap_timing["anchor_error_seconds"] < 0.01
    warmup_frames = []
    for index, marker in enumerate([9, 8, 20, 21, 22]):
        warmup_frames.append(
            {
                "index": index,
                "pts": index * 0.1,
                "palette": index % 4,
                "marker": marker,
                "marker_bad_tiles": 0,
                "marker_inverse_ok": True,
                "checksum_ok": True,
                "parity_ok": True,
                "event_color_confidence": 1.0,
                "motion": None,
            }
        )
    warmup_summary = summarize_video(
        warmup_frames,
        {
            "events": [{"palette": index} for index in range(16)],
            "duration_seconds": 2.0,
            "event_period_seconds": 1.0,
            "analysis_start_seconds": 0.2,
        },
        {"capture_to_stimulus_offset_seconds": 0.0},
    )
    assert warmup_summary["out_of_order_markers"] == 0
    marker_frames = []
    marker_values = [244, 252, 260, 268, 274, 278, 286, 294, 302, 310]
    for index, marker in enumerate(marker_values):
        marker_frames.append(
            {
                "index": index,
                "pts": [0.14, 0.18, 0.22, 0.26, 0.283333, 0.300000, 0.34, 0.38, 0.42, 0.46][index],
                "palette": 0 if index < 5 else 1,
                "marker": marker,
                "marker_bad_tiles": 0,
                "marker_inverse_ok": True,
                "checksum_ok": True,
                "parity_ok": True,
                "event_color_confidence": 1.0,
                "motion": None,
            }
        )
    marker_summary = summarize_video(
        marker_frames,
        {
            "events": [{"palette": index} for index in range(16)],
            "duration_seconds": 2.0,
            "event_period_seconds": 1.0,
            "analysis_start_seconds": 0.0,
            "target_fps": 240,
        },
        {"capture_to_stimulus_offset_seconds": 0.7},
        [
            {"frame_id": 240, "stimulus_seconds": 0.857231, "kind": "frame", "order": 0},
            {"frame_id": 480, "stimulus_seconds": 1.857231, "kind": "frame", "order": 0},
        ],
    )
    assert marker_summary["marker_timing_available"]
    assert marker_summary["transitions"][0]["time_mode"] == "source_marker_interpolated"
    assert abs(marker_summary["transitions"][0]["time"] - 0.2845) < 0.002
    assert abs(marker_summary["transitions"][0]["display_time"] - 0.3000) < 0.0001
    assert abs(
        effective_marker_fps_for_extrapolation(
            {
                "target_fps": 240,
                "frame_pacing": {
                    "average_present_delta_ms": 6.944444,
                    "present_delta_count": 100,
                    "planned_source_gap_count": 0,
                },
            }
        )
        - 144.0
    ) < 0.01
    assert effective_marker_fps_for_extrapolation(
        {
            "target_fps": 240,
            "frame_pacing": {
                "average_present_delta_ms": 12.0,
                "present_delta_count": 100,
                "planned_source_gap_count": 1,
            },
        }
    ) == 240
    low_source_frames = []
    low_source_pts = [0.00, 0.04, 0.08, 0.10, 0.12, 0.14, 0.16, 0.20, 0.24, 0.28, 0.32]
    low_source_markers = [126, 129, 132, 132, 132, 132, 135, 135, 138, 138, 141]
    for index, marker in enumerate(low_source_markers):
        low_source_frames.append(
            {
                "index": index,
                "pts": low_source_pts[index],
                "palette": 1 if index < 6 else 2,
                "marker": marker,
                "marker_bad_tiles": 0,
                "marker_inverse_ok": True,
                "checksum_ok": True,
                "parity_ok": True,
                "event_color_confidence": 1.0,
                "motion": None,
            }
        )
    low_source_summary = summarize_video(
        low_source_frames,
        {
            "events": [{"palette": index} for index in range(16)],
            "duration_seconds": 3.0,
            "event_period_seconds": 1.0,
            "analysis_start_seconds": 0.0,
            "target_fps": 45,
        },
        {"capture_to_stimulus_offset_seconds": 0.0},
        [
            {"frame_id": 90, "stimulus_seconds": 1.054, "kind": "frame", "order": 0},
            {"frame_id": 135, "stimulus_seconds": 2.054, "kind": "frame", "order": 0},
        ],
    )
    assert low_source_summary["transitions"][0]["time_mode"] == "source_marker_interpolated"
    assert low_source_summary["transitions"][0]["time"] < 0.11
    assert abs(low_source_summary["transitions"][0]["display_time"] - 0.16) < 0.0001
    motion_manifest = {
        "width": 1280,
        "marker_margin": 24,
        "motion_lane_bar_width": 96,
        "motion_lane_speed_cycles_per_second": 0.25,
    }
    expected_center = expected_motion_from_stimulus(0.0, motion_manifest)
    assert abs(expected_center - ((24 + 48) / 1280.0)) < 0.001
    fast_manifest = {
        "width": 1280,
        "marker_margin": 24,
        "motion_lane_count": 2,
        "fast_motion_lane_bar_width": 48,
        "fast_motion_lane_speed_cycles_per_second": 1.0,
    }
    expected_fast_center = expected_fast_motion_from_stimulus(0.5, fast_manifest)
    assert abs(expected_fast_center - ((24 + 0.5 * (1280 - 48 - 48) + 24) / 1280.0)) < 0.001
    assert fast_motion_missing_limit({"fast_motion_available": False, "frames": 1200}, 3) == 0
    assert fast_motion_missing_limit({"fast_motion_available": True, "frames": 1343}, 3) == 21
    assert fast_motion_missing_limit({"fast_motion_available": True, "frames": 120}, 3) == 3
    print("self-test: PASS")


def main():
    parser = argparse.ArgumentParser(description="Analyze dx12_av_sync_test captures against the stimulus manifest.")
    parser.add_argument("capture", nargs="?", type=Path)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--ffmpeg", type=Path, default=Path("ffmpeg"))
    parser.add_argument("--ffprobe", type=Path, default=Path("ffprobe"))
    parser.add_argument("--ce-log", type=Path)
    parser.add_argument("--app-log", type=Path)
    parser.add_argument("--json-out", type=Path)
    parser.add_argument(
        "--video-scale-width",
        type=int,
        default=0,
        help="Decoded video width for marker analysis; 0 auto-scales so marker tiles remain readable.",
    )
    parser.add_argument("--max-av-offset-ms", type=float, default=25.0)
    parser.add_argument("--max-mean-av-offset-ms", type=float, default=15.0)
    parser.add_argument("--max-track-spread-ms", type=float, default=10.0)
    parser.add_argument("--max-offset-slope-ms-per-min", type=float, default=30.0)
    parser.add_argument(
        "--min-offset-slope-excursion-ms",
        type=float,
        default=12.0,
        help="Only fail short-run offset slope when matched offsets span more than this many milliseconds.",
    )
    parser.add_argument("--transition-match-window-ms", type=float, default=300.0)
    parser.add_argument("--min-video-transitions", type=int, default=4)
    parser.add_argument("--min-audio-transitions", type=int, default=4)
    parser.add_argument("--max-missing-transition-matches", type=int, default=1)
    parser.add_argument("--max-corrupt-frames", type=int, default=0)
    parser.add_argument("--max-timing-anchor-error-ms", type=float, default=50.0)
    parser.add_argument("--max-longest-repeat", type=int, default=2)
    parser.add_argument("--max-motion-stall", type=int, default=3)
    parser.add_argument("--max-motion-error-frames", type=int, default=3)
    parser.add_argument(
        "--non-strict-audio-ordinals",
        default="",
        help="Comma-separated audio stream ordinals to report without making them pass/fail gates.",
    )
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        self_test()
        return
    if not args.capture or not args.manifest:
        fail("capture and --manifest are required unless --self-test is used")
    args.non_strict_audio_ordinals_set = parse_ordinal_set(args.non_strict_audio_ordinals)

    manifest = load_manifest(args.manifest)
    _, video_stream, audio_streams = analyze_streams(args.ffprobe, args.capture)
    pts, _ = read_video_pts(args.ffprobe, args.capture)
    frames = decode_video(args.ffmpeg, args.capture, manifest, video_stream, pts, args.video_scale_width)
    timing = infer_capture_timing(frames, manifest)
    app_frame_anchors = load_app_frame_anchors(args.app_log) if args.app_log else []
    video_summary = summarize_video(frames, manifest, timing, app_frame_anchors)
    if "error" in video_summary:
        fail(video_summary["error"])

    frequencies = [float(event["frequency_hz"]) for event in manifest["events"]]
    audio_results = []
    detector_bias_cache = {}
    for ordinal, stream in enumerate(audio_streams):
        samples, decode_error = decode_audio_track(args.ffmpeg, args.capture, ordinal)
        sample_rate = parse_int(stream.get("sample_rate"))
        stream_start_seconds = parse_float(stream.get("start_time"), 0.0)
        initial_padding = parse_int(stream.get("initial_padding"), 0)
        transitions = []
        detector_biases = {}
        if samples is not None:
            # FFmpeg raw PCM decode is the presentation-content timeline; AAC
            # priming and Opus pre-skip are already handled by the decoder.
            # Keep stream start/padding metadata in the report for diagnostics,
            # but do not add it here or codec pre-roll gets counted twice.
            _, transitions = detect_audio_states(samples, sample_rate, frequencies)
            detector_biases = detector_bias_cache.get(sample_rate)
            if detector_biases is None:
                detector_biases = calibrate_audio_detector_biases(sample_rate, frequencies)
                detector_bias_cache[sample_rate] = detector_biases
            transitions = apply_audio_detector_bias_correction(transitions, detector_biases)
        audio_results.append(
            {
                "ordinal": ordinal,
                "stream_index": parse_int(stream.get("index")),
                "codec": stream.get("codec_name", ""),
                "sample_rate": sample_rate,
                "stream_start_seconds": stream_start_seconds,
                "initial_padding_samples": initial_padding,
                "detector_bias_corrections_ms": [
                    {
                        "from": from_index,
                        "to": to_index,
                        "bias_ms": round(bias * 1000.0, 3),
                    }
                    for (from_index, to_index), bias in sorted(detector_biases.items())
                ],
                "transitions": transitions,
                "decode_error": decode_error,
                "strict": ordinal not in args.non_strict_audio_ordinals_set,
            }
        )

    ce_counts = analyze_ce_log(args.ce_log) if args.ce_log else {}
    app_counts = analyze_app_log(args.app_log) if args.app_log else {}
    checks = evaluate(args, video_summary, audio_results, ce_counts, app_counts)
    report = {
        "capture": str(args.capture),
        "manifest": str(args.manifest),
        "video": video_summary,
        "audio": audio_results,
        "ce_log_counts": ce_counts,
        "app_log_counts": app_counts,
        "checks": checks,
        "passed": all(check["passed"] for check in checks),
    }
    print_report(report)
    if args.json_out:
        args.json_out.write_text(json.dumps(report, indent=2), encoding="utf-8")
    if not report["passed"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
