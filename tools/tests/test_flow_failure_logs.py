# build.py executes its fragments via exec, so its module attributes exist only
# at runtime; pyright cannot see them through the facade.
# pyright: reportAttributeAccessIssue=false

import json
import os
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from unittest.mock import patch

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


class FlowSchedulingTest(unittest.TestCase):
    """The slowest scenarios start first, from the times the previous run recorded."""

    def test_longest_scenarios_start_first_and_unrecorded_ones_before_them(self) -> None:
        tests = ["Flow.A", "Flow.B", "Flow.C", "Flow.D", "Flow.E"]
        durations = {"Flow.A": 2.0, "Flow.B": 30.0, "Flow.C": 2.0, "Flow.E": 9.5}
        self.assertEqual(build.order_flow_tests(tests, durations), ["Flow.D", "Flow.B", "Flow.E", "Flow.A", "Flow.C"])
        self.assertEqual(build.order_flow_tests(tests, {}), tests)

    def test_a_filtered_run_keeps_the_other_scenarios_times(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = str(Path(temporary) / "scenario_seconds.json")
            self.assertEqual(build.load_flow_durations(path), {})
            build.save_flow_durations(path, {"Flow.A": 3.0, "Flow.B": 12.5})
            build.save_flow_durations(path, {"Flow.B": 11.0})
            self.assertEqual(build.load_flow_durations(path), {"Flow.A": 3.0, "Flow.B": 11.0})
            Path(path).write_text("not json", encoding="utf-8")
            self.assertEqual(build.load_flow_durations(path), {})


class FlowPassCacheTest(unittest.TestCase):
    """An unfiltered run is skipped when its inputs match the last passing run; a named or forced run never is, and
    a failed run withdraws the pass that would otherwise skip the next gate."""

    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.flow_exe = self.directory / "fg_flow_tests.exe"
        self.flow_exe.write_bytes(b"exe")
        # The runner's lines would land in the gate's own log, where a fake "FAILED" reads like a real one.
        for patcher in (patch.object(build, "FLOW_TEST_PASS_FILE", str(self.directory / "passed_inputs.json")),
                        patch.object(build, "FLOW_TEST_DURATIONS_FILE", str(self.directory / "scenario_seconds.json")),
                        patch.object(build, "verification_artifact_path", return_value=None),
                        patch.object(build, "log")):
            patcher.start()
            self.addCleanup(patcher.stop)

    def run_flow(self, key: str, rerun=False, gtest_filter=None, listed=("Flow.A",), exit_code=0):
        with patch.object(build, "flow_test_input_key", return_value=key), patch.object(
            build, "list_flow_tests", return_value=None if listed is None else list(listed)
        ) as listing, patch.object(build, "run_flow_scenario_process", return_value=(exit_code, "")):
            ok = build.run_flow_tests({}, str(self.flow_exe), gtest_filter, rerun)
        return ok, listing.called

    def test_the_same_inputs_are_not_run_twice(self) -> None:
        self.assertEqual(self.run_flow("k1"), (True, True))
        self.assertEqual(self.run_flow("k1"), (True, False))
        self.assertEqual(self.run_flow("k2"), (True, True), "changed inputs run again")

    def test_a_forced_or_filtered_run_always_runs(self) -> None:
        self.run_flow("k1")
        self.assertEqual(self.run_flow("k1", rerun=True), (True, True))
        self.assertEqual(self.run_flow("k1", gtest_filter="Flow.A"), (True, True))
        self.assertEqual(self.run_flow("k1"), (True, False), "a passing forced or filtered run keeps the pass")

    def test_a_failed_or_filtered_run_records_no_pass(self) -> None:
        self.assertEqual(self.run_flow("k1", exit_code=1), (False, True))
        self.assertIsNone(build.flow_tests_passed_with("k1"))
        self.run_flow("k1", gtest_filter="Flow.A")
        self.assertIsNone(build.flow_tests_passed_with("k1"))

    def test_a_failed_rerun_withdraws_the_earlier_pass(self) -> None:
        # Pass, then a forced or filtered rerun with the same inputs fails: the next ordinary gate must run again.
        for forced, gtest_filter in ((True, None), (False, "Flow.A")):
            with self.subTest(forced=forced, gtest_filter=gtest_filter):
                self.run_flow("k1")
                self.assertIsNotNone(build.flow_tests_passed_with("k1"))
                self.assertEqual(self.run_flow("k1", rerun=forced, gtest_filter=gtest_filter, exit_code=1),
                                 (False, True))
                self.assertIsNone(build.flow_tests_passed_with("k1"))
                self.assertEqual(self.run_flow("k1", exit_code=1), (False, True), "the next gate runs the scenarios")

    def test_a_failed_or_empty_listing_is_no_pass(self) -> None:
        self.run_flow("k1")
        self.assertEqual(self.run_flow("k1", rerun=True, listed=None), (False, True))
        self.assertIsNone(build.flow_tests_passed_with("k1"), "a listing failure withdraws the pass")
        self.assertEqual(self.run_flow("k1", listed=()), (False, True), "an unfiltered run that lists nothing fails")
        self.assertEqual(self.run_flow("k1", gtest_filter="Unit*", listed=()), (True, True),
                         "a filter that names no scenario is no failure")
        self.assertEqual(self.run_flow("k1", gtest_filter="Unit*", listed=None), (False, True))

    def test_a_listing_that_exits_non_zero_lists_nothing(self) -> None:
        # The interpreter refuses the unknown --gtest_list_tests option with exit code 2, as a crashing executable
        # would end its listing: no scenario names, and no empty list either.
        with patch.object(build, "FLOW_TEST_OUTPUT_DIR", str(self.directory)):
            self.assertIsNone(build.list_flow_tests(sys.executable, None))


class FlowInputKeyTest(unittest.TestCase):
    """The input key covers the link inputs no depfile names (2026-10-25: a changed .rc version resource or .def
    export list rebuilt the fake runtimes and kept the key, so the gate reused a pass of the old binaries)."""

    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        objects = root / "obj" / "x64-flow" / "fakes" / "sl.interposer"
        objects.mkdir(parents=True)
        output = root / "flow_tests"
        output.mkdir()
        compiler = root / "bin" / "clang++.exe"
        compiler.parent.mkdir()
        compiler.write_bytes(b"compiler")
        self.source = root / "sl_interposer.cpp"
        self.source.write_text("int flow;", encoding="utf-8")
        self.object = objects / "sl_interposer.o"
        self.object.write_bytes(b"object")
        (objects / "sl_interposer.o.d").write_text(f"{self.object.as_posix()}: {self.source.as_posix()}\n",
                                                   encoding="utf-8")
        self.resource = objects / "sl_version.res.o"
        self.resource.write_bytes(b"FILEVERSION 2, 7, 32, 0")
        self.exports = root / "sl.interposer.def"
        self.exports.write_text("EXPORTS\n    D3D12CreateDevice=flowD3D12CreateDevice\n", encoding="utf-8")
        command = [str(compiler), str(self.object), str(self.resource), str(self.exports), "-shared",
                   "-o", str(output / "sl.interposer.dll")]
        (output / "sl.interposer.dll.link-cache.json").write_text(
            json.dumps({"schema": 1, "command": command, "cwd": None}), encoding="utf-8")
        # Nothing of the real toolchain or system: the key sees only this layout.
        for patcher in (patch.object(build, "OBJ_DIR", str(root / "obj")),
                        patch.object(build, "FLOW_TEST_OUTPUT_DIR", str(output)),
                        patch.object(build, "MSYS2_DIR", str(root / "msys64")),
                        patch.object(build, "FLOW_TEST_SYSTEM_MODULES", ()),
                        patch.object(build, "get_compiler_exe", return_value=str(compiler)),
                        patch.object(build, "log")):
            patcher.start()
            self.addCleanup(patcher.stop)

    def test_resource_and_export_files_are_link_inputs(self) -> None:
        inputs = {os.path.normcase(path) for path in build.flow_test_link_inputs()}
        self.assertIn(os.path.normcase(str(self.resource)), inputs)
        self.assertIn(os.path.normcase(str(self.exports)), inputs)
        self.assertNotIn(os.path.normcase(str(self.object)), inputs, "a compiled object is keyed by its depfile")

    def test_every_build_input_changes_the_key(self) -> None:
        for path, content in ((self.resource, b"FILEVERSION 1, 0, 0, 0"),
                              (self.exports, b"EXPORTS\n    CreateDXGIFactory=flowCreateDXGIFactory\n"),
                              (self.source, b"int flow = 1;")):
            with self.subTest(path=path.name):
                before = build.flow_test_input_key()
                path.write_bytes(content)
                self.assertNotEqual(build.flow_test_input_key(), before)

    def test_a_rebuilt_object_with_the_same_sources_keeps_the_key(self) -> None:
        # build_identity.o is rebuilt with every version string; no scenario sees it.
        before = build.flow_test_input_key()
        self.object.write_bytes(b"object with another version string")
        self.assertEqual(build.flow_test_input_key(), before)


class TestPhaseTest(unittest.TestCase):
    """Unit and flow tests run side by side, and each reports even when the other fails."""

    def setUp(self) -> None:
        patcher = patch.object(build, "log")
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_both_suites_run_at_once_and_either_failure_fails_the_phase(self) -> None:
        for unit_result, flow_result in ((True, True), (False, True), (True, False)):
            both_running = threading.Barrier(2, timeout=60)
            ran = []

            def unit(*_args, result=unit_result, **_kwargs) -> bool:
                both_running.wait()  # raises unless the flow suite runs at the same time
                ran.append("unit")
                return result

            def flow(*_args, result=flow_result, **_kwargs) -> bool:
                both_running.wait()
                ran.append("flow")
                return result

            with patch.object(build, "run_tests", side_effect=unit), patch.object(
                build, "run_flow_tests", side_effect=flow
            ):
                self.assertEqual(build.run_test_phase({}, "unit_tests.exe", "fg_flow_tests.exe"),
                                 unit_result and flow_result)
            self.assertCountEqual(ran, ["unit", "flow"])

    def test_no_unit_test_binary_still_runs_the_flow_suite(self) -> None:
        with patch.object(build, "run_tests") as unit, patch.object(build, "run_flow_tests", return_value=True) as flow:
            self.assertTrue(build.run_test_phase({}, None, "fg_flow_tests.exe"))
        unit.assert_not_called()
        flow.assert_called_once()


if __name__ == "__main__":
    unittest.main()
