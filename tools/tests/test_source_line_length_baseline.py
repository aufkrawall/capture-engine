"""Regression tests for the source line-length ratchet in build.py.

The file-size ratchet measures *lines*, so a tool that packs several declarations onto one
line satisfies the 800-line ceiling while making the file harder to read. That is exactly
what happened here: ddraw_hook_internal.h carried a 541-character line holding eight separate
declarations, and wgc_capture_internal.h still carries one of 2053. This ratchet closes that
gap, and the behaviour worth pinning is its direction - a recorded file may shorten but never
grow, a compliant file may not cross the limit, and a file brought back under it is dropped
so the slack cannot be silently reclaimed.
"""

# build.py executes its fragments via exec, so its module attributes exist only
# at runtime; pyright cannot see them through the facade.
# pyright: reportAttributeAccessIssue=false

import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import build


class SourceLineLengthBaselineTest(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.baseline_path = self.root / "tools" / "source_line_length_baseline.json"
        self.messages: list = []

        for patcher in (
            patch.object(build, "PROJECT_ROOT", str(self.root)),
            patch.object(build, "SOURCE_LINE_LENGTH_BASELINE_PATH", str(self.baseline_path)),
            patch.object(build, "log", lambda message, **_: self.messages.append(message)),
            patch.object(build, "record_verification_step", lambda *args, **kwargs: None),
        ):
            patcher.start()
            self.addCleanup(patcher.stop)

    # --- helpers -----------------------------------------------------------------

    def write_source(self, relative: str, longest: int) -> Path:
        source = self.root / relative
        source.parent.mkdir(parents=True, exist_ok=True)
        body = "// short line\n" + ("/" * longest) + "\n"
        source.write_text(body, encoding="utf-8")
        return source

    def evaluate(self) -> dict:
        details: dict = {}
        build.evaluate_source_line_length(details)
        return details

    def read_baseline(self) -> dict:
        return json.loads(self.baseline_path.read_text(encoding="utf-8"))

    def logged(self, needle: str) -> bool:
        return any(needle in message for message in self.messages)

    # --- measurement -------------------------------------------------------------

    def test_measurement_reports_the_longest_line_only_for_files_over_the_limit(self) -> None:
        self.write_source("hook/packed.h", build.SOURCE_LINE_LENGTH_LIMIT + 41)
        self.write_source("hook/tidy.cpp", build.SOURCE_LINE_LENGTH_LIMIT - 1)
        overlong = build.collect_overlong_source_lines()
        self.assertEqual(overlong, {"hook/packed.h": build.SOURCE_LINE_LENGTH_LIMIT + 41})

    def test_a_line_exactly_at_the_limit_is_allowed(self) -> None:
        self.write_source("hook/edge.cpp", build.SOURCE_LINE_LENGTH_LIMIT)
        self.assertEqual(build.collect_overlong_source_lines(), {})

    def test_vendored_trees_are_not_measured(self) -> None:
        self.write_source("hook/external/vendor.h", build.SOURCE_LINE_LENGTH_LIMIT + 500)
        self.assertEqual(build.collect_overlong_source_lines(), {})

    # --- ratchet direction --------------------------------------------------------

    def test_first_run_records_only_files_over_the_limit(self) -> None:
        self.write_source("hook/packed.h", 400)
        self.write_source("hook/tidy.cpp", 80)
        self.evaluate()
        self.assertEqual(self.read_baseline()["files"], {"hook/packed.h": 400})

    def test_a_recorded_file_may_stay_the_same(self) -> None:
        self.write_source("hook/packed.h", 400)
        self.evaluate()
        details = self.evaluate()
        self.assertEqual(details["source_line_length_baseline"], "unchanged")

    def test_growing_a_recorded_line_fails(self) -> None:
        self.write_source("hook/packed.h", 400)
        self.evaluate()
        self.write_source("hook/packed.h", 401)
        with self.assertRaises(SystemExit) as raised:
            self.evaluate()
        self.assertEqual(raised.exception.code, 1)
        self.assertTrue(self.logged("longest line grew 400 -> 401"))

    def test_a_new_file_crossing_the_limit_fails(self) -> None:
        self.write_source("hook/packed.h", 400)
        self.evaluate()
        self.write_source("hook/fresh.cpp", build.SOURCE_LINE_LENGTH_LIMIT + 1)
        with self.assertRaises(SystemExit) as raised:
            self.evaluate()
        self.assertEqual(raised.exception.code, 1)
        self.assertTrue(self.logged("new violation"))

    def test_shortening_tightens_the_baseline_immediately(self) -> None:
        self.write_source("hook/packed.h", 400)
        self.evaluate()
        self.write_source("hook/packed.h", 300)
        details = self.evaluate()
        self.assertEqual(details["source_line_length_baseline"], "tightened")
        self.assertEqual(self.read_baseline()["files"], {"hook/packed.h": 300})

    def test_a_file_brought_under_the_limit_is_dropped(self) -> None:
        self.write_source("hook/packed.h", 400)
        self.evaluate()
        self.write_source("hook/packed.h", 100)
        self.evaluate()
        self.assertEqual(self.read_baseline()["files"], {})

    def test_a_deleted_file_is_dropped_from_the_baseline(self) -> None:
        source = self.write_source("hook/packed.h", 400)
        self.evaluate()
        source.unlink()
        self.evaluate()
        self.assertEqual(self.read_baseline()["files"], {})

    def test_preflight_never_writes_the_baseline(self) -> None:
        # The preflight runs before the build and must only report, so a failing run cannot
        # quietly record its own regression as the new accepted state.
        self.write_source("hook/packed.h", 400)
        self.evaluate()
        self.write_source("hook/packed.h", 300)
        details: dict = {}
        build.evaluate_source_line_length(details, mutate_baseline=False)
        self.assertEqual(self.read_baseline()["files"], {"hook/packed.h": 400})

    def test_unreadable_baseline_is_fatal_rather_than_silently_ignored(self) -> None:
        self.baseline_path.parent.mkdir(parents=True, exist_ok=True)
        self.baseline_path.write_text("{not json", encoding="utf-8")
        with self.assertRaises(SystemExit) as raised:
            build.load_source_line_length_baseline()
        self.assertEqual(raised.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
