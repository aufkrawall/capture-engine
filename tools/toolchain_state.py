"""The MSYS2 toolchain as release input: identity, health, and proven closure builds.

The toolchain is shared by every checkout on the PC and changes whenever a default
`python build.py` runs `pacman -Syu` or someone downgrades a package by hand. Two
stable-release attempts on 2026-11-01 died inside the FFmpeg dependency closure for
exactly that reason, with nothing in the repository having changed: doxygen
1.18.0-5 could not start (0xC0000135, a missing DLL) and mingw-w64-headers r569
broke libvpl's stralign.h. The release run was the first build of the closure with
that toolchain, so it was also the first test of it.

This module makes that visible before a release is dispatched:

  * `toolchain_packages` / `fingerprint` - the exact installed package set, read from
    pacman's local database without starting pacman; recorded in every verification
    manifest, so an attested release names the toolchain that built it.
  * `probe_tools` - runs each executable the closure and the product build need, so
    a broken install fails in a second instead of minutes into makepkg.
  * closure proofs - every successful from-source build of the dependency closure or
    of FFmpeg records (kind, inputs, toolchain fingerprint). The release preflight
    asks whether the current combination was ever built successfully on this machine
    and, if not, names the packages that changed since the last proof.
"""

from __future__ import annotations

import datetime
import hashlib
import json
import os
import subprocess
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from typing import Any, Dict, List, Optional, Sequence, Tuple

PROOF_HISTORY_LIMIT = 60
CLOSURE_KINDS = ("dependency_closure", "ffmpeg")
# STATUS_DLL_NOT_FOUND as a process exit code: the doxygen failure of release run 41311122823.
STATUS_DLL_NOT_FOUND = 0xC0000135

# Executables the FFmpeg closure (makepkg recipes, FFmpeg configure) and the product
# build start. Each answers --version in ~50 ms.
TOOL_PROBES: Tuple[Tuple[str, ...], ...] = (
    ("clang64", "bin", "clang.exe"),
    ("clang64", "bin", "clang-tidy.exe"),
    ("clang64", "bin", "llvm-readobj.exe"),
    ("clang64", "bin", "cmake.exe"),
    ("clang64", "bin", "ninja.exe"),
    ("clang64", "bin", "meson.exe"),
    ("clang64", "bin", "doxygen.exe"),
    ("clang64", "bin", "nasm.exe"),
    ("clang64", "bin", "pkg-config.exe"),
    ("clang64", "bin", "python.exe"),
    ("usr", "bin", "bash.exe"),
    ("usr", "bin", "make.exe"),
    ("usr", "bin", "gpg.exe"),
    ("usr", "bin", "tar.exe"),
    ("usr", "bin", "zstd.exe"),
    ("usr", "bin", "git.exe"),
    ("usr", "bin", "perl.exe"),
    ("usr", "bin", "patch.exe"),
)


@dataclass(frozen=True)
class ToolProbe:
    tool: str
    ok: bool
    detail: str


def toolchain_packages(msys2_dir: str) -> List[str]:
    """Installed packages as `name-version-release`, from pacman's local database."""
    local = os.path.join(msys2_dir, "var", "lib", "pacman", "local")
    try:
        return sorted(entry for entry in os.listdir(local) if os.path.isdir(os.path.join(local, entry)))
    except OSError:
        return []


def fingerprint(packages: Sequence[str]) -> str:
    return hashlib.sha256("\n".join(sorted(packages)).encode("utf-8")).hexdigest()


def toolchain_summary(msys2_dir: str) -> Dict[str, Any]:
    packages = toolchain_packages(msys2_dir)
    return {"packages_sha256": fingerprint(packages), "package_count": len(packages), "packages": packages}


def pacman_busy(msys2_dir: str) -> bool:
    """True while pacman holds its database lock, i.e. the toolchain is being changed right now."""
    return os.path.exists(os.path.join(msys2_dir, "var", "lib", "pacman", "db.lck"))


def _probe(path: str) -> ToolProbe:
    name = os.path.basename(path)
    if not os.path.isfile(path):
        return ToolProbe(name, False, "missing")
    argument = "-v" if name == "nasm.exe" else "--version"
    try:
        result = subprocess.run([path, argument], capture_output=True, timeout=60, stdin=subprocess.DEVNULL)
    except (OSError, subprocess.TimeoutExpired) as error:
        return ToolProbe(name, False, f"cannot start: {error}")
    if result.returncode == 0:
        return ToolProbe(name, True, "ok")
    code = result.returncode & 0xFFFFFFFF
    if code == STATUS_DLL_NOT_FOUND:
        return ToolProbe(name, False, "exits 0xC0000135 (a DLL it needs is missing: partial or broken pacman update)")
    return ToolProbe(name, False, f"`{argument}` exits {result.returncode}")


def probe_tools(msys2_dir: str, probes: Sequence[Sequence[str]] = TOOL_PROBES) -> List[ToolProbe]:
    paths = [os.path.join(msys2_dir, *parts) for parts in probes]
    with ThreadPoolExecutor(max_workers=8) as executor:
        return list(executor.map(_probe, paths))


# -- closure proofs -------------------------------------------------------------
def _proofs_path(state_dir: str) -> str:
    return os.path.join(state_dir, "closure_proofs.json")


def _packages_path(state_dir: str, toolchain_sha256: str) -> str:
    return os.path.join(state_dir, "packages", f"{toolchain_sha256}.txt")


def load_proofs(state_dir: str) -> List[Dict[str, Any]]:
    try:
        with open(_proofs_path(state_dir), "r", encoding="utf-8") as source:
            proofs = json.load(source)
        return [proof for proof in proofs if isinstance(proof, dict)] if isinstance(proofs, list) else []
    except (OSError, ValueError):
        return []


def _write_atomically(path: str, text: str) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    temporary = f"{path}.{os.getpid()}.tmp"
    with open(temporary, "w", encoding="utf-8", newline="\n") as target:
        target.write(text)
    os.replace(temporary, path)


def record_proof(state_dir: str, kind: str, inputs_sha256: str, packages: Sequence[str], origin: str) -> None:
    """Remember that `kind` built successfully from `inputs_sha256` with this exact toolchain."""
    if kind not in CLOSURE_KINDS:
        raise ValueError(f"unknown closure kind {kind!r}")
    toolchain_sha256 = fingerprint(packages)
    packages_path = _packages_path(state_dir, toolchain_sha256)
    if not os.path.isfile(packages_path):
        _write_atomically(packages_path, "\n".join(sorted(packages)) + "\n")
    proofs = [
        proof for proof in load_proofs(state_dir)
        if (proof.get("kind"), proof.get("inputs_sha256"), proof.get("toolchain_sha256"))
        != (kind, inputs_sha256, toolchain_sha256)
    ]
    proofs.append({
        "kind": kind,
        "inputs_sha256": inputs_sha256,
        "toolchain_sha256": toolchain_sha256,
        "origin": origin,
        "recorded_at": datetime.datetime.now().isoformat(timespec="seconds"),
    })
    _write_atomically(_proofs_path(state_dir), json.dumps(proofs[-PROOF_HISTORY_LIMIT:], indent=2) + "\n")


def find_proof(state_dir: str, kind: str, inputs_sha256: str, toolchain_sha256: str) -> Optional[Dict[str, Any]]:
    for proof in reversed(load_proofs(state_dir)):
        if (proof.get("kind"), proof.get("inputs_sha256"), proof.get("toolchain_sha256")) == (
            kind, inputs_sha256, toolchain_sha256
        ):
            return proof
    return None


def latest_proof(state_dir: str, kind: str) -> Optional[Dict[str, Any]]:
    return next((proof for proof in reversed(load_proofs(state_dir)) if proof.get("kind") == kind), None)


def proof_packages(state_dir: str, toolchain_sha256: str) -> Optional[List[str]]:
    try:
        with open(_packages_path(state_dir, toolchain_sha256), "r", encoding="utf-8") as source:
            return [line.strip() for line in source if line.strip()]
    except OSError:
        return None


def _split_package(entry: str) -> Tuple[str, str]:
    """`name-version-release` -> (name, `version-release`); pacman names may contain dashes."""
    parts = entry.rsplit("-", 2)
    return (parts[0], "-".join(parts[1:])) if len(parts) == 3 else (entry, "")


def package_changes(old: Sequence[str], new: Sequence[str]) -> List[str]:
    """Human-readable differences between two package sets, changed versions first."""
    before = dict(_split_package(entry) for entry in old)
    after = dict(_split_package(entry) for entry in new)
    changed = [f"{name} {before[name]} -> {after[name]}" for name in sorted(before.keys() & after.keys())
               if before[name] != after[name]]
    added = [f"+{name} {after[name]}" for name in sorted(after.keys() - before.keys())]
    removed = [f"-{name} {before[name]}" for name in sorted(before.keys() - after.keys())]
    return changed + added + removed
