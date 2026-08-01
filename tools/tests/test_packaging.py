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

            with patch.object(build, "PACKAGED_X64_TEST_APPS", ("sample",)), patch.object(
                build, "PACKAGED_X86_TEST_APPS", ("sample",)
            ), patch.object(build, "IS_WINDOWS", False):
                copied = build._stage_testapps_package(str(source), str(destination), str(note))

            self.assertEqual(
                copied,
                [
                    "THIRD_PARTY_RUNTIME_REQUIREMENTS.txt",
                    "sample.exe",
                    "sample.pdb",
                    "x86/sample.exe",
                    "x86/sample.pdb",
                ],
            )
            self.assertFalse(any(path.suffix.lower() == ".dll" for path in destination.rglob("*")))
            self.assertFalse((destination / "sample.log").exists())
            self.assertFalse((destination / "sample.exe.link-cache.json").exists())

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
        package_index = source.index("package_build_outputs()", verify_index)
        complete_index = source.index('log("Build Complete.")', package_index)
        self.assertLess(verify_index, package_index)
        self.assertLess(package_index, complete_index)
        self.assertIn('env.get("CE_SANITIZE") == "1" or ISOLATED_BUILD_ROOT', source[verify_index:package_index])

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
