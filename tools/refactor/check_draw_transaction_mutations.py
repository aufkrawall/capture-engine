#!/usr/bin/env python3
"""Verify DX12 historical-defect sensitivity using the real production transaction.

Only the private header is temporarily mutated; every change is restored in finally.
Runs bounded native tests-only builds, requires assertion failures (not build errors),
and finishes by rebuilding/running the original transaction. Do not run concurrently
with another build or edit the transaction header while this check is running.
"""
from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / "hook/d3d12/overlay_draw_transaction.h"
FILTER = "DX12DrawTransactionTest.*"
COMMAND = [sys.executable, "build.py", "--incremental", "--tests-only", "--run-tests",
           f"--gtest-filter={FILTER}", "--skip-updates", "--concise"]
MUTATIONS = (
    ("success enters RTV recovery", "    operations.RefreshRenderTarget(backBuffer.Borrow());",
     "    operations.RetireRenderTargets();\n    state.overlayInit = false;\n"
     "    operations.RefreshRenderTarget(backBuffer.Borrow());"),
    ("successful reset invalidates synchronization", "    flow = operations.PrepareRecording();",
     "    state.syncInit = false;\n    flow = operations.PrepareRecording();"),
    ("normal release delayed until destructor", "    // Preserve the original release before post-overlay capture/publication.\n"
     "    backBuffer.Release();", "    // Mutant: only the destructor retires this reference."),
    ("release missing on every path", "    void Release() { if (Buffer* buffer = std::exchange(buffer_, nullptr)) buffer->Release(); }",
     "    void Release() {}"),
    ("normal release duplicated", "    // Preserve the original release before post-overlay capture/publication.\n"
     "    backBuffer.Release();", "    backBuffer.Borrow()->Release();\n    backBuffer.Release();"),
    ("backbuffer retired before recording", "    flow = operations.RecordCommands();",
     "    backBuffer.Release();\n    flow = operations.RecordCommands();"),
)


def run_tests() -> int:
    process = subprocess.Popen(COMMAND, cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    try:
        return process.wait(timeout=180)
    except BaseException:
        if process.poll() is not None:
            raise
        if sys.platform == "win32":
            subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"], check=False)
        else:
            process.kill()
        process.wait()
        raise


def assertion_failure_recorded() -> bool:
    manifest_path = ROOT / "build/verification/latest_manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    # The runner's artifact is read directly; a compiler/loader failure cannot
    # masquerade as sensitivity to the historical recovery/release defect.
    artifacts = manifest.get("artifacts", {})
    failure_path = artifacts.get("unit_tests_failure_log")
    if not failure_path:
        return False
    if manifest.get("steps", {}).get("unit_tests", {}).get("status") != "failed":
        return False
    # Manifest paths intentionally scrub the developer profile. Resolve only
    # the recorded run and artifact basenames inside this workspace.
    run_name = Path(manifest["run_dir"]).name
    failure = (ROOT / "build/verification" / run_name / Path(failure_path).name).read_text(
        encoding="utf-8", errors="replace")
    return "[  FAILED  ] DX12DrawTransactionTest." in failure


def main() -> int:
    original = HEADER.read_bytes()
    source = original.decode("utf-8").replace("\r\n", "\n")
    detected = []
    try:
        for name, before, after in MUTATIONS:
            if source.count(before) != 1:
                raise RuntimeError(f"mutation anchor changed: {name}")
            print(f"[DX12 mutation] checking: {name}", flush=True)
            HEADER.write_bytes(source.replace(before, after).replace("\n", "\r\n").encode("utf-8"))
            result = run_tests()
            if result == 0 or not assertion_failure_recorded():
                raise RuntimeError(f"mutation was not rejected by behavioral assertions: {name}")
            detected.append(name)
            print(f"[DX12 mutation] detected: {name}", flush=True)
    finally:
        HEADER.write_bytes(original)
        print("[DX12 mutation] restored production transaction; verifying original", flush=True)
        restored_result = run_tests()
    if restored_result != 0:
        raise RuntimeError("restored production transaction did not pass")
    print(f"[DX12 mutation] {len(detected)} of {len(MUTATIONS)} defects detected; original passes", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
