import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).parent / "tools" / "verify_pe_hardening.py"
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


if __name__ == "__main__":
    unittest.main()
