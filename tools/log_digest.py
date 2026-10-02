"""Compact digest of a CaptureEngine session log directory - the first thing to read in an investigation.

    python tools/log_digest.py <session-dir> [--pid PID] [--since HH:MM:SS] [--until HH:MM:SS]
                               [--timeline-lines N] [--problem-groups N]
    python tools/log_digest.py --self-test

A session directory holds megabytes of logs (hundreds of thousands of tokens). The digest prints a few
hundred lines instead:

  files      every file with size and lines; vendor logs (sl.log, nvngx_*.log) are marked, read them only
             when the question is about Streamline/NGX itself
  processes  pid -> executable/arch, first/last time and line count per log
  problems   warnings, errors, failures, device removal, dumps, lost lines - grouped by message template
             with count, first/last time and one example
  timeline   state transitions (frame generation, recording, swapchains, overlay routes, injection, config)
             with consecutive repeats collapsed
  volume     the templates that dominate each log (exclude them when grepping)
  gaps       lost lines per file (sequence gaps) and the longest silences in hook_debug.log

Then grep the named file for the template it shows. Reads both the current hook prefix
(`HH:MM:SS.mmm T<tid> #<seq> p<pid>`, 0.1.6946+) and the older `[time] [T:x] [S:n] [exe]` one.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
import tempfile
from collections import Counter, OrderedDict, defaultdict
from dataclasses import dataclass, field

# Streamline/NGX write their own logs into the session directory; nvngx_debug.log is CE's.
VENDOR_LOG = re.compile(r"^(sl\.log|nvngx\.log|nvngx_(?!debug\.log)[a-z0-9_]+\.log)$", re.IGNORECASE)
TEXT_SUFFIXES = (".log", ".txt", ".csv")

# Line formats, newest first. Groups: time, tid, seq, process (pid digits or exe name), message.
HOOK_NEW = re.compile(r"^(\d\d:\d\d:\d\d\.\d{3}) T([0-9A-Fa-f]+) #(\d+) p(\d+) (.*)$")
HOOK_OLD = re.compile(r"^\[(\d\d:\d\d:\d\d\.\d{3})\] \[T:([0-9A-Fa-f]+)\] \[S:(\d+)\] \[([^\]]+)\] (.*)$")
LAYER_NEW = re.compile(r"^(\d\d:\d\d:\d\d\.\d{3}) T([0-9A-Fa-f]+) p(\d+) (.*)$")
LAYER_OLD = re.compile(r"^\[(\d\d:\d\d:\d\d\.\d{3})\] (?:\[T:([0-9A-Fa-f]+)\] )?\[([^\]]+)\] (.*)$")
SERVICE = re.compile(r"^\[\d{4}-\d\d-\d\d (\d\d:\d\d:\d\d\.\d{3})\] \[([A-Z]+)\] (.*)$")
TIME_ONLY = re.compile(r"^\[?(\d\d:\d\d:\d\d\.\d{3})\]? (.*)$")

PROCESS_LEGEND = re.compile(r"Process '([^']+)' pid=(\d+) (x64|x86)")
LAYER_LEGEND = re.compile(r"=== Layer DLL Loaded: (\S+) pid=(\d+) (x64|x86)")

PROBLEM = re.compile(
    r"\[(?:WARN|ERROR)\]|\b(error|errors|fail(?:ed|ure|ures|s)?|exception|crash(?:ed)?|fatal|"
    r"device[_ ]?(?:removed|hung|reset)|hung|timed out|time-?out(?!\s*[=:(])|refus(?:ed|ing)|denied|dropped|"
    r"overflow(?:ed)?|lost|"
    r"stall(?:ed)?|freeze (?:detected|dump)|violation|assert(?:ion)?|invariant|unexpected)\b",
    re.IGNORECASE,
)
# Counters and identifiers that report nothing wrong: "dropped=0", "errors: 0", "deviceRemoved=0x00000000",
# "failed=false", the watchdog's and dump hooks' own names.
BENIGN_COUNTER = re.compile(
    r"\b(?:errors?|fail\w*|dropped|overflow\w*|lost|stall\w*|refus\w*|timeouts?|crash\w*|dumps?|aborts?|"
    r"device\w*removed\w*)\s*[=:]\s*(?:0x0+\b|0(?![.\d])|false|none)|"
    r"FreezeWatchdog|pre-termination|SetUnhandledExceptionFilter|exception (?:filter|handler)",
    re.IGNORECASE,
)

TIMELINE = re.compile(
    r"FG (?:EVENT|PLAN DIFF|TRANSITION)|frame.?generation (?:on|off|enabled|disabled|activ)|"
    r"DLSS-?G (?:enabled|disabled|turned|activ)|"
    r"Recording (?:started|stopped|failed)|recording (?:start|stop)|[Ff]inaliz(?:ed|ation)|"
    r"[Ss]wap ?[Cc]hain (?:created|destroyed|released|resized|replaced)|"
    r"\b(?:activated|deactivated|resumed|suspended|reactivated)\b|OVERLAY (?:VISIBILITY|SWAPCHAIN HANDOFF)|"
    r"Injected x(?:64|86)|[Ii]njection (?:successful|failed)|Tracked injected process exited|"
    r"whitelisted hook target|[Cc]onfig (?:reload|changed)|ReloadConfig|[Ff]ocus (?:lost|regained)|"
    r"DEVICE.?REMOVED|Screenshot (?:saved|failed)|Layer DLL Loaded|Session target",
)

NORMALIZE = [
    (re.compile(r"\(\+\d+ unchanged\)"), ""),
    (re.compile(r"'[^']*'|\"[^\"]*\""), "'_'"),
    (re.compile(r"[A-Za-z]:\\[^\s,)]*|\\\\[^\s,)]*"), "<path>"),
    (re.compile(r"\b0x[0-9A-Fa-f]+\b|\b[0-9A-Fa-f]{8,16}\b"), "#"),
    (re.compile(r"-?\d+(?:\.\d+)?"), "#"),
]


def template(message: str) -> str:
    for pattern, replacement in NORMALIZE:
        message = pattern.sub(replacement, message)
    return message.strip()[:160]


@dataclass
class Line:
    file: str
    time: str
    process: str  # pid digits, exe name, or "" when the format has no process column
    seq: int | None
    level: str
    message: str


@dataclass
class FileStats:
    name: str
    size: int
    lines: int = 0
    vendor: bool = False
    parsed: list = field(default_factory=list)


def parse_line(file_name: str, raw: str) -> Line | None:
    for regex, kind in ((HOOK_NEW, "hook"), (HOOK_OLD, "hook"), (SERVICE, "service"), (LAYER_NEW, "layer"),
                        (LAYER_OLD, "layer_old"), (TIME_ONLY, "time")):
        match = regex.match(raw)
        if not match:
            continue
        if kind == "hook":
            time, _tid, seq, process, message = match.groups()
            return Line(file_name, time, process, int(seq), "", message)
        if kind == "service":
            time, level, message = match.groups()
            return Line(file_name, time, "", None, level, message)
        if kind == "layer":
            time, _tid, pid, message = match.groups()
            return Line(file_name, time, pid, None, "", message)
        if kind == "layer_old":
            time, _tid, process, message = match.groups()
            return Line(file_name, time, process, None, "", message)
        time, message = match.groups()
        return Line(file_name, time, "", None, "", message)
    return None


def load_session(directory: str, pid: str | None, since: str | None, until: str | None) -> list[FileStats]:
    files: list[FileStats] = []
    for name in sorted(os.listdir(directory)):
        path = os.path.join(directory, name)
        if not os.path.isfile(path):
            continue
        stats = FileStats(name, os.path.getsize(path), vendor=bool(VENDOR_LOG.match(name)))
        files.append(stats)
        if not name.lower().endswith(TEXT_SUFFIXES):
            continue
        with open(path, encoding="utf-8", errors="replace") as handle:
            for raw in handle:
                stats.lines += 1
                if stats.vendor or name.lower().endswith(".csv"):
                    continue
                line = parse_line(name, raw.rstrip("\r\n"))
                if line is None:
                    continue
                if pid and line.process and line.process != pid:
                    continue
                if (since and line.time < since) or (until and line.time > until):
                    continue
                stats.parsed.append(line)
    return files


def is_problem(line: Line) -> bool:
    if line.level in ("WARN", "ERROR"):
        return True
    match = PROBLEM.search(line.message)
    if not match:
        return False
    stripped = BENIGN_COUNTER.sub("", line.message)
    return PROBLEM.search(stripped) is not None


def human(size: int) -> str:
    for unit in ("B", "KB", "MB", "GB"):
        if size < 1024 or unit == "GB":
            return f"{size:.0f} {unit}" if unit == "B" else f"{size:.1f} {unit}"
        size /= 1024
    return str(size)


def section_files(files: list[FileStats]) -> list[str]:
    out = ["## files"]
    for stats in files:
        note = " (vendor log - skip unless the question is about it)" if stats.vendor else ""
        if stats.name.lower().endswith(".dmp"):
            note = " (crash/stall dump - analyze with cdb, see llm-wiki/debug-tools.md)"
        lines = f"{stats.lines} lines" if stats.lines else ""
        out.append(f"- {stats.name}: {human(stats.size)} {lines}{note}".rstrip())
    return out


def section_processes(files: list[FileStats]) -> list[str]:
    legend: dict[str, str] = {}
    spans: dict[tuple[str, str], list] = {}
    for stats in files:
        for line in stats.parsed:
            for regex in (PROCESS_LEGEND, LAYER_LEGEND):
                match = regex.search(line.message)
                if match:
                    legend[match.group(2)] = f"{match.group(1)} {match.group(3)}"
            if line.process:
                span = spans.setdefault((line.process, stats.name), [line.time, line.time, 0])
                span[1] = line.time
                span[2] += 1
    if not spans:
        return []
    out = ["## processes"]
    for (process, name), (first, last, count) in sorted(spans.items(), key=lambda item: item[1][0]):
        label = f"p{process} = {legend[process]}" if process in legend else process
        out.append(f"- {label} in {name}: {first} - {last}, {count} lines")
    return out


def section_problems(files: list[FileStats], limit: int) -> list[str]:
    groups: OrderedDict[tuple[str, str], list] = OrderedDict()
    for stats in files:
        for line in stats.parsed:
            if not is_problem(line):
                continue
            key = (stats.name, template(line.message))
            group = groups.setdefault(key, [0, line.time, line.time, line])
            group[0] += 1
            group[2] = line.time
    if not groups:
        return ["## problems", "- none matched (warnings, errors, failures, device removal, dumps, lost lines)"]
    out = [f"## problems ({len(groups)} templates; first occurrence order)"]
    for (name, _tmpl), (count, first, last, example) in list(groups.items())[:limit]:
        where = f"p{example.process} " if example.process.isdigit() else ""
        times = first if count == 1 else f"{first}-{last} x{count}"
        out.append(f"- [{name} {where}{times}] {example.message[:220]}")
    if len(groups) > limit:
        out.append(f"- ... {len(groups) - limit} more templates (raise --problem-groups or narrow --since/--until)")
    return out


def section_timeline(files: list[FileStats], limit: int) -> list[str]:
    events = []
    for stats in files:
        for index, line in enumerate(stats.parsed):
            if TIMELINE.search(line.message):
                events.append((line.time, stats.name, index, line))
    events.sort(key=lambda event: (event[0], event[1], event[2]))
    collapsed: list[list] = []
    for time, name, _index, line in events:
        key = (name, line.process, template(line.message))
        if collapsed and collapsed[-1][0] == key:
            collapsed[-1][2] += 1
            collapsed[-1][3] = time
            continue
        collapsed.append([key, line, 1, time])
    if not collapsed:
        return []
    out = [f"## timeline ({len(collapsed)} entries, consecutive repeats collapsed)"]
    shown = collapsed if len(collapsed) <= limit else collapsed[: limit // 2] + [None] + collapsed[-limit // 2:]
    for entry in shown:
        if entry is None:
            out.append(f"- ... {len(collapsed) - limit} entries omitted (narrow with --since/--until or --pid)")
            continue
        (name, process, _tmpl), line, count, last = entry
        where = f"p{process} " if process.isdigit() else ""
        repeat = f" (x{count} until {last})" if count > 1 else ""
        out.append(f"- {line.time} [{name} {where}]{repeat} {line.message[:200]}".replace(" ]", "]"))
    return out


def section_volume(files: list[FileStats], top: int = 6) -> list[str]:
    out = ["## volume (templates that dominate each log)"]
    for stats in files:
        if len(stats.parsed) < 200:
            continue
        counter: Counter = Counter()
        sizes: Counter = Counter()
        for line in stats.parsed:
            key = template(line.message)
            counter[key] += 1
            sizes[key] += len(line.message)
        total = sum(sizes.values()) or 1
        out.append(f"- {stats.name}:")
        for key, size in sizes.most_common(top):
            out.append(f"  - {100 * size // total}% x{counter[key]}: {key[:120]}")
    return out if len(out) > 1 else []


def section_gaps(files: list[FileStats]) -> list[str]:
    out = []
    for stats in files:
        by_process: dict[str, list[int]] = defaultdict(list)
        for line in stats.parsed:
            if line.seq is not None and line.process.isdigit():
                by_process[line.process].append(line.seq)
        for process, seqs in by_process.items():
            seqs.sort()
            missing = (seqs[-1] - seqs[0] + 1) - len(set(seqs))
            if missing:
                out.append(f"- {stats.name} p{process}: {missing} line(s) lost between #{seqs[0]} and #{seqs[-1]}")
    hook = next((s for s in files if s.name == "hook_debug.log"), None)
    if hook and len(hook.parsed) > 1:
        def seconds(time: str) -> float:
            h, m, rest = time.split(":")
            return int(h) * 3600 + int(m) * 60 + float(rest)

        silences = []
        for previous, current in zip(hook.parsed, hook.parsed[1:]):
            delta = seconds(current.time) - seconds(previous.time)
            if delta >= 2.0:
                silences.append((delta, previous, current))
        for delta, previous, current in sorted(silences, key=lambda item: -item[0])[:5]:
            out.append(f"- hook_debug.log silent {delta:.1f}s: {previous.time} '{previous.message[:80]}' -> "
                       f"{current.time} '{current.message[:80]}'")
    return (["## gaps"] + out) if out else []


def digest(directory: str, pid: str | None = None, since: str | None = None, until: str | None = None,
           timeline_lines: int = 120, problem_groups: int = 60) -> str:
    files = load_session(directory, pid, since, until)
    parts = [f"# log digest: {os.path.basename(os.path.normpath(directory))}"]
    manifest = os.path.join(directory, "session_manifest.txt")
    if os.path.isfile(manifest):
        with open(manifest, encoding="utf-8", errors="replace") as handle:
            parts.append("## manifest")
            parts.extend(f"- {line.rstrip()}" for line in handle if line.strip())
    for section in (section_files(files), section_processes(files), section_problems(files, problem_groups),
                    section_timeline(files, timeline_lines), section_gaps(files), section_volume(files)):
        if section:
            parts.extend(section)
    return "\n".join(parts) + "\n"


def self_test() -> int:
    with tempfile.TemporaryDirectory() as session:
        def write(name: str, lines: list[str]) -> None:
            with open(os.path.join(session, name), "w", encoding="utf-8") as handle:
                handle.write("\n".join(lines) + "\n")

        write("hook_debug.log", [
            "10:00:00.000 T0001 #0 p42 DllMain: Process 'game.exe' pid=42 x64 is a whitelisted hook target",
            "10:00:00.100 T0001 #1 p42 DX12: Post-SL overlay SUBMIT #1 (queue=0000000000001234) (+5 unchanged)",
            "10:00:00.200 T0001 #3 p42 [WARN] Overlay init failed hr=0x887A0005",
            "10:00:05.000 T0002 #4 p42 FG EVENT kind=streamline-enable runtime=DLSS_FG",
            "10:00:05.100 T0002 #5 p42 FG EVENT kind=streamline-enable runtime=DLSS_FG",
            "10:00:06.000 T0002 #6 p42 Stats dropped=0 errors=0",
            "[10:00:07.000] [T:0003] [S:9] [old.exe] Recording started",
        ])
        write("captureengine.log", ["[2026-10-02 10:00:01.000] [ERROR] [Controller] Recording failed to start"])
        write("sl.log", ["vendor noise"] * 3)
        text = digest(session)
        checks = {
            "process legend": "p42 = game.exe x64" in text,
            "warning grouped": "Overlay init failed" in text,
            "service error": "Recording failed to start" in text,
            "zero counters are not problems": "Stats dropped=0" not in text.split("## timeline")[0],
            "timeline collapses repeats": "(x2 until 10:00:05.100)" in text,
            "old format parsed": "Recording started" in text,
            "lost line reported": "1 line(s) lost between #0 and #6" in text,
            "silence reported": "silent 4.8s" in text,
            "vendor marked": "sl.log" in text and "vendor log" in text,
        }
        failed = [name for name, ok in checks.items() if not ok]
        if failed:
            print(text)
            print("FAILED:", ", ".join(failed))
            return 1
    print("log_digest self-test passed")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("session", nargs="?", help="session log directory (installed/captureengine/logs/<stamp>)")
    parser.add_argument("--pid", help="only lines of this process (hook/layer logs)")
    parser.add_argument("--since", help="HH:MM:SS lower bound")
    parser.add_argument("--until", help="HH:MM:SS upper bound")
    parser.add_argument("--timeline-lines", type=int, default=120)
    parser.add_argument("--problem-groups", type=int, default=60)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if not args.session or not os.path.isdir(args.session):
        parser.error("session directory required")
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stdout.write(digest(args.session, args.pid, args.since, args.until, args.timeline_lines, args.problem_groups))
    return 0


if __name__ == "__main__":
    sys.exit(main())
