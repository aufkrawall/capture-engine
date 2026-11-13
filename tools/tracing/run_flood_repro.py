#!/usr/bin/env python3
"""Runs tools/tracing/flood_repro.cpp under an unelevated ETW session and lines its timeline up with dwm.exe's.

Builds the repro with the project's clang (build/msys64/clang64) into build/tracing/flood_repro.exe when the source is
newer, then per run records Dwm-Core (all keywords), DxgKrnl (base + performance) and Win32k (composition updates)
into a QPC-clocked ETL, digests it with dwm_flood_digest.py and deletes it. Each run takes over the primary monitor
for a few seconds with a borderless window (dark blue = old swapchain, dark green = new one) and, in clip modes,
confines the cursor to it. Refuses to start while GTA V Enhanced or Capture Engine runs.

    python tools/tracing/run_flood_repro.py clip:10000:0:56000:30000 [none:2000:3000 ...] [--settle-s 35]

A run spec is mode[:spinBeforeMs[:spinAfterMs[:rate[:maxPostMs]]]] (flood_repro.cpp's arguments; rate 0 = free
running). Settle time between runs lets a backlog from the previous run drain first. Reference result (5.9 h
uptime): clip at 45000/s shows the new swapchain after 6 ms, at 56000/s for 10 s after 1063 ms.
"""

import argparse
import os
import re
import subprocess
import sys
import time
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import dwm_flood_digest  # noqa: E402

PROJECT_ROOT = os.path.dirname(os.path.dirname(HERE))
SOURCE = os.path.join(HERE, "flood_repro.cpp")
EXE = os.path.join(PROJECT_ROOT, "build", "tracing", "flood_repro.exe")
CLANG = os.path.join(PROJECT_ROOT, "build", "msys64", "clang64", "bin", "clang++.exe")
SESSION = "CE_FloodRepro"
PROVIDERS = [
    ("{9e9bba3c-2e38-40cb-99f4-9e8281425164}", "0xFFFFFFFFFFFFFFFF", "4"),  # Microsoft-Windows-Dwm-Core
    ("{802ec45a-1e99-4b83-9920-87c98277ba9d}", "0x8000000000000001", "5"),  # Microsoft-Windows-DxgKrnl
    ("{8c416c79-d49b-4f01-a467-e56d3aa8234c}", "0x8000000400001000", "4"),  # Microsoft-Windows-Win32k
]
RUN_TIMEOUT_S = 150


def parse_repro_output(text):
    """flood_repro's stdout as {header values, 'events': {name: qpc}, 'spin': [cycles per 100 ms]}."""
    lines = text.splitlines()
    if not lines or not lines[0].startswith("repro "):
        raise ValueError("not flood_repro output")
    result: dict = {key: int(value) for key, value in re.findall(r"(\w+)=(-?\d+)", lines[0])}
    result["events"] = {name: int(qpc) for name, qpc in re.findall(r"^event (\w+) qpc=(\d+)", text, re.M)}
    spin_line = next((line for line in lines if line.startswith("spin per100ms:")), "spin per100ms:")
    result["spin"] = [int(value) for value in spin_line.split(":", 1)[1].split()]
    return result


def summarize_run(repro, digest, dwm_pid):
    """Per-500 ms rows from the spin start (spinner calls/s, dwm channel-2 batches/s) and the summary line."""
    freq = digest.perf_freq
    scale = freq / repro["freq"]
    events = {name: qpc * scale for name, qpc in repro["events"].items()}
    switch = events.get("switch")
    if switch is None:
        return ["no swapchain switch in the repro output"]
    origin = repro["origin"] * scale
    buckets = dwm_flood_digest.channel_buckets(digest, str(dwm_pid))
    spin_by_bucket = {int((origin + index * freq / 10) * 10 // freq): cycles
                      for index, cycles in enumerate(repro["spin"])}
    marks = defaultdict(list)
    for marker in digest.markers:
        label = dwm_flood_digest.marker_label(marker, repro["pid"], set())
        if label and marker.qpc >= switch:
            marks[int(marker.qpc * 10 // freq)].append(label)
    lines = ["  t(s from switch)  calls/s  dwm-ch2/s  phase / markers"]
    phase_at = {int(qpc * 10 // freq): name for name, qpc in events.items()}
    first = int(events.get("spin_start", switch) * 10 // freq) - 2
    last = int(events.get("end", switch) * 10 // freq)
    for row in range(first, last + 1, 5):
        rows = range(row, row + 5)
        calls = sum(spin_by_bucket.get(b, 0) for b in rows) * 2
        handled = sum(buckets.get(b, 0) for b in rows) * 2
        tags = [phase_at[b] for b in rows if b in phase_at] + [m for b in rows for m in dict.fromkeys(marks[b])]
        lines.append(f"  {(row * freq / 10 - switch) / freq:8.1f}  {calls:8d}  {handled:9d}  {' '.join(tags)[:90]}")
    realized = dwm_flood_digest.new_surface_realization(digest, int(switch))
    flood = dwm_flood_digest.floods(buckets)

    def since(qpc):
        return f"{(qpc - switch) * 1000 / freq:+.0f} ms" if qpc is not None else "never"

    flood_end = (flood[-1][1] + 1) * freq / 10 if flood else None
    lines.append(f"  SUMMARY new surface realized {since(realized[0] if realized else None)}, spin stopped "
                 f"{since(events.get('spin_stop'))}, compositor flood ends {since(flood_end)}; "
                 f"{sum(buckets.values())} channel-2 batches for {sum(repro['spin'])} spinner cycles")
    return lines


def build_repro():
    if os.path.exists(EXE) and os.path.getmtime(EXE) >= os.path.getmtime(SOURCE):
        return
    if not os.path.exists(CLANG):
        sys.exit(f"project clang not found at {CLANG}; run build.py once first")
    os.makedirs(os.path.dirname(EXE), exist_ok=True)
    subprocess.run([CLANG, "-O2", "-std=c++20", "-Wall", "-Wextra", "-static", "-o", EXE, SOURCE,
                    "-ld3d11", "-ldxgi", "-limm32", "-luser32"], check=True)


def running(image):
    out = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {image}", "/FO", "CSV", "/NH"], capture_output=True,
                         text=True, encoding="oem", errors="replace").stdout
    return [int(line.split('","')[1]) for line in out.splitlines() if line.startswith(f'"{image}"')]


def logman(*args):
    return subprocess.run(["logman", *args], capture_output=True, text=True, encoding="oem", errors="replace")


def record_run(spec, etl):
    """Runs one spec under the ETW session; returns the repro's stdout."""
    logman("stop", SESSION, "-ets")
    guid, keywords, level = PROVIDERS[0]
    started = logman("start", SESSION, "-ets", "-o", etl, "-ct", "perf", "-mode", "Circular", "-max", "4000",
                     "-bs", "1024", "-nb", "64", "512", "-p", guid, keywords, level)
    if started.returncode:
        sys.exit(f"logman start failed: {started.stdout.strip()} {started.stderr.strip()}")
    for guid, keywords, level in PROVIDERS[1:]:
        if logman("update", SESSION, "-ets", "-p", guid, keywords, level).returncode:
            print(f"  provider {guid} could not be enabled")
    try:
        return subprocess.run([EXE, *spec.split(":")], capture_output=True, text=True,
                              timeout=RUN_TIMEOUT_S).stdout
    except subprocess.TimeoutExpired as error:
        subprocess.run(["taskkill", "/F", "/IM", "flood_repro.exe"], capture_output=True)
        print("  repro timed out and was killed")
        return error.stdout.decode(errors="replace") if isinstance(error.stdout, bytes) else (error.stdout or "")
    finally:
        logman("stop", SESSION, "-ets")


def main(argv=None):
    parser = argparse.ArgumentParser(description=(__doc__ or "").split("\n\n")[0])
    parser.add_argument("specs", nargs="+")
    parser.add_argument("--settle-s", type=float, default=35.0, help="wait between runs so a backlog drains")
    parser.add_argument("--keep-etl", action="store_true")
    args = parser.parse_args(argv)
    if running("GTA5_Enhanced.exe") or running("captureengine.exe"):
        sys.exit("GTA V Enhanced or Capture Engine is running; not starting the repro")
    build_repro()
    dwm_pid = running("dwm.exe")[0]
    out_dir = os.path.dirname(EXE)
    for index, spec in enumerate(args.specs):
        if index:
            time.sleep(args.settle_s)
        etl = os.path.join(out_dir, f"flood_repro_{index:02d}.etl")
        output = record_run(spec, etl)
        print(f"=== {spec}")
        try:
            repro = parse_repro_output(output)
        except ValueError:
            print(f"  no usable repro output: {output[:300]!r}")
            continue
        print("  " + "\n  ".join(line for line in output.splitlines() if not line.startswith("spin per100ms")))
        digest = dwm_flood_digest.read_digest(etl)
        if not args.keep_etl:
            os.remove(etl)
        print("\n".join(summarize_run(repro, digest, dwm_pid)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
