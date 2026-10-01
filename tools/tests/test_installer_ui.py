"""Click through the real setup window with real mouse messages.

`--files-only` (without /S) opens the genuine wizard off-screen and limits its
work to the file operations, so the whole page flow - license, folder, options,
progress, result - runs against a scratch folder without a visible window or any
system change. Needs the setup stub from the product build (see
test_installer_native.py) and skips when it is absent.
"""

import ctypes
import os
import shutil
import subprocess
import tempfile
import time
import unittest
from ctypes import wintypes
from pathlib import Path

from tools import installer_payload as payload
from tools.tests.test_installer_native import STUB, payload_files

WINDOW_CLASS = "CaptureEngineSetupWindow"
ID_ACCEPT, ID_PATH, ID_BROWSE = 1001, 1002, 1003
ID_DESKTOP = 1004
ID_REMOVE_DATA, ID_BACK, ID_NEXT, ID_CANCEL = 1011, 1012, 1013, 1014
ID_LICENSE = 1015

WM_NULL, WM_GETTEXT, WM_GETTEXTLENGTH, WM_SETTEXT = 0x0000, 0x000D, 0x000E, 0x000C
WM_LBUTTONDOWN, WM_LBUTTONUP = 0x0201, 0x0202
BM_GETCHECK = 0x00F0


def _user32():
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    user32.FindWindowW.restype = wintypes.HWND
    user32.FindWindowW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR]
    user32.GetDlgItem.restype = wintypes.HWND
    user32.GetDlgItem.argtypes = [wintypes.HWND, ctypes.c_int]
    user32.SendMessageW.restype = ctypes.c_ssize_t
    user32.SendMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    user32.IsWindowEnabled.argtypes = [wintypes.HWND]
    user32.IsWindowVisible.argtypes = [wintypes.HWND]
    user32.GetClientRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
    return user32


@unittest.skipUnless(os.name == "nt" and STUB.is_file(), "setup stub not built")
class SetupWindowTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.user32 = _user32()
        cls.work = Path(tempfile.mkdtemp(prefix="ce_setup_ui_"))
        cls.setup = cls.work / "setup.exe"
        cls.setup.write_bytes(payload.assemble(STUB.read_bytes(), payload_files(1)))

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.work, ignore_errors=True)

    def setUp(self):
        self.target = self.work / f"target-{self._testMethodName}"
        self.process = None

    def tearDown(self):
        if self.process and self.process.poll() is None:
            self.process.kill()
            self.process.wait(timeout=10)

    # -- helpers ----------------------------------------------------------------

    def launch(self, *arguments):
        self.process = subprocess.Popen(
            [str(self.setup), "--files-only", f"--dir={self.target}", *arguments],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            window = self.user32.FindWindowW(WINDOW_CLASS, None)
            if window:
                # The window exists a moment before its controls do; a message round trip
                # only completes once the setup's message loop is running.
                self.user32.SendMessageW(window, WM_NULL, 0, 0)
                return window
            self.assertIsNone(self.process.poll(), "setup exited before showing its window")
            time.sleep(0.05)
        self.fail("the setup window did not appear")

    def control(self, window, control_id):
        handle = self.user32.GetDlgItem(window, control_id)
        self.assertTrue(handle, f"control {control_id} does not exist")
        return handle

    def text(self, handle):
        length = self.user32.SendMessageW(handle, WM_GETTEXTLENGTH, 0, 0)
        buffer = ctypes.create_unicode_buffer(length + 1)
        self.user32.SendMessageW(handle, WM_GETTEXT, length + 1, ctypes.addressof(buffer))
        return buffer.value

    def set_text(self, handle, value):
        buffer = ctypes.create_unicode_buffer(value)  # must outlive the call
        self.user32.SendMessageW(handle, WM_SETTEXT, 0, ctypes.addressof(buffer))

    def checked(self, handle):
        return self.user32.SendMessageW(handle, BM_GETCHECK, 0, 0) == 1

    def enabled(self, window, control_id):
        return bool(self.user32.IsWindowEnabled(self.control(window, control_id)))

    def visible(self, window, control_id):
        return bool(self.user32.IsWindowVisible(self.control(window, control_id)))

    def click(self, window, control_id):
        handle = self.control(window, control_id)
        rect = wintypes.RECT()
        self.user32.GetClientRect(handle, ctypes.byref(rect))
        position = ((rect.bottom // 2) << 16) | (rect.right // 2)
        self.user32.SendMessageW(handle, WM_LBUTTONDOWN, 1, position)
        self.user32.SendMessageW(handle, WM_LBUTTONUP, 0, position)

    def wait_until(self, condition, message, timeout=60):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if condition():
                return
            time.sleep(0.05)
        self.fail(message)

    def to_options_page(self, window):
        self.click(window, ID_ACCEPT)
        self.click(window, ID_NEXT)
        self.assertTrue(self.visible(window, ID_PATH))
        self.click(window, ID_NEXT)
        self.assertTrue(self.visible(window, ID_DESKTOP))

    # -- tests --------------------------------------------------------------------

    def test_the_license_must_be_accepted_before_continuing(self):
        window = self.launch()
        self.assertTrue(self.visible(window, ID_LICENSE))
        self.assertFalse(self.enabled(window, ID_NEXT))
        self.assertFalse(self.visible(window, ID_BACK))
        self.click(window, ID_ACCEPT)
        self.assertTrue(self.enabled(window, ID_NEXT))
        self.click(window, ID_ACCEPT)
        self.assertFalse(self.enabled(window, ID_NEXT))

    def test_cancel_exits_with_the_cancelled_code_and_installs_nothing(self):
        window = self.launch()
        self.click(window, ID_CANCEL)
        self.assertEqual(self.process.wait(timeout=20), 2)
        self.assertFalse(self.target.exists())

    def test_folder_page_shows_the_target_and_rejects_unusable_folders(self):
        window = self.launch()
        self.click(window, ID_ACCEPT)
        self.click(window, ID_NEXT)
        edit = self.control(window, ID_PATH)
        self.assertEqual(self.text(edit), str(self.target))
        self.assertTrue(self.enabled(window, ID_NEXT))
        for bad in ("C:\\", "relative\\folder", "", "\\\\server\\share\\ce", "C:\\a\\..\\b"):
            self.set_text(edit, bad)
            self.assertFalse(self.enabled(window, ID_NEXT), bad)
        self.set_text(edit, str(self.target))
        self.assertTrue(self.enabled(window, ID_NEXT))

    def test_back_returns_to_the_previous_page(self):
        window = self.launch()
        self.click(window, ID_ACCEPT)
        self.click(window, ID_NEXT)
        self.assertTrue(self.visible(window, ID_BACK))
        self.click(window, ID_BACK)
        self.assertTrue(self.visible(window, ID_LICENSE))
        self.assertTrue(self.checked(self.control(window, ID_ACCEPT)))

    def test_full_install_through_the_pages(self):
        window = self.launch()
        self.to_options_page(window)
        desktop = self.control(window, ID_DESKTOP)
        self.assertTrue(self.checked(desktop))
        self.click(window, ID_DESKTOP)
        self.assertFalse(self.checked(desktop))
        self.assertIn(self.text(self.control(window, ID_NEXT)), ("Install", "Update"))
        self.click(window, ID_NEXT)
        self.wait_until(lambda: self.text(self.control(window, ID_NEXT)) == "Finish", "setup never reached its result page")
        self.assertFalse(self.visible(window, ID_CANCEL))
        self.click(window, ID_NEXT)
        self.assertEqual(self.process.wait(timeout=20), 0)
        for path, expected in payload_files(1):
            self.assertEqual((self.target / path).read_bytes(), expected, path)

    def test_a_failed_install_ends_with_a_failure_exit_code(self):
        # A file where the install folder should be makes the install fail on the options page.
        self.target.parent.mkdir(parents=True, exist_ok=True)
        self.target.write_bytes(b"in the way")
        window = self.launch()
        self.click(window, ID_ACCEPT)
        self.click(window, ID_NEXT)
        # The folder page already refuses it or the install fails afterwards; both end non-zero.
        if self.enabled(window, ID_NEXT):
            self.click(window, ID_NEXT)
            self.click(window, ID_NEXT)
            self.wait_until(lambda: self.text(self.control(window, ID_NEXT)) == "Finish", "no result page")
            self.click(window, ID_NEXT)
            self.assertEqual(self.process.wait(timeout=20), 1)
        else:
            self.click(window, ID_CANCEL)
            self.assertEqual(self.process.wait(timeout=20), 2)
        self.assertEqual(self.target.read_bytes(), b"in the way")

    def test_uninstall_window_confirms_removes_and_keeps_data_by_default(self):
        subprocess.run(
            [str(self.setup), "--files-only", f"--dir={self.target}", "/S"], check=True, timeout=120, capture_output=True
        )
        (self.target / "logs" / "a.log").write_bytes(b"log")
        window = self.launch("--uninstall")
        self.assertEqual(self.text(self.control(window, ID_NEXT)), "Uninstall")
        self.assertFalse(self.checked(self.control(window, ID_REMOVE_DATA)))
        self.click(window, ID_NEXT)
        self.wait_until(lambda: self.text(self.control(window, ID_NEXT)) == "Finish", "uninstall never finished")
        self.click(window, ID_NEXT)
        self.assertEqual(self.process.wait(timeout=20), 0)
        self.assertFalse((self.target / "captureengine.exe").exists())
        self.assertEqual((self.target / "config.ini").read_bytes(), dict(payload_files(1))["config.ini"])
        self.assertEqual((self.target / "logs" / "a.log").read_bytes(), b"log")


if __name__ == "__main__":
    unittest.main()
