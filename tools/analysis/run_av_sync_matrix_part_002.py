

def overload_requirements(report, min_shortfall_ms):
    evidence = (report or {}).get("evidence", {})
    counts = evidence.get("log_counts", {})
    perf_csv = evidence.get("perf_csv", [])
    smoothness = evidence.get("wgc_smoothness_summary", [])
    cadence_events = evidence.get("wgc_cadence_events", [])
    encoder_limited_drops = counts.get("wgc_encoder_limited_source_drop", 0) + sum(
        int(item.get("encoder_limited_drops", 0)) for item in smoothness
    )
    max_shortfall_ms = max(
        [float(item.get("shortfall_max_ms", 0.0)) for item in smoothness]
        + [parse_cadence_shortfall_ms(item.get("shortfall")) for item in cadence_events],
        default=0.0,
    )
    encoder_pressure = (
        counts.get("wgc_output_limited", 0) > 0
        or any(item.get("overload_rows", 0) > 0 for item in perf_csv)
        or max_shortfall_ms >= min_shortfall_ms
        or encoder_limited_drops > 0
    )
    wgc_overload_flags = (
        any(item.get("overload_rows", 0) > 0 for item in perf_csv)
        or any(str(item.get("overload", "0")).lower() not in ("0", "0x0") for item in cadence_events)
        or counts.get("wgc_output_limited", 0) > 0
    )
    encoder_limited_cadence = (
        counts.get("wgc_encoder_limited_source_drop", 0) > 0
        or any(str(item.get("mode", "")).lower() == "encoder_limited" for item in cadence_events)
        or any(item.get("encoder_limited_drops", 0) > 0 for item in smoothness)
    )
    shortfall_or_drop_pressure = max_shortfall_ms >= min_shortfall_ms or encoder_limited_drops > 0
    met = encoder_pressure and wgc_overload_flags and shortfall_or_drop_pressure and encoder_limited_cadence
    return {
        "required": True,
        "met": met,
        "encoder_pressure": encoder_pressure,
        "wgc_overload_flags": wgc_overload_flags,
        "max_shortfall_ms": max_shortfall_ms,
        "min_shortfall_ms": min_shortfall_ms,
        "encoder_limited_drops": encoder_limited_drops,
        "shortfall_or_drop_pressure": shortfall_or_drop_pressure,
        "encoder_limited_cadence": encoder_limited_cadence,
    }


def strict_audio_mean_offsets_by_ordinal_ms(analyzer_report):
    offsets = {}
    if not isinstance(analyzer_report, dict):
        return offsets
    for audio in analyzer_report.get("audio", []):
        if not audio.get("strict", False):
            continue
        stats = audio.get("av_offset_stats_ms", {})
        try:
            matched = int(stats.get("matched", 0))
            mean_signed = float(stats.get("mean_signed", 0.0))
            ordinal = int(audio.get("ordinal", -1))
        except (TypeError, ValueError):
            continue
        if matched > 0 and ordinal >= 0:
            offsets[ordinal] = mean_signed
    return offsets


def strict_audio_mean_offsets_ms(analyzer_report):
    return list(strict_audio_mean_offsets_by_ordinal_ms(analyzer_report).values())


def derive_sync_smoothness_latency_ms(analyzer_report):
    offsets_by_ordinal = strict_audio_mean_offsets_by_ordinal_ms(analyzer_report)
    return derive_sync_smoothness_latency_from_ordinals([offsets_by_ordinal])


def derive_sync_smoothness_latency_from_ordinals(offset_shots_by_ordinal):
    values_by_ordinal = {}
    for offsets_by_ordinal in offset_shots_by_ordinal:
        for ordinal, offset in offsets_by_ordinal.items():
            values_by_ordinal.setdefault(int(ordinal), []).append(float(offset))
    offsets_by_ordinal = {
        ordinal: statistics.median(values) for ordinal, values in sorted(values_by_ordinal.items())
    }
    offsets = list(offsets_by_ordinal.values())
    if not offsets:
        fail("sync-smoothness preflight could not measure strict audio/video offsets")
    median_offset = statistics.median(offsets)
    spread = max(offsets) - min(offsets) if len(offsets) > 1 else 0.0
    if any(abs(offset) > 500.0 for offset in offsets):
        fail(
            "sync-smoothness preflight offset is implausible: "
            + ",".join(f"a:{ordinal}={offset:.3f}ms" for ordinal, offset in sorted(offsets_by_ordinal.items()))
        )
    if any(offset < -SYNC_SMOOTHNESS_MAX_MEAN_OFFSET_MS for offset in offsets):
        fail(
            "sync-smoothness preflight measured audio early by "
            + ",".join(f"a:{ordinal}={offset:.3f}ms" for ordinal, offset in sorted(offsets_by_ordinal.items()))
            + "; video-delay-only correction cannot fix this safely"
        )
    system_latency_ms = max(0.0, offsets_by_ordinal.get(0, median_offset))
    app_latency_ms = max(0.0, offsets_by_ordinal.get(1, system_latency_ms))
    return {
        "strict_track_mean_offsets_ms": [round(value, 3) for value in offsets],
        "strict_track_mean_offsets_by_ordinal_ms": {
            str(ordinal): round(value, 3) for ordinal, value in sorted(offsets_by_ordinal.items())
        },
        "strict_track_spread_ms": round(spread, 3),
        "derived_latency_ms": round(max(system_latency_ms, app_latency_ms), 3),
        "system_latency_ms": round(system_latency_ms, 3),
        "app_latency_ms": round(app_latency_ms, 3),
        "preflight_shot_count": len(offset_shots_by_ordinal),
        "preflight_shot_offsets_by_ordinal_ms": {
            str(ordinal): [round(value, 3) for value in values]
            for ordinal, values in sorted(values_by_ordinal.items())
        },
    }


def sync_smoothness_retry_offsets_from_report(analyzer_report):
    allowed_failure_classes = {"audio_video_event_offset", "inter_track_spread", "ce_strict_log_event"}
    failed_checks = [check for check in analyzer_report.get("checks", []) if not check.get("passed")]
    if not failed_checks:
        return None
    if any(check.get("failure_class") not in allowed_failure_classes for check in failed_checks):
        return None
    offsets_by_ordinal = strict_audio_mean_offsets_by_ordinal_ms(analyzer_report)
    if not offsets_by_ordinal:
        return None
    if any(abs(offset) > SYNC_SMOOTHNESS_RETRY_MAX_RESIDUAL_MS for offset in offsets_by_ordinal.values()):
        return None
    return offsets_by_ordinal


def _audio_check_ordinal(check_name):
    parts = str(check_name).split(".")
    if len(parts) < 2 or not parts[1].startswith("a:"):
        return None
    try:
        return int(parts[1].split(":", 1)[1])
    except (TypeError, ValueError):
        return None


def sync_smoothness_retry_corrections_ms(analyzer_report):
    offsets_by_ordinal = sync_smoothness_retry_offsets_from_report(analyzer_report)
    if not offsets_by_ordinal:
        return None

    failed_checks = [check for check in analyzer_report.get("checks", []) if not check.get("passed")]
    mean_failed_ordinals = set()
    max_failed_ordinals = set()
    for check in failed_checks:
        name = str(check.get("name", ""))
        ordinal = _audio_check_ordinal(name)
        if ordinal is None:
            continue
        if name.endswith(".av_mean_offset_ms"):
            mean_failed_ordinals.add(ordinal)
        elif name.endswith(".av_offset_ms"):
            max_failed_ordinals.add(ordinal)

    corrections = {}
    for ordinal in sorted(mean_failed_ordinals | max_failed_ordinals):
        offset = offsets_by_ordinal.get(ordinal)
        if offset is None:
            continue
        if ordinal in mean_failed_ordinals and abs(offset) > SYNC_SMOOTHNESS_MAX_MEAN_OFFSET_MS:
            excess = abs(offset) - SYNC_SMOOTHNESS_MAX_MEAN_OFFSET_MS + SYNC_SMOOTHNESS_RETRY_GUARD_MS
            step = max(1.0, excess)
        else:
            step = abs(offset)
        step = min(step, abs(offset), SYNC_SMOOTHNESS_RETRY_MAX_MEAN_STEP_MS)
        if step > 0.0:
            corrections[ordinal] = math.copysign(step, offset)

    if corrections:
        return corrections

    spread_failed = [check for check in failed_checks if check.get("name") == "audio.inter_track_spread_ms"]
    if not spread_failed or len(offsets_by_ordinal) < 2:
        return None

    actual = spread_failed[0].get("actual", {})
    try:
        spread_ms = float(actual.get("max_spread_ms", 0.0))
    except (AttributeError, TypeError, ValueError):
        spread_ms = max(offsets_by_ordinal.values()) - min(offsets_by_ordinal.values())
    excess_ms = spread_ms - SYNC_SMOOTHNESS_MAX_TRACK_SPREAD_MS + SYNC_SMOOTHNESS_RETRY_GUARD_MS
    if excess_ms <= 0.0:
        return None

    positive_offsets = {ordinal: offset for ordinal, offset in offsets_by_ordinal.items() if offset > 0.0}
    negative_offsets = {ordinal: offset for ordinal, offset in offsets_by_ordinal.items() if offset < 0.0}
    if positive_offsets:
        ordinal, offset = max(positive_offsets.items(), key=lambda item: item[1])
    elif negative_offsets:
        ordinal, offset = min(negative_offsets.items(), key=lambda item: item[1])
    else:
        return None

    step = min(excess_ms, abs(offset), SYNC_SMOOTHNESS_RETRY_MAX_SPREAD_STEP_MS)
    return {ordinal: math.copysign(step, offset)} if step > 0.0 else None


def sync_smoothness_retry_offsets_ms(result):
    if result.get("analyzer_exit_code") == 0:
        return None
    analyzer_path = result.get("paths", {}).get("analyzer_json")
    analyzer_report = load_json_file(Path(analyzer_path)) if analyzer_path else None
    if not analyzer_report:
        return None
    return sync_smoothness_retry_offsets_from_report(analyzer_report)


def sync_smoothness_retry_corrections_from_result(result):
    if result.get("analyzer_exit_code") == 0:
        return None
    analyzer_path = result.get("paths", {}).get("analyzer_json")
    analyzer_report = load_json_file(Path(analyzer_path)) if analyzer_path else None
    if not analyzer_report:
        return None
    return sync_smoothness_retry_corrections_ms(analyzer_report)


def clamp_sync_smoothness_latency_ms(value):
    return round(min(500.0, max(0.0, float(value))), 3)


def run_scenario(args, scenario, run_root, ce_exe, app_exe, preflight_info=None):
    scenario_dir = run_root / scenario.name
    captures_dir = scenario_dir / "captures"
    analyzer_json = scenario_dir / "analyzer.json"
    analyzer_stdout = scenario_dir / "analyzer_stdout.txt"
    triage_json = scenario_dir / "triage.json"
    triage_stdout = scenario_dir / "triage_stdout.txt"
    scenario_report_path = scenario_dir / "scenario_report.json"
    scenario_dir.mkdir(parents=True, exist_ok=True)

    print(f"running {scenario.name}")
    taskkill_processes()
    remove_stale_app_artifacts()
    secondary_app_exe = prepare_secondary_app_alias(app_exe) if scenario.secondary_app_audio else None
    write_scenario_config(scenario, captures_dir, args.include_microphone, args.include_mixed_track,
                          args.video_encoder, args.audio_capture_latency_ms, args.app_capture_latency_ms,
                          getattr(args, "wgc_smoothness_floor_ms", None))

    delay_ms = args.delay_ms
    scenario_duration_sec = scenario.duration_sec if scenario.duration_sec is not None else args.duration_sec
    scenario_app_fps_arg = scenario.app_fps if scenario.app_fps is not None else args.app_fps
    scenario_gpu_load = scenario.gpu_load if scenario.gpu_load is not None else args.gpu_load
    scenario_width = scenario.width if scenario.width is not None else args.width
    scenario_height = scenario.height if scenario.height is not None else args.height
    audio_layout = resolve_audio_layout(scenario, args.include_mixed_track)
    duration_ms = scenario_duration_sec * 1000
    app_duration = scenario_duration_sec + max(4, delay_ms // 1000 + 2)
    app_fps = resolve_app_fps(scenario_app_fps_arg, scenario.capture_method, scenario.fps)
    app_audio_lead_ms = resolve_app_audio_lead_ms(
        args.app_audio_lead_ms, scenario.capture_method, scenario.fps, app_fps
    )
    launch = [
        ce_exe,
        f"--auto-record={delay_ms},{duration_ms}",
        "--launch",
        app_exe,
        "--width",
        scenario_width,
        "--height",
        scenario_height,
        "--fps",
        app_fps,
        "--duration",
        app_duration,
        "--gpu-load",
        scenario_gpu_load,
        "--vsync",
        0,
        "--fullscreen",
        args.fullscreen,
        "--window-chrome",
        args.window_chrome,
        "--topmost",
        1,
        "--no-allow-tearing",
        "--audio-buffer-ms",
        args.app_audio_buffer_ms,
        "--audio-lead-ms",
        app_audio_lead_ms,
        "--analysis-start-sec",
        args.analysis_start_sec,
    ]
    if scenario.encoder_stress_scene or args.encoder_stress_scene:
        launch.append("--encoder-stress-scene")
    if args.app_audio_clock_scheduling:
        launch.append("--audio-clock-scheduling")
    if args.allow_tearing:
        launch.remove("--no-allow-tearing")
        launch.append("--allow-tearing")
    include_source_stall = args.include_source_stall or scenario.include_source_stall
    source_stall_text = scenario.source_stall or args.source_stall
    if include_source_stall:
        for source_stall in split_csv(source_stall_text):
            launch.extend(["--source-stall", source_stall])

    secondary_launch = None
    secondary_launch_delay = 0.0
    if secondary_app_exe:
        secondary_duration = max(4, scenario_duration_sec - int(scenario.secondary_app_start_sec) + 3)
        secondary_launch = [
            secondary_app_exe,
            "--width",
            min(640, scenario_width),
            "--height",
            min(360, scenario_height),
            "--fps",
            app_fps,
            "--duration",
            secondary_duration,
            "--gpu-load",
            0,
            "--vsync",
            0,
            "--fullscreen",
            0,
            "--window-chrome",
            0,
            "--topmost",
            0,
            "--no-allow-tearing",
            "--audio-buffer-ms",
            args.app_audio_buffer_ms,
            "--audio-lead-ms",
            app_audio_lead_ms,
            "--analysis-start-sec",
            args.analysis_start_sec,
        ]
        if args.app_audio_clock_scheduling:
            secondary_launch.append("--audio-clock-scheduling")
        secondary_launch_delay = delay_ms / 1000.0 + max(0.0, scenario.secondary_app_start_sec)

    start_unix = time.time()
    contention_workers = start_cpu_contention_workers(getattr(args, "contention_workers", 0))
    try:
        return_code, elapsed, timed_out = run_process(
            launch,
            timeout=scenario_duration_sec + delay_ms / 1000.0 + 30.0,
            secondary_command=secondary_launch,
            secondary_delay_sec=secondary_launch_delay,
        )
    finally:
        stop_cpu_contention_workers(contention_workers)
    app_exited = wait_for_process_exit(PROCESS_NAME, args.app_exit_timeout_sec)
    secondary_app_exited = True
    if scenario.secondary_app_audio:
        secondary_app_exited = wait_for_process_exit(SECONDARY_PROCESS_NAME, args.app_exit_timeout_sec)
    if not app_exited or not secondary_app_exited:
        taskkill_processes()
    time.sleep(0.5)

    manifest = newest_existing(generated_app_paths("dx12_av_sync_test_manifest.json"), start_unix)
    app_log = newest_existing(generated_app_paths("dx12_av_sync_test.log"), start_unix)
    manifest_snapshot = snapshot_artifact(manifest, scenario_dir / "dx12_av_sync_test_manifest.json")
    app_log_snapshot = snapshot_artifact(app_log, scenario_dir / "dx12_av_sync_test.log")
    secondary_manifest = newest_existing(generated_secondary_app_paths("dx12_av_sync_test_manifest.json"), start_unix)
    secondary_app_log = newest_existing(generated_secondary_app_paths("dx12_av_sync_test.log"), start_unix)
    secondary_manifest_snapshot = snapshot_artifact(
        secondary_manifest, scenario_dir / "dx12_av_sync_late_manifest.json"
    )
    secondary_app_log_snapshot = snapshot_artifact(secondary_app_log, scenario_dir / "dx12_av_sync_late.log")
    capture = find_latest_capture(captures_dir, start_unix)
    session_dir = find_latest_run_log_dir(start_unix)
    legacy_media_log = (session_dir / "media.log") if session_dir else CAPTURE_BIN / "logs" / "media.log"
    log_snapshot_dir, media_log_snapshots, hook_log_snapshots, perf_csv_snapshots, session_manifest_snapshot = (
        snapshot_session_logs(session_dir, scenario_dir / "ce_logs")
    )
    media_log_ambiguous = len(media_log_snapshots) > 1
    media_log_snapshot = media_log_snapshots[0] if len(media_log_snapshots) == 1 else None
    analysis_session_dir = log_snapshot_dir if log_snapshot_dir else session_dir
    analysis_media_log = media_log_snapshot if media_log_snapshot else legacy_media_log
    media_text = analysis_media_log.read_text(encoding="utf-8", errors="replace") if analysis_media_log.exists() else ""
    hags_enabled_evidence = bool(re.search(r"hagsEnabled=1\b", media_text, re.IGNORECASE))

    result = {
        "scenario": {
            "capture_method": scenario.capture_method,
            "audio_codec": scenario.audio_codec,
            "fps": scenario.fps,
            "label": scenario.label,
            "profile": args.profile,
            "duration_sec": scenario_duration_sec,
            "app_fps": app_fps,
            "audio_layout": audio_layout,
            "gpu_load": scenario_gpu_load,
            "width": scenario_width,
            "height": scenario_height,
            "encoder_stress_scene": scenario.encoder_stress_scene or args.encoder_stress_scene,
            "nvenc_preset": scenario.nvenc_preset,
            "rate_control": scenario.rate_control,
            "bitrate": scenario.bitrate,
            "max_bitrate": scenario.max_bitrate,
            "app_audio_clock_scheduling": args.app_audio_clock_scheduling,
            "app_audio_buffer_ms": args.app_audio_buffer_ms,
            "app_audio_lead_ms": app_audio_lead_ms,
            "audio_capture_latency_ms": args.audio_capture_latency_ms,
            "app_capture_latency_ms": args.app_capture_latency_ms,
            "sync_smoothness_latency_mode": getattr(args, "sync_smoothness_latency_mode", ""),
            "analysis_start_sec": args.analysis_start_sec,
            "max_motion_error_frames": args.max_motion_error_frames,
            "microphone_enabled": args.include_microphone,
            "mixed_track_enabled": args.include_mixed_track,
            "external_system_audio": args.external_system_audio,
            "allow_tearing": args.allow_tearing,
            "source_stall": source_stall_text if include_source_stall else None,
            "secondary_app_audio": scenario.secondary_app_audio,
            "secondary_app_start_sec": scenario.secondary_app_start_sec if scenario.secondary_app_audio else None,
            "bit_depth": scenario.bit_depth,
            "contention_workers": getattr(args, "contention_workers", 0),
            "hags_enabled_evidence": hags_enabled_evidence,
        },
        "process": {
            "return_code": return_code,
            "elapsed_seconds": round(elapsed, 3),
            "timed_out": timed_out,
            "stimulus_app_exited": app_exited,
            "secondary_stimulus_app_exited": secondary_app_exited,
        },
        "paths": {
            "capture_file": str(capture) if capture else None,
            "ce_session_dir": str(analysis_session_dir) if analysis_session_dir else None,
            "ce_session_dir_original": str(session_dir) if session_dir else None,
            "media_log": str(analysis_media_log) if analysis_media_log and analysis_media_log.exists() else None,
            "media_logs": [str(path) for path in media_log_snapshots],
            "hook_logs": hook_log_snapshots if hook_log_snapshots else list_hook_logs(session_dir),
            "perf_csv": perf_csv_snapshots,
            "session_manifest": str(session_manifest_snapshot) if session_manifest_snapshot else None,
            "app_log": str(app_log_snapshot) if app_log_snapshot else (str(app_log) if app_log else None),
            "manifest": str(manifest_snapshot) if manifest_snapshot else (str(manifest) if manifest else None),
            "secondary_app_log": str(secondary_app_log_snapshot) if secondary_app_log_snapshot else None,
            "secondary_manifest": str(secondary_manifest_snapshot) if secondary_manifest_snapshot else None,
            "analyzer_json": str(analyzer_json),
            "analyzer_stdout": str(analyzer_stdout),
            "triage_json": str(triage_json),
            "triage_stdout": str(triage_stdout),
            "scenario_report": str(scenario_report_path),
        },
        "sync_smoothness_preflight": preflight_info,
        "analyzer_exit_code": None,
        "triage_exit_code": None,
        "passed": False,
        "inconclusive": False,
        "overload_requirements": None,
        "failure": None,
    }

    if timed_out:
        result["failure"] = "captureengine timed out"
    elif return_code != 0:
        result["failure"] = f"captureengine exited with {return_code}"
    elif not app_exited:
        result["failure"] = "stimulus app did not exit before manifest snapshot"
    elif scenario.secondary_app_audio and not secondary_app_exited:
        result["failure"] = "secondary app-audio helper did not exit"
    elif not capture:
        result["failure"] = "capture file not found"
    elif not manifest_snapshot:
        result["failure"] = "stimulus manifest not found"
    elif not app_log_snapshot:
        result["failure"] = "stimulus app log not found"
    elif media_log_ambiguous:
        result["failure"] = "multiple CE media logs found in one matrix scenario"
    elif not analysis_media_log.exists():
        result["failure"] = "CE media log not found"
    elif args.profile == "contention" and not hags_enabled_evidence:
        result["failure"] = "HAGS-enabled adapter evidence missing; contention gate requires hagsEnabled=1"
    else:
        # 0-based ffmpeg audio ordinals in strict default: a:0=Track 1 system, a:1=Track 2 app,
        # a:2=Track 4 microphone. With --include-mixed-track, a:2=Track 3 mixed and a:3=Track 4 mic.
        # Mixed and mic streams are diagnostic evidence; pure system/app are strict timing gates by default.
        # When unrelated desktop audio is known to be playing, system loopback can be downgraded for that run only.
        if audio_layout == "duplicate_app":
            non_strict_ordinals = [3] if args.include_microphone else []
            if args.external_system_audio:
                non_strict_ordinals.append(2)
        elif audio_layout == "late_secondary_app":
            non_strict_ordinals = [1, 2]
            if args.include_microphone:
                non_strict_ordinals.append(3)
        else:
            non_strict_ordinals = [2]
            if args.include_microphone and audio_layout == "mixed":
                non_strict_ordinals.append(3)
        if args.external_system_audio and audio_layout != "duplicate_app":
            non_strict_ordinals.append(0)
        non_strict_audio = ",".join(str(value) for value in sorted(set(non_strict_ordinals)))
        scenario_max_motion_error_frames = args.max_motion_error_frames
        if include_source_stall:
            scenario_max_motion_error_frames = max(scenario_max_motion_error_frames, 40)
        analyzer_cmd = [
            sys.executable,
            SCRIPT_DIR / "analyze_av_sync_stimulus.py",
            capture,
            "--manifest",
            manifest_snapshot,
            "--ffmpeg",
            args.ffmpeg,
            "--ffprobe",
            args.ffprobe,
            "--ce-log",
            analysis_media_log,
            "--app-log",
            app_log_snapshot,
            "--json-out",
            analyzer_json,
            "--min-video-transitions",
            args.min_transitions,
            "--min-audio-transitions",
            args.min_transitions,
            "--max-av-offset-ms",
            args.max_av_offset_ms,
            "--max-mean-av-offset-ms",
            args.max_mean_av_offset_ms,
            "--max-track-spread-ms",
            args.max_track_spread_ms,
            "--max-offset-slope-ms-per-min",
            args.max_offset_slope_ms_per_min,
            "--min-offset-slope-excursion-ms",
            args.min_offset_slope_excursion_ms,
            "--max-longest-repeat",
            args.max_longest_repeat,
            "--max-motion-stall",
            args.max_motion_stall,
            "--max-motion-error-frames",
            scenario_max_motion_error_frames,
            "--non-strict-audio-ordinals",
            non_strict_audio,
        ]
        analyzer_rc = run_analyzer(analyzer_cmd, analyzer_stdout)
        result["analyzer_exit_code"] = analyzer_rc
        if analysis_session_dir:
            triage_cmd = [
                sys.executable,
                SCRIPT_DIR / "analyze_capture_av.py",
                "--session-dir",
                analysis_session_dir,
                "--capture",
                capture,
                "--json-out",
                triage_json,
            ]
            result["triage_exit_code"] = run_analyzer(triage_cmd, triage_stdout)
        result["passed"] = analyzer_rc == 0 and result["triage_exit_code"] in (None, 0)
        if analyzer_rc != 0:
            result["failure"] = "analyzer failed"
        elif result["triage_exit_code"] not in (None, 0):
            result["failure"] = "triage analyzer failed"
        triage_report = load_json_file(triage_json) if result["triage_exit_code"] == 0 else None
        if result["passed"] and triage_report:
            faults = triage_report.get("faults", {})
            strict_triage_faults = [
                name
                for name in (
                    "audio_timeline",
                    "visual_timeline",
                    "wgc_encoder_overload_policy",
                    "late_app_source_backlog",
                    "started_app_source_underrun",
                    "post_mux_probe_hang",
                    "ce_process_crash",
                )
                if faults.get(name)
            ]
            if strict_triage_faults:
                result["passed"] = False
                result["failure"] = "triage strict fault: " + ",".join(strict_triage_faults)
        if args.require_overload:
            requirement = overload_requirements(triage_report, args.min_overload_shortfall_ms)
            result["overload_requirements"] = requirement
            if not requirement["met"] and result["passed"]:
                result["passed"] = False
                result["inconclusive"] = True
                result["failure"] = "required WGC encoder overload was not reached"

    scenario_report_path.write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(f"  {'PASS' if result['passed'] else 'FAIL'} report={scenario_report_path}")
    if result["failure"]:
        print(f"  failure={result['failure']}")
    return result
