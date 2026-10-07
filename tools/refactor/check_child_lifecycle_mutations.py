#!/usr/bin/env python3
"""Reject lost helper ownership and reentrant resurrection in the production lifecycle.

Restores exact source bytes and verifies the restored implementation. Do not run
alongside builds or edits to the lifecycle header.
"""
import sys

from check_ecl_dispatch_mutations import ROOT, assertion_failure_recorded, run_tests

SOURCE = ROOT / "captureengine/app/child_process_lifecycle.h"
COMMAND = [sys.executable, "build.py", "--incremental", "--tests-only", "--run-tests",
           "--gtest-filter=ChildProcessLifecycleTest.*:HostChildrenTest.*",
           "--skip-updates", "--concise"]
MUTATIONS = (
    ("finalization retirement closes a still-running media process",
     "retired_.push_back({role, slot.process, slot.generation - 1});", "effects_.Close(slot.process);",
     "FinalizingMediaStaysOwnedWhileAFreshAuthenticatedChildStarts"),
    ("retired generation resumes the old readiness operation",
     "slot.generation != ticket", "false",
     "RetirementDuringMessagePumpingCancelsTheOldEnsure"),
    ("late spawn resurrects a shutdown slot",
     "if (closing_ || launch != slot.generation || !effects_.AcceptingWork())", "if (false)",
     "ShutdownDuringSpawnRetainsTheLateSuccessWithoutResurrection"),
    ("recursive readiness creates another peer",
     " || slot.installing", "",
     "RecursiveEnsureCannotCreateASecondPeerDuringStartup"),
    ("failed termination is treated as a confirmed kill",
     "!effects_.Terminate(process)", "false && !effects_.Terminate(process)",
     "FailedTerminationRetainsOwnershipAndDoesNotWaitInfinitely"),
)


def main() -> int:
    original = SOURCE.read_bytes()
    source = original.decode("utf-8")
    output_dir = ROOT / "build/refactor/child-lifecycle-mutations"
    output_dir.mkdir(parents=True, exist_ok=True)
    try:
        for index, (name, before, after, test) in enumerate(MUTATIONS, 1):
            if source.count(before) != 1:
                raise RuntimeError(f"mutation anchor changed: {name}")
            SOURCE.write_bytes(source.replace(before, after).encode("utf-8"))
            print(f"[Child lifecycle mutation] checking: {name}", flush=True)
            log = output_dir / f"mutation-{index}.log"
            result = run_tests(log, COMMAND)
            if result == 0 or not assertion_failure_recorded(log, "unit", f"ChildProcessLifecycleTest.{test}"):
                raise RuntimeError(f"lifecycle assertions did not reject: {name}; evidence: {log.name}")
            SOURCE.write_bytes(original)
            print(f"[Child lifecycle mutation] detected: {name}", flush=True)
    finally:
        SOURCE.write_bytes(original)
        print("[Child lifecycle mutation] restored exact source; verifying production", flush=True)
        restored = run_tests(output_dir / "restored.log", COMMAND)
    if restored != 0:
        raise RuntimeError("restored lifecycle did not pass")
    print(f"[Child lifecycle mutation] {len(MUTATIONS)} defects detected; original passes", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
