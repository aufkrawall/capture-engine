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
            self.assertTrue((deploy / tool.RECORDINGS_DIRECTORY).is_dir())
            self.assertIn(f"output_dir={deploy / tool.RECORDINGS_DIRECTORY}\n",
                          (deploy / "config.ini").read_text(encoding="utf-8"),
                          "recordings must land in the deployment, never in the user's Videos folder")


class StagedConfigTest(unittest.TestCase):
    TEMPLATE = ("; comment\r\n[Output]\r\noutput_dir=%VIDEOS%\\Capture Engine\r\nformat=mp4\r\n"
                "[Benchmark]\r\noutput_dir=%DOCUMENTS%\\Capture Engine Benchmarks\r\n")

    def test_only_the_output_section_folder_changes_and_line_endings_stay(self) -> None:
        staged = tool.staged_config(self.TEMPLATE, Path("C:/deploy/recordings"))
        expected = self.TEMPLATE.replace("output_dir=%VIDEOS%\\Capture Engine",
                                         f"output_dir={Path('C:/deploy/recordings')}")
        self.assertEqual(staged, expected)

    def test_a_template_without_the_output_folder_is_refused(self) -> None:
        with self.assertRaises(ValueError):
            tool.staged_config("[Benchmark]\noutput_dir=x\n", Path("C:/deploy/recordings"))


class HelperModeArgumentTest(unittest.TestCase):
    def test_client_arguments_record_and_repeat(self) -> None:
        self.assertEqual(tool.client_arguments(3000, 2, False), ["--record-ms", "3000", "--rounds", "2"])

    def test_isolated_mode_adds_only_the_client_flag(self) -> None:
        self.assertEqual(tool.client_arguments(3000, 2, True),
                         ["--record-ms", "3000", "--rounds", "2", "--isolated-helpers"])

    def test_video_several_recordings_and_overlap_reach_the_client(self) -> None:
        self.assertEqual(tool.client_arguments(3000, 2, False, video=True, recordings=3, overlap=True),
                         ["--record-ms", "3000", "--rounds", "2", "--video", "--recordings", "3", "--overlap"])

    def test_overlap_needs_more_than_one_recording(self) -> None:
        self.assertEqual(tool.client_arguments(3000, 1, False, video=True, recordings=1, overlap=True),
                         ["--record-ms", "3000", "--rounds", "1", "--video"])

    def test_the_parser_defaults_to_overlapping_video_recordings(self) -> None:
        args = tool.build_parser().parse_args([])
        self.assertEqual(args.recordings, 2)
        self.assertFalse(args.no_overlap)
        self.assertFalse(args.audio_only)
        self.assertFalse(args.strict_residue)

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


class ResidueTest(unittest.TestCase):
    LINE = ("[12:00:00.000][INFO] [RuntimeTeardown] residue handles=+3 threads=+0 gdi=+0 user=+0 "
            "private=+1.5MiB vram=n/a")

    def test_a_residue_line_parses_into_fields(self) -> None:
        residue = tool.parse_residue("20261021_120000", self.LINE)
        self.assertIsNotNone(residue)
        assert residue is not None
        self.assertEqual(residue.kind, "RuntimeTeardown")
        self.assertEqual(residue.fields["handles"], "+3")
        self.assertEqual(residue.fields["vram"], "n/a")

    def test_other_lines_are_not_residue(self) -> None:
        self.assertIsNone(tool.parse_residue("s", "[INFO] [Runtime] helper transports: inject=thread"))

    def test_logs_of_every_session_are_collected_in_order(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            deployment = Path(scratch)
            for session, line in (("20261021_120002", "[MediaTeardown] residue sessions=2 handles=+0 threads=+0 "
                                   "gdi=+0 user=+0 private=+0.0MiB vram=+0.0MiB"),
                                  ("20261021_120001", self.LINE)):
                folder = deployment / "logs" / session
                folder.mkdir(parents=True)
                (folder / "captureengine.log").write_text(line + "\n", encoding="utf-8")
            residues = tool.collect_residue(deployment)
        self.assertEqual([r.session for r in residues], ["20261021_120001", "20261021_120002"])
        self.assertEqual([r.kind for r in residues], ["RuntimeTeardown", "MediaTeardown"])

    def test_the_first_round_is_warm_up_and_later_growth_fails(self) -> None:
        first = tool.parse_residue("a", self.LINE)
        clean = tool.parse_residue("b", "[RuntimeTeardown] residue handles=+0 threads=+0 gdi=+0 user=+0 "
                                        "private=+0.2MiB vram=+0.0MiB")
        grown = tool.parse_residue("c", "[MediaTeardown] residue sessions=2 handles=+0 threads=+1 gdi=+2 user=+0 "
                                        "private=+0.0MiB vram=+0.0MiB")
        assert first is not None and clean is not None and grown is not None
        self.assertEqual(tool.residue_failures([first, clean]), [])
        self.assertEqual(tool.residue_failures([first, clean, grown]), ["c MediaTeardown: threads=+1 gdi=+2"])


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
