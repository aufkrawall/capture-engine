# SPDX-License-Identifier: MIT
# Copyright (c) 2026 aufkrawall

# build.py executes its fragments via exec, so its module attributes exist only
# at runtime; pyright cannot see them through the facade.
# pyright: reportAttributeAccessIssue=false

import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import build


class PackagingTests(unittest.TestCase):
    def test_corresponding_source_stages_patched_ffmpeg_and_verified_libiconv_inputs(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "ffmpeg-repo"
            downloads = root / "downloads"
            destination = root / "stage" / "ffmpeg-corresponding-source"
            (source / ".git").mkdir(parents=True)
            downloads.mkdir()
            (source / "configure").write_text("synthetic source", encoding="utf-8")
            (source / ".git" / "private").write_text("not packaged", encoding="utf-8")
            libiconv = next(
                item for item in build.FFMPEG_DEPENDENCY_MANIFEST_DATA["dependencies"] if item["name"] == "libiconv"
            )
            upstream_name = os.path.basename(libiconv["upstream_source_url"])
            for name in (upstream_name, libiconv["source_package"], libiconv["source_package"] + ".sig"):
                (downloads / name).write_bytes(name.encode("utf-8"))

            hashes = {
                upstream_name: libiconv["upstream_source_sha256"],
                libiconv["source_package"]: libiconv["source_package_sha256"],
            }
            command_results = [build.FFMPEG_SOURCE_REF, "", ""] + [
                "" for _ in (Path(build.PROJECT_ROOT) / "tools" / "patches" / "ffmpeg").glob("*.patch")
            ]
            with patch.object(build.shutil, "which", return_value="git"), patch.object(
                build, "run_command", side_effect=command_results
            ), patch.object(build, "normalize_custom_patch_targets", return_value=[]), patch.object(
                build, "sha256_file", side_effect=lambda path: hashes[Path(path).name]
            ):
                copied = build._stage_ffmpeg_corresponding_source(
                    str(source), str(downloads), str(destination)
                )

            self.assertIn("ffmpeg/configure", copied)
            self.assertIn("SOURCE_MANIFEST.txt", copied)
            self.assertTrue((destination / "captureengine-build-inputs" / "build.py").is_file())
            self.assertTrue((destination / "libiconv-source" / upstream_name).is_file())
            self.assertFalse((destination / "ffmpeg" / ".git").exists())

    def test_corresponding_source_refuses_a_dirty_ffmpeg_checkout(self) -> None:
        with tempfile.TemporaryDirectory() as temporary, patch.object(
            build.shutil, "which", return_value="git"
        ), patch.object(build, "run_command", side_effect=[build.FFMPEG_SOURCE_REF, " M libavcodec/example.c"]):
            with self.assertRaisesRegex(RuntimeError, "checkout is dirty"):
                build._stage_ffmpeg_corresponding_source(temporary, temporary, os.path.join(temporary, "stage"))

    def test_captureengine_staging_excludes_local_state_and_uses_clean_config(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "installed" / "captureengine"
            destination = root / "stage" / "captureengine"
            clean_config = root / "config.ini.template"
            (source / "ffmpeg").mkdir(parents=True)
            (source / "logs" / "session").mkdir(parents=True)
            (source / "bak").mkdir()
            (source / "captureengine.exe").write_bytes(b"exe")
            (source / "captureengine.pdb").write_bytes(b"pdb")
            (source / "ffmpeg" / "avcodec.dll").write_bytes(b"dll")
            (source / "logs" / "session" / "captureengine.log").write_text("private", encoding="utf-8")
            (source / "bak" / "config.ini").write_text("private", encoding="utf-8")
            (source / "config.ini").write_text("user-specific", encoding="utf-8")
            (source / "captureengine.exe.old.123").write_bytes(b"stale")
            (source / "nul").write_text("stale", encoding="utf-8")
            clean_config.write_text("clean-default", encoding="utf-8")

            copied = build._stage_captureengine_package(str(source), str(destination), str(clean_config))

            self.assertIn("captureengine.exe", copied)
            self.assertIn("ffmpeg/avcodec.dll", copied)
            self.assertEqual((destination / "config.ini").read_text(encoding="utf-8"), "clean-default")
            self.assertFalse((destination / "logs").exists())
            self.assertFalse((destination / "bak").exists())
            self.assertFalse((destination / "captureengine.exe.old.123").exists())
            self.assertNotIn("nul", copied)
            self.assertNotIn("nul", [path.name.lower() for path in destination.iterdir()])

    def test_testapp_staging_is_first_party_only_and_keeps_x86_separate(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "installed" / "testapp"
            destination = root / "stage" / "testapps"
            note = root / "THIRD_PARTY_RUNTIME_REQUIREMENTS.txt"
            config = root / "testappconfig.ini"
            (source / "x86").mkdir(parents=True)
            for relative in (
                "sample.exe",
                "sample.pdb",
                "x86/sample.exe",
                "x86/sample.pdb",
                "sl.interposer.dll",
                "sample.log",
                "sample.exe.link-cache.json",
            ):
                path = source / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(relative.encode("utf-8"))
            note.write_text("requirements", encoding="utf-8")
            config.write_text("default-config", encoding="utf-8")

            with patch.object(build, "PACKAGED_X64_TEST_APPS", ("sample",)), patch.object(
                build, "PACKAGED_X86_TEST_APPS", ("sample",)
            ), patch.object(build, "IS_WINDOWS", False):
                copied = build._stage_testapps_package(
                    str(source), str(destination), str(note), str(config)
                )

            self.assertEqual(
                copied,
                [
                    "THIRD_PARTY_RUNTIME_REQUIREMENTS.txt",
                    "sample.exe",
                    "sample.pdb",
                    "testappconfig.ini",
                    "x86/sample.exe",
                    "x86/sample.pdb",
                    "x86/testappconfig.ini",
                ],
            )
            self.assertEqual((destination / "testappconfig.ini").read_text(encoding="utf-8"), "default-config")
            self.assertEqual(
                (destination / "x86" / "testappconfig.ini").read_text(encoding="utf-8"), "default-config"
            )
            self.assertFalse(any(path.suffix.lower() == ".dll" for path in destination.rglob("*")))
            self.assertFalse((destination / "sample.log").exists())
            self.assertFalse((destination / "sample.exe.link-cache.json").exists())

    def test_testapp_staging_ships_default_config_only_for_staged_architectures(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "installed" / "testapp"
            destination = root / "stage" / "testapps"
            note = root / "THIRD_PARTY_RUNTIME_REQUIREMENTS.txt"
            config = root / "testappconfig.ini"
            (source / "x86").mkdir(parents=True)
            (source / "sample.exe").write_bytes(b"exe")
            (source / "x86" / "sample.exe").write_bytes(b"exe-x86")
            note.write_text("requirements", encoding="utf-8")
            config.write_text("default-config", encoding="utf-8")

            with patch.object(build, "PACKAGED_X64_TEST_APPS", ("sample",)), patch.object(
                build, "PACKAGED_X86_TEST_APPS", ()
            ), patch.object(build, "IS_WINDOWS", False):
                copied = build._stage_testapps_package(
                    str(source), str(destination), str(note), str(config)
                )

            self.assertIn("testappconfig.ini", copied)
            self.assertNotIn("x86/testappconfig.ini", copied)
            self.assertEqual((destination / "testappconfig.ini").read_text(encoding="utf-8"), "default-config")
            self.assertFalse((destination / "x86").exists())

    def test_testapp_staging_fails_closed_without_default_config(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "installed" / "testapp"
            destination = root / "stage" / "testapps"
            note = root / "THIRD_PARTY_RUNTIME_REQUIREMENTS.txt"
            missing_config = root / "missing" / "testappconfig.ini"
            source.mkdir(parents=True)
            (source / "sample.exe").write_bytes(b"exe")
            note.write_text("requirements", encoding="utf-8")

            with patch.object(build, "PACKAGED_X64_TEST_APPS", ("sample",)), patch.object(
                build, "PACKAGED_X86_TEST_APPS", ()
            ), patch.object(build, "IS_WINDOWS", False):
                with self.assertRaisesRegex(RuntimeError, "default config template is missing"):
                    build._stage_testapps_package(
                        str(source), str(destination), str(note), str(missing_config)
                    )

    def test_cmake_creates_a_verified_7z_with_one_root_folder(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            stage = root / "stage"
            package_root = stage / "captureengine"
            package_root.mkdir(parents=True)
            (package_root / "captureengine.exe").write_bytes(b"exe")
            archive = root / "captureengine.7z"

            members = build._create_7z_archive(
                build._get_cmake_archiver(),
                str(stage),
                "captureengine",
                str(archive),
            )

            self.assertTrue(archive.is_file())
            self.assertIn("captureengine/captureengine.exe", members)
            self.assertTrue(all(member == "captureengine" or member.startswith("captureengine/") for member in members))

    def test_testapps_archive_ships_default_config_in_both_folders(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "installed" / "testapp"
            stage = root / "stage"
            note = root / "THIRD_PARTY_RUNTIME_REQUIREMENTS.txt"
            config = root / "testappconfig.ini"
            archive = root / "testapps.7z"
            (source / "x86").mkdir(parents=True)
            for relative in ("sample.exe", "x86/sample.exe"):
                path = source / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(relative.encode("utf-8"))
            note.write_text("requirements", encoding="utf-8")
            config.write_text("default-config", encoding="utf-8")

            with patch.object(build, "PACKAGED_X64_TEST_APPS", ("sample",)), patch.object(
                build, "PACKAGED_X86_TEST_APPS", ("sample",)
            ), patch.object(build, "IS_WINDOWS", False):
                build._stage_testapps_package(
                    str(source), str(stage / "testapps"), str(note), str(config)
                )
                members = build._create_7z_archive(
                    build._get_cmake_archiver(), str(stage), "testapps", str(archive)
                )

            self.assertIn("testapps/testappconfig.ini", members)
            self.assertIn("testapps/x86/testappconfig.ini", members)

    def test_runtime_note_maps_every_requested_feature_to_root_dlls(self) -> None:
        note = Path(build.TESTAPP_RUNTIME_NOTE).read_text(encoding="utf-8")
        for required in (
            "Put every DLL listed below directly in this testapps folder",
            "amd_fidelityfx_framegeneration_dx12.dll",
            "amd_fidelityfx_upscaler_dx12.dll",
            "amd_fidelityfx_vk.dll",
            "sl.interposer.dll",
            "sl.dlss.dll",
            "sl.dlss_g.dll",
            "sl.reflex.dll",
            "nvngx_dlss.dll",
            "nvngx_dlssg.dll",
            "_nvngx.dll",
        ):
            self.assertIn(required, note)

    def test_product_build_packages_only_after_binary_verification(self) -> None:
        source = build.read_source_text()
        verify_index = source.index(
            'log("Verified PE mitigations, architecture, section permissions, effective CFG, imports, and PDBs")'
        )
        complete_index = source.index('log("Build Complete.")', verify_index)
        # The finalize tail no longer serializes packaging: privacy scrub and
        # PE verification stay in the finalize phase, while build_cli schedules
        # the archives only after the build step recorded and runs them
        # concurrently with the advisory lint pass.
        self.assertNotIn("package_build_outputs()", source[verify_index:complete_index])
        self.assertIn(
            'env.get("CE_SANITIZE") == "1" or ISOLATED_BUILD_ROOT', source[verify_index:complete_index]
        )
        build_step = source.index('record_verification_step(\n        "build",')
        submit_index = source.index(
            "package_future = package_executor.submit(package_build_outputs)", build_step
        )
        lint_call = source.index("run_lint(env, advisory=True)", submit_index)
        self.assertLess(submit_index, lint_call)
        self.assertIn("should_package_outputs(", source)

    def test_output_packaging_policy_excludes_non_shippable_runs(self) -> None:
        self.assertTrue(
            build.should_package_outputs(
                tests_only=False,
                no_build=False,
                sanitize=False,
                isolated_root=False,
                skip_package=False,
            )
        )
        for excluded in (
            {"tests_only": True},
            {"no_build": True},
            {"sanitize": True},
            {"isolated_root": True},
            {"skip_package": True},
        ):
            kwargs = {
                "tests_only": False,
                "no_build": False,
                "sanitize": False,
                "isolated_root": False,
                "skip_package": False,
            }
            kwargs.update(excluded)
            self.assertFalse(build.should_package_outputs(**kwargs))

    def test_package_cleanup_is_scoped_to_workspace_temp(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            workspace_temp = Path(temporary) / "workspace-temp"
            inside = workspace_temp / "package-staging"
            outside = Path(temporary) / "outside"
            with patch.object(build, "WORKSPACE_TEMP_DIR", str(workspace_temp)):
                self.assertEqual(build._validate_workspace_cleanup_target(str(inside)), os.path.abspath(inside))
                with self.assertRaisesRegex(RuntimeError, "outside the workspace temp"):
                    build._validate_workspace_cleanup_target(str(outside))
                with self.assertRaisesRegex(RuntimeError, "outside the workspace temp"):
                    build._validate_workspace_cleanup_target(str(workspace_temp))


if __name__ == "__main__":
    unittest.main()
