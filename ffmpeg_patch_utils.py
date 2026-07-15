# MIT License
#
# Copyright (c) 2026 aufkrawall
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

import os
from pathlib import Path
from typing import Iterable, List


class CustomPatchTargetError(RuntimeError):
    """A custom patch names a target that is unsafe or unsuitable to normalize."""


def _decode_patch_target(encoded_path: bytes, prefix: bytes, patch_path: str) -> str:
    encoded_path = encoded_path.split(b"\t", 1)[0]
    if encoded_path == b"/dev/null":
        return ""
    if encoded_path.startswith(b'"'):
        raise CustomPatchTargetError(f"Quoted target paths are unsupported in custom patch: {patch_path}")
    if not encoded_path.startswith(prefix):
        raise CustomPatchTargetError(
            f"Custom patch target must use the {prefix.decode('ascii')} prefix: {patch_path}"
        )
    try:
        return encoded_path[len(prefix) :].decode("utf-8")
    except UnicodeDecodeError as error:
        raise CustomPatchTargetError(f"Custom patch target is not valid UTF-8: {patch_path}") from error


def _text_patch_targets(patch_path: str) -> List[str]:
    patch_data = Path(patch_path).read_bytes()
    targets: List[str] = []
    seen = set()
    in_file_header = False
    waiting_for_new_path = False

    for line in patch_data.splitlines():
        if line.startswith(b"diff --git "):
            in_file_header = True
            waiting_for_new_path = False
            continue
        if not in_file_header:
            continue
        if line.startswith((b"@@ ", b"GIT binary patch", b"Binary files ")):
            in_file_header = False
            waiting_for_new_path = False
            continue
        if line.startswith(b"--- "):
            relative_path = _decode_patch_target(line[4:], b"a/", patch_path)
            if relative_path and relative_path not in seen:
                seen.add(relative_path)
                targets.append(relative_path)
            waiting_for_new_path = True
            continue
        if not waiting_for_new_path:
            continue
        if not line.startswith(b"+++ "):
            raise CustomPatchTargetError(f"Malformed target header in custom patch: {patch_path}")

        relative_path = _decode_patch_target(line[4:], b"b/", patch_path)
        waiting_for_new_path = False
        if relative_path and relative_path not in seen:
            seen.add(relative_path)
            targets.append(relative_path)

    return targets


def _resolve_patch_target(build_root: Path, relative_path: str, patch_path: str) -> Path:
    if "\\" in relative_path:
        raise CustomPatchTargetError(f"Custom patch target uses a Windows separator: {patch_path}: {relative_path}")

    parts = relative_path.split("/")
    if (
        not relative_path
        or any(part in ("", ".", "..") or ":" in part for part in parts)
        or parts[0].casefold() == ".git"
    ):
        raise CustomPatchTargetError(f"Unsafe custom patch target: {patch_path}: {relative_path}")

    target = build_root.joinpath(*parts)
    resolved_target = target.resolve(strict=False)
    try:
        common_path = os.path.commonpath((str(build_root), str(resolved_target)))
    except ValueError as error:
        raise CustomPatchTargetError(f"Custom patch target escapes the build directory: {patch_path}") from error
    if os.path.normcase(common_path) != os.path.normcase(str(build_root)):
        raise CustomPatchTargetError(f"Custom patch target escapes the build directory: {patch_path}: {relative_path}")
    return target


def normalize_custom_patch_targets(build_dir: str, patch_paths: Iterable[str]) -> List[str]:
    """Normalize CRLF in disposable text targets before strict ``git apply``.

    A Windows Git configuration can check the upstream FFmpeg worktree out as
    CRLF even though project patches are intentionally LF. Only files named by
    text patch headers are touched, and every target must remain inside the
    disposable FFmpeg build directory.
    """

    build_root = Path(build_dir).resolve(strict=True)
    patch_targets = []
    seen = set()
    target_origins = {}
    for patch_path in patch_paths:
        for relative_path in _text_patch_targets(patch_path):
            if relative_path not in seen:
                seen.add(relative_path)
                patch_targets.append(relative_path)
                target_origins[relative_path] = patch_path

    normalized = []
    for relative_path in patch_targets:
        target = _resolve_patch_target(build_root, relative_path, target_origins[relative_path])
        if not target.exists():
            continue
        if not target.is_file():
            raise CustomPatchTargetError(f"Custom patch target is not a file: {target}")

        content = target.read_bytes()
        if b"\x00" in content:
            raise CustomPatchTargetError(f"Refusing to normalize binary custom patch target: {target}")
        standalone_carriage_returns = content.count(b"\r") - content.count(b"\r\n")
        if standalone_carriage_returns:
            raise CustomPatchTargetError(f"Custom patch target has unsupported CR-only line endings: {target}")
        if b"\r\n" not in content:
            continue

        target.write_bytes(content.replace(b"\r\n", b"\n"))
        normalized.append(relative_path)

    return normalized
