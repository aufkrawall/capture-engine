#!/usr/bin/env python3
"""
Automated capture regression test runner.

Runs selected API/architecture test apps with auto-record capture, analyzes
frame_times.csv, and enforces strict per-target pass criteria.

Usage:
    python run_tests.py
    python run_tests.py --api dx9 --arch both --tests 1 --duration 5
    python run_tests.py --api dx9ex --arch both --tests 1 --duration 5
    python run_tests.py --api all --arch both --min-frames 60
"""

import argparse
import csv
import json
import os
import re
import statistics
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

# Paths
SCRIPT_DIR = Path(__file__).parent.absolute()
PROJECT_ROOT = SCRIPT_DIR.parent
TESTAPP_BIN = PROJECT_ROOT / "installed" / "testapp"
if not TESTAPP_BIN.exists():
    TESTAPP_BIN = SCRIPT_DIR / "bin"

CAPTURE_BIN = PROJECT_ROOT / "installed" / "captureengine"
if not CAPTURE_BIN.exists():
    CAPTURE_BIN = PROJECT_ROOT / "build" / "bin"

FRAME_TIMES_CSV = CAPTURE_BIN / "logs" / "frame_times.csv"
MEDIA_LOG = CAPTURE_BIN / "logs" / "media.log"
DEFAULT_RESULTS_JSON_NAME = "integration_results.json"
RUN_LOG_DIR_RE = re.compile(r"^\d{8}_\d{6}$")
CAPTURE_CONFIG = CAPTURE_BIN / "config.ini"
CAPTURE_CONFIG_TEMPLATE = PROJECT_ROOT / "captureengine" / "config.ini.template"

DEFAULT_APIS = [
    "dx12",
    "dx11",
    "dx9",
    "dx9ex",
    "dx8",
    "dx7",
    "dx6",
    "vulkan",
    "opengl",
    "opengl_legacy",
    "directdraw7",
]
OPT_IN_APIS = ["vulkan_fg"]
SUPPORTED_APIS = DEFAULT_APIS + OPT_IN_APIS
API_EXECUTABLES = {
    "dx12": "dx12_test.exe",
    "dx11": "dx11_test.exe",
    "dx9": "dx9_test.exe",
    "dx9ex": "dx9ex_test.exe",
    "dx8": "dx8_test.exe",
    "dx7": "dx7_test.exe",
    "dx6": "dx6_test.exe",
    "vulkan": "vulkan_test.exe",
    "vulkan_fg": "vulkan_fg_switch_test.exe",
    "opengl": "opengl_test.exe",
    "opengl_legacy": "opengl_legacy_test.exe",
    "directdraw7": "directdraw7_test.exe",
}
API_LOG_NAMES = {
    "dx12": {"dx12"},
    "dx11": {"dx11"},
    "dx9": {"dx9", "dx9ex"},
    "dx9ex": {"dx9", "dx9ex"},
    "dx8": {"dx8"},
    "dx7": {"dx7", "d3d7", "direct3d7", "ddraw", "directdraw"},
    "dx6": {"dx6", "d3d6", "direct3d6", "ddraw", "directdraw"},
    "vulkan": {"vulkan"},
    "vulkan_fg": {"vulkan"},
    "opengl": {"opengl"},
    "opengl_legacy": {"opengl_legacy", "opengl"},
    "directdraw7": {"directdraw7", "directdraw", "ddraw"},
}

FATAL_LOG_PATTERNS = [
    (re.compile(r"DXGI_ERROR_DEVICE_REMOVED", re.IGNORECASE), "DXGI device removed"),
    (re.compile(r"DXGI_ERROR_DEVICE_HUNG", re.IGNORECASE), "DXGI device hung"),
    (re.compile(r"DEVICE_REMOVED", re.IGNORECASE), "device removed"),
    (re.compile(r"DEVICE_HUNG", re.IGNORECASE), "device hung"),
    (re.compile(r"VK_ERROR_DEVICE_LOST", re.IGNORECASE), "Vulkan device lost"),
    (re.compile(r"unhandled exception", re.IGNORECASE), "unhandled exception"),
]

HOOK_RUNTIME_PATTERNS = {
    "dx9": [
        re.compile(r"DX9: Direct3DCreate9 called \(Intercepted\)", re.IGNORECASE),
        re.compile(r"DX9: DetourEndScene", re.IGNORECASE),
        re.compile(r"DX9: DetourD3D9PresentInline called", re.IGNORECASE),
    ],
    "dx9ex": [
        re.compile(r"DX9: Direct3DCreate9 called \(Intercepted\)", re.IGNORECASE),
        re.compile(r"DX9: DetourEndScene", re.IGNORECASE),
        re.compile(r"DX9: DetourD3D9PresentInline called", re.IGNORECASE),
    ],
    "dx11": [
        re.compile(r"IAT: Patched D3D11CreateDevice", re.IGNORECASE),
        re.compile(r"DX11: \[frame \d+\] calling RenderOverlay", re.IGNORECASE),
    ],
    "dx12": [
        re.compile(r"DX12: ProcessFrame queue=", re.IGNORECASE),
        re.compile(r"DX12: Overlay frame #", re.IGNORECASE),
    ],
    "vulkan": [
        re.compile(r"Vulkan Layer: Capture_vkCreateInstance END - success", re.IGNORECASE),
        re.compile(r"Vulkan Layer: Overlay initialized successfully", re.IGNORECASE),
        re.compile(r"RenderOverlay#", re.IGNORECASE),
    ],
    "vulkan_fg": [
        re.compile(r"Vulkan Layer: Capture_vkCreateInstance END - success", re.IGNORECASE),
        re.compile(r"Vulkan Layer: Overlay initialized successfully", re.IGNORECASE),
        re.compile(r"RenderOverlay#", re.IGNORECASE),
    ],
    "opengl": [
        re.compile(r"OpenGL hooks installed", re.IGNORECASE),
        re.compile(r"OpenGLHook::Init", re.IGNORECASE),
    ],
    "opengl_legacy": [
        re.compile(r"OpenGL hooks installed", re.IGNORECASE),
        re.compile(r"OpenGLHook::Init", re.IGNORECASE),
    ],
    "dx8": [
        re.compile(r"DX8: Direct3DCreate8 hook installed", re.IGNORECASE),
        re.compile(r"DX8: Present hook installed", re.IGNORECASE),
        re.compile(r"DX8: State hooks initialized", re.IGNORECASE),
    ],
    "dx7": [
        re.compile(r"DDraw: DirectDrawCreateEx inline hook installed", re.IGNORECASE),
        re.compile(r"DDraw: Bootstrap DirectDraw7 created", re.IGNORECASE),
        re.compile(r"DDraw: Overlay helper ready", re.IGNORECASE),
    ],
    "dx6": [
        re.compile(r"DDraw: DirectDrawCreateEx inline hook installed", re.IGNORECASE),
        re.compile(r"DDraw: Bootstrap DirectDraw7 created", re.IGNORECASE),
        re.compile(r"DDraw: Overlay helper ready", re.IGNORECASE),
    ],
    "directdraw7": [
        re.compile(r"DDraw: DirectDrawCreateEx inline hook installed", re.IGNORECASE),
        re.compile(r"DDraw: Bootstrap DirectDraw7 created", re.IGNORECASE),
        re.compile(r"DDraw: Overlay helper ready", re.IGNORECASE),
    ],
}


def ensure_capture_config_exists() -> str:
    if CAPTURE_CONFIG.exists():
        return CAPTURE_CONFIG.read_text(encoding="utf-8")

    if not CAPTURE_CONFIG_TEMPLATE.exists():
        raise RuntimeError(f"Authoritative config template is missing: {CAPTURE_CONFIG_TEMPLATE}")
    text = CAPTURE_CONFIG_TEMPLATE.read_text(encoding="utf-8")

    CAPTURE_CONFIG.parent.mkdir(parents=True, exist_ok=True)
    CAPTURE_CONFIG.write_text(text, encoding="utf-8")
    return text


def add_test_profiles_to_config_text(config_text: str, executable_names: List[str]) -> str:
    executable_names = [name for name in executable_names if name]
    if not executable_names:
        return config_text

    suffix = "" if not config_text or config_text.endswith("\n") else "\n"
    blocks = []
    for index, executable_name in enumerate(executable_names, start=1):
        blocks.append(
            f"\n[Profile.CE Test {index}]\n"
            f"process={executable_name}\n"
            "video_capture=inject\n"
            "injection_mode=capture\n"
        )
    return config_text + suffix + "".join(blocks)


def ensure_testapp_profiles(
    executable_names: List[str],
) -> Optional[Tuple[bool, str]]:
    config_existed = CAPTURE_CONFIG.exists()
    original_text = ensure_capture_config_exists()
    updated_text = add_test_profiles_to_config_text(original_text, executable_names)
    if updated_text == original_text:
        return None

    CAPTURE_CONFIG.write_text(updated_text, encoding="utf-8")
    added_names = ", ".join(executable_names)
    print(f"Temporarily added CaptureEngine test profiles: {added_names}")
    return config_existed, original_text


def restore_capture_config(snapshot: Optional[Tuple[bool, str]]) -> None:
    if snapshot is None:
        return

    config_existed, original_text = snapshot
    if config_existed:
        CAPTURE_CONFIG.write_text(original_text, encoding="utf-8")
    elif CAPTURE_CONFIG.exists():
        CAPTURE_CONFIG.unlink()


def kill_processes() -> None:
    """Kill any existing test/capture processes."""
    for proc in [
        "captureengine.exe",
        "dx12_test.exe",
        "dx11_test.exe",
        "dx9_test.exe",
        "dx9ex_test.exe",
        "dx8_test.exe",
        "dx7_test.exe",
        "dx6_test.exe",
        "vulkan_test.exe",
        "vulkan_fg_switch_test.exe",
        "opengl_test.exe",
        "opengl_legacy_test.exe",
        "directdraw7_test.exe",
    ]:
        subprocess.run(
            ["taskkill", "/F", "/IM", proc],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    time.sleep(1)


def resolve_test_exe(api: str, arch: str) -> Path:
    base_dir = TESTAPP_BIN if arch == "x64" else TESTAPP_BIN / "x86"
    return base_dir / API_EXECUTABLES[api]


def start_test_app(api: str, arch: str, width: int, height: int, gpu_load: int) -> Optional[subprocess.Popen]:
    """Start a test app process for the selected API and architecture."""
    exe = resolve_test_exe(api, arch)
    if not exe.exists():
        print(f"ERROR: {exe} not found. Run 'python build.py' first.")
        return None

    return subprocess.Popen(
        [str(exe), str(width), str(height), str(gpu_load)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def start_auto_record(
    delay_ms: int,
    duration_ms: int,
    launch_command: Optional[List[str]] = None,
) -> Optional[subprocess.Popen]:
    """Start captureengine with --auto-record."""
    exe = CAPTURE_BIN / "captureengine.exe"
    if not exe.exists():
        print(f"ERROR: {exe} not found")
        return None

    args = [str(exe), f"--auto-record={delay_ms},{duration_ms}"]
    if launch_command:
        args.append("--launch")
        args.extend(launch_command)

    return subprocess.Popen(
        args,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def parse_frame_times(csv_path: Path) -> List[float]:
    """Parse frame_times.csv and return recording frame times in milliseconds."""
    if not csv_path.exists():
        return []

    frame_times: List[float] = []
    try:
        with open(csv_path, "r", encoding="utf-8") as f:
            reader = csv.reader(f)
            next(reader, None)

            for row in reader:
                if len(row) < 6:
                    continue
                try:
                    frame_time_us = float(row[2])
                    is_recording = int(row[5])
                    if is_recording == 1:
                        frame_times.append(frame_time_us / 1000.0)
                except (ValueError, IndexError):
                    continue
    except Exception as e:
        print(f"Error parsing CSV: {e}")

    return frame_times


def parse_perf_metrics_frame_times(api: str, since_unix_ts: float) -> List[float]:
    """Parse per-process perf_metrics CSV files and return frame times in ms."""
    return parse_perf_metrics_frame_times_from_dir(api, CAPTURE_BIN / "logs", since_unix_ts)


def parse_perf_metrics_frame_times_from_dir(api: str, logs_dir: Path, since_unix_ts: float) -> List[float]:
    """Parse per-process perf_metrics CSV files from a specific log directory."""
    if not logs_dir.exists():
        return []

    frame_times: List[float] = []
    pattern = "perf_metrics_*.csv"
    perf_files = sorted(
        logs_dir.glob(pattern),
        key=lambda p: p.stat().st_mtime,
        reverse=True,
    )

    for perf_path in perf_files:
        try:
            if perf_path.stat().st_mtime + 1.0 < since_unix_ts:
                continue
        except OSError:
            continue

        try:
            with open(perf_path, "r", encoding="utf-8") as f:
                reader = csv.DictReader(f)
                for row in reader:
                    api_name = str(row.get("api", "")).strip().lower()
                    allowed_api_names = API_LOG_NAMES.get(api, {api.lower()})
                    if api_name and api_name not in allowed_api_names:
                        continue
                    total_us_str = str(row.get("total_us", "")).strip()
                    if not total_us_str:
                        continue
                    try:
                        total_us = float(total_us_str)
                        if total_us >= 0:
                            frame_times.append(total_us / 1000.0)
                    except ValueError:
                        continue
        except Exception:
            continue

        if frame_times:
            return frame_times

    return frame_times


def find_latest_run_log_dir(since_unix_ts: float) -> Optional[Path]:
    logs_root = CAPTURE_BIN / "logs"
    if not logs_root.exists():
        return None

    candidates: List[Path] = []
    for path in logs_root.iterdir():
        if not path.is_dir() or not RUN_LOG_DIR_RE.match(path.name):
            continue
        try:
            if path.stat().st_mtime + 1.0 >= since_unix_ts:
                candidates.append(path)
        except OSError:
            continue

    if not candidates:
        return None

    candidates.sort(key=lambda p: p.stat().st_mtime, reverse=True)
    return candidates[0]


def parse_media_log_frame_times(media_log_path: Path, since_unix_ts: float) -> Tuple[List[float], int]:
    """Parse media.log timing lines written since since_unix_ts."""
    if not media_log_path.exists():
        return [], 0

    frame_times: List[float] = []
    max_frame_num = 0
    time_pattern = re.compile(r"^\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})\]")
    perf_pattern = re.compile(r"\[PERF\]\s+Frame\s+(\d+):\s+TOTAL=([0-9]*\.?[0-9]+)ms")
    packet_pattern = re.compile(r"Queuing video pkt #(\d+): pts=(\d+) dts=\d+ dur=(\d+) .* codec_tb=(\d+)/(\d+)")
    recording_stats_pattern = re.compile(r"Recording stats: input=(\d+) output=(\d+)")
    previous_packet_pts_ms: Optional[float] = None
    try:
        with open(media_log_path, "r", encoding="utf-8", errors="ignore") as f:
            for line in f:
                time_match = time_pattern.search(line)
                if time_match:
                    try:
                        line_ts = datetime.strptime(time_match.group(1), "%Y-%m-%d %H:%M:%S").timestamp()
                        if line_ts + 1.0 < since_unix_ts:
                            continue
                    except ValueError:
                        pass

                match = perf_pattern.search(line)
                if match:
                    try:
                        frame_num = int(match.group(1))
                        total_ms = float(match.group(2))
                        frame_times.append(total_ms)
                        if frame_num > max_frame_num:
                            max_frame_num = frame_num
                    except ValueError:
                        continue

                packet_match = packet_pattern.search(line)
                if packet_match:
                    try:
                        packet_num = int(packet_match.group(1))
                        pts = int(packet_match.group(2))
                        duration = int(packet_match.group(3))
                        time_base_num = int(packet_match.group(4))
                        time_base_den = int(packet_match.group(5))
                    except ValueError:
                        continue

                    max_frame_num = max(max_frame_num, packet_num)
                    if time_base_den <= 0:
                        continue

                    pts_ms = pts * 1000.0 * time_base_num / time_base_den
                    duration_ms = duration * 1000.0 * time_base_num / time_base_den
                    if previous_packet_pts_ms is not None:
                        delta_ms = pts_ms - previous_packet_pts_ms
                        if delta_ms >= 0:
                            frame_times.append(delta_ms)
                    elif duration_ms > 0:
                        frame_times.append(duration_ms)
                    previous_packet_pts_ms = pts_ms
                    continue

                recording_stats_match = recording_stats_pattern.search(line)
                if recording_stats_match:
                    try:
                        output_frames = int(recording_stats_match.group(2))
                        max_frame_num = max(max_frame_num, output_frames)
                    except ValueError:
                        continue
    except Exception:
        return [], 0

    return frame_times, max_frame_num


def parse_recorded_output_frames(media_log_path: Path, since_unix_ts: float) -> Optional[int]:
    """Return the final encoder output count for this recording, or None when no completion stats exist."""
    if not media_log_path.exists():
        return None

    output_frames: Optional[int] = None
    time_pattern = re.compile(r"^\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})\]")
    recording_stats_pattern = re.compile(r"Recording stats: input=(\d+) output=(\d+)")
    try:
        with open(media_log_path, "r", encoding="utf-8", errors="ignore") as f:
            for line in f:
                time_match = time_pattern.search(line)
                if time_match:
                    try:
                        line_ts = datetime.strptime(time_match.group(1), "%Y-%m-%d %H:%M:%S").timestamp()
                        if line_ts + 1.0 < since_unix_ts:
                            continue
                    except ValueError:
                        pass

                recording_stats_match = recording_stats_pattern.search(line)
                if recording_stats_match:
                    output_frames = int(recording_stats_match.group(2))
    except (OSError, ValueError):
        return None

    return output_frames


def default_results_json_path(since_unix_ts: float) -> Path:
    """Keep harness diagnostics beside the CE session logs they describe."""
    run_log_dir = find_latest_run_log_dir(since_unix_ts)
    if run_log_dir is None:
        run_id = datetime.fromtimestamp(since_unix_ts).strftime("%Y%m%d_%H%M%S")
        run_log_dir = CAPTURE_BIN / "logs" / run_id
    return run_log_dir / DEFAULT_RESULTS_JSON_NAME


def analyze_frame_times(frame_times: List[float], target_fps: int, name: str = "") -> Dict[str, Any]:
    """Analyze frame times and return stats dictionary."""
    if not frame_times:
        return {"name": name, "error": "No frames"}

    stats: Dict[str, Any] = {
        "name": name,
        "count": len(frame_times),
        "min": min(frame_times),
        "max": max(frame_times),
        "avg": statistics.mean(frame_times),
        "median": statistics.median(frame_times),
        "stdev": statistics.stdev(frame_times) if len(frame_times) > 1 else 0.0,
        "variance": max(frame_times) - min(frame_times),
    }

    stats["spikes_10ms"] = sum(1 for ft in frame_times if ft > 10)
    stats["spikes_12ms"] = sum(1 for ft in frame_times if ft > 12)
    stats["spikes_15ms"] = sum(1 for ft in frame_times if ft > 15)
    stats["spikes_20ms"] = sum(1 for ft in frame_times if ft > 20)

    count = int(stats["count"])
    stats["spike_pct_10ms"] = 100.0 * int(stats["spikes_10ms"]) / count
    stats["spike_pct_12ms"] = 100.0 * int(stats["spikes_12ms"]) / count

    if target_fps > 0:
        target_frame_time_ms = 1000.0 / target_fps
        spikes_15x_budget = sum(1 for ft in frame_times if ft > target_frame_time_ms * 1.5)
        spikes_2x_budget = sum(1 for ft in frame_times if ft > target_frame_time_ms * 2.0)
        spikes_3x_budget = sum(1 for ft in frame_times if ft > target_frame_time_ms * 3.0)
        stats["target_fps"] = target_fps
        stats["target_frame_time_ms"] = target_frame_time_ms
        stats["spikes_1_5x_budget"] = spikes_15x_budget
        stats["spikes_2x_budget"] = spikes_2x_budget
        stats["spikes_3x_budget"] = spikes_3x_budget
        stats["spike_pct_2x_budget"] = 100.0 * spikes_2x_budget / count

    return stats


def scan_logs_for_fatal_errors(run_log_dir: Optional[Path], since_unix_ts: float) -> Optional[str]:
    candidate_logs: List[Path] = []
    if run_log_dir and run_log_dir.exists():
        candidate_logs.extend(sorted(run_log_dir.glob("*.log")))
    elif MEDIA_LOG.exists():
        candidate_logs.append(MEDIA_LOG)

    for log_path in candidate_logs:
        try:
            if log_path.stat().st_mtime + 1.0 < since_unix_ts:
                continue
        except OSError:
            continue

        try:
            with open(log_path, "r", encoding="utf-8", errors="ignore") as f:
                for line in f:
                    for pattern, label in FATAL_LOG_PATTERNS:
                        if pattern.search(line):
                            return f"Fatal log pattern '{label}' found in {log_path.name}"
        except Exception:
            continue

    return None


def verify_runtime_hook_activity(api: str, run_log_dir: Optional[Path], since_unix_ts: float) -> Optional[str]:
    if not run_log_dir or not run_log_dir.exists():
        return "Run log directory not found for hook validation"

    log_files: List[Path] = []
    hook_debug_log = run_log_dir / "hook_debug.log"
    if hook_debug_log.exists():
        log_files.append(hook_debug_log)
    if api in {"vulkan", "vulkan_fg"}:
        vulkan_layer_log = run_log_dir / "vulkan_layer.log"
        if vulkan_layer_log.exists():
            log_files.append(vulkan_layer_log)

    patterns = HOOK_RUNTIME_PATTERNS.get(api)
    if not patterns or not log_files:
        return None

    for log_path in log_files:
        try:
            if log_path.stat().st_mtime + 1.0 < since_unix_ts:
                continue
        except OSError:
            continue

        try:
            with open(log_path, "r", encoding="utf-8", errors="ignore") as f:
                for line in f:
                    for pattern in patterns:
                        if pattern.search(line):
                            return None
        except Exception:
            continue

    checked_logs = ", ".join(path.name for path in log_files)
    return f"No runtime hook activity detected for {api} in {checked_logs}"


def evaluate_quality(
    stats: Dict[str, Any],
    total_record_s: float,
    min_frames: int,
    target_fps: int,
    min_frame_ratio: float,
    max_avg_frame_ratio: float,
    max_frame_spike_ratio: float,
    max_spike_pct: float,
) -> Optional[str]:
    if "error" in stats:
        return str(stats["error"])

    effective_count = int(stats.get("effective_count", stats["count"]))
    required_frames = min_frames
    if target_fps > 0 and min_frame_ratio > 0.0 and total_record_s > 0.0:
        required_frames = max(required_frames, int(target_fps * total_record_s * min_frame_ratio))
    stats["required_frames"] = required_frames

    if effective_count < required_frames:
        return f"Frame count below threshold ({effective_count} < {required_frames})"

    target_frame_time_ms = float(stats.get("target_frame_time_ms", 0.0))
    if target_frame_time_ms <= 0.0:
        return None

    if max_avg_frame_ratio > 0.0 and float(stats["avg"]) > target_frame_time_ms * max_avg_frame_ratio:
        return (
            f"Average frame time too high ({float(stats['avg']):.2f}ms > "
            f"{target_frame_time_ms * max_avg_frame_ratio:.2f}ms)"
        )

    if max_frame_spike_ratio > 0.0 and float(stats["max"]) > target_frame_time_ms * max_frame_spike_ratio:
        return (
            f"Worst frame time too high ({float(stats['max']):.2f}ms > "
            f"{target_frame_time_ms * max_frame_spike_ratio:.2f}ms)"
        )

    if max_spike_pct > 0.0 and float(stats.get("spike_pct_2x_budget", 0.0)) > max_spike_pct:
        return f"Too many >2x-budget spikes ({float(stats['spike_pct_2x_budget']):.1f}% > " f"{max_spike_pct:.1f}%)"

    return None
