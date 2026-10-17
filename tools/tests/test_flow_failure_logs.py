# build.py executes its fragments via exec, so its module attributes exist only
# at runtime; pyright cannot see them through the facade.
# pyright: reportAttributeAccessIssue=false

import tempfile
import unittest
from pathlib import Path

import build


class FlowFailureLogsTest(unittest.TestCase):
    """A failed flow scenario's logs survive the next run of the scenario (FlowFSR, 2026-10-17)."""

    def test_failed_scenario_logs_are_copied_into_the_run_folder(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            logs = Path(temporary) / "flow_tests" / "logs" / "FlowFSR.Example"
            logs.mkdir(parents=True)
            (logs / "hook_debug.log").write_text("hook", encoding="utf-8")
            (logs / "d3d12_debug.log").write_text("frame 364 ERROR id=907", encoding="utf-8")
            destination = Path(temporary) / "verification" / "run" / "flow_logs" / "FlowFSR.Example"

            kept = build.preserve_failed_flow_logs(str(logs), str(destination))

            self.assertEqual(kept, str(destination))
            self.assertEqual((destination / "d3d12_debug.log").read_text(encoding="utf-8"), "frame 364 ERROR id=907")
            # The rerun recreates the scenario folder; the kept copy stays as it was.
            (logs / "d3d12_debug.log").write_text("rerun", encoding="utf-8")
            self.assertEqual((destination / "hook_debug.log").read_text(encoding="utf-8"), "hook")
            self.assertEqual((destination / "d3d12_debug.log").read_text(encoding="utf-8"), "frame 364 ERROR id=907")

    def test_nothing_is_kept_without_a_run_folder_or_logs(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            logs = Path(temporary) / "logs"
            logs.mkdir()
            self.assertIsNone(build.preserve_failed_flow_logs(str(logs), None))
            missing = Path(temporary) / "missing"
            self.assertIsNone(build.preserve_failed_flow_logs(str(missing), str(Path(temporary) / "out")))


if __name__ == "__main__":
    unittest.main()
