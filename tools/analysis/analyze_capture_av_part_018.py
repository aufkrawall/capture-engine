

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
                analyze_completed_capture(
                    args.ffprobe,
                    args.ffmpeg,
                    effective_capture,
                    args.full_scan,
                    args.tail_threshold,
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
    window_duration = max(
        0.0, min(args.window_seconds, format_duration if format_duration > 0.0 else args.window_seconds)
    )
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
                    "actual": (
                        f"returncode={result['returncode']} "
                        f"stderr={'empty' if result['stderr'] == '' else 'nonempty'}"
                    ),
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
