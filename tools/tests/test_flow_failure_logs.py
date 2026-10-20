# build.py executes its fragments via exec, so its module attributes exist only
# at runtime; pyright cannot see them through the facade.
# pyright: reportAttributeAccessIssue=false

import sys
import tempfile
import unittest
from pathlib import Path

import build

DESKTOP_PROBE = """
import ctypes, sys
from ctypes import wintypes
user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
user32.GetThreadDesktop.argtypes = [wintypes.DWORD]
user32.GetThreadDesktop.restype = wintypes.HANDLE
user32.GetUserObjectInformationW.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD,
                                             ctypes.c_void_p]
name = ctypes.create_unicode_buffer(256)
user32.GetUserObjectInformationW(user32.GetThreadDesktop(kernel32.GetCurrentThreadId()), 2, name,
                                 ctypes.sizeof(name), None)
print(name.value)
print("warning text", file=sys.stderr)
sys.exit(3)
"""


class FlowFailureLogsTest(unittest.TestCase):
    """A failed flow scenario's logs survive the next run of the scenario (FlowFSR, 2026-10-17)."""

    def test_failed_scenario_logs_are_copied_into_the_run_folder(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            logs = Path(temporary) / "flow_tests" / "logs" / "FlowFSR.Example"
            logs.mkdir(parents=True)
            (logs / "hook_debug.log").write_text("hook", encoding="utf-8")
            (logs / "d3d12_debug.log").write_text("frame 364 ERROR id=907", encoding="utf-8")
            destination = Path(temporary) / "verification" / "run" / "flow_logs" / "FlowFSR.Example"

            kept = build.preserve_failed_flow_logs(str(logs), str(destination))

            self.assertEqual(kept, str(destination))
            self.assertEqual((destination / "d3d12_debug.log").read_text(encoding="utf-8"), "frame 364 ERROR id=907")
            # The rerun recreates the scenario folder; the kept copy stays as it was.
            (logs / "d3d12_debug.log").write_text("rerun", encoding="utf-8")
            self.assertEqual((destination / "hook_debug.log").read_text(encoding="utf-8"), "hook")
            self.assertEqual((destination / "d3d12_debug.log").read_text(encoding="utf-8"), "frame 364 ERROR id=907")

    def test_nothing_is_kept_without_a_run_folder_or_logs(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            logs = Path(temporary) / "logs"
            logs.mkdir()
            self.assertIsNone(build.preserve_failed_flow_logs(str(logs), None))
            missing = Path(temporary) / "missing"
            self.assertIsNone(build.preserve_failed_flow_logs(str(missing), str(Path(temporary) / "out")))


class FlowScenarioProcessTest(unittest.TestCase):
    """A scenario process returns its exit code and output, and on Windows runs on a desktop of its own."""

    def test_exit_code_and_both_output_streams_come_back(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            code, output = build.run_flow_scenario_process(
                [sys.executable, "-c", "import sys; sys.stdout.write('out'); sys.stderr.write('err'); sys.exit(4)"],
                directory,
                120,
            )
        self.assertEqual((code, output), (4, "outerr"))

    @unittest.skipUnless(build.IS_WINDOWS, "scenario desktops are a Windows mechanism")
    def test_scenario_process_runs_on_a_private_desktop(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            code, output = build.run_flow_scenario_process([sys.executable, "-c", DESKTOP_PROBE], directory, 120)
        self.assertEqual(code, 3, output)
        desktop, warning = output.splitlines()
        # The desktop is the scenario's own, never the runner's or the default desktop.
        self.assertTrue(desktop.startswith("CE_FG_"), output)
        self.assertEqual(warning, "warning text")


if __name__ == "__main__":
    unittest.main()
