import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch


MODULE_PATH = Path(__file__).resolve().parents[1] / "verify_pe_hardening.py"
SPEC = importlib.util.spec_from_file_location("verify_pe_hardening", MODULE_PATH)
assert SPEC and SPEC.loader
hardening = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = hardening
SPEC.loader.exec_module(hardening)


class PeHardeningParserTest(unittest.TestCase):
    def test_accepts_complete_x64_cfg_metadata(self) -> None:
        output = """
Arch: x86_64
  IMAGE_DLL_CHARACTERISTICS_DYNAMIC_BASE
  IMAGE_DLL_CHARACTERISTICS_HIGH_ENTROPY_VA
  IMAGE_DLL_CHARACTERISTICS_NX_COMPAT
  IMAGE_DLL_CHARACTERISTICS_GUARD_CF
Section {
  Name: .text
  IMAGE_SCN_MEM_EXECUTE
  IMAGE_SCN_MEM_READ
}
GuardCFFunctionTable: 0x140001000
GuardCFFunctionCount: 42
  CF_INSTRUMENTED
  CF_FUNCTION_TABLE_PRESENT
"""
        result = hardening.parse_llvm_readobj_hardening(output, "x64")
        self.assertEqual(result.errors, ())
        self.assertEqual(result.guard_function_count, 42)

    def test_rejects_empty_cfg_and_writable_executable_section(self) -> None:
        output = """
Arch: x86_64
  IMAGE_DLL_CHARACTERISTICS_DYNAMIC_BASE
  IMAGE_DLL_CHARACTERISTICS_HIGH_ENTROPY_VA
  IMAGE_DLL_CHARACTERISTICS_NX_COMPAT
Section {
  Name: .bad
  IMAGE_SCN_MEM_EXECUTE
  IMAGE_SCN_MEM_WRITE
}
GuardCFFunctionTable: 0x0
GuardCFFunctionCount: 0
"""
        result = hardening.parse_llvm_readobj_hardening(output, "x64")
        self.assertIn("GuardCFFunctionTable is empty", result.errors)
        self.assertIn("writable/executable section .bad", result.errors)

    def test_x86_cfg_can_be_deferred_without_weakening_other_checks(self) -> None:
        output = """
Arch: i386
  IMAGE_DLL_CHARACTERISTICS_DYNAMIC_BASE
  IMAGE_DLL_CHARACTERISTICS_NX_COMPAT
Section {
  Name: .text
  IMAGE_SCN_MEM_EXECUTE
  IMAGE_SCN_MEM_READ
}
GuardCFFunctionTable: 0x0
GuardCFFunctionCount: 0
"""
        deferred = hardening.parse_llvm_readobj_hardening(output, "x86", require_cfg=False)
        self.assertEqual(deferred.errors, ())

        missing_nx = hardening.parse_llvm_readobj_hardening(
            output.replace("IMAGE_DLL_CHARACTERISTICS_NX_COMPAT", ""), "x86", require_cfg=False
        )
        self.assertIn("missing IMAGE_DLL_CHARACTERISTICS_NX_COMPAT", missing_nx.errors)

    def test_x64_cfg_can_be_deferred_without_weakening_other_checks(self) -> None:
        output = """
Arch: x86_64
  IMAGE_DLL_CHARACTERISTICS_DYNAMIC_BASE
  IMAGE_DLL_CHARACTERISTICS_HIGH_ENTROPY_VA
  IMAGE_DLL_CHARACTERISTICS_NX_COMPAT
Section {
  Name: .text
  IMAGE_SCN_MEM_EXECUTE
  IMAGE_SCN_MEM_READ
}
GuardCFFunctionTable: 0x0
GuardCFFunctionCount: 0
"""
        deferred = hardening.parse_llvm_readobj_hardening(output, "x64", require_cfg=False)
        self.assertEqual(deferred.errors, ())

        missing_high_entropy = hardening.parse_llvm_readobj_hardening(
            output.replace("IMAGE_DLL_CHARACTERISTICS_HIGH_ENTROPY_VA", ""), "x64", require_cfg=False
        )
        self.assertIn("missing IMAGE_DLL_CHARACTERISTICS_HIGH_ENTROPY_VA", missing_high_entropy.errors)

    def test_extracts_runtime_import_names_case_insensitively(self) -> None:
        output = """
Import {
  Name: KERNEL32.dll
}
Import {
  Name: libaom.dll
}
Section {
  Name: .text
}
"""
        self.assertEqual(hardening.parse_llvm_readobj_imports(output), {"kernel32.dll", "libaom.dll"})

    def test_sanitizer_scan_can_exclude_stale_x86_outputs(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "captureengine.exe").touch()
            (root / "capture_hook_x86.dll").touch()
            names = [path.name for path, _ in hardening.shipped_binaries(root, skip_x86=True)]
        self.assertEqual(names, ["captureengine.exe"])

    def test_effective_cfg_requires_instrumentation_flag(self) -> None:
        output = """
Arch: x86_64
  IMAGE_DLL_CHARACTERISTICS_DYNAMIC_BASE
  IMAGE_DLL_CHARACTERISTICS_HIGH_ENTROPY_VA
  IMAGE_DLL_CHARACTERISTICS_NX_COMPAT
  IMAGE_DLL_CHARACTERISTICS_GUARD_CF
GuardCFFunctionTable: 0x140001000
GuardCFFunctionCount: 1
  CF_FUNCTION_TABLE_PRESENT
"""
        result = hardening.parse_llvm_readobj_hardening(output, "x64")
        self.assertIn("GuardFlags lacks CF_INSTRUMENTED", result.errors)

    def test_x86_subdirectory_selects_x86_architecture(self) -> None:
        self.assertEqual(hardening.expected_architecture(Path("testapp") / "x86" / "dx11_test.exe"), "x86")

    def test_executables_only_excludes_third_party_dlls(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "dx12_test.exe").touch()
            (root / "vendor.dll").touch()
            names = [path.name for path, _ in hardening.shipped_binaries(root, executables_only=True)]
        self.assertEqual(names, ["dx12_test.exe"])

    def test_allowed_runtime_dll_does_not_require_first_party_pdb(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "captureengine.dll").touch()
            (root / "libc++.dll").touch()
            result = hardening.PeHardeningResult("x64", 1, ())
            with patch.object(hardening, "verify_binary", return_value=result) as verify:
                failures = hardening.verify_tree(
                    Path("llvm-readobj"), root, allowed_runtime_dlls=("libc++.dll",)
                )

        self.assertEqual(failures, [])
        require_pdb_by_name = {call.args[1].name: call.args[2] for call in verify.call_args_list}
        self.assertTrue(require_pdb_by_name["captureengine.dll"])
        self.assertFalse(require_pdb_by_name["libc++.dll"])

    @staticmethod
    def _section(name: str, virtual_size: int, raw_size: int, characteristics: str) -> str:
        return f"""Section {{
  Number: 1
  Name: {name} (2E 00)
  VirtualSize: {hex(virtual_size)}
  RawDataSize: {raw_size}
  Characteristics [ (0xC0000040)
{characteristics}
  ]
}}
"""

    def test_dwarf_next_to_a_pdb_is_rejected(self) -> None:
        output = self._section(".text", 4096, 4096, "    IMAGE_SCN_MEM_EXECUTE") + self._section(
            ".debug_info", 350000, 350720, "    IMAGE_SCN_CNT_INITIALIZED_DATA\n    IMAGE_SCN_MEM_DISCARDABLE"
        )
        errors = hardening.image_budget_errors(output, check_dwarf=True, check_budgets=True)
        self.assertEqual(errors, ["carries 1 DWARF sections (350720 bytes) next to its PDB"])
        # Cross builds without a PDB keep DWARF on purpose.
        self.assertEqual(hardening.image_budget_errors(output, check_dwarf=False, check_budgets=True), [])

    def test_initialized_writable_data_and_tls_have_ceilings(self) -> None:
        writable = "    IMAGE_SCN_CNT_INITIALIZED_DATA\n    IMAGE_SCN_MEM_READ\n    IMAGE_SCN_MEM_WRITE"
        # The 0.1.7291 x86 hook: 5.6 MiB of trace rings in .data, a 39 KiB TLS template.
        regressed = self._section(".data", 0x6A76B1, 5796352, writable) + self._section(".tls", 0x9A84, 39936, writable)
        errors = hardening.image_budget_errors(regressed, check_dwarf=True, check_budgets=True)
        self.assertEqual(len(errors), 2, errors)
        self.assertTrue(errors[0].startswith("initialized writable data is 5796352 bytes"))
        self.assertTrue(errors[1].startswith("TLS template is 39556 bytes per thread"))
        # Zero-fill beyond the raw data costs no file size and is not counted.
        zero_filled = self._section(".data", 8 * 1024 * 1024, 32768, writable) + self._section(
            ".rdata", 4 * 1024 * 1024, 4 * 1024 * 1024, "    IMAGE_SCN_CNT_INITIALIZED_DATA\n    IMAGE_SCN_MEM_READ"
        )
        self.assertEqual(hardening.image_budget_errors(zero_filled, check_dwarf=True, check_budgets=True), [])
        self.assertEqual(hardening.image_budget_errors(regressed, check_dwarf=True, check_budgets=False), [])

    def test_image_budgets_cover_only_project_images(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "capture_hook_x64.dll").touch()
            (root / "libc++.dll").touch()
            (root / "ffmpeg").mkdir()
            (root / "ffmpeg" / "avcodec-ce-63.dll").touch()
            result = hardening.PeHardeningResult("x64", 1, ())
            with patch.object(hardening, "verify_binary", return_value=result) as verify:
                hardening.verify_tree(Path("llvm-readobj"), root, allowed_runtime_dlls=("libc++.dll",))
                budgets = {call.args[1].name: call.kwargs["check_image_budgets"] for call in verify.call_args_list}
                verify.reset_mock()
                hardening.verify_tree(Path("llvm-readobj"), root, skip_image_budgets=True)
                skipped = {call.args[1].name: call.kwargs["check_image_budgets"] for call in verify.call_args_list}

        self.assertEqual(budgets, {"capture_hook_x64.dll": True, "libc++.dll": False, "avcodec-ce-63.dll": False})
        self.assertFalse(any(skipped.values()))

    def test_cross_build_can_defer_pdb_without_disabling_other_verification(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "captureengine.exe").touch()
            result = hardening.PeHardeningResult("x64", 0, ())
            with patch.object(hardening, "verify_binary", return_value=result) as verify:
                failures = hardening.verify_tree(
                    Path("llvm-readobj"),
                    root,
                    allow_missing_x64_cfg=True,
                    allow_missing_pdb=True,
                )

        self.assertEqual(failures, [])
        self.assertFalse(verify.call_args.args[2])
        self.assertTrue(verify.call_args.kwargs["allow_missing_x64_cfg"])


if __name__ == "__main__":
    unittest.main()
