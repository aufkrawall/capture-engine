"""The outside-repo SDK run stages only the shippable package (the run itself is opt-in; see the tool)."""

from __future__ import annotations

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

    def test_client_arguments_record_and_repeat(self) -> None:
        self.assertEqual(tool.client_arguments(3000, 2), ["--record-ms", "3000", "--rounds", "2"])


if __name__ == "__main__":
    unittest.main()
