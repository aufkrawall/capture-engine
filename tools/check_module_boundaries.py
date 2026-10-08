"""Check first-party include edges against the library's module contracts.

The JSON file is the approved exception set, not a wildcard waiver. Checks are
read-only unless --prune-exceptions is requested; that mode can only remove debt.
"""

from __future__ import annotations

import argparse
import json
import posixpath
import re
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable


SOURCE_SUFFIXES = {".c", ".cpp", ".h", ".hpp", ".inl"}
INCLUDE = re.compile(r'^\s*#\s*include\s*["<]([^">]+)[">]')
COMMENTS = re.compile(
    r'(?:u8|u|U|L)?R"(?P<delimiter>[^ ()\\\t\r\n]{0,16})\(.*?\)(?P=delimiter)"'
    r'|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/',
    re.DOTALL,
)


@dataclass(frozen=True, order=True)
class Edge:
    source: str
    target: str
    line: int

    @property
    def key(self) -> tuple[str, str]:
        return self.source, self.target


def include_directives(text: str) -> Iterable[tuple[int, str]]:
    """Ignore comments/raw strings, including directives in test fixture text."""
    logical: list[str] = []
    numbers: list[int] = []
    fragments: list[str] = []
    start = 1
    for number, line in enumerate(text.splitlines(), 1):
        if not fragments:
            start = number
        if line.endswith("\\"):
            fragments.append(line[:-1])
            continue
        logical.append("".join(fragments) + line)
        numbers.append(start)
        fragments.clear()
    if fragments:
        logical.append("".join(fragments))
        numbers.append(start)

    def mask(match: re.Match[str]) -> str:
        token = match.group(0)
        if token.startswith(("//", "/*")) or match.group("delimiter") is not None:
            return re.sub(r"[^\n]", " ", token)
        return token

    cleaned = COMMENTS.sub(mask, "\n".join(logical))
    for number, line in zip(numbers, cleaned.splitlines()):
        match = INCLUDE.match(line)
        if match:
            yield number, match.group(1)


def source_files(root: Path, modules: Iterable[str]) -> list[Path]:
    return sorted(
        path for module in modules for path in (root / module).rglob("*")
        if path.is_file() and path.suffix in SOURCE_SUFFIXES
        and path.relative_to(root).as_posix() != "common/build_version.h"
    )


def include_edges(root: Path, modules: list[str]) -> tuple[list[Edge], list[str]]:
    files = source_files(root, modules)
    names = {path.relative_to(root).as_posix().casefold(): path.relative_to(root).as_posix() for path in files}
    edges: list[Edge] = []
    missing: list[str] = []
    for path in files:
        source = path.relative_to(root).as_posix()
        for line, include in include_directives(path.read_text(encoding="utf-8-sig")):
            include = include.replace("\\", "/")
            local = posixpath.normpath(posixpath.join(posixpath.dirname(source), include)).casefold()
            rooted = posixpath.normpath(include).casefold()
            target = names.get(local) or names.get(rooted)
            if target:
                edges.append(Edge(source, target, line))
            elif include.split("/")[0].casefold() in modules:
                # Generated build identity is deliberately outside the tracked source inventory.
                if rooted != "common/build_version.h":
                    missing.append(f"{source}:{line}: unresolved first-party include {include}")
    return edges, missing


def internal_header(target: str) -> bool:
    return target.endswith("_internal.h")


def allowed(edge: Edge, rules: dict[str, list[str]]) -> bool:
    prefixes = [prefix for prefix in rules if edge.source.startswith(prefix)]
    if not prefixes:
        return False
    rule = rules[max(prefixes, key=len)]
    if "*" in rule:
        return True  # Unit tests may exercise every production interface.
    if internal_header(edge.target) and posixpath.dirname(edge.source) != posixpath.dirname(edge.target):
        return False
    for permit in rule:
        if permit == "@own":
            subsystem = "/".join(edge.source.split("/")[:2]) + "/"
            if edge.target.startswith(subsystem):
                return True
        elif permit == "@runtime_public":
            parts = edge.target.split("/")
            if len(parts) == 3 and parts[0] == "runtime" and edge.target.endswith((".h", ".hpp")):
                return True
        elif (edge.target.startswith(permit) if permit.endswith("/") else edge.target == permit):
            return True
    return False


def exception_keys(config: dict[str, Any]) -> set[tuple[str, str]]:
    result: set[tuple[str, str]] = set()
    for item in config["exceptions"]:
        key = item["source"], item["target"]
        if key in result or not item["reason"].strip() or not item["milestone"].strip():
            raise ValueError(f"Duplicate or undocumented boundary exception: {key}")
        for path in key:
            if path != posixpath.normpath(path) or path.startswith(("/", "..")) or "\\" in path:
                raise ValueError(f"Noncanonical exception path: {path}")
        result.add(key)
    return result


def evaluate(root: Path, config: dict[str, Any], approved: dict[str, Any] | None = None) -> dict[str, Any]:
    if config.get("version") != 1 or not config.get("modules") or not config.get("rules"):
        raise ValueError("Unsupported or empty module boundary policy")
    exceptions = exception_keys(config)
    additions = exceptions - exception_keys(approved) if approved is not None else set()
    edges, missing = include_edges(root, config["modules"])
    debt = {edge.key for edge in edges if not allowed(edge, config["rules"])}
    violations = [edge for edge in edges if edge.key in debt - exceptions]
    reach: dict[str, dict[str, int]] = {}
    for target in sorted({edge.target for edge in edges if internal_header(edge.target)}):
        includers = {edge.source for edge in edges if edge.target == target}
        outside = {source for source in includers if posixpath.dirname(source) != posixpath.dirname(target)}
        reach[target] = {"includers": len(includers), "outside_subsystem": len(outside)}
    return {
        "success": not (violations or missing or additions),
        "source_files": len(source_files(root, config["modules"])),
        "include_edges": len(edges),
        "exceptions": len(exceptions),
        "used_exceptions": len(exceptions & debt),
        "stale_exceptions": sorted(exceptions - debt),
        "added_exceptions": sorted(additions),
        "violations": [{"source": e.source, "target": e.target, "line": e.line} for e in violations],
        "unresolved": missing,
        "internal_headers": reach,
    }


def committed_policy(root: Path, path: Path) -> dict[str, Any] | None:
    """Compare against the checked-out approval set; never add an exception via CLI."""
    repository = subprocess.run(
        ["git", "rev-parse", "--show-toplevel"], cwd=root,
        capture_output=True, text=True, encoding="utf-8", timeout=10,
    )
    if repository.returncode != 0:
        if (root / ".git").exists():
            raise ValueError("Cannot identify module boundary repository")
        return None
    if Path(repository.stdout.strip()).resolve() != root.resolve():
        # Build self-tests place standalone fixtures under build/tmp. They must
        # not accidentally inherit the enclosing project's approval set.
        return None
    relative = path.resolve().relative_to(root.resolve()).as_posix()
    result = subprocess.run(
        ["git", "show", f"HEAD:{relative}"], cwd=root, capture_output=True, text=True, encoding="utf-8", timeout=10,
    )
    if result.returncode == 0:
        return json.loads(result.stdout)
    # Initial adoption/standalone fixtures have no approved file. An existing
    # tracked policy failing to read must not silently bypass its ratchet.
    tracked = subprocess.run(
        ["git", "ls-tree", "--name-only", "HEAD", "--", relative], cwd=root,
        capture_output=True, text=True, encoding="utf-8", timeout=10,
    )
    if tracked.returncode == 0 and tracked.stdout.strip():
        raise ValueError("Cannot read committed module boundary approval set")
    if tracked.returncode != 0:
        raise ValueError("Cannot verify committed module boundary approval set")
    return None


def summary_lines(report: dict[str, Any]) -> list[str]:
    lines = [
        f"Module boundaries: {'OK' if report['success'] else 'FAILED'} "
        f"({report['include_edges']} edges, {report['used_exceptions']} accepted exceptions)",
    ]
    headers = sorted(report["internal_headers"].items(), key=lambda item: (-item[1]["includers"], item[0]))
    for path, counts in headers[:3]:
        lines.append(f"  {path}: {counts['includers']} includers, {counts['outside_subsystem']} outside subsystem")
    return lines


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--config", type=Path)
    parser.add_argument("--json", action="store_true", help="Emit the full report as JSON")
    parser.add_argument("--prune-exceptions", action="store_true", help="Remove unused approved exceptions only")
    args = parser.parse_args(argv)
    root = args.root.resolve()
    path = args.config or root / "tools/module_boundaries.json"
    try:
        config = json.loads(path.read_text(encoding="utf-8"))
        report = evaluate(root, config, committed_policy(root, path))
        if args.prune_exceptions and report["success"]:
            stale = {tuple(item) for item in report["stale_exceptions"]}
            if stale:
                config["exceptions"] = [
                    item for item in config["exceptions"] if (item["source"], item["target"]) not in stale
                ]
                path.write_text(json.dumps(config, indent=2) + "\n", encoding="utf-8", newline="\n")
                report = evaluate(root, config)
        if args.json:
            print(json.dumps(report, sort_keys=True))
        else:
            print("\n".join(summary_lines(report)))
            for item in report["violations"][:20]:
                print(f"  {item['source']}:{item['line']}: forbidden include {item['target']}")
            for item in report["added_exceptions"][:20]:
                print(f"  unapproved exception: {item[0]} -> {item[1]}")
            for item in report["unresolved"][:20]:
                print(f"  {item}")
        return 0 if report["success"] else 1
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        print(f"Module boundary check failed: {error}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
