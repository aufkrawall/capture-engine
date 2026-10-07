#!/usr/bin/env python3
"""Check device trace provenance and reset recovery through the real WARP hook.

Restores exact source bytes in finally and verifies the original afterward. Do not
run alongside a build or edits to dx12_device_trace.cpp. The historical wrong-receiver
mutation deliberately reproduces an access violation; other failures must be assertions.
"""
import sys

from check_ecl_dispatch_mutations import ROOT, assertion_failure_recorded, run_tests

SOURCE = ROOT / "hook/d3d12/dx12_device_trace.cpp"
TEST = "FlowDeviceTrace.NativeBootstrapAndDebugDeviceCreationKeepTheirOwnEntries"
COMMAND = [sys.executable, "build.py", "--incremental", "--tests-only", "--flow-tests",
           "--run-tests", f"--gtest-filter={TEST}", "--skip-updates", "--concise"]
MUTATIONS = (
    ("first device original is borrowed by another implementation", (
        ("        using Result = typename ce::dx12::ExecuteDispatchRegistry<Target>::InstallResult;",
         "        if (captured.original && !borrowedFirst_) borrowedFirst_ = captured.original;\n"
         "        using Result = typename ce::dx12::ExecuteDispatchRegistry<Target>::InstallResult;"),
        ("        if (target)\n            return target(device, std::forward<Args>(args)...);",
         "        if (target)\n            return borrowedFirst_(device, std::forward<Args>(args)...);"),
        ("    const size_t slot_;", "    Target borrowedFirst_ = nullptr;\n    const size_t slot_;"),
    ), True),
    ("physical trace slots lose exact predecessors after reset", (
        ("                if (VTableHook::GetOriginal(&vtable[slot_], reinterpret_cast<void*>(detour_), &original))",
         "                if (false && VTableHook::GetOriginal(&vtable[slot_], "
         "reinterpret_cast<void*>(detour_), &original))"),
    ), False),
)


def main() -> int:
    original = SOURCE.read_bytes()
    output_dir = ROOT / "build/refactor/device-trace-mutations"
    output_dir.mkdir(parents=True, exist_ok=True)
    try:
        for index, (name, changes, crash) in enumerate(MUTATIONS, 1):
            source = original.decode("utf-8")
            for before, after in changes:
                if source.count(before) != 1:
                    raise RuntimeError(f"mutation anchor changed: {name}")
                source = source.replace(before, after)
            SOURCE.write_bytes(source.encode("utf-8"))
            print(f"[Device trace mutation] checking: {name}", flush=True)
            log = output_dir / f"mutation-{index}.log"
            result = run_tests(log, COMMAND)
            if crash:
                rejected = f"[flow:{TEST}] FAILED (exit code 3221225477)" in log.read_text(encoding="utf-8")
            else:
                rejected = assertion_failure_recorded(log, "flow", TEST)
            if result == 0 or not rejected:
                raise RuntimeError(f"expected defect was not detected: {name}; evidence: {log.name}")
            SOURCE.write_bytes(original)
            print(f"[Device trace mutation] detected: {name}", flush=True)
    finally:
        SOURCE.write_bytes(original)
        print("[Device trace mutation] restored exact source; verifying original", flush=True)
        restored = run_tests(output_dir / "restored.log", COMMAND)
    if restored != 0:
        raise RuntimeError("restored device tracing did not pass")
    print(f"[Device trace mutation] {len(MUTATIONS)} defects detected; original passes", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
