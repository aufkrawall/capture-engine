import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from ffmpeg_patch_utils import CustomPatchTargetError, normalize_custom_patch_targets


class FfmpegCustomPatchTest(unittest.TestCase):
    def test_crlf_target_is_normalized_before_strict_git_apply(self) -> None:
        git = shutil.which("git")
        if git is None:
            self.fail("git is required to validate strict custom patch application")

        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            # build.py routes TEMP into the workspace, so make this fixture its
            # own repository instead of letting Git discover the parent checkout.
            subprocess.run([git, "init", "--quiet"], cwd=root, check=True)
            target = root / "libavformat" / "sample.c"
            target.parent.mkdir()
            target.write_bytes(b"first\r\nold value\r\nlast\r\n")
            patch = root / "change.patch"
            patch.write_bytes(
                b"diff --git a/libavformat/sample.c b/libavformat/sample.c\n"
                b"--- a/libavformat/sample.c\n"
                b"+++ b/libavformat/sample.c\n"
                b"@@ -1,3 +1,3 @@\n"
                b" first\n"
                b"-old value\n"
                b"+new value\n"
                b" last\n"
            )

            self.assertEqual(
                normalize_custom_patch_targets(str(root), [str(patch)]),
                ["libavformat/sample.c"],
            )
            self.assertEqual(target.read_bytes(), b"first\nold value\nlast\n")
            subprocess.run([git, "apply", "--check", str(patch)], cwd=root, check=True)
            subprocess.run([git, "apply", str(patch)], cwd=root, check=True)
            self.assertIn(
                target.read_bytes(),
                (b"first\nnew value\nlast\n", b"first\r\nnew value\r\nlast\r\n"),
            )

    def test_target_traversal_is_rejected_before_any_file_is_changed(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir) / "working"
            root.mkdir()
            outside = Path(temp_dir) / "outside.c"
            outside.write_bytes(b"old\r\n")
            patch = Path(temp_dir) / "escape.patch"
            patch.write_bytes(
                b"diff --git a/../outside.c b/../outside.c\n"
                b"--- a/../outside.c\n"
                b"+++ b/../outside.c\n"
                b"@@ -1 +1 @@\n-old\n+new\n"
            )

            with self.assertRaises(CustomPatchTargetError):
                normalize_custom_patch_targets(str(root), [str(patch)])
            self.assertEqual(outside.read_bytes(), b"old\r\n")

    def test_duplicate_targets_are_normalized_once(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            target = root / "source.c"
            target.write_bytes(b"line\r\n")
            patches = []
            for index in range(2):
                patch = root / f"change-{index}.patch"
                patch.write_bytes(
                    b"diff --git a/source.c b/source.c\n"
                    b"--- a/source.c\n"
                    b"+++ b/source.c\n"
                    b"@@ -1 +1 @@\n-line\n+changed\n"
                )
                patches.append(str(patch))

            self.assertEqual(normalize_custom_patch_targets(str(root), patches), ["source.c"])
            self.assertEqual(target.read_bytes(), b"line\n")

    def test_deleted_crlf_target_is_normalized_from_old_path_header(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            target = root / "deleted.c"
            target.write_bytes(b"old\r\n")
            patch = root / "delete.patch"
            patch.write_bytes(
                b"diff --git a/deleted.c b/deleted.c\n"
                b"deleted file mode 100644\n"
                b"--- a/deleted.c\n"
                b"+++ /dev/null\n"
                b"@@ -1 +0,0 @@\n"
                b"-old\n"
            )

            self.assertEqual(normalize_custom_patch_targets(str(root), [str(patch)]), ["deleted.c"])
            self.assertEqual(target.read_bytes(), b"old\n")


if __name__ == "__main__":
    unittest.main()
