#!/usr/bin/env python3
"""Check queue ECL/Signal binding defects through production transactions and real hooks.

Temporarily changes ECL registry/dispatch sources, restores their exact bytes in finally,
and requires flow assertion failures rather than compiler/loader errors. Do not run
alongside a build or edit the lifecycle source while this bounded check is running.
"""
from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
COMMAND = [sys.executable, "build.py", "--incremental", "--tests-only", "--flow-tests",
           "--run-tests", "--gtest-filter=ExecuteDispatchRegistryTest.*:FlowQueue*:FlowSignalInterposer.*",
           "--skip-updates", "--concise"]
MUTATIONS = (
    ("foreign live entry outranks retained interception evidence", "hook/d3d12/execute_dispatch_registry.h",
     "        if (const Target retained = recover(); retained && retained != detour)\n            return retained;",
     "        (void)recover;",
     "unit", "ExecuteDispatchRegistryTest.InterceptionRecoveryPrecedesForeignLiveEntryAfterReset"),
    ("removal discards predecessor evidence while a foreign follower still calls CE", "hook/hooking/vtable_hook.cpp",
     "if (ce::vtable_hook_policy::ShouldPreserveForeignFollower(",
     "if (false && ce::vtable_hook_policy::ShouldPreserveForeignFollower(",
     "flow", "FlowSignalInterposer.ForeignFollowerSurvivesCERemovalAndBindingReset"),
    ("missing Signal receiver inherits a saved queue predecessor", "hook/d3d12/dx12_queue_dispatch.cpp",
     "    SignalPtr target = nullptr;",
     "    if (!vtable) {\n"
     "        const auto bindings = signalRegistry.Snapshot();\n"
     "        if (!bindings.empty()) return bindings.front().original;\n"
     "    }\n    SignalPtr target = nullptr;",
     "flow", "FlowQueueSignal.MissingReceiverCannotBorrowSavedSignalEntry"),
    ("untracked queue inherits first global predecessor", "hook/d3d12/dx12_queue_dispatch.cpp",
     "    auto target = registry.ResolveInterception(",
     "    if (!registry.HasBinding(vtable)) {\n"
     "        const auto bindings = registry.Snapshot();\n"
     "        if (!bindings.empty()) return bindings.front().original;\n"
     "    }\n    auto target = registry.ResolveInterception(",
     "flow", "FlowQueueDispatch.UntrackedImplementationUsesItsOwnEntry"),
    ("cached target survives reset and identity reuse", "hook/d3d12/execute_dispatch_registry.h",
     "cache.generation == generation", "true",
     "unit", "ExecuteDispatchRegistryTest.ResetInvalidatesCachedPairsBeforeIdentityReuse"),
    ("cancelled capture resurrects retired evidence", "hook/d3d12/execute_dispatch_registry.h",
     "        if (entry == originals_.end() || entry->second.pending != &captured)\n"
     "            return {InstallResult::kRetired, captured};",
     "        if (entry == originals_.end()) {\n"
     "            entry = originals_.try_emplace(vtable).first;\n"
     "            entry->second.pending = &captured;\n        }",
     "unit", "ExecuteDispatchRegistryTest.ReentrantRetirementCannotResurrectAnErasedInstallation"),
)


def run_tests(log_path: Path, command: list[str] | None = None) -> int:
    with log_path.open("w", encoding="utf-8") as output:
        process = subprocess.Popen(COMMAND if command is None else command, cwd=ROOT,
                                   stdout=output, stderr=subprocess.STDOUT)
        try:
            return process.wait(timeout=180)
        except BaseException:
            if process.poll() is None:
                if sys.platform == "win32":
                    subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"],
                                   check=False, timeout=15)
                else:
                    process.kill()
                process.wait(timeout=15)
            raise


def assertion_failure_recorded(log_path: Path, kind: str, expected_test: str) -> bool:
    manifest = json.loads((ROOT / "build/verification/latest_manifest.json").read_text(encoding="utf-8"))
    name = "flow_tests" if kind == "flow" else "unit_tests"
    step = manifest.get("steps", {}).get(name, {})
    if step.get("status") != "failed":
        return False
    if kind == "flow" and expected_test not in step.get("details", {}).get("failed", []):
        return False
    output = log_path.read_text(encoding="utf-8", errors="replace")
    return f"[  FAILED  ] {expected_test}" in output


def main() -> int:
    originals = {ROOT / relative: (ROOT / relative).read_bytes() for _, relative, _, _, _, _ in MUTATIONS}
    output_dir = ROOT / "build/refactor/ecl-mutations"
    output_dir.mkdir(parents=True, exist_ok=True)
    detected = []
    try:
        for index, (name, relative, before, after, kind, expected_test) in enumerate(MUTATIONS, 1):
            path = ROOT / relative
            original = originals[path]
            newline = "\r\n" if b"\r\n" in original else "\n"
            source = original.decode("utf-8").replace("\r\n", "\n")
            if source.count(before) != 1:
                raise RuntimeError(f"mutation anchor changed: {name}")
            print(f"[ECL mutation] checking: {name}", flush=True)
            path.write_bytes(source.replace(before, after).replace("\n", newline).encode("utf-8"))
            log_path = output_dir / f"mutation-{index}.log"
            result = run_tests(log_path)
            if result == 0 or not assertion_failure_recorded(log_path, kind, expected_test):
                raise RuntimeError(f"flow assertions did not reject: {name}; evidence: {log_path.name}")
            path.write_bytes(original)
            detected.append(name)
            print(f"[ECL mutation] detected: {name}", flush=True)
    finally:
        for path, original in originals.items():
            path.write_bytes(original)
        print("[ECL mutation] restored production dispatch; verifying original", flush=True)
        restored_result = run_tests(output_dir / "restored.log")
    if restored_result != 0:
        raise RuntimeError("restored production dispatch did not pass")
    print(f"[ECL mutation] {len(detected)} of {len(MUTATIONS)} defects detected; original passes", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
