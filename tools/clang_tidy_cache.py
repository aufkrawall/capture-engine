"""Content-addressed per-translation-unit clang-tidy execution.

The cache is deliberately stricter than a timestamp cache.  A result is reused
only when the clang-tidy binary, project configuration, compile command, source,
and every dependency recorded by the compiler's depfile have identical bytes.
Unreadable or incomplete inputs fail closed to a fresh, non-cacheable run.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import subprocess
from collections import Counter
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Iterable, List, Mapping, Optional, Sequence, Tuple


CACHE_SCHEMA = 1
RUN_FLAGS = ("-extra-arg=-w", "-quiet")


@dataclass(frozen=True)
class ClangTidyCacheResult:
    stdout: str
    stderr: str
    returncode: int
    hits: int
    misses: int
    uncacheable: int
    entries: int


@dataclass(frozen=True)
class SnapshotClangTidyRun:
    result: ClangTidyCacheResult
    compile_database: Sequence[Mapping[str, Any]]
    command: Sequence[str]


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _atomic_write_json(path: Path, payload: Any) -> None:
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


def _entry_arguments(entry: Mapping[str, Any]) -> Optional[List[str]]:
    arguments = entry.get("arguments")
    if isinstance(arguments, list) and all(isinstance(argument, str) for argument in arguments):
        return list(arguments)
    return None


def _dependency_file(arguments: Sequence[str], directory: Path) -> Optional[Path]:
    for index, argument in enumerate(arguments):
        candidate: Optional[str] = None
        if argument == "-MF" and index + 1 < len(arguments):
            candidate = arguments[index + 1]
        elif argument.startswith("-MF") and len(argument) > 3:
            candidate = argument[3:]
        if candidate:
            path = Path(candidate)
            return path if path.is_absolute() else directory / path
    return None


def _parse_depfile(path: Path, directory: Path) -> Optional[List[Path]]:
    try:
        content = path.read_text(encoding="utf-8", errors="surrogateescape")
    except OSError:
        return None
    content = content.replace("\\\n", " ").replace("\r", " ").strip()
    split_match = re.search(r":\s", content)
    if not split_match:
        return None
    dependency_text = content[split_match.end() :].strip()
    placeholder = "__CE_CLANG_TIDY_ESCAPED_SPACE__"
    tokens = dependency_text.replace("\\ ", placeholder).split()
    dependencies: List[Path] = []
    for token in tokens:
        candidate = Path(token.replace(placeholder, " "))
        dependencies.append(candidate if candidate.is_absolute() else directory / candidate)
    return dependencies


def _config_files(source: Path, project_root: Path) -> List[Path]:
    """Return clang-tidy configuration files that can affect this source."""
    configs: List[Path] = []
    current = source.parent
    root = project_root.resolve()
    while True:
        candidate = current / ".clang-tidy"
        if candidate.is_file():
            configs.append(candidate)
            # clang-tidy stops at the first config unless InheritParentConfig is
            # enabled. Hashing every ancestor remains conservative and cheap.
        if current == root or current.parent == current:
            break
        current = current.parent
    return configs


def _entry_fingerprint(
    entry: Mapping[str, Any],
    *,
    project_root: Path,
    tool_sha256: str,
    content_hashes: Dict[str, str],
) -> Optional[str]:
    arguments = _entry_arguments(entry)
    source_value = entry.get("file")
    directory_value = entry.get("directory")
    if not arguments or not isinstance(source_value, str) or not isinstance(directory_value, str):
        return None
    directory = Path(directory_value).resolve()
    source = Path(source_value)
    source = source.resolve() if source.is_absolute() else (directory / source).resolve()
    depfile = _dependency_file(arguments, directory)
    dependencies = _parse_depfile(depfile, directory) if depfile else None
    if dependencies is None:
        return None
    dependencies.append(source)
    dependencies.extend(_config_files(source, project_root))

    inputs = []
    for dependency in sorted({path.resolve() for path in dependencies}, key=lambda path: os.path.normcase(str(path))):
        if not dependency.is_file():
            return None
        cache_key = os.path.normcase(str(dependency))
        try:
            digest = content_hashes.get(cache_key)
            if digest is None:
                digest = _sha256_file(dependency)
                content_hashes[cache_key] = digest
        except OSError:
            return None
        inputs.append((cache_key, digest))

    payload = {
        "schema": CACHE_SCHEMA,
        "tool_sha256": tool_sha256,
        "run_flags": RUN_FLAGS,
        "directory": os.path.normcase(str(directory)),
        "file": os.path.normcase(str(source)),
        "arguments": arguments,
        "inputs": inputs,
    }
    return hashlib.sha256(
        json.dumps(payload, sort_keys=True, separators=(",", ":")).encode("utf-8", errors="surrogatepass")
    ).hexdigest()


def _cache_path(cache_dir: Path, source: Path) -> Path:
    source_key = hashlib.sha256(os.path.normcase(str(source.resolve())).encode("utf-8")).hexdigest()
    return cache_dir / f"{source_key}.json"


def _read_cache(path: Path, fingerprint: str) -> Optional[Dict[str, Any]]:
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
        if (
            payload.get("schema") == CACHE_SCHEMA
            and payload.get("fingerprint") == fingerprint
            and payload.get("returncode") == 0
            and isinstance(payload.get("stdout"), str)
            and isinstance(payload.get("stderr"), str)
        ):
            return payload
    except (OSError, json.JSONDecodeError, TypeError, AttributeError):
        pass
    return None


def run_cached_clang_tidy(
    *,
    clang_tidy: str,
    compile_database: Sequence[Mapping[str, Any]],
    compile_database_dir: str,
    project_root: str,
    cache_dir: str,
    jobs: int,
    env: Optional[Mapping[str, str]] = None,
) -> ClangTidyCacheResult:
    """Run clang-tidy only for entries whose complete input fingerprint changed."""
    root = Path(project_root).resolve()
    database_dir = Path(compile_database_dir).resolve()
    cache_root = Path(cache_dir)
    tool_sha256 = _sha256_file(Path(clang_tidy))
    content_hashes: Dict[str, str] = {}
    prepared = []
    hits = 0
    uncacheable = 0
    outputs: Dict[str, Dict[str, Any]] = {}

    for entry in sorted(compile_database, key=lambda item: os.path.normcase(str(item.get("file", "")))):
        source_value = entry.get("file")
        directory_value = entry.get("directory")
        if not isinstance(source_value, str) or not isinstance(directory_value, str):
            uncacheable += 1
            continue
        directory = Path(directory_value)
        source = Path(source_value)
        source = source.resolve() if source.is_absolute() else (directory / source).resolve()
        fingerprint = _entry_fingerprint(
            entry,
            project_root=root,
            tool_sha256=tool_sha256,
            content_hashes=content_hashes,
        )
        cache_path = _cache_path(cache_root, source)
        cached = _read_cache(cache_path, fingerprint) if fingerprint else None
        key = os.path.normcase(str(source))
        if cached:
            outputs[key] = cached
            hits += 1
            continue
        if fingerprint is None:
            uncacheable += 1
        prepared.append((key, source, cache_path, fingerprint))

    def run_one(item: tuple[str, Path, Path, Optional[str]]) -> tuple[str, Dict[str, Any]]:
        key, source, cache_path, fingerprint = item
        command = [
            clang_tidy,
            *RUN_FLAGS,
            f"-p={database_dir}",
            str(source),
        ]
        result = subprocess.run(
            command,
            cwd=root,
            env=dict(env) if env is not None else None,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        payload = {
            "schema": CACHE_SCHEMA,
            "fingerprint": fingerprint,
            "source": str(source),
            "command": command,
            "returncode": result.returncode,
            "stdout": result.stdout or "",
            "stderr": result.stderr or "",
        }
        if fingerprint and result.returncode == 0:
            _atomic_write_json(cache_path, payload)
        return key, payload

    with ThreadPoolExecutor(max_workers=max(1, min(jobs, len(prepared) or 1))) as executor:
        futures = [executor.submit(run_one, item) for item in prepared]
        for future in as_completed(futures):
            key, payload = future.result()
            outputs[key] = payload

    ordered = [outputs[key] for key in sorted(outputs)]
    return_codes = [int(payload["returncode"]) for payload in ordered]
    return ClangTidyCacheResult(
        stdout="\n".join(str(payload["stdout"]).rstrip() for payload in ordered if payload["stdout"]).rstrip(),
        stderr="\n".join(str(payload["stderr"]).rstrip() for payload in ordered if payload["stderr"]).rstrip(),
        returncode=next((code for code in return_codes if code != 0), 0),
        hits=hits,
        misses=len(prepared),
        uncacheable=uncacheable,
        entries=len(compile_database),
    )


def write_compile_database_snapshot(
    *,
    compile_database: Sequence[Mapping[str, Any]],
    snapshot_dir: str,
    build_script_sha256: str,
) -> None:
    destination = Path(snapshot_dir)
    _atomic_write_json(destination / "compile_commands.json", list(compile_database))
    _atomic_write_json(
        destination / "metadata.json",
        {
            "schema": CACHE_SCHEMA,
            "build_script_sha256": build_script_sha256,
            "entries": len(compile_database),
        },
    )


def load_compile_database_snapshot(
    *, snapshot_dir: str, build_script_sha256: str
) -> Optional[List[Mapping[str, Any]]]:
    directory = Path(snapshot_dir)
    try:
        metadata = json.loads((directory / "metadata.json").read_text(encoding="utf-8"))
        entries = json.loads((directory / "compile_commands.json").read_text(encoding="utf-8"))
        if (
            metadata.get("schema") != CACHE_SCHEMA
            or metadata.get("build_script_sha256") != build_script_sha256
            or not isinstance(entries, list)
            or metadata.get("entries") != len(entries)
        ):
            return None
        return entries
    except (OSError, json.JSONDecodeError, TypeError, AttributeError):
        return None


def run_snapshot_preflight(
    *,
    clang_tidy: str,
    snapshot_dir: str,
    build_script_sha256: str,
    project_root: str,
    cache_dir: str,
    jobs: int,
    env: Optional[Mapping[str, str]] = None,
) -> Optional[SnapshotClangTidyRun]:
    compile_database = load_compile_database_snapshot(
        snapshot_dir=snapshot_dir,
        build_script_sha256=build_script_sha256,
    )
    if not compile_database or not Path(clang_tidy).is_file():
        return None
    result = run_cached_clang_tidy(
        clang_tidy=clang_tidy,
        compile_database=compile_database,
        compile_database_dir=snapshot_dir,
        project_root=project_root,
        cache_dir=cache_dir,
        jobs=jobs,
        env=env,
    )
    command = (
        clang_tidy,
        *RUN_FLAGS,
        f"-p={snapshot_dir}",
        "<content-addressed preflight translation units>",
    )
    return SnapshotClangTidyRun(result, compile_database, command)


def warning_lines(output: str) -> Iterable[str]:
    return (line for line in output.splitlines() if "warning:" in line)


def analyze_warning_output(output: str, project_root: str) -> Tuple[List[str], Counter, Counter]:
    warnings = list(warning_lines(output))
    checks = Counter()
    subsystems = Counter()
    for line in warnings:
        check_match = re.search(r"\[([^\]]+)\]\s*$", line)
        if check_match:
            checks[check_match.group(1)] += 1
        path_match = re.match(r"^(.+?):\d+:\d+:\s+warning:", line)
        if path_match:
            try:
                relative = os.path.relpath(path_match.group(1), project_root)
            except ValueError:
                relative = path_match.group(1)
            subsystems[relative.replace("\\", "/").split("/", 1)[0]] += 1
    return warnings, checks, subsystems
