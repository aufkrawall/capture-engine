"""Regression tests for the source file-size ratchet in build.py.

`AGENTS.md` keeps source files at roughly 600-800 lines. That rule was
documentation-only for a long time and the tree drifted far past it, so the
ratchet records the existing violations and makes growth fatal. The behaviour
worth pinning is the ratchet direction: a recorded file may shrink but never
grow, a previously compliant file may not cross the ceiling, and a file that
drops back under it is removed so the space cannot be silently reclaimed.
"""

import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import build


class FileSizeBaselineTest(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.baseline_path = self.root / "tools" / "file_size_baseline.json"
        self.messages: list = []

        for patcher in (
            patch.object(build, "PROJECT_ROOT", str(self.root)),
            patch.object(build, "FILE_SIZE_BASELINE_PATH", str(self.baseline_path)),
            patch.object(build, "log", lambda message, **_: self.messages.append(message)),
            patch.object(build, "record_verification_step", lambda *args, **kwargs: None),
        ):
            patcher.start()
            self.addCleanup(patcher.stop)

    # --- helpers -----------------------------------------------------------------

    def write_source(self, relative: str, lines: int, *, trailing_newline: bool = True) -> Path:
        source = self.root / relative
        source.parent.mkdir(parents=True, exist_ok=True)
        text = "\n".join(f"// line {index}" for index in range(lines))
        source.write_text(text + ("\n" if trailing_newline else ""), encoding="utf-8")
        return source

    def evaluate(self, sizes: dict) -> dict:
        details: dict = {}
        build.evaluate_file_size_baseline(sizes, details)
        return details

    def read_baseline(self) -> dict:
        return json.loads(self.baseline_path.read_text(encoding="utf-8"))

    def logged(self, needle: str) -> bool:
        return any(needle in message for message in self.messages)

    # --- measurement -------------------------------------------------------------

    def test_line_count_matches_a_trailing_newline_file(self) -> None:
        source = self.write_source("hook/big.cpp", 900)
        self.assertEqual(build.count_source_lines(str(source)), 900)

    def test_line_count_counts_a_final_unterminated_line(self) -> None:
        source = self.write_source("hook/big.cpp", 900, trailing_newline=False)
        self.assertEqual(build.count_source_lines(str(source)), 900)

    def test_collection_covers_cpp_and_python_but_skips_vendored_trees(self) -> None:
        self.write_source("hook/apis/dx12_hook.cpp", 10)
        self.write_source("common/shared_defs.h", 10)
        self.write_source("tools/analyze_capture_av.py", 10)
        self.write_source("testapp/run_tests.py", 10)
        self.write_source("build.py", 10)
        self.write_source("hook/external/vendor.cpp", 10)
        self.write_source("hook/common/imgui/imgui_draw.cpp", 10)
        self.write_source("hook/notes.md", 10)

        measured = set(build.collect_source_file_sizes())
        self.assertEqual(
            measured,
            {
                "hook/apis/dx12_hook.cpp",
                "common/shared_defs.h",
                "tools/analyze_capture_av.py",
                "testapp/run_tests.py",
                "build.py",
            },
        )

    def test_collection_covers_inl_files(self) -> None:
        # The test apps split a single translation unit across .inl files, so
        # leaving them out would be an easy way past the ceiling.
        self.write_source("testapp/dx12_fg_switch_streamline.inl", 10)
        self.assertIn("testapp/dx12_fg_switch_streamline.inl", build.collect_source_file_sizes())

    def test_collection_covers_wiki_markdown_at_any_depth(self) -> None:
        # `llm-wiki/log/recent.md` is rolling memory that the wiki's own
        # convention archives at ~230 lines. Nothing enforced that, so it reached
        # 6212 lines and dominated the repository's history size. The ratchet now
        # governs the wiki, including nested pages.
        self.write_source("llm-wiki/index.md", 10)
        self.write_source("llm-wiki/log/recent.md", 10)
        self.write_source("llm-wiki/frame-generation/guardrails.md", 10)

        measured = build.collect_source_file_sizes()
        self.assertIn("llm-wiki/index.md", measured)
        self.assertIn("llm-wiki/log/recent.md", measured)
        self.assertIn("llm-wiki/frame-generation/guardrails.md", measured)

    def test_markdown_outside_the_wiki_stays_unmeasured(self) -> None:
        # READMEs and design notes next to code are not the wiki's rolling log
        # and are deliberately left out, so the scope stays predictable.
        self.write_source("README.md", 10)
        self.write_source("patches/ffmpeg/README.md", 10)

        measured = build.collect_source_file_sizes()
        self.assertNotIn("README.md", measured)
        self.assertNotIn("patches/ffmpeg/README.md", measured)

    def test_an_unrotated_wiki_log_fails_the_ceiling(self) -> None:
        # The exact regression that went unnoticed: recent.md growing without
        # being archived. Would have failed on the first lint run past 800 lines.
        self.write_source("llm-wiki/log/recent.md", 6212)
        self.evaluate({"hook/apis/dx12_hook.cpp": 900})  # seed a baseline
        with self.assertRaises(SystemExit):
            self.evaluate(build.collect_source_file_sizes())
        self.assertTrue(self.logged("llm-wiki/log/recent.md"))

    # --- ratchet behaviour -------------------------------------------------------

    def test_first_run_records_only_files_over_the_ceiling(self) -> None:
        details = self.evaluate({"hook/big.cpp": 1200, "hook/small.cpp": 400})
        self.assertEqual(details["file_size_baseline"], "written")
        self.assertEqual(self.read_baseline()["files"], {"hook/big.cpp": 1200})
        self.assertEqual(self.read_baseline()["limit"], build.FILE_SIZE_LIMIT)

    def test_recorded_file_may_stay_the_same_size(self) -> None:
        self.evaluate({"hook/big.cpp": 1200})
        details = self.evaluate({"hook/big.cpp": 1200})
        self.assertEqual(details["file_size_baseline"], "unchanged")

    def test_growing_a_recorded_file_fails(self) -> None:
        self.evaluate({"hook/big.cpp": 1200})
        with self.assertRaises(SystemExit) as raised:
            self.evaluate({"hook/big.cpp": 1201})
        self.assertEqual(raised.exception.code, 1)
        self.assertTrue(self.logged("1201 > 1200 accepted lines"))

    def test_a_new_file_crossing_the_ceiling_fails(self) -> None:
        self.evaluate({"hook/big.cpp": 1200})
        with self.assertRaises(SystemExit) as raised:
            self.evaluate({"hook/big.cpp": 1200, "hook/fresh.cpp": build.FILE_SIZE_LIMIT + 1})
        self.assertEqual(raised.exception.code, 1)
        self.assertTrue(self.logged("new violation"))

    def test_a_new_file_at_the_ceiling_is_allowed(self) -> None:
        self.evaluate({"hook/big.cpp": 1200})
        details = self.evaluate({"hook/big.cpp": 1200, "hook/fresh.cpp": build.FILE_SIZE_LIMIT})
        self.assertEqual(details["file_size_baseline"], "unchanged")

    def test_shrinking_tightens_the_baseline_immediately(self) -> None:
        self.evaluate({"hook/big.cpp": 1200})
        details = self.evaluate({"hook/big.cpp": 900})
        self.assertEqual(details["file_size_baseline"], "tightened")
        self.assertEqual(self.read_baseline()["files"], {"hook/big.cpp": 900})
        # The reclaimed space must not be available again.
        with self.assertRaises(SystemExit):
            self.evaluate({"hook/big.cpp": 901})

    def test_a_file_split_under_the_ceiling_is_dropped_from_the_baseline(self) -> None:
        self.evaluate({"hook/big.cpp": 1200})
        details = self.evaluate({"hook/big.cpp": 700, "hook/big_part2.cpp": 500})
        self.assertEqual(details["file_size_baseline"], "tightened")
        self.assertEqual(details["file_size_resolved"], ["hook/big.cpp"])
        self.assertEqual(self.read_baseline()["files"], {})

    def test_a_deleted_file_is_dropped_from_the_baseline(self) -> None:
        self.evaluate({"hook/big.cpp": 1200, "hook/other.cpp": 1000})
        details = self.evaluate({"hook/other.cpp": 1000})
        self.assertEqual(details["file_size_baseline"], "tightened")
        self.assertEqual(self.read_baseline()["files"], {"hook/other.cpp": 1000})

    def test_update_flag_rewrites_the_baseline_from_the_current_tree(self) -> None:
        self.evaluate({"hook/big.cpp": 1200})
        with patch.object(build.sys, "argv", ["build.py", "--lint", "--update-lint-baseline"]):
            details = self.evaluate({"hook/big.cpp": 5000})
        self.assertEqual(details["file_size_baseline"], "written")
        self.assertEqual(self.read_baseline()["files"], {"hook/big.cpp": 5000})

    def test_baseline_records_totals_for_progress_tracking(self) -> None:
        self.evaluate({"hook/big.cpp": 1200, "hook/other.cpp": 1000, "hook/small.cpp": 10})
        baseline = self.read_baseline()
        self.assertEqual(baseline["count"], 2)
        self.assertEqual(baseline["total"], 2200)

    def test_unreadable_baseline_is_fatal_rather_than_silently_ignored(self) -> None:
        self.baseline_path.parent.mkdir(parents=True, exist_ok=True)
        self.baseline_path.write_text("{not json", encoding="utf-8")
        with self.assertRaises(SystemExit) as raised:
            self.evaluate({"hook/big.cpp": 1200})
        self.assertEqual(raised.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
