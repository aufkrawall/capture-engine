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

"""Rehearse the release job's dependency-closure phase locally.

Why this exists: five consecutive stable-release attempts failed in that phase,
each on a different fault, and none of them could occur locally. A normal local
build reuses state the release runner never has, so the release run was acting as
the first real test of the closure - at ten to forty minutes per attempt, plus a
manual runner start.

The differences this reproduces:

  * empty download cache - exercises every URL, TLS trust decision, retry path
    and detached-signature check (the cert-chain and dropped-TCP failures)
  * empty PGP keyring    - exercises importing the vendored keys (the aom key had
    no user ID, so gpg skipped it; run 31207385807)
  * runner path depth    - 89 characters by default against the runner's 73 and a
    dev checkout's 46, so a MAX_PATH fault surfaces here first (the opus doxygen
    man pages)

It drives the real SourceDependencyBuilder, so it cannot drift from what the
release actually runs. Nothing outside the throwaway root is touched: the real
`ffmpeg_build` tree, and therefore the closure your normal builds use, is left
alone.

    python tools/rehearse_dependency_closure.py

Takes roughly as long as the release job's closure phase. `--keep` leaves the tree
for inspection; `--root-length` widens the synthetic root to probe path limits.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
import time
from typing import List

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from tools import ffmpeg_dependencies as dependencies  # noqa: E402

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# Measured 2026-08-07: dev checkout 46 characters, self-hosted runner workspace 73.
DEV_ROOT_LENGTH = 46
RUNNER_ROOT_LENGTH = 73
DEFAULT_ROOT_LENGTH = 89


def is_path_derived_name(name: str) -> bool:
    """True when a generated file name has an absolute path baked into it.

    This is the defect signature itself rather than a guess about where such files
    land: doxygen's man backend replaces the separators of an absolute input path
    with underscores, giving names like
    `C__Users_..._src_opus-1.6.1_include_.3`. Matching the shape means a
    dependency that generates them somewhere new is still caught, and legitimate
    man pages a package installs itself (libiconv ships `share/man/man3`) are not
    flagged, because their names are ordinary.
    """
    return bool(re.match(r"^[A-Za-z]__(?:[A-Za-z0-9.+~-]+_)+", name))


def find_path_derived_names(root: str) -> List[str]:
    return sorted(
        os.path.join(current, name)
        for current, _directories, files in os.walk(root)
        for name in files
        if is_path_derived_name(name)
    )


def synthetic_root(root_length: int) -> str:
    """A throwaway project root padded to `root_length` characters."""
    base = os.path.join(os.environ.get("TEMP", os.getcwd()), "ce-release-rehearsal")
    fixed = os.path.join(base, "w")
    padding = max(0, root_length - len(fixed))
    return fixed + ("x" * padding)


def rehearse(root_length: int, keep: bool) -> int:
    root = synthetic_root(root_length)
    msys2_dir = os.path.join(PROJECT_ROOT, "build", "msys64")
    if not os.path.isdir(msys2_dir):
        print(f"MSYS2 toolchain not found at {msys2_dir}", file=sys.stderr)
        return 2

    print(f"rehearsal root : {root}")
    print(f"root length    : {len(root)} (dev {DEV_ROOT_LENGTH}, runner {RUNNER_ROOT_LENGTH})")
    print("reused state   : none - empty downloads, empty keyring, fresh recipes\n")
    if os.path.isdir(root):
        dependencies.remove_tree(root)

    started = time.time()
    builder = dependencies.SourceDependencyBuilder(
        root,
        msys2_dir,
        manifest_path=os.path.join(PROJECT_ROOT, "tools", "ffmpeg_dependencies.json"),
        logger=lambda message: print(f"[{time.time() - started:7.1f}s] {message}", flush=True),
    )

    try:
        builder.ensure(force_rebuild=True)
    except Exception as error:  # noqa: BLE001 - reporting the fault is the point
        print(f"\nREHEARSAL FAILED after {time.time() - started:.0f}s")
        print(f"  {type(error).__name__}: {error}")
        print(f"  tree kept for inspection: {root}")
        return 1

    problems: List[str] = []
    missing = [
        name
        for name in dependencies.manifest_runtime_dlls(builder.manifest)
        if not os.path.isfile(os.path.join(builder.bin_dir, name))
    ]
    if missing:
        problems.append(f"missing runtime DLLs: {', '.join(missing)}")

    declared = {
        output
        for dependency in builder.manifest["dependencies"]
        for output in dependency["package_outputs"]
    }
    built = sorted(
        name
        for _current, _directories, files in os.walk(builder.recipe_dir)
        for name in files
        if name.endswith(".pkg.tar.zst")
    )
    undeclared = [name for name in built if not any(name.startswith(f"{output}-") for output in declared)]
    if undeclared:
        problems.append(f"packages built that no package_outputs declares: {', '.join(undeclared)}")

    path_derived = find_path_derived_names(builder.recipe_dir)
    if path_derived:
        problems.append(
            f"a generated file name encodes its absolute path (the MAX_PATH defect): {path_derived[0]}"
        )

    print(f"\npackages built : {len(built)} ({len(declared)} declared)")
    print(f"runtime DLLs   : {len(dependencies.manifest_runtime_dlls(builder.manifest))}, all present")
    print(f"path-derived   : {path_derived or 'none'}")

    if not keep:
        dependencies.remove_tree(synthetic_root(root_length))
    else:
        print(f"tree kept      : {root}")

    if problems:
        print("\nREHEARSAL FAILED")
        for problem in problems:
            print(f"  {problem}")
        return 1
    print(f"\nREHEARSAL PASSED in {time.time() - started:.0f}s")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--root-length",
        type=int,
        default=DEFAULT_ROOT_LENGTH,
        help=f"synthetic project-root length (default {DEFAULT_ROOT_LENGTH}; runner is {RUNNER_ROOT_LENGTH})",
    )
    parser.add_argument("--keep", action="store_true", help="keep the rehearsal tree for inspection")
    arguments = parser.parse_args()
    return rehearse(arguments.root_length, arguments.keep)


if __name__ == "__main__":
    raise SystemExit(main())
