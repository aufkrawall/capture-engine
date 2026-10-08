"""Report library module surface/hidden size and include-boundary debt.

Counts are descriptive, never a maintainability score. C++ declaration counts are
lexical estimates; writable-global analysis is deferred until runtime extraction.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.check_module_boundaries import SOURCE_SUFFIXES, evaluate


def measure(root: Path, boundaries: dict[str, Any]) -> dict[str, Any]:
    modules: dict[str, dict[str, int | float | None]] = {}
    runtime = root / "runtime"
    for directory in sorted(runtime.iterdir()) if runtime.exists() else []:
        if not directory.is_dir():
            continue
        public_lines = hidden_lines = declarations = 0
        for path in sorted(directory.rglob("*")):
            if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
                continue
            text = path.read_text(encoding="utf-8-sig")
            public = path.parent == directory and path.suffix in {".h", ".hpp"} and not path.stem.endswith("_internal")
            if public:
                public_lines += len(text.splitlines())
                declarations += len(re.findall(r"(?m)^\s*[\w:<>*& ,]+\s+\w+\([^;{}]*\)\s*(?:const\s*)?[;{]", text))
            else:
                hidden_lines += len(text.splitlines())
        modules[directory.name] = {
            "public_header_lines": public_lines,
            "hidden_lines": hidden_lines,
            "public_declarations_estimate": declarations,
            "depth_ratio_estimate": round(hidden_lines / declarations, 2) if declarations else None,
        }
    frontend_files = [p for p in (root / "frontend").rglob("*") if p.is_file() and p.suffix in SOURCE_SUFFIXES]
    headers = list((root / "include/cengine").glob("*.h"))
    functions: set[str] = set()
    for path in headers:
        functions.update(re.findall(r"\b(ce_[a-z_]+)\s*\(", path.read_text(encoding="utf-8-sig")))
    return {
        "public_c_functions": len(functions),
        "runtime_modules": modules,
        "frontend_lines": sum(len(p.read_text(encoding="utf-8-sig").splitlines()) for p in frontend_files),
        "writable_globals": None if modules or frontend_files else 0,
        "writable_globals_note": "Not measured once runtime/frontend exist; requires C++ scope-aware analysis.",
        "boundary_exceptions": boundaries["exceptions"],
        "internal_headers": boundaries["internal_headers"],
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args(argv)
    root = args.root.resolve()
    try:
        config = json.loads((root / "tools/module_boundaries.json").read_text(encoding="utf-8"))
        print(json.dumps(measure(root, evaluate(root, config)), indent=2, sort_keys=True))
        return 0
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"Module depth report failed: {error}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
