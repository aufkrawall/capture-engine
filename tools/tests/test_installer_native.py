"""Drive the real setup executable against scratch folders.

`--files-only` runs the file transaction, config.ini handling and manifest
logic of the installer without touching the registry, services, shortcuts or
running programs, so these tests are safe on a development machine. They need
the setup stub that the product build produces (build/installer/
captureengine_setup_stub.exe, or CE_INSTALLER_STUB) and skip when it is absent.
"""

import ctypes
import hashlib
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from tools import installer_payload as payload

PROJECT_ROOT = Path(__file__).resolve().parents[2]
STUB = Path(os.environ.get("CE_INSTALLER_STUB") or PROJECT_ROOT / "build" / "installer" / "captureengine_setup_stub.exe")

CONFIG_V1 = b"[Output]\r\noutput_dir=\r\n; shipped default v1\r\n"
CONFIG_V2 = b"[Output]\r\noutput_dir=\r\n; shipped default v2\r\n[New]\r\nfeature=1\r\n"


def payload_files(version: int):
    big = (b"capture engine hook " * 70000)[: payload.BLOCK_SIZE + 4096]
    files = {
        "captureengine.exe": b"MZ exe v%d" % version + b"\x00" * 64,
        "mediaengine.dll": b"MZ media v%d" % version + big,
        "ffmpeg/avcodec-63.dll": b"avcodec " + big,
        "plugins/LibreHardwareMonitor/README.txt": b"notes v%d\n" % version,
        "config.ini": CONFIG_V1 if version == 1 else CONFIG_V2,
        "captureengine_uninstall.exe": b"MZ uninstaller v%d" % version,
    }
    if version == 1:
        files["old_helper.dll"] = b"only in v1"
    else:
        files["new_helper.dll"] = b"only in v2"
    return sorted(files.items())


def expected(version, path):
    return dict(payload_files(version))[path]


@unittest.skipUnless(os.name == "nt" and STUB.is_file(), "setup stub not built")
class NativeInstallerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = Path(tempfile.mkdtemp(prefix="ce_setup_test_"))
        stub = STUB.read_bytes()
        cls.setups = {}
        for version in (1, 2):
            path = cls.work / f"setup-v{version}.exe"
            path.write_bytes(payload.assemble(stub, payload_files(version)))
            cls.setups[version] = path

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.work, ignore_errors=True)

    def setUp(self):
        slug = self._testMethodName[:20]
        token = hashlib.sha256(self._testMethodName.encode()).hexdigest()[:8]
        self.target = self.work / f"t_{slug}_{token}"

    def run_setup(self, version, *arguments, expect=0):
        completed = subprocess.run(
            [str(self.setups[version]), *arguments], capture_output=True, timeout=120, check=False
        )
        self.assertEqual(completed.returncode, expect, f"{arguments} exited {completed.returncode}")
        return completed

    def install(self, version=1, expect=0):
        return self.run_setup(version, "--files-only", f"--dir={self.target}", "/S", expect=expect)

    def uninstall(self, *extra, expect=0):
        return self.run_setup(1, "--files-only", "--uninstall", f"--dir={self.target}", "/S", *extra, expect=expect)

    @staticmethod
    def icacls_findsid(path, sid):
        # icacls prints in the console code page, which is not UTF-8 on most systems.
        completed = subprocess.run(["icacls", str(path), "/findsid", sid], capture_output=True, timeout=60, check=False)
        return completed.stdout.decode("mbcs", errors="replace")

    def content(self, relative):
        return (self.target / relative).read_bytes()

    # -- payload modes --------------------------------------------------------

    def test_verify_payload_accepts_a_good_file_and_rejects_a_damaged_one(self):
        self.run_setup(1, "--verify-payload")
        damaged = bytearray(self.setups[1].read_bytes())
        damaged[len(STUB.read_bytes()) + 20] ^= 0xFF
        broken = self.work / "damaged.exe"
        broken.write_bytes(bytes(damaged))
        completed = subprocess.run([str(broken), "--verify-payload"], capture_output=True, timeout=120, check=False)
        self.assertEqual(completed.returncode, 1)

    def test_extract_writes_every_file_exactly_and_refuses_a_used_folder(self):
        out = self.work / "extracted"
        self.run_setup(1, f"--extract={out}")
        for path, expected in payload_files(1):
            self.assertEqual((out / path).read_bytes(), expected, path)
        self.run_setup(1, f"--extract={out}", expect=3)

    # -- fresh install ----------------------------------------------------------

    def test_fresh_install_places_every_file_and_the_manifest(self):
        self.install(1)
        for path, expected in payload_files(1):
            if path == "config.ini":
                continue
            self.assertEqual(self.content(path), expected, path)
        manifest = self.content("captureengine_install.manifest").decode()
        self.assertTrue(manifest.startswith("CEINSTALL1"))
        self.assertIn("file=ffmpeg/avcodec-63.dll", manifest)
        self.assertNotIn("config.ini", manifest)
        self.assertEqual(self.content("config.ini"), CONFIG_V1)
        self.assertFalse((self.target / "config.ini.new").exists())
        for folder in ("logs", "captures", "screenshots", "benchmarks"):
            self.assertTrue((self.target / folder).is_dir(), folder)
        self.assertEqual([p.name for p in self.target.rglob("*") if p.suffix in (".cenew", ".cebak")], [])

    # -- update over an existing installation -------------------------------------

    def test_update_keeps_config_and_user_files_and_writes_config_new(self):
        self.install(1)
        (self.target / "config.ini").write_bytes(b"[Output]\r\noutput_dir=D:\\mine\r\n")
        (self.target / "notes.txt").write_bytes(b"my notes")
        (self.target / "logs" / "session").mkdir()
        (self.target / "logs" / "session" / "hook.log").write_bytes(b"log")
        (self.target / "captures" / "clip.mkv").write_bytes(b"recording")
        self.install(2)
        self.assertEqual(self.content("config.ini"), b"[Output]\r\noutput_dir=D:\\mine\r\n")
        self.assertEqual(self.content("config.ini.new"), CONFIG_V2)
        self.assertEqual(self.content("captureengine.exe"), expected(2, "captureengine.exe"))
        self.assertEqual(self.content("notes.txt"), b"my notes")
        self.assertEqual(self.content("logs/session/hook.log"), b"log")
        self.assertEqual(self.content("captures/clip.mkv"), b"recording")
        # Stale: shipped by v1, not by v2. New: only in v2.
        self.assertFalse((self.target / "old_helper.dll").exists())
        self.assertEqual(self.content("new_helper.dll"), b"only in v2")
        self.assertEqual([p.name for p in self.target.rglob("*") if p.suffix in (".cenew", ".cebak")], [])

    def test_update_never_overwrites_an_edited_config_even_when_repeated(self):
        self.install(1)
        (self.target / "config.ini").write_bytes(b"edited")
        self.install(2)
        self.install(2)
        self.assertEqual(self.content("config.ini"), b"edited")
        self.assertEqual(self.content("config.ini.new"), CONFIG_V2)

    def test_install_over_an_unmanaged_copy_overwrites_same_named_files_only(self):
        # A copy extracted from the 7z archive: no manifest, user config present.
        (self.target / "ffmpeg").mkdir(parents=True)
        (self.target / "captureengine.exe").write_bytes(b"ancient")
        (self.target / "config.ini").write_bytes(b"[Mine]\r\n")
        (self.target / "extra.dll").write_bytes(b"third party")
        self.install(1)
        self.assertEqual(self.content("captureengine.exe"), expected(1, "captureengine.exe"))
        self.assertEqual(self.content("config.ini"), b"[Mine]\r\n")
        self.assertEqual(self.content("config.ini.new"), CONFIG_V1)
        self.assertEqual(self.content("extra.dll"), b"third party")

    # -- failure and in-use files ----------------------------------------------------

    def test_a_locked_file_rolls_everything_back(self):
        self.install(1)
        before = {p.name: p.read_bytes() for p in self.target.iterdir() if p.is_file()}
        # Python opens files without FILE_SHARE_DELETE, so the rename aside fails.
        with open(self.target / "mediaengine.dll", "rb"):
            self.install(2, expect=1)
        after = {p.name: p.read_bytes() for p in self.target.iterdir() if p.is_file()}
        self.assertEqual(before, after)
        self.assertEqual([p.name for p in self.target.rglob("*") if p.suffix in (".cenew", ".cebak")], [])

    def test_a_dll_loaded_by_a_running_process_is_replaced_by_renaming_it_aside(self):
        self.install(1)
        system_dll = Path(os.environ["SystemRoot"]) / "System32" / "version.dll"
        loaded_copy = self.target / "capture_hook_x64.dll"
        shutil.copy2(system_dll, loaded_copy)
        # Register it as part of this installation, as a previous version would have.
        manifest = self.target / "captureengine_install.manifest"
        manifest.write_bytes(manifest.read_bytes() + b"file=capture_hook_x64.dll\n")
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.LoadLibraryW.restype = ctypes.c_void_p
        kernel32.LoadLibraryW.argtypes = [ctypes.c_wchar_p]
        kernel32.FreeLibrary.argtypes = [ctypes.c_void_p]
        module = kernel32.LoadLibraryW(str(loaded_copy))
        self.assertTrue(module, "could not load the scratch DLL")
        try:
            self.install(2)
        finally:
            kernel32.FreeLibrary(module)
        self.assertEqual(self.content("captureengine.exe"), expected(2, "captureengine.exe"))
        # v2 does not ship the hook DLL, so it was stale; in use, it moved aside instead of failing.
        self.assertFalse(loaded_copy.exists())

    def test_a_link_inside_the_installation_is_never_written_through(self):
        outside = self.work / "outside-target"
        outside.mkdir()
        self.target.mkdir()
        subprocess.run(["cmd", "/c", "mklink", "/J", str(self.target / "ffmpeg"), str(outside)], check=True,
                       capture_output=True)
        self.install(1, expect=1)
        self.assertEqual(list(outside.iterdir()), [])
        self.assertFalse((self.target / "captureengine.exe").exists())

    def test_a_refused_install_never_cleans_up_through_a_junction(self):
        outside = self.work / "outside-cleanup"
        outside.mkdir()
        expected = {"private.cebak": b"user backup", "avcodec-63.dll.cenew": b"external staging"}
        for name, contents in expected.items():
            (outside / name).write_bytes(contents)
        self.target.mkdir()
        link = self.target / "ffmpeg"
        subprocess.run(["cmd", "/c", "mklink", "/J", str(link), str(outside)], check=True, capture_output=True)
        try:
            self.install(1, expect=1)
            for name, contents in expected.items():
                self.assertTrue((outside / name).exists(), name)
                self.assertEqual((outside / name).read_bytes(), contents)
        finally:
            link.rmdir()

    def test_unusable_folders_are_refused(self):
        self.run_setup(1, "--files-only", "--dir=C:\\", "/S", expect=1)
        self.run_setup(1, "--files-only", "--dir=relative\\folder", "/S", expect=1)
        self.run_setup(1, "--files-only", f"--dir=C:\\{'a' * 201}", "/S", expect=1)
        self.run_setup(1, "--files-only", "/S", expect=3)

    def test_a_running_program_from_the_installation_folder_is_closed_and_others_are_left_alone(self):
        self.install(1)
        ping = Path(os.environ["SystemRoot"]) / "System32" / "PING.EXE"
        outside = self.work / "outside-program"
        outside.mkdir(exist_ok=True)
        shutil.copy2(ping, self.target / "captureengine.exe")
        shutil.copy2(ping, outside / "captureengine.exe")
        running_inside = subprocess.Popen([str(self.target / "captureengine.exe"), "-t", "127.0.0.1"],
                                          stdout=subprocess.DEVNULL)
        running_outside = subprocess.Popen([str(outside / "captureengine.exe"), "-t", "127.0.0.1"],
                                           stdout=subprocess.DEVNULL)
        try:
            # A console program has no window to ask politely, so the short timeout
            # exercises the terminate fallback.
            self.run_setup(2, "--files-only", f"--dir={self.target}", "/S", "--close-timeout=1")
            running_inside.wait(timeout=10)
            self.assertIsNone(running_outside.poll(), "a program outside the installation folder was closed")
            self.assertEqual(self.content("captureengine.exe"), expected(2, "captureengine.exe"))
        finally:
            for process in (running_inside, running_outside):
                if process.poll() is None:
                    process.kill()
                process.wait(timeout=10)

    def test_the_folders_the_program_writes_to_are_open_to_ordinary_users(self):
        self.install(1)
        users = "*S-1-5-32-545"  # the Users group, independent of the Windows language
        for relative in ("logs", "captures", "screenshots", "benchmarks", "config.ini"):
            found = self.icacls_findsid(self.target / relative, users)
            self.assertIn(relative.lower(), found.lower(), f"no Users entry on {relative}")
        # The program files themselves are not.
        self.assertNotIn("captureengine.exe", self.icacls_findsid(self.target / "captureengine.exe", users).lower())

    # -- uninstall ---------------------------------------------------------------------

    def test_uninstall_removes_the_program_and_keeps_user_data(self):
        self.install(1)
        (self.target / "notes.txt").write_bytes(b"mine")
        (self.target / "config.ini").write_bytes(b"cfg")
        (self.target / "logs" / "a.log").write_bytes(b"log")
        (self.target / "captures" / "clip.mkv").write_bytes(b"rec")
        self.install(2)  # also leaves config.ini.new behind
        self.uninstall()
        remaining = sorted(str(p.relative_to(self.target)).replace("\\", "/") for p in self.target.rglob("*") if p.is_file())
        self.assertEqual(remaining, ["captures/clip.mkv", "config.ini", "logs/a.log", "notes.txt"])
        self.assertFalse((self.target / "ffmpeg").exists())
        self.assertFalse((self.target / "plugins").exists())

    def test_uninstall_with_remove_data_deletes_config_and_logs_but_never_recordings(self):
        self.install(1)
        (self.target / "logs" / "a.log").write_bytes(b"log")
        (self.target / "captures" / "clip.mkv").write_bytes(b"rec")
        self.uninstall("--remove-data")
        self.assertFalse((self.target / "config.ini").exists())
        self.assertFalse((self.target / "logs").exists())
        self.assertEqual(self.content("captures/clip.mkv"), b"rec")

    def test_remove_data_unlinks_the_logs_root_and_keeps_its_target(self):
        self.install(1)
        (self.target / "logs").rmdir()
        outside = self.work / "outside-recordings"
        outside.mkdir()
        canary = outside / "recording.mkv"
        canary.write_bytes(b"external recording")
        link = self.target / "logs"
        subprocess.run(["cmd", "/c", "mklink", "/J", str(link), str(outside)], check=True, capture_output=True)
        try:
            self.uninstall("--remove-data")
            self.assertTrue(canary.exists())
            self.assertEqual(canary.read_bytes(), b"external recording")
            self.assertFalse(link.exists())
        finally:
            if link.is_junction():
                link.rmdir()

    def test_uninstall_never_cleans_manifest_temporary_files_through_a_junction(self):
        self.install(1)
        shutil.rmtree(self.target / "ffmpeg")
        outside = self.work / "outside-uninstall-cleanup"
        outside.mkdir()
        canary = outside / "saved.cebak"
        canary.write_bytes(b"external backup")
        link = self.target / "ffmpeg"
        subprocess.run(["cmd", "/c", "mklink", "/J", str(link), str(outside)], check=True, capture_output=True)
        try:
            self.uninstall()
            self.assertTrue(canary.exists())
            self.assertEqual(canary.read_bytes(), b"external backup")
        finally:
            if link.is_junction():
                link.rmdir()

    def installed_uninstaller(self):
        binary = Path(os.environ.get("CE_UNINSTALLER") or STUB.parent / "captureengine_uninstall.exe")
        if not binary.is_file():
            self.skipTest("uninstaller not built")
        target = self.target / "captureengine_uninstall.exe"
        shutil.copy2(binary, target)
        return target

    def test_silent_installed_uninstaller_waits_for_removal_and_keeps_its_launcher_alive(self):
        self.install(1)
        uninstaller = self.installed_uninstaller()
        completed = subprocess.run([str(uninstaller), "--files-only", "/S", "--remove-data", "--close-timeout=1"],
                                   capture_output=True, timeout=20, check=False)
        self.assertEqual(completed.returncode, 0)
        for relative in ("captureengine.exe", "mediaengine.dll", "captureengine_install.manifest", "config.ini"):
            self.assertFalse((self.target / relative).exists(), relative)
        self.assertFalse(uninstaller.exists())

    def test_setup_inside_the_installation_hands_off_removal_instead_of_reinstalling(self):
        self.install(1)
        setup = self.target / "maintenance-setup.exe"
        shutil.copy2(self.setups[1], setup)
        manifest = self.target / "captureengine_install.manifest"
        manifest.write_bytes(manifest.read_bytes() + b"file=maintenance-setup.exe\n")
        completed = subprocess.run([str(setup), "--uninstall", "--files-only", "/S", f"--dir={self.target}",
                                    "--close-timeout=1"], capture_output=True, timeout=20, check=False)
        self.assertEqual(completed.returncode, 0)
        self.assertFalse((self.target / "captureengine.exe").exists())
        self.assertFalse(manifest.exists())
        self.assertFalse(setup.exists())

    def test_temporary_copy_rejects_an_unverifiable_elevation_launcher(self):
        self.install(1)
        uninstaller = self.installed_uninstaller()
        completed = subprocess.run([str(uninstaller), "--files-only", "/S", "--elevation-launcher=not-a-process"],
                                   capture_output=True, timeout=20, check=False)
        self.assertEqual(completed.returncode, 3)
        self.assertTrue(uninstaller.exists())
        self.assertTrue((self.target / "captureengine.exe").exists())

    def test_silent_installed_uninstaller_returns_the_temporary_copys_failure(self):
        self.target.mkdir()
        uninstaller = self.installed_uninstaller()
        completed = subprocess.run([str(uninstaller), "--files-only", "/S", "--close-timeout=1"],
                                   capture_output=True, timeout=20, check=False)
        self.assertEqual(completed.returncode, 1)
        self.assertTrue(uninstaller.exists())

    def test_uninstall_without_a_manifest_leaves_the_files(self):
        self.target.mkdir()
        (self.target / "captureengine.exe").write_bytes(b"exe")
        self.uninstall()
        self.assertTrue((self.target / "captureengine.exe").exists())

    def test_uninstall_of_an_empty_folder_reports_failure(self):
        self.target.mkdir()
        self.uninstall(expect=1)


if __name__ == "__main__":
    unittest.main()
