#!/usr/bin/env python3
"""Re-run splits from grouping files after restoring facades from git.

Usage: python tools/refactor/reapply.py [--source <commit>] <grouping.json>...

--source restores the facade and fragment files from that commit's tree
(use the parent of the commit that converted the module when the fragments
were already deleted from HEAD).
"""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path
from typing import List


REPO = Path(__file__).resolve().parents[2]


def main(argv: List[str]) -> int:
    source = None
    args = list(argv[1:])
    if args and args[0] == "--source":
        source = args[1]
        args = args[2:]
    for grouping_path in args:
        grouping = json.loads(Path(grouping_path).read_text(encoding="utf-8"))
        module = grouping["module"]
        facades = [Path(grouping["facade"])] if "facade" in grouping else [REPO / f"{module}.cpp"]
        for facade in facades:
            if not facade.is_absolute():
                facade = REPO / facade
            targets = [str(facade.relative_to(REPO))]
            targets += [str((facade.parent / name).relative_to(REPO)) for name in grouping["delete"]]
            restore_cmd = ["git", "restore"]
            if source:
                restore_cmd += ["--source", source]
            restore_cmd += ["--", *targets]
            subprocess.run(restore_cmd, cwd=REPO, check=True)
            for stale in facade.parent.glob(f"{facade.stem}_*.cpp"):
                stale.unlink(missing_ok=True)
            for name in grouping["units"]:
                if name != facade.name:
                    (facade.parent / name).unlink(missing_ok=True)
            (facade.parent / grouping["header"]).unlink(missing_ok=True)
        facade = facades[0]
        subprocess.run(
            [sys.executable, str(REPO / "tools" / "refactor" / "source_splitter.py"),
             "split", str(facade), str(Path(grouping_path).resolve())],
            cwd=REPO,
            check=True,
        )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
