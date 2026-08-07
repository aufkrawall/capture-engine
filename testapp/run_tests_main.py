

def print_stats(stats: Dict[str, Any]) -> None:
    """Pretty-print per-test stats."""
    if "error" in stats:
        print(f"  ERROR: {stats['error']}")
        return

    print(f"  Frames: {stats['count']}")
    if "recorded_output_frames" in stats:
        print(f"  Recorded output frames: {stats['recorded_output_frames']}")
    elif "effective_count" in stats and int(stats["effective_count"]) != int(stats["count"]):
        print(f"  Estimated total frames: {stats['effective_count']}")
    if "source" in stats:
        print(f"  Source: {stats['source']}")
    if "target_frame_time_ms" in stats:
        print(f"  Target: {int(stats['target_fps'])} FPS " f"({float(stats['target_frame_time_ms']):.2f}ms budget)")
    print(f"  Min: {float(stats['min']):.2f}ms, Max: {float(stats['max']):.2f}ms, " f"Avg: {float(stats['avg']):.2f}ms")
    print(f"  Median: {float(stats['median']):.2f}ms, " f"StdDev: {float(stats['stdev']):.2f}ms")
    print(f"  Variance (max-min): {float(stats['variance']):.2f}ms")
    print(f"  Spikes >10ms: {int(stats['spikes_10ms'])} " f"({float(stats['spike_pct_10ms']):.1f}%)")
    print(f"  Spikes >12ms: {int(stats['spikes_12ms'])} " f"({float(stats['spike_pct_12ms']):.1f}%)")
    if "spikes_2x_budget" in stats:
        print(f"  Spikes >2x budget: {int(stats['spikes_2x_budget'])} " f"({float(stats['spike_pct_2x_budget']):.1f}%)")
    if "required_frames" in stats:
        print(f"  Required frames: {int(stats['required_frames'])}")
    if int(stats["spikes_20ms"]) > 0:
        print(f"  Spikes >20ms: {int(stats['spikes_20ms'])}")


def run_single_test(
    api: str,
    arch: str,
    width: int,
    height: int,
    gpu_load: int,
    total_record_s: float,
    test_name: str,
    min_frames: int,
    target_fps: int,
    min_frame_ratio: float,
    max_avg_frame_ratio: float,
    max_frame_spike_ratio: float,
    max_spike_pct: float,
) -> Tuple[Optional[Dict[str, Any]], Optional[str]]:
    """Run a single integration test and return (stats, error_message)."""
    print(f"\n{'=' * 60}")
    print(f"TEST: {test_name}")
    print(f"  API: {api.upper()}, ARCH: {arch.upper()}, Resolution: {width}x{height}, " f"GPU Load: {gpu_load}")
    print(f"  Recording duration: {total_record_s}s, Min frames: {min_frames}")
    print("=" * 60)

    if FRAME_TIMES_CSV.exists():
        os.remove(FRAME_TIMES_CSV)

    test_start_unix_ts = time.time()

    captureengine_lead_s = 2
    app_init_s = 3
    delay_ms = int((captureengine_lead_s + app_init_s) * 1000)
    duration_ms = int(total_record_s * 1000)
    launch_via_captureengine = api in {"directdraw7", "dx7", "dx6"}

    launch_command: Optional[List[str]] = None
    if launch_via_captureengine:
        launch_command = [
            str(resolve_test_exe(api, arch)),
            str(width),
            str(height),
            str(gpu_load),
        ]

    print(f"Starting capture (delay={delay_ms}ms, record={duration_ms}ms)...")
    capture_proc = start_auto_record(delay_ms, duration_ms, launch_command)
    if not capture_proc:
        return None, "Failed to start captureengine"
    capture_start_ts = time.monotonic()

    app_proc: Optional[subprocess.Popen] = None
    if launch_via_captureengine:
        print("Starting test app via captureengine --launch...")
        time.sleep(captureengine_lead_s + app_init_s)
    else:
        print(f"Waiting {captureengine_lead_s}s before launching test app...")
        time.sleep(captureengine_lead_s)

        print("Starting test app...")
        app_proc = start_test_app(api, arch, width, height, gpu_load)
        if not app_proc:
            capture_proc.terminate()
            return None, "Failed to start test app"

        time.sleep(app_init_s)

    total_wait = (delay_ms + duration_ms) / 1000.0 + 3.0
    elapsed = time.monotonic() - capture_start_ts
    remaining_wait = max(0.0, total_wait - elapsed)
    print(f"  Waiting {remaining_wait:.0f}s for recording to complete...")
    time.sleep(remaining_wait)

    if app_proc:
        print("Stopping test app...")
        app_proc.terminate()
        try:
            app_proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            app_proc.kill()

    time.sleep(2)
    kill_processes()
    time.sleep(1)

    run_log_dir = find_latest_run_log_dir(test_start_unix_ts)
    frame_times_csv = (run_log_dir / "frame_times.csv") if run_log_dir else FRAME_TIMES_CSV
    media_log_path = resolve_media_log(run_log_dir)
    recorded_output_frames = parse_recorded_output_frames(media_log_path, test_start_unix_ts)

    frame_times = parse_frame_times(frame_times_csv)
    frame_source = "frame_times.csv"
    if not frame_times:
        perf_logs_dir = run_log_dir if run_log_dir else CAPTURE_BIN / "logs"
        frame_times = parse_perf_metrics_frame_times_from_dir(api, perf_logs_dir, test_start_unix_ts)
        frame_source = "perf_metrics_*.csv"
    estimated_frame_count = 0
    if not frame_times:
        frame_times, estimated_frame_count = parse_media_log_frame_times(media_log_path, test_start_unix_ts)
        frame_source = f"{media_log_path.parent.name}/{media_log_path.name}"

    stats = analyze_frame_times(frame_times, target_fps, test_name)
    if "error" not in stats:
        stats["source"] = frame_source
        if recorded_output_frames is not None:
            stats["recorded_output_frames"] = recorded_output_frames
            stats["effective_count"] = recorded_output_frames
        elif estimated_frame_count > 0:
            stats["effective_count"] = estimated_frame_count

    print("\nResults:")
    print_stats(stats)

    fatal_log_error = scan_logs_for_fatal_errors(run_log_dir, test_start_unix_ts)
    if fatal_log_error:
        return stats, fatal_log_error

    hook_runtime_error = verify_runtime_hook_activity(api, run_log_dir, test_start_unix_ts)
    if hook_runtime_error:
        return stats, hook_runtime_error

    if recorded_output_frames is None:
        return stats, f"Recording completion stats missing from {media_log_path.name}"
    if recorded_output_frames == 0:
        return stats, "Recording produced zero encoded video frames"

    quality_error = evaluate_quality(
        stats,
        total_record_s=total_record_s,
        min_frames=min_frames,
        target_fps=target_fps,
        min_frame_ratio=min_frame_ratio,
        max_avg_frame_ratio=max_avg_frame_ratio,
        max_frame_spike_ratio=max_frame_spike_ratio,
        max_spike_pct=max_spike_pct,
    )
    if quality_error:
        return stats, quality_error

    return stats, None


def ensure_binaries_exist(apis: List[str], arches: List[str]) -> None:
    missing = []
    for api in apis:
        for arch in arches:
            exe = resolve_test_exe(api, arch)
            if not exe.exists():
                missing.append(str(exe))

    if missing:
        print("\nERROR: Missing test binaries:")
        for path in missing:
            print(f"  - {path}")
        print("Run 'python build.py' first to build all required test apps.")
        sys.exit(1)


def write_results_json(
    output_path: Path,
    args: argparse.Namespace,
    apis_to_test: List[str],
    arches_to_test: List[str],
    results: List[Dict[str, Any]],
) -> None:
    payload: Dict[str, Any] = {
        "timestamp": datetime.now().isoformat(timespec="seconds"),
        "config": {
            "resolution": args.resolution,
            "gpu_load": args.gpu_load,
            "tests": args.tests,
            "duration": args.duration,
            "min_frames": args.min_frames,
            "api": args.api,
            "arch": args.arch,
            "apis_resolved": apis_to_test,
            "arches_resolved": arches_to_test,
            "target_fps": args.target_fps,
            "min_frame_ratio": args.min_frame_ratio,
            "max_avg_frame_ratio": args.max_avg_frame_ratio,
            "max_frame_spike_ratio": args.max_frame_spike_ratio,
            "max_spike_pct": args.max_spike_pct,
        },
        "results": results,
        "passed_count": sum(1 for r in results if r["status"] == "passed"),
        "failed_count": sum(1 for r in results if r["status"] == "failed"),
    }

    output_path.parent.mkdir(parents=True, exist_ok=True)
    with open(output_path, "w", encoding="utf-8") as f:
        json.dump(payload, f, indent=2)

    print(f"\nWrote JSON results: {output_path}")


def main() -> None:
    parser = argparse.ArgumentParser(description="Run capture performance tests")
    parser.add_argument(
        "--resolution",
        type=int,
        nargs=2,
        default=[3840, 2160],
        metavar=("WIDTH", "HEIGHT"),
        help="Test resolution (default: 3840 2160)",
    )
    parser.add_argument(
        "--gpu-load",
        type=int,
        default=15,
        help="GPU load passes per frame (default: 15)",
    )
    parser.add_argument(
        "--tests",
        type=int,
        default=3,
        help="Number of test iterations per API/arch target (default: 3)",
    )
    parser.add_argument(
        "--duration",
        type=float,
        default=10.0,
        help="Total recording duration per test in seconds (default: 10)",
    )
    parser.add_argument(
        "--api",
        choices=[
            "dx12",
            "dx11",
            "dx10",
            "dx9",
            "dx9ex",
            "dx8",
            "dx7",
            "dx6",
            "vulkan",
            "vulkan_fg",
            "opengl",
            "opengl_legacy",
            "directdraw7",
            "both",
            "all",
        ],
        default="all",
        help="API selection (default: all)",
    )
    parser.add_argument(
        "--arch",
        choices=["x64", "x86", "both"],
        default="x64",
        help="Architecture selection (default: x64)",
    )
    parser.add_argument(
        "--min-frames",
        type=int,
        default=60,
        help="Minimum recorded frames required per test (default: 60)",
    )
    parser.add_argument(
        "--results-json",
        default=None,
        help="Path to write machine-readable JSON results (default: latest run log directory)",
    )
    parser.add_argument(
        "--target-fps",
        type=int,
        default=120,
        help="Expected recording FPS for local quality checks (default: 120)",
    )
    parser.add_argument(
        "--min-frame-ratio",
        type=float,
        default=0.60,
        help="Minimum fraction of expected frames required per test (default: 0.60)",
    )
    parser.add_argument(
        "--max-avg-frame-ratio",
        type=float,
        default=1.35,
        help="Maximum allowed average frame time as a multiple of budget (default: 1.35)",
    )
    parser.add_argument(
        "--max-frame-spike-ratio",
        type=float,
        default=4.0,
        help="Maximum allowed worst frame time as a multiple of budget (default: 4.0)",
    )
    parser.add_argument(
        "--max-spike-pct",
        type=float,
        default=5.0,
        help="Maximum allowed percentage of >2x-budget spikes (default: 5.0)",
    )
    args = parser.parse_args()
    suite_start_unix_ts = time.time()

    width, height = args.resolution

    print("=" * 60)
    print("CAPTURE PERFORMANCE TEST SUITE")
    print("=" * 60)
    print(f"Timestamp: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    print(f"Resolution: {width}x{height}")
    print(f"GPU Load: {args.gpu_load} passes/frame")
    print(f"Tests per target: {args.tests}")
    print(f"Recording duration: {args.duration}s per test")
    print(f"Minimum frames required: {args.min_frames}")
    print(f"Target FPS: {args.target_fps}")
    print(f"Minimum frame ratio: {args.min_frame_ratio:.2f}")
    print(f"Max average frame ratio: {args.max_avg_frame_ratio:.2f}")
    print(f"Max spike ratio: {args.max_frame_spike_ratio:.2f}")
    print(f"Max >2x-budget spike percentage: {args.max_spike_pct:.1f}%")

    if args.api == "all":
        apis_to_test = list(DEFAULT_APIS)
    elif args.api == "both":
        apis_to_test = ["dx12", "vulkan"]
    else:
        apis_to_test = [args.api]

    if "vulkan_fg" in apis_to_test and args.arch != "x64":
        parser.error("--api vulkan_fg is x64-only; use --arch x64")

    arches_to_test = ["x64", "x86"] if args.arch == "both" else [args.arch]

    print(f"APIs under test: {', '.join(api.upper() for api in apis_to_test)}")
    print(f"Architectures under test: {', '.join(a.upper() for a in arches_to_test)}")

    ensure_binaries_exist(apis_to_test, arches_to_test)
    config_snapshot = ensure_testapp_profiles([API_EXECUTABLES[api] for api in apis_to_test])

    try:
        print("\nCleaning up existing processes...")
        kill_processes()

        all_results: List[Dict[str, Any]] = []

        for arch in arches_to_test:
            for api in apis_to_test:
                for test_num in range(1, args.tests + 1):
                    test_name = f"{api.upper()}-{arch.upper()} Test {test_num}"
                    stats, error = run_single_test(
                        api=api,
                        arch=arch,
                        width=width,
                        height=height,
                        gpu_load=args.gpu_load,
                        total_record_s=args.duration,
                        test_name=test_name,
                        min_frames=args.min_frames,
                        target_fps=args.target_fps,
                        min_frame_ratio=args.min_frame_ratio,
                        max_avg_frame_ratio=args.max_avg_frame_ratio,
                        max_frame_spike_ratio=args.max_frame_spike_ratio,
                        max_spike_pct=args.max_spike_pct,
                    )

                    status = "passed" if error is None else "failed"
                    result_entry: Dict[str, Any] = {
                        "name": test_name,
                        "api": api,
                        "arch": arch,
                        "iteration": test_num,
                        "status": status,
                        "error": error,
                        "stats": stats,
                    }
                    all_results.append(result_entry)

                    kill_processes()
                    time.sleep(2)

        print("\n" + "=" * 60)
        print("SUMMARY")
        print("=" * 60)

        failures = [r for r in all_results if r["status"] == "failed"]

        for arch in arches_to_test:
            for api in apis_to_test:
                combo = [r for r in all_results if r["api"] == api and r["arch"] == arch]
                combo_passed = [r for r in combo if r["status"] == "passed" and r["stats"]]

                print(f"\n{api.upper()}-{arch.upper()} " f"(passed {len(combo_passed)}/{len(combo)} tests):")

                stats_for_combo: List[Dict[str, Any]] = [
                    r["stats"] for r in combo_passed if isinstance(r["stats"], dict)
                ]
                if stats_for_combo:
                    avg_min = statistics.mean(float(s["min"]) for s in stats_for_combo)
                    avg_max = statistics.mean(float(s["max"]) for s in stats_for_combo)
                    avg_avg = statistics.mean(float(s["avg"]) for s in stats_for_combo)
                    avg_variance = statistics.mean(float(s["variance"]) for s in stats_for_combo)
                    total_spikes = sum(int(s["spikes_12ms"]) for s in stats_for_combo)
                    total_frames = sum(int(s.get("effective_count", s["count"])) for s in stats_for_combo)
                    spike_pct = 100.0 * total_spikes / total_frames if total_frames > 0 else 0.0

                    print(f"  Avg frame time: {avg_avg:.2f}ms " f"(range: {avg_min:.2f}-{avg_max:.2f}ms)")
                    print(f"  Avg variance: {avg_variance:.2f}ms")
                    print(f"  Total spikes >12ms: {total_spikes}/{total_frames} " f"({spike_pct:.1f}%)")
                else:
                    print("  No passing runs for this target.")

        if failures:
            print("\nFAILURES")
            print("-" * 60)
            for failure in failures:
                print(f"  {failure['name']}: {failure['error'] or 'Unknown failure'}")

        results_json_path = (
            Path(args.results_json) if args.results_json else default_results_json_path(suite_start_unix_ts)
        )
        write_results_json(
            output_path=results_json_path,
            args=args,
            apis_to_test=apis_to_test,
            arches_to_test=arches_to_test,
            results=all_results,
        )

        print("\n" + "=" * 60)
        print("TEST COMPLETE")
        print("=" * 60)

        if failures:
            sys.exit(1)
    finally:
        restore_capture_config(config_snapshot)
        kill_processes()
        time.sleep(1)


if __name__ == "__main__":
    main()
