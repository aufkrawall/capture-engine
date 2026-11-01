"""Pinned git sources come from a machine-wide mirror; the network only fills gaps.

Drives the real gits against throwaway repositories: Git for Windows for the mirror and
the fixture, the toolchain's MSYS2 git for the working checkout. "Upstream" is a local
bare repository standing in for git.ffmpeg.org / GitHub; making its URL unusable after
the mirror exists proves a later clone needs no network at all.
"""

# build.py executes its fragments via exec; pyright cannot see their attributes.
# pyright: reportAttributeAccessIssue=false

from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from typing import List
from unittest import mock

from tools.git_mirror import PIN_NAMESPACE, GitMirror, GitMirrorError, find_mirror_git

PROJECT_ROOT = Path(__file__).resolve().parents[2]
MSYS_DIR = PROJECT_ROOT / "build" / "msys64"
MSYS_GIT = MSYS_DIR / "usr" / "bin" / "git.exe"


def _unix(path: str) -> str:
    path = path.replace("\\", "/")
    return "/" + path[0].lower() + path[2:] if len(path) > 1 and path[1] == ":" else path


def _windows_git() -> str:
    try:
        return find_mirror_git(str(MSYS_DIR))
    except GitMirrorError:
        return ""


@unittest.skipUnless(MSYS_GIT.is_file() and _windows_git(), "MSYS2 git and Git for Windows are required")
class GitMirrorTest(unittest.TestCase):
    def setUp(self) -> None:
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.temp = temp.name
        empty_config = os.path.join(self.temp, "empty.gitconfig")
        Path(empty_config).write_text("", encoding="utf-8")
        self.env = dict(os.environ)
        self.env.update(
            {
                "GIT_CONFIG_NOSYSTEM": "1",
                "GIT_CONFIG_GLOBAL": empty_config,
                "GIT_TERMINAL_PROMPT": "0",
                "GIT_AUTHOR_NAME": "mirror-test",
                "GIT_AUTHOR_EMAIL": "mirror-test@invalid",
                "GIT_COMMITTER_NAME": "mirror-test",
                "GIT_COMMITTER_EMAIL": "mirror-test@invalid",
            }
        )
        self.windows_git = _windows_git()
        work = os.path.join(self.temp, "work")
        os.makedirs(work)
        self._git(work, "init", "--quiet", "-b", "master", ".")
        self.first = self._commit(work, "first")
        self.pinned = self._commit(work, "pinned")
        self._git(work, "tag", "--annotate", "--message", "release", "n1.0")
        self.upstream = os.path.join(self.temp, "upstream.git")
        self._git(self.temp, "clone", "--quiet", "--bare", work, self.upstream)
        self.work = work
        self.messages: List[str] = []
        self.mirror = GitMirror(
            self.windows_git,
            str(MSYS_GIT),
            self.env,
            os.path.join(self.temp, "cache", "git"),
            path_for_checkout_git=_unix,
            logger=self.messages.append,
            lock_dir=os.path.join(self.temp, "cache", "locks"),
        )

    def _git(self, cwd: str, *arguments: str, msys: bool = False) -> str:
        git = str(MSYS_GIT) if msys else self.windows_git
        return subprocess.run([git, *arguments], cwd=cwd, env=self.env, check=True, stdin=subprocess.DEVNULL,
                              capture_output=True).stdout.decode().strip()

    def _commit(self, cwd: str, message: str) -> str:
        self._git(cwd, "commit", "--quiet", "--allow-empty", "--message", message)
        return self._git(cwd, "rev-parse", "HEAD")

    def _break_upstream(self) -> None:
        shutil.move(self.upstream, self.upstream + ".offline")

    def test_clone_checks_out_the_pin_from_the_mirror(self) -> None:
        dest = os.path.join(self.temp, "repos", "ffmpeg")
        self.mirror.clone("ffmpeg", ["https://canonical.invalid/ffmpeg.git", self.upstream],
                          self.pinned, dest)
        self.assertEqual(self._git(dest, "rev-parse", "HEAD"), self.pinned)
        self.assertEqual(self._git(dest, "status", "--porcelain"), "")
        # Humans and later network fetches still see the canonical upstream.
        self.assertEqual(self._git(dest, "remote", "get-url", "origin"), "https://canonical.invalid/ffmpeg.git")
        mirror = self.mirror.mirror_path("ffmpeg")
        self.assertEqual(self._git(mirror, "rev-parse", f"{PIN_NAMESPACE}/{self.pinned}"), self.pinned)
        # FFmpeg's version.sh describes the checkout from its tags; they must survive.
        self.assertIn("n1.0", self._git(dest, "tag", "--list"))

    def test_a_populated_mirror_needs_no_network(self) -> None:
        self.mirror.ensure("ffmpeg", [self.upstream], self.pinned)
        self._break_upstream()
        self.messages.clear()
        dest = os.path.join(self.temp, "repos", "ffmpeg")
        self.mirror.clone("ffmpeg", [self.upstream], self.pinned, dest)
        self.assertEqual(self._git(dest, "rev-parse", "HEAD"), self.pinned)
        self.assertTrue(any("no network access" in message for message in self.messages), self.messages)

    def test_unreachable_first_source_falls_back_to_the_next(self) -> None:
        missing = os.path.join(self.temp, "missing.git")
        mirror = self.mirror.ensure("ffnvcodec", [missing, self.upstream], self.pinned)
        self.assertEqual(self._git(mirror, "rev-parse", f"{self.pinned}^0"), self.pinned)

    def test_a_pin_on_no_branch_is_fetched_by_id_and_kept_reachable(self) -> None:
        # The #6 failure: FFMPEG_SOURCE_REF was absent from the default branch.
        self.mirror.ensure("ffmpeg", [self.upstream], self.pinned)
        self._git(self.work, "checkout", "--quiet", "--detach")
        detached = self._commit(self.work, "only reachable by id")
        self._git(self.work, "push", "--quiet", self.upstream, f"{detached}:refs/hidden/x")
        self._git(self.upstream, "config", "uploadpack.allowAnySHA1InWant", "true")
        mirror = self.mirror.ensure("ffmpeg", [self.upstream], detached)
        self.assertEqual(self._git(mirror, "rev-parse", f"{PIN_NAMESPACE}/{detached}"), detached)

    def test_existing_checkout_moves_to_a_new_pin_through_the_mirror(self) -> None:
        dest = os.path.join(self.temp, "repos", "ffmpeg")
        self.mirror.clone("ffmpeg", [self.upstream], self.first, dest)
        newer = self._commit(self.work, "newer pin")
        self._git(self.work, "push", "--quiet", self.upstream, "master")
        self.mirror.replace_checkout("ffmpeg", [self.upstream], newer, dest)
        self.assertEqual(self._git(dest, "rev-parse", "HEAD", msys=True), newer)

    def test_the_checkout_keeps_script_bytes_and_is_made_by_the_toolchain_git(self) -> None:
        # FFmpeg's configure must keep LF. The checkout is the MSYS2 git's, never Git
        # for Windows' (whose system core.autocrlf would rewrite it).
        script_bytes = b"#!/bin/sh\necho ok\n"
        Path(self.work, "configure").write_bytes(script_bytes)
        self._git(self.work, "add", "configure")
        self._git(self.work, "commit", "--quiet", "--message", "script")
        script = self._git(self.work, "rev-parse", "HEAD")
        self._git(self.work, "push", "--quiet", self.upstream, "master")
        dest = os.path.join(self.temp, "repos", "ffmpeg")
        self.mirror.clone("ffmpeg", [self.upstream], script, dest)
        self.assertEqual(Path(dest, "configure").read_bytes(), script_bytes)

    def test_a_pin_nobody_serves_fails_without_leaving_a_checkout(self) -> None:
        dest = os.path.join(self.temp, "repos", "ffmpeg")
        with self.assertRaises(GitMirrorError):
            self.mirror.clone("ffmpeg", [self.upstream], "0123456789abcdef0123456789abcdef01234567", dest)
        self.assertFalse(os.path.exists(dest))

    def test_no_source_at_all_leaves_no_partial_mirror(self) -> None:
        with self.assertRaises(GitMirrorError):
            self.mirror.ensure("ffmpeg", [os.path.join(self.temp, "missing.git")], self.pinned)
        self.assertEqual([], [name for name in os.listdir(self.mirror.root) if name.startswith("ffmpeg")])


class BuilderUsesMirrorTest(unittest.TestCase):
    def test_a_fresh_pinned_clone_never_clones_from_the_network_directly(self) -> None:
        import build

        with tempfile.TemporaryDirectory() as root:
            builder = build.FFmpegBuilder(root_dir=root, msys_dir=os.path.join(root, "msys64"),
                                          install_dir=os.path.join(root, "install"),
                                          dependency_prefix_path=os.path.join(root, "deps"))
            fake = mock.Mock()
            with mock.patch.object(builder, "source_mirror", return_value=fake), \
                    mock.patch.object(builder, "run") as direct:
                dest, updated = builder.git_clone(["https://a.invalid/x.git", "https://b.invalid/x.git"], "ffmpeg",
                                                  update=False, ref="a" * 40)
            self.assertTrue(updated)
            fake.clone.assert_called_once_with("ffmpeg", ["https://a.invalid/x.git", "https://b.invalid/x.git"],
                                               "a" * 40, dest)
            direct.assert_not_called()


if __name__ == "__main__":
    unittest.main()
