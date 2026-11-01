"""Release preflight, dispatch policy and the privacy-safe job summary."""

from __future__ import annotations

import json
import os
import tempfile
import types
import unittest
from pathlib import Path
from typing import Any, Dict, List
from unittest import mock

import yaml

from tools import release_summary, stable_release
from tools.stable_release import CheckResult, Preflight, ReleaseError, choose_version

WORKFLOW = Path(__file__).resolve().parents[2] / ".github" / "workflows" / "release-stable.yml"


class VersionTest(unittest.TestCase):
    def test_next_version_follows_the_newest_tag(self) -> None:
        self.assertEqual(choose_version("", [7155, 7158, 7157]), "0.1.7159")

    def test_requested_version_is_validated_and_never_reuses_a_tag(self) -> None:
        self.assertEqual(choose_version(" 0.1.7160 ", [7158]), "0.1.7160")
        for bad in ("0.2.1", "7160", "0.1.x", "0.1.0"):
            with self.assertRaises(ReleaseError, msg=bad):
                choose_version(bad, [7158])
        with self.assertRaisesRegex(ReleaseError, "already exists"):
            choose_version("0.1.7158", [7158])
        with self.assertRaisesRegex(ReleaseError, "pass --version"):
            choose_version("", [])

    def test_tags_are_read_from_ls_remote(self) -> None:
        listing = "a\trefs/tags/v0.1.7158\nb\trefs/tags/v0.1.7159\nc\trefs/tags/v0.1.7159^{}\nd\trefs/tags/v0.2.1\n"
        completed = types.SimpleNamespace(returncode=0, stdout=listing, stderr="")
        with mock.patch.object(stable_release, "_run", return_value=completed):
            self.assertEqual(stable_release.remote_release_numbers(), [7158, 7159])


class EnvironmentTest(unittest.TestCase):
    def test_git_auth_header_exists_only_in_the_release_job(self) -> None:
        with mock.patch.dict(os.environ, {"GH_TOKEN": "secret-token", "GITHUB_ACTIONS": "true"}):
            env = stable_release._git_env()
        self.assertEqual(env["GIT_CONFIG_KEY_0"], "http.https://github.com/.extraheader")
        self.assertNotIn("secret-token", env["GIT_CONFIG_VALUE_0"])  # base64, never the raw token
        with mock.patch.dict(os.environ, {"GH_TOKEN": "secret-token"}, clear=False):
            os.environ.pop("GITHUB_ACTIONS", None)
            self.assertNotIn("GIT_CONFIG_COUNT", stable_release._git_env())

    def test_release_job_output_is_redacted(self) -> None:
        profile = r"C:\Users\TestUser"
        with mock.patch.dict(os.environ, {"USERPROFILE": profile, "COMPUTERNAME": "DESKBOX",
                                          "CE_PRIVACY_SANITIZE_LOGS": "1"}):
            text = stable_release._redact(r"C:\Users\TestUser\x and C:/Users/TestUser/y on deskbox")
        self.assertEqual(text, r"%USERPROFILE%\x and %USERPROFILE%/y on <host>")
        with mock.patch.dict(os.environ, {"USERPROFILE": profile, "CE_PRIVACY_SANITIZE_LOGS": "0"}):
            self.assertIn(profile, stable_release._redact(profile))


def _fake_build(**attributes: Any) -> Any:
    return types.SimpleNamespace(log=lambda *args, **kwargs: None, **attributes)


class PreflightCompositionTest(unittest.TestCase):
    def names(self, **kwargs: Any) -> List[str]:
        return [name for name, _ in Preflight(build_module=_fake_build(), **kwargs).checks()]

    def test_release_job_mode_skips_local_only_checks(self) -> None:
        ci = self.names(ci=True, version="0.1.1")
        self.assertNotIn("git_state", ci)
        self.assertNotIn("clang_tidy_ratchet", ci)
        for required in ("flake8", "pyright", "changelog", "size_ratchets", "python_self_tests", "toolchain",
                         "closure_proofs", "sources"):
            self.assertIn(required, ci)

    def test_local_mode_checks_git_state_and_optional_clang_tidy(self) -> None:
        local = self.names(ci=False, version="0.1.1", clang_tidy=True, prefetch=False)
        self.assertEqual(local[0], "git_state")
        self.assertIn("clang_tidy_ratchet", local)
        self.assertNotIn("sources", local)

    def test_build_output_suites_are_left_to_the_build(self) -> None:
        captured: Dict[str, Any] = {}

        def fake_run(**kwargs: Any) -> list:
            captured.update(kwargs)
            return [types.SimpleNamespace(name="x", returncode=0, stdout="", stderr="")]

        with mock.patch("tools.python_tool_self_tests.run_tool_self_tests", side_effect=fake_run):
            result = Preflight(ci=True, version="0.1.1", build_module=_fake_build()).check_self_tests()
        self.assertEqual(result.status, "pass")
        self.assertEqual(set(captured["exclude"]), {"installer_native", "installer_ui"})

    def test_a_failing_check_fails_the_preflight_and_lands_in_the_report(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            def failing() -> CheckResult:
                return CheckResult("flake8", "fail", 0.0, "exit 1", ["x.py:1:121: E501"])

            def passing() -> CheckResult:
                return CheckResult("python", "pass", 0.0, "3.15")

            with mock.patch.object(stable_release, "PROJECT_ROOT", root), \
                    mock.patch.object(Preflight, "checks", lambda self: [("python", passing), ("flake8", failing)]), \
                    mock.patch.object(Preflight, "_build", lambda self: _fake_build()), \
                    mock.patch("builtins.print"):
                results, _ = stable_release.run_preflight(ci=True, version="0.1.9", prefetch=False, clang_tidy=False)
            report = json.loads(Path(root, "build", "verification", "release_preflight.json").read_text("utf-8"))
        self.assertEqual([result.status for result in results], ["pass", "fail"])
        self.assertEqual(report["failed"], ["flake8"])
        self.assertEqual(report["mode"], "ci")
        self.assertNotIn("details", report["checks"][1], "check details may hold paths; the report keeps none")

    def test_a_crashing_check_is_a_failure_not_a_crash(self) -> None:
        def broken() -> CheckResult:
            raise OSError("disk gone")

        result = stable_release._timed("toolchain", broken)
        self.assertEqual((result.status, result.summary), ("fail", "OSError: disk gone"))


class ClosureProofCheckTest(unittest.TestCase):
    def setUp(self) -> None:
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        from tools import toolchain_state

        self.state = toolchain_state
        self.dir = temp.name
        self.packages = ["doxygen-1.18.0-5", "headers-14.0.0.r569-1"]
        self.build = _fake_build(
            toolchain_state=toolchain_state,
            MSYS2_DIR="unused",
            toolchain_state_dir=lambda: self.dir,
            closure_input_fingerprints=lambda: {"dependency_closure": "deps", "ffmpeg": "ff"},
        )

    def run_check(self) -> tuple:
        preflight = Preflight(ci=True, version="0.1.1", build_module=self.build)
        with mock.patch.object(self.state, "toolchain_packages", return_value=self.packages):
            return preflight.check_closure_proofs(), preflight

    def test_known_drift_names_the_changed_packages(self) -> None:
        self.state.record_proof(self.dir, "dependency_closure", "deps", ["doxygen-1.18.0-1", "headers-14.0.0.r302-1"],
                                "release")
        self.state.record_proof(self.dir, "ffmpeg", "ff", self.packages, "build")
        result, preflight = self.run_check()
        self.assertEqual(result.status, "warn")
        self.assertEqual(preflight.closure_drift, ["dependency_closure"])
        self.assertIn("    doxygen 1.18.0-1 -> 1.18.0-5", result.details)
        self.assertTrue(any("rehearse_dependency_closure.py" in line for line in result.details))

    def test_proven_toolchain_passes(self) -> None:
        for kind, inputs in (("dependency_closure", "deps"), ("ffmpeg", "ff")):
            self.state.record_proof(self.dir, kind, inputs, self.packages, "release")
        result, preflight = self.run_check()
        self.assertEqual(result.status, "pass")
        self.assertEqual(preflight.closure_drift, [])

    def test_no_history_warns_without_blocking_dispatch(self) -> None:
        result, preflight = self.run_check()
        self.assertEqual(result.status, "warn")
        self.assertEqual(preflight.closure_drift, [])


class DispatchPolicyTest(unittest.TestCase):
    def arguments(self, **overrides: Any) -> Any:
        values = dict(version="", attest="always", yes=False, watch=False, skip_clang_tidy=True,
                      accept_unproven_toolchain=False)
        values.update(overrides)
        return types.SimpleNamespace(**values)

    def dispatch(self, preflight: Any, results: List[CheckResult], **overrides: Any) -> tuple:
        calls: List[List[str]] = []

        def fake_run(command, **_kwargs):
            calls.append(list(command))
            return types.SimpleNamespace(returncode=0, stdout="abc123\n", stderr="")

        with mock.patch.object(stable_release, "remote_release_numbers", return_value=[7158]), \
                mock.patch.object(stable_release, "run_preflight", return_value=(results, preflight)), \
                mock.patch.object(stable_release, "_gh_json", return_value={"nameWithOwner": "o/r"}), \
                mock.patch.object(stable_release, "runner_status", return_value={"status": "online", "busy": False}), \
                mock.patch.object(stable_release, "find_dispatched_run", return_value=None), \
                mock.patch.object(stable_release, "_run", side_effect=fake_run), \
                mock.patch("builtins.print"):
            code = stable_release.dispatch(self.arguments(**overrides))
        return code, calls

    def test_failures_and_known_toolchain_drift_block_the_dispatch(self) -> None:
        ok = [CheckResult("flake8", "pass", 0.0, "clean")]
        code, calls = self.dispatch(types.SimpleNamespace(closure_drift=[]), [CheckResult("flake8", "fail", 0, "x")])
        self.assertEqual(code, 1)
        code, calls = self.dispatch(types.SimpleNamespace(closure_drift=["dependency_closure"]), ok)
        self.assertEqual(code, 1)
        self.assertFalse(any("workflow" in command for command in calls))

    def test_dispatch_is_a_dry_run_without_yes(self) -> None:
        ok = [CheckResult("flake8", "pass", 0.0, "clean")]
        code, calls = self.dispatch(types.SimpleNamespace(closure_drift=[]), ok)
        self.assertEqual(code, 0)
        self.assertFalse(any(command[:3] == ["gh", "workflow", "run"] for command in calls))
        code, calls = self.dispatch(types.SimpleNamespace(closure_drift=["ffmpeg"]), ok, yes=True,
                                    accept_unproven_toolchain=True)
        self.assertEqual(code, 0)
        dispatched = [command for command in calls if command[:3] == ["gh", "workflow", "run"]]
        self.assertEqual(dispatched, [["gh", "workflow", "run", "release-stable.yml", "--ref", "main",
                                       "-f", "version=0.1.7159", "-f", "attest=always"]])


class SummaryTest(unittest.TestCase):
    MANIFEST = {
        "start_time": "2026-11-01T15:00:00.000",
        "run_dir": r"C:\Users\TestUser\runner-work\build\verification\x",
        "duration_seconds": 2600,
        "steps": {
            "external_preparation": {"status": "passed", "duration_seconds": 980.0},
            "flow_tests": {"status": "failed", "duration_seconds": 446.0},
        },
    }

    def test_preflight_failures_are_named_with_the_next_action(self) -> None:
        failure, action = release_summary.classify({"failed": ["flake8"]}, None, {})
        self.assertEqual(failure, "preflight: flake8")
        self.assertIn("flake8", action)

    def test_failed_build_stage_and_workflow_step_are_classified(self) -> None:
        self.assertEqual(release_summary.classify({"failed": []}, self.MANIFEST, {})[0], "build stage: flow_tests")
        failure, _ = release_summary.classify({"failed": []}, None, {"publish": {"outcome": "failure"}})
        self.assertEqual(failure, "workflow step: tag and GitHub release")
        self.assertEqual(release_summary.classify({"failed": []}, None, {}), ("", ""))

    def test_summary_carries_no_paths_and_tells_how_to_promote_the_changelog(self) -> None:
        report = {"started_at": "2026-11-01T14:59:00", "seconds": 41.2, "toolchain_sha256": "ab" * 32,
                  "checks": [{"name": "flake8", "status": "pass", "seconds": 4.1}], "failed": []}
        failed = release_summary.render(version="0.1.7159", job_status="failure", report=report,
                                        manifest=self.MANIFEST, steps={})
        self.assertIn("**Failed at:** build stage: flow_tests", failed)
        self.assertIn("| FG flow scenarios | failed | 446 |", failed)
        self.assertIn("Build gate: 2600 s", failed)
        self.assertNotIn("Users", failed)
        published = release_summary.render(version="0.1.7159", job_status="success", report=report,
                                           manifest=None, steps={})
        self.assertIn("python tools/manage_changelog.py --promote-release 0.1.7159", published)

    def test_a_manifest_from_an_earlier_build_is_ignored(self) -> None:
        report = {"started_at": "2026-11-01T16:00:00"}
        self.assertIsNone(release_summary.manifest_for_this_run(self.MANIFEST, report))
        self.assertIs(release_summary.manifest_for_this_run(self.MANIFEST, {"started_at": "2026-11-01T14:00:00"}),
                      self.MANIFEST)


class WorkflowOrderTest(unittest.TestCase):
    def setUp(self) -> None:
        document = yaml.safe_load(WORKFLOW.read_text(encoding="utf-8"))
        self.steps = document["jobs"]["build-release"]["steps"]
        self.index = {step["name"]: position for position, step in enumerate(self.steps)}

    def step(self, prefix: str) -> Dict[str, Any]:
        return next(step for step in self.steps if step["name"].startswith(prefix))

    def test_the_preflight_runs_before_any_compile_and_without_a_token(self) -> None:
        preflight = self.step("Release preflight")
        self.assertIn("tools/stable_release.py preflight --ci", preflight["run"])
        self.assertNotIn("GH_TOKEN", preflight.get("env") or {})
        self.assertEqual((preflight.get("env") or {}).get("CE_PRIVACY_SANITIZE_LOGS"), "1")
        self.assertLess(self.steps.index(preflight), self.steps.index(self.step("Build release product")))
        self.assertLess(self.steps.index(self.step("Retire the previous FFmpeg")), self.steps.index(preflight))

    def test_summary_and_trash_cleanup_always_run_last(self) -> None:
        summary = self.step("Summarize the run")
        cleanup = self.step("Delete retired FFmpeg build trees")
        self.assertEqual(summary.get("if"), "always()")
        self.assertEqual(cleanup.get("if"), "always()")
        self.assertNotIn("GH_TOKEN", summary.get("env") or {})
        self.assertGreater(self.steps.index(summary), self.steps.index(self.step("Publish stable tag")))
        self.assertIn("-not $_.LinkType", cleanup["run"], "the trash cleanup must never recurse into a junction")

    def test_summary_step_ids_match_the_classifier(self) -> None:
        ids = {step.get("id") for step in self.steps}
        for step_id, _label in release_summary.WORKFLOW_STEPS:
            self.assertIn(step_id, ids)


if __name__ == "__main__":
    unittest.main()
