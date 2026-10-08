"""Boundary checks must run in both lint and the unfiltered closing gate."""

# build.py executes fragments dynamically; its attributes are runtime-only.
# pyright: reportAttributeAccessIssue=false

from __future__ import annotations

import json
import subprocess
import unittest
from types import SimpleNamespace
from unittest.mock import Mock, patch

import build
from tools.lint_driver import _check_module_boundaries, run_lint
from tools.python_tool_self_tests import ToolSelfTestResult, _commands


def valid_report() -> dict:
    return {
        "success": True, "include_edges": 12, "exceptions": 2, "used_exceptions": 2,
        "internal_headers": {"hook/runtime/main_internal.h": {"includers": 3, "outside_subsystem": 1}},
    }


class ModuleBoundaryGateTest(unittest.TestCase):
    def lint_context(self) -> SimpleNamespace:
        return SimpleNamespace(PROJECT_ROOT=build.PROJECT_ROOT, log=Mock(), record_verification_step=Mock(),
                               write_process_diagnostics_artifact=Mock())

    def test_lint_preserves_full_counts_and_requests_only_exception_pruning(self) -> None:
        b = self.lint_context()
        report = valid_report()
        with patch("tools.lint_driver.subprocess.run", return_value=subprocess.CompletedProcess([], 0,
                   stdout=json.dumps(report), stderr="")) as run:
            _check_module_boundaries({}, b)
        self.assertIn("--prune-exceptions", run.call_args.args[0])
        self.assertEqual(run.call_args.kwargs["cwd"], build.PROJECT_ROOT)
        self.assertEqual(run.call_args.kwargs["timeout"], 60)
        step = b.record_verification_step.call_args
        self.assertEqual(step.args, ("module_boundaries", "passed"))
        self.assertEqual(step.kwargs["details"]["internal_headers"], report["internal_headers"])
        self.assertTrue(any("3 includers" in call.args[0] for call in b.log.call_args_list))

    def test_advisory_lint_cannot_accept_a_boundary_regression(self) -> None:
        b = self.lint_context()
        report = {**valid_report(), "success": False}
        with patch("tools.lint_driver.subprocess.run", return_value=subprocess.CompletedProcess([], 1,
                   stdout=json.dumps(report), stderr="")):
            with self.assertRaises(SystemExit):
                run_lint({}, advisory=True, build_module=b)
        self.assertEqual(b.record_verification_step.call_args.args, ("module_boundaries", "failed"))

    def test_timeout_and_corrupt_output_fail_closed(self) -> None:
        for result in (subprocess.TimeoutExpired("checker", 60),
                       subprocess.CompletedProcess([], 0, stdout="not JSON", stderr=""),
                       subprocess.CompletedProcess([], 0, stdout="{}", stderr="")):
            with self.subTest(result=result):
                b = self.lint_context()
                with patch("tools.lint_driver.subprocess.run") as run, self.assertRaises(SystemExit):
                    if isinstance(result, Exception):
                        run.side_effect = result
                    else:
                        run.return_value = result
                    _check_module_boundaries({}, b)
                self.assertEqual(b.record_verification_step.call_args.args, ("module_boundaries", "failed"))

    def test_unfiltered_python_suite_registers_policy_live_tree_and_depth_checks(self) -> None:
        commands = dict(_commands(build.PROJECT_ROOT, "project-python"))
        self.assertIn("tools.tests.test_module_boundaries", commands["module_boundary_policy"])
        self.assertIn("tools.tests.test_module_boundary_gates", commands["module_boundary_gates"])
        self.assertIn("--json", commands["module_boundary_tree"])
        self.assertTrue(commands["module_depth"][-1].endswith("module_depth.py"))

    def test_closing_gate_records_and_reports_live_graph_metrics(self) -> None:
        report = valid_report()
        result = ToolSelfTestResult("module_boundary_tree", ["checker"], 0, json.dumps(report), "", 0.2)
        with patch.object(build, "run_tool_self_tests", return_value=[result]), \
                patch.object(build, "record_verification_step") as record, patch.object(build, "log") as log:
            self.assertTrue(build.run_python_tool_self_tests({}))
        self.assertEqual(record.call_args.args, ("python_tool_self_test.module_boundary_tree", "passed"))
        self.assertEqual(record.call_args.kwargs["details"]["metrics"], report)
        self.assertTrue(any("3 includers" in call.args[0] for call in log.call_args_list))

    def test_closing_gate_fails_on_live_violation_or_invalid_metrics(self) -> None:
        for stdout in (json.dumps({**valid_report(), "success": False}), "not JSON", "{}"):
            with self.subTest(stdout=stdout):
                result = ToolSelfTestResult("module_boundary_tree", ["checker"], 0, stdout, "", 0.2)
                with patch.object(build, "run_tool_self_tests", return_value=[result]), \
                        patch.object(build, "record_verification_step") as record, patch.object(build, "log"):
                    self.assertFalse(build.run_python_tool_self_tests({}))
                self.assertEqual(record.call_args.args, ("python_tool_self_test.module_boundary_tree", "failed"))


if __name__ == "__main__":
    unittest.main()
