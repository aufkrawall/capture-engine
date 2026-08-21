"""Turn CaptureEngine's `Streamline 1.x probe:` log lines into a struct-layout hypothesis.

The 1.5.6 layouts of sl::DLSSConstants / sl::DLSSGConstants / sl::DLSSSettings /
sl::DLSSGSettings are unpublished, so CE records the raw payloads instead of guessing them.
This decodes those records: for every captured (call, feature) it prints each 4-byte slot
under the interpretations a Streamline constants struct is actually built from - small
enums, uint32 dimensions, and floats - and flags the slots whose value changed between
captures, which is what separates real fields from padding.

Usage:
    python analyze_sl1_probe.py <hook_debug.log> [more logs...]
"""

import re
import struct
import sys
from collections import defaultdict

LINE = re.compile(
    r"Streamline 1\.x probe: (?P<call>[\w:]+)\(feature=(?P<fname>[^/]+)/(?P<fid>\d+)\).*?"
    r"first (?P<n>\d+) bytes: (?P<hex>[0-9a-f ]+)"
)


def parse(paths):
    captures = defaultdict(list)
    for path in paths:
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            for line in handle:
                m = LINE.search(line)
                if not m:
                    continue
                raw = bytes.fromhex(m.group("hex").replace(" ", ""))
                captures[(m.group("call"), m.group("fname"), int(m.group("fid")))].append(raw)
    return captures


def describe_slot(word):
    u32 = struct.unpack("<I", word)[0]
    i32 = struct.unpack("<i", word)[0]
    f32 = struct.unpack("<f", word)[0]
    notes = []
    if u32 <= 8:
        notes.append("enum/bool/count?")
    if 16 <= u32 <= 16384:
        notes.append("dimension?")
    if u32 == 0xFFFFFFFF:
        notes.append("INVALID/UINT_MAX")
    # A plausible float: finite and in a range constants actually use.
    if f32 == f32 and abs(f32) != float("inf"):
        if 1e-6 < abs(f32) < 1e6 or f32 == 0.0:
            notes.append(f"float={f32:.6g}")
    return u32, i32, f32, ", ".join(notes)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    captures = parse(argv[1:])
    if not captures:
        print("No `Streamline 1.x probe:` records found.")
        print("Run the game UNBRIDGED (streamline_upgrade=false) so it drives its own Streamline.")
        return 1

    for (call, fname, fid), samples in sorted(captures.items()):
        print("=" * 78)
        print(f"{call}(feature={fname}/{fid})   captures={len(samples)}")
        print("=" * 78)
        width = min(len(s) for s in samples)
        varying = set()
        for off in range(0, width - 3, 4):
            values = {s[off : off + 4] for s in samples}
            if len(values) > 1:
                varying.add(off)
        for off in range(0, width - 3, 4):
            word = samples[0][off : off + 4]
            u32, i32, f32, notes = describe_slot(word)
            mark = "*" if off in varying else " "
            print(f" {mark} +{off:3d}  u32={u32:<12} i32={i32:<12} raw={word.hex()}  {notes}")
        if varying:
            print(f"\n   * = value differed across captures ({len(varying)} slot(s)) - these are live fields,")
            print("     the rest are either constant settings or padding.")
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
