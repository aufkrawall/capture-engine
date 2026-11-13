#!/usr/bin/env python3
"""Digest of a display stall trace (display_stall_<pid>_ep<N>_<reason>.etl in a CE session folder).

CE's elevation service records the display pipeline into a circular ETL while a game is tracked with trace logging
(common/platform/display_flight_recorder.h) and seals it when an episode with nothing on screen ends
(captureengine/display_timing/display_stall_trace_policy.h). This reads it with Windows' own ETW consumer API (no
external program) and prints, per event stream (provider, event name, optionally process), how many events it carries,
when it starts and ends and its largest silence. A stage that stopped shows its gap over the episode; the first stage
whose gap starts earliest is where the frames stopped.

Times are QPC microseconds (the recorder uses the QPC clock), the unit of CE's `qpcUs=` log fields, so a gap lines up
with `[SL LIFETIME]`, `[SCANOUT STALL]` and `[DisplayTiming]` lines directly.

    python tools/tracing/etl_stall_digest.py <file.etl> [--pid 6612 --pid 1800] [--by-pid] [--from-us N --to-us N]
"""

import argparse
import ctypes
import sys
from collections import defaultdict
from ctypes import wintypes
from dataclasses import dataclass, field

INVALID_PROCESSTRACE_HANDLE = 0xFFFFFFFFFFFFFFFF
PROCESS_TRACE_MODE_EVENT_RECORD = 0x10000000
PROCESS_TRACE_MODE_RAW_TIMESTAMP = 0x00001000
# EVENT_TRACE_LOGFILEW on x64; the header fields are read at fixed offsets (TRACE_LOGFILE_HEADER at 120).
LOGFILE_SIZE = 448
LOGFILE_HEADER = 120
HEADER_POINTER_SIZE = LOGFILE_HEADER + 44
HEADER_EVENTS_LOST = LOGFILE_HEADER + 48
HEADER_PERF_FREQ = LOGFILE_HEADER + 256
HEADER_BUFFERS_LOST = LOGFILE_HEADER + 276
LOGFILE_EVENTS_LOST = 416
LOGFILE_CALLBACK = 424


class Guid(ctypes.Structure):
    _fields_ = [("data1", ctypes.c_uint32), ("data2", ctypes.c_uint16), ("data3", ctypes.c_uint16),
                ("data4", ctypes.c_ubyte * 8)]

    def text(self):
        tail = bytes(self.data4).hex()
        return f"{self.data1:08x}-{self.data2:04x}-{self.data3:04x}-{tail[:4]}-{tail[4:]}"


class EventDescriptor(ctypes.Structure):
    _fields_ = [("id", ctypes.c_uint16), ("version", ctypes.c_ubyte), ("channel", ctypes.c_ubyte),
                ("level", ctypes.c_ubyte), ("opcode", ctypes.c_ubyte), ("task", ctypes.c_uint16),
                ("keyword", ctypes.c_uint64)]


class EventHeader(ctypes.Structure):
    _fields_ = [("size", ctypes.c_uint16), ("header_type", ctypes.c_uint16), ("flags", ctypes.c_uint16),
                ("event_property", ctypes.c_uint16), ("thread_id", ctypes.c_uint32), ("process_id", ctypes.c_uint32),
                ("timestamp", ctypes.c_int64), ("provider", Guid), ("descriptor", EventDescriptor),
                ("processor_time", ctypes.c_uint64), ("activity", Guid)]


class EventRecord(ctypes.Structure):
    _fields_ = [("header", EventHeader), ("buffer_context", ctypes.c_uint32), ("extended_count", ctypes.c_uint16),
                ("user_data_length", ctypes.c_uint16), ("extended", ctypes.c_void_p), ("user_data", ctypes.c_void_p),
                ("user_context", ctypes.c_void_p)]


assert ctypes.sizeof(EventHeader) == 80
CALLBACK = ctypes.WINFUNCTYPE(None, ctypes.POINTER(EventRecord))


@dataclass
class Stream:
    count: int = 0
    first: int = 0
    last: int = 0
    gap: int = 0
    gap_start: int = 0
    gap_end: int = 0

    def add(self, timestamp):
        if self.count == 0:
            self.first = timestamp
        elif timestamp - self.last > self.gap:
            self.gap = timestamp - self.last
            self.gap_start = self.last
            self.gap_end = timestamp
        self.last = max(self.last, timestamp)
        self.count += 1


@dataclass
class Digest:
    perf_freq: int = 0
    events: int = 0
    first: int = 0
    last: int = 0
    header_events_lost: int = 0
    header_buffers_lost: int = 0
    reader_events_lost: int = 0
    streams: dict = field(default_factory=lambda: defaultdict(Stream))

    def add(self, key, timestamp):
        if self.events == 0:
            self.first = timestamp
        self.events += 1
        self.last = max(self.last, timestamp)
        self.streams[key].add(timestamp)


def summarize(events, perf_freq, by_pid=False, pids=None, window=None):
    """events: iterable of (timestamp_qpc, provider, name, pid). window: (from_us, to_us) in QPC microseconds."""
    digest = Digest(perf_freq=perf_freq)
    for timestamp, provider, name, pid in events:
        if pids and pid not in pids:
            continue
        if window and not window[0] <= to_us(timestamp, perf_freq) <= window[1]:
            continue
        digest.add((provider, name, pid) if by_pid else (provider, name), timestamp)
    return digest


def to_us(timestamp, perf_freq):
    return timestamp * 1_000_000 // perf_freq if perf_freq else timestamp


def format_digest(digest, top):
    us = lambda value: to_us(value, digest.perf_freq)  # noqa: E731
    lines = [f"events={digest.events} perfFreq={digest.perf_freq} span={us(digest.first)}..{us(digest.last)}us "
             f"lost(header events={digest.header_events_lost} buffers={digest.header_buffers_lost} "
             f"reader={digest.reader_events_lost})",
             "streams by largest silence (QPC us; gap = longest stretch without the event):"]
    ranked = sorted(digest.streams.items(), key=lambda item: item[1].gap, reverse=True)
    for key, stream in ranked[:top]:
        label = " / ".join(str(part) for part in key)
        lines.append(f"  {label}: count={stream.count} first={us(stream.first)} last={us(stream.last)} "
                     f"gap={(us(stream.gap_end) - us(stream.gap_start)) // 1000}ms "
                     f"[{us(stream.gap_start)}..{us(stream.gap_end)}]")
    return "\n".join(lines)


class EventNames:
    """Provider and event names from the providers' manifests (TDH), cached per event descriptor."""

    def __init__(self):
        self.tdh = ctypes.WinDLL("tdh")
        self.tdh.TdhGetEventInformation.argtypes = [ctypes.POINTER(EventRecord), wintypes.ULONG, ctypes.c_void_p,
                                                    ctypes.c_void_p, ctypes.POINTER(wintypes.ULONG)]
        self.cache = {}

    def lookup(self, record):
        header = record.header
        key = (header.provider.text(), header.descriptor.id, header.descriptor.version, header.descriptor.opcode)
        names = self.cache.get(key)
        if names is None:
            names = self.decode(record) or (key[0], f"id{key[1]}/op{key[3]}")
            self.cache[key] = names
        return names

    def decode(self, record):
        size = wintypes.ULONG(0)
        self.tdh.TdhGetEventInformation(ctypes.byref(record), 0, None, None, ctypes.byref(size))
        if size.value == 0:
            return None
        buffer = ctypes.create_string_buffer(size.value)
        if self.tdh.TdhGetEventInformation(ctypes.byref(record), 0, None, buffer, ctypes.byref(size)) != 0:
            return None
        raw = buffer.raw

        def text(offset_at):
            offset = int.from_bytes(raw[offset_at:offset_at + 4], "little")
            if offset == 0 or offset >= len(raw):
                return ""
            end = offset
            while end + 1 < len(raw) and raw[end:end + 2] != b"\0\0":
                end += 2
            return raw[offset:end].decode("utf-16-le", "replace").strip()

        provider = text(52) or record.header.provider.text()
        event = text(92) or "/".join(part for part in (text(68), text(72)) if part)
        return provider, event or f"id{record.header.descriptor.id}"


def read_etl(path, sink):
    """Calls sink(timestamp, provider, name, pid) for every event; returns (perf_freq, header lost, reader lost)."""
    def forward(record, provider, name):
        sink(record.header.timestamp, provider, name, record.header.process_id)

    return read_records(path, forward)


def read_records(path, on_record, on_open=None):
    """Calls on_record(record, provider, name) for every event, with the EVENT_RECORD valid only during the call (its
    payload is at record.user_data), after on_open(perf_freq) once the file is open; returns (perf_freq, header lost,
    reader lost)."""
    advapi = ctypes.WinDLL("advapi32", use_last_error=True)
    advapi.OpenTraceW.restype = ctypes.c_uint64
    advapi.OpenTraceW.argtypes = [ctypes.c_void_p]
    advapi.ProcessTrace.argtypes = [ctypes.POINTER(ctypes.c_uint64), wintypes.ULONG, ctypes.c_void_p,
                                    ctypes.c_void_p]
    advapi.CloseTrace.argtypes = [ctypes.c_uint64]
    names = EventNames()

    def on_event(pointer):
        record = pointer.contents
        provider, name = names.lookup(record)
        on_record(record, provider, name)

    callback = CALLBACK(on_event)
    logfile = (ctypes.c_ubyte * LOGFILE_SIZE)()
    path_buffer = ctypes.create_unicode_buffer(path)
    ctypes.c_void_p.from_buffer(logfile, 0).value = ctypes.cast(path_buffer, ctypes.c_void_p).value
    ctypes.c_uint32.from_buffer(logfile, 28).value = PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP
    ctypes.c_void_p.from_buffer(logfile, LOGFILE_CALLBACK).value = ctypes.cast(callback, ctypes.c_void_p).value
    handle = ctypes.c_uint64(advapi.OpenTraceW(ctypes.byref(logfile)))
    if handle.value == INVALID_PROCESSTRACE_HANDLE:
        raise OSError(ctypes.get_last_error(), f"OpenTraceW failed for {path}")
    try:
        if ctypes.c_uint32.from_buffer(logfile, HEADER_POINTER_SIZE).value not in (4, 8):
            raise RuntimeError("unexpected EVENT_TRACE_LOGFILEW layout (pointer size)")
        if on_open:
            on_open(ctypes.c_int64.from_buffer(logfile, HEADER_PERF_FREQ).value)
        status = advapi.ProcessTrace(ctypes.byref(handle), 1, None, None)
        if status not in (0, 1223):  # ERROR_CANCELLED ends a trace the same way
            raise OSError(status, f"ProcessTrace failed for {path}")
    finally:
        advapi.CloseTrace(handle)
    perf_freq = ctypes.c_int64.from_buffer(logfile, HEADER_PERF_FREQ).value
    header_lost = (ctypes.c_uint32.from_buffer(logfile, HEADER_EVENTS_LOST).value,
                   ctypes.c_uint32.from_buffer(logfile, HEADER_BUFFERS_LOST).value)
    return perf_freq, header_lost, ctypes.c_uint32.from_buffer(logfile, LOGFILE_EVENTS_LOST).value


def main(argv=None):
    parser = argparse.ArgumentParser(description=(__doc__ or "").split("\n\n")[0])
    parser.add_argument("etl")
    parser.add_argument("--pid", type=int, action="append", help="only these processes (repeatable)")
    parser.add_argument("--by-pid", action="store_true", help="one stream per process")
    parser.add_argument("--from-us", type=int, help="window start, QPC microseconds")
    parser.add_argument("--to-us", type=int, help="window end, QPC microseconds")
    parser.add_argument("--top", type=int, default=40)
    args = parser.parse_args(argv)
    collected = []
    perf_freq, header_lost, reader_lost = read_etl(args.etl, lambda *event: collected.append(event))
    window = None
    if args.from_us is not None or args.to_us is not None:
        window = (args.from_us or 0, args.to_us if args.to_us is not None else 2 ** 63)
    digest = summarize(collected, perf_freq, args.by_pid, set(args.pid or []), window)
    digest.header_events_lost, digest.header_buffers_lost = header_lost
    digest.reader_events_lost = reader_lost
    print(format_digest(digest, args.top))
    return 0


if __name__ == "__main__":
    sys.exit(main())
