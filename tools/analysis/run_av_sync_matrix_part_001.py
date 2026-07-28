#!/usr/bin/env python3

import argparse
import copy
import json
import math
import os
import re
import shutil
import statistics
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
SUPPORTED_METHODS = ["dxgi_dup", "wgc", "inject"]
SYNC_SMOOTHNESS_DEFAULT_DELAY_MS = 35.0
SYNC_SMOOTHNESS_MAX_OFFSET_MS = 10.0
SYNC_SMOOTHNESS_MAX_MEAN_OFFSET_MS = 5.0
SYNC_SMOOTHNESS_MAX_TRACK_SPREAD_MS = 5.0
SYNC_SMOOTHNESS_MAX_CONTENT_RETRIES = 3
SYNC_SMOOTHNESS_RETRY_MAX_RESIDUAL_MS = 120.0
SYNC_SMOOTHNESS_RETRY_GUARD_MS = 0.75
SYNC_SMOOTHNESS_RETRY_MAX_SPREAD_STEP_MS = 2.5
SYNC_SMOOTHNESS_RETRY_MAX_MEAN_STEP_MS = 24.0
WGC_TEAR_FREE_AUDIO_LEAD_MS = 76.0
WGC_BELOW_TARGET_EXTRA_AUDIO_LEAD_FRAMES = 2.0


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
    bit_depth: int = 8

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
            return WGC_TEAR_FREE_AUDIO_LEAD_MS + WGC_BELOW_TARGET_EXTRA_AUDIO_LEAD_FRAMES * (
                1000.0 / max(1, int(output_fps))
            )
        return WGC_TEAR_FREE_AUDIO_LEAD_MS
    try:
        value = float(text)
    except ValueError:
        fail(f"invalid app audio lead: {app_audio_lead_arg}")
    if value < -500.0 or value > 500.0:
        fail(f"app audio lead must be between -500 and 500 ms: {app_audio_lead_arg}")
    return value


def modeled_sync_smoothness_initial_latencies_ms(args, scenario):
    app_fps = resolve_app_fps(scenario.app_fps or args.app_fps, scenario.capture_method, scenario.fps)
    if getattr(args, "sync_smoothness_delay_explicit", False):
        system_latency_ms = float(args.sync_smoothness_delay_ms)
    else:
        system_latency_ms = resolve_app_audio_lead_ms("auto", scenario.capture_method, scenario.fps, app_fps)
    app_latency_ms = system_latency_ms
    return {
        "mode": "modeled",
        "modeled_delay_ms": round(float(args.sync_smoothness_delay_ms), 3),
        "system_latency_ms": round(system_latency_ms, 3),
        "app_latency_ms": round(app_latency_ms, 3),
        "modeled_source": (
            "explicit_delay" if getattr(args, "sync_smoothness_delay_explicit", False) else "method_aware"
        ),
    }


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


def write_scenario_config(scenario, output_dir, include_microphone, include_mixed_track, video_encoder,
                          audio_capture_latency_ms=0.0, app_capture_latency_ms=None,
                          wgc_smoothness_floor_ms=None):
    CAPTURE_CONFIG.parent.mkdir(parents=True, exist_ok=True)
    output_dir = output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    # App-source capture latency defaults to the global value (so the app inherits it and the
    # equalization delay is 0); set it distinctly to exercise per-source equalization.
    if app_capture_latency_ms is None:
        app_capture_latency_ms = audio_capture_latency_ms
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
[Profile.2]
Process={SECONDARY_PROCESS_NAME}
video_capture=none
audio_enabled=true
audio_track=2
"""
    # Smoothness floor override: when set, exercises the WGC baseline jitter buffer (use with
    # audio_capture_latency_ms=0 to validate the video-only / low-confidence floor path: the realized
    # delay should pin near the floor and A/V sync must stay clean). Omitted -> product default (auto).
    smoothness_floor_line = (
        f"wgc_smoothness_floor_ms={wgc_smoothness_floor_ms}\n" if wgc_smoothness_floor_ms is not None else ""
    )
    text = f"""[Capture]
capture_method={scenario.capture_method}

[WGC]
{smoothness_floor_line}

[AudioSync]
audio_capture_latency_ms={audio_capture_latency_ms}
audio_latency_autodetect=false

[Logging]
log_level=trace

[Output]
container=mkv
output_dir={output_dir}

[Overlay]
enabled=false
capture_include_overlay=false
screenshot_include_overlay=false

[Video]
encoder={video_encoder}
fps={scenario.fps}
vfr=false
capture_cursor=false
bit_depth={scenario.bit_depth}
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
lookahead=off
spatial_aq=false
temporal_aq=false

[Audio]
codec={scenario.audio_codec}
bitrate=192
sample_rate=default
bit_depth=default
downmix=false

[SystemAudio]
enabled=true
device=
track={system_tracks}

[Profile.1]
Process={PROCESS_NAME}
video_capture=inherit
audio_enabled=true
audio_track={app_tracks}
audio_capture_latency_ms={app_capture_latency_ms}
{secondary_app_section}

[Microphone]
enabled={mic_enabled}
device=
track=4

[FpsLimiter]
capture_sync_enabled=false
general_enabled=false

[DesktopOverlay]
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


MEDIA_LOG_NAME_RE = re.compile(r"^media_[A-Za-z0-9_-]+_[0-9]+\.log$", re.IGNORECASE)


def is_media_log_name(name):
    return name.lower() == "media.log" or MEDIA_LOG_NAME_RE.match(name) is not None


def list_hook_logs(session_dir):
    if not session_dir or not session_dir.exists():
        return []
    logs = []
    for path in session_dir.glob("*.log"):
        if not is_media_log_name(path.name):
            logs.append(str(path))
    return sorted(logs)


def snapshot_session_logs(session_dir, destination_dir):
    if not session_dir or not session_dir.exists():
        return None, [], [], [], None
    media_logs = []
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
            or (lower_name.startswith("recording_") and lower_name.endswith(".manifest"))
            or (lower_name.startswith("perf_metrics_") and lower_name.endswith(".csv"))
        )
        if not should_copy:
            continue
        snapshot = snapshot_artifact(path, destination_dir / path.name)
        if not snapshot:
            continue
        copied_any = True
        if is_media_log_name(path.name):
            media_logs.append(snapshot)
        elif lower_name.endswith(".log"):
            hook_logs.append(str(snapshot))
        elif lower_name.startswith("perf_metrics_") and lower_name.endswith(".csv"):
            perf_csvs.append(str(snapshot))
        elif lower_name == "session_manifest.txt":
            session_manifest = snapshot
    if not copied_any:
        return None, [], [], [], None
    return destination_dir, media_logs, hook_logs, perf_csvs, session_manifest


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


def start_cpu_contention_workers(count):
    workers = []
    worker_code = (
        "x=0x123456789abcdef\n"
        "while True:\n"
        " x=(x*6364136223846793005+1442695040888963407)&0xffffffffffffffff\n"
    )
    for _ in range(max(0, int(count))):
        workers.append(
            subprocess.Popen(
                [sys.executable, "-c", worker_code],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        )
    return workers


def stop_cpu_contention_workers(workers):
    for worker in workers:
        if worker.poll() is None:
            worker.terminate()
    for worker in workers:
        try:
            worker.wait(timeout=3)
        except subprocess.TimeoutExpired:
            worker.kill()
            worker.wait(timeout=3)


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
