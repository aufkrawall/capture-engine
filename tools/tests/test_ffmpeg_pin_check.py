import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import build

PROJECT_ROOT = Path(__file__).resolve().parents[2]
MSYS_DIR = PROJECT_ROOT / "build" / "msys64"
MSYS_GIT = MSYS_DIR / "usr" / "bin" / "git.exe"
# The MSYS2 runtime globs and brace-expands these in the unquoted arguments a
# native parent passes it; subprocess quotes only whitespace.
MSYS_GLOB_CHARACTERS = set("{}*?[")


def _builder(root: str, msys_dir: Path = MSYS_DIR):
    return build.FFmpegBuilder(
        root_dir=root,
        msys_dir=str(msys_dir),
        install_dir=os.path.join(root, "install"),
        dependency_prefix_path=os.path.join(root, "deps"),
    )


class PinCheckArgumentTest(unittest.TestCase):
    """Runs everywhere: the pin check must pass git arguments MSYS2 leaves intact."""

    def test_pin_check_arguments_survive_msys_globbing(self) -> None:
        # Until c08a7f45 the check asked git for `<sha>^{commit}`, which msys git
        # received as `<sha>^commit`; the check failed even at the pinned HEAD and
        # the build ran `git fetch --unshallow` in the shared FFmpeg repo.
        sha = "86940d45aff7d59810794df3ab2b39b7b83b478c"
        commands = []

        def fake_check_output(command, **_kwargs):
            commands.append(list(command))
            return (sha + "\n").encode()

        with tempfile.TemporaryDirectory() as root:
            builder = _builder(root)
            with mock.patch.object(build.subprocess, "check_output", side_effect=fake_check_output):
                self.assertTrue(builder._repo_is_at_ref(root, sha, "git", {}))
                self.assertTrue(builder._ref_resolves_locally(root, sha, "git", {}))
        self.assertTrue(commands)
        for command in commands:
            for argument in command[1:]:
                self.assertFalse(
                    MSYS_GLOB_CHARACTERS & set(argument),
                    f"{argument!r} would be glob/brace-expanded by the MSYS2 runtime",
                )


@unittest.skipUnless(MSYS_GIT.is_file(), "MSYS2 git is required to exercise its argument handling")
class MsysGitPinCheckTest(unittest.TestCase):
    """Drives the real msys git against a throwaway repository, never the shared one.

    test_ffmpeg_dependencies covers a plain commit pin at HEAD; these cover tags,
    a HEAD off the pin, and that git_clone never goes to the network for either.
    """

    @classmethod
    def setUpClass(cls) -> None:
        temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(temp.cleanup)
        # Keep the user's and the MSYS2 system git configuration (signing,
        # hooks, templates) out of the fixture.
        empty_config = os.path.join(temp.name, "empty.gitconfig")
        Path(empty_config).write_text("", encoding="utf-8")
        isolation = mock.patch.dict(
            os.environ,
            {
                "GIT_CONFIG_NOSYSTEM": "1",
                "GIT_CONFIG_GLOBAL": empty_config,
                "GIT_TERMINAL_PROMPT": "0",
                "GIT_AUTHOR_NAME": "pin-test",
                "GIT_AUTHOR_EMAIL": "pin-test@invalid",
                "GIT_COMMITTER_NAME": "pin-test",
                "GIT_COMMITTER_EMAIL": "pin-test@invalid",
            },
        )
        isolation.start()
        cls.addClassCleanup(isolation.stop)
        builder = _builder(temp.name)
        cls.env = builder.get_msys_env()
        cls.git = builder.get_tool_path("git")
        # One template repository, copied per test: each msys git call costs a
        # process start, and the tests move HEAD.
        cls.template = os.path.join(temp.name, "template")
        os.makedirs(cls.template)
        cls._git_in(cls.template, "init", "--quiet", ".")
        cls._git_in(cls.template, "remote", "add", "origin", os.path.join(temp.name, "missing-origin"))
        cls._git_in(cls.template, "commit", "--quiet", "--allow-empty", "--message", "pinned")
        cls.pinned = cls._git_in(cls.template, "rev-parse", "HEAD")
        cls._git_in(cls.template, "tag", "--annotate", "--message", "pinned tag", "n0.0-test")

    def setUp(self) -> None:
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.builder = _builder(temp.name)
        self.repo = os.path.join(self.builder.repos_dir, "ffmpeg")
        shutil.copytree(self.template, self.repo)

    @classmethod
    def _git_in(cls, cwd: str, *arguments: str) -> str:
        return subprocess.run(
            [cls.git, *arguments],
            cwd=cwd,
            env=cls.env,
            check=True,
            capture_output=True,
        ).stdout.decode().strip()

    def _git(self, *arguments: str) -> str:
        return self._git_in(self.repo, *arguments)

    def _commit(self, message: str) -> str:
        self._git("commit", "--quiet", "--allow-empty", "--message", message)
        return self._git("rev-parse", "HEAD")

    def _offline_run(self, commands):
        def run(command, cwd=None, env=None, check=True):
            commands.append(list(command))
            if command[1] in ("fetch", "clone"):
                raise AssertionError(f"pin check went to the network: {command[1:]}")
            subprocess.run(command, cwd=cwd, env=env, check=check, capture_output=True)

        return run

    def test_head_at_pinned_annotated_tag_is_recognized(self) -> None:
        # `^0` must keep peeling a tag object to its commit, as `^{commit}` did.
        self.assertTrue(self.builder._repo_is_at_ref(self.repo, "n0.0-test", self.git, self.env))

    def test_head_off_the_pin_is_not_at_ref_but_resolves_locally(self) -> None:
        self._commit("after the pin")
        self.assertFalse(self.builder._repo_is_at_ref(self.repo, self.pinned, self.git, self.env))
        self.assertTrue(self.builder._ref_resolves_locally(self.repo, self.pinned, self.git, self.env))

    def test_absent_commit_does_not_resolve_locally(self) -> None:
        # The only case that may fetch.
        absent = "0123456789abcdef0123456789abcdef01234567"
        self.assertFalse(self.builder._ref_resolves_locally(self.repo, absent, self.git, self.env))

    def test_clone_at_pin_needs_no_git_command(self) -> None:
        commands = []
        with mock.patch.object(self.builder, "run", side_effect=self._offline_run(commands)):
            dest, updated = self.builder.git_clone(["https://invalid.test/ffmpeg.git"], "ffmpeg",
                                                   update=False, ref=self.pinned)
        self.assertEqual(os.path.normcase(dest), os.path.normcase(self.repo))
        self.assertFalse(updated)
        self.assertEqual(commands, [])

    def test_clone_off_pin_with_local_commit_checks_out_without_fetch(self) -> None:
        self._commit("after the pin")
        commands = []
        with mock.patch.object(self.builder, "run", side_effect=self._offline_run(commands)):
            _, updated = self.builder.git_clone(["https://invalid.test/ffmpeg.git"], "ffmpeg",
                                                update=False, ref=self.pinned)
        self.assertTrue(updated)
        self.assertEqual([command[1:] for command in commands], [["checkout", "--force", self.pinned]])
        self.assertEqual(self._git("rev-parse", "HEAD"), self.pinned)


if __name__ == "__main__":
    unittest.main()
