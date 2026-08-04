"""Privacy regression tests: tracked files must not contain developer paths.

This repository is intended to be public. Its history was scrubbed (twice) of the
maintainer's real Windows profile path: every `C:\\Users\\<developer>\\...` literal was
replaced with the `C:\\Users\\TestUser\\...` placeholder. On 2026-08-04 a literal
developer-profile example slipped back into `.github/workflows/release-stable.yml`
(present in HEAD and five commits, including the `v0.1.5268` tag tree) and was
scrubbed from history again; see `llm-wiki/log/recent.md`. These tests fail closed
whenever a tracked working-tree file contains a Windows user-profile path whose user
component is not an approved placeholder, so the same mistake cannot reach a release
again: `release-stable.yml` runs this module in its "Run build policy tests" step, and
build.py runs it as the `privacy_paths` Python tool self-test inside the `--verify` gate.

Only the tracked working tree is scanned, not git history: a leak that reaches
origin/main is by definition present in the tree, and the release workflow checks out
origin/main before building, so the tree scan is the fail-closed boundary for the
release path. History was verified separately with a full-object scan during the
2026-08-04 scrub.
"""

from __future__ import annotations

import re
import subprocess
import unittest
from pathlib import Path
from typing import List, Tuple

REPO_ROOT = Path(__file__).resolve().parents[2]

# User components that may legitimately appear in tracked files. Anything else under
# C:\Users\ (or the MSYS/Cygwin PUA-colon spelling) is treated as a
# developer-identifying path and fails the gate.
ALLOWED_USER_COMPONENTS = frozenset(
    {
        "TestUser",
        "dev",
        "Public",
        "Default",
        "<developer>",
        "<name>",
        "<user>",
        "<username>",
        "username",
        "User",
    }
)

# C:\Users\<name>\... (any drive letter, backslash spelling).
USER_PATH_RE = re.compile(r"[A-Za-z]:\\Users\\([^\\\r\n\s\"'`]+)")
# MSYS/Cygwin private-use-area colon spelling: C<U+F03A/U+FF1A>Users<name>.
PUA_USER_RE = re.compile(r"[A-Za-z][\uf03a\uff1a]Users([A-Za-z0-9_]+)")

RELEASE_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "release-stable.yml"


def _tracked_files() -> List[Path]:
    result = subprocess.run(
        ["git", "ls-files", "-z"],
        cwd=REPO_ROOT,
        capture_output=True,
        check=True,
    )
    return [
        REPO_ROOT / path
        for path in result.stdout.decode("utf-8", errors="replace").split("\0")
        if path
    ]


def _user_path_hits(path: Path) -> List[Tuple[int, str]]:
    raw = path.read_bytes()
    if b"\x00" in raw:
        return []  # Binary; the path spellings below cannot match meaningfully.
    text = raw.decode("utf-8", errors="replace")
    try:
        relative = path.relative_to(REPO_ROOT)
    except ValueError:
        relative = path  # Only repo files are scanned in practice.
    hits: List[Tuple[int, str]] = []
    for lineno, line in enumerate(text.splitlines(), start=1):
        for match in USER_PATH_RE.finditer(line):
            user = match.group(1)
            if user not in ALLOWED_USER_COMPONENTS:
                hits.append((lineno, f"literal Windows user path C:\\Users\\{user}\\ in {relative}"))
        for match in PUA_USER_RE.finditer(line):
            user = match.group(1)
            if user not in ALLOWED_USER_COMPONENTS:
                hits.append((lineno, f"PUA-colon user path in {relative} (user {user})"))
    return hits


class PrivacyPathPolicyTest(unittest.TestCase):
    def test_no_developer_user_paths_in_tracked_files(self) -> None:
        findings: List[str] = []
        for path in _tracked_files():
            for lineno, message in _user_path_hits(path):
                findings.append(f"{path.relative_to(REPO_ROOT)}:{lineno}: {message}")
        self.assertEqual(
            [],
            findings,
            "Tracked files contain developer-identifying user paths; replace them with "
            "placeholder components from ALLOWED_USER_COMPONENTS "
            "(see llm-wiki/log/recent.md):\n" + "\n".join(findings),
        )

    def test_release_workflow_example_uses_environment_placeholder(self) -> None:
        text = RELEASE_WORKFLOW.read_text(encoding="utf-8")
        self.assertNotIn("C:\\Users\\", text)
        example_lines = [
            line
            for line in text.splitlines()
            if "CE_TOOLCHAIN_ROOT must point to the maintainer's dev checkout" in line
        ]
        if example_lines:
            self.assertIn("%USERPROFILE%", example_lines[0])


if __name__ == "__main__":
    unittest.main()
