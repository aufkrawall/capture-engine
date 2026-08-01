"""Regression tests for the scope-aware clang-tidy baseline ratchet in build.py.

A `--tests-only` build regenerates `compile_commands.json` with only the test and
hook/common translation units. A lint run against that partial database sees far
fewer warnings for reasons that have nothing to do with the code, and before the
scope record existed it folded those lower counts into the baseline; the next full
run then failed with ~21 phantom "regressions" and needed a manual git checkout.
"""

# build.py executes its fragments via exec, so its module attributes exist only
# at runtime; pyright cannot see them through the facade.
# pyright: reportAttributeAccessIssue=false

import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import build

PRODUCT_SOURCES = ("captureengine/capture.cpp", "mediaengine/encoder.cpp", "common/config.cpp")
TEST_SOURCES = ("tests/test_config.cpp", "hook/common/lifecycle.cpp")


class ClangTidyBaselineScopeTest(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.baseline_path = self.root / "tools" / "clang_tidy_baseline.json"
        self.messages: list = []

        for patcher in (
            patch.object(build, "PROJECT_ROOT", str(self.root)),
            patch.object(build, "CLANG_TIDY_BASELINE_PATH", str(self.baseline_path)),
            patch.object(build, "log", lambda message, **_: self.messages.append(message)),
        ):
            patcher.start()
            self.addCleanup(patcher.stop)

    # --- helpers -----------------------------------------------------------------

    def create_sources(self, *relative_paths: str) -> None:
        for relative in relative_paths:
            source = self.root / relative
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_text("int main() { return 0; }\n", encoding="utf-8")

    def scope_for(self, *relative_paths: str) -> dict:
        """Scope exactly as run_lint derives it from a compilation database."""
        database = [
            {"directory": str(self.root), "file": str(self.root / relative), "arguments": ["clang++", relative]}
            for relative in relative_paths
        ]
        return build.clang_tidy_scope_from_entries(database)

    def write_baseline(self, checks: dict, scope_units) -> None:
        build.write_clang_tidy_baseline(checks, {"translation_units": list(scope_units)} if scope_units else None)

    def read_baseline(self) -> dict:
        return json.loads(self.baseline_path.read_text(encoding="utf-8"))

    def logged(self, needle: str) -> bool:
        return any(needle in message for message in self.messages)

    # --- scope derivation --------------------------------------------------------

    def test_scope_is_the_deduplicated_project_relative_translation_unit_set(self) -> None:
        database = [
            {"file": str(self.root / "captureengine" / "capture.cpp")},
            {"file": str(self.root / "captureengine" / "capture.cpp")},
            {"file": str(self.root) + "\\tests\\test_config.cpp"},
            {"directory": str(self.root)},
        ]
        self.assertEqual(
            build.clang_tidy_scope_from_entries(database),
            {"entries": 2, "translation_units": ["captureengine/capture.cpp", "tests/test_config.cpp"]},
        )

    def test_scope_keys_match_across_hosts_for_a_windows_written_database(self) -> None:
        """The baseline is committed, so a key must not depend on the reading host.

        Backslashes are separators in a Windows-written compilation database but
        ordinary filename characters on Linux, which used to leave the whole
        absolute path as the key there and silently break scope-gap detection.
        """
        windows_style = str(self.root) + "\\captureengine\\capture.cpp"
        posix_style = str(self.root / "captureengine" / "capture.cpp")
        self.assertEqual(build.clang_tidy_scope_path(windows_style), "captureengine/capture.cpp")
        self.assertEqual(build.clang_tidy_scope_path(windows_style), build.clang_tidy_scope_path(posix_style))
        self.assertEqual(
            build.clang_tidy_scope_from_entries([{"file": windows_style}, {"file": posix_style}]),
            {"entries": 1, "translation_units": ["captureengine/capture.cpp"]},
        )

    def test_scope_gap_reports_baseline_units_the_run_did_not_lint(self) -> None:
        self.create_sources(*PRODUCT_SOURCES, *TEST_SOURCES)
        baseline_scope = self.scope_for(*PRODUCT_SOURCES, *TEST_SOURCES)
        reduced_scope = self.scope_for(*TEST_SOURCES)
        self.assertEqual(build.clang_tidy_scope_gap(baseline_scope, reduced_scope), sorted(PRODUCT_SOURCES))
        self.assertEqual(build.clang_tidy_scope_gap(baseline_scope, baseline_scope), [])
        self.assertIsNone(build.clang_tidy_scope_gap(baseline_scope, None))
        self.assertIsNone(build.clang_tidy_scope_gap(None, baseline_scope))

    # --- the reduced-scope corruption --------------------------------------------

    def test_reduced_scope_run_does_not_fold_lower_counts_into_the_baseline(self) -> None:
        self.create_sources(*PRODUCT_SOURCES, *TEST_SOURCES)
        accepted = {"bugprone-narrowing-conversions": 333, "bugprone-branch-clone": 26}
        self.write_baseline(accepted, PRODUCT_SOURCES + TEST_SOURCES)
        before = self.baseline_path.read_text(encoding="utf-8")

        details: dict = {}
        build.evaluate_clang_tidy_baseline(
            {"bugprone-narrowing-conversions": 122, "bugprone-branch-clone": 9}, details, self.scope_for(*TEST_SOURCES)
        )

        self.assertEqual(self.baseline_path.read_text(encoding="utf-8"), before)
        self.assertEqual(details["clang_tidy_scope"], "reduced")
        self.assertEqual(details["clang_tidy_scope_unlinted"], len(PRODUCT_SOURCES))
        self.assertEqual(details["clang_tidy_baseline"], "tightening_skipped")
        skipped = details["clang_tidy_baseline_tightening_skipped"]
        self.assertEqual(skipped["bugprone-narrowing-conversions"], {"was": 333, "now": 122})
        self.assertTrue(self.logged("clang-tidy lint scope reduced"))
        self.assertTrue(self.logged("left unchanged (reduced lint scope)"))

    def test_full_scope_run_still_folds_lower_counts_in(self) -> None:
        self.create_sources(*PRODUCT_SOURCES, *TEST_SOURCES)
        self.write_baseline({"bugprone-narrowing-conversions": 333}, PRODUCT_SOURCES + TEST_SOURCES)

        details: dict = {}
        build.evaluate_clang_tidy_baseline(
            {"bugprone-narrowing-conversions": 122}, details, self.scope_for(*PRODUCT_SOURCES, *TEST_SOURCES)
        )

        self.assertEqual(self.read_baseline()["checks"]["bugprone-narrowing-conversions"], 122)
        self.assertEqual(details["clang_tidy_scope"], "full")
        self.assertEqual(details["clang_tidy_baseline"], "tightened")

    def test_deleted_sources_do_not_freeze_the_ratchet(self) -> None:
        self.create_sources(*PRODUCT_SOURCES)
        self.write_baseline({"bugprone-branch-clone": 26}, PRODUCT_SOURCES + ("captureengine/removed.cpp",))

        details: dict = {}
        build.evaluate_clang_tidy_baseline({"bugprone-branch-clone": 9}, details, self.scope_for(*PRODUCT_SOURCES))

        self.assertEqual(self.read_baseline()["checks"]["bugprone-branch-clone"], 9)
        self.assertEqual(details["clang_tidy_baseline"], "tightened")
        self.assertNotIn("captureengine/removed.cpp", self.read_baseline()["scope"]["translation_units"])

    def test_added_sources_refresh_the_recorded_scope(self) -> None:
        self.create_sources(*PRODUCT_SOURCES, *TEST_SOURCES)
        self.write_baseline({"bugprone-branch-clone": 9}, PRODUCT_SOURCES)

        details: dict = {}
        build.evaluate_clang_tidy_baseline(
            {"bugprone-branch-clone": 9}, details, self.scope_for(*PRODUCT_SOURCES, *TEST_SOURCES)
        )

        self.assertEqual(self.read_baseline()["scope"]["entries"], len(PRODUCT_SOURCES) + len(TEST_SOURCES))
        self.assertEqual(details["clang_tidy_baseline"], "scope_refreshed")

    # --- increases stay fatal regardless of scope --------------------------------

    def test_reduced_scope_run_still_fails_on_an_increase(self) -> None:
        self.create_sources(*PRODUCT_SOURCES, *TEST_SOURCES)
        self.write_baseline({"bugprone-branch-clone": 9}, PRODUCT_SOURCES + TEST_SOURCES)
        before = self.baseline_path.read_text(encoding="utf-8")

        details: dict = {}
        with self.assertRaises(SystemExit) as failure:
            build.evaluate_clang_tidy_baseline({"bugprone-branch-clone": 10}, details, self.scope_for(*TEST_SOURCES))

        self.assertEqual(failure.exception.code, 1)
        self.assertEqual(self.baseline_path.read_text(encoding="utf-8"), before)
        self.assertEqual(details["clang_tidy_baseline"], "regressed")
        self.assertTrue(self.logged("This run linted a subset of the baseline scope"))

    def test_reduced_scope_run_still_fails_on_a_previously_unseen_check(self) -> None:
        self.create_sources(*PRODUCT_SOURCES, *TEST_SOURCES)
        self.write_baseline({"bugprone-branch-clone": 9}, PRODUCT_SOURCES + TEST_SOURCES)

        details: dict = {}
        with self.assertRaises(SystemExit) as failure:
            build.evaluate_clang_tidy_baseline(
                {"bugprone-branch-clone": 9, "bugprone-use-after-move": 1}, details, self.scope_for(*TEST_SOURCES)
            )

        self.assertEqual(failure.exception.code, 1)
        regressions = details["clang_tidy_baseline_regressions"]
        self.assertIn("bugprone-use-after-move: new check with 1 warning(s)", regressions)

    # --- explicit baseline updates -----------------------------------------------

    def test_update_lint_baseline_refuses_a_partial_compilation_database(self) -> None:
        self.create_sources(*PRODUCT_SOURCES, *TEST_SOURCES)
        self.write_baseline({"bugprone-narrowing-conversions": 333}, PRODUCT_SOURCES + TEST_SOURCES)
        before = self.baseline_path.read_text(encoding="utf-8")

        details: dict = {}
        with patch.object(sys, "argv", ["build.py", "--no-build", "--lint", "--update-lint-baseline"]):
            with self.assertRaises(SystemExit) as failure:
                build.evaluate_clang_tidy_baseline(
                    {"bugprone-narrowing-conversions": 122}, details, self.scope_for(*TEST_SOURCES)
                )

        self.assertEqual(failure.exception.code, 2)
        self.assertEqual(self.baseline_path.read_text(encoding="utf-8"), before)
        self.assertEqual(details["clang_tidy_baseline"], "update_refused_reduced_scope")
        self.assertTrue(self.logged("refusing to rewrite the clang-tidy baseline from a partial compilation database"))

    def test_update_lint_baseline_records_the_full_scope(self) -> None:
        self.create_sources(*PRODUCT_SOURCES, *TEST_SOURCES)
        self.write_baseline({"bugprone-narrowing-conversions": 122}, PRODUCT_SOURCES)

        details: dict = {}
        with patch.object(sys, "argv", ["build.py", "--lint", "--update-lint-baseline"]):
            build.evaluate_clang_tidy_baseline(
                {"bugprone-narrowing-conversions": 333}, details, self.scope_for(*PRODUCT_SOURCES, *TEST_SOURCES)
            )

        written = self.read_baseline()
        self.assertEqual(written["checks"]["bugprone-narrowing-conversions"], 333)
        self.assertEqual(written["scope"]["translation_units"], sorted(PRODUCT_SOURCES + TEST_SOURCES))
        self.assertEqual(details["clang_tidy_baseline"], "written")

    # --- unknown scope is as conservative as a reduced one ------------------------

    def test_baseline_without_a_recorded_scope_is_not_tightened(self) -> None:
        self.create_sources(*PRODUCT_SOURCES)
        self.write_baseline({"bugprone-branch-clone": 26}, None)
        self.assertNotIn("scope", self.read_baseline())
        before = self.baseline_path.read_text(encoding="utf-8")

        details: dict = {}
        build.evaluate_clang_tidy_baseline({"bugprone-branch-clone": 9}, details, self.scope_for(*PRODUCT_SOURCES))

        self.assertEqual(self.baseline_path.read_text(encoding="utf-8"), before)
        self.assertEqual(details["clang_tidy_scope"], "unknown")
        self.assertEqual(details["clang_tidy_baseline"], "tightening_skipped")
        self.assertTrue(self.logged("left unchanged (unknown lint scope)"))

    def test_unreadable_compilation_database_is_not_tightened(self) -> None:
        self.create_sources(*PRODUCT_SOURCES)
        self.write_baseline({"bugprone-branch-clone": 26}, PRODUCT_SOURCES)
        before = self.baseline_path.read_text(encoding="utf-8")

        details: dict = {}
        build.evaluate_clang_tidy_baseline({"bugprone-branch-clone": 9}, details, None)

        self.assertEqual(self.baseline_path.read_text(encoding="utf-8"), before)
        self.assertEqual(details["clang_tidy_scope"], "unknown")
        self.assertEqual(details["clang_tidy_baseline"], "tightening_skipped")

    def test_missing_baseline_is_created_with_the_current_scope(self) -> None:
        self.create_sources(*PRODUCT_SOURCES)

        details: dict = {}
        build.evaluate_clang_tidy_baseline({"bugprone-branch-clone": 9}, details, self.scope_for(*PRODUCT_SOURCES))

        self.assertEqual(self.read_baseline()["scope"]["translation_units"], sorted(PRODUCT_SOURCES))
        self.assertEqual(details["clang_tidy_baseline"], "written")


class CommittedClangTidyBaselineTest(unittest.TestCase):
    """The checked-in baseline must carry a full-scope record, not a partial one."""

    def test_committed_baseline_records_a_consistent_full_lint_scope(self) -> None:
        baseline = build.load_clang_tidy_baseline()
        self.assertIsNotNone(baseline)
        assert baseline is not None
        scope = baseline["scope"]
        self.assertIsNotNone(scope, "tools/clang_tidy_baseline.json must record the lint scope of its counts")
        assert scope is not None

        units = scope["translation_units"]
        self.assertEqual(scope["entries"], len(set(units)))
        self.assertEqual(units, sorted(units))
        for unit in units:
            self.assertNotIn("\\", unit)
            self.assertFalse(unit.startswith(("/", "..")), unit)

        # Same directories run_lint feeds to clang-format; a tests-only database
        # covers only tests/ and hook/common, which is how the baseline got
        # corrupted before the scope record existed.
        for directory in ("common", "hook", "captureengine", "mediaengine", "testapp", "tests"):
            self.assertTrue(
                any(unit.startswith(f"{directory}/") for unit in units),
                f"baseline scope has no {directory}/ translation unit; it was recorded from a partial database",
            )


if __name__ == "__main__":
    unittest.main()
