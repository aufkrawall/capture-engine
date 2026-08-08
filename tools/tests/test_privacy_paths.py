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

The Actions run log is covered too: it is the one release-path surface the build
cannot sanitize, because GitHub's runner writes the Windows computer name into it
and exposes no override. `release-log-cleanup.yml` deletes the log after the run,
and `ReleaseLogCleanupPolicyTest` pins that wiring.
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
        # The length-preserving filler `redact_user_component` writes into shipped
        # binaries. Correctly scrubbed text quotes it (`C:/Users/redact/...`), so
        # flagging it would fail the gate on evidence that the scrub worked.
        "redact",
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

# Domains that may legitimately appear in tracked files: the placeholder and
# noreply identities the project uses for commits. A real mailbox is as
# identifying as a profile path, so anything else fails the gate.
ALLOWED_EMAIL_DOMAINS = frozenset(
    {
        "example.com",
        "example.org",
        "users.noreply.github.com",
        "localhost",
    }
)

EMAIL_RE = re.compile(r"[A-Za-z0-9._%+-]+@([A-Za-z0-9.-]+\.[A-Za-z]{2,})")

# C:\Users\<name>\... (any drive letter, backslash spelling).
USER_PATH_RE = re.compile(r"[A-Za-z]:\\Users\\([^\\\r\n\s\"'`]+)")
# MSYS/Cygwin private-use-area colon spelling: C<U+F03A/U+FF1A>Users<name>.
PUA_USER_RE = re.compile(r"[A-Za-z][\uf03a\uff1a]Users([A-Za-z0-9_]+)")
# Path mangled into an identifier, where every separator became an underscore:
# `C__Users_<name>_Programme_...`. Doxygen names man pages this way, and the two
# patterns above cannot see it because the separators they anchor on are gone.
#
# This is the third place the same blind spot appeared - the binary scrub had it
# (fixed in a9590837), the log scrub had it, and this gate had it too, which is
# how documenting the log-scrub fix put the maintainer's real user name into four
# tracked files without anything objecting. `_` terminates the name here, which
# the general patterns must not accept.
MANGLED_USER_RE = re.compile(r"(?<![A-Za-z0-9])[A-Za-z]__Users_([A-Za-z0-9-]+)_")

RELEASE_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "release-stable.yml"
CLEANUP_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "release-log-cleanup.yml"


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


def _cleanup_events() -> dict:
    """Parsed triggers of the log-cleanup workflow.

    Parsed rather than text-matched because that file's comments quote the old
    design; a text assertion is satisfied by prose. Note `on` is a YAML 1.1
    boolean, so safe_load returns it under the key True.
    """
    import yaml

    document = yaml.safe_load(CLEANUP_WORKFLOW.read_text(encoding="utf-8"))
    trigger = document.get("on", document.get(True))
    if isinstance(trigger, str):
        return {trigger: None}
    if isinstance(trigger, list):
        return {name: None for name in trigger}
    return dict(trigger or {})


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
        for match in MANGLED_USER_RE.finditer(line):
            user = match.group(1)
            if user not in ALLOWED_USER_COMPONENTS:
                hits.append((lineno, f"path-derived identifier C__Users_{user}_ in {relative}"))
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

    def test_no_machine_name_in_tracked_files(self) -> None:
        skip = build.machine_name_scan_skip_reason()
        if skip:
            self.skipTest(f"machine-name scan unavailable: {skip}")
        findings: List[str] = []
        for path in _tracked_files():
            raw = path.read_bytes()
            if b"\x00" in raw:
                continue
            if build.count_machine_name_hits(raw):
                findings.append(str(path.relative_to(REPO_ROOT)))
        self.assertEqual(
            [],
            findings,
            "Tracked files contain this machine's name; replace it with a placeholder:\n"
            + "\n".join(findings),
        )

    def test_no_unexpected_email_addresses_in_tracked_files(self) -> None:
        # A real address is as identifying as a profile path and just as easy to
        # paste in. Only the placeholder/noreply domains the project already uses
        # for commit identities are allowed.
        findings: List[str] = []
        for path in _tracked_files():
            raw = path.read_bytes()
            if b"\x00" in raw:
                continue
            text = raw.decode("utf-8", errors="replace")
            for match in EMAIL_RE.finditer(text):
                if match.group(1).lower() not in ALLOWED_EMAIL_DOMAINS:
                    findings.append(f"{path.relative_to(REPO_ROOT)}: {match.group(0)}")
        self.assertEqual(
            [],
            findings,
            "Tracked files contain email addresses outside ALLOWED_EMAIL_DOMAINS:\n"
            + "\n".join(findings),
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


class ReleaseLogCleanupPolicyTest(unittest.TestCase):
    """The release run log is the last surface carrying maintainer identity.

    The release itself is clean - assets, notes, tag and verification manifest are
    scrubbed and the finalize stage fails the build otherwise - but GitHub's runner
    writes "Machine name: '<hostname>'" into the "Set up job" section of every run
    log from Environment.MachineName, and no environment or runner setting
    overrides it. release-log-cleanup.yml therefore deletes the log once the run
    completes. Each assertion below pins one way that cleanup could silently stop
    happening while release-stable itself kept succeeding.
    """

    text: str

    @classmethod
    def setUpClass(cls) -> None:
        cls.text = CLEANUP_WORKFLOW.read_text(encoding="utf-8") if CLEANUP_WORKFLOW.is_file() else ""

    def test_cleanup_workflow_exists(self) -> None:
        self.assertTrue(
            CLEANUP_WORKFLOW.is_file(),
            "release-log-cleanup.yml is missing; release run logs would keep publishing the runner hostname",
        )

    def test_cleanup_workflow_run_trigger_is_valid(self) -> None:
        # `workflows:` is REQUIRED for workflow_run. An earlier revision dropped it
        # so the trigger would match every workflow; GitHub rejected the file
        # instead, reported it by path rather than name, and every push failed -
        # there is no fallback to matching everything. The release run's log then
        # stayed published because the cleanup never ran at all.
        #
        # Assert on parsed YAML, not raw text: that file's comments quote the
        # patterns being discussed, so a text match is satisfied by prose.
        events = _cleanup_events()
        self.assertIn("workflow_run", events)
        trigger = events["workflow_run"] or {}
        self.assertIn(
            "workflows",
            trigger,
            "workflow_run requires `workflows:`; without it GitHub treats the file as invalid",
        )
        release_name = re.search(r"^name:\s*(\S+)", RELEASE_WORKFLOW.read_text(encoding="utf-8"), re.MULTILINE)
        self.assertIsNotNone(release_name)
        assert release_name is not None
        self.assertIn(
            release_name.group(1),
            trigger["workflows"],
            "the fast path must name the current release workflow; renaming it detaches the trigger",
        )
        self.assertEqual(trigger.get("types"), ["completed"], "logs are only final once the run completes")
        self.assertIn("self-hosted", self.text, "the job must gate on the self-hosted label")

    def test_cleanup_sweep_is_the_name_independent_mechanism(self) -> None:
        # Because the trigger above must name a workflow, the sweep is what covers
        # renamed, new and since-deleted workflows - the `debug-token` case, whose
        # two successful self-hosted logs nothing would ever have cleaned. It must
        # therefore decide from the run's own labels, not from a workflow name.
        self.assertIn("actions/runs/$id/jobs", self.text, "the sweep must inspect each run's labels")
        self.assertIn(".workflow_runs[].id", self.text, "the sweep must enumerate every run")

    def test_cleanup_retention_is_immediate_for_failures_too(self) -> None:
        # Failed logs carry the same hostname. Detailed diagnostics remain on the
        # persistent runner, so no GitHub-side grace period is justified.
        events = _cleanup_events()
        self.assertIn("schedule", events, "no scheduled sweep; missed logs would stay published")
        self.assertNotIn("grace_days", self.text)
        self.assertNotIn('if [ "$CONCLUSION" != "success" ]', self.text)

    def test_cleanup_runs_on_a_github_hosted_runner(self) -> None:
        # Running the cleanup on the self-hosted runner would write the very
        # hostname it exists to remove into this workflow's own log. Every job
        # must be GitHub-hosted, however many there are.
        runners = [value.strip() for value in re.findall(r"^\s*runs-on:\s*(.+)$", self.text, re.MULTILINE)]
        self.assertTrue(runners, "no runs-on found")
        for runner in runners:
            self.assertNotIn("self-hosted", runner)
            self.assertIn("ubuntu", runner)

    def test_cleanup_requests_actions_write_permission(self) -> None:
        # The repository default for GITHUB_TOKEN is read-only; without an
        # explicit grant the delete fails with 403.
        self.assertRegex(self.text, r"permissions:\s*\n\s*actions: write")

    def test_cleanup_deletes_the_triggering_runs_log(self) -> None:
        self.assertIn("github.event.workflow_run.id", self.text)
        self.assertIn('gh api -X DELETE "repos/$REPO/actions/runs/$RUN_ID/logs"', self.text)

    def test_cleanup_verifies_deletion_and_fails_closed(self) -> None:
        # A 204 from the DELETE is not proof the archive is gone, so the workflow
        # re-reads the endpoint and must fail the run when the log survived.
        self.assertIn("404|410)", self.text)
        self.assertIn("::error::", self.text)
        self.assertIn("exit 1", self.text)


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

    def test_sanitize_privacy_paths_redacts_path_derived_identifiers(self) -> None:
        # Run 31192891717 logged the maintainer's user name as
        # `C__Users_<developer>_Programme_...`: doxygen names its man pages after the
        # escaped absolute input path, so every separator the path-shaped rules
        # anchor on was gone, and the same path one line earlier was correctly
        # redacted while this copy was not.
        with patch.dict(build.os.environ, {"USERPROFILE": r"C:\Users\TestUser"}):
            text = "man3/C__Users_TestUser_Programme_build_x.3"
            sanitized = build.sanitize_privacy_paths(text)
            self.assertNotIn("TestUser", sanitized)
            self.assertIn("C__Users_<developer>_Programme_build_x.3", sanitized)

    def test_sanitize_privacy_paths_terminators_match_the_binary_scrub(self) -> None:
        # The log scrub kept the terminator allowlist that a9590837 replaced in
        # the binary scrub, so `;`, `)` and end-of-buffer were missed here.
        with patch.dict(build.os.environ, {"USERPROFILE": r"C:\Users\TestUser"}):
            for text in ("cmd=/c/Users/TestUser;next", "path=(/c/Users/TestUser)", "tail=/c/Users/TestUser"):
                self.assertNotIn("TestUser", build.sanitize_privacy_paths(text), text)

    def test_sanitize_privacy_paths_leaves_unrelated_names_alone(self) -> None:
        # A longer name that merely starts with the user name is not a leak, and
        # rewriting it would corrupt diagnostics.
        with patch.dict(build.os.environ, {"USERPROFILE": r"C:\Users\TestUser"}):
            text = "TestUserGroup and /c/Users/TestUserOther/x"
            self.assertEqual(build.sanitize_privacy_paths(text), text)

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

    def test_count_profile_path_hits_covers_terminators_beyond_path_separators(self) -> None:
        # Regression: the pattern used to accept only [\\/= "\x00] after the user
        # name, so the same path followed by ';', ')', "'", a newline, or ending
        # the buffer was neither scrubbed nor reported - the scrub and its
        # verification pass share this pattern, so a miss was invisible twice.
        with patch.dict(build.os.environ, {"USERPROFILE": r"C:\Users\TestUser"}):
            for suffix in (b"\\x", b"/x", b";", b")", b"'", b">", b"\n", b"\r\n", b"", b".bak"):
                data = b"C:\\Users\\TestUser" + suffix
                self.assertEqual(
                    build.count_profile_path_hits(data),
                    1,
                    f"user path went undetected when followed by {suffix!r}",
                )
                self.assertEqual(build.count_profile_path_hits(build.scrub_profile_path_bytes(data)), 0)

    def test_count_profile_path_hits_still_refuses_longer_names(self) -> None:
        # Widening the terminator set must not let a different, longer user name
        # that merely starts with this one match.
        with patch.dict(build.os.environ, {"USERPROFILE": r"C:\Users\User"}):
            self.assertEqual(build.count_profile_path_hits(b"C:\\Users\\UserData\\a"), 0)
            self.assertEqual(build.count_profile_path_hits(b"C:\\Users\\User-old\\a"), 0)
            self.assertEqual(build.count_profile_path_hits(b"C:\\Users\\User\\a"), 1)

    def test_user_component_patterns_escape_regex_metacharacters(self) -> None:
        # A user name containing '.' or '+' must be matched literally; unescaped
        # it would become a metacharacter and over-match unrelated bytes. The
        # profile is assembled rather than written as one literal so this file
        # stays clean for test_no_developer_user_paths_in_tracked_files.
        with patch.dict(build.os.environ, {"USERPROFILE": "C:\\Users\\" + "Test.User"}):
            self.assertEqual(build.count_profile_path_hits(b"C:\\Users\\Test.User\\x"), 1)
            self.assertEqual(build.count_profile_path_hits(b"C:\\Users\\TestxUser\\x"), 0)
            utf16 = "C:\\Users\\TestxUser\\x".encode("utf-16le")
            self.assertEqual(build.count_profile_path_hits(utf16), 0)

    def test_privacy_sanitize_log_text_is_env_gated(self) -> None:
        with patch.dict(build.os.environ, {"USERPROFILE": r"C:\Users\TestUser"}):
            message = r"Built: C:\Users\TestUser\proj\x.dll and msys=/c/Users/TestUser/y"
            with patch.dict(build.os.environ, {"CE_PRIVACY_SANITIZE_LOGS": ""}, clear=False):
                self.assertEqual(build.privacy_sanitize_log_text(message), message)
            with patch.dict(build.os.environ, {"CE_PRIVACY_SANITIZE_LOGS": "1"}, clear=False):
                sanitized = build.privacy_sanitize_log_text(message)
                self.assertNotIn("TestUser", sanitized)
                self.assertIn("proj", sanitized)
                self.assertIn("x.dll", sanitized)

    def test_log_output_uses_privacy_sanitizer(self) -> None:
        source = build.read_source_text()
        self.assertIn("formatted = privacy_sanitize_log_text(", source)

    def test_count_machine_name_hits_matches_whole_tokens_in_both_encodings(self) -> None:
        with patch.dict(build.os.environ, {"COMPUTERNAME": "BUILDHOST-01"}), patch.object(
            build.platform, "node", return_value="BUILDHOST-01"
        ):
            data = (
                b"host=BUILDHOST-01 "
                + "path=BUILDHOST-01".encode("utf-16le")
                + b" lower=buildhost-01"
            )
            # Upper + lower UTF-8 spelling and the UTF-16LE spelling.
            self.assertEqual(build.count_machine_name_hits(data), 3)
            self.assertEqual(build.count_machine_name_hits(b"nothing to see here"), 0)

    def test_count_machine_name_hits_ignores_substring_occurrences(self) -> None:
        # A name that merely occurs inside a longer identifier is not a leak, and
        # failing the build on it would be worse than not checking at all.
        with patch.dict(build.os.environ, {"COMPUTERNAME": "BUILDHOST"}), patch.object(
            build.platform, "node", return_value="BUILDHOST"
        ):
            self.assertEqual(build.count_machine_name_hits(b"MYBUILDHOSTX"), 0)
            self.assertEqual(build.count_machine_name_hits(b"BUILDHOSTING"), 0)
            self.assertEqual(build.count_machine_name_hits(b"built on BUILDHOST."), 1)

    def test_machine_name_scan_is_skipped_for_ambiguously_short_names(self) -> None:
        with patch.dict(build.os.environ, {"COMPUTERNAME": "PC"}), patch.object(
            build.platform, "node", return_value="PC"
        ):
            self.assertNotEqual(build.machine_name_scan_skip_reason(), "")
            self.assertEqual(build.count_machine_name_hits(b"PC PC PC"), 0)

    def test_machine_name_scan_survives_a_reassigned_computername(self) -> None:
        # Presetting COMPUTERNAME does not change the real host name (the runner's
        # Machine name line proves it), so the scan must also consult
        # platform.node() and cannot be disabled by reassigning the variable.
        with patch.dict(build.os.environ, {"COMPUTERNAME": "NEUTRAL"}), patch.object(
            build.platform, "node", return_value="REALHOST-77"
        ):
            self.assertEqual(build.machine_name_scan_skip_reason(), "")
            self.assertEqual(build.count_machine_name_hits(b"built on REALHOST-77"), 1)

    def test_finalize_privacy_scan_verifies_the_machine_name(self) -> None:
        # Source policy: the finalize stage must keep checking, and must not start
        # scrubbing the machine name - a hit is a new leak source to identify.
        source = build.read_source_text()
        self.assertIn("count_machine_name_hits(data)", source)
        self.assertNotIn("scrub_machine_name", source)


if __name__ == "__main__":
    unittest.main()
