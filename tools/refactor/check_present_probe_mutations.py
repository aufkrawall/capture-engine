#!/usr/bin/env python3
"""Reject status-probe side effects in each native/wrapper Present entry point.

Restores exact source bytes and verifies production afterward. Do not run alongside
a build or edits to the listed presentation sources.
"""
import sys

from check_ecl_dispatch_mutations import ROOT, assertion_failure_recorded, run_tests

TEST = "FlowPresentInterposer.PresentAndPresent1StatusQueriesLeaveOutputAccountingUnchanged"
COMMAND = [sys.executable, "build.py", "--incremental", "--tests-only", "--flow-tests", "--run-tests",
           f"--gtest-filter={TEST}", "--skip-updates", "--concise"]
MUTATIONS = (
    ("native Present treats a status probe as an output", "hook/present/dxgi_shared_present.cpp",
     "if (Flags & DXGI_PRESENT_TEST)", "if (false && (Flags & DXGI_PRESENT_TEST))"),
    ("native Present1 treats a status probe as an output", "hook/present/dxgi_shared_present1.cpp",
     "if (Flags & DXGI_PRESENT_TEST)", "if (false && (Flags & DXGI_PRESENT_TEST))"),
    ("wrapper Present processes a status probe", "hook/wrappers/dxgi_swapchain_wrap_present.cpp",
     "if ((Flags & DXGI_PRESENT_TEST) || HookIsShuttingDown())", "if (HookIsShuttingDown())"),
    ("wrapper Present1 processes a status probe", "hook/wrappers/dxgi_swapchain_wrap_present.cpp",
     "if ((PresentFlags & DXGI_PRESENT_TEST) || HookIsShuttingDown())", "if (HookIsShuttingDown())"),
)


def main() -> int:
    originals = {ROOT / relative: (ROOT / relative).read_bytes() for _, relative, _, _ in MUTATIONS}
    output_dir = ROOT / "build/refactor/present-probe-mutations"
    output_dir.mkdir(parents=True, exist_ok=True)
    try:
        for index, (name, relative, before, after) in enumerate(MUTATIONS, 1):
            path = ROOT / relative
            original = originals[path]
            source = original.decode("utf-8")
            if source.count(before) != 1:
                raise RuntimeError(f"mutation anchor changed: {name}")
            path.write_bytes(source.replace(before, after).encode("utf-8"))
            print(f"[Present probe mutation] checking: {name}", flush=True)
            log = output_dir / f"mutation-{index}.log"
            result = run_tests(log, COMMAND)
            if result == 0 or not assertion_failure_recorded(log, "flow", TEST):
                raise RuntimeError(f"probe assertions did not reject: {name}; evidence: {log.name}")
            path.write_bytes(original)
            print(f"[Present probe mutation] detected: {name}", flush=True)
    finally:
        for path, original in originals.items():
            path.write_bytes(original)
        print("[Present probe mutation] restored exact source; verifying production", flush=True)
        restored = run_tests(output_dir / "restored.log", COMMAND)
    if restored != 0:
        raise RuntimeError("restored presentation did not pass")
    print(f"[Present probe mutation] {len(MUTATIONS)} defects detected; original passes", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
