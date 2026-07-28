

def run_sync_smoothness_preflight(args, scenario, run_root, ce_exe, app_exe):
    preflight_args = copy.copy(args)
    preflight_args.profile = "sync-smoothness-preflight"
    preflight_args.audio_capture_latency_ms = 0.0
    preflight_args.app_capture_latency_ms = None
    preflight_args.sync_smoothness_latency_mode = "preflight-raw"
    preflight_args.max_av_offset_ms = 500.0
    preflight_args.max_mean_av_offset_ms = 500.0
    preflight_args.max_track_spread_ms = max(args.max_track_spread_ms, 50.0)
    preflight_args.max_offset_slope_ms_per_min = max(args.max_offset_slope_ms_per_min, 120.0)
    preflight_args.min_offset_slope_excursion_ms = max(args.min_offset_slope_excursion_ms, 50.0)
    preflight_args.max_motion_error_frames = max(args.max_motion_error_frames, 12)

    shot_offsets = []
    shot_infos = []
    shot_count = max(1, int(args.sync_smoothness_preflight_shots))
    base_label = f"{scenario.label}_preflight_raw_offset" if scenario.label else "preflight_raw_offset"
    for shot_index in range(shot_count):
        preflight_scenario = copy.copy(scenario)
        preflight_scenario.label = base_label if shot_count == 1 else f"{base_label}_shot{shot_index + 1}"
        preflight_scenario.duration_sec = min(scenario.duration_sec or args.duration_sec, 12)
        preflight_scenario.include_source_stall = False
        preflight_scenario.source_stall = None

        preflight_result = run_scenario(preflight_args, preflight_scenario, run_root, ce_exe, app_exe)
        if not preflight_result["passed"]:
            preflight_result["failure"] = "sync-smoothness preflight failed: " + str(
                preflight_result.get("failure") or ""
            )
            return None, preflight_result

        analyzer_path = preflight_result.get("paths", {}).get("analyzer_json")
        analyzer_report = load_json_file(Path(analyzer_path)) if analyzer_path else None
        offsets_by_ordinal = strict_audio_mean_offsets_by_ordinal_ms(analyzer_report)
        if not offsets_by_ordinal:
            fail("sync-smoothness preflight could not measure strict audio/video offsets")
        shot_offsets.append(offsets_by_ordinal)
        shot_infos.append(
            {
                "shot": shot_index + 1,
                "preflight_scenario": preflight_scenario.name,
                "preflight_report": preflight_result.get("paths", {}).get("scenario_report"),
                "preflight_capture": preflight_result.get("paths", {}).get("capture_file"),
                "preflight_analyzer_json": analyzer_path,
                "strict_track_mean_offsets_by_ordinal_ms": {
                    str(ordinal): round(value, 3) for ordinal, value in sorted(offsets_by_ordinal.items())
                },
            }
        )

    derived = derive_sync_smoothness_latency_from_ordinals(shot_offsets)
    derived.update(
        {
            "mode": "preflight",
            "preflight_scenario": shot_infos[0]["preflight_scenario"],
            "preflight_report": shot_infos[0]["preflight_report"],
            "preflight_capture": shot_infos[0]["preflight_capture"],
            "preflight_analyzer_json": shot_infos[0]["preflight_analyzer_json"],
            "preflight_shots": shot_infos,
            "modeled_delay_ms": args.sync_smoothness_delay_ms,
        }
    )
    print(
        "  preflight derived latency={latency:.3f}ms system={system:.3f}ms app={app:.3f}ms shots={shots} "
        "from strict means={means}".format(
            latency=derived["derived_latency_ms"],
            system=derived["system_latency_ms"],
            app=derived["app_latency_ms"],
            shots=derived["preflight_shot_count"],
            means=",".join(f"{value:.3f}" for value in derived["strict_track_mean_offsets_ms"]),
        )
    )
    return derived, None


def build_matrix_scenarios(capture_methods, codecs, fps_values):
    return [Scenario(method, codec, fps) for method in capture_methods for codec in codecs for fps in fps_values]


def build_scenarios(args):
    if args.profile == "quick":
        return [
            Scenario("dxgi_dup", "alac", 60, label="quick_lossless_60", duration_sec=12),
            Scenario("dxgi_dup", "aac", 120, label="quick_lossy_120", duration_sec=12),
            Scenario("wgc", "alac", 60, label="quick_lossless_60", duration_sec=12),
            Scenario("wgc", "aac", 120, label="quick_lossy_120", duration_sec=12),
            Scenario("inject", "alac", 120, label="quick_lossless_120", duration_sec=12),
            Scenario("inject", "aac", 60, label="quick_lossy_60", duration_sec=12),
            Scenario("wgc", "alac", 60, label="late_app_lossless", duration_sec=16,
                     audio_layout="late_secondary_app", secondary_app_audio=True),
            Scenario("inject", "aac", 60, label="late_app_lossy", duration_sec=16,
                     audio_layout="late_secondary_app", secondary_app_audio=True),
        ]
    if args.profile == "late-app":
        return [
            Scenario("wgc", "alac", 60, label="late_app_lossless", duration_sec=16,
                     audio_layout="late_secondary_app", secondary_app_audio=True),
            Scenario("inject", "aac", 60, label="late_app_lossy", duration_sec=16,
                     audio_layout="late_secondary_app", secondary_app_audio=True),
        ]
    if args.profile == "raw-offset":
        # Raw (uncalibrated) capture-path A/V offset gate. Runs with --app-audio-lead-ms 0
        # (forced in parse_args unless explicitly overridden) so the measured event offset is
        # the true capture differential Delta, not Delta minus the method-aware stimulus lead.
        # This is the gate that catches a constant "audio late vs video" capture offset that the
        # calibrated profiles deliberately cancel. Codec is timeline-irrelevant for the offset,
        # but a lossless and a lossy codec are covered on both WGC and inject at 60/120.
        return [
            Scenario("wgc", "alac", 60, label="raw_offset_lossless_60", duration_sec=12),
            Scenario("wgc", "aac", 120, label="raw_offset_lossy_120", duration_sec=12),
            Scenario("inject", "alac", 120, label="raw_offset_lossless_120", duration_sec=12),
            Scenario("inject", "aac", 60, label="raw_offset_lossy_60", duration_sec=12),
        ]
    if args.profile == "codec-pass":
        return [
            Scenario(method, codec, 60, label="codec_finalize", duration_sec=10)
            for method in SUPPORTED_METHODS
            for codec in SUPPORTED_CODECS
        ]
    if args.profile == "stress":
        return [
            Scenario("wgc", "alac", 60, label="duplicate_app_fanout", duration_sec=20, audio_layout="duplicate_app"),
            Scenario("wgc", "alac", 120, label="source_fps_below_target", duration_sec=20, app_fps=45),
            Scenario("inject", "aac", 60, label="source_fps_above_target", duration_sec=20, app_fps=240),
            Scenario("wgc", "alac", 60, label="planned_source_stall", duration_sec=20,
                     include_source_stall=True, source_stall="8.0:300"),
            Scenario("wgc", "alac", 120, label="render_frame_pressure", duration_sec=20, gpu_load=200),
            Scenario("wgc", "alac", 120, label="wgc_p5_encoder_overload", duration_sec=25, app_fps=240,
                     width=1920, height=1080, gpu_load=200, nvenc_preset="p5", bitrate="160Mbps",
                     max_bitrate="240Mbps"),
            Scenario("inject", "aac", 120, label="mild_encoder_pressure", duration_sec=20, width=1920, height=1080,
                     gpu_load=150),
        ]
    if args.profile == "wgc-overload":
        return [
            Scenario("wgc", "alac", 120, label=f"wgc_overload_{preset}_4k120_entropy", duration_sec=25,
                     app_fps=240, width=3840, height=2160, gpu_load=150, encoder_stress_scene=True,
                     nvenc_preset=preset, bitrate="180Mbps", max_bitrate="260Mbps")
            for preset in ("p5", "p6", "p7")
        ]
    if args.profile == "contention":
        scenarios = []
        for run_index in range(1, 4):
            for method in SUPPORTED_METHODS:
                scenarios.append(
                    Scenario(method, "alac", 120, label=f"hags_p1_4k120_run{run_index}", duration_sec=25,
                             app_fps=240, width=3840, height=2160, gpu_load=200,
                             encoder_stress_scene=True, nvenc_preset="p1", bitrate="180Mbps",
                             max_bitrate="260Mbps", bit_depth=10)
                )
            for preset in ("p5", "p6", "p7"):
                for method in SUPPORTED_METHODS:
                    scenarios.append(
                        Scenario(method, "alac", 120,
                                 label=f"hags_{preset}_4k120_run{run_index}", duration_sec=25,
                                 app_fps=240, width=3840, height=2160, gpu_load=200,
                                 encoder_stress_scene=True, nvenc_preset=preset, bitrate="180Mbps",
                                 max_bitrate="260Mbps", bit_depth=10)
                    )
        return scenarios
    if args.profile == "sync-smoothness":
        # This profile is a product-safe stress gate for the video-delay sync model:
        # lead=0 plus a test-only modeled render-loopback latency means WGC/inject must
        # preserve near-zero content sync while handling realistic CFR smoothness pressure.
        return [
            Scenario("wgc", "alac", 120, label="active_delay_near_target", duration_sec=22, app_fps=120),
            Scenario("wgc", "aac", 60, label="active_delay_above_target_stall", duration_sec=22, app_fps=90,
                     include_source_stall=True, source_stall="8.0:220,14.0:120"),
            Scenario("wgc", "opus", 120, label="active_delay_encoder_pressure", duration_sec=22, app_fps=240,
                     width=1920, height=1080, gpu_load=180, nvenc_preset="p5", bitrate="160Mbps",
                     max_bitrate="240Mbps"),
            Scenario("inject", "flac", 120, label="active_delay_above_target", duration_sec=22, app_fps=144),
            Scenario("inject", "pcm", 60, label="active_delay_below_target_stall", duration_sec=22, app_fps=50,
                     include_source_stall=True, source_stall="8.0:220,14.0:120"),
            Scenario("inject", "aac", 120, label="active_delay_encoder_pressure", duration_sec=22,
                     width=1920, height=1080, gpu_load=150),
        ]
    if args.profile == "full":
        return build_matrix_scenarios(SUPPORTED_METHODS, SUPPORTED_CODECS, [60, 120])
    if args.profile == "long-soak":
        duration_sec = int(round(args.long_soak_minutes * 60.0))
        return [
            Scenario("wgc", "alac", 60, label="long_soak", duration_sec=duration_sec),
            Scenario("inject", "alac", 60, label="long_soak", duration_sec=duration_sec),
        ]
    methods = split_csv(args.capture_methods, SUPPORTED_METHODS)
    codecs = split_csv(args.codecs, SUPPORTED_CODECS)
    fps_values = split_int_csv(args.fps)
    return [
        Scenario(
            method,
            codec,
            fps,
            nvenc_preset=args.nvenc_preset,
            rate_control=args.rate_control,
            bitrate=args.bitrate,
            max_bitrate=args.max_bitrate,
        )
        for method in methods
        for codec in codecs
        for fps in fps_values
    ]


MATRIX_SELECTION_OPTIONS = ("--capture-methods", "--codecs", "--fps")


def explicit_matrix_selection(argv):
    if argv is None:
        argv = sys.argv[1:]
    for item in argv:
        if any(item == option or item.startswith(f"{option}=") for option in MATRIX_SELECTION_OPTIONS):
            return True
    return False


def explicit_app_audio_lead(argv):
    if argv is None:
        argv = sys.argv[1:]
    return any(item == "--app-audio-lead-ms" or item.startswith("--app-audio-lead-ms=") for item in argv)


def explicit_option(argv, option):
    if argv is None:
        argv = sys.argv[1:]
    return any(item == option or item.startswith(f"{option}=") for item in argv)


def build_parser():
    parser = argparse.ArgumentParser(description="Run deterministic A/V sync capture scenarios and analyze them.")
    parser.add_argument(
        "--profile",
        choices=[
            "quick",
            "codec-pass",
            "stress",
            "wgc-overload",
            "contention",
            "late-app",
            "raw-offset",
            "sync-smoothness",
            "full",
            "long-soak",
            "custom",
        ],
        default="quick",
        help="Scenario profile. Bare runner defaults to the fast zero-drift quick gate.",
    )
    parser.add_argument("--fast-zero-drift", dest="profile_aliases", action="append_const", const="quick")
    parser.add_argument("--codec-finalization-pass", dest="profile_aliases", action="append_const", const="codec-pass")
    parser.add_argument("--short-stress", dest="profile_aliases", action="append_const", const="stress")
    parser.add_argument("--wgc-overload-gate", dest="profile_aliases", action="append_const", const="wgc-overload")
    parser.add_argument("--contention-gate", dest="profile_aliases", action="append_const", const="contention")
    parser.add_argument("--late-app-source-gate", dest="profile_aliases", action="append_const", const="late-app")
    parser.add_argument("--raw-offset-gate", dest="profile_aliases", action="append_const", const="raw-offset")
    parser.add_argument("--sync-smoothness-gate", dest="profile_aliases", action="append_const",
                        const="sync-smoothness")
    parser.add_argument("--full-matrix", dest="profile_aliases", action="append_const", const="full")
    parser.add_argument("--long-soak", dest="profile_aliases", action="append_const", const="long-soak")
    parser.add_argument("--long-soak-minutes", type=float, default=40.0)
    parser.add_argument("--capture-methods", default="dxgi_dup,wgc,inject")
    parser.add_argument("--codecs", default="aac,alac,flac,opus,pcm")
    parser.add_argument("--fps", default="60,120")
    parser.add_argument(
        "--app-fps",
        default="auto",
        help="Stimulus render FPS. 'auto' chooses a method-specific cadence; explicit values may be below output FPS.",
    )
    parser.add_argument("--duration-sec", type=int, default=20)
    parser.add_argument("--delay-ms", type=int, default=1200)
    parser.add_argument("--app-exit-timeout-sec", type=float, default=10.0)
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=720)
    parser.add_argument("--fullscreen", type=int, choices=[0, 1], default=1)
    parser.add_argument("--window-chrome", type=int, choices=[0, 1], default=0)
    parser.add_argument("--gpu-load", type=int, default=0)
    parser.add_argument(
        "--contention-workers",
        type=int,
        default=0,
        help="Deterministic busy CPU workers active only during each capture scenario.",
    )
    parser.add_argument("--encoder-stress-scene", action="store_true")
    parser.add_argument("--nvenc-preset", default="p1")
    parser.add_argument("--rate-control", default="VBR")
    parser.add_argument("--bitrate", default="125Mbps")
    parser.add_argument("--max-bitrate", default="200Mbps")
    parser.add_argument(
        "--allow-tearing",
        action="store_true",
        help=(
            "Opt into DXGI tearing in the stimulus app for stress experiments. "
            "Default keeps visual evidence tear-free."
        ),
    )
    parser.add_argument("--video-encoder", default="av1_nvenc")
    parser.add_argument("--ffmpeg", type=Path, default=default_tool_path("ffmpeg"))
    parser.add_argument("--ffprobe", type=Path, default=default_tool_path("ffprobe"))
    parser.add_argument("--output-root", type=Path)
    parser.add_argument("--min-transitions", type=int, default=4)
    parser.add_argument("--max-av-offset-ms", type=float, default=25.0)
    parser.add_argument("--max-mean-av-offset-ms", type=float, default=15.0)
    parser.add_argument("--max-track-spread-ms", type=float, default=10.0)
    parser.add_argument("--max-offset-slope-ms-per-min", type=float, default=30.0)
    parser.add_argument("--min-offset-slope-excursion-ms", type=float, default=12.0)
    parser.add_argument("--max-longest-repeat", type=int, default=2)
    parser.add_argument("--max-motion-stall", type=int, default=3)
    parser.add_argument("--max-motion-error-frames", type=int, default=3)
    parser.add_argument("--include-source-stall", action="store_true")
    parser.add_argument("--source-stall", default="8.35:300")
    parser.add_argument("--app-audio-buffer-ms", type=int, default=20)
    parser.add_argument("--analysis-start-sec", type=float, default=2.0)
    parser.add_argument(
        "--app-audio-lead-ms",
        default="auto",
        help="Stimulus audio lead in ms, or 'auto' for method-aware WGC/inject calibration.",
    )
    parser.add_argument(
        "--audio-capture-latency-ms",
        type=float,
        default=0.0,
        help="CE-side loopback audio capture latency compensation written to the scenario config "
        "([AudioSync] audio_capture_latency_ms). Use with --raw-offset-gate to validate the fix: 0 measures "
        "the raw capture differential, the measured value drives it toward 0.",
    )
    parser.add_argument(
        "--wgc-smoothness-floor-ms",
        default=None,
        help="Override [WGC] wgc_smoothness_floor_ms in the scenario config (\"auto\", \"0\", or an "
        "explicit ms value). Pair with --audio-capture-latency-ms 0 to validate the WGC baseline "
        "jitter-buffer floor (video-only / low-confidence path): the realized delay should pin near the "
        "floor and A/V sync must stay clean (no realized-delay rubber-band, no ghost-image judder). "
        "Omitted leaves the product default (auto).",
    )
    parser.add_argument(
        "--sync-smoothness-delay-ms",
        type=float,
        default=SYNC_SMOOTHNESS_DEFAULT_DELAY_MS,
        help="Test-only modeled render-loopback latency for --sync-smoothness-gate when "
        "--audio-capture-latency-ms is not explicitly provided.",
    )
    parser.add_argument(
        "--sync-smoothness-latency-mode",
        choices=["preflight", "modeled", "manual"],
        default="preflight",
        help="For --sync-smoothness-gate without an explicit --audio-capture-latency-ms, "
        "'preflight' measures raw decoded content offset per scenario and uses that correction; "
        "'modeled' keeps the fixed --sync-smoothness-delay-ms diagnostic path; "
        "'manual' is selected automatically when --audio-capture-latency-ms is explicit.",
    )
    parser.add_argument(
        "--sync-smoothness-preflight-shots",
        type=int,
        default=3,
        help="Raw-offset preflight shots to median per strict audio source for --sync-smoothness-gate.",
    )
    parser.add_argument(
        "--app-latency-ms",
        dest="app_capture_latency_ms",
        type=float,
        default=None,
        help="Override the app source's capture_latency_ms distinctly from the global value, to exercise "
        "per-source A/V equalization (the app gets delayed by max_latency - this value).",
    )
    parser.add_argument("--no-app-audio-clock-scheduling", dest="app_audio_clock_scheduling", action="store_false")
    parser.add_argument(
        "--external-system-audio",
        action="store_true",
        help="Treat system loopback as opportunistic evidence when unrelated desktop audio is playing.",
    )
    parser.add_argument(
        "--include-mixed-track",
        action="store_true",
        help="Enable opportunistic system+app mixed track 3; strict default keeps one system and one app capture.",
    )
    parser.add_argument("--no-microphone", dest="include_microphone", action="store_false")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument(
        "--scenario-filter",
        default="",
        help="Comma-separated scenario name substrings to run. Useful for resuming one expensive profile case.",
    )
    parser.add_argument("--keep-going", action="store_true")
    parser.add_argument("--require-overload", action="store_true")
    parser.add_argument("--min-overload-shortfall-ms", type=float, default=80.0)
    parser.add_argument("--self-test", action="store_true")
    parser.set_defaults(include_microphone=True, app_audio_clock_scheduling=True)
    return parser


def parse_args(argv=None):
    parser = build_parser()
    args = parser.parse_args(argv)
    args.sync_smoothness_delay_explicit = explicit_option(argv, "--sync-smoothness-delay-ms")
    aliases = args.profile_aliases or []
    if len(set(aliases)) > 1:
        fail(f"conflicting scenario profiles requested: {', '.join(aliases)}")
    if aliases:
        args.profile = aliases[-1]
    elif args.profile == "quick" and explicit_matrix_selection(argv):
        args.profile = "custom"
    if args.profile == "wgc-overload":
        args.require_overload = True
    if args.profile == "contention" and not explicit_option(argv, "--contention-workers"):
        args.contention_workers = max(1, (os.cpu_count() or 4) // 2)
    if args.contention_workers < 0:
        fail("--contention-workers must be non-negative")
    if args.profile == "raw-offset" and not explicit_app_audio_lead(argv):
        # The raw-offset gate measures the true uncalibrated capture differential, so it must
        # not apply the method-aware stimulus lead that the other profiles use to cancel it.
        args.app_audio_lead_ms = "0"
    if args.profile == "sync-smoothness":
        if args.sync_smoothness_delay_ms < 0.0 or args.sync_smoothness_delay_ms > 500.0:
            fail("--sync-smoothness-delay-ms must be between 0 and 500")
        if args.sync_smoothness_preflight_shots < 1 or args.sync_smoothness_preflight_shots > 5:
            fail("--sync-smoothness-preflight-shots must be between 1 and 5")
        if args.sync_smoothness_latency_mode == "manual" and not explicit_option(argv, "--audio-capture-latency-ms"):
            fail("--sync-smoothness-latency-mode=manual requires --audio-capture-latency-ms")
        if not explicit_app_audio_lead(argv):
            # The active-delay gate models the post-probe product state. The stimulus must not
            # hide the offset by emitting early audio; video-delay correction must do the work.
            args.app_audio_lead_ms = "0"
        if explicit_option(argv, "--audio-capture-latency-ms"):
            args.sync_smoothness_latency_mode = "manual"
        elif args.sync_smoothness_latency_mode == "modeled":
            args.audio_capture_latency_ms = args.sync_smoothness_delay_ms
        else:
            args.audio_capture_latency_ms = 0.0
        if not explicit_option(argv, "--max-av-offset-ms"):
            args.max_av_offset_ms = SYNC_SMOOTHNESS_MAX_OFFSET_MS
        if not explicit_option(argv, "--max-mean-av-offset-ms"):
            args.max_mean_av_offset_ms = SYNC_SMOOTHNESS_MAX_MEAN_OFFSET_MS
        if not explicit_option(argv, "--max-track-spread-ms"):
            args.max_track_spread_ms = SYNC_SMOOTHNESS_MAX_TRACK_SPREAD_MS
    if args.long_soak_minutes < 30.0 or args.long_soak_minutes > 45.0:
        fail("--long-soak-minutes must be between 30 and 45")
    return args
