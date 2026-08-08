"""Workflow security policy: nothing an outsider can trigger may run on this PC.

The `release-stable` job runs on a self-hosted Windows runner that IS the
maintainer's development machine, with `build\\msys64` and `external` junctioned to
the real toolchain tree, executing as the maintainer. GitHub explicitly advises
against self-hosted runners on public repositories for exactly this reason: any
workflow an outside contributor can trigger executes their code there.

The GitHub-side control ("require approval for fork pull request workflows")
**cannot be configured while the repository is private** - the REST API answers
`422 Fork PR approval is not allowed for private repositories`. So it cannot be
set in advance of going public, and there is a window after flipping visibility in
which a laxer default applies. This module is the part that does not depend on
remembering a setting: it fails closed in the repository itself, today, and keeps
failing if someone later adds a fork-triggerable event to a self-hosted workflow.

While the repository is private, `run_workflows_from_fork_pull_requests` is false,
so fork PRs cannot run at all; that is the current containment.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path
from typing import Dict, List, Set

import yaml

WORKFLOW_DIR = Path(__file__).resolve().parents[2] / ".github" / "workflows"

# Events no one outside the repository can cause. Everything else is treated as
# outsider-triggerable, so an event nobody thought about fails closed rather than
# being quietly permitted on the maintainer's machine.
TRUSTED_EVENTS = frozenset(
    {
        "workflow_dispatch",
        "workflow_call",
        "schedule",
        "push",
        "release",
        "repository_dispatch",
        "deployment",
        "deployment_status",
    }
)


def _load(path: Path) -> Dict:
    with open(path, "r", encoding="utf-8") as handle:
        return yaml.safe_load(handle)


def _events(document: Dict) -> Set[str]:
    """Trigger names for a workflow.

    `on` is a YAML 1.1 boolean, so `yaml.safe_load` returns it as the key `True`
    rather than the string "on". Missing that is the classic Actions YAML trap and
    would make this gate silently pass on every file.
    """
    trigger = document.get("on", document.get(True))
    if trigger is None:
        return set()
    if isinstance(trigger, str):
        return {trigger}
    if isinstance(trigger, list):
        return set(trigger)
    return set(trigger.keys())


def _runs_on(document: Dict) -> Set[str]:
    labels: Set[str] = set()
    for job in (document.get("jobs") or {}).values():
        target = job.get("runs-on")
        if isinstance(target, str):
            labels.add(target)
        elif isinstance(target, list):
            labels.update(str(item) for item in target)
        elif isinstance(target, dict):  # runs-on: {group:..., labels:[...]}
            value = target.get("labels", [])
            labels.update([value] if isinstance(value, str) else [str(v) for v in value])
    return labels


def _workflows() -> List[Path]:
    return sorted(WORKFLOW_DIR.glob("*.yml")) + sorted(WORKFLOW_DIR.glob("*.yaml"))


class WorkflowSecurityPolicyTest(unittest.TestCase):
    def test_the_yaml_on_key_trap_is_handled(self) -> None:
        # Guards the helper the rest of this module depends on: if `_events` ever
        # stops seeing triggers, every assertion below passes vacuously.
        parsed = yaml.safe_load("on:\n  workflow_dispatch:\njobs: {}\n")
        self.assertNotIn("on", parsed, "PyYAML should parse the `on` key as boolean True")
        self.assertEqual(_events(parsed), {"workflow_dispatch"})
        self.assertEqual(_events(yaml.safe_load("on: [push, pull_request]\n")), {"push", "pull_request"})

    def test_workflows_are_discovered(self) -> None:
        # A glob that silently matches nothing would make this module useless.
        self.assertGreaterEqual(len(_workflows()), 3, "expected the repository's workflows to be found")

    def test_no_self_hosted_workflow_can_be_triggered_by_an_outsider(self) -> None:
        findings: List[str] = []
        for path in _workflows():
            document = _load(path)
            labels = _runs_on(document)
            if not any("self-hosted" in label for label in labels):
                continue
            untrusted = sorted(_events(document) - TRUSTED_EVENTS)
            if untrusted:
                findings.append(f"{path.name}: runs on {sorted(labels)} and triggers on {untrusted}")
        self.assertEqual(
            [],
            findings,
            "A workflow on the self-hosted runner can be triggered by someone outside the "
            "repository. That runner is the maintainer's own machine with the real toolchain "
            "junctioned in, so this would execute outside code there. Move the job to a "
            "GitHub-hosted runner or restrict the trigger:\n" + "\n".join(findings),
        )

    def test_release_stable_stays_manual_only(self) -> None:
        # Pinned separately from the rule above so the intent survives even if the
        # allowlist is ever widened: this job publishes releases and holds write
        # permissions, so it must never gain an automatic trigger.
        document = _load(WORKFLOW_DIR / "release-stable.yml")
        self.assertEqual(_events(document), {"workflow_dispatch"})
        self.assertTrue(
            any("self-hosted" in label for label in _runs_on(document)),
            "release-stable is expected to be the self-hosted job; re-check this module if that changed",
        )

    def test_the_log_cleanup_job_stays_github_hosted(self) -> None:
        # It is triggered by workflow_run, which chains off the release job, and it
        # holds actions:write to delete logs. It must not run on the self-hosted PC.
        document = _load(WORKFLOW_DIR / "release-log-cleanup.yml")
        labels = _runs_on(document)
        self.assertTrue(labels, "expected a runs-on label")
        for label in labels:
            self.assertNotIn("self-hosted", label)

    def test_actions_are_pinned_to_full_commit_shas(self) -> None:
        findings: List[str] = []
        for path in _workflows():
            document = _load(path)
            for job_name, job in (document.get("jobs") or {}).items():
                for step in job.get("steps") or []:
                    action = step.get("uses")
                    if action and action.startswith("actions/") and not re.fullmatch(
                        r"actions/[^@]+@[0-9a-f]{40}", action
                    ):
                        findings.append(f"{path.name}:{job_name}: {action}")
        self.assertEqual([], findings, "Official Actions must be immutable commit pins: " + ", ".join(findings))

    def test_release_build_is_exact_full_clean_verification(self) -> None:
        document = _load(WORKFLOW_DIR / "release-stable.yml")
        job = document["jobs"]["build-release"]
        steps = {step["name"]: step for step in job["steps"]}
        sync = steps["Sync repository (persistent workspace, never cleans)"]["run"]
        build = steps["Build release product"]
        self.assertIn("$env:GITHUB_REF -ne 'refs/heads/main'", sync)
        self.assertIn("git switch -C main $env:GITHUB_SHA", sync)
        self.assertIn("$actual -ne $env:GITHUB_SHA", sync)
        self.assertIn("python build.py --verify --verify-clean --skip-updates --concise", build["run"])
        self.assertNotIn("gh auth setup-git", sync)
        self.assertIn("GIT_CONFIG_VALUE_0", sync)
        self.assertNotIn("GITHUB_TOKEN", job.get("env") or {})
        self.assertNotIn("GH_TOKEN", build.get("env") or {})

    def test_release_publishes_and_attests_corresponding_source(self) -> None:
        document = _load(WORKFLOW_DIR / "release-stable.yml")
        steps = {step["name"]: step for step in document["jobs"]["build-release"]["steps"]}
        source_name = "ffmpeg-corresponding-source.7z"
        self.assertIn(source_name, steps["Attest release assets"]["with"]["subject-path"])
        self.assertIn(source_name, steps["Publish stable tag and GitHub release"]["run"])


if __name__ == "__main__":
    unittest.main()
