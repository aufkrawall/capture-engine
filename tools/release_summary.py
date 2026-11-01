"""The release job's own account of a run, written to the GitHub job summary.

`release-log-cleanup.yml` deletes every self-hosted run log as soon as the run ends
(the runner writes the host name into it), so a failed release used to leave nothing
on GitHub but a red cross; the reason had to be dug out of the runner's verification
directory. The job summary survives log deletion. It is built only from fixed labels,
check names, stage statuses and durations - never from log text - so it cannot carry a
path or the host name.
"""

from __future__ import annotations

import datetime
import json
import os
from typing import Any, Dict, List, Mapping, Optional, Tuple

# Top-level stages of `build.py --verify`, in the order a reader thinks about them.
STAGES: Tuple[Tuple[str, str], ...] = (
    ("preflight", "build.py static preflight"),
    ("external_preparation", "FFmpeg dependency closure + FFmpeg"),
    ("sanitize_regression", "ASan/UBSan regression (concurrent)"),
    ("unit_tests", "native unit tests"),
    ("flow_tests", "FG flow scenarios"),
    ("build", "product build, tests, finalize"),
    ("lint", "lint ratchets"),
    ("package_archives", "packaging"),
)

# Failure class -> what to do next. Keyed by manifest step names and preflight check names.
NEXT_ACTION: Dict[str, str] = {
    "git_state": "Push or merge the release commit to main (PR), then dispatch again.",
    "changelog": "Fix CHANGELOG.md (python tools/manage_changelog.py --validate).",
    "size_ratchets": "Split the grown file or shorten the line; see the size baselines.",
    "flake8": "Fix the flake8 findings (python build.py --no-build --lint --skip-updates --concise).",
    "pyright": "Fix the pyright findings (python build.py --no-build --lint --skip-updates --concise).",
    "module_boundaries": "Remove the new cross-module include; exceptions are not extended.",
    "python_self_tests": "Run the failing tools/tests suite locally and fix it.",
    "python_tool_self_tests": "Run the failing tools/tests suite locally and fix it.",
    "toolchain": "Repair the MSYS2 install (pacman -Syu, or downgrade the named package) and rerun the preflight.",
    "sources": "A pinned source host is unreachable even after retries and fallbacks; retry later or fix the pin.",
    "external_preparation": "Rerun tools/rehearse_dependency_closure.py locally; check the toolchain changes "
                            "the preflight listed.",
    "sanitize_regression": "A real ASan/UBSan finding: reproduce with build.py --sanitize --run-tests.",
    "unit_tests": "Reproduce with the tests-only loop and the failing suite as --gtest-filter.",
    "flow_tests": "Reproduce with build.py --incremental --run-tests --rerun-flow-tests --gtest-filter=<scenario>.",
    "lint": "Run python build.py --no-build --lint --skip-updates --concise after a full product build.",
    "package_archives": "Packaging failed; inspect build.details.log in the runner's verification directory.",
    "build": "The product build failed; inspect build.details.log in the runner's verification directory.",
}

WORKFLOW_STEPS: Tuple[Tuple[str, str], ...] = (
    ("version", "resolve version"),
    ("preflight", "release preflight"),
    ("build", "build and verify"),
    ("verify", "built version check"),
    ("attestation", "attestation"),
    ("publish", "tag and GitHub release"),
)


def _load(path: str) -> Optional[Dict[str, Any]]:
    try:
        with open(path, "r", encoding="utf-8") as source:
            data = json.load(source)
        return data if isinstance(data, dict) else None
    except (OSError, ValueError):
        return None


def _time(value: Any) -> Optional[datetime.datetime]:
    try:
        return datetime.datetime.fromisoformat(str(value))
    except ValueError:
        return None


def manifest_for_this_run(manifest: Optional[Dict[str, Any]], report: Optional[Dict[str, Any]]):
    """A manifest older than this run's preflight belongs to an earlier build; ignore it."""
    if not manifest or not report:
        return None
    started, built = _time(report.get("started_at")), _time(manifest.get("start_time"))
    return manifest if started and built and built >= started else None


def _seconds(value: Any) -> str:
    return f"{float(value):.0f}" if isinstance(value, (int, float)) else ""


def _step(manifest: Optional[Mapping[str, Any]], name: str) -> Dict[str, Any]:
    step = ((manifest or {}).get("steps") or {}).get(name) or {}
    return step[-1] if isinstance(step, list) and step else step if isinstance(step, dict) else {}


def classify(report: Optional[Mapping[str, Any]], manifest: Optional[Mapping[str, Any]],
             steps: Mapping[str, Any]) -> Tuple[str, str]:
    """(failure class, next action); ("", "") when nothing failed."""
    failed_checks = list((report or {}).get("failed") or [])
    if failed_checks:
        first = failed_checks[0]
        return f"preflight: {', '.join(failed_checks)}", NEXT_ACTION.get(first, "")
    for name, _label in STAGES:
        if _step(manifest, name).get("status") == "failed":
            return f"build stage: {name}", NEXT_ACTION.get(name, "")
    for name, _status in ((manifest or {}).get("steps") or {}).items():
        if name.startswith("python_tool_self_test.") and _step(manifest, name).get("status") == "failed":
            return f"build stage: {name}", NEXT_ACTION["python_tool_self_tests"]
    for step_id, label in WORKFLOW_STEPS:
        if (steps.get(step_id) or {}).get("outcome") == "failure":
            return f"workflow step: {label}", ""
    return "", ""


def render(*, version: str, job_status: str, report: Optional[Mapping[str, Any]],
           manifest: Optional[Mapping[str, Any]], steps: Mapping[str, Any]) -> str:
    failure, action = classify(report, manifest, steps)
    title = {"success": "published", "cancelled": "cancelled"}.get(job_status, "failed")
    lines: List[str] = [f"### Stable release {version or '(unresolved)'}: {title}", ""]
    if failure:
        lines += [f"**Failed at:** {failure}", ""]
        if action:
            lines += [f"**Next:** {action}", ""]
    if report:
        lines += [f"Preflight: {report.get('seconds', '?')} s, toolchain "
                  f"`{str(report.get('toolchain_sha256') or '?')[:12]}`", "",
                  "| check | result | seconds |", "|---|---|---:|"]
        for check in report.get("checks") or []:
            lines.append(f"| {check.get('name')} | {check.get('status')} | {check.get('seconds')} |")
        lines.append("")
    if manifest:
        lines += [f"Build gate: {_seconds(manifest.get('duration_seconds')) or '?'} s", "",
                  "| stage | result | seconds |", "|---|---|---:|"]
        for name, label in STAGES:
            step = _step(manifest, name)
            if step:
                lines.append(f"| {label} | {step.get('status', '?')} | {_seconds(step.get('duration_seconds'))} |")
        lines.append("")
    if job_status == "success":
        lines += ["Remaining step, in a local clone on main:", "",
                  f"    python tools/manage_changelog.py --promote-release {version}", "",
                  f"Then commit CHANGELOG.md. Until that runs, '## Unreleased' still holds v{version}'s entries."]
    elif not failure:
        lines.append("No stage reported a failure; the run was stopped from outside.")
    lines += ["", "Full logs stay on the runner: build/verification/<run>/ (run logs are deleted for privacy)."]
    return "\n".join(lines) + "\n"


def summarize_command(project_root: str) -> int:
    verification = os.path.join(project_root, "build", "verification")
    report = _load(os.path.join(verification, "release_preflight.json"))
    if report and report.get("mode") != "ci":
        report = None  # a local preflight on the runner's tree is not this run's
    manifest = manifest_for_this_run(_load(os.path.join(verification, "latest_manifest.json")), report)
    try:
        steps = json.loads(os.environ.get("STEPS_JSON") or "{}")
    except ValueError:
        steps = {}
    text = render(version=os.environ.get("VERSION", ""), job_status=os.environ.get("JOB_STATUS", ""),
                  report=report, manifest=manifest, steps=steps if isinstance(steps, dict) else {})
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a", encoding="utf-8") as target:
            target.write(text)
    else:
        print(text)
    return 0
