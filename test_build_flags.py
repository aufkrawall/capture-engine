import unittest
import tempfile
from pathlib import Path

import build


class BuildFlagPolicyTest(unittest.TestCase):
    def test_testapp_policy_limits_cfg_exception_to_x86(self) -> None:
        x86_arch_flags = [
            "--target=i686-w64-mingw32",
            "--sysroot=" + build.MSYS2_DIR + "\\mingw32",
        ]

        x86_flags = build.make_cpp_cflags(
            build.TESTAPP_OPT_FLAGS_X86,
            arch_flags=x86_arch_flags,
            enable_cfg=False,
        )
        x64_flags = build.make_cpp_cflags(build.TESTAPP_OPT_FLAGS_X64)

        self.assertNotIn(build.CFG_COMPILE_FLAG, x86_flags)
        self.assertIn(build.CFG_COMPILE_FLAG, x64_flags)
        for flags in (x86_flags, x64_flags):
            self.assertIn("-fstack-protector-strong", flags)
            self.assertIn("-D_FORTIFY_SOURCE=2", flags)
        self.assertIn("-fcf-protection=full", x64_flags)
        self.assertNotIn("-fcf-protection=full", x86_flags)

    def test_captureengine_x64_policy_still_has_cfg(self) -> None:
        captureengine_flags = build.make_cpp_cflags(build.OPT_FLAGS_X64)
        self.assertIn(build.CFG_COMPILE_FLAG, captureengine_flags)

    def test_cfg_link_flag_is_x64_only(self) -> None:
        self.assertNotIn(build.CFG_LINK_FLAG, build.LD_OPT_FLAGS)
        self.assertIn(build.CFG_LINK_FLAG, build.LD_OPT_FLAGS_X64)
        self.assertEqual(build.TESTAPP_X86_CFG_LINK_FLAGS, ["-Wl,--no-guard-cf"])

    def test_ffmpeg_policy_includes_stack_and_object_size_hardening(self) -> None:
        with open(build.__file__, encoding="utf-8") as build_file:
            source = build_file.read()
        self.assertIn("-mguard=cf -fstack-protector-strong -D_FORTIFY_SOURCE=2", source)

    def test_plain_build_forces_ffmpeg_source_closure_rebuild(self) -> None:
        with open(build.__file__, encoding="utf-8") as build_file:
            source = build_file.read()
        self.assertIn("full_source_rebuild = not skip_updates", source)
        self.assertIn("dependency_builder.ensure(force_rebuild=full_source_rebuild)", source)
        self.assertIn("needs_rebuild = full_source_rebuild", source)

    def test_sanitizer_child_reuses_parent_build_version(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            header = Path(temporary) / "build_version.h"
            header.write_text("#define BUILD_NUMBER 1234\n", encoding="utf-8")
            self.assertEqual(build.read_build_version_number(str(header)), 1234)

        with open(build.__file__, encoding="utf-8") as build_file:
            source = build_file.read()
        self.assertIn("if sanitize_regression_child:", source)
        self.assertIn("current_build_number = read_build_version_number()", source)

    def test_windows_sdk_headers_are_in_safe_include_order(self) -> None:
        project_root = Path(build.__file__).parent
        format_config = (project_root / ".clang-format").read_text(encoding="utf-8")
        self.assertIn("SortIncludes: Never", format_config)
        for relative_path, dependent_header in (
            ("common/module_enumeration.h", "#include <psapi.h>"),
            ("tests/test_process_ipc.cpp", "#include <shellapi.h>"),
        ):
            lines = (project_root / relative_path).read_text(
                encoding="utf-8"
            ).splitlines()
            windows_index = lines.index("#include <windows.h>")
            dependent_index = lines.index(dependent_header)
            self.assertLess(windows_index, dependent_index, relative_path)

    def test_vulkan_fg_embedded_sources_keep_dependency_order(self) -> None:
        project_root = Path(build.__file__).parent
        source = (project_root / "testapp/vulkan_fg_switch_test.cpp").read_text(encoding="utf-8")
        self.assertLess(
            source.index('#include "vulkan_fg_switch_wsi.inl"'),
            source.index('#include "vulkan_fg_switch_resources.inl"'),
        )

    def test_clang_tidy_excludes_external_and_generated_headers(self) -> None:
        config = (Path(build.__file__).parent / ".clang-tidy").read_text(encoding="utf-8")
        self.assertIn("HeaderFilterRegex:", config)
        self.assertIn("ExcludeHeaderFilterRegex:", config)
        self.assertIn("external|build|installed|ffmpeg_build", config)

    def test_vulkan_layer_build_has_no_registry_side_effects(self) -> None:
        with open(build.__file__, encoding="utf-8") as build_file:
            source = build_file.read()
        self.assertNotIn("cleanup_vulkan_layer_registry", source)
        self.assertNotIn("import winreg", source)
        self.assertNotIn("winreg.", source)


if __name__ == "__main__":
    unittest.main()
