

def self_test():
    quick = parse_args(["--fast-zero-drift", "--dry-run"])
    quick_names = [scenario.name for scenario in build_scenarios(quick)]
    assert len(quick_names) == 8
    assert quick_names == [
        "dxgi_dup_alac_60fps_quick_lossless_60",
        "dxgi_dup_aac_120fps_quick_lossy_120",
        "wgc_alac_60fps_quick_lossless_60",
        "wgc_aac_120fps_quick_lossy_120",
        "inject_alac_120fps_quick_lossless_120",
        "inject_aac_60fps_quick_lossy_60",
        "wgc_alac_60fps_late_app_lossless",
        "inject_aac_60fps_late_app_lossy",
    ]
    assert sum(1 for scenario in build_scenarios(quick) if scenario.secondary_app_audio) == 2
    contention = parse_args(["--contention-gate", "--dry-run"])
    contention_scenarios = build_scenarios(contention)
    assert contention.profile == "contention"
    assert contention.contention_workers >= 1
    assert len(contention_scenarios) == 36
    assert {scenario.capture_method for scenario in contention_scenarios} == set(SUPPORTED_METHODS)
    assert {scenario.nvenc_preset for scenario in contention_scenarios} == {"p1", "p5", "p6", "p7"}
    assert all(scenario.width == 3840 and scenario.height == 2160 and scenario.fps == 120
               and scenario.bit_depth == 10 and scenario.encoder_stress_scene
               for scenario in contention_scenarios)
    assert resolve_app_audio_lead_ms("auto", "wgc", 60, 240) == WGC_TEAR_FREE_AUDIO_LEAD_MS
    assert resolve_app_audio_lead_ms("auto", "dxgi_dup", 120, 240) == WGC_TEAR_FREE_AUDIO_LEAD_MS
    assert resolve_app_audio_lead_ms("auto", "wgc", 120, 240) == WGC_TEAR_FREE_AUDIO_LEAD_MS
    below_target_lead = WGC_TEAR_FREE_AUDIO_LEAD_MS + WGC_BELOW_TARGET_EXTRA_AUDIO_LEAD_FRAMES * (1000.0 / 120.0)
    assert abs(resolve_app_audio_lead_ms("auto", "wgc", 120, 90) - below_target_lead) < 0.001
    assert resolve_app_audio_lead_ms("auto", "inject", 120, 144) == 50.0

    late_app = parse_args(["--late-app-source-gate", "--dry-run"])
    late_app_scenarios = build_scenarios(late_app)
    assert late_app.profile == "late-app"
    assert len(late_app_scenarios) == 2
    assert all(scenario.secondary_app_audio for scenario in late_app_scenarios)
    assert {scenario.capture_method for scenario in late_app_scenarios} == {"wgc", "inject"}

    raw_offset = parse_args(["--raw-offset-gate", "--dry-run"])
    raw_offset_scenarios = build_scenarios(raw_offset)
    assert raw_offset.profile == "raw-offset"
    # The gate must force zero stimulus lead so it measures the true capture differential.
    assert str(raw_offset.app_audio_lead_ms) == "0"
    assert len(raw_offset_scenarios) == 4
    assert {scenario.capture_method for scenario in raw_offset_scenarios} == {"wgc", "inject"}
    assert all(
        resolve_app_audio_lead_ms(
            raw_offset.app_audio_lead_ms,
            scenario.capture_method,
            scenario.fps,
            resolve_app_fps(
                scenario.app_fps if scenario.app_fps is not None else raw_offset.app_fps,
                scenario.capture_method,
                scenario.fps,
            ),
        )
        == 0.0
        for scenario in raw_offset_scenarios
    )
    # An explicit override is still honored even under the raw-offset gate.
    raw_offset_override = parse_args(["--raw-offset-gate", "--app-audio-lead-ms", "5", "--dry-run"])
    assert str(raw_offset_override.app_audio_lead_ms) == "5"

    sync_smoothness = parse_args(["--sync-smoothness-gate", "--dry-run"])
    sync_smoothness_scenarios = build_scenarios(sync_smoothness)
    assert sync_smoothness.profile == "sync-smoothness"
    assert str(sync_smoothness.app_audio_lead_ms) == "0"
    assert sync_smoothness.sync_smoothness_latency_mode == "preflight"
    assert sync_smoothness.sync_smoothness_preflight_shots == 3
    assert sync_smoothness.audio_capture_latency_ms == 0.0
    assert sync_smoothness.max_av_offset_ms == SYNC_SMOOTHNESS_MAX_OFFSET_MS
    assert sync_smoothness.max_mean_av_offset_ms == SYNC_SMOOTHNESS_MAX_MEAN_OFFSET_MS
    assert sync_smoothness.max_track_spread_ms == SYNC_SMOOTHNESS_MAX_TRACK_SPREAD_MS
    assert len(sync_smoothness_scenarios) == 6
    assert {scenario.capture_method for scenario in sync_smoothness_scenarios} == {"wgc", "inject"}
    assert {scenario.audio_codec for scenario in sync_smoothness_scenarios} == set(SUPPORTED_CODECS)
    assert {scenario.fps for scenario in sync_smoothness_scenarios} == {60, 120}
    assert any(scenario.include_source_stall for scenario in sync_smoothness_scenarios)
    assert any(int(scenario.app_fps) < scenario.fps for scenario in sync_smoothness_scenarios if scenario.app_fps)
    assert any(int(scenario.app_fps) > scenario.fps for scenario in sync_smoothness_scenarios if scenario.app_fps)
    assert any(scenario.nvenc_preset == "p5" for scenario in sync_smoothness_scenarios)
    sync_smoothness_modeled = parse_args(["--sync-smoothness-gate", "--sync-smoothness-latency-mode", "modeled",
                                          "--dry-run"])
    assert sync_smoothness_modeled.audio_capture_latency_ms == SYNC_SMOOTHNESS_DEFAULT_DELAY_MS
    modeled_wgc_latency = modeled_sync_smoothness_initial_latencies_ms(
        sync_smoothness_modeled,
        Scenario("wgc", "alac", 120, label="active_delay_near_target", app_fps=120),
    )
    assert modeled_wgc_latency["system_latency_ms"] == WGC_TEAR_FREE_AUDIO_LEAD_MS
    assert modeled_wgc_latency["app_latency_ms"] == WGC_TEAR_FREE_AUDIO_LEAD_MS
    modeled_inject_latency = modeled_sync_smoothness_initial_latencies_ms(
        sync_smoothness_modeled,
        Scenario("inject", "flac", 120, label="active_delay_above_target", app_fps=144),
    )
    assert modeled_inject_latency["system_latency_ms"] == 50.0
    sync_smoothness_modeled_explicit = parse_args(
        ["--sync-smoothness-gate", "--sync-smoothness-latency-mode", "modeled", "--sync-smoothness-delay-ms", "35",
         "--dry-run"]
    )
    modeled_explicit_latency = modeled_sync_smoothness_initial_latencies_ms(
        sync_smoothness_modeled_explicit,
        Scenario("wgc", "alac", 120, label="active_delay_near_target", app_fps=120),
    )
    assert modeled_explicit_latency["system_latency_ms"] == 35.0
    sync_smoothness_filtered = parse_args([
        "--sync-smoothness-gate", "--scenario-filter", "inject_aac_120fps_active_delay_encoder_pressure", "--dry-run"
    ])
    filtered_scenarios = [
        scenario for scenario in build_scenarios(sync_smoothness_filtered)
        if sync_smoothness_filtered.scenario_filter in scenario.name
    ]
    assert [scenario.name for scenario in filtered_scenarios] == [
        "inject_aac_120fps_active_delay_encoder_pressure"
    ]
    sync_smoothness_override = parse_args(
        [
            "--sync-smoothness-gate",
            "--audio-capture-latency-ms",
            "12",
            "--app-audio-lead-ms",
            "3",
            "--max-av-offset-ms",
            "20",
            "--dry-run",
        ]
    )
    assert sync_smoothness_override.audio_capture_latency_ms == 12.0
    assert sync_smoothness_override.sync_smoothness_latency_mode == "manual"
    assert str(sync_smoothness_override.app_audio_lead_ms) == "3"
    assert sync_smoothness_override.max_av_offset_ms == 20.0
    assert sync_smoothness_override.max_mean_av_offset_ms == SYNC_SMOOTHNESS_MAX_MEAN_OFFSET_MS
    preflight_latency = derive_sync_smoothness_latency_ms(
        {
            "audio": [
                {"ordinal": 0, "strict": True, "av_offset_stats_ms": {"matched": 8, "mean_signed": 70.1}},
                {"ordinal": 1, "strict": True, "av_offset_stats_ms": {"matched": 8, "mean_signed": 71.3}},
                {"ordinal": 2, "strict": False, "av_offset_stats_ms": {"matched": 0, "mean_signed": 0.0}},
            ]
        }
    )
    assert preflight_latency["derived_latency_ms"] == 71.3
    assert preflight_latency["system_latency_ms"] == 70.1
    assert preflight_latency["app_latency_ms"] == 71.3
    assert preflight_latency["strict_track_mean_offsets_by_ordinal_ms"] == {"0": 70.1, "1": 71.3}
    assert preflight_latency["strict_track_spread_ms"] == 1.2
    preflight_latency_multi = derive_sync_smoothness_latency_from_ordinals(
        [
            {0: 68.0, 1: 71.0},
            {0: 78.0, 1: 75.0},
            {0: 72.0, 1: 73.0},
        ]
    )
    assert preflight_latency_multi["system_latency_ms"] == 72.0
    assert preflight_latency_multi["app_latency_ms"] == 73.0
    assert preflight_latency_multi["preflight_shot_count"] == 3
    assert preflight_latency_multi["preflight_shot_offsets_by_ordinal_ms"] == {
        "0": [68.0, 78.0, 72.0],
        "1": [71.0, 75.0, 73.0],
    }
    retry_offsets = sync_smoothness_retry_offsets_from_report(
        {
            "audio": [
                {"ordinal": 0, "strict": True, "av_offset_stats_ms": {"matched": 8, "mean_signed": 6.0}},
                {"ordinal": 1, "strict": True, "av_offset_stats_ms": {"matched": 8, "mean_signed": 4.0}},
            ],
            "checks": [
                {"passed": False, "failure_class": "audio_video_event_offset"},
                {"passed": False, "failure_class": "inter_track_spread"},
            ],
        }
    )
    assert retry_offsets == {0: 6.0, 1: 4.0}
    retry_corrections = sync_smoothness_retry_corrections_ms(
        {
            "audio": [
                {"ordinal": 0, "strict": True, "av_offset_stats_ms": {"matched": 8, "mean_signed": 6.0}},
                {"ordinal": 1, "strict": True, "av_offset_stats_ms": {"matched": 8, "mean_signed": 4.0}},
            ],
            "checks": [
                {"name": "audio.a:0.av_mean_offset_ms", "passed": False,
                 "failure_class": "audio_video_event_offset"},
            ],
        }
    )
    assert retry_corrections == {0: 1.75}
    high_residual_retry_corrections = sync_smoothness_retry_corrections_ms(
        {
            "audio": [
                {"ordinal": 0, "strict": True, "av_offset_stats_ms": {"matched": 8, "mean_signed": 76.0}},
            ],
            "checks": [
                {"name": "audio.a:0.av_mean_offset_ms", "passed": False,
                 "failure_class": "audio_video_event_offset"},
            ],
        }
    )
    assert high_residual_retry_corrections == {0: SYNC_SMOOTHNESS_RETRY_MAX_MEAN_STEP_MS}
    no_retry_for_implausible_residual = sync_smoothness_retry_offsets_from_report(
        {
            "audio": [
                {"ordinal": 0, "strict": True, "av_offset_stats_ms": {"matched": 8, "mean_signed": 140.0}},
            ],
            "checks": [{"passed": False, "failure_class": "audio_video_event_offset"}],
        }
    )
    assert no_retry_for_implausible_residual is None
    spread_retry_corrections = sync_smoothness_retry_corrections_ms(
        {
            "audio": [
                {"ordinal": 0, "strict": True, "av_offset_stats_ms": {"matched": 8, "mean_signed": 1.0}},
                {"ordinal": 1, "strict": True, "av_offset_stats_ms": {"matched": 8, "mean_signed": 4.9}},
            ],
            "checks": [
                {
                    "name": "audio.inter_track_spread_ms",
                    "passed": False,
                    "failure_class": "inter_track_spread",
                    "actual": {"max_spread_ms": 5.6},
                },
            ],
        }
    )
    assert spread_retry_corrections and abs(spread_retry_corrections[1] - 1.35) < 0.001
    no_retry_for_video_fault = sync_smoothness_retry_offsets_from_report(
        {
            "audio": [{"ordinal": 0, "strict": True, "av_offset_stats_ms": {"matched": 8, "mean_signed": 6.0}}],
            "checks": [{"passed": False, "failure_class": "visual_judder"}],
        }
    )
    assert no_retry_for_video_fault is None

    codec_pass = parse_args(["--codec-finalization-pass", "--dry-run"])
    codec_scenarios = build_scenarios(codec_pass)
    assert len(codec_scenarios) == len(SUPPORTED_METHODS) * len(SUPPORTED_CODECS)
    assert {scenario.capture_method for scenario in codec_scenarios} == set(SUPPORTED_METHODS)
    assert all(scenario.fps == 60 for scenario in codec_scenarios)

    stress = parse_args(["--short-stress", "--dry-run"])
    stress_scenarios = build_scenarios(stress)
    assert len(stress_scenarios) == 7
    assert any(resolve_audio_layout(scenario, False) == "duplicate_app" for scenario in stress_scenarios)
    assert any(scenario.include_source_stall for scenario in stress_scenarios)
    assert any(str(scenario.app_fps) == "45" for scenario in stress_scenarios)
    assert any(str(scenario.app_fps) == "240" for scenario in stress_scenarios)
    assert any(scenario.capture_method == "wgc" and scenario.nvenc_preset == "p5" for scenario in stress_scenarios)

    overload = parse_args(["--wgc-overload-gate", "--dry-run"])
    overload_scenarios = build_scenarios(overload)
    assert overload.require_overload
    assert len(overload_scenarios) == 3
    assert [scenario.nvenc_preset for scenario in overload_scenarios] == ["p5", "p6", "p7"]
    assert all(scenario.capture_method == "wgc" for scenario in overload_scenarios)
    assert all(scenario.encoder_stress_scene for scenario in overload_scenarios)
    assert all(scenario.width == 3840 and scenario.height == 2160 for scenario in overload_scenarios)
    assert not overload_requirements({"evidence": {"log_counts": {}}}, 80.0)["met"]
    assert overload_requirements(
        {
            "evidence": {
                "log_counts": {"wgc_output_limited": 1, "wgc_encoder_limited_source_drop": 2},
                "perf_csv": [{"overload_rows": 5}],
                "wgc_smoothness_summary": [{"shortfall_max_ms": 120.0, "encoder_limited_drops": 2}],
                "wgc_cadence_events": [{"mode": "encoder_limited", "shortfall": "15/125.0ms", "overload": "0x1"}],
            }
        },
        80.0,
    )["met"]
    bounded_overload = overload_requirements(
        {
            "evidence": {
                "log_counts": {"wgc_output_limited": 1},
                "perf_csv": [{"overload_rows": 3}],
                "wgc_smoothness_summary": [{"shortfall_max_ms": 30.0, "encoder_limited_drops": 4}],
                "wgc_cadence_events": [{"mode": "encoder_limited", "shortfall": "4/30.0ms", "overload": "0x1"}],
            }
        },
        80.0,
    )
    assert bounded_overload["met"]
    assert bounded_overload["shortfall_or_drop_pressure"]
    assert bounded_overload["encoder_limited_drops"] == 4

    full = parse_args(["--full-matrix", "--dry-run"])
    assert len(build_scenarios(full)) == len(SUPPORTED_METHODS) * len(SUPPORTED_CODECS) * 2

    custom = parse_args(["--capture-methods", "wgc", "--codecs", "pcm", "--fps", "60", "--dry-run"])
    assert custom.profile == "custom"
    custom_scenarios = build_scenarios(custom)
    assert len(custom_scenarios) == 1
    assert custom_scenarios[0].name == "wgc_pcm_60fps"

    custom_p5 = parse_args(
        [
            "--profile",
            "custom",
            "--capture-methods",
            "wgc",
            "--codecs",
            "alac",
            "--fps",
            "120",
            "--nvenc-preset",
            "p5",
            "--bitrate",
            "160Mbps",
            "--max-bitrate",
            "240Mbps",
            "--dry-run",
        ]
    )
    custom_p5_scenarios = build_scenarios(custom_p5)
    assert custom_p5_scenarios[0].nvenc_preset == "p5"
    assert custom_p5_scenarios[0].bitrate == "160Mbps"
    assert custom_p5_scenarios[0].max_bitrate == "240Mbps"

    soak = parse_args(["--long-soak", "--long-soak-minutes", "30", "--dry-run"])
    soak_scenarios = build_scenarios(soak)
    assert len(soak_scenarios) == 2
    assert all(scenario.duration_sec == 1800 for scenario in soak_scenarios)
    print("self-test: PASS")


def main(argv=None):
    args = parse_args(argv)

    if args.self_test:
        self_test()
        return

    ce_exe, app_exe = ensure_inputs()
    scenarios = build_scenarios(args)
    if args.scenario_filter:
        filters = [item.strip().lower() for item in args.scenario_filter.split(",") if item.strip()]
        scenarios = [
            scenario for scenario in scenarios
            if any(needle in scenario.name.lower() for needle in filters)
        ]
        if not scenarios:
            fail(f"--scenario-filter matched no scenarios: {args.scenario_filter}")
    run_root = (args.output_root or (
        CAPTURE_BIN / "avsync_runs" / datetime.now().strftime("%Y%m%d_%H%M%S")
    )).resolve()

    if args.dry_run:
        print(f"run_root={run_root}")
        for scenario in scenarios:
            print(scenario.name)
        return

    snapshot = read_config_snapshot()
    results = []
    adaptive_overload = args.profile == "wgc-overload"
    try:
        for scenario in scenarios:
            scenario_args = args
            latency_info = None
            if args.profile == "sync-smoothness" and args.sync_smoothness_latency_mode == "preflight":
                latency_info, preflight_failure = run_sync_smoothness_preflight(args, scenario, run_root, ce_exe,
                                                                                app_exe)
                if preflight_failure:
                    results.append(preflight_failure)
                    if not args.keep_going:
                        break
                    continue
                scenario_args = copy.copy(args)
                scenario_args.audio_capture_latency_ms = latency_info["system_latency_ms"]
                scenario_args.app_capture_latency_ms = latency_info["app_latency_ms"]
            elif args.profile == "sync-smoothness" and args.sync_smoothness_latency_mode == "modeled":
                latency_info = modeled_sync_smoothness_initial_latencies_ms(args, scenario)
                scenario_args = copy.copy(args)
                scenario_args.audio_capture_latency_ms = latency_info["system_latency_ms"]
                scenario_args.app_capture_latency_ms = latency_info["app_latency_ms"]
            result = run_scenario(scenario_args, scenario, run_root, ce_exe, app_exe, preflight_info=latency_info)
            if (
                args.profile == "sync-smoothness"
                and args.sync_smoothness_latency_mode in ("preflight", "modeled")
                and latency_info
            ):
                retry_args = copy.copy(scenario_args)
                retry_info = copy.deepcopy(latency_info)
                for retry_attempt in range(1, SYNC_SMOOTHNESS_MAX_CONTENT_RETRIES + 1):
                    if result["passed"]:
                        break
                    retry_offsets = sync_smoothness_retry_offsets_ms(result)
                    retry_corrections = sync_smoothness_retry_corrections_from_result(result)
                    if not retry_offsets or not retry_corrections:
                        break
                    content_retry = {
                        "attempt": retry_attempt,
                        "reason": "strict decoded content residual",
                        "from_scenario": result.get("scenario", {}).get("label", scenario.name),
                        "from_report": result.get("paths", {}).get("scenario_report"),
                        "strict_track_mean_offsets_by_ordinal_ms": {
                            str(ordinal): round(value, 3) for ordinal, value in sorted(retry_offsets.items())
                        },
                        "latency_corrections_by_ordinal_ms": {
                            str(ordinal): round(value, 3) for ordinal, value in sorted(retry_corrections.items())
                        },
                    }
                    base_system_latency_ms = float(retry_args.audio_capture_latency_ms)
                    base_app_latency_ms = (
                        float(retry_args.app_capture_latency_ms)
                        if retry_args.app_capture_latency_ms is not None
                        else base_system_latency_ms
                    )
                    retry_args = copy.copy(retry_args)
                    retry_args.audio_capture_latency_ms = clamp_sync_smoothness_latency_ms(
                        base_system_latency_ms + retry_corrections.get(0, 0.0)
                    )
                    retry_args.app_capture_latency_ms = clamp_sync_smoothness_latency_ms(
                        base_app_latency_ms + retry_corrections.get(1, 0.0)
                    )
                    content_retry.update(
                        {
                            "base_system_latency_ms": round(base_system_latency_ms, 3),
                            "base_app_latency_ms": round(base_app_latency_ms, 3),
                            "retry_system_latency_ms": retry_args.audio_capture_latency_ms,
                            "retry_app_latency_ms": retry_args.app_capture_latency_ms,
                        }
                    )
                    retry_info = copy.deepcopy(retry_info)
                    retry_info.setdefault("content_retries", []).append(content_retry)
                    retry_info["content_retry"] = content_retry
                    retry_scenario = copy.copy(scenario)
                    retry_scenario.label = (
                        f"{scenario.label}_content_retry{retry_attempt}"
                        if scenario.label
                        else f"content_retry{retry_attempt}"
                    )
                    print(
                        "  content retry {attempt} system={system:.3f}ms app={app:.3f}ms "
                        "from residuals={residuals} corrections={corrections}".format(
                            attempt=retry_attempt,
                            system=retry_args.audio_capture_latency_ms,
                            app=retry_args.app_capture_latency_ms,
                            residuals=",".join(
                                f"a:{ordinal}={value:.3f}" for ordinal, value in sorted(retry_offsets.items())
                            ),
                            corrections=",".join(
                                f"a:{ordinal}={value:.3f}" for ordinal, value in sorted(retry_corrections.items())
                            ),
                        )
                    )
                    result = run_scenario(
                        retry_args, retry_scenario, run_root, ce_exe, app_exe, preflight_info=retry_info
                    )
            results.append(result)
            if adaptive_overload and result["passed"]:
                break
            if not result["passed"] and not args.keep_going:
                if adaptive_overload and result.get("inconclusive"):
                    continue
                break
    finally:
        restore_config(snapshot)
        taskkill_processes()

    matrix_report = {
        "schema": "ce-avsync-matrix-report-v1",
        "run_root": str(run_root),
        "results": results,
        "passed": bool(results) and (any(result["passed"] for result in results)
                                     if adaptive_overload else all(result["passed"] for result in results)),
    }
    run_root.mkdir(parents=True, exist_ok=True)
    matrix_report_path = run_root / "matrix_report.json"
    matrix_report_path.write_text(json.dumps(matrix_report, indent=2), encoding="utf-8")
    print(f"matrix_report={matrix_report_path}")
    if not matrix_report["passed"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
