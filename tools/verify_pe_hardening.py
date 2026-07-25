#!/usr/bin/env python3
"""Verify mitigation metadata on shipped Windows PE files."""

from __future__ import annotations

import argparse
import dataclasses
import os
import re
import subprocess
import sys
from pathlib import Path
from typing import Iterable, Sequence

PROJECT_ROOT = Path(__file__).resolve().parents[1]
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))

from tools.ffmpeg_dependencies import is_windows_system_dll  # noqa: E402


@dataclasses.dataclass(frozen=True)
class PeHardeningResult:
    architecture: str
    guard_function_count: int
    errors: tuple[str, ...]


def parse_llvm_readobj_hardening(
    output: str, expected_architecture: str, require_cfg: bool = True
) -> PeHardeningResult:
    errors: list[str] = []
    architecture_match = re.search(r"^Arch:\s+(\S+)", output, re.MULTILINE)
    architecture = architecture_match.group(1) if architecture_match else "unknown"
    expected = "i386" if expected_architecture == "x86" else "x86_64"
    if architecture != expected:
        errors.append(f"architecture is {architecture}, expected {expected}")

    required_characteristics = [
        "IMAGE_DLL_CHARACTERISTICS_DYNAMIC_BASE",
        "IMAGE_DLL_CHARACTERISTICS_NX_COMPAT",
    ]
    if require_cfg:
        required_characteristics.append("IMAGE_DLL_CHARACTERISTICS_GUARD_CF")
    if expected_architecture == "x64":
        required_characteristics.append("IMAGE_DLL_CHARACTERISTICS_HIGH_ENTROPY_VA")
    for characteristic in required_characteristics:
        if characteristic not in output:
            errors.append(f"missing {characteristic}")

    guard_table_match = re.search(r"^\s*GuardCFFunctionTable:\s+(0x[0-9A-Fa-f]+|\d+)", output, re.MULTILINE)
    if require_cfg and (not guard_table_match or int(guard_table_match.group(1), 0) == 0):
        errors.append("GuardCFFunctionTable is empty")
    guard_count_match = re.search(r"^\s*GuardCFFunctionCount:\s+(0x[0-9A-Fa-f]+|\d+)", output, re.MULTILINE)
    guard_count = int(guard_count_match.group(1), 0) if guard_count_match else 0
    if require_cfg and guard_count <= 0:
        errors.append("GuardCFFunctionCount is zero")
    if require_cfg and "CF_FUNCTION_TABLE_PRESENT" not in output:
        errors.append("GuardFlags lacks CF_FUNCTION_TABLE_PRESENT")
    if require_cfg and "CF_INSTRUMENTED" not in output:
        errors.append("GuardFlags lacks CF_INSTRUMENTED")

    for section in re.findall(r"Section \{.*?^\s*\}", output, re.MULTILINE | re.DOTALL):
        if "IMAGE_SCN_MEM_EXECUTE" in section and "IMAGE_SCN_MEM_WRITE" in section:
            name_match = re.search(r"^\s*Name:\s+([^\s(]+)", section, re.MULTILINE)
            errors.append(f"writable/executable section {name_match.group(1) if name_match else '<unknown>'}")

    return PeHardeningResult(architecture=architecture, guard_function_count=guard_count, errors=tuple(errors))


def parse_llvm_readobj_imports(output: str) -> set[str]:
    return {
        match.group(1).lower()
        for match in re.finditer(r"^\s*Name:\s+([^\s]+\.dll)\s*$", output, re.MULTILINE | re.IGNORECASE)
    }


def is_available_system_dll(dll_name: str, architecture: str) -> bool:
    if is_windows_system_dll(dll_name):
        return True
    windows = Path(os.environ.get("WINDIR", r"C:\Windows"))
    candidates = [windows / "System32" / dll_name]
    if architecture == "x86":
        candidates.insert(0, windows / "SysWOW64" / dll_name)
    return any(candidate.is_file() for candidate in candidates)


def expected_architecture(path: Path) -> str:
    return "x86" if "_x86" in path.stem.lower() or any(part.lower() == "x86" for part in path.parts) else "x64"


def verify_binary(
    readobj: Path,
    path: Path,
    require_pdb: bool,
    available_dlls: set[str],
    allow_missing_x86_cfg: bool = False,
    allow_missing_x64_cfg: bool = False,
) -> PeHardeningResult:
    completed = subprocess.run(
        [str(readobj), "--file-headers", "--sections", "--coff-load-config", "--coff-imports", str(path)],
        check=False,
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0:
        return PeHardeningResult("unknown", 0, (f"llvm-readobj failed: {completed.stderr.strip()}",))
    architecture = expected_architecture(path)
    result = parse_llvm_readobj_hardening(
        completed.stdout,
        architecture,
        require_cfg=not (
            (allow_missing_x86_cfg and architecture == "x86")
            or (allow_missing_x64_cfg and architecture == "x64")
        ),
    )
    errors = list(result.errors)
    unresolved = sorted(
        dll_name
        for dll_name in parse_llvm_readobj_imports(completed.stdout)
        if dll_name not in available_dlls and not is_available_system_dll(dll_name, architecture)
    )
    if unresolved:
        errors.append("unresolved runtime imports: " + ", ".join(unresolved))
    if require_pdb and not path.with_suffix(".pdb").is_file():
        errors.append("matching first-party PDB is missing")
    return dataclasses.replace(result, errors=tuple(errors))


def shipped_binaries(
    root: Path, skip_x86: bool = False, executables_only: bool = False
) -> Iterable[tuple[Path, bool]]:
    for path in sorted(root.glob("*.exe")) + sorted(root.glob("*.dll")):
        if executables_only and path.suffix.lower() != ".exe":
            continue
        if skip_x86 and expected_architecture(path) == "x86":
            continue
        yield path, True
    ffmpeg = root / "ffmpeg"
    if not executables_only and ffmpeg.is_dir():
        for path in sorted(ffmpeg.glob("*.dll")):
            yield path, False


def is_ffmpeg_dll(path: Path, root: Path) -> bool:
    """Return True if the DLL is under a 'ffmpeg/' subdirectory of root."""
    try:
        rel = path.relative_to(root)
        return rel.parts[0].lower() == "ffmpeg" if len(rel.parts) > 1 else False
    except ValueError:
        return False


def verify_tree(
    readobj: Path,
    root: Path,
    allowed_runtime_dlls: Iterable[str] = (),
    skip_x86: bool = False,
    allow_missing_x86_cfg: bool = False,
    allow_missing_x64_cfg: bool = False,
    allow_missing_pdb: bool = False,
    executables_only: bool = False,
    skip_imports_for_ffmpeg: bool = False,
) -> list[str]:
    failures: list[str] = []
    allowed_runtime_names = {name.lower() for name in allowed_runtime_dlls}
    binaries = list(shipped_binaries(root, skip_x86=skip_x86, executables_only=executables_only))
    if not binaries:
        return [f"no shipped PE files found under {root}"]
    available_dlls = {path.name.lower() for path, _ in binaries if path.suffix.lower() == ".dll"}
    available_dlls.update(allowed_runtime_names)
    for path, first_party in binaries:
        # Runtime DLLs may be copied into the product root for sanitizer or
        # test execution, but they are not project-owned artifacts and do not
        # ship with a matching first-party PDB. Keep all PE hardening and
        # import-closure checks active for them.
        require_pdb = not allow_missing_pdb and first_party and path.name.lower() not in allowed_runtime_names
        result = verify_binary(
            readobj,
            path,
            require_pdb,
            available_dlls,
            allow_missing_x86_cfg=allow_missing_x86_cfg,
            allow_missing_x64_cfg=allow_missing_x64_cfg,
        )
        if result.errors:
            if skip_imports_for_ffmpeg and is_ffmpeg_dll(path, root):
                result_errors = [
                    e for e in result.errors if not e.startswith("unresolved runtime imports")
                ]
                if not result_errors:
                    architecture = expected_architecture(path)
                    cfg_deferred = result.guard_function_count == 0 and (
                        (allow_missing_x86_cfg and architecture == "x86")
                        or (allow_missing_x64_cfg and architecture == "x64")
                    )
                    cfg_status = "CFG deferred" if cfg_deferred else f"{result.guard_function_count} CFG targets"
                    print(f"PE hardening OK: {path.name} ({result.architecture}, {cfg_status})")
                    continue
                result = dataclasses.replace(result, errors=tuple(result_errors))
            failures.append(f"{path}: " + "; ".join(result.errors))
        else:
            architecture = expected_architecture(path)
            cfg_deferred = result.guard_function_count == 0 and (
                (allow_missing_x86_cfg and architecture == "x86")
                or (allow_missing_x64_cfg and architecture == "x64")
            )
            cfg_status = "CFG deferred" if cfg_deferred else f"{result.guard_function_count} CFG targets"
            print(f"PE hardening OK: {path.name} ({result.architecture}, {cfg_status})")
    return failures


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--llvm-readobj", required=True, type=Path)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--allow-runtime-dll", action="append", default=[])
    parser.add_argument("--skip-x86", action="store_true")
    parser.add_argument("--allow-missing-x86-cfg", action="store_true")
    parser.add_argument("--allow-missing-x64-cfg", action="store_true")
    parser.add_argument("--allow-missing-pdb", action="store_true")
    parser.add_argument("--executables-only", action="store_true")
    parser.add_argument("--skip-ffmpeg-imports", action="store_true")
    args = parser.parse_args(argv)
    failures = verify_tree(
        args.llvm_readobj,
        args.root,
        allowed_runtime_dlls=args.allow_runtime_dll,
        skip_x86=args.skip_x86,
        allow_missing_x86_cfg=args.allow_missing_x86_cfg,
        allow_missing_x64_cfg=args.allow_missing_x64_cfg,
        allow_missing_pdb=args.allow_missing_pdb,
        executables_only=args.executables_only,
        skip_imports_for_ffmpeg=args.skip_ffmpeg_imports,
    )
    if failures:
        for failure in failures:
            print(f"ERROR: {failure}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
