"""Self-tests for the verified LibreHardwareMonitor runtime-file acquisition.

These cover the properties that keep an untrusted archive from putting an
unexpected or unverified file into the shipped plugin directory.
"""

import io
import json
import os
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

import build  # noqa: E402


# A minimal PE stub: the installer only checks the DOS signature before handing
# the file to the Authenticode check.
PE_STUB = b"MZ" + b"\0" * 126


def _make_archive(members: dict) -> bytes:
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w") as archive:
        for name, payload in members.items():
            archive.writestr(name, payload)
    return buffer.getvalue()


def _full_member_set(**overrides) -> dict:
    members = {name: PE_STUB for name in build.LHM_PLUGIN_FILES}
    members.update(overrides)
    return members


class LhmPluginAcquisitionTest(unittest.TestCase):
    def _select(self, members: dict):
        with tempfile.TemporaryDirectory() as temporary:
            archive_path = Path(temporary) / "archive.zip"
            archive_path.write_bytes(_make_archive(members))
            with zipfile.ZipFile(archive_path, "r") as archive:
                return build._select_archive_members(archive)

    def test_selects_exactly_the_four_pinned_files(self) -> None:
        selected = self._select(
            _full_member_set(
                **{
                    "LibreHardwareMonitor.exe": PE_STUB,
                    "OxyPlot.dll": PE_STUB,
                    "LibreHardwareMonitorLib.pdb": b"pdb",
                    "HidSharp.dll": PE_STUB,
                }
            )
        )
        self.assertEqual(set(selected), set(build.LHM_PLUGIN_FILES))
        self.assertEqual(len(build.LHM_PLUGIN_FILES), 4)

    def test_rejects_an_archive_missing_a_pinned_file(self) -> None:
        members = _full_member_set()
        del members["System.Memory.dll"]
        with self.assertRaisesRegex(RuntimeError, "missing expected file"):
            self._select(members)

    def test_rejects_a_duplicate_candidate_instead_of_choosing_one(self) -> None:
        members = _full_member_set(**{"nested/LibreHardwareMonitorLib.dll": PE_STUB})
        with self.assertRaisesRegex(RuntimeError, "more than one"):
            self._select(members)

    def test_rejects_an_oversized_member(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            archive_path = Path(temporary) / "archive.zip"
            archive_path.write_bytes(_make_archive(_full_member_set()))
            with zipfile.ZipFile(archive_path, "r") as archive:
                for member in archive.infolist():
                    member.file_size = build.LHM_MEMBER_MAXIMUM_BYTES + 1
                with self.assertRaisesRegex(RuntimeError, "larger than expected"):
                    build._select_archive_members(archive)

    def test_member_paths_cannot_escape_the_destination(self) -> None:
        # A traversal path whose base name is on the allowlist must still land
        # in the plugin directory: destinations are built from the constant.
        traversal = "../../../evil/LibreHardwareMonitorLib.dll"
        members = _full_member_set()
        del members["LibreHardwareMonitorLib.dll"]
        members[traversal] = PE_STUB
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive_path = root / "archive.zip"
            archive_path.write_bytes(_make_archive(members))
            destination = root / "plugins"
            with patch.object(build, "pe_has_authenticode_certificate", return_value=True):
                digests = build._install_archive_members(str(archive_path), str(destination))
            self.assertEqual(set(digests), set(build.LHM_PLUGIN_FILES))
            for name in build.LHM_PLUGIN_FILES:
                self.assertTrue((destination / name).is_file())
            self.assertFalse((root / "evil").exists())
            self.assertEqual(sorted(p.name for p in root.iterdir()), ["archive.zip", "plugins"])

    def test_rejects_a_member_that_is_not_a_windows_image(self) -> None:
        members = _full_member_set(**{"System.Memory.dll": b"#!/bin/sh\necho not a dll\n"})
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive_path = root / "archive.zip"
            archive_path.write_bytes(_make_archive(members))
            with patch.object(build, "pe_has_authenticode_certificate", return_value=True):
                with self.assertRaisesRegex(RuntimeError, "not a Windows executable image"):
                    build._install_archive_members(str(archive_path), str(root / "plugins"))

    def test_rejects_an_unsigned_microsoft_dependency(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive_path = root / "archive.zip"
            archive_path.write_bytes(_make_archive(_full_member_set()))
            destination = root / "plugins"
            with patch.object(build, "pe_has_authenticode_certificate", return_value=False):
                with self.assertRaisesRegex(RuntimeError, "not Authenticode signed"):
                    build._install_archive_members(str(archive_path), str(destination))
            # The refused file must not be left behind for packaging to pick up.
            for name in build.LHM_MICROSOFT_SIGNED_FILES:
                self.assertFalse((destination / name).exists())

    def test_the_library_itself_is_not_required_to_be_signed(self) -> None:
        # LibreHardwareMonitorLib.dll ships unsigned upstream; the pinned archive
        # digest is what vouches for it.
        self.assertNotIn("LibreHardwareMonitorLib.dll", build.LHM_MICROSOFT_SIGNED_FILES)
        self.assertEqual(len(build.LHM_MICROSOFT_SIGNED_FILES), 3)

    def test_archive_is_pinned_by_https_url_size_and_digest(self) -> None:
        self.assertTrue(build.LHM_ARCHIVE_URL.startswith("https://github.com/LibreHardwareMonitor/"))
        self.assertIn(build.LHM_RELEASE_TAG, build.LHM_ARCHIVE_URL)
        self.assertEqual(len(build.LHM_ARCHIVE_SHA256), 64)
        self.assertEqual(build.LHM_ARCHIVE_SHA256, build.LHM_ARCHIVE_SHA256.lower())
        self.assertGreater(build.LHM_ARCHIVE_SIZE, 0)
        self.assertLess(build.LHM_ARCHIVE_SIZE, build.LHM_ARCHIVE_MAXIMUM_BYTES)

    def test_every_pinned_file_is_allowed_into_the_release_archive(self) -> None:
        for name in build.LHM_PLUGIN_FILES:
            expected = f"plugins/librehardwaremonitor/{name.lower()}"
            self.assertIn(expected, build.CAPTURE_PACKAGE_PLUGIN_FILES)
        self.assertIn("plugins/librehardwaremonitor/pawnio_setup.exe", build.CAPTURE_PACKAGE_PLUGIN_FILES)
        # Nothing beyond the pinned files, installer and setup notes may be packaged.
        self.assertEqual(
            build.CAPTURE_PACKAGE_PLUGIN_FILES,
            {
                "plugins/librehardwaremonitor/readme.txt",
                "plugins/librehardwaremonitor/pawnio_setup.exe",
            }
            | {f"plugins/librehardwaremonitor/{name.lower()}" for name in build.LHM_PLUGIN_FILES},
        )

    def test_currency_check_rejects_a_tampered_or_stale_installation(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            destination = Path(temporary) / "plugins"
            destination.mkdir()
            for name in build.LHM_PLUGIN_FILES:
                (destination / name).write_bytes(PE_STUB)
            manifest = {
                "release": build.LHM_RELEASE_TAG,
                "archive_sha256": build.LHM_ARCHIVE_SHA256,
                "files": {name: build.sha256_file(str(destination / name)) for name in build.LHM_PLUGIN_FILES},
            }
            manifest_path = destination / build.LHM_MANIFEST_NAME
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
            with patch.object(build, "lhm_plugin_directory", return_value=str(destination)):
                self.assertTrue(build.lhm_plugin_files_are_current())
                (destination / "System.Memory.dll").write_bytes(PE_STUB + b"tampered")
                self.assertFalse(build.lhm_plugin_files_are_current())
                (destination / "System.Memory.dll").write_bytes(PE_STUB)
                self.assertTrue(build.lhm_plugin_files_are_current())
                manifest["release"] = "v0.0.1"
                manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
                self.assertFalse(build.lhm_plugin_files_are_current())

    def test_missing_installation_is_not_reported_as_current(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            with patch.object(build, "lhm_plugin_directory", return_value=temporary):
                self.assertFalse(build.lhm_plugin_files_are_current())


if __name__ == "__main__":
    unittest.main()
