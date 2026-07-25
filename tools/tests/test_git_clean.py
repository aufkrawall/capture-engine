"""Regression tests for tools/git-clean.py path anchoring.

git-clean.py deletes every gitignored file, so where it believes the repository
root is matters more than for an ordinary script. The module lives in tools/ but
must operate on the repository root one level up. When it moved out of the root
its `Path(__file__).resolve().parent` anchor silently started pointing at
tools/; the ensure_repo_root() guard caught it, but only at run time.

The filename uses a hyphen, so it is loaded by path rather than imported.
"""

import importlib.util
import subprocess
import sys
import unittest
from pathlib import Path
from typing import Any

MODULE_PATH = Path(__file__).resolve().parents[1] / "git-clean.py"
SPEC = importlib.util.spec_from_file_location("git_clean", MODULE_PATH)
assert SPEC and SPEC.loader
# Annotated Any because the module is loaded by path: a static checker cannot see
# its attributes, and one test reassigns PROJECT_ROOT to exercise the guard.
git_clean: Any = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = git_clean
SPEC.loader.exec_module(git_clean)

REPO_ROOT = Path(__file__).resolve().parents[2]


class GitCleanPathAnchorTest(unittest.TestCase):
    def test_project_root_is_the_repository_root_not_the_tools_directory(self) -> None:
        self.assertEqual(git_clean.PROJECT_ROOT.resolve(), REPO_ROOT)
        self.assertNotEqual(git_clean.PROJECT_ROOT.name, "tools")

    def test_project_root_contains_the_repository_marker_files(self) -> None:
        # Cheap proof the anchor points at a checkout rather than a subdirectory.
        self.assertTrue((git_clean.PROJECT_ROOT / "build.py").is_file())
        self.assertTrue((git_clean.PROJECT_ROOT / ".gitignore").is_file())
        self.assertTrue((git_clean.PROJECT_ROOT / ".git").exists())

    def test_log_path_stays_at_the_repository_root(self) -> None:
        # .gitignore ignores git-clean.log at the root; a tools/git-clean.log
        # would be an untracked stray instead.
        self.assertEqual(git_clean.LOG_PATH.resolve(), REPO_ROOT / "git-clean.log")

    def test_repo_root_guard_agrees_with_git(self) -> None:
        # Asserted via --show-prefix, which is relative to the repository root
        # and therefore identical under Windows git and MSYS2 git. Comparing
        # --show-toplevel output would fail under an MSYS2 git on PATH, which is
        # exactly the environment build.py runs the self-tests in.
        result = subprocess.run(
            ["git", "rev-parse", "--show-prefix"],
            cwd=git_clean.PROJECT_ROOT,
            capture_output=True,
            text=True,
            check=True,
        )
        self.assertEqual(result.stdout.strip(), "")
        # The guard raises when the prefix is non-empty, so it must pass here.
        git_clean.ensure_repo_root()

    def test_repo_root_guard_rejects_a_subdirectory(self) -> None:
        # The failure this guard exists for: pointed one level down, it must
        # refuse rather than clean the wrong tree.
        original = git_clean.PROJECT_ROOT
        git_clean.PROJECT_ROOT = original / "tools"
        try:
            with self.assertRaises(RuntimeError):
                git_clean.ensure_repo_root()
        finally:
            git_clean.PROJECT_ROOT = original


if __name__ == "__main__":
    unittest.main()
