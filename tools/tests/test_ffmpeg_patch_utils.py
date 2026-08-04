import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from tools.ffmpeg_patch_utils import CustomPatchTargetError, normalize_custom_patch_targets


PROJECT_ROOT = Path(__file__).resolve().parents[2]
PATCH_DIR = PROJECT_ROOT / "tools" / "patches" / "ffmpeg"
PINNED_FFMPEG = PROJECT_ROOT / "ffmpeg_build" / "repos" / "ffmpeg"


def added_patch_lines(patch_text: str) -> str:
    return "\n".join(
        line[1:]
        for line in patch_text.splitlines()
        if line.startswith("+") and not line.startswith("+++")
    )


class FfmpegCustomPatchTest(unittest.TestCase):
    def test_project_patches_apply_strictly_to_pinned_ffmpeg(self) -> None:
        git = shutil.which("git")
        if git is None:
            self.fail("git is required to validate the project FFmpeg patches")

        patches = [
            PATCH_DIR / "0001-matroska-add-timestamp-precision-option.patch",
            PATCH_DIR / "0002-nvenc-bframe-cfr-improvements.patch",
        ]
        targets = [
            Path("libavformat/matroskaenc.c"),
            Path("libavcodec/nvenc.c"),
            Path("libavcodec/nvenc.h"),
            Path("libavcodec/nvenc_av1.c"),
            Path("libavcodec/nvenc_h264.c"),
            Path("libavcodec/nvenc_hevc.c"),
        ]

        # Only a host that builds FFmpeg from source has the pinned checkout;
        # Linux consumes prebuilt MSYS2 FFmpeg packages and never creates it.
        # Skip loudly in that case rather than failing on a missing input, but
        # still fail when the checkout exists and is incomplete, because that is
        # a broken pin rather than an absent one.
        if not PINNED_FFMPEG.is_dir():
            self.skipTest(
                f"pinned FFmpeg checkout is absent at {PINNED_FFMPEG}; "
                "run a source FFmpeg build (python build.py) to validate the project patches"
            )
        missing = [str(relative) for relative in targets if not (PINNED_FFMPEG / relative).is_file()]
        if missing:
            self.fail(f"pinned FFmpeg checkout at {PINNED_FFMPEG} is missing patch targets: {', '.join(missing)}")

        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir) / "ffmpeg"
            root.mkdir()
            subprocess.run([git, "init", "--quiet"], cwd=root, check=True)
            for relative in targets:
                destination = root / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(PINNED_FFMPEG / relative, destination)

            normalize_custom_patch_targets(str(root), [str(patch) for patch in patches])
            subprocess.run(
                [git, "apply", "--check", "--verbose", *map(str, patches)],
                cwd=root,
                check=True,
            )
            subprocess.run([git, "apply", *map(str, patches)], cwd=root, check=True)

            nvenc = (root / "libavcodec/nvenc.c").read_text(encoding="utf-8")
            matroska = (root / "libavformat/matroskaenc.c").read_text(encoding="utf-8")
            self.assertIn("Automatic B-reference mode", nvenc)
            self.assertIn("max_qp_b is ignored in constant-QP mode", nvenc)
            self.assertIn("timestamp_precision", matroska)

    def test_matroska_patch_preserves_exact_timebase_and_block_range(self) -> None:
        patch_text = (PATCH_DIR / "0001-matroska-add-timestamp-precision-option.patch").read_text(
            encoding="utf-8"
        )
        added = added_patch_lines(patch_text)

        self.assertIn("duration_den = (int64_t)duration.den * timestamp_precision", added)
        self.assertIn("av_rescale(s->duration, 1000, mkv->timestamp_precision)", added)
        self.assertIn(
            "avpriv_set_pts_info(st, 64, (unsigned)mkv->timestamp_precision, 1000000000)",
            added,
        )
        self.assertIn("cluster_time < INT16_MIN", added)
        self.assertIn("INT16_MAX", added)
        self.assertNotIn("1000000000LL / mkv->timestamp_precision", added)

    def test_nvenc_patch_keeps_explicit_policy_distinct_from_preset_defaults(self) -> None:
        patch_text = (PATCH_DIR / "0002-nvenc-bframe-cfr-improvements.patch").read_text(
            encoding="utf-8"
        )
        added = added_patch_lines(patch_text)

        self.assertIn("if (ctx->aq >= 0)", added)
        self.assertIn("if (ctx->temporal_aq >= 0)", added)
        self.assertIn("if (ctx->rc_lookahead == 0)", added)
        self.assertIn("NV_ENC_CAPS_SUPPORT_BFRAME_REF_MODE", added)
        self.assertIn("max_qp_b", added)
        self.assertIn("NV_ENC_PIC_TYPE_INTRA_REFRESH", added)
        self.assertIn("NV_ENC_PIC_TYPE_SWITCH", added)
        self.assertNotIn("Reduced lookahead safety margin", added)
        self.assertNotIn("ff_nvenc_receive_packet", added)
        self.assertNotIn("capability not reported for AV1, attempting anyway", added)
        self.assertNotIn("treating as P-frame", added)

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
