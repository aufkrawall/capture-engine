"""Process launch and reporting tests for the Python tool self-test harness."""

# pyright: reportAttributeAccessIssue=false

import errno
import os
import subprocess
import threading
import unittest
from unittest.mock import MagicMock, patch

import build
from tools import python_tool_self_tests
from tools.python_tool_self_tests import ToolSelfTestResult
from tools.stable_release import Preflight


class ToolSelfTestLaunchTest(unittest.TestCase):
    def test_only_access_denied_launches_retry_after_the_parallel_batch_finishes(self) -> None:
        names = ("recovers", "still_denied", "missing", "test_fails", "passes")
        commands = [(name, ["python", name]) for name in names]
        attempts: list[str] = []
        attempts_lock = threading.Lock()
        passed_suite_finished = threading.Event()

        def spawn(command, **kwargs):
            name = command[-1]
            with attempts_lock:
                is_retry = name in attempts
                attempts.append(name)
            if is_retry:
                self.assertTrue(passed_suite_finished.is_set())
            if name == "still_denied" or (name == "recovers" and not is_retry):
                raise PermissionError(errno.EACCES, "fixture access denied")
            if name == "missing":
                raise FileNotFoundError(errno.ENOENT, "fixture missing")

            process = MagicMock()
            process.__enter__.return_value = process
            process.returncode = 1 if name == "test_fails" else 0

            def collect_output(*args, **kwargs):
                if name == "passes":
                    passed_suite_finished.set()
                return "suite output", ""

            process.communicate.side_effect = collect_output
            return process

        with patch.object(python_tool_self_tests, "_commands", return_value=commands), patch.object(
            subprocess, "Popen", side_effect=spawn
        ) as launch:
            results = python_tool_self_tests.run_tool_self_tests(project_root="fixture", env={"EMPTY": ""}, jobs=2)

        self.assertCountEqual(attempts[:5], names)
        self.assertEqual(attempts[5:], ["recovers", "still_denied"])
        self.assertEqual([result.name for result in results], list(names))
        self.assertEqual([result.returncode for result in results], [0, None, None, 1, 0])
        self.assertEqual([len(result.launch_errors) for result in results], [1, 2, 1, 0, 0])
        self.assertEqual(results[0].stdout, "suite output")
        for call in launch.call_args_list:
            self.assertEqual(call.kwargs["env"], {"EMPTY": ""})
            self.assertEqual(call.kwargs["stdin"], subprocess.DEVNULL)

    def test_serial_launch_failure_reports_the_suite_and_keeps_the_gate_failed(self) -> None:
        error = PermissionError(errno.EACCES, "fixture access denied")
        inherited = {"EMPTY": ""}
        with patch.object(os, "environ", inherited), patch.object(
            python_tool_self_tests, "_commands", return_value=[("blocked", ["python"])]
        ), patch.object(
            subprocess, "Popen", side_effect=error
        ) as launch:
            results = python_tool_self_tests.run_tool_self_tests(project_root="fixture", jobs=1)
        self.assertEqual(launch.call_count, 1)
        self.assertEqual(launch.call_args.kwargs["env"], inherited)
        self.assertIsNone(results[0].returncode)

        with patch.object(build, "run_tool_self_tests", return_value=results), patch.object(
            build, "record_verification_step"
        ) as record, patch.object(build, "log") as log:
            self.assertFalse(build.run_python_tool_self_tests({}))
        self.assertEqual(record.call_args.args, ("python_tool_self_test.blocked", "failed"))
        self.assertIsNone(record.call_args.kwargs["details"]["exit_code"])
        self.assertEqual(record.call_args.kwargs["details"]["launch_errors"], [str(error)])
        self.assertTrue(any("blocked" in call.args[0] and "fixture access denied" in call.args[0]
                            for call in log.call_args_list))

    def test_release_preflight_explains_a_launch_failure(self) -> None:
        error = FileNotFoundError(errno.ENOENT, "fixture interpreter missing")
        results = [ToolSelfTestResult("blocked", ["python"], None, "", "", 0.01, (error,))]
        with patch.object(python_tool_self_tests, "run_tool_self_tests", return_value=results):
            result = Preflight(ci=True, version="0.1.1").check_self_tests()
        self.assertEqual(result.status, "fail")
        self.assertIn("blocked (launch failed)", "\n".join(result.details))
        self.assertIn(str(error), result.details)


if __name__ == "__main__":
    unittest.main()
