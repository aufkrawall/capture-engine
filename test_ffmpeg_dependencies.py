import os
import tempfile
import unittest
from pathlib import Path

import ffmpeg_dependencies as dependencies


class FfmpegDependencyManifestTest(unittest.TestCase):
    def test_manifest_has_pinned_build_order_and_versions(self) -> None:
        manifest_path = Path(__file__).with_name("ffmpeg_dependencies.json")
        manifest = dependencies.load_dependency_manifest(str(manifest_path))
        self.assertEqual(manifest["toolchain_version"], "22.1.8")
        self.assertEqual(
            [dependency["name"] for dependency in manifest["dependencies"]],
            ["llvm-runtime", "libiconv", "opus", "libva", "onevpl", "svt-av1"],
        )
        self.assertIn("libiconv-2.dll", dependencies.manifest_runtime_dlls(manifest))
        self.assertIn("libcharset-1.dll", dependencies.manifest_runtime_dlls(manifest, optional=True))
        self.assertNotIn("libcharset-1.dll", dependencies.manifest_runtime_dlls(manifest))
        for dependency in manifest["dependencies"]:
            self.assertEqual(len(dependency["source_package_pgp_keys"]), 1)
            self.assertEqual(
                dependency["source_package_pgp_keys"][0],
                "5F944B027F7FE2091985AA2EFA11531AA0AA7F57",
            )
            self.assertTrue(dependency["source_package_signature_url"].endswith(".src.tar.zst.sig"))

    def test_manifest_fingerprint_is_stable(self) -> None:
        manifest_path = Path(__file__).with_name("ffmpeg_dependencies.json")
        self.assertEqual(
            dependencies.dependency_manifest_fingerprint(str(manifest_path)),
            dependencies.dependency_manifest_fingerprint(str(manifest_path)),
        )


class FfmpegDependencyPeHelperTest(unittest.TestCase):
    def test_parses_imported_dll_names_case_insensitively(self) -> None:
        output = """
        DLL Name: KERNEL32.dll
        DLL Name: libcharset-1.dll
        DLL name: image.dll
        """
        self.assertEqual(
            dependencies.parse_pe_import_names(output),
            {"kernel32.dll", "libcharset-1.dll", "image.dll"},
        )

    def test_system_dll_allowlist_does_not_allow_msys_runtime(self) -> None:
        self.assertTrue(dependencies.is_windows_system_dll("KERNEL32.dll"))
        self.assertTrue(dependencies.is_windows_system_dll("api-ms-win-crt-runtime-l1-1-0.dll"))
        self.assertFalse(dependencies.is_windows_system_dll("libgcc_s_seh-1.dll"))
        self.assertFalse(dependencies.is_windows_system_dll("libcharset-1.dll"))

    def test_selects_the_latest_matching_package_archive(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            old_archive = Path(temp_dir) / "mingw-w64-clang-x86_64-opus-1.6.1-1-any.pkg.tar.zst"
            new_archive = Path(temp_dir) / "mingw-w64-clang-x86_64-opus-1.6.1-1-any-2.pkg.tar.zst"
            docs_archive = Path(temp_dir) / "mingw-w64-clang-x86_64-opus-docs-1.6.1-1-any.pkg.tar.zst"
            old_archive.touch()
            new_archive.touch()
            docs_archive.touch()
            os.utime(old_archive, (1, 1))
            os.utime(new_archive, (2, 2))
            os.utime(docs_archive, (3, 3))
            self.assertEqual(
                dependencies.select_package_archive(
                    temp_dir,
                    "mingw-w64-clang-x86_64-opus",
                    "1.6.1",
                ),
                str(new_archive),
            )


if __name__ == "__main__":
    unittest.main()
