#!/usr/bin/env python3
"""Reject dropped vtable predecessors and invalid forwarding scope through real production code.

Restores exact source bytes and verifies production afterward. Do not run alongside
a build or edits to the vtable dispatch source.
"""
import sys

from check_ecl_dispatch_mutations import ROOT, assertion_failure_recorded, run_tests

DISPATCH = "hook/present/dxgi_shared_hooks_present_vtable.cpp"
SCOPE = "hook/present/dxgi_shared_detail/present_vtable_call.h"
PREFIX = [sys.executable, "build.py", "--incremental", "--tests-only", "--flow-tests", "--run-tests"]
SUFFIX = ["--skip-updates", "--concise"]
MUTATIONS = (
    ("Present skips its captured vtable predecessor", DISPATCH,
     "return ScopedVTableCall<PFN_Present>::TryForward(",
     "return false && ScopedVTableCall<PFN_Present>::TryForward(",
     "flow", "FlowPresentInterposer.ForeignLayerInstalledBeforeCEVTableClaimRemainsInTheChain"),
    ("Present1 skips its captured vtable predecessor", DISPATCH,
     "return ScopedVTableCall<PFN_Present1>::TryForward(",
     "return false && ScopedVTableCall<PFN_Present1>::TryForward(",
     "flow", "FlowPresentInterposer.ForeignPresent1InstalledBeforeCEVTableClaimRemainsInTheChain"),
    ("inline reentry reuses an already forwarding vtable link", SCOPE,
     " || call->forwarding_", "",
     "unit", "PresentVTableFixture.PresentForwardsExactArgumentsAndResultWithoutReusingAnActiveLink"),
    ("another receiver borrows the active vtable link", SCOPE,
     "call->receiver_ != receiver || ", "",
     "unit", "PresentVTableFixture.AnotherReceiverCannotBorrowAnIdleScope"),
    ("vtable adapter hides the original SDK caller", SCOPE,
     "return call && call->receiver_ == receiver", "return false && call && call->receiver_ == receiver",
     "unit", "PresentVTableFixture.OriginalCallerIsScopedToTheReceiverMethodAndInterceptionView"),
    ("vtable interception is mistaken for foreign code", DISPATCH,
     " || entry == reinterpret_cast<void*>(&DetourVTablePresent)", "",
     "unit", "PresentVTableFixture.BothInterceptionViewsAreRecognizedWithoutForeignOrCrossMethodTargets"),
)


def main() -> int:
    originals = {ROOT / relative: (ROOT / relative).read_bytes() for _, relative, _, _, _, _ in MUTATIONS}
    output_dir = ROOT / "build/refactor/present-chain-mutations"
    output_dir.mkdir(parents=True, exist_ok=True)
    try:
        for index, (name, relative, before, after, kind, test) in enumerate(MUTATIONS, 1):
            path = ROOT / relative
            original = originals[path]
            newline = "\r\n" if b"\r\n" in original else "\n"
            source = original.decode("utf-8").replace("\r\n", "\n")
            if source.count(before) != 1:
                raise RuntimeError(f"mutation anchor changed: {name}")
            path.write_bytes(source.replace(before, after).replace("\n", newline).encode("utf-8"))
            print(f"[Present chain mutation] checking: {name}", flush=True)
            log = output_dir / f"mutation-{index}.log"
            result = run_tests(log, PREFIX + [f"--gtest-filter={test}"] + SUFFIX)
            if result == 0 or not assertion_failure_recorded(log, kind, test):
                raise RuntimeError(f"dispatch assertions did not reject: {name}; evidence: {log.name}")
            path.write_bytes(original)
            print(f"[Present chain mutation] detected: {name}", flush=True)
    finally:
        for path, original in originals.items():
            path.write_bytes(original)
        print("[Present chain mutation] restored exact source; verifying production", flush=True)
        restored = run_tests(output_dir / "restored.log", PREFIX + [
            "--gtest-filter=PresentVTableFixture.*:DXGIShared*:InjectLifecycleSourceTest.*:Flow*"] + SUFFIX)
    if restored != 0:
        raise RuntimeError("restored vtable dispatch did not pass")
    print(f"[Present chain mutation] {len(MUTATIONS)} defects detected; original passes", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
