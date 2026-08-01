# build.py executes its fragments via exec, so its module attributes exist only
# at runtime; pyright cannot see them through the facade.
# pyright: reportAttributeAccessIssue=false

import json
import os
import tempfile
import time
import unittest
import zipfile
from unittest.mock import patch
from pathlib import Path

import build
from testapp import run_tests as integration_runner


class BuildFlagPolicyTest(unittest.TestCase):
    def test_amf_header_license_is_packaged_without_a_runtime_dll(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            licenses = root / "licenses"
            ffmpeg = root / "ffmpeg"
            amf_license = root / "msys64/clang64/share/licenses/amf-headers/LICENSE"
            licenses.mkdir()
            ffmpeg.mkdir()
            amf_license.parent.mkdir(parents=True)
            amf_license.write_text("AMF license and standards notice\n", encoding="utf-8")
            (ffmpeg / "avcodec-63.dll").write_bytes(b"test")

            with patch.object(build, "get_host_msys2_dir", return_value=str(root / "msys64")), patch.object(
                build, "get_msys_license_root", return_value=str(root / "runtime-licenses")
            ):
                build.copy_bundled_runtime_licenses(str(licenses), str(ffmpeg))

            self.assertEqual(
                (licenses / "MIT_AMF-Headers.txt").read_text(encoding="utf-8"),
                "AMF license and standards notice\n",
            )

    def test_missing_amf_header_license_fails_packaging_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            licenses = root / "licenses"
            ffmpeg = root / "ffmpeg"
            licenses.mkdir()
            ffmpeg.mkdir()
            (ffmpeg / "avcodec-63.dll").write_bytes(b"test")

            with patch.object(build, "get_host_msys2_dir", return_value=str(root / "missing-msys64")), patch.object(
                build, "get_msys_license_root", return_value=str(root / "runtime-licenses")
            ):
                with self.assertRaisesRegex(RuntimeError, "Missing bundled build-time license source"):
                    build.copy_bundled_runtime_licenses(str(licenses), str(ffmpeg))

    def test_ffmpeg_notice_documents_vendor_runtime_boundary(self) -> None:
        notice = (Path(build.__file__).parent / "licenses/FFmpeg_NOTICE.txt").read_text(encoding="utf-8")
        self.assertIn("MIT_ffnvcodec.txt", notice)
        self.assertIn("nvEncodeAPI64.dll", notice)
        self.assertIn("MIT_AMF-Headers.txt", notice)
        self.assertIn("amfrt64.dll", notice)
        self.assertIn("MIT_libvpl.txt", notice)
        self.assertIn("libmfx64-gen.dll", notice)
        self.assertIn("standards and patent disclaimer", notice)

    def test_ffnvcodec_license_file_exists(self) -> None:
        license_file = Path(build.__file__).parent / "licenses" / "MIT_ffnvcodec.txt"
        self.assertTrue(license_file.exists(), f"Missing {license_file}")
        content = license_file.read_text(encoding="utf-8")
        self.assertIn("NVIDIA Corporation", content)
        self.assertIn("MIT License", content)

    def test_obsolete_process_loopback_helper_outputs_are_removed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            installed = Path(temporary) / "installed"
            objects = Path(temporary) / "build" / "obj" / "x64"
            installed.mkdir(parents=True)
            objects.mkdir(parents=True)
            stale_outputs = (
                installed / "process_loopback_helper.exe",
                installed / "process_loopback_helper.pdb",
                objects / "process_loopback_helper.tmp.exe",
                objects / "process_loopback_helper.pdb.old.1.1234",
            )
            for output in stale_outputs:
                output.write_bytes(b"stale")

            roots = (str(installed), str(Path(temporary) / "build"))
            build.remove_obsolete_process_loopback_helper_artifacts(roots)

            self.assertEqual(build.find_obsolete_process_loopback_helper_artifacts(roots), [])
            self.assertTrue(all(not output.exists() for output in stale_outputs))

    def test_locked_obsolete_process_loopback_helper_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            stale = Path(temporary) / "process_loopback_helper.exe"
            stale.write_bytes(b"locked")
            with patch.object(build, "safe_delete_file", return_value=False):
                with self.assertRaisesRegex(RuntimeError, "Obsolete process-loopback helper artifact remains"):
                    build.remove_obsolete_process_loopback_helper_artifacts((temporary,))
            self.assertTrue(stale.exists())
            with self.assertRaisesRegex(RuntimeError, "Obsolete process-loopback helper artifact present"):
                build.assert_no_obsolete_process_loopback_helper_artifacts((temporary,))

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
        self.assertFalse(any(flag.startswith("-flto") for flag in build.TESTAPP_OPT_FLAGS_X64))

    def test_validation_binaries_do_not_change_product_lto_policy(self) -> None:
        self.assertIn("-flto", build.OPT_FLAGS_X64)
        self.assertIn("-flto", build.HOOK_OPT_FLAGS_X64)
        self.assertFalse(any(flag.startswith("-flto") for flag in build.UNIT_TEST_OPT_FLAGS_X64))
        self.assertFalse(any(flag.startswith("-flto") for flag in build.TESTAPP_OPT_FLAGS_X64))

    def test_captureengine_x64_policy_still_has_cfg(self) -> None:
        captureengine_flags = build.make_cpp_cflags(build.OPT_FLAGS_X64)
        self.assertIn(build.CFG_COMPILE_FLAG, captureengine_flags)

    def test_independent_architecture_environments_inherit_force_rebuild(self) -> None:
        target = {"PATH": "x86-tools"}
        build.propagate_build_control_environment(
            {"FORCE_REBUILD": "1", "CE_BUILD_JOBS": "7", "CE_PRODUCTION_BUILD": "1"}, target
        )
        self.assertEqual(target["FORCE_REBUILD"], "1")
        self.assertEqual(target["CE_BUILD_JOBS"], "7")
        self.assertEqual(target["CE_PRODUCTION_BUILD"], "1")

    def test_unit_test_objects_are_isolated_from_product_and_sanitizer_objects(self) -> None:
        product_dir = os.path.normpath(os.path.join(build.OBJ_DIR, "x64"))
        test_dir = os.path.normpath(build.get_unit_test_object_dir({}))
        sanitizer_dir = os.path.normpath(build.get_unit_test_object_dir({"CE_SANITIZE": "1"}))

        self.assertNotEqual(test_dir, product_dir)
        self.assertNotEqual(sanitizer_dir, product_dir)
        self.assertNotEqual(sanitizer_dir, test_dir)

    def test_cfg_link_flag_is_x64_only(self) -> None:
        self.assertNotIn(build.CFG_LINK_FLAG, build.LD_OPT_FLAGS)
        self.assertIn(build.CFG_LINK_FLAG, build.LD_OPT_FLAGS_X64)
        self.assertEqual(build.TESTAPP_X86_CFG_LINK_FLAGS, ["-Wl,--no-guard-cf"])

    def test_linux_mingw_gcc_uses_only_supported_hardening_flags(self) -> None:
        gcc = "/usr/bin/x86_64-w64-mingw32-g++"
        flags = build.make_cpp_cflags(
            build.OPT_FLAGS_X64,
            compiler_exe=gcc,
            suppress_microsoft_exception_spec=True,
        )

        self.assertNotIn(build.CFG_COMPILE_FLAG, flags)
        self.assertNotIn("-Wno-microsoft-exception-spec", flags)
        self.assertNotIn(build.CFG_LINK_FLAG, build.get_x64_linker_flags(gcc))
        self.assertNotIn("-Wl,--no-guard-cf", build.get_x86_testapp_cfg_link_flags(gcc))
        self.assertNotIn("-ffp-model=strict", build.get_strict_fp_flags(gcc))
        self.assertIn("-ffp-contract=off", build.get_strict_fp_flags(gcc))
        self.assertIn("-fstack-protector-strong", flags)
        self.assertIn("-D_FORTIFY_SOURCE=2", flags)
        self.assertIn("-fcf-protection=full", flags)
        self.assertIn("-Wl,--high-entropy-va", build.get_x64_linker_flags(gcc))

    def test_linux_mingw_sources_do_not_require_newer_d3d12_header_helpers(self) -> None:
        project_root = Path(build.__file__).parent
        sampler_source = (project_root / "hook/common/dx12_sampler_policy.cpp").read_text(encoding="utf-8")
        dred_source = (project_root / "hook/common/dx12_dred.cpp").read_text(encoding="utf-8")
        fg_resource_source = (project_root / "testapp/dx12_fg_resources.h").read_text(encoding="utf-8")
        fg_dred_source = (project_root / "testapp/dx12_fg_switch_dred.inl").read_text(encoding="utf-8")
        overlay_sources = (
            (project_root / "hook/apis/dx12_streamline_ui_overlay.cpp").read_text(encoding="utf-8"),
            (project_root / "hook/apis/dx12_ffx_suspend_overlay.cpp").read_text(encoding="utf-8"),
        )

        self.assertFalse(
            any(
                "D3D12_ENCODE_BASIC_FILTER(" in line and not line.lstrip().startswith("//")
                for line in sampler_source.splitlines()
            )
        )
        self.assertIn("constexpr D3D12_FILTER EncodeBasicFilter", sampler_source)
        self.assertIn("#if CE_HAS_D3D12_DRED_SETTINGS1", dred_source)
        self.assertIn("#if CE_HAS_D3D12_DRED_DATA1", dred_source)
        self.assertIn("ID3D12DeviceRemovedExtendedData* dred", dred_source)
        self.assertNotIn(
            "D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE",
            fg_resource_source,
        )
        self.assertGreaterEqual(fg_resource_source.count("static_cast<UINT>(D3D12_RESOURCE_STATE_"), 4)
        self.assertIn("#if CE_TESTAPP_HAS_D3D12_DRED_SETTINGS1", fg_dred_source)
        self.assertIn("#elif CE_TESTAPP_HAS_D3D12_DRED_SETTINGS", fg_dred_source)
        self.assertIn("#elif CE_TESTAPP_HAS_D3D12_DRED_DATA", fg_dred_source)
        self.assertIn("Compiler headers expose no DRED data interface", fg_dred_source)
        self.assertIn("static const char* DredOpName(UINT op)", fg_dred_source)
        self.assertNotIn("case D3D12_AUTO_BREADCRUMB_OP_", fg_dred_source)
        for source in overlay_sources:
            self.assertNotIn(
                "D3D12_FORMAT_SUPPORT1_RENDER_TARGET | D3D12_FORMAT_SUPPORT1_BLENDABLE",
                source,
            )

    def test_sequence_lock_declares_pause_intrinsic_without_transitive_headers(self) -> None:
        project_root = Path(build.__file__).parent
        source = (project_root / "common/sequence_lock.h").read_text(encoding="utf-8")

        self.assertIn("#include <intrin.h>  // for _mm_pause", source)

    def test_native_clang_keeps_cfg_and_strict_fp_policy(self) -> None:
        clang = r"C:\msys64\clang64\bin\clang++.exe"
        flags = build.make_cpp_cflags(build.OPT_FLAGS_X64, compiler_exe=clang)

        self.assertIn(build.CFG_COMPILE_FLAG, flags)
        self.assertIn(build.CFG_LINK_FLAG, build.get_x64_linker_flags(clang))
        self.assertEqual(build.get_x86_testapp_cfg_link_flags(clang), build.TESTAPP_X86_CFG_LINK_FLAGS)
        self.assertEqual(build.get_strict_fp_flags(clang), build.CLANG_STRICT_FP_FLAGS)

    def test_every_screenshot_encoding_source_is_registered_for_strict_floating_point(self) -> None:
        # screenshot_encoding_internal.h is what defines the screenshot encoding
        # module, so every translation unit that includes it carries part of the
        # HDR conversion and must be compiled with -ffp-model=strict. Splitting
        # one of these files without registering the new half would silently drop
        # strict FP from the moved code, which no other gate would notice.
        captureengine = Path(build.PROJECT_ROOT) / "captureengine"
        module_sources = {
            source.name
            for source in captureengine.glob("*.cpp")
            if "screenshot_encoding_internal.h" in source.read_text(encoding="utf-8")
        }

        self.assertTrue(module_sources, "no screenshot encoding sources found")
        self.assertEqual(module_sources, build.STRICT_FP_SCREENSHOT_SOURCES)
        for name in build.STRICT_FP_SCREENSHOT_SOURCES:
            self.assertTrue((captureengine / name).is_file(), f"{name} is registered but does not exist")

    def test_linux_llvm_readobj_resolution_uses_host_executable(self) -> None:
        with patch.object(build, "IS_LINUX", True), patch.object(
            build.shutil,
            "which",
            side_effect=lambda executable: "/usr/bin/llvm-readobj" if executable == "llvm-readobj" else None,
        ):
            self.assertEqual(build.get_llvm_readobj_exe(), "/usr/bin/llvm-readobj")

    def test_linux_vulkan_shader_tools_are_host_native_and_installed_by_ci(self) -> None:
        executables = {
            "glslangValidator": "/usr/bin/glslangValidator",
            "spirv-val": "/usr/bin/spirv-val",
        }
        with patch.object(build, "IS_LINUX", True), patch.object(
            build.shutil,
            "which",
            side_effect=lambda executable: executables.get(executable),
        ):
            self.assertEqual(
                build.get_vulkan_fg_shader_tools(),
                ("/usr/bin/glslangValidator", "/usr/bin/spirv-val"),
            )

        with patch.object(build, "IS_LINUX", True), patch.object(build.shutil, "which", return_value=None):
            with self.assertRaisesRegex(RuntimeError, "glslang-tools.*spirv-tools"):
                build.get_vulkan_fg_shader_tools()

        workflow = (Path(build.__file__).parent / ".github/workflows/hardening-ci.yml").read_text(
            encoding="utf-8"
        )
        self.assertIn("glslang-tools", workflow)
        self.assertIn("spirv-tools", workflow)

    def test_parallel_compile_rejects_duplicate_object_outputs_before_spawning_workers(self) -> None:
        tasks = [([], "first.cpp", "shared.o"), ([], "second.cpp", "shared.o")]
        with self.assertRaisesRegex(RuntimeError, "same object.*first.cpp.*second.cpp"):
            build.parallel_compile_varied({}, "unused-compiler", tasks)

    def test_linux_requires_x64_vulkan_layer_inputs_and_propagates_link_failures(self) -> None:
        with patch.object(build, "IS_LINUX", True), patch.object(
            build, "get_linux_vulkan_import_lib_path", return_value=None
        ):
            with self.assertRaisesRegex(RuntimeError, r"required Vulkan Layer \(x64\)"):
                build.compile_vulkan_layer({}, "/usr/bin/x86_64-w64-mingw32-g++", [], "x64")
            self.assertIsNone(build.compile_vulkan_layer({}, "/usr/bin/i686-w64-mingw32-g++", [], "x86"))

        source = build.read_source_text()
        self.assertIn("Vulkan import library unavailable for required x64 test applications", source)
        self.assertIn('if IS_LINUX and arch == "x86":\n                log(f"Linux host: skipping Hook DLL', source)
        self.assertIn('log(f"Error linking layer: {e}")\n        raise', source)

    def test_fg_sdk_host_setup_keeps_linux_header_only(self) -> None:
        with (
            patch.object(build, "IS_LINUX", True),
            patch.object(build, "setup_fg_sdk_headers") as headers,
            patch.object(build, "setup_fg_sdk_dlls") as runtime,
        ):
            build.setup_fg_sdk_for_host(skip_updates=True)
            headers.assert_called_once_with(skip_updates=True)
            runtime.assert_not_called()

        with (
            patch.object(build, "IS_LINUX", False),
            patch.object(build, "setup_fg_sdk_headers") as headers,
            patch.object(build, "setup_fg_sdk_dlls") as runtime,
        ):
            build.setup_fg_sdk_for_host(skip_updates=True)
            headers.assert_not_called()
            runtime.assert_called_once_with(skip_updates=True)

    def test_fg_sdk_header_setup_extracts_cross_compile_inputs_without_runtime_payloads(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cache = root / "cache"
            includes = root / "include"
            testapps = root / "testapp"
            cache.mkdir()

            streamline_zip = cache / build.STREAMLINE_SDK_ZIP_NAME
            with zipfile.ZipFile(streamline_zip, "w") as archive:
                archive.writestr("include/sl.h", "// Streamline header\n")
                archive.writestr("bin/x64/sl.interposer.dll", b"not a PE")

            fidelityfx_zip = cache / build.FFX_SDK_SOURCE_ZIP_NAME
            with zipfile.ZipFile(fidelityfx_zip, "w") as archive:
                prefix = "FidelityFX-SDK-2.2.0/Kits/FidelityFX/"
                archive.writestr(prefix + "api/include/ffx_api/ffx_api.h", "// FFX API\n")
                archive.writestr(prefix + "api/include/dx12/ffx_api_dx12.h", "// FFX DX12 API\n")
                archive.writestr(prefix + "framegeneration/include/ffx_framegeneration.h", "// FFX FG\n")
                archive.writestr(prefix + "upscalers/include/ffx_upscale.h", "// FFX upscale\n")

            fidelityfx_vk_zip = cache / build.FFX_VK_SDK_ZIP_NAME
            with zipfile.ZipFile(fidelityfx_vk_zip, "w") as archive:
                archive.writestr("ffx-api/include/ffx_api/ffx_api.h", "// Vulkan FFX API\n")
                archive.writestr("ffx-api/include/ffx_api/vk/ffx_api_vk.h", "// Vulkan backend\n")
                archive.writestr("PrebuiltSignedDLL/amd_fidelityfx_vk.dll", b"not a PE")
                archive.writestr("sdk/LICENSE.txt", "test license\n")

            with patch.object(build, "FG_SDK_CACHE_DIR", str(cache)), patch.object(
                build, "FG_SDK_INCLUDE_DIR", str(includes)
            ), patch.object(build, "TESTAPP_BIN_DIR", str(testapps)), patch.object(
                build, "FFX_VK_SDK_SHA256", build.sha256_file(str(fidelityfx_vk_zip))
            ), patch.object(
                build.urllib.request, "urlretrieve", side_effect=AssertionError("unexpected download")
            ), patch.object(
                build, "pe_has_authenticode_certificate", side_effect=AssertionError("unexpected PE inspection")
            ), patch.object(
                build, "_find_nvngx_driverstore_paths", side_effect=AssertionError("unexpected driver scan")
            ):
                build.setup_fg_sdk_headers(skip_updates=True)

            expected_headers = (
                includes / "streamline/include/sl.h",
                includes / "fidelityfx/Kits/FidelityFX/api/include/ffx_api/ffx_api.h",
                includes / "fidelityfx/Kits/FidelityFX/api/include/dx12/ffx_api_dx12.h",
                includes / "fidelityfx/Kits/FidelityFX/framegeneration/include/ffx_framegeneration.h",
                includes / "fidelityfx/Kits/FidelityFX/upscalers/include/ffx_upscale.h",
                includes / "fidelityfx_vk_v1_1_4/ffx_api/ffx_api.h",
                includes / "fidelityfx_vk_v1_1_4/ffx_api/vk/ffx_api_vk.h",
            )
            self.assertTrue(all(header.is_file() for header in expected_headers))
            self.assertFalse(testapps.exists())

    def test_ffmpeg_policy_includes_stack_and_object_size_hardening(self) -> None:
        source = build.read_source_text()
        self.assertIn("-mguard=cf -fstack-protector-strong -D_FORTIFY_SOURCE=2", source)

    def test_plain_build_forces_ffmpeg_source_closure_rebuild(self) -> None:
        source = build.read_source_text()
        self.assertIn("full_source_rebuild = not skip_updates", source)
        self.assertIn("dependency_builder.ensure(force_rebuild=full_source_rebuild)", source)
        self.assertIn("needs_rebuild = full_source_rebuild", source)

    def test_compile_signature_tracks_project_header_contents(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "source.cpp"
            header = Path(temporary) / "shared_layout.h"
            compiler = Path(temporary) / "clang++.exe"
            source.write_text('#include "shared_layout.h"\n', encoding="utf-8")
            header.write_text("#define SHARED_VERSION 33\n", encoding="utf-8")
            compiler.write_bytes(b"compiler")

            with patch.object(build, "PROJECT_ROOT", temporary):
                build.compute_file_content_hash.cache_clear()
                build.compute_compiler_fingerprint.cache_clear()
                original = build.compute_build_signature(
                    str(source), str(compiler), ["-std=c++20"], [str(header)]
                )
                header.write_text("#define SHARED_VERSION 34\n", encoding="utf-8")
                os.utime(header, (1, 1))
                build.compute_file_content_hash.cache_clear()
                changed = build.compute_build_signature(
                    str(source), str(compiler), ["-std=c++20"], [str(header)]
                )

            self.assertNotEqual(original, changed)

    def test_compile_signature_tracks_compiler_contents(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "source.cpp"
            compiler = Path(temporary) / "clang++.exe"
            source.write_text("int value = 1;\n", encoding="utf-8")
            compiler.write_bytes(b"compiler-a")

            build.compute_compiler_fingerprint.cache_clear()
            original = build.compute_build_signature(str(source), str(compiler), ["-std=c++20"])
            compiler.write_bytes(b"compiler-b")
            os.utime(compiler, (1, 1))
            build.compute_compiler_fingerprint.cache_clear()
            changed = build.compute_build_signature(str(source), str(compiler), ["-std=c++20"])

            self.assertNotEqual(original, changed)

    def test_generated_build_version_isolated_from_fanout_headers(self) -> None:
        project_root = Path(build.__file__).parent
        for relative_path in ("common/shared_defs.h", "common/config.h"):
            source = (project_root / relative_path).read_text(encoding="utf-8")
            self.assertNotIn('"build_version.h"', source, relative_path)

        identity_source = (project_root / "common/build_identity.cpp").read_text(encoding="utf-8")
        self.assertIn('#include "build_version.h"', identity_source)
        build_source = build.read_source_text()
        self.assertIn('os.path.join(PROJECT_ROOT, "common", "build_identity.cpp")', build_source)

    def test_incremental_signature_failure_recompiles_instead_of_trusting_timestamps(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "source.cpp"
            obj = Path(temporary) / "source.o"
            dep = Path(str(obj) + ".d")
            source.write_text("int value = 1;\n", encoding="utf-8")
            obj.write_bytes(b"object")
            dep.write_text(f"{obj}: {source}\n", encoding="utf-8")
            os.utime(source, (1, 1))
            os.utime(dep, (1, 1))
            os.utime(obj, (2, 2))

            with patch.object(build, "compute_build_signature", side_effect=OSError("unreadable")):
                self.assertTrue(
                    build.should_recompile(
                        str(source), str(obj), str(dep), {"FORCE_REBUILD": "0"}, "clang++", ["-std=c++20"]
                    )
                )

    def test_no_build_verification_reuses_version_without_bumping(self) -> None:
        with patch.object(build, "read_build_version_number", return_value=4321) as read_version, patch.object(
            build, "bump_and_write_build_version"
        ) as bump_version:
            selected = build.resolve_build_number_for_invocation(
                sanitize_regression_child=False, resume_failed_build=False, no_build=True
            )

        self.assertEqual(selected, 4321)
        read_version.assert_called_once_with()
        bump_version.assert_not_called()

    def test_tests_only_reuses_product_version_without_bumping(self) -> None:
        with patch.object(build, "read_build_version_number", return_value=4321) as read_version, patch.object(
            build, "bump_and_write_build_version"
        ) as bump_version:
            selected = build.resolve_build_number_for_invocation(
                sanitize_regression_child=False,
                resume_failed_build=False,
                no_build=False,
                tests_only=True,
            )

        self.assertEqual(selected, 4321)
        read_version.assert_called_once_with()
        bump_version.assert_not_called()

    def test_link_cache_validates_inputs_and_outputs_by_content(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            compiler = Path(temporary) / "clang++.exe"
            object_file = Path(temporary) / "input.o"
            output = Path(temporary) / "unit_tests.exe"
            compiler.write_bytes(b"compiler")
            object_file.write_bytes(b"object-a")
            output.write_bytes(b"output-a")
            command = [str(compiler), str(object_file), "-o", str(output)]
            env = {"PATH": "tools"}

            build.compute_compiler_fingerprint.cache_clear()
            build.compute_link_input_fingerprint.cache_clear()
            build.get_link_resource_dir.cache_clear()
            with patch.object(build, "detect_clang_resource_dir", return_value=None):
                signature, inputs = build.compute_link_signature(command, env)
                manifest = {
                    "schema": build.LINK_CACHE_SCHEMA_VERSION,
                    "command": command,
                    "cwd": None,
                    "input_signature": signature,
                    "input_count": len(inputs),
                    "outputs": {str(output.resolve()): build.sha256_file(str(output))},
                }
                Path(build.link_cache_manifest_path(str(output))).write_text(
                    json.dumps(manifest), encoding="utf-8"
                )
                self.assertTrue(build.validate_cached_link_output(str(output), env))

                object_file.write_bytes(b"object-b")
                build.compute_link_input_fingerprint.cache_clear()
                self.assertFalse(build.validate_cached_link_output(str(output), env))

                object_file.write_bytes(b"object-a")
                output.write_bytes(b"output-b")
                build.compute_link_input_fingerprint.cache_clear()
                self.assertFalse(build.validate_cached_link_output(str(output), env))

    def test_resume_requires_matching_immediately_failed_build_identity(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            header = Path(temporary) / "build_version.h"
            manifest = Path(temporary) / "latest_manifest.json"
            header.write_text("#define BUILD_NUMBER 8765\n", encoding="utf-8")
            failed_state = {
                "top_level": True,
                "success": False,
                "build_number": 8765,
                "build_script_sha256": build.sha256_file(build.__file__),
                "args": ["--skip-updates"],
            }
            manifest.write_text(json.dumps(failed_state), encoding="utf-8")

            self.assertEqual(build.read_failed_build_resume_version(str(manifest), str(header)), 8765)

            failed_state["success"] = True
            manifest.write_text(json.dumps(failed_state), encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "did not fail"):
                build.read_failed_build_resume_version(str(manifest), str(header))

            failed_state["success"] = False
            failed_state["build_script_sha256"] = "stale"
            manifest.write_text(json.dumps(failed_state), encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "build.py changed"):
                build.read_failed_build_resume_version(str(manifest), str(header))

    def test_lint_findings_are_only_fatal_for_standalone_lint_invocation(self) -> None:
        self.assertTrue(build.is_standalone_lint_invocation(["--lint"]))
        self.assertTrue(build.is_standalone_lint_invocation(["--lint", "--concise", "--jobs=4"]))
        self.assertFalse(build.is_standalone_lint_invocation([]))
        self.assertFalse(build.is_standalone_lint_invocation(["--verify"]))
        self.assertFalse(build.is_standalone_lint_invocation(["--lint", "--run-tests"]))
        self.assertFalse(build.is_standalone_lint_invocation(["--lint", "--skip-updates"]))

    def test_presentation_options_preserve_default_quality_mode(self) -> None:
        self.assertTrue(build.is_default_quality_invocation([]))
        self.assertTrue(build.is_default_quality_invocation(["--concise"]))
        self.assertTrue(build.is_default_quality_invocation(["--jobs", "6", "--log-file=custom.log"]))
        self.assertTrue(build.is_default_quality_invocation(["--verbose-commands", "--detail-log", "detail.log"]))
        self.assertFalse(build.is_default_quality_invocation(["--skip-updates", "--concise"]))
        self.assertFalse(build.is_default_quality_invocation(["--incremental", "--jobs=3"]))
        self.assertFalse(build.is_default_quality_invocation(["--jobs", "--skip-updates"]))

    def test_sanitizer_child_always_reuses_prepared_externals_and_stays_concise(self) -> None:
        command = build.sanitizer_regression_command(ccache_flag=False)
        self.assertIn("--skip-updates", command)
        self.assertIn("--concise", command)
        self.assertIn("--sanitize-regression-child", command)

    def test_python_tool_bootstrap_does_not_mutate_link_environment(self) -> None:
        completed = build.subprocess.CompletedProcess(args=[], returncode=0)
        with patch.dict(os.environ, {"PATH": "host-tools"}), patch.object(
            build.subprocess, "run", return_value=completed
        ):
            self.assertTrue(build.check_python_lsp_tools())
            self.assertEqual(os.environ["PATH"], "host-tools")

    def test_lint_modes_bootstrap_their_own_python_tools(self) -> None:
        self.assertTrue(build.should_bootstrap_python_tools(True, False, False, False))
        self.assertTrue(build.should_bootstrap_python_tools(False, True, False, False))
        self.assertTrue(build.should_bootstrap_python_tools(False, False, True, False))
        self.assertFalse(build.should_bootstrap_python_tools(False, False, False, False))

    def test_concise_logging_splits_summary_and_complete_detail_logs(self) -> None:
        previous_log = build.LOG_FILE
        previous_detail_log = build.DETAIL_LOG_FILE
        previous_concise = build.CONCISE_OUTPUT
        previous_verbose = build.VERBOSE_COMMANDS
        with tempfile.TemporaryDirectory() as temporary:
            build.LOG_FILE = str(Path(temporary) / "build.log")
            build.DETAIL_LOG_FILE = str(Path(temporary) / "build.details.log")
            build.CONCISE_OUTPUT = True
            build.VERBOSE_COMMANDS = False
            try:
                with patch("builtins.print") as print_output:
                    build.log("compile detail", detail=True)
                    print_output.assert_not_called()
                    build.log("stage summary")
                    print_output.assert_called_once()
                log_text = Path(build.LOG_FILE).read_text(encoding="utf-8")
                detail_text = Path(build.DETAIL_LOG_FILE).read_text(encoding="utf-8")
                self.assertNotIn("compile detail", log_text)
                self.assertIn("stage summary", log_text)
                self.assertIn("compile detail", detail_text)
                self.assertIn("stage summary", detail_text)
            finally:
                build.LOG_FILE = previous_log
                build.DETAIL_LOG_FILE = previous_detail_log
                build.CONCISE_OUTPUT = previous_concise
                build.VERBOSE_COMMANDS = previous_verbose

    def test_run_command_preserves_successful_stderr_in_detail_log(self) -> None:
        previous_log = build.LOG_FILE
        previous_detail_log = build.DETAIL_LOG_FILE
        previous_concise = build.CONCISE_OUTPUT
        with tempfile.TemporaryDirectory() as temporary:
            build.LOG_FILE = str(Path(temporary) / "build.log")
            build.DETAIL_LOG_FILE = str(Path(temporary) / "build.details.log")
            build.CONCISE_OUTPUT = True
            result = build.subprocess.CompletedProcess(
                args=["tool"], returncode=0, stdout=b"normal output\n", stderr=b"warning output\n"
            )
            try:
                with patch.object(build.subprocess, "run", return_value=result):
                    self.assertEqual(build.run_command(["tool"]), "normal output\n")
                detail_text = Path(build.DETAIL_LOG_FILE).read_text(encoding="utf-8")
                summary_text = Path(build.LOG_FILE).read_text(encoding="utf-8") if Path(build.LOG_FILE).exists() else ""
                self.assertIn("warning output", detail_text)
                self.assertNotIn("warning output", summary_text)
            finally:
                build.LOG_FILE = previous_log
                build.DETAIL_LOG_FILE = previous_detail_log
                build.CONCISE_OUTPUT = previous_concise

    def test_sanitizer_child_reuses_parent_build_version(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            header = Path(temporary) / "build_version.h"
            header.write_text("#define BUILD_NUMBER 1234\n", encoding="utf-8")
            self.assertEqual(build.read_build_version_number(str(header)), 1234)

        with patch.object(build, "read_build_version_number", return_value=1234) as read_version, patch.object(
            build, "bump_and_write_build_version"
        ) as bump_version:
            selected = build.resolve_build_number_for_invocation(
                sanitize_regression_child=True, resume_failed_build=False, no_build=False
            )

        self.assertEqual(selected, 1234)
        read_version.assert_called_once_with()
        bump_version.assert_not_called()

    def test_vulkan_manifests_use_build_specific_layer_identity(self) -> None:
        source = build.read_source_text()
        self.assertIn('layer_name = f"{layer_name_base}_b{CURRENT_BUILD_NUMBER}"', source)
        self.assertIn('"implementation_version": str(CURRENT_BUILD_NUMBER)', source)

    def test_failed_unit_tests_capture_diagnostics_on_linux(self) -> None:
        previous_context = build.VERIFICATION_CONTEXT
        with tempfile.TemporaryDirectory() as temporary:
            test_exe = Path(temporary) / "unit_tests.exe"
            test_exe.write_bytes(b"")
            build.VERIFICATION_CONTEXT = {"run_dir": temporary, "artifacts": {}}
            failure = build.subprocess.CompletedProcess(
                args=[str(test_exe)],
                returncode=1,
                stdout="[  FAILED  ] ExampleTest.Fails\n",
                stderr="diagnostic stderr\n",
            )
            try:
                with patch.object(build, "IS_LINUX", True), patch.object(
                    build.shutil,
                    "which",
                    side_effect=lambda executable: "/usr/bin/wine64" if executable == "wine64" else None,
                ), patch.object(build.subprocess, "run", return_value=failure) as run, patch.object(build, "log"):
                    self.assertFalse(build.run_tests({}, str(test_exe)))
                failure_log = Path(temporary) / "unit_tests_failure.log"
                self.assertTrue(failure_log.exists())
                diagnostics = failure_log.read_text(encoding="utf-8")
                self.assertIn("ExampleTest.Fails", diagnostics)
                self.assertIn("diagnostic stderr", diagnostics)
                self.assertTrue(run.call_args.kwargs["capture_output"])
                self.assertEqual(run.call_args.kwargs["encoding"], "utf-8")
                self.assertEqual(run.call_args.args[0][0], "/usr/bin/wine64")
            finally:
                build.VERIFICATION_CONTEXT = previous_context

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

    def test_vulkan_layer_build_has_no_registry_side_effects(self) -> None:
        source = build.read_source_text()
        self.assertNotIn("cleanup_vulkan_layer_registry", source)
        self.assertNotIn("import winreg", source)
        self.assertNotIn("winreg.", source)

    def test_integration_runner_requires_recorded_encoder_output(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            media_log = Path(temporary) / "media.log"
            media_log.write_text(
                "[2026-07-17 15:02:00] [VideoEncoder] Recording stats: input=4 output=0\n"
                "[2026-07-17 15:02:01] [VideoEncoder] Recording stats: input=123 output=120\n",
                encoding="utf-8",
            )

            self.assertEqual(integration_runner.parse_recorded_output_frames(media_log, 0.0), 120)

    def test_integration_results_default_to_latest_session_directory(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            capture_bin = Path(temporary) / "captureengine"
            session_dir = capture_bin / "logs" / "20260717_150205"
            session_dir.mkdir(parents=True)
            since = time.time() - 1.0

            with patch.object(integration_runner, "CAPTURE_BIN", capture_bin):
                output_path = integration_runner.default_results_json_path(since)

            self.assertEqual(output_path, session_dir / "integration_results.json")


if __name__ == "__main__":
    unittest.main()
