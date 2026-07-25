#!/usr/bin/env python3
r"""
gpu_trace.py - Automated GPUView (kernel GPU-scheduler) capture for the DX12 32-bit
Alt+Tab freeze investigation, optionally combined with the in-process D3D12 debug layer
and the focus-analysis flight recorder. You trigger the freeze with Alt+Tab; the script
does everything else.

Requires an ELEVATED (Administrator) shell — xperf kernel logging needs admin.
Uses the in-box Windows Performance Toolkit GPUView capture (gpuview\\log.cmd) which is
already installed; produces a GPUView-compatible Merged.etl.

What one capture run gathers:
  - Merged.etl (open in GPUView.exe) — the kernel GPU-scheduler timeline: per-process GPU
    queues, DMA packets, paging packets (residency), preemption, and the TDR/reset, so you
    can see whether the app's queue is blocked behind a paging op vs a dxgkrnl device-lock
    held by DWM during the iflip<->composited switch.
  - hook_debug.log — the focus-analysis flight recorder (residency budget/usage + present
    gap) and, with --debug-layer, any D3D12 validation errors across the mode switch.

Examples (run from an elevated shell):
  python tools\gpu_trace.py capture                 # clean GPUView trace + flight recorder
  python tools\gpu_trace.py capture --debug-layer 1 # also enable the D3D12 debug layer (combined)
  python tools\gpu_trace.py capture --open          # auto-open Merged.etl in GPUView when done
  python tools\gpu_trace.py parse <Merged.etl>      # coarse summary (paging packets / TDR / gaps)
"""

import argparse
import ctypes
import os
import shutil
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CE_DIR = os.path.join(REPO, "installed", "captureengine")
LOGS_DIR = os.path.join(CE_DIR, "logs")
CONFIG_INI = os.path.join(CE_DIR, "config.ini")
DEBUG_LAYER_FLAG = os.path.join(CE_DIR, "ce_dx12_debug_layer")
CONTROLLER = os.path.join(CE_DIR, "captureengine.exe")
DX12_TEST_X86 = os.path.join(REPO, "installed", "testapp", "x86", "dx12_test.exe")

WPT_DIR = r"C:\Program Files (x86)\Windows Kits\10\Windows Performance Toolkit"
GPUVIEW_DIR = os.path.join(WPT_DIR, "gpuview")
LOG_CMD = os.path.join(GPUVIEW_DIR, "log.cmd")
GPUVIEW_EXE = os.path.join(GPUVIEW_DIR, "GPUView.exe")
XPERF = os.path.join(WPT_DIR, "xperf.exe")
MERGED_ETL = os.path.join(GPUVIEW_DIR, "Merged.etl")


def is_admin() -> bool:
    try:
        return bool(ctypes.windll.shell32.IsUserAnAdmin())
    except Exception:
        return False


def is_running(exe_name: str) -> bool:
    try:
        out = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {exe_name}"],
                             capture_output=True, text=True, encoding="utf-8",
                             errors="replace", timeout=10).stdout or ""
        return exe_name.lower() in out.lower()
    except Exception:
        return False


def taskkill(exe_name: str):
    try:
        subprocess.run(["taskkill", "/F", "/IM", exe_name], capture_output=True, text=True,
                       encoding="utf-8", errors="replace", timeout=10)
    except Exception:
        pass


# ---- config / flags ------------------------------------------------------------------
def set_config_bool(key: str, value: bool):
    if not os.path.exists(CONFIG_INI):
        print(f"[warn] {CONFIG_INI} not found")
        return
    import re
    with open(CONFIG_INI, "r", encoding="utf-8") as f:
        lines = f.readlines()
    val = "true" if value else "false"
    pat = re.compile(rf"^\s*;?\s*{re.escape(key)}\s*=", re.IGNORECASE)
    done = False
    for i, ln in enumerate(lines):
        if pat.match(ln):
            lines[i] = f"{key}={val}\n"
            done = True
            break
    if not done:
        for i, ln in enumerate(lines):
            if ln.strip().lower() == "[overlay]":
                lines.insert(i + 1, f"{key}={val}\n")
                done = True
                break
    with open(CONFIG_INI, "w", encoding="utf-8") as f:
        f.writelines(lines)
    print(f"[config] {key}={val} ({'set' if done else 'FAILED'})")


def set_debug_layer(level: int):
    if level > 0:
        with open(DEBUG_LAYER_FLAG, "w", encoding="utf-8") as f:
            f.write(str(level))
        print(f"[flag] D3D12 debug layer level {level} (ce_dx12_debug_layer)")
    elif os.path.exists(DEBUG_LAYER_FLAG):
        os.remove(DEBUG_LAYER_FLAG)
        print("[flag] D3D12 debug layer disabled")


# ---- GPUView trace via the official log.cmd ------------------------------------------
def trace_start(profile: str):
    print(f"[trace] starting GPUView capture (profile={profile}) ...")
    # First invocation (TLOG unset) starts the loggers.
    r = subprocess.run(["cmd", "/c", "log.cmd", profile], cwd=GPUVIEW_DIR,
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    if "Invalid command line" in (r.stdout + r.stderr):
        sys.exit(f"[error] log.cmd rejected profile '{profile}'")
    print("[trace] loggers started")


def trace_stop() -> str:
    print("[trace] stopping GPUView capture + merging ...")
    if os.path.exists(MERGED_ETL):
        try:
            os.remove(MERGED_ETL)
        except OSError:
            pass
    # Second invocation: inject TLOG so log.cmd takes the stop+merge path.
    env = dict(os.environ)
    env["TLOG"] = "NORMAL"
    subprocess.run(["cmd", "/c", "log.cmd"], cwd=GPUVIEW_DIR, env=env,
                   capture_output=True, text=True, encoding="utf-8", errors="replace")
    for _ in range(30):
        if os.path.exists(MERGED_ETL):
            return MERGED_ETL
        time.sleep(1.0)
    return MERGED_ETL if os.path.exists(MERGED_ETL) else ""


# ---- capture orchestration -----------------------------------------------------------
def newest_logdir():
    if not os.path.isdir(LOGS_DIR):
        return None
    subs = [os.path.join(LOGS_DIR, d) for d in os.listdir(LOGS_DIR)
            if os.path.isdir(os.path.join(LOGS_DIR, d))]
    return max(subs, key=os.path.getmtime) if subs else None


def find_new_dump(run_logdir, since):
    if not run_logdir or not os.path.isdir(run_logdir):
        return None
    for f in os.listdir(run_logdir):
        if f.lower().endswith(".dmp") and os.path.getmtime(os.path.join(run_logdir, f)) >= since:
            return os.path.join(run_logdir, f)
    return None


def do_capture(profile: str, debug_layer: int, timeout: int, auto_open: bool):
    if not is_admin():
        sys.exit("[error] must run from an ELEVATED (Administrator) shell — xperf needs admin.")
    for p in (LOG_CMD, XPERF, CONTROLLER, DX12_TEST_X86):
        if not os.path.exists(p):
            sys.exit(f"[error] missing: {p}")

    set_config_bool("observer_only", False)         # overlay ON (the freezing case)
    set_config_bool("dx12_focus_analysis", True)    # flight recorder
    set_debug_layer(debug_layer)

    pre_dirs = set(os.listdir(LOGS_DIR)) if os.path.isdir(LOGS_DIR) else set()
    trace_start(profile)
    start_ts = time.time()

    controller_ours = False
    if not is_running("captureengine.exe"):
        print("[run] starting captureengine.exe controller ...")
        subprocess.Popen([CONTROLLER], cwd=CE_DIR)
        controller_ours = True
        time.sleep(3.0)
    else:
        print("[run] reusing already-running captureengine.exe")

    print("[run] launching 32-bit dx12_test.exe ...")
    subprocess.Popen([DX12_TEST_X86], cwd=os.path.dirname(DX12_TEST_X86))

    print("\n  >>> ALT+TAB now (in/out of the dx12_test window) to trigger the freeze. <<<")
    print(f"  Auto-stops when a crash/FREEZE dump appears, or after {timeout}s.\n")

    run_logdir = None
    dump = None
    deadline = time.time() + timeout
    while time.time() < deadline:
        if run_logdir is None and os.path.isdir(LOGS_DIR):
            new = [d for d in os.listdir(LOGS_DIR) if d not in pre_dirs
                   and os.path.isdir(os.path.join(LOGS_DIR, d))]
            if new:
                run_logdir = os.path.join(LOGS_DIR, sorted(new)[-1])
                print(f"[run] this run's log dir: {run_logdir}")
        dump = find_new_dump(run_logdir, start_ts)
        if dump:
            print(f"[run] dump detected: {os.path.basename(dump)} — letting it settle, then stopping.")
            time.sleep(3.0)
            break
        time.sleep(1.0)
    else:
        print("[run] timeout reached (no dump seen) — stopping trace anyway.")

    etl = trace_stop()
    taskkill("dx12_test.exe")
    if controller_ours:
        taskkill("captureengine.exe")

    # Collect results next to the run's logs.
    out_dir = run_logdir or newest_logdir() or CE_DIR
    saved_etl = ""
    if etl and os.path.exists(etl):
        saved_etl = os.path.join(out_dir, "Merged.etl")
        try:
            shutil.move(etl, saved_etl)
        except Exception as e:
            print(f"[warn] could not move Merged.etl: {e}; left at {etl}")
            saved_etl = etl
        print(f"[done] GPU trace: {saved_etl}")
    else:
        print("[warn] no Merged.etl produced (trace may have failed; check admin + xperf).")

    print(f"[done] logs: {out_dir}\\hook_debug.log")
    if saved_etl:
        parse_etl(saved_etl)
        if auto_open and os.path.exists(GPUVIEW_EXE):
            print("[done] opening GPUView ...")
            subprocess.Popen([GPUVIEW_EXE, saved_etl])


# ---- coarse ETL parse (best-effort, no GUI) -----------------------------------------
def parse_etl(etl_path: str):
    if not os.path.exists(XPERF):
        return
    print(f"\n[parse] coarse scan of {os.path.basename(etl_path)} (xperf dumper) ...")
    dump_txt = etl_path + ".dump.txt"
    try:
        with open(dump_txt, "w", encoding="utf-8", errors="replace") as out:
            subprocess.run([XPERF, "-i", etl_path, "-a", "dumper"], stdout=out,
                           stderr=subprocess.DEVNULL, timeout=600)
    except Exception as e:
        print(f"[parse] xperf dumper failed: {e}")
        return
    counts = {"PagingQueuePacket": 0, "DmaPacket": 0, "QueuePacket": 0,
              "Preempt": 0, "DeviceRemoved": 0, "TdrInfo": 0, "Reset": 0}
    try:
        with open(dump_txt, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                for key in counts:
                    if key in line:
                        counts[key] += 1
    except Exception as e:
        print(f"[parse] could not read dump: {e}")
        return
    print("[parse] event counts (coarse):")
    for k, v in counts.items():
        print(f"    {k:18} {v}")
    print(f"[parse] full dumper text: {dump_txt}")
    print("[parse] Open Merged.etl in GPUView for the visual timeline: red=paging, black=preemption,")
    print("[parse] red cross-hatch=present. Look at the app's 3D queue during the ~2s stall.")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    pc = sub.add_parser("capture", help="run an automated GPUView capture (you Alt+Tab to trigger)")
    pc.add_argument("--profile", default="normal", choices=["normal", "present", "light", "min", "verbose"])
    pc.add_argument("--debug-layer", type=int, default=0, choices=[0, 1, 2],
                    help="also enable the D3D12 debug layer (1) / + GPU validation (2)")
    pc.add_argument("--timeout", type=int, default=60, help="max seconds to wait for the freeze")
    pc.add_argument("--open", action="store_true", help="auto-open Merged.etl in GPUView when done")
    pp = sub.add_parser("parse", help="coarse-scan an existing Merged.etl")
    pp.add_argument("etl")
    args = ap.parse_args()

    if args.cmd == "capture":
        do_capture(args.profile, args.debug_layer, args.timeout, args.open)
    elif args.cmd == "parse":
        parse_etl(args.etl)


if __name__ == "__main__":
    main()
