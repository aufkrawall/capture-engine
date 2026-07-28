"""Policy tests for isolated and parallel sanitizer verification."""

import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import build


class VerificationParallelismTest(unittest.TestCase):
    def test_job_split_preserves_the_total_and_favors_product_build(self) -> None:
        self.assertEqual(build.verification_parallel_job_counts(1), (1, 0))
        self.assertEqual(build.verification_parallel_job_counts(16), (10, 6))
        for total in (2, 4, 16, 31):
            product, sanitizer = build.verification_parallel_job_counts(total)
            self.assertEqual(product + sanitizer, total)
            self.assertGreaterEqual(product, sanitizer)
            self.assertGreater(sanitizer, 0)

    def test_sanitizer_child_receives_its_worker_budget(self) -> None:
        command = build.sanitizer_regression_command(ccache_flag=False, jobs=5)
        self.assertIn("--jobs=5", command)
        self.assertIn("--sanitize-regression-child", command)
        self.assertIn("--incremental", command)

    def test_sanitizer_link_inputs_include_recorded_objects_and_linker(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            stage = Path(temporary)
            compiler = stage / "clang++.exe"
            linker = stage / "ld.lld.exe"
            object_file = stage / "source.o"
            output = stage / "captureengine.exe"
            for path in (compiler, linker, object_file, output):
                path.write_bytes(path.name.encode("ascii"))
            manifest = output.with_name(output.name + ".link-cache.json")
            manifest.write_text(
                json.dumps(
                    {
                        "command": [str(compiler), str(object_file), "-o", str(output)],
                        "cwd": None,
                    }
                )
                + "\n",
                encoding="utf-8",
            )
            with patch.object(build, "SANITIZER_STAGE_ROOT", temporary), patch.object(
                build, "get_link_resource_dir", return_value=None
            ):
                inputs = build.sanitizer_stage_link_inputs()

        self.assertIn(str(object_file.resolve()), inputs)
        self.assertIn(str(linker.resolve()), inputs)

    def test_sanitizer_child_uses_an_isolated_build_root(self) -> None:
        completed = build.subprocess.CompletedProcess(args=[], returncode=0, stdout="", stderr="")
        with tempfile.TemporaryDirectory() as temporary, patch.object(
            build, "SANITIZER_STAGE_ROOT", temporary
        ), patch.object(
            build, "verification_artifact_path", return_value=None
        ), patch.object(
            build, "record_verification_step"
        ), patch.object(
            build, "record_sanitizer_stage_success"
        ), patch.object(
            build, "log"
        ), patch.object(
            build, "run_logged_subprocess", return_value=completed
        ) as execute:
            build.run_sanitizer_regression_pass(ccache_flag=False, jobs=3)

        child_env = execute.call_args.kwargs["env"]
        self.assertEqual(child_env["CE_ISOLATED_BUILD_ROOT"], temporary)
        self.assertEqual(execute.call_args.args[0][-1], "--jobs=3")

    def test_compile_database_follows_the_isolated_root(self) -> None:
        with tempfile.TemporaryDirectory() as project, tempfile.TemporaryDirectory() as isolated:
            with patch.object(build, "PROJECT_ROOT", project), patch.object(
                build, "ISOLATED_BUILD_ROOT", isolated
            ):
                self.assertEqual(build.get_compile_commands_path(), str(Path(isolated) / "compile_commands.json"))
            with patch.object(build, "PROJECT_ROOT", project), patch.object(build, "ISOLATED_BUILD_ROOT", None):
                self.assertEqual(build.get_compile_commands_path(), str(Path(project) / "compile_commands.json"))

    def test_isolated_child_does_not_target_product_or_source_test_outputs(self) -> None:
        source = build.read_source_text()
        self.assertIn('TEST_OUTPUT_DIR = os.path.join(ISOLATED_BUILD_ROOT, "tests")', source)
        self.assertIn('child_env["CE_ISOLATED_BUILD_ROOT"] = SANITIZER_STAGE_ROOT', source)
        self.assertIn("tests_dir = TEST_OUTPUT_DIR", source)
        self.assertIn('isolated_config_dir = os.path.join(ISOLATED_BUILD_ROOT, "captureengine")', source)
        self.assertIn('VERIFICATION_DIR = os.path.join(ISOLATED_BUILD_ROOT or BUILD_DIR, "verification")', source)
        self.assertNotEqual(build.SANITIZER_STAGE_ROOT, build.INSTALLED_DIR)
        self.assertTrue(os.path.commonpath([build.BUILD_DIR, build.SANITIZER_STAGE_ROOT]) == build.BUILD_DIR)


if __name__ == "__main__":
    unittest.main()
