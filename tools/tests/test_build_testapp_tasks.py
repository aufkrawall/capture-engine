import os
import tempfile
import unittest
from contextlib import ExitStack
from pathlib import Path
from unittest.mock import patch

import build
from tools.verification_stage_cache import SOURCE_DIRS


LINUX_X64_COMPILER = "/usr/bin/x86_64-w64-mingw32-g++"
LINUX_X86_COMPILER = "/usr/bin/i686-w64-mingw32-g++"
NATIVE_HANDLE_SCAN_SUFFIXES = {".cpp", ".h", ".hpp", ".inl"}


def scan_native_handle_uses(project_root: Path) -> list[str]:
    """Report first-party uses of std::thread::native_handle().

    Scoped to the first-party source directories on purpose. Vendored trees
    (external/, ffmpeg_build/) follow their own conventions, are never built
    against this contract, and are not even UTF-8 - AMD's and Valve's headers
    carry CP1252 copyright signs - so walking the whole project root raises
    UnicodeDecodeError long before it can report anything. Decoding stays
    tolerant for the same reason: the token searched for is pure ASCII, so a
    stray byte in a first-party file must not abort the scan either.
    """
    allowed = project_root / "common" / "thread_wait.h"
    offenders: list[str] = []
    for directory_name in SOURCE_DIRS:
        directory = project_root / directory_name
        if not directory.is_dir():
            continue
        for source in sorted(directory.rglob("*")):
            if source.suffix not in NATIVE_HANDLE_SCAN_SUFFIXES or source == allowed:
                continue
            text = source.read_text(encoding="utf-8", errors="replace")
            for number, line in enumerate(text.splitlines(), start=1):
                code = line.split("//", 1)[0]
                if "native_handle" in code:
                    offenders.append(f"{source.relative_to(project_root)}:{number}")
    return offenders


class TestAppTaskTest(unittest.TestCase):
    """Guard the per-architecture identity of test app compile/link tasks.

    Linux cross builds select the architecture through prefixed system
    compilers and pass no --target/--sysroot, so anything that re-derives the
    architecture from the command text used to classify every x86 test app as
    x64. Both variants then shared one object path, and parallel workers
    overwrote each other's object mid-link.
    """

    def run_testapps(self, *, have_x86: bool = True):
        # Kept on the instance so a failing run can still be inspected.
        objects = self.objects = []
        links = self.links = []

        def fake_compile_object(env, compiler, cflags, src, obj):
            os.makedirs(os.path.dirname(obj), exist_ok=True)
            Path(obj).write_bytes(b"")
            objects.append({"compiler": compiler, "source": src, "object": obj, "env": env})
            return True

        def fake_run_cached_link(command, env, output, required_outputs=None, cwd=None):
            os.makedirs(os.path.dirname(output), exist_ok=True)
            Path(output).write_bytes(b"")
            links.append({"command": list(command), "output": output, "env": env})
            return True

        def fake_get_compiler_exe(arch: str = "x64"):
            if arch == "x86":
                return LINUX_X86_COMPILER if have_x86 else None
            return LINUX_X64_COMPILER

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            x64_env = {"CE_BUILD_JOBS": "1", "CXX": LINUX_X64_COMPILER}
            x86_env = {"CE_BUILD_JOBS": "1", "CXX": LINUX_X86_COMPILER} if have_x86 else None

            with ExitStack() as stack:
                for target, replacement in (
                    ("IS_LINUX", True),
                    ("IS_WINDOWS", False),
                    ("OBJ_DIR", str(root / "obj")),
                    ("TESTAPP_BIN_DIR", str(root / "bin" / "testapps")),
                ):
                    stack.enter_context(patch.object(build, target, replacement))
                for target, replacement in (
                    ("log", lambda *args, **kwargs: None),
                    ("get_compiler_exe", fake_get_compiler_exe),
                    ("get_linux_vulkan_import_lib_path", lambda arch: f"/usr/lib/{arch}/libvulkan-1.dll.a"),
                    ("compile_vulkan_fg_shaders", lambda env: None),
                    ("get_fg_sdk_include_flags", list),
                    ("get_vulkan_fg_sdk_include_flags", list),
                    ("compute_compiler_fingerprint", lambda compiler: ""),
                    ("_get_linux_cross_linker_info", lambda compiler: ""),
                    ("make_task_temp_environment", lambda base_env, temp_key: dict(base_env)),
                    ("compile_object", fake_compile_object),
                    ("run_cached_link", fake_run_cached_link),
                ):
                    stack.enter_context(patch.object(build, target, replacement))

                build.compile_testapps(x64_env, x86_env, LINUX_X64_COMPILER, [])

        return objects, links

    def test_linux_cross_compilers_are_classified_by_driver_name(self) -> None:
        self.assertTrue(build.is_x86_compile_command([LINUX_X86_COMPILER, "-O2", "a.cpp", "-o", "a.exe"]))
        self.assertFalse(build.is_x86_compile_command([LINUX_X64_COMPILER, "-O2", "a.cpp", "-o", "a.exe"]))
        self.assertTrue(build.is_x86_compile_command(["clang++", "-m32", "a.cpp"]))
        self.assertFalse(build.is_x86_compile_command([]))

    def test_every_test_app_task_owns_a_distinct_object_path(self) -> None:
        objects, links = self.run_testapps()

        self.assertTrue(objects)
        object_paths = [entry["object"] for entry in objects]
        self.assertEqual(len(object_paths), len(set(object_paths)))
        self.assertEqual(len(objects), len(links))

    def test_linux_x86_test_apps_do_not_reuse_the_x64_object_path(self) -> None:
        objects, _ = self.run_testapps()

        by_compiler = {}
        for entry in objects:
            by_compiler.setdefault(entry["compiler"], set()).add(os.path.basename(entry["object"]))
        self.assertIn(LINUX_X86_COMPILER, by_compiler)

        shared_names = by_compiler[LINUX_X64_COMPILER] & by_compiler[LINUX_X86_COMPILER]
        self.assertTrue(shared_names, "expected test apps built for both architectures")

        for entry in objects:
            expected_arch = "x86" if entry["compiler"] == LINUX_X86_COMPILER else "x64"
            self.assertEqual(
                os.path.basename(os.path.dirname(entry["object"])),
                expected_arch,
                f"{entry['object']} was placed in the wrong architecture directory",
            )

    def test_linux_x86_test_apps_build_in_the_x86_environment(self) -> None:
        objects, links = self.run_testapps()

        for entry in objects + links:
            compiler = entry.get("compiler") or entry["command"][0]
            self.assertEqual(entry["env"].get("CXX"), compiler)

    def test_missing_x86_compiler_keeps_the_x64_tasks_intact(self) -> None:
        objects, _ = self.run_testapps(have_x86=False)

        self.assertTrue(objects)
        self.assertEqual({entry["compiler"] for entry in objects}, {LINUX_X64_COMPILER})

    def test_object_collisions_are_rejected_instead_of_raced_on(self) -> None:
        build.ensure_unique_testapp_objects(
            [
                ("dx12_test.exe", "obj/testapps/x64/dx12_test.o"),
                ("dx12_test.exe (x86)", "obj/testapps/x86/dx12_test.o"),
            ]
        )

        with self.assertRaisesRegex(RuntimeError, "Multiple test app tasks target the same object"):
            build.ensure_unique_testapp_objects(
                [
                    ("dx12_test.exe", "obj/testapps/x64/dx12_test.o"),
                    ("dx12_test.exe (x86)", "obj/testapps/x64/dx12_test.o"),
                ]
            )

    def test_object_collision_check_runs_before_any_worker(self) -> None:
        def reject(entries):
            self.assertTrue(entries)
            raise RuntimeError("Multiple test app tasks target the same object: injected")

        with patch.object(build, "ensure_unique_testapp_objects", reject):
            with self.assertRaisesRegex(RuntimeError, "injected"):
                self.run_testapps()

        # The guard must reject the task list up front rather than surface as a
        # per-worker failure once objects have already been written.
        self.assertEqual(self.objects, [])
        self.assertEqual(self.links, [])

    def test_no_source_treats_a_native_thread_handle_as_a_win32_handle(self) -> None:
        """std::thread::native_handle() is only a Win32 HANDLE under the Win32 threading model.

        A winpthreads MinGW - the system default on several Linux distributions,
        and therefore for cross builds - returns a pthread_t. A plain assignment
        fails to compile there; a reinterpret_cast compiles and silently yields a
        handle WaitForSingleObject rejects. Windows-only work would never see
        either, so the contract is checked here instead.
        """
        offenders = scan_native_handle_uses(Path(build.__file__).parent)

        self.assertEqual(
            offenders,
            [],
            "use ce::Win32ThreadHandle() from common/thread_wait.h instead of std::thread::native_handle(): "
            + ", ".join(offenders),
        )

    def test_native_handle_scan_skips_vendored_trees_and_survives_their_encodings(self) -> None:
        """The scan must not read third-party sources, whatever they contain."""
        with tempfile.TemporaryDirectory() as temporary:
            project_root = Path(temporary)
            first_party = project_root / "common"
            first_party.mkdir()
            (first_party / "worker.cpp").write_text(
                "void wait() { worker.native_handle(); }\n", encoding="utf-8"
            )
            for vendored in ("external", "ffmpeg_build", "build"):
                directory = project_root / vendored / "nested"
                directory.mkdir(parents=True)
                # Latin-1 copyright banners are what actually broke the walk.
                (directory / "vendor.h").write_bytes(
                    b"// Copyright \xa9 2023 Vendor\nauto h = t.native_handle();\n"
                )

            offenders = scan_native_handle_uses(project_root)

        self.assertEqual(offenders, [os.path.join("common", "worker.cpp") + ":1"])

    def test_test_app_commands_carry_their_architecture(self) -> None:
        objects, _ = self.run_testapps()
        self.assertTrue(objects)

        source = build.read_source_text()
        # The object directory must come from the recorded architecture, never
        # from re-reading the command flags.
        self.assertIn("object_path = testapp_object_path(cmd, arch)", source)
        self.assertIn("return TestAppCommand(", source)


if __name__ == "__main__":
    unittest.main()
