#!/usr/bin/env python3
"""
dx12_call_trace.py - Run a DX12 app with CaptureEngine injected and the gated DX12 API
call-trace diagnostic enabled, then summarize the traced D3D12 device/queue calls.

Pairs with the CE hook's DX12 call trace (enabled by env CE_DX12_TRACE=1 or a `ce_dx12_trace`
flag file next to capture_hook_*.dll). When enabled, CE -- which is injected next to any other
co-resident injected modules and hooks the app's whole D3D12 device -- logs caller-attributed
D3D12 calls:
  CreateCommandQueue / CreateCommittedResource / CreateDescriptorHeap /
  CreateSwapChain[ForHwnd] / ExecuteCommandLists / Signal
This is useful for inspecting how the overlay -- and any co-resident injected module -- interacts
with the app's D3D12 device (queue usage, resource/descriptor footprint, per-frame
submission/fence pattern) when diagnosing focus/mode-switch and overlay-coexistence issues.
A steady-state run is enough; no focus changes are required to capture the rendering pattern.

Examples:
  python tools/dx12_call_trace.py run --overlay    # CE overlay on; launch app, ~10s, parse
  python tools/dx12_call_trace.py run --observe     # CE observer-only (adds no overlay GPU work)
  python tools/dx12_call_trace.py parse [logdir]    # parse newest (or given) log dir
  python tools/dx12_call_trace.py setup --overlay|--observe   # set flags, run the scenario yourself
  python tools/dx12_call_trace.py cleanup           # remove trace flag, restore observer_only=false
"""

import argparse
import os
import re
import subprocess
import sys
import time
from collections import Counter, defaultdict

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CE_DIR = os.path.join(REPO, "installed", "captureengine")
LOGS_DIR = os.path.join(CE_DIR, "logs")
CONFIG_INI = os.path.join(CE_DIR, "config.ini")
TRACE_FLAG = os.path.join(CE_DIR, "ce_dx12_trace")
CONTROLLER = os.path.join(CE_DIR, "captureengine.exe")
DX12_TEST_X86 = os.path.join(REPO, "installed", "testapp", "x86", "dx12_test.exe")

INFRA_MODULES = {
    "capture_hook_x86.dll", "capture_hook_x64.dll", "d3d12.dll", "d3d12core.dll",
    "dxgi.dll", "kernelbase.dll", "kernel32.dll", "ntdll.dll", "win32u.dll",
}


# --------------------------------------------------------------------------------------
# Flag / config management
# --------------------------------------------------------------------------------------
def set_trace_flag(enabled: bool):
    if enabled:
        with open(TRACE_FLAG, "w", encoding="utf-8") as f:
            f.write("1")
        print(f"[trace] enabled flag: {TRACE_FLAG}")
    elif os.path.exists(TRACE_FLAG):
        os.remove(TRACE_FLAG)
        print(f"[trace] removed flag: {TRACE_FLAG}")


def set_observer_only(value: bool):
    """Set [Overlay] observer_only=value in config.ini, preserving the rest of the file."""
    if not os.path.exists(CONFIG_INI):
        print(f"[warn] {CONFIG_INI} not found; cannot set observer_only")
        return
    with open(CONFIG_INI, "r", encoding="utf-8") as f:
        lines = f.readlines()
    val = "true" if value else "false"
    pat = re.compile(r"^\s*;?\s*observer_only\s*=", re.IGNORECASE)
    replaced = False
    for i, ln in enumerate(lines):
        if pat.match(ln):
            lines[i] = f"observer_only={val}\n"
            replaced = True
            break
    if not replaced:
        for i, ln in enumerate(lines):
            if ln.strip().lower() == "[overlay]":
                lines.insert(i + 1, f"observer_only={val}\n")
                replaced = True
                break
    with open(CONFIG_INI, "w", encoding="utf-8") as f:
        f.writelines(lines)
    print(f"[config] observer_only={val} ({'updated' if replaced else 'FAILED to set'})")


# --------------------------------------------------------------------------------------
# Process helpers (built-in tasklist/taskkill; no third-party dependency)
# --------------------------------------------------------------------------------------
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


# --------------------------------------------------------------------------------------
# Orchestrated run
# --------------------------------------------------------------------------------------
def do_run(observer: bool, seconds: int):
    if not os.path.exists(CONTROLLER):
        sys.exit(f"[error] controller not found: {CONTROLLER} (build CE first)")
    if not os.path.exists(DX12_TEST_X86):
        sys.exit(f"[error] 32-bit test app not found: {DX12_TEST_X86}")

    set_trace_flag(True)
    set_observer_only(observer)

    controller_ours = None
    if is_running("captureengine.exe"):
        print("[run] captureengine.exe already running -- reusing it (will NOT kill it).")
    else:
        print("[run] starting captureengine.exe controller ...")
        controller_ours = subprocess.Popen([CONTROLLER], cwd=CE_DIR)
        time.sleep(3.0)  # let the controller initialize before the app launches

    mode = "observer-only" if observer else "overlay-on"
    print(f"[run] launching 32-bit dx12_test.exe ({mode}); steady-state {seconds}s ...")
    subprocess.Popen([DX12_TEST_X86], cwd=os.path.dirname(DX12_TEST_X86))

    try:
        time.sleep(seconds)
    finally:
        print("[run] stopping processes we started ...")
        taskkill("dx12_test.exe")
        if controller_ours is not None:
            taskkill("captureengine.exe")

    time.sleep(1.0)
    logdir = newest_log_dir()
    if logdir:
        print(f"\n[run] newest log dir: {logdir}\n")
        parse_report(logdir)
    else:
        print("[run] no log dir found to parse.")


# --------------------------------------------------------------------------------------
# Parsing / reporting
# --------------------------------------------------------------------------------------
def newest_log_dir():
    if not os.path.isdir(LOGS_DIR):
        return None
    subs = [os.path.join(LOGS_DIR, d) for d in os.listdir(LOGS_DIR)
            if os.path.isdir(os.path.join(LOGS_DIR, d))]
    if not subs:
        return None
    return max(subs, key=os.path.getmtime)


TRACE_RE = re.compile(
    r"DX12 TRACE:\s+(?P<api>\S+)\s+orig=(?P<orig>\S+)\s+\|\s+(?P<details>.*?)\s+\|\s+trail=(?P<trail>.*)$"
)
KV_RE = re.compile(r"(\w+)=([^\s]+)")


def parse_trace_lines(logdir):
    path = os.path.join(logdir, "hook_debug.log")
    if not os.path.exists(path):
        sys.exit(f"[error] no hook_debug.log in {logdir}")
    events = []
    install_lines = []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            if "DX12 TRACE:" not in line:
                continue
            m = TRACE_RE.search(line)
            if not m:
                install_lines.append(line.strip().split("DX12 TRACE:", 1)[1].strip())
                continue
            kv = dict(KV_RE.findall(m.group("details")))
            events.append({
                "api": m.group("api"),
                "orig": m.group("orig"),
                "details": m.group("details"),
                "trail": m.group("trail"),
                "kv": kv,
            })
    return events, install_lines


def _classify_owner(orig, trail):
    """Best-effort owner of a call: the app, CE/infra, Streamline, or a co-resident module."""
    o = orig.lower()
    if o.endswith(".exe"):
        return "APP"
    t = trail.lower()
    if "sl.interposer" in t or "sl.common" in t:
        return "Streamline"
    if orig in INFRA_MODULES or orig == "?":
        return "CE/infra"
    return orig  # the actual module basename (e.g. a co-resident overlay's DLL)


def parse_report(logdir):
    events, install_lines = parse_trace_lines(logdir)
    if not events and not install_lines:
        print("[parse] No 'DX12 TRACE' lines found. Was the ce_dx12_trace flag set during the run, "
              "and the trace-enabled build deployed?")
        return

    print("=" * 90)
    print(f"DX12 API call-trace report  --  {logdir}")
    print("=" * 90)

    if install_lines:
        print("\n# Trace hook installation notices")
        for s in sorted(set(install_lines)):
            print(f"  - {s}")

    by_api = defaultdict(list)
    for e in events:
        by_api[e["api"]].append(e)

    # ---- Command queues created ---------------------------------------------------
    print("\n# Command queues created (owner of each queue)")
    queue_owner = {}
    if not by_api.get("CreateCommandQueue"):
        print("  (none captured)")
    for e in by_api.get("CreateCommandQueue", []):
        who = _classify_owner(e["orig"], e["trail"])
        q = e["kv"].get("queue", "?")
        queue_owner[q] = who
        print(f"  [{who:14}] queue={q} type={e['kv'].get('type')} "
              f"prio={e['kv'].get('prio')} flags={e['kv'].get('flags')}  (trail={e['trail']})")

    # ---- Swapchains ---------------------------------------------------------------
    print("\n# Swapchains created")
    sc_events = by_api.get("CreateSwapChain", []) + by_api.get("CreateSwapChainForHwnd", [])
    if not sc_events:
        print("  (none captured)")
    for e in sc_events:
        who = _classify_owner(e["orig"], e["trail"])
        print(f"  [{who:14}] {e['api']} sc={e['kv'].get('sc')} fmt={e['kv'].get('fmt')} "
              f"count={e['kv'].get('count')} effect={e['kv'].get('effect')} flags={e['kv'].get('flags')}")

    # ---- Committed resources ------------------------------------------------------
    print("\n# Committed resources (footprint per owner)")
    res_by_owner = defaultdict(list)
    for e in by_api.get("CreateCommittedResource", []):
        res_by_owner[_classify_owner(e["orig"], e["trail"])].append(e)
    if not res_by_owner:
        print("  (none captured)")
    for who, evs in sorted(res_by_owner.items()):
        heaps = Counter(e["kv"].get("heapType") for e in evs)
        dims = Counter(e["kv"].get("dim") for e in evs)
        print(f"  [{who}] {len(evs)} resource(s); heapType counts={dict(heaps)} dim counts={dict(dims)}")
        for e in evs[:12]:
            print(f"      heapType={e['kv'].get('heapType')} dim={e['kv'].get('dim')} "
                  f"w={e['kv'].get('w')} h={e['kv'].get('h')} fmt={e['kv'].get('fmt')} "
                  f"resFlags={e['kv'].get('resFlags')} res={e['kv'].get('res')}")
        if len(evs) > 12:
            print(f"      ... (+{len(evs) - 12} more)")

    # ---- Descriptor heaps ---------------------------------------------------------
    print("\n# Descriptor heaps")
    dh_by_owner = defaultdict(list)
    for e in by_api.get("CreateDescriptorHeap", []):
        dh_by_owner[_classify_owner(e["orig"], e["trail"])].append(e)
    if not dh_by_owner:
        print("  (none captured)")
    for who, evs in sorted(dh_by_owner.items()):
        print(f"  [{who}] {len(evs)} heap(s)")
        for e in evs[:12]:
            print(f"      type={e['kv'].get('type')} num={e['kv'].get('num')} flags={e['kv'].get('flags')}")

    # ---- ExecuteCommandLists (per queue) ------------------------------------------
    print("\n# ExecuteCommandLists submissions (sampled; grouped by queue)")
    ecl_by_queue = defaultdict(list)
    for e in by_api.get("ExecuteCommandLists", []):
        ecl_by_queue[e["kv"].get("queue", "?")].append(e)
    if not ecl_by_queue:
        print("  (none captured)")
    for q, evs in ecl_by_queue.items():
        label = queue_owner.get(q, "?")
        owners = Counter(_classify_owner(e["orig"], e["trail"]) for e in evs)
        numlists = Counter(e["kv"].get("numLists") for e in evs)
        print(f"  queue={q} [created-by={label}] samples={len(evs)} owners={dict(owners)} numLists={dict(numlists)}")
        seen = set()
        for e in evs:
            t = e["trail"]
            if t not in seen:
                seen.add(t)
                print(f"      trail: {t}")
            if len(seen) >= 3:
                break

    # ---- Signal (per queue) -------------------------------------------------------
    print("\n# Fence Signal calls (sampled; grouped by queue)")
    sig_by_queue = defaultdict(list)
    for e in by_api.get("Signal", []):
        sig_by_queue[e["kv"].get("queue", "?")].append(e)
    if not sig_by_queue:
        print("  (none captured)")
    for q, evs in sig_by_queue.items():
        label = queue_owner.get(q, "?")
        owners = Counter(_classify_owner(e["orig"], e["trail"]) for e in evs)
        print(f"  queue={q} [created-by={label}] samples={len(evs)} owners={dict(owners)}")

    print("\n# Note")
    print("  CE submits its own overlay work via the raw (unhooked) ExecuteCommandLists pointer, so")
    print("  CE's own overlay ECLs/Signals are not captured here; the captured submissions are the")
    print("  app's own and any co-resident injected module's. Run --overlay vs --observe and compare")
    print("  the resource/descriptor footprint to see CE's contribution.")
    print("=" * 90)


# --------------------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p_run = sub.add_parser("run", help="set up, launch app, run steady-state, parse")
    grp = p_run.add_mutually_exclusive_group(required=True)
    grp.add_argument("--overlay", action="store_true", help="CE overlay on (observer_only=false)")
    grp.add_argument("--observe", action="store_true", help="CE observer-only (adds no overlay GPU work)")
    p_run.add_argument("--seconds", type=int, default=10, help="steady-state capture duration")

    p_setup = sub.add_parser("setup", help="enable trace flag and set observer_only")
    g2 = p_setup.add_mutually_exclusive_group(required=True)
    g2.add_argument("--overlay", action="store_true", help="trace on + observer_only=false")
    g2.add_argument("--observe", action="store_true", help="trace on + observer_only=true")

    sub.add_parser("cleanup", help="remove trace flag and restore observer_only=false")

    p_parse = sub.add_parser("parse", help="parse a log dir (default: newest) into a report")
    p_parse.add_argument("logdir", nargs="?", default=None)

    args = ap.parse_args()

    if args.cmd == "run":
        do_run(observer=args.observe, seconds=args.seconds)
    elif args.cmd == "setup":
        set_trace_flag(True)
        set_observer_only(args.observe)
        print("[setup] done. Launch captureengine.exe + dx12_test.exe, run ~10s steady-state, "
              "then: python tools/dx12_call_trace.py parse")
    elif args.cmd == "cleanup":
        set_trace_flag(False)
        set_observer_only(False)
        print("[cleanup] trace flag removed, observer_only=false restored.")
    elif args.cmd == "parse":
        logdir = args.logdir or newest_log_dir()
        if not logdir:
            sys.exit("[error] no log dir found")
        parse_report(logdir)


if __name__ == "__main__":
    main()
