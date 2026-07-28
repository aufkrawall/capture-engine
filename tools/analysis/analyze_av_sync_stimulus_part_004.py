

def evaluate(args, video_summary, audio_results, ce_counts, app_counts):
    checks = []
    non_strict_audio = getattr(args, "non_strict_audio_ordinals_set", set())
    checks.append(
        make_check(
            "video.timing_anchor",
            parse_float(video_summary.get("timing", {}).get("anchor_error_seconds"), 999999.0) * 1000.0
            <= args.max_timing_anchor_error_ms,
            round(parse_float(video_summary.get("timing", {}).get("anchor_error_seconds"), 999999.0) * 1000.0, 3),
            f"<= {args.max_timing_anchor_error_ms} ms",
            "video_content_drift",
        )
    )
    checks.append(
        make_check(
            "video.corrupt_frames",
            video_summary["corrupt_frames"] <= args.max_corrupt_frames,
            video_summary["corrupt_frames"],
            f"<= {args.max_corrupt_frames}",
            "corrupted_video_frame",
        )
    )
    checks.append(
        make_check(
            "video.longest_marker_repeat",
            video_summary["longest_unplanned_marker_repeat"] <= args.max_longest_repeat,
            video_summary["longest_unplanned_marker_repeat"],
            f"<= {args.max_longest_repeat}",
            "unplanned_repeat_cluster",
        )
    )
    checks.append(
        make_check(
            "video.planned_source_stalls",
            len(video_summary["missing_planned_source_stalls"]) == 0,
            len(video_summary["missing_planned_source_stalls"]),
            "0 missing planned source stalls",
            "planned_source_stall",
        )
    )
    checks.append(
        make_check(
            "video.longest_motion_stall",
            video_summary["longest_motion_stall"] <= args.max_motion_stall,
            video_summary["longest_motion_stall"],
            f"<= {args.max_motion_stall}",
            "visual_judder",
        )
    )
    checks.append(
        make_check(
            "video.motion_expected_position",
            video_summary["motion_error_frames"] <= args.max_motion_error_frames,
            video_summary["motion_error_frames"],
            f"<= {args.max_motion_error_frames}",
            "visual_judder",
        )
    )
    fast_missing_limit = fast_motion_missing_limit(video_summary, args.max_motion_error_frames)
    checks.append(
        make_check(
            "video.fast_motion_decode",
            (not video_summary.get("fast_motion_available", False))
            or video_summary.get("fast_motion_missing_frames", 0) <= fast_missing_limit,
            video_summary.get("fast_motion_missing_frames", 0),
            f"<= {fast_missing_limit} isolated missing fast motion frames",
            "visual_judder",
        )
    )
    checks.append(
        make_check(
            "video.longest_fast_motion_stall",
            video_summary.get("longest_fast_motion_stall", 1) <= args.max_motion_stall,
            video_summary.get("longest_fast_motion_stall", 1),
            f"<= {args.max_motion_stall}",
            "visual_judder",
        )
    )
    checks.append(
        make_check(
            "video.fast_motion_expected_position",
            video_summary.get("fast_motion_error_frames", 0) <= args.max_motion_error_frames,
            video_summary.get("fast_motion_error_frames", 0),
            f"<= {args.max_motion_error_frames}",
            "visual_judder",
        )
    )
    checks.append(
        make_check(
            "video.out_of_order_markers",
            video_summary["out_of_order_markers"] == 0,
            video_summary["out_of_order_markers"],
            "0",
            "video_content_drift",
        )
    )
    checks.append(
        make_check(
            "video.event_transitions",
            len(video_summary["transitions"]) >= args.min_video_transitions,
            len(video_summary["transitions"]),
            f">= {args.min_video_transitions}",
            "missing_marker",
        )
    )

    audio_lead_seconds = parse_float(video_summary.get("audio_stimulus_lead_seconds"), 0.0)
    nominal_output_fps = parse_float(video_summary.get("nominal_output_fps"), 0.0)
    frame_quantization_excursion_ms = 1000.0 / nominal_output_fps if nominal_output_fps > 1.0 else 0.0
    slope_excursion_limit_ms = max(args.min_offset_slope_excursion_ms, frame_quantization_excursion_ms)
    video_transitions = filter_transitions_for_analysis(video_summary["transitions"], video_summary)
    video_audio_reference_transitions = use_video_transition_uncertainty_intervals(video_transitions)
    strict_audio_offset_point_sets = []
    for result in audio_results:
        is_strict_audio = result["ordinal"] not in non_strict_audio
        if result.get("decode_error"):
            checks.append(
                make_check(
                    f"audio.a:{result['ordinal']}.decode"
                    if is_strict_audio
                    else f"audio.a:{result['ordinal']}.opportunistic_decode",
                    not is_strict_audio,
                    result["decode_error"],
                    "no decode errors",
                    "decode_error" if is_strict_audio else "opportunistic_audio_decode",
                )
            )
            continue
        transitions = filter_transitions_for_analysis(
            result["transitions"], video_summary, stimulus_time_adjust=audio_lead_seconds
        )
        transitions_for_matching = transitions
        checks.append(
            make_check(
                f"audio.a:{result['ordinal']}.markers"
                if is_strict_audio
                else f"audio.a:{result['ordinal']}.opportunistic_markers",
                (len(transitions) >= args.min_audio_transitions) if is_strict_audio else True,
                len(transitions),
                f">= {args.min_audio_transitions}",
                "audio_marker_missing" if is_strict_audio else "opportunistic_audio_marker",
            )
        )
        offset_points, missing = match_transition_offset_points(
            video_audio_reference_transitions, transitions_for_matching, args.transition_match_window_ms / 1000.0
        )
        offsets = [point["offset_seconds"] for point in offset_points]
        offset_stats = summarize_offsets_ms(offsets)
        offset_slope_ms_per_minute = compute_offset_slope_ms_per_minute(offset_points)
        if transitions and not offsets:
            offset_stats["max_abs"] = 999999.0
        offset_stats["missing"] = missing
        offset_stats["slope_ms_per_minute"] = offset_slope_ms_per_minute
        offset_stats["target_signed_ms"] = 0.0
        result["av_offset_stats_ms"] = {
            key: round(value, 3) if isinstance(value, float) else value for key, value in offset_stats.items()
        }
        if is_strict_audio:
            strict_audio_offset_point_sets.append((result["ordinal"], offset_points))
        max_offset_ms = offset_stats["max_abs"]
        checks.append(
            make_check(
                f"audio.a:{result['ordinal']}.av_offset_ms"
                if is_strict_audio
                else f"audio.a:{result['ordinal']}.opportunistic_av_offset_ms",
                (offsets and missing <= args.max_missing_transition_matches and max_offset_ms <= args.max_av_offset_ms)
                if is_strict_audio
                else True,
                round(max_offset_ms, 3),
                f"<= {args.max_av_offset_ms} ms",
                "audio_video_event_offset" if is_strict_audio else "opportunistic_audio_video_event_offset",
            )
        )
        checks.append(
            make_check(
                f"audio.a:{result['ordinal']}.av_mean_offset_ms"
                if is_strict_audio
                else f"audio.a:{result['ordinal']}.opportunistic_av_mean_offset_ms",
                (offsets and abs(offset_stats["mean_signed"]) <= args.max_mean_av_offset_ms)
                if is_strict_audio
                else True,
                round(offset_stats["mean_signed"], 3),
                f"0 +/- {args.max_mean_av_offset_ms} ms",
                "audio_video_event_offset" if is_strict_audio else "opportunistic_audio_video_event_offset",
            )
        )
        checks.append(
            make_check(
                f"audio.a:{result['ordinal']}.offset_slope_ms_per_min"
                if is_strict_audio
                else f"audio.a:{result['ordinal']}.opportunistic_offset_slope_ms_per_min",
                offset_slope_is_acceptable(
                    offsets,
                    offset_stats,
                    offset_slope_ms_per_minute,
                    args.max_offset_slope_ms_per_min,
                    args.min_offset_slope_excursion_ms,
                    frame_quantization_excursion_ms,
                    args.max_av_offset_ms,
                    args.max_mean_av_offset_ms,
                )
                if is_strict_audio
                else True,
                {
                    "slope_ms_per_minute": round(offset_slope_ms_per_minute, 3),
                    "span_ms": round(offset_stats["span"], 3),
                },
                "0 +/- {slope} ms/min, or span <= {span:.3f} ms with max/mean offset guards".format(
                    slope=args.max_offset_slope_ms_per_min,
                    span=slope_excursion_limit_ms,
                ),
                "audio_video_offset_slope" if is_strict_audio else "opportunistic_audio_video_offset_slope",
            )
        )

    if len(strict_audio_offset_point_sets) >= 2:
        reference_ordinal, reference_points = strict_audio_offset_point_sets[0]
        reference_offsets = {
            (point.get("to"), round(parse_float(point.get("reference_time"), 0.0), 6)): point["offset_seconds"]
            for point in reference_points
        }
        max_spread_ms = 0.0
        shared_events = 0
        missing_shared_events = 0
        for ordinal, points in strict_audio_offset_point_sets[1:]:
            candidate_offsets = {
                (point.get("to"), round(parse_float(point.get("reference_time"), 0.0), 6)): point["offset_seconds"]
                for point in points
            }
            shared_keys = sorted(set(reference_offsets.keys()) & set(candidate_offsets.keys()))
            shared_events += len(shared_keys)
            missing_shared_events += max(0, len(reference_offsets) - len(shared_keys))
            for key in shared_keys:
                max_spread_ms = max(max_spread_ms, abs(candidate_offsets[key] - reference_offsets[key]) * 1000.0)
        checks.append(
            make_check(
                "audio.inter_track_spread_ms",
                shared_events > 0 and max_spread_ms <= args.max_track_spread_ms,
                {
                    "max_spread_ms": round(max_spread_ms, 3),
                    "shared_events": shared_events,
                    "missing_shared_events": missing_shared_events,
                },
                f"<= {args.max_track_spread_ms} ms across shared video-matched events",
                "inter_track_spread",
            )
        )

    strict_audio_content_failure_classes = {
        "audio_marker_missing",
        "audio_video_event_offset",
        "audio_video_offset_slope",
        "decode_error",
        "inter_track_spread",
    }
    strict_audio_content_ok = not any(
        (not check["passed"]) and check["failure_class"] in strict_audio_content_failure_classes for check in checks
    )
    for name, count in sorted(ce_counts.items()):
        if name == "audio_stop_force_drain_backlog":
            checks.append(
                make_check(
                    f"ce_log.{name}",
                    True,
                    count,
                    "diagnostic-only post-target backlog",
                    "ce_strict_log_event",
                )
            )
        elif name in {"audio_strict_source_padding", "audio_stop_tail_padding"}:
            checks.append(
                make_check(
                    f"ce_log.{name}",
                    count == 0 or strict_audio_content_ok,
                    count,
                    "0, or decoded strict audio markers/spread remain clean",
                    "ce_strict_log_event",
                )
            )
        else:
            checks.append(make_check(f"ce_log.{name}", count == 0, count, "0", "ce_strict_log_event"))
    for name, count in sorted(app_counts.items()):
        if name == "frame_pacing_spike":
            visible_video_ok = (
                video_summary["longest_unplanned_marker_repeat"] <= args.max_longest_repeat
                and video_summary["longest_motion_stall"] <= args.max_motion_stall
                and video_summary["motion_error_frames"] <= args.max_motion_error_frames
                and video_summary.get("longest_fast_motion_stall", 1) <= args.max_motion_stall
                and video_summary.get("fast_motion_error_frames", 0) <= args.max_motion_error_frames
            )
            checks.append(
                make_check(
                    f"app_log.{name}",
                    count == 0 or visible_video_ok,
                    count,
                    "0, or source-only when decoded video remains smooth",
                    "stimulus_app_pacing_spike",
                )
            )
        else:
            checks.append(make_check(f"app_log.{name}", count == 0, count, "0", "stimulus_app_fault"))

    return checks


def print_report(report):
    print("avsync_stimulus:")
    print(f"  capture={report['capture']}")
    print(f"  manifest={report['manifest']}")
    timing = report["video"].get("timing", {})
    print(
        "  timing capture_to_stimulus_offset={offset:.6f}s anchor_error={error:.6f}s matches={matches}".format(
            offset=parse_float(timing.get("capture_to_stimulus_offset_seconds"), 0.0),
            error=parse_float(timing.get("anchor_error_seconds"), 0.0),
            matches=parse_int(timing.get("anchor_transition_matches"), 0),
        )
    )
    print(
        "  video frames={frames} corrupt={corrupt} repeats={repeat} longest_unplanned_repeat={longest} "
        "planned_stalls={planned} source_fps_clusters={source_fps} unplanned_clusters={unplanned} "
        "motion_stall={motion} fast_motion_stall={fast_motion}".format(
            frames=report["video"]["frames"],
            corrupt=report["video"]["corrupt_frames"],
            repeat=report["video"]["repeated_marker_frames"],
            longest=report["video"]["longest_unplanned_marker_repeat"],
            planned=len(report["video"].get("planned_source_stall_clusters", [])),
            source_fps=len(report["video"].get("source_fps_limited_repeat_clusters", [])),
            unplanned=len(report["video"].get("unplanned_repeat_clusters", [])),
            motion=report["video"]["longest_motion_stall"],
            fast_motion=report["video"].get("longest_fast_motion_stall", 1),
        )
    )
    for audio in report["audio"]:
        stats = audio.get("av_offset_stats_ms", {})
        print(
            "  audio a:{ordinal} codec={codec} rate={rate} transitions={transitions} decode_error={decode_error} "
            "offset_mean={mean}ms offset_max_abs={max_abs}ms offset_span={span}ms offset_slope={slope}ms/min".format(
                ordinal=audio["ordinal"],
                codec=audio["codec"],
                rate=audio["sample_rate"],
                transitions=len(audio.get("transitions", [])),
                decode_error="yes" if audio.get("decode_error") else "no",
                mean=stats.get("mean_signed", "n/a"),
                max_abs=stats.get("max_abs", "n/a"),
                span=stats.get("span", "n/a"),
                slope=stats.get("slope_ms_per_minute", "n/a"),
            )
        )
    print("checks:")
    for check in report["checks"]:
        status = "PASS" if check["passed"] else "FAIL"
        print(
            f"  {status} {check['name']}: actual={check['actual']} expected={check['expected']} "
            f"class={check['failure_class']}"
        )
    print(f"summary: passed={report['passed']}")
