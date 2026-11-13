"""Decodes an ETW event's payload into `name=value` text from the provider's manifest (TDH), with no external program.

`PayloadDecoder.describe(record)` takes the EVENT_RECORD handed out by `etl_stall_digest.read_records` and returns its
top-level properties, e.g. `channelHandle=2 processId=1732 numberOfBatchesProcessed=1`. Schemas are cached per event
descriptor. Structs end the decode (`name=<struct>`), arrays show at most 8 items, floats are not decoded.

As a program it dumps the decoded events of a time window (QPC microseconds, CE's `qpcUs=` unit):

    python tools/tracing/etl_payload.py <file.etl> --from-us N --to-us N [--provider REGEX] [--name REGEX] [--pid N]
"""

import argparse
import ctypes
import os
import re
import sys
from ctypes import wintypes
from dataclasses import dataclass

# TRACE_EVENT_INFO: TopLevelPropertyCount at 104, EVENT_PROPERTY_INFO[] (24 bytes each) from 112.
TOP_LEVEL_COUNT_OFFSET = 104
PROPERTY_ARRAY_OFFSET = 112
PROPERTY_INFO_SIZE = 24
PROPERTY_STRUCT = 0x1
PROPERTY_PARAM_LENGTH = 0x2
PROPERTY_PARAM_COUNT = 0x4
PROPERTY_PARAM_FIXED_COUNT = 0x20
MAX_ARRAY_ITEMS = 8
EVENT_HEADER_FLAG_32_BIT_HEADER = 0x20

# TDH in-types: size in bytes and signedness of the fixed-size integers.
IN_TYPE_UNICODE_STRING = 1
IN_TYPE_ANSI_STRING = 2
IN_TYPE_FLOAT = 11
IN_TYPE_DOUBLE = 12
IN_TYPE_BINARY = 14
IN_TYPE_GUID = 15
IN_TYPE_POINTER = 16
IN_TYPE_SID = 19
FIXED_INTEGERS = {3: (1, True), 4: (1, False), 5: (2, True), 6: (2, False), 7: (4, True), 8: (4, False),
                  9: (8, True), 10: (8, False), 13: (4, False), 17: (8, False), 20: (4, False), 21: (8, False)}
HEX_IN_TYPES = {20, 21}


@dataclass(frozen=True)
class Property:
    flags: int
    name: str
    in_type: int
    count: int  # fixed count, or the index of the property holding the count (PROPERTY_PARAM_COUNT)
    length: int  # fixed length, or the index of the property holding the length (PROPERTY_PARAM_LENGTH)


def parse_event_info(raw):
    """The top-level properties of a TRACE_EVENT_INFO buffer as returned by TdhGetEventInformation."""

    def text(offset):
        if offset == 0 or offset >= len(raw):
            return ""
        end = offset
        while end + 1 < len(raw) and raw[end:end + 2] != b"\0\0":
            end += 2
        return raw[offset:end].decode("utf-16-le", "replace").strip()

    def u16(at):
        return int.from_bytes(raw[at:at + 2], "little")

    def u32(at):
        return int.from_bytes(raw[at:at + 4], "little")

    properties = []
    for index in range(u32(TOP_LEVEL_COUNT_OFFSET)):
        base = PROPERTY_ARRAY_OFFSET + index * PROPERTY_INFO_SIZE
        properties.append(Property(u32(base), text(u32(base + 4)), u16(base + 8), u16(base + 16), u16(base + 18)))
    return properties


def _read_item(prop, data, at, pointer_size, values):
    """One item of prop at data[at:]: (shown text, value for later count/length references, next offset), or None."""
    if prop.in_type in FIXED_INTEGERS:
        size, signed = FIXED_INTEGERS[prop.in_type]
        if at + size > len(data):
            return None
        value = int.from_bytes(data[at:at + size], "little", signed=signed)
        return (hex(value) if prop.in_type in HEX_IN_TYPES else str(value)), value, at + size
    if prop.in_type == IN_TYPE_POINTER:
        if at + pointer_size > len(data):
            return None
        value = int.from_bytes(data[at:at + pointer_size], "little")
        return hex(value), value, at + pointer_size
    if prop.in_type == IN_TYPE_UNICODE_STRING:
        end = at
        while end + 1 < len(data) and data[end:end + 2] != b"\0\0":
            end += 2
        value = data[at:end].decode("utf-16-le", "replace")
        return repr(value[:60]), value, end + 2
    if prop.in_type == IN_TYPE_ANSI_STRING:
        end = data.find(b"\0", at)
        end = len(data) if end < 0 else end
        value = data[at:end].decode("latin-1")
        return repr(value[:60]), value, end + 1
    if prop.in_type == IN_TYPE_GUID:
        return data[at:at + 16].hex(), None, at + 16
    if prop.in_type == IN_TYPE_SID:
        size = 8 + 4 * data[at + 1] if at + 2 <= len(data) else 0
        return "sid", None, at + size
    if prop.in_type in (IN_TYPE_FLOAT, IN_TYPE_DOUBLE):
        return "<float>", None, at + (4 if prop.in_type == IN_TYPE_FLOAT else 8)
    if prop.in_type == IN_TYPE_BINARY:
        size = prop.length
        if prop.flags & PROPERTY_PARAM_LENGTH:
            size = _int_value(values, prop.length)
        return data[at:at + min(size, 24)].hex(), None, at + size
    return None


def _int_value(values, index):
    value = values[index] if index < len(values) else None
    return value if isinstance(value, int) else 0


def decode_properties(properties, data, pointer_size):
    """`name=value` text of a payload laid out as properties describe; stops at the first part it cannot decode."""
    out = []
    values = []
    at = 0
    for prop in properties:
        if prop.flags & PROPERTY_STRUCT:
            out.append(f"{prop.name}=<struct>")
            break
        items = 1
        if prop.flags & PROPERTY_PARAM_COUNT:
            items = _int_value(values, prop.count)
        elif prop.flags & PROPERTY_PARAM_FIXED_COUNT or prop.count > 1:
            items = prop.count
        shown = []
        value = None
        complete = items <= MAX_ARRAY_ITEMS
        for _ in range(min(items, MAX_ARRAY_ITEMS)):
            item = _read_item(prop, data, at, pointer_size, values)
            if item is None:
                complete = False
                break
            text, value, at = item
            shown.append(text)
        values.append(value)
        out.append(f"{prop.name}={','.join(shown) if items != 1 else (shown[0] if shown else '?')}")
        if not complete:
            out.append("...")
            break
    return " ".join(out)


def fields(text):
    """The `name=value` pairs of decoded text as a dict (values that contain spaces are cut at the first space)."""
    return dict(part.split("=", 1) for part in text.split(" ") if "=" in part)


class PayloadDecoder:
    """Decodes EVENT_RECORDs with TDH; one schema lookup per event descriptor."""

    def __init__(self):
        self.tdh = ctypes.WinDLL("tdh")
        self.tdh.TdhGetEventInformation.argtypes = [ctypes.c_void_p, wintypes.ULONG, ctypes.c_void_p,
                                                    ctypes.c_void_p, ctypes.POINTER(wintypes.ULONG)]
        self.schemas = {}

    def describe(self, record):
        header = record.header
        key = (header.provider.text(), header.descriptor.id, header.descriptor.version, header.descriptor.opcode)
        if key not in self.schemas:
            self.schemas[key] = self._schema(record)
        properties = self.schemas[key]
        data = ctypes.string_at(record.user_data, record.user_data_length) if record.user_data_length else b""
        if not properties:
            return data[:32].hex()
        pointer_size = 4 if header.flags & EVENT_HEADER_FLAG_32_BIT_HEADER else 8
        return decode_properties(properties, data, pointer_size)

    def _schema(self, record):
        size = wintypes.ULONG(0)
        self.tdh.TdhGetEventInformation(ctypes.byref(record), 0, None, None, ctypes.byref(size))
        if size.value == 0:
            return None
        buffer = ctypes.create_string_buffer(size.value)
        if self.tdh.TdhGetEventInformation(ctypes.byref(record), 0, None, buffer, ctypes.byref(size)) != 0:
            return None
        return parse_event_info(buffer.raw)


def main(argv=None):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import etl_stall_digest

    parser = argparse.ArgumentParser(description=(__doc__ or "").split("\n\n")[0])
    parser.add_argument("etl")
    parser.add_argument("--from-us", type=int, required=True, help="window start, QPC microseconds")
    parser.add_argument("--to-us", type=int, required=True, help="window end, QPC microseconds")
    parser.add_argument("--provider", help="regex on the provider name")
    parser.add_argument("--name", help="regex on the event name")
    parser.add_argument("--pid", type=int, action="append", help="only these processes (repeatable)")
    parser.add_argument("--limit", type=int, default=20000, help="stop printing after this many events")
    args = parser.parse_args(argv)
    provider_filter = re.compile(args.provider) if args.provider else None
    name_filter = re.compile(args.name) if args.name else None
    decoder = PayloadDecoder()
    state = {"freq": 0, "printed": 0}

    def on_open(perf_freq):
        state["freq"] = perf_freq

    def on_record(record, provider, name):
        header = record.header
        us = header.timestamp * 1_000_000 // state["freq"]
        if not args.from_us <= us <= args.to_us or state["printed"] >= args.limit:
            return
        if args.pid and header.process_id not in args.pid:
            return
        if (provider_filter and not provider_filter.search(provider)) or (name_filter and not name_filter.search(name)):
            return
        state["printed"] += 1
        print(f"{us} p{header.process_id} t{header.thread_id} {provider.replace('Microsoft-Windows-', '')}/{name} "
              f"{decoder.describe(record)}")

    etl_stall_digest.read_records(args.etl, on_record, on_open)
    return 0


if __name__ == "__main__":
    sys.exit(main())
