"""Privacy regression tests: tracked files and generated release artifacts must
not contain developer paths.

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

Generated release artifacts are covered separately: the build embeds the
developer profile root into PDBs, PE debug records, and the verification
manifest/summary unless redacted, so the helpers below (prefix-map flags,
manifest sanitization, binary scrubbing) must stay leak-free and are exercised
against a fake profile. The finalize stage additionally fails the build when a
shipped first-party PE/PDB retains the profile root.
"""

from __future__ import annotations

import re
import subprocess
import unittest
from pathlib import Path
from typing import List, Tuple
from unittest.mock import patch

import build

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


class PrivacyArtifactPolicyTest(unittest.TestCase):
    """The build must never leak the developer profile into generated artifacts."""

    def test_privacy_prefix_map_flags_cover_profile_in_all_native_flag_lists(self) -> None:
        spellings = build.profile_path_spellings()
        if not spellings:
            self.skipTest("USERPROFILE not set (non-Windows host)")
        flag_lists = [
            build.OPT_FLAGS_X64,
            build.HOOK_OPT_FLAGS_X64,
            build.OPT_FLAGS_X86,
            build.HOOK_OPT_FLAGS_X86,
            build.TESTAPP_OPT_FLAGS_X64,
            build.TESTAPP_OPT_FLAGS_X86,
        ]
        for flags in flag_lists:
            for spelling in spellings:
                sep = "/" if "/" in spelling else "\\"
                self.assertIn(
                    f"-ffile-prefix-map={spelling}=C:{sep}Users{sep}<developer>",
                    flags,
                    "native flag list is missing the privacy prefix map",
                )

    def test_append_windows_pdb_linker_flag_embeds_bare_pdb_name(self) -> None:
        if not build.IS_WINDOWS:
            self.skipTest("Windows-only PDB link flags")
        flags: list = []
        build.append_windows_pdb_linker_flag(flags, r"C:\installed\captureengine\capture_hook_x64.dll")
        self.assertIn("-Wl,--pdb=C:/installed/captureengine/capture_hook_x64.pdb", flags)
        self.assertIn("-Wl,/pdbaltpath:capture_hook_x64.pdb", flags)
        # Idempotent: repeated calls must not duplicate the flags.
        build.append_windows_pdb_linker_flag(flags, r"C:\installed\captureengine\capture_hook_x64.dll")
        self.assertEqual(flags.count("-Wl,/pdbaltpath:capture_hook_x64.pdb"), 1)

    def test_sanitize_privacy_paths_replaces_profile_in_text(self) -> None:
        with patch.dict(build.os.environ, {"USERPROFILE": r"C:\Users\TestUser"}):
            text = r"run_dir=C:\Users\TestUser\proj and fwd=C:/Users/TestUser/proj"
            sanitized = build.sanitize_privacy_paths(text)
            self.assertNotIn("TestUser", sanitized)
            self.assertIn(r"C:\Users\<developer>", sanitized)
            self.assertIn("C:/Users/<developer>", sanitized)
            self.assertIn("proj", sanitized)

    def test_sanitize_privacy_paths_replaces_component_spellings_in_text(self) -> None:
        with patch.dict(build.os.environ, {"USERPROFILE": r"C:\Users\TestUser"}):
            text = r"escaped=C:\\Users\\TestUser\\proj and msys=/c/Users/TestUser/proj"
            sanitized = build.sanitize_privacy_paths(text)
            self.assertNotIn("TestUser", sanitized)
            self.assertIn(r"C:\\Users\\<developer>\\proj", sanitized)
            self.assertIn("/c/Users/<developer>/proj", sanitized)

    def test_sanitize_privacy_values_walks_manifest_structures(self) -> None:
        with patch.dict(build.os.environ, {"USERPROFILE": r"C:\Users\TestUser"}):
            payload = {
                "run_dir": r"C:\Users\TestUser\build\verification\run1",
                "command": [r"C:\Users\TestUser\_tool\Python\x64\python.exe", "build.py"],
                "steps": {"build": {"details": {"archive": "C:/Users/TestUser/pkg.7z"}}},
                "success": True,
                "count": 3,
            }
            sanitized = build.sanitize_privacy_values(payload)
            self.assertEqual(sanitized["success"], True)
            self.assertEqual(sanitized["count"], 3)
            self.assertEqual(sanitized["command"][0], r"C:\Users\<developer>\_tool\Python\x64\python.exe")
            self.assertEqual(sanitized["steps"]["build"]["details"]["archive"], "C:/Users/<developer>/pkg.7z")
            self.assertNotIn("TestUser", str(sanitized))

    def test_scrub_profile_path_bytes_is_length_preserving_and_complete(self) -> None:
        with patch.dict(build.os.environ, {"USERPROFILE": r"C:\Users\TestUser"}):
            utf8 = (
                b"C:\\Users\\TestUser\\proj\\x.pdb "
                b"C:/Users/TestUser/proj/y.cpp "
                b"C:\\\\Users\\\\TestUser\\\\escaped.o "  # escaped command line
                b"/c/Users/TestUser/msys.o "  # MSYS drive path
                b"home\\TestUser\\posix.o"
            )
            utf16 = "C:\\Users\\TestUser\\proj\\z.cpp".encode("utf-16le")
            data = utf8 + b"\x00" + utf16
            scrubbed = build.scrub_profile_path_bytes(data)
            self.assertEqual(len(scrubbed), len(data))
            self.assertEqual(build.count_profile_path_hits(scrubbed), 0)
            self.assertIn(b"redact", scrubbed)
            self.assertIn("redact".encode("utf-16le"), scrubbed)
            self.assertNotIn(b"TestUser", scrubbed)

    def test_count_profile_path_hits_counts_both_spellings_and_encodings(self) -> None:
        with patch.dict(build.os.environ, {"USERPROFILE": r"C:\Users\TestUser"}):
            data = (
                b"C:\\Users\\TestUser\\a "
                b"C:/Users/TestUser/b "
                b"C:\\\\Users\\\\TestUser\\\\c "
                b"/c/Users/TestUser/d "
                b"home\\TestUser\\e "
                + "C:\\Users\\TestUser\\c".encode("utf-16le")
            )
            self.assertEqual(build.count_profile_path_hits(data), 6)
            self.assertEqual(build.count_profile_path_hits(b"no hits here"), 0)
            self.assertEqual(build.count_profile_path_hits(b"TestUser is not a path"), 0)


if __name__ == "__main__":
    unittest.main()
