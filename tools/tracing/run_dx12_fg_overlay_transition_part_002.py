

def run_probe(args):
    if not CAPTURE_EXE.exists():
        fail(f"captureengine.exe not found: {CAPTURE_EXE}")
    if not TESTAPP_EXE.exists():
        fail(f"dx12_fg_switch_test.exe not found: {TESTAPP_EXE}")

    ensure_no_lingering_processes()
    try:
        TESTAPP_LOG.unlink()
    except FileNotFoundError:
        pass
    OUTPUT_ROOT.mkdir(parents=True, exist_ok=True)
    run_dir = OUTPUT_ROOT / time.strftime("%Y%m%d_%H%M%S")
    run_dir.mkdir()
    start_wall_ns = time.time_ns()
    start = time.monotonic()
    samples = []
    events = {}
    phase = "startup"
    log_offset = 0
    minimum_frame = None
    minimum_dark_ratio = 1.0
    minimum_edge_ratio = 1.0
    phase_minimum_frames = {}
    phase_maximum_frames = {}
    phase_extremes = {}
    ce_process = None
    app_process = None
    extended_step_index = 0
    extended_step_key_sent = False
    extended_step_anchor = None

    try:
        ce_process = subprocess.Popen(
            [str(CAPTURE_EXE)], cwd=str(CAPTURE_BIN), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        )
        time.sleep(args.ce_lead_ms / 1000.0)
        long_sequence = args.sequence in ("extended-aba-cycle", "repeat-pure-dlss")
        app_duration = max(args.duration_seconds, 18) if long_sequence else args.duration_seconds
        probe_timeout = max(args.timeout_seconds, 18.0) if long_sequence else args.timeout_seconds
        app_args = [
            str(TESTAPP_EXE),
            f"--duration={app_duration}",
            "--auto-return-fsr=3600",
            "--no-dlss-suspend-stress",
        ]
        if args.sequence == "fsr-to-dlss":
            app_args.extend(
                (
                    f"--auto-fsr-start={args.fsr_start_seconds}",
                    f"--auto-dlss-start={args.dlss_start_seconds}",
                    "--no-dlss-off-stress",
                )
            )
        else:
            app_args.extend(
                (
                    "--auto-fsr-start=3600",
                    "--auto-dlss-start=3600",
                    "--no-dlss-off-stress",
                )
            )
        app_process = subprocess.Popen(
            app_args,
            cwd=str(TESTAPP_BIN),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

        with DesktopProbe(CAPTURE_WIDTH, CAPTURE_HEIGHT) as probe:
            deadline = start + probe_timeout
            while time.monotonic() < deadline and app_process.poll() is None:
                now = time.monotonic()
                log_offset, new_log = read_new_log_text(log_offset)
                if "Auto exit after" in new_log or "Exiting (total frames rendered:" in new_log:
                    events.setdefault("app_exit", now)
                    break
                if "Auto sequence clock reset" in new_log:
                    events.setdefault("ready", now)
                    phase = "off"
                    if extended_step_anchor is None:
                        extended_step_anchor = now
                if "Mode now FSR FG" in new_log:
                    if args.sequence == "dlss-off-fsr" and "off_after" in events:
                        events.setdefault("fsr_after", now)
                        phase = "fsr-after"
                    else:
                        events.setdefault("fsr", now)
                        phase = "fsr"
                if args.sequence == "fsr-to-dlss" and "Mode request: FSR FG -> DLSS FG" in new_log:
                    events.setdefault("transition", now)
                    phase = "transition"
                if (
                    args.sequence in ("dlss-to-off", "dlss-off-fsr", "repeat-pure-dlss")
                    and "Mode request: OFF -> DLSS FG" in new_log
                ):
                    if args.sequence == "repeat-pure-dlss" and "off_after" in events:
                        events.setdefault("dlss_again_transition", now)
                        events.setdefault("transition", now)
                        phase = "dlss-again-transition"
                    else:
                        events.setdefault("dlss_transition", now)
                        phase = "dlss-transition"
                if "DLSS replacement passthrough Present completed" in new_log:
                    events.setdefault("replacement_present", now)
                if "Mode now DLSS FG" in new_log:
                    if args.sequence == "repeat-pure-dlss" and "off_after" in events:
                        events.setdefault("dlss_again", now)
                        phase = "dlss-again"
                    else:
                        events.setdefault("dlss", now)
                        phase = "dlss"
                if args.sequence == "dlss-to-off" and "Mode request: DLSS FG -> OFF" in new_log:
                    events.setdefault("transition", now)
                    phase = "transition"
                if args.sequence in ("dlss-off-fsr", "repeat-pure-dlss") and "Mode request: DLSS FG -> OFF" in new_log:
                    phase = "off-transition"
                if (
                    args.sequence in ("dlss-to-off", "dlss-off-fsr", "repeat-pure-dlss")
                    and "Mode now OFF" in new_log
                    and "dlss" in events
                ):
                    events.setdefault("off_after", now)
                    phase = "off-after"
                if args.sequence == "dlss-off-fsr" and "Mode request: OFF -> FSR FG" in new_log:
                    events.setdefault("transition", now)
                    phase = "fsr-transition"

                if args.sequence == "extended-aba-cycle" and extended_step_index < len(EXTENDED_ABA_STEPS):
                    event_name, virtual_key, completion_text, step_phase, delay_seconds = EXTENDED_ABA_STEPS[
                        extended_step_index
                    ]
                    if extended_step_key_sent and completion_text in new_log:
                        events[event_name] = now
                        phase = step_phase
                        extended_step_index += 1
                        extended_step_key_sent = False
                        extended_step_anchor = now
                    if (
                        extended_step_index < len(EXTENDED_ABA_STEPS)
                        and not extended_step_key_sent
                        and extended_step_anchor is not None
                    ):
                        event_name, virtual_key, _completion_text, step_phase, delay_seconds = EXTENDED_ABA_STEPS[
                            extended_step_index
                        ]
                        if now - extended_step_anchor >= delay_seconds:
                            if not post_key_to_process(app_process.pid, virtual_key):
                                fail(f"could not post extended-cycle key for {event_name}")
                            events[f"{event_name}_key"] = now
                            if event_name == "cycle_off_final":
                                events["transition"] = now
                            phase = f"{step_phase}-transition"
                            extended_step_key_sent = True

                if (
                    args.sequence in ("dlss-to-off", "dlss-off-fsr", "repeat-pure-dlss")
                    and "ready" in events
                    and "dlss_key" not in events
                    and now - events["ready"] >= args.dlss_start_seconds
                ):
                    if not post_key_to_process(app_process.pid, VK_DLSS):
                        fail("could not post the DLSS key to the test app window")
                    events["dlss_key"] = now
                    phase = "dlss-transition"
                if (
                    args.sequence in ("dlss-to-off", "dlss-off-fsr", "repeat-pure-dlss")
                    and "dlss" in events
                    and "off_key" not in events
                    and now - events["dlss"] >= args.off_after_seconds
                ):
                    if not post_key_to_process(app_process.pid, VK_OFF):
                        fail("could not post the all-FG-off key to the test app window")
                    events["off_key"] = now
                if (
                    args.sequence == "dlss-off-fsr"
                    and "off_after" in events
                    and "fsr_key" not in events
                    and now - events["off_after"] >= args.fsr_after_seconds
                ):
                    if not post_key_to_process(app_process.pid, VK_FSR):
                        fail("could not post the FSR key to the test app window")
                    events["fsr_key"] = now
                    events.setdefault("transition", now)
                    phase = "fsr-transition"
                if (
                    args.sequence == "repeat-pure-dlss"
                    and "off_after" in events
                    and "dlss_again_key" not in events
                    and now - events["off_after"] >= args.second_dlss_after_seconds
                ):
                    if not post_key_to_process(app_process.pid, VK_DLSS):
                        fail("could not post the second DLSS key to the test app window")
                    events["dlss_again_key"] = now
                    events.setdefault("transition", now)
                    phase = "dlss-again-transition"

                frame = probe.capture()
                dark_ratio, mean_luma, edge_ratio, bright_rg_ratio = overlay_metric(frame)
                samples.append(
                    Sample((now - start) * 1000.0, phase, dark_ratio, mean_luma, edge_ratio, bright_rg_ratio)
                )
                dark_tracked_phases = ("fsr", "transition", "dlss", "off-after", "fsr-transition", "fsr-after")
                if phase in dark_tracked_phases and dark_ratio < minimum_dark_ratio:
                    minimum_dark_ratio = dark_ratio
                    minimum_frame = frame
                phase_minimum, phase_maximum = phase_extremes.get(phase, (2.0, -1.0))
                if edge_ratio < phase_minimum:
                    phase_minimum = edge_ratio
                    phase_minimum_frames[phase] = frame
                if edge_ratio > phase_maximum:
                    phase_maximum = edge_ratio
                    phase_maximum_frames[phase] = frame
                phase_extremes[phase] = (phase_minimum, phase_maximum)
                minimum_edge_ratio = min(minimum_edge_ratio, edge_ratio)
                target_sequence_complete = (
                    args.sequence == "extended-aba-cycle" and "cycle_off_final" in events
                ) or (
                    args.sequence == "repeat-pure-dlss" and "dlss_again" in events
                )
                target_completion_time = events.get("cycle_off_final", events.get("dlss_again"))
                if (
                    target_sequence_complete
                    and target_completion_time is not None
                    and (now - target_completion_time) * 1000.0 >= args.seam_ms
                ):
                    events["probe_complete"] = now
                    break
                time.sleep(args.sample_interval_ms / 1000.0)

        if app_process.poll() is None and "app_exit" not in events and "probe_complete" not in events:
            fail(f"test app exceeded {probe_timeout:.1f}s probe timeout")
    finally:
        close_process(app_process, "dx12_fg_switch_test.exe")
        close_process(ce_process, "captureengine.exe")
        ensure_no_lingering_processes()

    if minimum_frame:
        write_bmp(run_dir / "minimum_overlay_metric.bmp", minimum_frame, CAPTURE_WIDTH, CAPTURE_HEIGHT)
    for phase_name, frame in phase_minimum_frames.items():
        write_bmp(run_dir / f"{phase_name}_minimum_edge.bmp", frame, CAPTURE_WIDTH, CAPTURE_HEIGHT)
    for phase_name, frame in phase_maximum_frames.items():
        write_bmp(run_dir / f"{phase_name}_maximum_edge.bmp", frame, CAPTURE_WIDTH, CAPTURE_HEIGHT)
    with (run_dir / "samples.csv").open("w", newline="", encoding="utf-8") as output:
        writer = csv.writer(output)
        writer.writerow(("elapsed_ms", "phase", "dark_ratio", "mean_luma", "edge_ratio", "bright_rg_ratio"))
        for sample in samples:
            writer.writerow(
                (
                    f"{sample.elapsed_ms:.3f}",
                    sample.phase,
                    f"{sample.dark_ratio:.6f}",
                    f"{sample.mean_luma:.3f}",
                    f"{sample.edge_ratio:.6f}",
                    f"{sample.bright_rg_ratio:.6f}",
                )
            )

    phase_names = (
        "off", "dlss-transition", "fsr", "transition", "dlss", "off-transition", "off-after",
        "fsr-transition", "fsr-after", "dlss-again-transition", "dlss-again",
    ) + tuple(step[3] for step in EXTENDED_ABA_STEPS) + tuple(f"{step[3]}-transition" for step in EXTENDED_ABA_STEPS)
    summaries = {phase_name: phase_summary(samples, phase_name) for phase_name in phase_names}
    if args.sequence == "extended-aba-cycle":
        baseline_samples = [
            sample
            for sample in samples
            if "cycle_dlss_2" in events
            and "cycle_off_final_key" in events
            and (events["cycle_dlss_2"] - start) * 1000.0
            <= sample.elapsed_ms
            <= (events["cycle_off_final_key"] - start) * 1000.0
        ]
        baseline = statistics.median(sample.bright_rg_ratio for sample in baseline_samples) if baseline_samples else 0.0
    elif args.sequence == "repeat-pure-dlss":
        baseline_summary = summaries.get("dlss")
        baseline = baseline_summary["bright_rg_median"] if baseline_summary else 0.0
    else:
        baseline_phase = "fsr" if args.sequence == "fsr-to-dlss" else "dlss"
        baseline_summary = summaries.get(baseline_phase)
        baseline = baseline_summary["bright_rg_median"] if baseline_summary else 0.0
    transition_start_ms = (events["transition"] - start) * 1000.0 if "transition" in events else 0.0
    if args.sequence == "fsr-to-dlss":
        target_event = "dlss"
    elif args.sequence == "dlss-to-off":
        target_event = "off_after"
    elif args.sequence == "extended-aba-cycle":
        target_event = "cycle_off_final"
    elif args.sequence == "repeat-pure-dlss":
        target_event = "dlss_again"
    else:
        target_event = "fsr_after"
    target_start_ms = (
        (events[target_event] - start) * 1000.0 if target_event in events else samples[-1].elapsed_ms
    )
    seam_end_ms = target_start_ms + args.seam_ms
    seam_samples = [
        sample for sample in samples if transition_start_ms <= sample.elapsed_ms <= seam_end_ms
    ]
    seam_minimum = min((sample.bright_rg_ratio for sample in seam_samples), default=0.0)
    loss_ratio = seam_minimum / baseline if baseline > 0.0 else 0.0
    required_final_retained_ratio = (
        args.minimum_initial_retained_ratio if args.sequence == "repeat-pure-dlss" else args.minimum_retained_ratio
    )
    visible = baseline >= args.minimum_baseline_bright_rg_ratio
    seam_complete = bool(samples) and samples[-1].elapsed_ms >= seam_end_ms
    if args.sequence == "fsr-to-dlss":
        required_events = ("fsr", "dlss")
    elif args.sequence == "dlss-to-off":
        required_events = ("dlss", "off_after")
    elif args.sequence == "extended-aba-cycle":
        required_events = tuple(step[0] for step in EXTENDED_ABA_STEPS)
    elif args.sequence == "repeat-pure-dlss":
        required_events = ("dlss", "off_after", "dlss_again")
    else:
        required_events = ("dlss", "off_after", "fsr_after")
    passed = (
        visible
        and seam_complete
        and seam_minimum >= args.minimum_overlay_bright_rg_ratio
        and loss_ratio >= required_final_retained_ratio
        and all(
            event in events for event in required_events
        )
    )

    initial_dlss_seam = None
    if (
        args.sequence in ("dlss-to-off", "dlss-off-fsr", "repeat-pure-dlss")
        and "dlss_key" in events
        and "dlss" in events
    ):
        initial_start_ms = (events["dlss_key"] - start) * 1000.0
        initial_target_ms = (events["dlss"] - start) * 1000.0
        initial_end_ms = initial_target_ms + args.seam_ms
        initial_samples = [
            sample for sample in samples if initial_start_ms <= sample.elapsed_ms <= initial_end_ms
        ]
        initial_minimum = min((sample.bright_rg_ratio for sample in initial_samples), default=0.0)
        initial_loss_ratio = initial_minimum / baseline if baseline > 0.0 else 0.0
        initial_complete = bool(samples) and samples[-1].elapsed_ms >= initial_end_ms
        initial_dlss_seam = (initial_minimum, initial_loss_ratio, initial_complete)
        passed = (
            passed
            and initial_complete
            and initial_minimum >= args.minimum_overlay_bright_rg_ratio
            and initial_loss_ratio >= args.minimum_initial_retained_ratio
        )

    session = newest_capture_session(start_wall_ns)
    strict_log_markers = (
        "[OVERLAY VISIBILITY] INTERRUPTED/UNPROVEN",
        "[OVERLAY COVERAGE] uncovered streak STARTED",
        "DEVICE REMOVED",
        "DEVICE_REMOVED",
        "0x887A002B",
        "REFLEX-NOT-DETECTED",
    )
    strict_log_counts = {marker: 0 for marker in strict_log_markers}
    required_log_markers = ()
    if args.sequence == "repeat-pure-dlss":
        required_log_markers = (
            "Armed exact prewarmed PostSL handoff backend for its first Present",
            "First exact prewarmed PostSL handoff Present preserved its ready overlay backend",
        )
    required_log_counts = {marker: 0 for marker in required_log_markers}
    if session:
        hook_log = session / "hook_debug.log"
        if hook_log.exists():
            hook_log_text = hook_log.read_text(encoding="utf-8", errors="replace")
            strict_log_counts = {marker: hook_log_text.count(marker) for marker in strict_log_markers}
            required_log_counts = {marker: hook_log_text.count(marker) for marker in required_log_markers}
        else:
            passed = False
    else:
        passed = False
    passed = passed and all(count == 0 for count in strict_log_counts.values())
    passed = passed and all(count > 0 for count in required_log_counts.values())

    print(f"probe duration: {(time.monotonic() - start):.3f}s; samples: {len(samples)}")
    for phase_name, summary in summaries.items():
        if summary:
            print(
                f"{phase_name:10s} n={summary['count']:4d} dark-ratio "
                f"min/median/max={summary['minimum']:.4f}/{summary['median']:.4f}/{summary['maximum']:.4f}; "
                f"edge min/median/max={summary['edge_minimum']:.4f}/{summary['edge_median']:.4f}/"
                f"{summary['edge_maximum']:.4f}; bright-RG min/median/max="
                f"{summary['bright_rg_minimum']:.4f}/{summary['bright_rg_median']:.4f}/"
                f"{summary['bright_rg_maximum']:.4f}"
            )
    print(
        f"{args.sequence} final seam retained overlay bright-RG ratio: {loss_ratio:.3f} "
        f"(required {required_final_retained_ratio:.3f}; window through {args.seam_ms:.0f}ms after target)"
    )
    if initial_dlss_seam:
        initial_minimum, initial_loss_ratio, initial_complete = initial_dlss_seam
        print(
            f"off-to-dlss initial seam retained overlay bright-RG ratio: {initial_loss_ratio:.3f} "
            f"(minimum={initial_minimum:.4f}; required ratio {args.minimum_initial_retained_ratio:.3f} and "
            f"absolute {args.minimum_overlay_bright_rg_ratio:.4f})"
        )
        if not initial_complete:
            print("off-to-dlss initial seam incomplete before the test app exited")
    if not seam_complete:
        observed_until_ms = samples[-1].elapsed_ms - target_start_ms if samples else 0.0
        print(
            f"seam incomplete: observed {max(0.0, observed_until_ms):.0f}ms after target "
            f"before the test app exited (required {args.seam_ms:.0f}ms)"
        )
    print(
        "strict runtime log gates: "
        + ", ".join(f"{marker}={count}" for marker, count in strict_log_counts.items())
    )
    if required_log_counts:
        print(
            "required runtime handoff proofs: "
            + ", ".join(f"{marker}={count}" for marker, count in required_log_counts.items())
        )
    print(f"result: {'PASS' if passed else 'FAIL'}")
    print(f"probe artifacts: {run_dir}")
    if session:
        print(f"captureengine session: {session}")
    return 0 if passed else 1


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--sequence",
        choices=("fsr-to-dlss", "dlss-to-off", "dlss-off-fsr", "extended-aba-cycle", "repeat-pure-dlss"),
        default="fsr-to-dlss",
    )
    parser.add_argument("--timeout-seconds", type=float, default=13.0)
    parser.add_argument("--duration-seconds", type=int, default=7)
    parser.add_argument("--ce-lead-ms", type=float, default=350.0)
    parser.add_argument("--fsr-start-seconds", type=int, default=1)
    parser.add_argument("--dlss-start-seconds", type=int, default=3)
    parser.add_argument("--off-after-seconds", type=int, default=1)
    parser.add_argument("--fsr-after-seconds", type=int, default=1)
    parser.add_argument("--second-dlss-after-seconds", type=float, default=1.0)
    parser.add_argument("--sample-interval-ms", type=float, default=1.0)
    parser.add_argument("--minimum-baseline-bright-rg-ratio", type=float, default=0.02)
    parser.add_argument("--minimum-overlay-bright-rg-ratio", type=float, default=0.01)
    parser.add_argument("--minimum-initial-retained-ratio", type=float, default=0.20)
    parser.add_argument("--minimum-retained-ratio", type=float, default=0.50)
    parser.add_argument("--seam-ms", "--dlss-seam-ms", dest="seam_ms", type=float, default=750.0)
    args = parser.parse_args()
    if (
        args.timeout_seconds <= 0
        or args.duration_seconds <= 0
        or args.sample_interval_ms < 0
        or args.seam_ms < 0
        or args.off_after_seconds <= 0
        or args.fsr_after_seconds <= 0
        or args.second_dlss_after_seconds <= 0
    ):
        parser.error("timeouts, duration, and sample interval must be positive")
    return args


def main():
    try:
        ctypes.windll.user32.SetProcessDPIAware()
    except (AttributeError, OSError):
        pass
    return run_probe(parse_args())


if __name__ == "__main__":
    raise SystemExit(main())
