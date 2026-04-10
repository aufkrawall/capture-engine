import tempfile
import unittest
from pathlib import Path

import build


class ResolveMsys2GTestLinkInputsTest(unittest.TestCase):
    def test_prefers_import_libraries(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            lib_dir = Path(tmp_dir)
            (lib_dir / "libgtest_main.a").touch()
            (lib_dir / "libgtest.a").touch()
            gtest_main_import = lib_dir / "libgtest_main.dll.a"
            gtest_import = lib_dir / "libgtest.dll.a"
            gtest_main_import.touch()
            gtest_import.touch()

            self.assertEqual(
                build.resolve_msys2_gtest_link_inputs(str(lib_dir)),
                [str(gtest_main_import), str(gtest_import)],
            )

    def test_falls_back_to_static_archives(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            lib_dir = Path(tmp_dir)
            gtest_main_static = lib_dir / "libgtest_main.a"
            gtest_static = lib_dir / "libgtest.a"
            gtest_main_static.touch()
            gtest_static.touch()

            self.assertEqual(
                build.resolve_msys2_gtest_link_inputs(str(lib_dir)),
                [str(gtest_main_static), str(gtest_static)],
            )

    def test_raises_when_required_libraries_are_missing(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            lib_dir = Path(tmp_dir)
            (lib_dir / "libgtest_main.dll.a").touch()

            with self.assertRaises(RuntimeError) as context:
                build.resolve_msys2_gtest_link_inputs(str(lib_dir))

            self.assertIn("libgtest(.dll.a/.a)", str(context.exception))


if __name__ == "__main__":
    unittest.main()
