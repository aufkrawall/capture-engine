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

    def test_link_inputs_fingerprint_the_linker_the_driver_actually_runs(self) -> None:
        """A cross driver's linker usually lives outside its own bin directory.

        Resolving it only by sibling layout picks up the host linker that
        happens to share a name, so a real cross-linker change would not
        invalidate the link cache while an unrelated host change would.
        """
        with tempfile.TemporaryDirectory() as temporary:
            stage = Path(temporary)
            compiler_dir = stage / "bin"
            cross_dir = stage / "cross" / "bin"
            compiler_dir.mkdir(parents=True)
            cross_dir.mkdir(parents=True)
            compiler = compiler_dir / "x86_64-w64-mingw32-g++"
            sibling_linker = compiler_dir / "ld"
            cross_linker = cross_dir / "ld"
            for path in (compiler, sibling_linker, cross_linker):
                # A driver query only happens for a loadable image, so the
                # stand-in carries a program magic like the real toolchain.
                path.write_bytes(b"MZ" + path.name.encode("ascii"))

            def fake_check_output(command, **kwargs):
                self.assertEqual(command[0], str(compiler))
                if command[1] == "-print-prog-name=ld":
                    return str(cross_linker) + "\n"
                return command[1].split("=", 1)[1] + "\n"

            build.resolve_link_program_paths.cache_clear()
            try:
                with patch.object(build.subprocess, "check_output", fake_check_output):
                    resolved = build.resolve_link_program_paths(str(compiler))
            finally:
                build.resolve_link_program_paths.cache_clear()

        self.assertIn(str(cross_linker), resolved)
        self.assertIn(str(sibling_linker), resolved)

    def test_unresolvable_linker_names_are_not_fingerprinted(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            compiler = Path(temporary) / "clang++"
            compiler.write_bytes(b"MZcompiler")

            build.resolve_link_program_paths.cache_clear()
            try:
                # Drivers echo the bare program name back when lookup fails.
                with patch.object(build.subprocess, "check_output", lambda command, **kwargs: "ld\n"):
                    resolved = build.resolve_link_program_paths(str(compiler))
            finally:
                build.resolve_link_program_paths.cache_clear()

        self.assertEqual(resolved, ())

    def test_a_file_that_is_not_a_program_image_is_never_spawned(self) -> None:
        """Windows answers an unloadable child image with a modal dialog.

        CreateProcess hands a file it cannot classify to the 16-bit path, and
        CSRSS blocks the caller on an "unsupported 16-bit application" box until
        somebody clicks OK - a build stall with no log line and no timeout. The
        link fingerprint therefore inspects the magic bytes before it spawns
        anything, so a placeholder or truncated compiler stays a fast miss.
        """
        with tempfile.TemporaryDirectory() as temporary:
            stage = Path(temporary)
            compiler = stage / "clang++.exe"
            sibling_linker = stage / "ld.lld.exe"
            compiler.write_bytes(b"not a program image")
            sibling_linker.write_bytes(b"MZld.lld")

            def refuse_to_spawn(command, **kwargs):
                raise AssertionError(f"spawned a non-image file: {command}")

            build.resolve_link_program_paths.cache_clear()
            try:
                with patch.object(build.subprocess, "check_output", refuse_to_spawn):
                    resolved = build.resolve_link_program_paths(str(compiler))
            finally:
                build.resolve_link_program_paths.cache_clear()

        self.assertEqual(resolved, (str(sibling_linker.resolve()),))

    def test_program_images_are_recognized_by_their_magic_bytes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            stage = Path(temporary)
            cases = {
                "windows.exe": (b"MZ\x90\x00stub", True),
                "linux.elf": (b"\x7fELF\x02\x01", True),
                "wrapper.sh": (b"#!/bin/sh\nexec clang++ \"$@\"\n", True),
                "placeholder.exe": (b"compiler", False),
                "empty.exe": (b"", False),
            }
            for name, (content, expected) in cases.items():
                path = stage / name
                path.write_bytes(content)
                self.assertEqual(build.looks_like_executable_image(str(path)), expected, name)

            self.assertFalse(build.looks_like_executable_image(str(stage / "absent.exe")))
            self.assertFalse(build.looks_like_executable_image(str(stage)))

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
