"""Exact-input success manifests for expensive verification stages."""

from __future__ import annotations

import hashlib
import json
import os
import re
from pathlib import Path
from typing import Callable, Iterable, List, Mapping, Optional, Sequence, Set


SCHEMA = 1
SOURCE_SUFFIXES = {
    ".c",
    ".cpp",
    ".def",
    ".frag",
    ".h",
    ".hlsl",
    ".hpp",
    ".ico",
    ".inl",
    ".json",
    ".manifest",
    ".patch",
    ".py",
    ".rc",
    ".spv",
    ".template",
    ".vert",
}
SOURCE_DIRS = ("common", "captureengine", "hook", "mediaengine", "tests", "testapp", "tools")
EXCLUDED_INPUTS = {
    "common/build_version.h",
    "tools/clang_tidy_baseline.json",
    "tools/file_size_baseline.json",
}


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _normalized(path: Path) -> str:
    return os.path.normcase(str(path.resolve()))


def discover_project_inputs(project_root: str) -> List[str]:
    root = Path(project_root).resolve()
    inputs: Set[Path] = {root / "build.py"}
    for directory_name in SOURCE_DIRS:
        directory = root / directory_name
        if not directory.is_dir():
            continue
        for path in directory.rglob("*"):
            if path.is_file() and path.suffix.lower() in SOURCE_SUFFIXES:
                relative = path.relative_to(root).as_posix()
                if relative not in EXCLUDED_INPUTS and not path.name.endswith(".link-cache.json"):
                    inputs.add(path)
    return sorted(_normalized(path) for path in inputs)


def _parse_depfile(path: Path) -> Iterable[Path]:
    try:
        content = path.read_text(encoding="utf-8", errors="surrogateescape")
    except OSError:
        return ()
    content = content.replace("\\\n", " ").replace("\r", " ").strip()
    match = re.search(r":\s", content)
    if not match:
        return ()
    placeholder = "__CE_STAGE_ESCAPED_SPACE__"
    return (
        Path(token.replace(placeholder, " "))
        for token in content[match.end() :].replace("\\ ", placeholder).split()
    )


def collect_stage_inputs(
    *,
    project_root: str,
    stage_root: str,
    extra_files: Sequence[str] = (),
    extra_roots: Sequence[str] = (),
) -> tuple[List[str], List[str]]:
    root = Path(project_root).resolve()
    discovered = discover_project_inputs(project_root)
    inputs = {Path(path) for path in discovered}
    for depfile in (Path(stage_root) / "obj").rglob("*.d"):
        for dependency in _parse_depfile(depfile):
            candidate = dependency if dependency.is_absolute() else root / dependency
            try:
                if candidate.resolve() == (root / "common" / "build_version.h").resolve():
                    continue
            except OSError:
                continue
            inputs.add(candidate)
    inputs.update(Path(path) for path in extra_files)
    for extra_root in extra_roots:
        directory = Path(extra_root)
        if directory.is_dir():
            inputs.update(path for path in directory.rglob("*") if path.is_file())
    existing = sorted(_normalized(path) for path in inputs if path.is_file())
    return discovered, existing


def collect_link_manifest_inputs(
    stage_root: str,
    dependency_resolver: Callable[[List[str], Optional[str]], Iterable[str]],
) -> List[str]:
    inputs: Set[str] = set()
    for manifest_path in Path(stage_root).rglob("*.link-cache.json"):
        try:
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            command = manifest.get("command")
            if not isinstance(command, list) or not command or not all(isinstance(arg, str) for arg in command):
                continue
            cwd = manifest.get("cwd")
            inputs.update(dependency_resolver(command, cwd if isinstance(cwd, str) else None))
        except (OSError, json.JSONDecodeError, TypeError):
            continue
    return sorted(inputs, key=os.path.normcase)


def _atomic_write(path: Path, payload: Mapping) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f"{path.name}.tmp.{os.getpid()}")
    try:
        temporary.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
        os.replace(temporary, path)
    finally:
        try:
            temporary.unlink(missing_ok=True)
        except OSError:
            pass


def write_success_manifest(
    manifest_path: str,
    *,
    discovered_inputs: Sequence[str],
    all_inputs: Sequence[str],
    outputs: Sequence[str],
) -> None:
    _atomic_write(
        Path(manifest_path),
        {
            "schema": SCHEMA,
            "discovered_inputs": list(discovered_inputs),
            "inputs": {path: _sha256(Path(path)) for path in all_inputs},
            "outputs": {path: _sha256(Path(path)) for path in outputs},
        },
    )


def success_manifest_matches(
    manifest_path: str,
    *,
    discovered_inputs: Sequence[str],
) -> bool:
    try:
        payload = json.loads(Path(manifest_path).read_text(encoding="utf-8"))
        if payload.get("schema") != SCHEMA or payload.get("discovered_inputs") != list(discovered_inputs):
            return False
        for collection in ("inputs", "outputs"):
            values = payload.get(collection)
            if not isinstance(values, dict) or not values:
                return False
            for path, expected in values.items():
                candidate = Path(path)
                if not candidate.is_file() or _sha256(candidate) != expected:
                    return False
        return True
    except (OSError, json.JSONDecodeError, TypeError, AttributeError):
        return False
