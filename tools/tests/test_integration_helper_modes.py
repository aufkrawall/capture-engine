# pyright: reportAttributeAccessIssue=false
"""The integration matrix runs every scenario with helper processes and with helper threads (libcengine 11, M21)."""

from __future__ import annotations

import importlib.util
import subprocess
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]


def load_runner():
    """testapp/run_tests.py executes its fragments in its own globals; importing it does not start a run."""
    spec = importlib.util.spec_from_file_location("ce_testapp_run_tests", ROOT / "testapp" / "run_tests.py")
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class HelperIsolationConfigTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.runner = load_runner()

    def edit(self, text: str, helpers: str) -> str:
        return self.runner.set_helper_isolation_in_config_text(text, self.runner.HELPER_ISOLATION_VALUES[helpers])

    def test_auto_leaves_the_config_alone(self) -> None:
        text = "[Runtime]\nisolated_helpers=auto\n"
        self.assertEqual(self.edit(text, "auto"), text)

    def test_an_existing_key_is_replaced_with_its_line_ending(self) -> None:
        self.assertEqual(self.edit("[A]\r\nx=1\r\n[Runtime]\r\nisolated_helpers=auto\r\n[B]\r\n", "threads"),
                         "[A]\r\nx=1\r\n[Runtime]\r\nisolated_helpers=off\r\n[B]\r\n")

    def test_a_runtime_section_without_the_key_gets_it(self) -> None:
        self.assertEqual(self.edit("[A]\nx=1\n[Runtime]\n[B]\n", "processes"),
                         "[A]\nx=1\n[Runtime]\nisolated_helpers=on\n[B]\n")

    def test_a_config_without_the_section_gets_both(self) -> None:
        self.assertEqual(self.edit("[A]\nx=1", "processes"), "[A]\nx=1\n\n[Runtime]\nisolated_helpers=on\n")

    def test_the_key_in_another_section_is_not_touched(self) -> None:
        self.assertEqual(self.edit("[Other]\nisolated_helpers=x\n", "threads"),
                         "[Other]\nisolated_helpers=x\n\n[Runtime]\nisolated_helpers=off\n")


class MatrixTest(unittest.TestCase):
    def test_every_smoke_scenario_runs_in_both_helper_modes(self) -> None:
        import build  # noqa: E402  build.py executes its fragments (tools/build/*) in its own module

        executed = []

        def fake_run(cmd, *args, **kwargs):
            executed.append(cmd)
            return subprocess.CompletedProcess(cmd, 0)

        patches = (
            mock.patch.object(build.subprocess, "run", side_effect=fake_run),
            mock.patch.object(build, "log"),
            mock.patch.object(build, "record_verification_artifact"),
            mock.patch.object(build, "record_verification_step"),
            mock.patch.object(build.os, "makedirs"),
        )
        for patch in patches:
            patch.start()
            self.addCleanup(patch.stop)
        build.run_integration_tests({}, full_matrix=False)

        labels = [next(Path(arg).stem for arg in cmd if str(arg).endswith(".json")) for cmd in executed]
        for scenario in ("smoke_dx9", "smoke_dx11", "smoke_modern"):
            for helpers in ("processes", "threads"):
                self.assertIn(f"integration_{scenario}_{helpers}", labels)
                command = executed[labels.index(f"integration_{scenario}_{helpers}")]
                self.assertEqual(command[command.index("--helpers") + 1], helpers)


if __name__ == "__main__":
    unittest.main()
