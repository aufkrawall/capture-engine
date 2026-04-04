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
DEFAULT_RESULTS_JSON = CAPTURE_BIN / "logs" / "integration_results.json"
RUN_LOG_DIR_RE = re.compile(r"^\d{8}_\d{6}$")
CAPTURE_CONFIG = CAPTURE_BIN / "config.ini"
CAPTURE_CONFIG_TEMPLATE = PROJECT_ROOT / "captureengine" / "config.ini.template"

SUPPORTED_APIS = [
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
API_EXECUTABLES = {
    "dx12": "dx12_test.exe",
    "dx11": "dx11_test.exe",
    "dx9": "dx9_test.exe",
    "dx9ex": "dx9ex_test.exe",
    "dx8": "dx8_test.exe",
    "dx7": "dx7_test.exe",
    "dx6": "dx6_test.exe",
    "vulkan": "vulkan_test.exe",
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
    "opengl": {"opengl"},
    "opengl_legacy": {"opengl_legacy", "opengl"},
    "directdraw7": {"directdraw7", "directdraw", "ddraw"},
}


def ensure_capture_config_exists() -> str:
    if CAPTURE_CONFIG.exists():
        return CAPTURE_CONFIG.read_text(encoding="utf-8")

    if CAPTURE_CONFIG_TEMPLATE.exists():
        text = CAPTURE_CONFIG_TEMPLATE.read_text(encoding="utf-8")
    else:
        text = "[General]\ndebug_logging=true\n\n[Injection]\nwhitelist=(\n)\n"

    CAPTURE_CONFIG.parent.mkdir(parents=True, exist_ok=True)
    CAPTURE_CONFIG.write_text(text, encoding="utf-8")
    return text


def add_whitelist_entries_to_config_text(config_text: str, executable_names: List[str]) -> str:
    lines = config_text.splitlines(keepends=True)
    executable_names = [name for name in executable_names if name]
    if not executable_names:
        return config_text

    injection_start = None
    for idx, line in enumerate(lines):
        if line.strip().lower() == "[injection]":
            injection_start = idx
            break

    if injection_start is None:
        if lines and not lines[-1].endswith("\n"):
            lines[-1] += "\n"
        if lines and lines[-1].strip():
            lines.append("\n")
        lines.extend(["[Injection]\n", "whitelist=(\n", ")\n"])
        injection_start = len(lines) - 3

    section_end = len(lines)
    for idx in range(injection_start + 1, len(lines)):
        stripped = lines[idx].strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            section_end = idx
            break

    whitelist_start = None
    for idx in range(injection_start + 1, section_end):
        if lines[idx].strip().lower().startswith("whitelist=("):
            whitelist_start = idx
            break

    if whitelist_start is None:
        insert_idx = injection_start + 1
        block = ["whitelist=(\n", ")\n"]
        lines[insert_idx:insert_idx] = block
        whitelist_start = insert_idx
        section_end += len(block)

    whitelist_end = None
    for idx in range(whitelist_start + 1, section_end):
        if lines[idx].strip() == ")":
            whitelist_end = idx
            break

    if whitelist_end is None:
        whitelist_end = section_end
        lines.insert(whitelist_end, ")\n")

    existing_entries = {
        lines[idx].strip().lower()
        for idx in range(whitelist_start + 1, whitelist_end)
        if lines[idx].strip() and not lines[idx].lstrip().startswith(";")
    }

    new_lines = []
    for executable_name in executable_names:
        if executable_name.lower() not in existing_entries:
            new_lines.append(f"{executable_name}\n")
            existing_entries.add(executable_name.lower())

    if not new_lines:
        return config_text

    lines[whitelist_end:whitelist_end] = new_lines
    return "".join(lines)


def ensure_testapp_whitelist_entries(executable_names: List[str]) -> Optional[Tuple[bool, str]]:
    config_existed = CAPTURE_CONFIG.exists()
    original_text = ensure_capture_config_exists()
    updated_text = add_whitelist_entries_to_config_text(original_text, executable_names)
    if updated_text == original_text:
        return None

    CAPTURE_CONFIG.write_text(updated_text, encoding="utf-8")
    added_names = ", ".join(executable_names)
    print(f"Temporarily added capture whitelist entries: {added_names}")
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


def analyze_frame_times(frame_times: List[float], name: str = "") -> Dict[str, Any]:
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

    return stats


def print_stats(stats: Dict[str, Any]) -> None:
    """Pretty-print per-test stats."""
    if "error" in stats:
        print(f"  ERROR: {stats['error']}")
        return

    print(f"  Frames: {stats['count']}")
    if "effective_count" in stats and int(stats["effective_count"]) != int(stats["count"]):
        print(f"  Estimated total frames: {stats['effective_count']}")
    if "source" in stats:
        print(f"  Source: {stats['source']}")
    print(f"  Min: {float(stats['min']):.2f}ms, Max: {float(stats['max']):.2f}ms, " f"Avg: {float(stats['avg']):.2f}ms")
    print(f"  Median: {float(stats['median']):.2f}ms, " f"StdDev: {float(stats['stdev']):.2f}ms")
    print(f"  Variance (max-min): {float(stats['variance']):.2f}ms")
    print(f"  Spikes >10ms: {int(stats['spikes_10ms'])} " f"({float(stats['spike_pct_10ms']):.1f}%)")
    print(f"  Spikes >12ms: {int(stats['spikes_12ms'])} " f"({float(stats['spike_pct_12ms']):.1f}%)")
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
        launch_command = [str(resolve_test_exe(api, arch)), str(width), str(height), str(gpu_load)]

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
    media_log_path = (run_log_dir / "media.log") if run_log_dir else MEDIA_LOG

    frame_times = parse_frame_times(frame_times_csv)
    frame_source = "frame_times.csv"
    if not frame_times:
        perf_logs_dir = run_log_dir if run_log_dir else CAPTURE_BIN / "logs"
        frame_times = parse_perf_metrics_frame_times_from_dir(api, perf_logs_dir, test_start_unix_ts)
        frame_source = "perf_metrics_*.csv"
    estimated_frame_count = 0
    if not frame_times:
        frame_times, estimated_frame_count = parse_media_log_frame_times(media_log_path, test_start_unix_ts)
        frame_source = f"{media_log_path.parent.name}/media.log"

    stats = analyze_frame_times(frame_times, test_name)
    if "error" not in stats:
        stats["source"] = frame_source
        if estimated_frame_count > 0:
            stats["effective_count"] = estimated_frame_count

    print("\nResults:")
    print_stats(stats)

    if "error" in stats:
        return stats, str(stats["error"])

    effective_count = int(stats.get("effective_count", stats["count"]))
    if effective_count < min_frames:
        return stats, f"Frame count below threshold ({effective_count} < {min_frames})"

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
            "dx9",
            "dx9ex",
            "dx8",
            "dx7",
            "dx6",
            "vulkan",
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
        default=str(DEFAULT_RESULTS_JSON),
        help=f"Path to write machine-readable JSON results (default: {DEFAULT_RESULTS_JSON})",
    )
    args = parser.parse_args()

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

    if args.api == "all":
        apis_to_test = list(SUPPORTED_APIS)
    elif args.api == "both":
        apis_to_test = ["dx12", "vulkan"]
    else:
        apis_to_test = [args.api]

    arches_to_test = ["x64", "x86"] if args.arch == "both" else [args.arch]

    print(f"APIs under test: {', '.join(api.upper() for api in apis_to_test)}")
    print(f"Architectures under test: {', '.join(a.upper() for a in arches_to_test)}")

    ensure_binaries_exist(apis_to_test, arches_to_test)
    config_snapshot = ensure_testapp_whitelist_entries([API_EXECUTABLES[api] for api in apis_to_test])

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

        write_results_json(
            output_path=Path(args.results_json),
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
