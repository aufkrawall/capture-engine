# pyright: reportAttributeAccessIssue=false
"""Build-time invariants of the setup program that no run would reveal."""

import re
import tempfile
import unittest
import xml.dom.minidom
from pathlib import Path

import build

INSTALLER = Path(build.PROJECT_ROOT) / "installer"


class InstallerBuildTest(unittest.TestCase):
    def test_every_source_file_belongs_to_the_setup_or_the_uninstaller(self):
        listed = set(build.INSTALLER_COMMON_SOURCES) | set(build.INSTALLER_SETUP_ONLY_SOURCES)
        on_disk = {path.name for path in INSTALLER.glob("*.cpp")}
        self.assertEqual(on_disk, listed, "a new installer/*.cpp must be added to the build lists")

    def test_the_uninstaller_image_leaves_out_the_payload_reader_and_install_engine(self):
        names = {Path(path).name for path in build.installer_sources(uninstaller=True)}
        self.assertNotIn("payload.cpp", names)
        self.assertNotIn("engine_install.cpp", names)
        self.assertIn("engine_uninstall.cpp", names)
        setup_names = {Path(path).name for path in build.installer_sources(uninstaller=False)}
        self.assertTrue({"payload.cpp", "engine_install.cpp"} <= setup_names)

    def test_shared_units_never_load_the_payload_decoder(self):
        for name in build.INSTALLER_COMMON_SOURCES:
            self.assertNotIn("cabinet.dll", (INSTALLER / name).read_text(encoding="utf-8"), name)

    def test_the_per_build_version_header_is_confined_to_one_unit(self):
        # build_version.h changes on every product build; including it elsewhere
        # would recompile the whole installer each time.
        offenders = [
            path.name
            for path in list(INSTALLER.glob("*.cpp")) + list(INSTALLER.glob("*.h"))
            if "build_version.h" in path.read_text(encoding="utf-8") and path.name != "version.cpp"
        ]
        self.assertEqual(offenders, [])

    def test_manifests_are_well_formed_and_do_not_request_elevation(self):
        for name in ("setup.manifest", "uninstall.manifest"):
            text = (INSTALLER / name).read_text(encoding="utf-8")
            xml.dom.minidom.parseString(text.encode("utf-8"))
            # "--" inside an XML comment is illegal and makes Windows refuse to start the
            # program with a side-by-side configuration error.
            for comment in re.findall(r"<!--(.*?)-->", text, re.S):
                self.assertNotIn("--", comment, name)
            self.assertIn('level="asInvoker"', text, name)
            self.assertIn("PerMonitorV2", text, name)
        self.assertNotEqual(
            (INSTALLER / "setup.manifest").read_text(encoding="utf-8"),
            (INSTALLER / "uninstall.manifest").read_text(encoding="utf-8"),
        )

    def test_resource_script_selects_the_matching_manifest_and_names(self):
        rc = (INSTALLER / "setup.rc").read_text(encoding="utf-8")
        self.assertIn('"uninstall.manifest"', rc)
        self.assertIn('"setup.manifest"', rc)
        self.assertIn("captureengine_uninstall.exe", rc)
        self.assertIn("101 ICON", rc)
        self.assertIn("kIconResource = 101", (INSTALLER / "setup.h").read_text(encoding="utf-8"))

    def test_the_payload_requirements_are_shippable_by_the_archive_policy(self):
        for member in build.INSTALLER_REQUIRED_MEMBERS:
            if member in ("captureengine_uninstall.exe", "config.ini"):
                continue  # added from the installer build / the default template, not copied from the tree
            self.assertTrue(build._capture_package_file_allowed(member), member)

    def test_the_uninstaller_surface_check_rejects_install_code(self):
        with tempfile.TemporaryDirectory() as folder:
            clean = Path(folder) / "clean.exe"
            clean.write_bytes(b"MZ" + "captureengine_uninstall".encode("utf-16le"))
            build.verify_uninstaller_surface(str(clean))
            for marker in ("CESETUP1", "cabinet.dll", "CreateDecompressor"):
                dirty = Path(folder) / "dirty.exe"
                dirty.write_bytes(b"MZ" + b"captureengine_uninstall" + marker.encode("ascii"))
                with self.assertRaises(RuntimeError, msg=marker):
                    build.verify_uninstaller_surface(str(dirty))
                dirty.write_bytes(b"MZ" + b"captureengine_uninstall" + marker.encode("utf-16le"))
                with self.assertRaises(RuntimeError, msg=marker):
                    build.verify_uninstaller_surface(str(dirty))

    def test_the_release_workflow_publishes_the_setup_program(self):
        workflow = (Path(build.PROJECT_ROOT) / ".github" / "workflows" / "release-stable.yml").read_text(encoding="utf-8")
        self.assertEqual(workflow.count("build/packages/captureengine-setup.exe"), 2)  # attestation and upload
        self.assertEqual(build.SETUP_PACKAGE_NAME, "captureengine-setup.exe")

    def test_installer_sources_are_inside_every_source_scope_list(self):
        self.assertIn("installer", build.LINTABLE_SOURCE_DIRS)
        from tools import verification_stage_cache

        self.assertIn("installer", verification_stage_cache.SOURCE_DIRS)


if __name__ == "__main__":
    unittest.main()
