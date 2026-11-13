#!/usr/bin/env python3
"""Digest of the compositor's update-batch flood and a swapchain hand-off in an ETL.

Reads a CE display stall seal (display_stall_<pid>_ep<N>_<reason>.etl) or any ETL recorded with
Microsoft-Windows-Dwm-Core (all keywords) and answers: how many update batches dwm.exe processed per second on each
channel, when a flood started and ended relative to the game's swapchain switch, and when the compositor realized the
new surface (the first CompSurfInfo_ForceUpdateRealization of a surface it had not realized before the switch).
GTA V Enhanced's cold frame-generation black screen is this pattern: the game's ClipCursor loop queues one batch per
call on dwm.exe's own channel 2, and the new swapchain is realized only once that backlog drains
(llm-wiki/display-stall-trace.md; tools/tracing/run_flood_repro.py reproduces it without the game).

    python tools/tracing/dwm_flood_digest.py <file.etl> [--switch-qpc-us N] [--pid GAME] [--save digest.json]
    python tools/tracing/dwm_flood_digest.py --load digest.json [--switch-qpc-us N] [--pid GAME]

--switch-qpc-us takes CE's `silenceStartQpcUs=` (any QPC microsecond time); without it the last SwapChainBind (of
--pid, when given) is the reference. Times print in seconds relative to the reference.
"""

import argparse
import json
import os
import sys
from collections import defaultdict
from dataclasses import dataclass, field

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import etl_payload  # noqa: E402
import etl_stall_digest  # noqa: E402

BUCKETS_PER_SECOND = 10
BATCH_EVENT = "CHANNELBATCHES_PROCESSED"
REALIZATION = "CompSurfInfo_ForceUpdateRealization"
MARKER_EVENTS = ("SwapChainBind", "SwapChainUnBind", REALIZATION, "IndependentFlip")
MAX_MARKERS = 200_000
FLOOD_BUCKET_THRESHOLD = 500  # batches per 100 ms (5k/s); DWM's own channel idles at ~60/s
FLOOD_GAP_BUCKETS = 5


@dataclass
class Marker:
    qpc: int
    pid: int
    tid: int
    name: str
    fields: dict


@dataclass
class FloodDigest:
    """Per-100 ms batch counts by (channel, process), per-second events by thread and by name, swapchain markers.
    Buckets are absolute: QPC time * 10 // perf_freq."""
    perf_freq: int = 0
    first: int = 0
    last: int = 0
    events: int = 0
    lost: int = 0
    batches: dict = field(default_factory=lambda: defaultdict(int))  # (bucket, channel, process) -> batches
    threads: dict = field(default_factory=lambda: defaultdict(int))  # (second, pid, tid) -> events
    names: dict = field(default_factory=lambda: defaultdict(int))  # (second, pid, name) -> events
    markers: list = field(default_factory=list)

    def add(self, qpc, pid, tid, name, describe):
        """One event; describe() returns its decoded payload text and is called only for the events kept."""
        if self.events == 0:
            self.first = qpc
        self.events += 1
        self.last = max(self.last, qpc)
        second = qpc // self.perf_freq
        self.threads[(second, pid, tid)] += 1
        self.names[(second, pid, name)] += 1
        if BATCH_EVENT in name:
            values = etl_payload.fields(describe())
            key = (qpc * BUCKETS_PER_SECOND // self.perf_freq, values.get("channelHandle", "?"),
                   values.get("processId", "?"))
            self.batches[key] += int(values.get("numberOfBatchesProcessed", "0") or 0)
        elif len(self.markers) < MAX_MARKERS and any(marker in name for marker in MARKER_EVENTS):
            self.markers.append(Marker(qpc, pid, tid, name, etl_payload.fields(describe())))

    def to_json(self):
        return {"perf_freq": self.perf_freq, "first": self.first, "last": self.last, "events": self.events,
                "lost": self.lost, "batches": [[*key, value] for key, value in self.batches.items()],
                "threads": [[*key, value] for key, value in self.threads.items()],
                "names": [[*key, value] for key, value in self.names.items()],
                "markers": [[m.qpc, m.pid, m.tid, m.name, m.fields] for m in self.markers]}

    @classmethod
    def from_json(cls, data):
        digest = cls(data["perf_freq"], data["first"], data["last"], data["events"], data["lost"])
        for bucket, channel, process, value in data["batches"]:
            digest.batches[(bucket, channel, process)] = value
        for second, pid, tid, value in data["threads"]:
            digest.threads[(second, pid, tid)] = value
        for second, pid, name, value in data["names"]:
            digest.names[(second, pid, name)] = value
        digest.markers = [Marker(*row) for row in data["markers"]]
        return digest


def read_digest(path):
    decoder = etl_payload.PayloadDecoder()
    digest = FloodDigest()

    def on_record(record, provider, name):
        header = record.header
        digest.add(header.timestamp, header.process_id, header.thread_id, name, lambda: decoder.describe(record))

    def on_open(perf_freq):
        digest.perf_freq = perf_freq

    _, header_lost, reader_lost = etl_stall_digest.read_records(path, on_record, on_open)
    digest.lost = header_lost[0] + header_lost[1] + reader_lost
    return digest


def busiest_process(digest, channel="2"):
    totals = defaultdict(int)
    for (_, ch, process), value in digest.batches.items():
        if ch == channel:
            totals[process] += value
    return max(totals, key=lambda process: totals[process]) if totals else None


def channel_buckets(digest, process, channel="2"):
    rates = defaultdict(int)
    for (bucket, ch, proc), value in digest.batches.items():
        if ch == channel and proc == process:
            rates[bucket] += value
    return rates


def floods(buckets, threshold=FLOOD_BUCKET_THRESHOLD, max_gap=FLOOD_GAP_BUCKETS):
    """Runs of busy 100 ms buckets as (first bucket, last bucket, total batches, peak bucket)."""
    busy = sorted(bucket for bucket, value in buckets.items() if value >= threshold)
    runs = []
    for bucket in busy:
        if runs and bucket - runs[-1][1] <= max_gap:
            runs[-1][1] = bucket
        else:
            runs.append([bucket, bucket])
    return [(start, end, sum(buckets.get(b, 0) for b in range(start, end + 1)),
             max(buckets.get(b, 0) for b in range(start, end + 1))) for start, end in runs]


def busiest_threads(digest, first_bucket, last_bucket, top=5):
    """The threads with the most events in the whole seconds covering the buckets, as `p<pid>/t<tid>=<per second>`."""
    lo = first_bucket // BUCKETS_PER_SECOND
    hi = last_bucket // BUCKETS_PER_SECOND
    totals = defaultdict(int)
    for (second, pid, tid), value in digest.threads.items():
        if lo <= second <= hi:
            totals[(pid, tid)] += value
    seconds = hi - lo + 1
    ranked = sorted(totals.items(), key=lambda item: item[1], reverse=True)[:top]
    return ", ".join(f"p{pid}/t{tid}={value / seconds:.0f}" for (pid, tid), value in ranked)


def reference_qpc(digest, switch_qpc_us=None, pid=None):
    if switch_qpc_us is not None:
        return switch_qpc_us * digest.perf_freq // 1_000_000
    binds = [m.qpc for m in digest.markers if m.name.startswith("SwapChainBind") and (pid is None or m.pid == pid)]
    return binds[-1] if binds else digest.first


def new_surface_realization(digest, after_qpc):
    """(qpc, luid) of the first realization after after_qpc of a surface not realized before it, or None."""
    before = set()
    for marker in digest.markers:
        luid = marker.fields.get("surfaceLuid")
        if not marker.name.startswith(REALIZATION) or luid is None:
            continue
        if marker.qpc < after_qpc:
            before.add(luid)
        elif luid not in before:
            return marker.qpc, luid
    return None


def first_independent_flip(digest, pid, after_qpc):
    return next((m.qpc for m in digest.markers
                 if m.name.startswith("IndependentFlip") and m.pid == pid and m.qpc >= after_qpc), None)


def report(digest, switch_qpc_us=None, pid=None, from_s=-30.0, to_s=15.0):
    freq = digest.perf_freq
    ref = reference_qpc(digest, switch_qpc_us, pid)

    def rel(qpc):
        return (qpc - ref) / freq

    def rel_bucket(bucket):
        return bucket / BUCKETS_PER_SECOND - ref / freq

    lines = [f"events={digest.events} span={(digest.last - digest.first) / freq:.1f}s perfFreq={freq} "
             f"lost={digest.lost} reference qpcUs={ref * 1_000_000 // freq}"
             f" ({'given' if switch_qpc_us is not None else 'last SwapChainBind'})"]
    dwm = busiest_process(digest)
    buckets = channel_buckets(digest, dwm) if dwm else {}
    lines.append(f"compositor channel 2: process {dwm}, {sum(buckets.values())} batches")
    runs = floods(buckets)
    for start, end, total, peak in runs:
        seconds = (end - start + 1) / BUCKETS_PER_SECOND
        lines.append(f"  flood {rel_bucket(start):+.1f}s..{rel_bucket(end + 1):+.1f}s: {total} batches, "
                     f"mean {total / seconds:.0f}/s, peak {peak * BUCKETS_PER_SECOND}/s")
    if runs:
        start, end, _, _ = max(runs, key=lambda run: run[2])
        lines.append("  busiest threads in the largest flood (events/s): " + busiest_threads(digest, start, end))
    realized = new_surface_realization(digest, ref)
    onset = f"{rel(realized[0]):+.3f}s (surfaceLuid={realized[1]})" if realized else "never"
    lines.append(f"new surface realized: {onset}")
    if pid is not None:
        flip = first_independent_flip(digest, pid, ref)
        lines.append(f"first independent flip of p{pid}: {f'{rel(flip):+.3f}s' if flip else 'never'}")
    lines.append("per second from the reference: t  ch2-batches  events" + (f"(p{pid})" if pid is not None else "")
                 + "  markers")
    events_by_second = defaultdict(int)
    for (second, event_pid, _), value in digest.threads.items():
        if pid is None or event_pid == pid:
            events_by_second[second] += value
    old_surfaces = {m.fields.get("surfaceLuid") for m in digest.markers
                    if m.name.startswith(REALIZATION) and m.qpc < ref}
    first_s = max(from_s, rel(digest.first))
    last_s = min(to_s, rel(digest.last))
    row = int(first_s // 1)
    while row <= last_s:
        lo, hi = ref + row * freq, ref + (row + 1) * freq
        batches = sum(v for b, v in buckets.items() if lo <= b * freq // BUCKETS_PER_SECOND < hi)
        events = sum(v for s, v in events_by_second.items() if lo <= s * freq < hi)
        marks = [marker_label(m, pid, old_surfaces) for m in digest.markers if lo <= m.qpc < hi]
        shown = " ".join(dict.fromkeys(mark for mark in marks if mark))
        lines.append(f"  {row:+4d}s {batches:9d} {events:9d}  {shown[:100]}")
        row += 1
    return "\n".join(lines)


def marker_label(marker, pid, old_surfaces):
    """Swapchain binds, realizations of surfaces new since the reference, and the game's independent flips."""
    if marker.name.startswith("SwapChainBind"):
        return f"bind(p{marker.pid})"
    luid = marker.fields.get("surfaceLuid")
    if marker.name.startswith(REALIZATION) and luid is not None and luid not in old_surfaces:
        return f"realize({luid})"
    if marker.name.startswith("IndependentFlip") and pid is not None and marker.pid == pid:
        return f"iflip(p{marker.pid})"
    return ""


def main(argv=None):
    parser = argparse.ArgumentParser(description=(__doc__ or "").split("\n\n")[0])
    parser.add_argument("etl", nargs="?")
    parser.add_argument("--load", help="a digest saved with --save instead of an ETL")
    parser.add_argument("--save", help="write the digest as JSON for later runs")
    parser.add_argument("--switch-qpc-us", type=int, help="reference time, QPC microseconds")
    parser.add_argument("--pid", type=int, help="the game process")
    parser.add_argument("--from-s", type=float, default=-30.0)
    parser.add_argument("--to-s", type=float, default=15.0)
    args = parser.parse_args(argv)
    if bool(args.etl) == bool(args.load):
        parser.error("give an ETL or --load, not both")
    if args.load:
        with open(args.load, encoding="utf-8") as source:
            digest = FloodDigest.from_json(json.load(source))
    else:
        digest = read_digest(args.etl)
    if args.save:
        with open(args.save, "w", encoding="utf-8") as out:
            json.dump(digest.to_json(), out)
    print(report(digest, args.switch_qpc_us, args.pid, args.from_s, args.to_s))
    return 0


if __name__ == "__main__":
    sys.exit(main())
