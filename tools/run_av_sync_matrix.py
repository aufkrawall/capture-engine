#!/usr/bin/env python3

import argparse
import json
import re
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = SCRIPT_DIR.parent
CAPTURE_BIN = PROJECT_ROOT / "installed" / "captureengine"
TESTAPP_BIN = PROJECT_ROOT / "installed" / "testapp"
CAPTURE_CONFIG = CAPTURE_BIN / "config.ini"
RUN_LOG_DIR_RE = re.compile(r"^\d{8}_\d{6}$")

PROCESS_NAME = "dx12_av_sync_test.exe"
SUPPORTED_CODECS = ["aac", "alac", "flac", "opus", "pcm"]
SUPPORTED_METHODS = ["wgc", "inject"]


@dataclass
class Scenario:
    capture_method: str
    audio_codec: str
    fps: int

    @property
    def name(self):
        return f"{self.capture_method}_{self.audio_codec}_{self.fps}fps"


def resolve_app_fps(app_fps_arg, capture_method, output_fps):
    text = str(app_fps_arg).strip().lower()
    if text in ("", "auto"):
        # Tear-free inject capture is usually composed near the desktop refresh
        # rate. A stable 144 fps source remains above 60/120 fps targets on
        # common high-refresh desktops without asking DWM for an impossible
        # nominal 240 fps tear-free cadence.
        if capture_method == "inject":
            return max(int(output_fps), 144)
        return max(240, int(output_fps) * 2)
    try:
        value = int(text)
    except ValueError:
        fail(f"invalid app fps: {app_fps_arg}")
    if value <= 0:
        fail(f"app fps must be positive: {app_fps_arg}")
    return value


def resolve_app_audio_lead_ms(app_audio_lead_arg, capture_method, output_fps, app_fps):
    text = str(app_audio_lead_arg).strip().lower()
    if text in ("", "auto"):
        # Stimulus-side calibration for the default tear-free oracle path.
        # WGC sees DWM-composed video timing; inject sees app Present timing but still
        # gains the same tear-free presentation offset when the app avoids DXGI tearing.
        if capture_method == "inject":
            if int(output_fps) >= 100:
                return 50.0
            return 45.0
        if int(app_fps) < int(output_fps):
            return 56.0 + 2.0 * (1000.0 / max(1, int(output_fps)))
        return 56.0
    try:
        value = float(text)
    except ValueError:
        fail(f"invalid app audio lead: {app_audio_lead_arg}")
    if value < -500.0 or value > 500.0:
        fail(f"app audio lead must be between -500 and 500 ms: {app_audio_lead_arg}")
    return value


def fail(message):
    print(f"error: {message}", file=sys.stderr)
    raise SystemExit(1)


def split_csv(text, allowed=None):
    values = []
    for item in str(text).split(","):
        value = item.strip().lower()
        if not value:
            continue
        if allowed and value not in allowed:
            fail(f"unsupported value '{value}', expected one of: {', '.join(allowed)}")
        values.append(value)
    if not values:
        fail("empty matrix selection")
    return values


def split_int_csv(text):
    values = []
    for item in str(text).split(","):
        item = item.strip()
        if not item:
            continue
        try:
            value = int(item)
        except ValueError:
            fail(f"invalid integer value: {item}")
        if value <= 0:
            fail(f"fps must be positive: {item}")
        values.append(value)
    if not values:
        fail("empty fps selection")
    return values


def default_tool_path(name):
    candidate = PROJECT_ROOT / "build" / "msys64" / "clang64" / "bin" / f"{name}.exe"
    return candidate if candidate.exists() else Path(name)


def ensure_inputs():
    ce = CAPTURE_BIN / "captureengine.exe"
    app = TESTAPP_BIN / PROCESS_NAME
    app_x86 = TESTAPP_BIN / "x86" / PROCESS_NAME
    if not ce.exists():
        fail(f"captureengine.exe not found: {ce}")
    if not app.exists() and not app_x86.exists():
        fail(f"{PROCESS_NAME} not found under {TESTAPP_BIN}; build test apps first")
    return ce, app if app.exists() else app_x86


def taskkill_processes():
    for proc in ["captureengine.exe", PROCESS_NAME]:
        subprocess.run(["taskkill", "/F", "/IM", proc], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.5)


def is_process_running(process_name):
    result = subprocess.run(
        ["tasklist", "/FI", f"IMAGENAME eq {process_name}", "/NH"],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )
    return process_name.lower() in result.stdout.lower()


def wait_for_process_exit(process_name, timeout_seconds):
    deadline = time.monotonic() + max(0.0, float(timeout_seconds))
    while time.monotonic() < deadline:
        if not is_process_running(process_name):
            return True
        time.sleep(0.25)
    return not is_process_running(process_name)


def read_config_snapshot():
    if CAPTURE_CONFIG.exists():
        return CAPTURE_CONFIG.read_text(encoding="utf-8", errors="replace")
    return None


def restore_config(snapshot):
    if snapshot is None:
        if CAPTURE_CONFIG.exists():
            CAPTURE_CONFIG.unlink()
        return
    CAPTURE_CONFIG.parent.mkdir(parents=True, exist_ok=True)
    CAPTURE_CONFIG.write_text(snapshot, encoding="utf-8")


def write_scenario_config(scenario, output_dir, include_microphone, include_mixed_track, video_encoder):
    CAPTURE_CONFIG.parent.mkdir(parents=True, exist_ok=True)
    output_dir.mkdir(parents=True, exist_ok=True)
    mic_enabled = "true" if include_microphone else "false"
    system_tracks = "1,3" if include_mixed_track else "1"
    app_tracks = "2,3" if include_mixed_track else "2"
    text = f"""[General]
log_level=trace
capture_method={scenario.capture_method}
wgc_window_detection=(
{PROCESS_NAME}
)

[Injection]
whitelist=(
{PROCESS_NAME}
)

[Overlay]
enabled=false
capture_include_overlay=false
screenshot_include_overlay=false

[Video]
encoder={video_encoder}
fps={scenario.fps}
container=mkv
output_dir={output_dir}
vfr=false
capture_cursor=false
bit_depth=8
color_space=bt709
color_range=limited
chroma_subsampling=420
rate_control=VBR
bitrate=125Mbps
max_bitrate=200Mbps
keyframe_interval=2
b_frames=0

[NVENC]
preset=p1
tuning=hq
multipass=disabled
lookahead=false
aq=false

[Audio]
enabled=true
device=
track={system_tracks}
codec={scenario.audio_codec}
bitrate=192
sample_rate=default
bit_depth=default
downmix=false

[AppAudio.1]
enabled=true
process={PROCESS_NAME}
track={app_tracks}

[Microphone]
enabled={mic_enabled}
device=
track=4

[FpsLimiter]
capture_sync_enabled=false
general_enabled=false

[pseudo-overlay]
enabled=false
"""
    CAPTURE_CONFIG.write_text(text, encoding="utf-8")


def generated_app_paths(file_name):
    return [TESTAPP_BIN / file_name, TESTAPP_BIN / "x86" / file_name]


def remove_stale_app_artifacts():
    for path in generated_app_paths("dx12_av_sync_test_manifest.json") + generated_app_paths("dx12_av_sync_test.log"):
        try:
            if path.exists():
                path.unlink()
        except OSError:
            pass


def newest_existing(paths, since_unix):
    candidates = []
    for path in paths:
        try:
            if path.exists() and path.stat().st_mtime + 1.0 >= since_unix:
                candidates.append(path)
        except OSError:
            continue
    if not candidates:
        return None
    candidates.sort(key=lambda p: p.stat().st_mtime, reverse=True)
    return candidates[0]


def snapshot_artifact(source, destination):
    if not source:
        return None
    try:
        if not source.exists():
            return None
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
        return destination
    except OSError:
        return None


def find_latest_run_log_dir(since_unix):
    logs_root = CAPTURE_BIN / "logs"
    if not logs_root.exists():
        return None
    candidates = []
    for path in logs_root.iterdir():
        try:
            if path.is_dir() and RUN_LOG_DIR_RE.match(path.name) and path.stat().st_mtime + 1.0 >= since_unix:
                candidates.append(path)
        except OSError:
            continue
    if not candidates:
        return None
    candidates.sort(key=lambda p: p.stat().st_mtime, reverse=True)
    return candidates[0]


def find_latest_capture(output_dir, since_unix):
    candidates = []
    for path in output_dir.glob("*.mkv"):
        try:
            if path.stat().st_mtime + 1.0 >= since_unix:
                candidates.append(path)
        except OSError:
            continue
    if not candidates:
        return None
    candidates.sort(key=lambda p: p.stat().st_mtime, reverse=True)
    return candidates[0]


def list_hook_logs(session_dir):
    if not session_dir or not session_dir.exists():
        return []
    logs = []
    for path in session_dir.glob("*.log"):
        if path.name.lower() != "media.log":
            logs.append(str(path))
    return sorted(logs)


def snapshot_session_logs(session_dir, destination_dir):
    if not session_dir or not session_dir.exists():
        return None, None, [], [], None
    media_log = None
    hook_logs = []
    perf_csvs = []
    session_manifest = None
    copied_any = False
    for path in sorted(session_dir.iterdir()):
        try:
            if not path.is_file():
                continue
        except OSError:
            continue
        lower_name = path.name.lower()
        should_copy = (
            lower_name.endswith(".log")
            or lower_name == "session_manifest.txt"
            or (lower_name.startswith("perf_metrics_") and lower_name.endswith(".csv"))
        )
        if not should_copy:
            continue
        snapshot = snapshot_artifact(path, destination_dir / path.name)
        if not snapshot:
            continue
        copied_any = True
        if lower_name == "media.log":
            media_log = snapshot
        elif lower_name.endswith(".log"):
            hook_logs.append(str(snapshot))
        elif lower_name.startswith("perf_metrics_") and lower_name.endswith(".csv"):
            perf_csvs.append(str(snapshot))
        elif lower_name == "session_manifest.txt":
            session_manifest = snapshot
    if not copied_any:
        return None, None, [], [], None
    return destination_dir, media_log, hook_logs, perf_csvs, session_manifest


def run_process(command, timeout):
    start = time.monotonic()
    proc = subprocess.Popen(
        [str(part) for part in command],
        cwd=str(CAPTURE_BIN),
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    timed_out = False
    try:
        return_code = proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        timed_out = True
        proc.terminate()
        try:
            return_code = proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            return_code = proc.wait(timeout=5)
    return return_code, time.monotonic() - start, timed_out


def run_analyzer(command, stdout_path):
    result = subprocess.run(
        [str(part) for part in command],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )
    stdout_path.write_text(result.stdout, encoding="utf-8")
    return result.returncode


def run_scenario(args, scenario, run_root, ce_exe, app_exe):
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
    write_scenario_config(scenario, captures_dir, args.include_microphone, args.include_mixed_track, args.video_encoder)

    delay_ms = args.delay_ms
    duration_ms = args.duration_sec * 1000
    app_duration = args.duration_sec + max(4, delay_ms // 1000 + 2)
    app_fps = resolve_app_fps(args.app_fps, scenario.capture_method, scenario.fps)
    app_audio_lead_ms = resolve_app_audio_lead_ms(args.app_audio_lead_ms, scenario.capture_method, scenario.fps, app_fps)
    launch = [
        ce_exe,
        f"--auto-record={delay_ms},{duration_ms}",
        "--launch",
        app_exe,
        "--width",
        args.width,
        "--height",
        args.height,
        "--fps",
        app_fps,
        "--duration",
        app_duration,
        "--gpu-load",
        args.gpu_load,
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
    if args.app_audio_clock_scheduling:
        launch.append("--audio-clock-scheduling")
    if args.allow_tearing:
        launch.remove("--no-allow-tearing")
        launch.append("--allow-tearing")
    if args.include_source_stall:
        for source_stall in split_csv(args.source_stall):
            launch.extend(["--source-stall", source_stall])

    start_unix = time.time()
    return_code, elapsed, timed_out = run_process(launch, timeout=args.duration_sec + delay_ms / 1000.0 + 30.0)
    app_exited = wait_for_process_exit(PROCESS_NAME, args.app_exit_timeout_sec)
    if not app_exited:
        taskkill_processes()
    time.sleep(0.5)

    manifest = newest_existing(generated_app_paths("dx12_av_sync_test_manifest.json"), start_unix)
    app_log = newest_existing(generated_app_paths("dx12_av_sync_test.log"), start_unix)
    manifest_snapshot = snapshot_artifact(manifest, scenario_dir / "dx12_av_sync_test_manifest.json")
    app_log_snapshot = snapshot_artifact(app_log, scenario_dir / "dx12_av_sync_test.log")
    capture = find_latest_capture(captures_dir, start_unix)
    session_dir = find_latest_run_log_dir(start_unix)
    media_log = (session_dir / "media.log") if session_dir else CAPTURE_BIN / "logs" / "media.log"
    log_snapshot_dir, media_log_snapshot, hook_log_snapshots, perf_csv_snapshots, session_manifest_snapshot = (
        snapshot_session_logs(session_dir, scenario_dir / "ce_logs")
    )
    analysis_session_dir = log_snapshot_dir if media_log_snapshot else session_dir
    analysis_media_log = media_log_snapshot if media_log_snapshot else media_log

    result = {
        "scenario": {
            "capture_method": scenario.capture_method,
            "audio_codec": scenario.audio_codec,
            "fps": scenario.fps,
            "app_fps": app_fps,
            "app_audio_clock_scheduling": args.app_audio_clock_scheduling,
            "app_audio_buffer_ms": args.app_audio_buffer_ms,
            "app_audio_lead_ms": app_audio_lead_ms,
            "analysis_start_sec": args.analysis_start_sec,
            "microphone_enabled": args.include_microphone,
            "mixed_track_enabled": args.include_mixed_track,
            "external_system_audio": args.external_system_audio,
            "allow_tearing": args.allow_tearing,
        },
        "process": {
            "return_code": return_code,
            "elapsed_seconds": round(elapsed, 3),
            "timed_out": timed_out,
            "stimulus_app_exited": app_exited,
        },
        "paths": {
            "capture_file": str(capture) if capture else None,
            "ce_session_dir": str(analysis_session_dir) if analysis_session_dir else None,
            "ce_session_dir_original": str(session_dir) if session_dir else None,
            "media_log": str(analysis_media_log) if analysis_media_log and analysis_media_log.exists() else None,
            "hook_logs": hook_log_snapshots if hook_log_snapshots else list_hook_logs(session_dir),
            "perf_csv": perf_csv_snapshots,
            "session_manifest": str(session_manifest_snapshot) if session_manifest_snapshot else None,
            "app_log": str(app_log_snapshot) if app_log_snapshot else (str(app_log) if app_log else None),
            "manifest": str(manifest_snapshot) if manifest_snapshot else (str(manifest) if manifest else None),
            "analyzer_json": str(analyzer_json),
            "analyzer_stdout": str(analyzer_stdout),
            "triage_json": str(triage_json),
            "triage_stdout": str(triage_stdout),
        },
        "analyzer_exit_code": None,
        "triage_exit_code": None,
        "passed": False,
        "failure": None,
    }

    if timed_out:
        result["failure"] = "captureengine timed out"
    elif return_code != 0:
        result["failure"] = f"captureengine exited with {return_code}"
    elif not app_exited:
        result["failure"] = "stimulus app did not exit before manifest snapshot"
    elif not capture:
        result["failure"] = "capture file not found"
    elif not manifest_snapshot:
        result["failure"] = "stimulus manifest not found"
    elif not app_log_snapshot:
        result["failure"] = "stimulus app log not found"
    elif not analysis_media_log.exists():
        result["failure"] = "CE media log not found"
    else:
        # 0-based ffmpeg audio ordinals in strict default: a:0=Track 1 system, a:1=Track 2 app,
        # a:2=Track 4 microphone. With --include-mixed-track, a:2=Track 3 mixed and a:3=Track 4 mic.
        # Mixed and mic streams are diagnostic evidence; pure system/app are strict timing gates by default.
        # When unrelated desktop audio is known to be playing, system loopback can be downgraded for that run only.
        non_strict_ordinals = [2]
        if args.external_system_audio:
            non_strict_ordinals.append(0)
        if args.include_microphone and args.include_mixed_track:
            non_strict_ordinals.append(3)
        non_strict_audio = ",".join(str(value) for value in sorted(set(non_strict_ordinals)))
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
            "--max-longest-repeat",
            args.max_longest_repeat,
            "--max-motion-stall",
            args.max_motion_stall,
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

    scenario_report_path.write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(f"  {'PASS' if result['passed'] else 'FAIL'} report={scenario_report_path}")
    if result["failure"]:
        print(f"  failure={result['failure']}")
    return result


def build_scenarios(args):
    methods = split_csv(args.capture_methods, SUPPORTED_METHODS)
    codecs = split_csv(args.codecs, SUPPORTED_CODECS)
    fps_values = split_int_csv(args.fps)
    return [Scenario(method, codec, fps) for method in methods for codec in codecs for fps in fps_values]


def main():
    parser = argparse.ArgumentParser(description="Run deterministic A/V sync capture scenarios and analyze them.")
    parser.add_argument("--capture-methods", default="wgc,inject")
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
        "--allow-tearing",
        action="store_true",
        help="Opt into DXGI tearing in the stimulus app for stress experiments. Default keeps visual evidence tear-free.",
    )
    parser.add_argument("--video-encoder", default="av1_nvenc")
    parser.add_argument("--ffmpeg", type=Path, default=default_tool_path("ffmpeg"))
    parser.add_argument("--ffprobe", type=Path, default=default_tool_path("ffprobe"))
    parser.add_argument("--output-root", type=Path)
    parser.add_argument("--min-transitions", type=int, default=4)
    parser.add_argument("--max-av-offset-ms", type=float, default=25.0)
    parser.add_argument("--max-mean-av-offset-ms", type=float, default=15.0)
    parser.add_argument("--max-track-spread-ms", type=float, default=10.0)
    parser.add_argument("--max-longest-repeat", type=int, default=2)
    parser.add_argument("--max-motion-stall", type=int, default=3)
    parser.add_argument("--include-source-stall", action="store_true")
    parser.add_argument("--source-stall", default="8.35:300")
    parser.add_argument("--app-audio-buffer-ms", type=int, default=20)
    parser.add_argument("--analysis-start-sec", type=float, default=2.0)
    parser.add_argument(
        "--app-audio-lead-ms",
        default="auto",
        help="Stimulus audio lead in ms, or 'auto' for method-aware WGC/inject calibration.",
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
    parser.add_argument("--keep-going", action="store_true")
    parser.set_defaults(include_microphone=True, app_audio_clock_scheduling=True)
    args = parser.parse_args()

    ce_exe, app_exe = ensure_inputs()
    scenarios = build_scenarios(args)
    run_root = args.output_root or (
        CAPTURE_BIN / "avsync_runs" / datetime.now().strftime("%Y%m%d_%H%M%S")
    )

    if args.dry_run:
        print(f"run_root={run_root}")
        for scenario in scenarios:
            print(scenario.name)
        return

    snapshot = read_config_snapshot()
    results = []
    try:
        for scenario in scenarios:
            result = run_scenario(args, scenario, run_root, ce_exe, app_exe)
            results.append(result)
            if not result["passed"] and not args.keep_going:
                break
    finally:
        restore_config(snapshot)
        taskkill_processes()

    matrix_report = {
        "schema": "ce-avsync-matrix-report-v1",
        "run_root": str(run_root),
        "results": results,
        "passed": bool(results) and all(result["passed"] for result in results),
    }
    run_root.mkdir(parents=True, exist_ok=True)
    matrix_report_path = run_root / "matrix_report.json"
    matrix_report_path.write_text(json.dumps(matrix_report, indent=2), encoding="utf-8")
    print(f"matrix_report={matrix_report_path}")
    if not matrix_report["passed"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
