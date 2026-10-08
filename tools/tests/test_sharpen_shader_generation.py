# pyright: reportAttributeAccessIssue=false
"""Keep shader regeneration on the paths consumed by the product build."""

from pathlib import Path
import importlib.util
import sys
import tempfile
import unittest
from unittest.mock import patch


class SharpenShaderGenerationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = Path(__file__).resolve().parents[1]
        spec = importlib.util.spec_from_file_location("ce_sharpen_codegen", cls.root / "compile_sharpen_shaders.py")
        assert spec is not None and spec.loader is not None
        cls.generator = importlib.util.module_from_spec(spec)
        sys.path.insert(0, str(cls.root))
        try:
            spec.loader.exec_module(cls.generator)
        finally:
            sys.path.pop(0)

    def test_output_tree_matches_the_product_include_tree(self):
        self.assertEqual(self.generator.COMMON_DIR, self.root.parent / "hook" / "sharpen")
        self.assertEqual(self.generator.DXBC_HEADER.parent, self.generator.COMMON_DIR)
        self.assertEqual(self.generator.SPIRV_HEADER.parent, self.generator.COMMON_DIR)

    def test_emitted_umbrella_resolves_every_registered_shader(self):
        with tempfile.TemporaryDirectory(dir=self.root.parent / "build") as directory:
            root = Path(directory)
            parts = root / "sharpen_shader_bytecode"
            header = root / "sharpen_shader_bytecode.h"
            with (
                patch.object(self.generator, "DXBC_PART_DIR", parts),
                patch.object(self.generator, "DXBC_HEADER", header),
                patch.object(self.generator, "compile_shader", return_value=b"DXBCfixture"),
            ):
                emitted = self.generator.build_dxbc()
            text = header.read_text(encoding="utf-8")
            self.assertEqual(len(emitted), len(self.generator.DXBC_SHADERS))
            for name in emitted:
                self.assertTrue((parts / name).is_file())
                self.assertIn(f'sharpen_shader_bytecode/{name}', text)


if __name__ == "__main__":
    unittest.main()
