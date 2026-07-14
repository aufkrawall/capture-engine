import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import ffmpeg_dependencies as dependencies


class FfmpegDependencyManifestTest(unittest.TestCase):
    def test_manifest_has_pinned_build_order_and_versions(self) -> None:
        manifest_path = Path(__file__).with_name("ffmpeg_dependencies.json")
        manifest = dependencies.load_dependency_manifest(str(manifest_path))
        self.assertEqual(manifest["toolchain_version"], "22.1.8")
        self.assertEqual(
            [dependency["name"] for dependency in manifest["dependencies"]],
            ["llvm-runtime", "libiconv", "opus", "libva", "onevpl", "libwinpthread", "aom", "svt-av1"],
        )
        self.assertIn("libaom.dll", dependencies.manifest_runtime_dlls(manifest))
        self.assertIn("libwinpthread-1.dll", dependencies.manifest_runtime_dlls(manifest))
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

    def test_manifest_fingerprint_changes_with_build_policy(self) -> None:
        manifest_path = Path(__file__).with_name("ffmpeg_dependencies.json")
        original = dependencies.dependency_manifest_fingerprint(str(manifest_path))
        with mock.patch.object(
            dependencies,
            "DEPENDENCY_BUILD_CONFIGURATION_VERSION",
            dependencies.DEPENDENCY_BUILD_CONFIGURATION_VERSION + 1,
        ):
            changed = dependencies.dependency_manifest_fingerprint(str(manifest_path))
        self.assertNotEqual(original, changed)

    def test_ffmpeg_libaom_component_and_cache_version_are_current(self) -> None:
        build_source = Path(__file__).with_name("build.py").read_text(encoding="utf-8")
        self.assertIn('"--enable-encoder=libaom_av1"', build_source)
        self.assertIn('"--enable-decoder=libaom_av1"', build_source)
        self.assertNotIn('"--enable-encoder=libaom-av1"', build_source)
        self.assertIn("FFMPEG_BUILD_CONFIGURATION_VERSION = 7", build_source)


class FfmpegDependencyPeHelperTest(unittest.TestCase):
    def test_removes_read_only_extracted_package_tree(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            extracted_tree = Path(temp_dir) / "recipe"
            read_only_object = extracted_tree / ".git" / "objects" / "00" / "object"
            read_only_object.parent.mkdir(parents=True)
            read_only_object.write_bytes(b"object")
            read_only_object.chmod(0o444)

            dependencies.remove_tree(str(extracted_tree))

            self.assertFalse(extracted_tree.exists())

    def test_injects_policy_after_makepkg_configuration(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            pkgbuild_path = Path(temp_dir) / "PKGBUILD"
            pkgbuild_path.write_text("build() {\n  true\n}\n", encoding="utf-8")
            dependencies.inject_dependency_build_policy(
                str(pkgbuild_path),
                "/c/private dependency prefix",
                "/clang64/lib",
            )
            content = pkgbuild_path.read_text(encoding="utf-8")
            self.assertIn(dependencies.DEPENDENCY_BUILD_POLICY_MARKER, content)
            self.assertIn("-march=x86-64", content)
            self.assertIn("-mguard=cf", content)
            self.assertIn("-Wl,--guard-cf", content)
            self.assertIn("PKG_CONFIG_PATH=", content)
            with self.assertRaises(dependencies.DependencyBuildError):
                dependencies.inject_dependency_build_policy(
                    str(pkgbuild_path),
                    "/c/private dependency prefix",
                    "/clang64/lib",
                )

    def test_requires_effective_guard_cf_metadata(self) -> None:
        output = """
        IMAGE_DLL_CHARACTERISTICS_GUARD_CF (0x4000)
        GuardCFFunctionTable: 0x180001000
        GuardCFFunctionCount: 17
        CF_FUNCTION_TABLE_PRESENT (0x400)
        """
        self.assertEqual(dependencies.parse_guard_cf_function_count(output), 17)
        with self.assertRaises(dependencies.DependencyBuildError):
            dependencies.parse_guard_cf_function_count(output.replace("GuardCFFunctionCount: 17", "GuardCFFunctionCount: 0"))

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
