"""The outside-repo SDK run: staging, helper-mode plumbing and the client job, without starting the runtime."""

from __future__ import annotations

import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools import run_sdk_outside_repo as tool  # noqa: E402


class StageTest(unittest.TestCase):
    def test_copies_binaries_package_folders_and_the_renamed_client_but_no_user_data(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            installed = root / "installed"
            for relative in ("captureengine.exe", "cengine.dll", "cengine.pdb", "ffmpeg/avcodec.dll",
                             "sdk/include/cengine/cengine.h", "logs/20260101_000000/controller.log",
                             "captures/clip.mp4", "config.ini", "dump.dmp"):
                path = installed / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("x", encoding="utf-8")
            client = root / "headless.exe"
            client.write_text("client", encoding="utf-8")

            staged = tool.stage(installed, client, root / "deploy")

            self.assertEqual(staged.name, tool.CLIENT_NAME)
            self.assertEqual(staged.read_text(encoding="utf-8"), "client")
            deploy = root / "deploy"
            for kept in ("captureengine.exe", "cengine.dll", "cengine.pdb", "ffmpeg/avcodec.dll",
                         "sdk/include/cengine/cengine.h"):
                self.assertTrue((deploy / kept).is_file(), kept)
            for dropped in ("logs", "captures", "dump.dmp"):
                self.assertFalse((deploy / dropped).exists(), dropped)
            self.assertNotEqual((deploy / "config.ini").read_text(encoding="utf-8"), "x",
                                "the deployment gets the shipped template, never the user's config.ini")


class HelperModeArgumentTest(unittest.TestCase):
    def test_client_arguments_record_and_repeat(self) -> None:
        self.assertEqual(tool.client_arguments(3000, 2, False), ["--record-ms", "3000", "--rounds", "2"])

    def test_isolated_mode_adds_only_the_client_flag(self) -> None:
        self.assertEqual(tool.client_arguments(3000, 2, True),
                         ["--record-ms", "3000", "--rounds", "2", "--isolated-helpers"])

    def test_both_runs_the_default_mode_first_then_the_isolated_one(self) -> None:
        self.assertEqual(tool.selected_modes("both"), [("default", False), ("isolated", True)])

    def test_a_single_mode_selects_only_itself(self) -> None:
        self.assertEqual(tool.selected_modes("default"), [("default", False)])
        self.assertEqual(tool.selected_modes("isolated"), [("isolated", True)])

    def test_helpers_defaults_to_both_and_rejects_unknown_modes(self) -> None:
        parser = tool.build_parser()
        self.assertEqual(parser.parse_args([]).helpers, "both")
        self.assertEqual(parser.parse_args(["--helpers", "isolated"]).helpers, "isolated")
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            parser.parse_args(["--helpers", "in-process"])


class ClientRunTest(unittest.TestCase):
    """The client is a plain Python child here: the job, the resume and the exit status are what is under test."""

    def test_the_exit_status_of_the_resumed_and_adopted_client_comes_back(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            status = tool.run_client(Path(sys.executable), ["-c", "raise SystemExit(3)"], Path(scratch), timeout=60)
        self.assertEqual(status, 3)

    def test_a_client_that_outlives_the_bound_is_reported_as_failed(self) -> None:
        # The child never exits on its own: the bound expires, and the job closes and ends it.
        with tempfile.TemporaryDirectory() as scratch, contextlib.redirect_stdout(io.StringIO()):
            blocks_forever = ["-c", "import threading; threading.Event().wait()"]
            status = tool.run_client(Path(sys.executable), blocks_forever, Path(scratch), timeout=1)
        self.assertEqual(status, 1)


if __name__ == "__main__":
    unittest.main()
