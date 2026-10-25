"""Behavioral fixtures for the blocking-WinRT-wait check (the 2026-10-24 screenshot freeze)."""

from __future__ import annotations

import contextlib
import io
import tempfile
import unittest
from pathlib import Path

from tools import check_winrt_blocking_waits as check
from tools.python_tool_self_tests import _commands

# The shape of the code that froze the engine thread (EnsureBorderlessAccessRequested before 55d7a6c3).
FROZE_THE_ENGINE_THREAD = """\
bool EnsureBorderlessAccessRequested() {
    std::call_once(g_BorderlessAccessRequestOnce, []() {
        try {
            const auto status = winrt::Windows::Graphics::Capture::GraphicsCaptureAccess::RequestAccessAsync(
                                    winrt::Windows::Graphics::Capture::GraphicsCaptureAccessKind::Borderless)
                                    .get();
        } catch (...) {
        }
    });
}
"""


class BlockingWaitDetectionTest(unittest.TestCase):
    def test_the_wait_that_froze_the_engine_thread_is_found_on_its_line(self) -> None:
        self.assertEqual(check.blocking_waits(FROZE_THE_ENGINE_THREAD), [(6, "RequestAccessAsync(...).get()")])

    def test_a_bounded_wait_blocks_too(self) -> None:
        text = "void F() { auto s = item.TryCreateAsync(a, (b + c)).wait_for(std::chrono::seconds(1)); }\n"
        self.assertEqual(check.blocking_waits(text), [(1, "TryCreateAsync(...).wait_for()")])

    def test_an_operation_kept_in_a_variable_and_waited_on_later_is_found(self) -> None:
        text = (
            "void F() {\n"
            "    auto operation = Picker().PickSingleItemAsync();\n"
            "    Log();\n"
            "    auto item = operation.get();\n"
            "}\n"
        )
        self.assertEqual(check.blocking_waits(text), [(4, "operation.get()")])

    def test_awaiting_is_not_waiting(self) -> None:
        text = (
            "IAsyncAction F() {\n"
            "    co_await RequestAccessAsync(kind);\n"
            "    auto status = co_await OtherAsync();\n"
            "    status.get();\n"
            "}\n"
        )
        self.assertEqual(check.blocking_waits(text), [])

    def test_std_async_and_unrelated_get_calls_are_not_winrt_waits(self) -> None:
        text = (
            "void F() {\n"
            "    auto future = std::async(std::launch::async, Work);\n"
            "    future.get();\n"
            "    std::async(Work).get();\n"
            "    owner.get();\n"
            "    ActivateAudioInterfaceAsync(path, iid, nullptr, handler, &operation);\n"
            "    operation.get();\n"
            "}\n"
        )
        self.assertEqual(check.blocking_waits(text), [])

    def test_comments_and_strings_are_ignored(self) -> None:
        text = (
            "// RequestAccessAsync(kind).get() froze the engine thread\n"
            "/* auto op = FooAsync(); op.get(); */\n"
            'const char* kWhy = "RequestAccessAsync(kind).get()";\n'
        )
        self.assertEqual(check.blocking_waits(text), [])


class TreeEvaluationTest(unittest.TestCase):
    def setUp(self) -> None:
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)

    def write(self, name: str, text: str) -> None:
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def test_an_unlisted_wait_fails_with_its_location(self) -> None:
        self.write("runtime/core/engine_screenshot.cpp", FROZE_THE_ENGINE_THREAD)
        problems = check.evaluate(self.root, {})
        self.assertEqual(len(problems), 1)
        self.assertTrue(problems[0].startswith("runtime/core/engine_screenshot.cpp:6: blocking wait"))

    def test_a_listed_mta_owned_file_passes(self) -> None:
        self.write("captureengine/media/border.cpp", FROZE_THE_ENGINE_THREAD)
        self.assertEqual(check.evaluate(self.root, {"captureengine/media/border.cpp": "own MTA thread"}), [])

    def test_a_listed_file_without_waits_must_leave_the_list(self) -> None:
        self.write("captureengine/media/border.cpp", "void F() {}\n")
        problems = check.evaluate(self.root, {"captureengine/media/border.cpp": "own MTA thread"})
        self.assertEqual(len(problems), 1)
        self.assertIn("remove the entry", problems[0])

    def test_tests_and_vendored_trees_are_outside_the_product_roots(self) -> None:
        self.write("tests/test_border.cpp", FROZE_THE_ENGINE_THREAD)
        self.write("build/msys64/include/winrt/base.h", FROZE_THE_ENGINE_THREAD)
        self.assertEqual(check.evaluate(self.root, {}), [])

    def test_the_command_line_fails_on_a_wait(self) -> None:
        self.write("common/platform/startup.cpp", FROZE_THE_ENGINE_THREAD)
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.assertEqual(check.main(["--root", str(self.root)]), 1)
        self.assertIn("common/platform/startup.cpp:6:", output.getvalue())


class GateRegistrationTest(unittest.TestCase):
    def test_the_closing_gate_runs_the_policy_and_the_live_tree_check(self) -> None:
        commands = dict(_commands("project-root", "project-python"))
        self.assertIn("tools.tests.test_winrt_blocking_waits", commands["winrt_blocking_wait_policy"])
        self.assertTrue(commands["winrt_blocking_wait_tree"][-1].endswith("check_winrt_blocking_waits.py"))

    def test_the_live_tree_passes(self) -> None:
        self.assertEqual(check.evaluate(Path(check.__file__).resolve().parents[1]), [])


if __name__ == "__main__":
    unittest.main()
