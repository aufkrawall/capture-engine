#!/usr/bin/env python3
"""
Automated performance test script for capture system
Runs DX12 and Vulkan test apps with recording, analyzes frame times

Usage:
    python run_tests.py                    # Run all tests with defaults
    python run_tests.py --resolution 1920 1080  # Custom resolution
    python run_tests.py --gpu-load 20      # Custom GPU load
    python run_tests.py --tests 3          # Number of test iterations per API
"""

import os
import sys
import time
import subprocess
import argparse
import csv
import statistics
from pathlib import Path
from datetime import datetime

# Paths
SCRIPT_DIR = Path(__file__).parent.absolute()
PROJECT_ROOT = SCRIPT_DIR.parent
TESTAPP_BIN = SCRIPT_DIR / "bin"
CAPTURE_BIN = PROJECT_ROOT / "build" / "bin"
FRAME_TIMES_CSV = CAPTURE_BIN / "logs" / "frame_times.csv"

def kill_processes():
    """Kill any existing test/capture processes"""
    for proc in ["captureengine.exe", "dx12_test.exe", "vulkan_test.exe", "dx11_test.exe", "dx9_test.exe"]:
        subprocess.run(["taskkill", "/F", "/IM", proc], 
                      stdout=subprocess.DEVNULL,
                      stderr=subprocess.DEVNULL)
    time.sleep(1)

def start_test_app(api, width, height, gpu_load):
    """Start test app and return process"""
    if api == "dx12":
        exe = TESTAPP_BIN / "dx12_test.exe"
    elif api == "dx11":
        exe = TESTAPP_BIN / "dx11_test.exe"
    elif api == "dx9":
        exe = TESTAPP_BIN / "dx9_test.exe"
    else:
        exe = TESTAPP_BIN / "vulkan_test.exe"
    
    if not exe.exists():
        print(f"ERROR: {exe} not found. Run 'python build.py' first.")
        return None
    
    proc = subprocess.Popen([str(exe), str(width), str(height), str(gpu_load)],
                           stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
    return proc

def start_auto_record(delay_ms, duration_ms):
    """Start captureengine with --auto-record"""
    exe = CAPTURE_BIN / "captureengine.exe"
    if not exe.exists():
        print(f"ERROR: {exe} not found")
        return None
    
    proc = subprocess.Popen(
        [str(exe), f"--auto-record={delay_ms},{duration_ms}"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL
    )
    return proc

def parse_frame_times(csv_path):
    """Parse frame_times.csv and return recording frame times in ms"""
    if not csv_path.exists():
        return []
    
    frame_times = []
    try:
        with open(csv_path, 'r') as f:
            reader = csv.reader(f)
            header = next(reader, None)
            
            for row in reader:
                if len(row) >= 6:
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

def analyze_frame_times(frame_times, name=""):
    """Analyze frame times and return stats dict"""
    if not frame_times:
        return {"name": name, "error": "No frames"}
    
    stats = {
        "name": name,
        "count": len(frame_times),
        "min": min(frame_times),
        "max": max(frame_times),
        "avg": statistics.mean(frame_times),
        "median": statistics.median(frame_times),
        "stdev": statistics.stdev(frame_times) if len(frame_times) > 1 else 0,
        "variance": max(frame_times) - min(frame_times),
    }
    
    stats["spikes_10ms"] = sum(1 for ft in frame_times if ft > 10)
    stats["spikes_12ms"] = sum(1 for ft in frame_times if ft > 12)
    stats["spikes_15ms"] = sum(1 for ft in frame_times if ft > 15)
    stats["spikes_20ms"] = sum(1 for ft in frame_times if ft > 20)
    
    stats["spike_pct_10ms"] = 100 * stats["spikes_10ms"] / stats["count"]
    stats["spike_pct_12ms"] = 100 * stats["spikes_12ms"] / stats["count"]
    
    return stats

def print_stats(stats):
    """Pretty print stats"""
    if "error" in stats:
        print(f"  ERROR: {stats['error']}")
        return
    
    print(f"  Frames: {stats['count']}")
    print(f"  Min: {stats['min']:.2f}ms, Max: {stats['max']:.2f}ms, Avg: {stats['avg']:.2f}ms")
    print(f"  Median: {stats['median']:.2f}ms, StdDev: {stats['stdev']:.2f}ms")
    print(f"  Variance (max-min): {stats['variance']:.2f}ms")
    print(f"  Spikes >10ms: {stats['spikes_10ms']} ({stats['spike_pct_10ms']:.1f}%)")
    print(f"  Spikes >12ms: {stats['spikes_12ms']} ({stats['spike_pct_12ms']:.1f}%)")
    if stats['spikes_20ms'] > 0:
        print(f"  Spikes >20ms: {stats['spikes_20ms']}")

def run_single_test(api, width, height, gpu_load, total_record_s, test_name):
    """Run a single test"""
    print(f"\n{'='*60}")
    print(f"TEST: {test_name}")
    print(f"  API: {api.upper()}, Resolution: {width}x{height}, GPU Load: {gpu_load}")
    print(f"  Recording duration: {total_record_s}s")
    print('='*60)
    
    # Clear old frame times
    if FRAME_TIMES_CSV.exists():
        os.remove(FRAME_TIMES_CSV)
    
    # Start test app FIRST
    print("Starting test app...")
    app_proc = start_test_app(api, width, height, gpu_load)
    if not app_proc:
        return None
    
    # Wait for app to fully initialize
    time.sleep(3)
    
    # Start captureengine with auto-record
    # Delay 2s for injection, then record for total_record_s
    delay_ms = 2000
    duration_ms = int(total_record_s * 1000)
    print(f"Starting capture (delay={delay_ms}ms, record={duration_ms}ms)...")
    capture_proc = start_auto_record(delay_ms, duration_ms)
    if not capture_proc:
        app_proc.terminate()
        return None
    
    # Wait for recording to complete
    total_wait = (delay_ms + duration_ms) / 1000 + 3
    print(f"  Waiting {total_wait:.0f}s for recording to complete...")
    time.sleep(total_wait)
    
    # Stop test app
    print("Stopping test app...")
    app_proc.terminate()
    try:
        app_proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        app_proc.kill()
    
    # Give capture time to finish
    time.sleep(2)
    kill_processes()
    time.sleep(1)
    
    # Parse and analyze results
    frame_times = parse_frame_times(FRAME_TIMES_CSV)
    stats = analyze_frame_times(frame_times, test_name)
    
    print("\nResults:")
    print_stats(stats)
    
    return stats

def main():
    parser = argparse.ArgumentParser(description="Run capture performance tests")
    parser.add_argument("--resolution", type=int, nargs=2, default=[3840, 2160],
                       metavar=("WIDTH", "HEIGHT"), help="Test resolution (default: 3840 2160)")
    parser.add_argument("--gpu-load", type=int, default=15,
                       help="GPU load passes per frame (default: 15)")
    parser.add_argument("--tests", type=int, default=3,
                       help="Number of test iterations per API (default: 3)")
    parser.add_argument("--duration", type=float, default=10.0,
                       help="Total recording duration per test in seconds (default: 10)")
    parser.add_argument("--api", choices=["dx12", "dx11", "dx9", "vulkan", "both", "all"], default="all",
                       help="Which API to test (default: all)")
    
    args = parser.parse_args()
    width, height = args.resolution
    
    print("="*60)
    print("CAPTURE PERFORMANCE TEST SUITE")
    print("="*60)
    print(f"Timestamp: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    print(f"Resolution: {width}x{height}")
    print(f"GPU Load: {args.gpu_load} passes/frame")
    print(f"Tests per API: {args.tests}")
    print(f"Recording duration: {args.duration}s per test")
    
    # Check binaries exist
    if not (TESTAPP_BIN / "dx12_test.exe").exists():
        print(f"\nERROR: Test apps not found in {TESTAPP_BIN}")
        print("Run 'python build.py' first to build the test apps.")
        sys.exit(1)
    
    print("\nCleaning up existing processes...")
    kill_processes()
    
    all_stats = []
    
    if args.api == "all":
        apis_to_test = ["dx12", "dx11", "dx9", "vulkan"]
    elif args.api == "both":
        apis_to_test = ["dx12", "vulkan"]
    else:
        apis_to_test = [args.api]
    
    for api in apis_to_test:
        for test_num in range(1, args.tests + 1):
            test_name = f"{api.upper()} Test {test_num}"
            stats = run_single_test(
                api=api,
                width=width,
                height=height,
                gpu_load=args.gpu_load,
                total_record_s=args.duration,
                test_name=test_name
            )
            if stats and "error" not in stats:
                all_stats.append(stats)
            
            kill_processes()
            time.sleep(2)
    
    # Summary
    print("\n" + "="*60)
    print("SUMMARY")
    print("="*60)
    
    if not all_stats:
        print("No test results collected!")
        return
    
    for api in apis_to_test:
        api_stats = [s for s in all_stats if api.upper() in s.get("name", "")]
        if not api_stats:
            continue
        
        print(f"\n{api.upper()} ({len(api_stats)} tests):")
        
        avg_min = statistics.mean(s["min"] for s in api_stats)
        avg_max = statistics.mean(s["max"] for s in api_stats)
        avg_avg = statistics.mean(s["avg"] for s in api_stats)
        avg_variance = statistics.mean(s["variance"] for s in api_stats)
        total_spikes = sum(s["spikes_12ms"] for s in api_stats)
        total_frames = sum(s["count"] for s in api_stats)
        spike_pct = 100 * total_spikes / total_frames if total_frames > 0 else 0
        
        print(f"  Avg frame time: {avg_avg:.2f}ms (range: {avg_min:.2f}-{avg_max:.2f}ms)")
        print(f"  Avg variance: {avg_variance:.2f}ms")
        print(f"  Total spikes >12ms: {total_spikes}/{total_frames} ({spike_pct:.1f}%)")
    
    print("\n" + "="*60)
    print("TEST COMPLETE")
    print("="*60)

if __name__ == "__main__":
    main()
