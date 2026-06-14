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
from typing import Optional, Union


SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = SCRIPT_DIR.parent
CAPTURE_BIN = PROJECT_ROOT / "installed" / "captureengine"
TESTAPP_BIN = PROJECT_ROOT / "installed" / "testapp"
CAPTURE_CONFIG = CAPTURE_BIN / "config.ini"
RUN_LOG_DIR_RE = re.compile(r"^\d{8}_\d{6}$")

PROCESS_NAME = "dx12_av_sync_test.exe"
SECONDARY_PROCESS_NAME = "dx12_av_sync_late.exe"
SUPPORTED_CODECS = ["aac", "alac", "flac", "opus", "pcm"]
SUPPORTED_METHODS = ["wgc", "inject"]


@dataclass
class Scenario:
    capture_method: str
    audio_codec: str
    fps: int
    label: str = ""
    duration_sec: Optional[int] = None
    app_fps: Optional[Union[str, int]] = None
    gpu_load: Optional[int] = None
    include_source_stall: bool = False
    source_stall: Optional[str] = None
    audio_layout: str = ""
    width: Optional[int] = None
    height: Optional[int] = None
    encoder_stress_scene: bool = False
    nvenc_preset: str = "p1"
    rate_control: str = "VBR"
    bitrate: str = "125Mbps"
    max_bitrate: str = "200Mbps"
    secondary_app_audio: bool = False
    secondary_app_start_sec: float = 5.0

    @property
    def name(self):
        parts = [self.capture_method, self.audio_codec, f"{self.fps}fps"]
        if self.label:
            parts.append(re.sub(r"[^A-Za-z0-9_.-]+", "_", self.label.strip().lower()))
        return "_".join(parts)


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


def prepare_secondary_app_alias(app_exe):
    alias_dir = TESTAPP_BIN / "secondary_app_audio"
    alias_dir.mkdir(parents=True, exist_ok=True)
    alias = alias_dir / SECONDARY_PROCESS_NAME
    try:
        shutil.copy2(app_exe, alias)
    except OSError as exc:
        fail(f"failed to prepare secondary app-audio helper: {exc}")
    return alias


def taskkill_processes():
    for proc in ["captureengine.exe", PROCESS_NAME, SECONDARY_PROCESS_NAME]:
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
    output_dir = output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    mic_enabled = "true" if include_microphone else "false"
    audio_layout = resolve_audio_layout(scenario, include_mixed_track)
    if audio_layout == "mixed":
        system_tracks = "1,3"
        app_tracks = "2,3"
    elif audio_layout == "duplicate_app":
        system_tracks = "3"
        app_tracks = "1,2"
    elif audio_layout == "late_secondary_app":
        system_tracks = "3"
        app_tracks = "1,2"
    else:
        system_tracks = "1"
        app_tracks = "2"
    secondary_app_section = ""
    if scenario.secondary_app_audio:
        secondary_app_section = f"""
[AppAudio.2]
enabled=true
process={SECONDARY_PROCESS_NAME}
track=2
"""
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
rate_control={scenario.rate_control}
bitrate={scenario.bitrate}
max_bitrate={scenario.max_bitrate}
keyframe_interval=2
b_frames=0

[NVENC]
preset={scenario.nvenc_preset}
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
{secondary_app_section}

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


def resolve_audio_layout(scenario, include_mixed_track):
    return scenario.audio_layout or ("mixed" if include_mixed_track else "strict")


def generated_app_paths(file_name):
    return [TESTAPP_BIN / file_name, TESTAPP_BIN / "x86" / file_name]


def generated_secondary_app_paths(file_name):
    return [TESTAPP_BIN / "secondary_app_audio" / file_name]


def remove_stale_app_artifacts():
    for path in (
        generated_app_paths("dx12_av_sync_test_manifest.json")
        + generated_app_paths("dx12_av_sync_test.log")
        + generated_secondary_app_paths("dx12_av_sync_test_manifest.json")
        + generated_secondary_app_paths("dx12_av_sync_test.log")
    ):
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


def run_process(command, timeout, secondary_command=None, secondary_delay_sec=0.0):
    start = time.monotonic()
    proc = subprocess.Popen(
        [str(part) for part in command],
        cwd=str(CAPTURE_BIN),
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    secondary_proc = None
    secondary_started = False
    timed_out = False
    try:
        if secondary_command:
            secondary_deadline = start + max(0.0, float(secondary_delay_sec))
            while time.monotonic() < secondary_deadline and proc.poll() is None:
                time.sleep(min(0.1, max(0.0, secondary_deadline - time.monotonic())))
            if proc.poll() is None:
                secondary_proc = subprocess.Popen(
                    [str(part) for part in secondary_command],
                    cwd=str(Path(secondary_command[0]).parent),
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )
                secondary_started = True
        return_code = proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        timed_out = True
        proc.terminate()
        try:
            return_code = proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            return_code = proc.wait(timeout=5)
    finally:
        if secondary_proc and secondary_proc.poll() is None:
            secondary_proc.terminate()
            try:
                secondary_proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                secondary_proc.kill()
                secondary_proc.wait(timeout=5)
    if secondary_started:
        wait_for_process_exit(SECONDARY_PROCESS_NAME, 2.0)
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


def load_json_file(path):
    try:
        if path and path.exists():
            return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    return None


def parse_cadence_shortfall_ms(value):
    text = str(value or "").strip()
    if text.endswith("ms"):
        text = text[:-2]
    if "/" in text:
        text = text.split("/", 1)[1]
    try:
        return float(text)
    except ValueError:
        return 0.0


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
    secondary_app_exe = prepare_secondary_app_alias(app_exe) if scenario.secondary_app_audio else None
    write_scenario_config(scenario, captures_dir, args.include_microphone, args.include_mixed_track, args.video_encoder)

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
    app_audio_lead_ms = resolve_app_audio_lead_ms(args.app_audio_lead_ms, scenario.capture_method, scenario.fps, app_fps)
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
    return_code, elapsed, timed_out = run_process(
        launch,
        timeout=scenario_duration_sec + delay_ms / 1000.0 + 30.0,
        secondary_command=secondary_launch,
        secondary_delay_sec=secondary_launch_delay,
    )
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
            "analysis_start_sec": args.analysis_start_sec,
            "microphone_enabled": args.include_microphone,
            "mixed_track_enabled": args.include_mixed_track,
            "external_system_audio": args.external_system_audio,
            "allow_tearing": args.allow_tearing,
            "source_stall": source_stall_text if include_source_stall else None,
            "secondary_app_audio": scenario.secondary_app_audio,
            "secondary_app_start_sec": scenario.secondary_app_start_sec if scenario.secondary_app_audio else None,
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
        },
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
    elif not analysis_media_log.exists():
        result["failure"] = "CE media log not found"
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


def build_matrix_scenarios(capture_methods, codecs, fps_values):
    return [Scenario(method, codec, fps) for method in capture_methods for codec in codecs for fps in fps_values]


def build_scenarios(args):
    if args.profile == "quick":
        return [
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
    if args.profile == "codec-pass":
        return [Scenario("wgc", codec, 60, label="codec_finalize", duration_sec=10) for codec in SUPPORTED_CODECS]
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


def build_parser():
    parser = argparse.ArgumentParser(description="Run deterministic A/V sync capture scenarios and analyze them.")
    parser.add_argument(
        "--profile",
        choices=["quick", "codec-pass", "stress", "wgc-overload", "late-app", "full", "long-soak", "custom"],
        default="quick",
        help="Scenario profile. Bare runner defaults to the fast zero-drift quick gate.",
    )
    parser.add_argument("--fast-zero-drift", dest="profile_aliases", action="append_const", const="quick")
    parser.add_argument("--codec-finalization-pass", dest="profile_aliases", action="append_const", const="codec-pass")
    parser.add_argument("--short-stress", dest="profile_aliases", action="append_const", const="stress")
    parser.add_argument("--wgc-overload-gate", dest="profile_aliases", action="append_const", const="wgc-overload")
    parser.add_argument("--late-app-source-gate", dest="profile_aliases", action="append_const", const="late-app")
    parser.add_argument("--full-matrix", dest="profile_aliases", action="append_const", const="full")
    parser.add_argument("--long-soak", dest="profile_aliases", action="append_const", const="long-soak")
    parser.add_argument("--long-soak-minutes", type=float, default=40.0)
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
    parser.add_argument("--encoder-stress-scene", action="store_true")
    parser.add_argument("--nvenc-preset", default="p1")
    parser.add_argument("--rate-control", default="VBR")
    parser.add_argument("--bitrate", default="125Mbps")
    parser.add_argument("--max-bitrate", default="200Mbps")
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
    parser.add_argument("--max-offset-slope-ms-per-min", type=float, default=30.0)
    parser.add_argument("--min-offset-slope-excursion-ms", type=float, default=12.0)
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
    parser.add_argument("--require-overload", action="store_true")
    parser.add_argument("--min-overload-shortfall-ms", type=float, default=80.0)
    parser.add_argument("--self-test", action="store_true")
    parser.set_defaults(include_microphone=True, app_audio_clock_scheduling=True)
    return parser


def parse_args(argv=None):
    parser = build_parser()
    args = parser.parse_args(argv)
    aliases = args.profile_aliases or []
    if len(set(aliases)) > 1:
        fail(f"conflicting scenario profiles requested: {', '.join(aliases)}")
    if aliases:
        args.profile = aliases[-1]
    elif args.profile == "quick" and explicit_matrix_selection(argv):
        args.profile = "custom"
    if args.profile == "wgc-overload":
        args.require_overload = True
    if args.long_soak_minutes < 30.0 or args.long_soak_minutes > 45.0:
        fail("--long-soak-minutes must be between 30 and 45")
    return args


def self_test():
    quick = parse_args(["--fast-zero-drift", "--dry-run"])
    quick_names = [scenario.name for scenario in build_scenarios(quick)]
    assert len(quick_names) == 6
    assert quick_names == [
        "wgc_alac_60fps_quick_lossless_60",
        "wgc_aac_120fps_quick_lossy_120",
        "inject_alac_120fps_quick_lossless_120",
        "inject_aac_60fps_quick_lossy_60",
        "wgc_alac_60fps_late_app_lossless",
        "inject_aac_60fps_late_app_lossy",
    ]
    assert sum(1 for scenario in build_scenarios(quick) if scenario.secondary_app_audio) == 2

    late_app = parse_args(["--late-app-source-gate", "--dry-run"])
    late_app_scenarios = build_scenarios(late_app)
    assert late_app.profile == "late-app"
    assert len(late_app_scenarios) == 2
    assert all(scenario.secondary_app_audio for scenario in late_app_scenarios)
    assert {scenario.capture_method for scenario in late_app_scenarios} == {"wgc", "inject"}

    codec_pass = parse_args(["--codec-finalization-pass", "--dry-run"])
    codec_scenarios = build_scenarios(codec_pass)
    assert [scenario.audio_codec for scenario in codec_scenarios] == SUPPORTED_CODECS
    assert all(scenario.capture_method == "wgc" and scenario.fps == 60 for scenario in codec_scenarios)

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
            result = run_scenario(args, scenario, run_root, ce_exe, app_exe)
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
