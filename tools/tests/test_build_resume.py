# build.py executes its fragments via exec, so its module attributes exist only
# at runtime; pyright cannot see them through the facade.
# pyright: reportAttributeAccessIssue=false

import json
import tempfile
import unittest
from pathlib import Path

import build


class BuildResumePolicyTest(unittest.TestCase):
    def test_resume_requires_matching_immediately_failed_build_identity(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            header = Path(temporary) / "build_version.h"
            manifest = Path(temporary) / "latest_manifest.json"
            header.write_text("#define BUILD_NUMBER 8765\n", encoding="utf-8")
            failed_state = {
                "top_level": True,
                "success": False,
                "build_number": 8765,
                "build_script_sha256": build.sha256_file(build.__file__),
                "args": ["--skip-updates"],
            }
            manifest.write_text(json.dumps(failed_state), encoding="utf-8")

            self.assertEqual(build.read_failed_build_resume_version(str(manifest), str(header)), 8765)

            failed_state["success"] = True
            manifest.write_text(json.dumps(failed_state), encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "did not fail"):
                build.read_failed_build_resume_version(str(manifest), str(header))

            failed_state["success"] = False
            failed_state["build_script_sha256"] = "stale"
            manifest.write_text(json.dumps(failed_state), encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "build.py changed"):
                build.read_failed_build_resume_version(str(manifest), str(header))

    def test_resume_restores_verification_mode_from_the_failed_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            header = Path(temporary) / "build_version.h"
            manifest = Path(temporary) / "latest_manifest.json"
            header.write_text("#define BUILD_NUMBER 8765\n", encoding="utf-8")
            failed_state = {
                "top_level": True,
                "success": False,
                "build_number": 8765,
                "build_script_sha256": build.sha256_file(build.__file__),
                "mode": "verify",
                "args": ["--resume", "--skip-updates", "--concise"],
            }
            manifest.write_text(json.dumps(failed_state), encoding="utf-8")

            self.assertEqual(
                build.read_failed_build_resume_state(str(manifest), str(header)),
                (8765, True),
            )

            # Backward compatibility for a failed manifest written before mode
            # was authoritative: an explicit prior --verify still restores it.
            failed_state["mode"] = "build"
            failed_state["args"] = ["--verify", "--skip-updates"]
            manifest.write_text(json.dumps(failed_state), encoding="utf-8")
            self.assertEqual(
                build.read_failed_build_resume_state(str(manifest), str(header)),
                (8765, True),
            )

            failed_state["args"] = ["--skip-updates"]
            manifest.write_text(json.dumps(failed_state), encoding="utf-8")
            self.assertEqual(
                build.read_failed_build_resume_state(str(manifest), str(header)),
                (8765, False),
            )

        source = build.read_source_text()
        self.assertIn("resume_build_number, resume_verification_mode = read_failed_build_resume_state()", source)
        self.assertIn("if resume_verification_mode and not verify_flag:", source)
        self.assertIn("Resume restored verification mode from the failed top-level run", source)


if __name__ == "__main__":
    unittest.main()
