#!/usr/bin/env python3
"""Check NGX lifecycle defect sensitivity through the production hook and fake core.

Temporarily changes NGX observation/lifecycle sources, restores their exact bytes in finally,
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
           "--run-tests", "--gtest-filter=FlowNGX.*", "--skip-updates", "--concise"]
MUTATIONS = (
    ("in-flight observation overrides explicit OFF", "hook/ngx/ngx_fg_observation.cpp",
     "    if (heldOff) {", "    if (false) {",
     "FlowNGX.InFlightEvaluationCannotOverrideExplicitStreamlineOffAndCanReactivate"),
    ("released feature remains published", "hook/ngx/nvngx_hook_lifecycle.cpp",
     "            RepublishFeatureStateAfterRelease(removal.feature);",
     "            (void)removal.feature;",
     "FlowNGX.FeaturePublicationFollowsSuccessfulEvaluationAndRelease"),
    ("created feature published before evaluation", "hook/ngx/nvngx_hook_lifecycle.cpp",
     "    const auto recorded = nvngx_hook_g_FeatureRegistry.RecordCreated(*handle, featureID);",
     "    const auto recorded = nvngx_hook_g_FeatureRegistry.RecordCreated(*handle, featureID);\n"
     "    if (upscalerFeature) PublishEvaluatedFeature(featureID, true);",
     "FlowNGX.FeaturePublicationFollowsSuccessfulEvaluationAndRelease"),
    ("creation ignores observed factor", "hook/ngx/ngx_fg_observation.cpp",
     "observation.parameterMultiplier);",
     "0);",
     "FlowNGX.CreationAndEvaluationPreserveObservedFactorsAndLegacyLatch"),
    ("unknown evaluation factor changes publication", "hook/ngx/ngx_fg_observation.cpp",
     "    if (multiplier <= 0)", "    if (multiplier < 0)",
     "FlowNGX.CreationAndEvaluationPreserveObservedFactorsAndLegacyLatch"),
)


def run_tests(log_path: Path) -> int:
    with log_path.open("w", encoding="utf-8") as output:
        process = subprocess.Popen(COMMAND, cwd=ROOT, stdout=output, stderr=subprocess.STDOUT)
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


def assertion_failure_recorded(log_path: Path, expected_test: str) -> bool:
    manifest = json.loads((ROOT / "build/verification/latest_manifest.json").read_text(encoding="utf-8"))
    step = manifest.get("steps", {}).get("flow_tests", {})
    if step.get("status") != "failed" or expected_test not in step.get("details", {}).get("failed", []):
        return False
    output = log_path.read_text(encoding="utf-8", errors="replace")
    return f"[  FAILED  ] {expected_test}" in output


def main() -> int:
    originals = {ROOT / relative: (ROOT / relative).read_bytes() for _, relative, _, _, _ in MUTATIONS}
    output_dir = ROOT / "build/refactor/ngx-mutations"
    output_dir.mkdir(parents=True, exist_ok=True)
    detected = []
    try:
        for index, (name, relative, before, after, expected_test) in enumerate(MUTATIONS, 1):
            path = ROOT / relative
            original = originals[path]
            newline = "\r\n" if b"\r\n" in original else "\n"
            source = original.decode("utf-8").replace("\r\n", "\n")
            if source.count(before) != 1:
                raise RuntimeError(f"mutation anchor changed: {name}")
            print(f"[NGX mutation] checking: {name}", flush=True)
            path.write_bytes(source.replace(before, after).replace("\n", newline).encode("utf-8"))
            log_path = output_dir / f"mutation-{index}.log"
            result = run_tests(log_path)
            if result == 0 or not assertion_failure_recorded(log_path, expected_test):
                raise RuntimeError(f"flow assertions did not reject: {name}; evidence: {log_path.name}")
            path.write_bytes(original)
            detected.append(name)
            print(f"[NGX mutation] detected: {name}", flush=True)
    finally:
        for path, original in originals.items():
            path.write_bytes(original)
        print("[NGX mutation] restored production lifecycle; verifying original", flush=True)
        restored_result = run_tests(output_dir / "restored.log")
    if restored_result != 0:
        raise RuntimeError("restored production lifecycle did not pass")
    print(f"[NGX mutation] {len(detected)} of {len(MUTATIONS)} defects detected; original passes", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
