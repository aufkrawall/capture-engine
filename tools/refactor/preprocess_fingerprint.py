"""Fingerprint every translation unit's preprocessed token stream.

    python tools/refactor/preprocess_fingerprint.py snapshot <out.json>
    python tools/refactor/preprocess_fingerprint.py compare <before.json> <after.json> [mapping.json]

A pure file move/include re-spelling must leave each TU's `clang -E -P` output byte-identical;
any difference means a different header was picked or text changed. `mapping.json`
(old repo path -> new repo path) pairs moved TUs.
"""

from __future__ import annotations

import concurrent.futures
import hashlib
import json
import os
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def rel(path: str) -> str:
    return os.path.normpath(os.path.relpath(path, ROOT)).replace("\\", "/")


def preprocess(entry: dict) -> tuple[str, str]:
    args = list(entry.get("arguments") or entry["command"].split())
    out: list[str] = []
    skip = False
    for a in args:
        if skip:
            skip = False
            continue
        if a in ("-o", "-MF", "-MT", "-MQ"):
            skip = True
            continue
        if a in ("-c", "-MD", "-MMD") or (a.startswith("-o") and len(a) > 2):
            continue
        out.append(a)
    out += ["-E", "-P", "-o", "-"]
    proc = subprocess.run(out, cwd=entry.get("directory", ROOT), capture_output=True)
    src = rel(os.path.join(entry.get("directory", ROOT), entry["file"]))
    if proc.returncode != 0:
        return src, "ERROR:" + proc.stderr.decode(errors="replace")[:400]
    return src, hashlib.sha256(proc.stdout).hexdigest()


def snapshot(out_path: str) -> None:
    db = json.load(open(os.path.join(ROOT, "compile_commands.json"), encoding="utf-8"))
    result: dict[str, str] = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 8) as pool:
        for src, digest in pool.map(preprocess, db):
            key = src
            n = 1
            while key in result:  # same file compiled with several flag sets
                n += 1
                key = f"{src}#{n}"
            result[key] = digest
    errors = {k: v for k, v in result.items() if v.startswith("ERROR")}
    json.dump(result, open(out_path, "w", encoding="utf-8"), indent=1, sort_keys=True)
    print(f"{len(result)} TUs fingerprinted, {len(errors)} errors")
    for k, v in list(errors.items())[:10]:
        print(k, v)


def compare(before_path: str, after_path: str, mapping_path: str | None) -> None:
    before = json.load(open(before_path, encoding="utf-8"))
    after = json.load(open(after_path, encoding="utf-8"))
    mapping = json.load(open(mapping_path, encoding="utf-8")) if mapping_path else {}
    same = differ = missing = 0
    for key, digest in sorted(before.items()):
        src, _, n = key.partition("#")
        new_key = mapping.get(src, src) + (f"#{n}" if n else "")
        if new_key not in after:
            missing += 1
            print(f"MISSING {key} -> {new_key}")
        elif after[new_key] != digest:
            differ += 1
            print(f"DIFFERS {key} -> {new_key}")
        else:
            same += 1
    print(f"identical={same} differ={differ} missing={missing} new={len(after) - same - differ}")


if __name__ == "__main__":
    if sys.argv[1] == "snapshot":
        snapshot(sys.argv[2])
    elif sys.argv[1] == "compare":
        compare(sys.argv[2], sys.argv[3], sys.argv[4] if len(sys.argv) > 4 else None)
    else:
        raise SystemExit(__doc__)
