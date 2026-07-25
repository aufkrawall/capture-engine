import tempfile
import unittest
from unittest import mock
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

    def test_prefers_static_archives_when_requested(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            lib_dir = Path(tmp_dir)
            gtest_main_static = lib_dir / "libgtest_main.a"
            gtest_static = lib_dir / "libgtest.a"
            gtest_main_import = lib_dir / "libgtest_main.dll.a"
            gtest_import = lib_dir / "libgtest.dll.a"
            gtest_main_static.touch()
            gtest_static.touch()
            gtest_main_import.touch()
            gtest_import.touch()

            self.assertEqual(
                build.resolve_msys2_gtest_link_inputs(str(lib_dir), prefer_static=True),
                [str(gtest_main_static), str(gtest_static)],
            )

    def test_raises_when_required_libraries_are_missing(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            lib_dir = Path(tmp_dir)
            (lib_dir / "libgtest_main.dll.a").touch()

            with self.assertRaises(RuntimeError) as context:
                build.resolve_msys2_gtest_link_inputs(str(lib_dir))

            self.assertIn("libgtest(.dll.a/.a)", str(context.exception))


class LinuxMsys2GTestLibDirTest(unittest.TestCase):
    def test_prefers_mingw64_for_gcc_x64(self) -> None:
        with (
            mock.patch.object(build, "get_linux_msys2_dir", return_value="/tmp/msys"),
            mock.patch.object(
                build,
                "get_compiler_exe",
                return_value="/usr/bin/x86_64-w64-mingw32-g++",
            ),
        ):
            self.assertEqual(
                build.get_linux_msys2_gtest_lib_dir("x64").replace("\\", "/"),
                "/tmp/msys/mingw64/lib",
            )

    def test_prefers_clang64_for_clang_x64(self) -> None:
        with (
            mock.patch.object(build, "get_linux_msys2_dir", return_value="/tmp/msys"),
            mock.patch.object(
                build,
                "get_compiler_exe",
                return_value="/usr/bin/x86_64-w64-mingw32-clang++",
            ),
        ):
            self.assertEqual(
                build.get_linux_msys2_gtest_lib_dir("x64").replace("\\", "/"),
                "/tmp/msys/clang64/lib",
            )


class LinuxMsys2GTestPackageNameTest(unittest.TestCase):
    def test_uses_gcc_package_name_for_gcc_x64(self) -> None:
        with mock.patch.object(
            build,
            "get_linux_msys2_gtest_subdir",
            return_value="mingw64",
        ):
            self.assertEqual(
                build.get_linux_msys2_gtest_package_name("x64"),
                "mingw-w64-x86_64-gtest",
            )

    def test_uses_clang_package_name_for_clang_x64(self) -> None:
        with mock.patch.object(
            build,
            "get_linux_msys2_gtest_subdir",
            return_value="clang64",
        ):
            self.assertEqual(
                build.get_linux_msys2_gtest_package_name("x64"),
                "mingw-w64-clang-x86_64-gtest",
            )


if __name__ == "__main__":
    unittest.main()
