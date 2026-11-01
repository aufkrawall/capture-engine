"""Where machine-wide build state lives: beside the toolchain it was produced with.

The release runner's workspace junctions `build/msys64` to the developer tree, and
worktrees junction it to main's. Resolving the toolchain's real location therefore
gives every checkout on the PC one shared directory without any configuration:

    <real toolchain parent>/shared-cache/
        sources/      verified source archives (makepkg SRCDEST for the FFmpeg closure)
        git/          bare mirrors of pinned git sources (FFmpeg, nv-codec-headers)
        toolchain/    toolchain package lists and closure build proofs
        clang-tidy/   location-independent clang-tidy results
        locks/        holder diagnostics for the machine-wide locks

Nothing here is trusted on its own. Every consumer re-verifies what it reads (pinned
SHA-256 and PGP signatures for archives, the pinned commit id for git checkouts, the
full input fingerprint for clang-tidy results); the cache only saves the network
round trips and the repeated work. `CE_SHARED_CACHE_DIR` overrides the location.
"""

from __future__ import annotations

import os
from typing import Optional

SHARED_CACHE_ENV = "CE_SHARED_CACHE_DIR"


def shared_cache_root(project_root: str, msys2_dir: Optional[str] = None) -> str:
    override = os.environ.get(SHARED_CACHE_ENV)
    if override:
        return os.path.abspath(override)
    toolchain = msys2_dir or os.path.join(project_root, "build", "msys64")
    if os.path.isdir(toolchain):
        return os.path.join(os.path.dirname(os.path.realpath(toolchain)), "shared-cache")
    return os.path.join(project_root, "build", "shared-cache")


def shared_cache_path(project_root: str, *parts: str, msys2_dir: Optional[str] = None) -> str:
    return os.path.join(shared_cache_root(project_root, msys2_dir), *parts)
