"""Post-mortem triage for CaptureEngine crash dumps.

Answers the three questions a minidump has to answer after a foreign crash:

1. where did the target fault (module + RVA, and what instruction is there),
2. what code led there (code pointers on the faulting thread's stack),
3. what was the code referencing (relative CALL targets and rip-relative
   LEA/MOV data targets - message strings and error records), and which of
   those references the dump actually captured.

Point 3 is the one session 20261010_160937 failed by hand: The Witcher 3 died
of its own compiled-in breakpoint trap, and the answer to "what was the game
reporting" sat in .rdata windows the dump did not contain. The fault
neighborhood collector (captureengine/diagnostics/dump_helper_fault_neighborhood)
now records them; this tool reads them back and says when one is still missing.

The scan is deliberately heuristic (no disassembler): a misread operand only
picks a wrong address to look up. x64 dumps only.
"""

from __future__ import annotations

import argparse
import os
import struct
import sys
import tempfile
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Sequence, Set, Tuple

_STREAM_MODULE_LIST = 4
_STREAM_MEMORY_LIST = 5
_STREAM_EXCEPTION = 6
_STREAM_MEMORY64_LIST = 9

_MODULE_ENTRY_SIZE = 108
_MEMORY_ENTRY_SIZE = 16

_RSP_OFFSET = 0x98
_RIP_OFFSET = 0xF8

_STATUS_BREAKPOINT = 0x80000003


@dataclass(frozen=True)
class Module:
    base: int
    size: int
    name: str

    def resolve(self, address: int) -> Optional[int]:
        if self.base <= address < self.base + self.size:
            return address - self.base
        return None


@dataclass(frozen=True)
class MemoryRange:
    start: int
    size: int
    file_offset: int

    def contains(self, address: int) -> bool:
        return self.start <= address < self.start + self.size


@dataclass
class DumpModel:
    path: str
    file_size: int
    modules: List[Module] = field(default_factory=list)
    ranges: List[MemoryRange] = field(default_factory=list)
    exception_code: int = 0
    exception_address: int = 0
    exception_thread: int = 0
    rip: int = 0
    rsp: int = 0

    def module_of(self, address: int) -> Optional[Tuple[Module, int]]:
        for module in self.modules:
            offset = module.resolve(address)
            if offset is not None:
                return module, offset
        return None

    def read_memory(self, address: int, size: int) -> bytes:
        """Reads up to `size` bytes at `address` from the captured ranges.

        Only contiguous bytes are returned: a gap means the dump did not
        capture the rest, which callers report instead of hiding.
        """
        for memory_range in self.ranges:
            if not memory_range.contains(address):
                continue
            available = min(size, memory_range.start + memory_range.size - address)
            with open(self.path, "rb") as handle:
                handle.seek(memory_range.file_offset + (address - memory_range.start))
                return handle.read(available)
        return b""

    def window_of(self, address: int) -> Optional[Tuple[int, bytes]]:
        """The whole captured range containing `address`, as (base, bytes)."""
        for memory_range in self.ranges:
            if memory_range.contains(address):
                return memory_range.start, self.read_memory(memory_range.start, memory_range.size)
        return None


def _read_struct(handle, offset: int, fmt: str) -> Tuple[int, ...]:
    handle.seek(offset)
    data = handle.read(struct.calcsize(fmt))
    if len(data) != struct.calcsize(fmt):
        raise ValueError(f"truncated minidump at offset {offset:#x}")
    return struct.unpack(fmt, data)


def _read_minidump_string(handle, offset: int) -> str:
    (length,) = _read_struct(handle, offset, "<I")
    handle.seek(offset + 4)
    raw = handle.read(min(length, 1024))
    return raw.decode("utf-16-le", "replace").rstrip("\x00")


def parse_minidump(path: str) -> DumpModel:
    model = DumpModel(path=path, file_size=0)
    with open(path, "rb") as handle:
        handle.seek(0, 2)
        model.file_size = handle.tell()

        signature, _version, stream_count, directory_rva = _read_struct(handle, 0, "<4sIII")
        if signature != b"MDMP":
            raise ValueError("not a minidump (missing MDMP signature)")

        streams: Dict[int, Tuple[int, int]] = {}
        for index in range(stream_count):
            stream_type, data_size, rva = _read_struct(handle, directory_rva + index * 12, "<III")
            streams.setdefault(stream_type, (rva, data_size))

        module_stream = streams.get(_STREAM_MODULE_LIST)
        if module_stream:
            (count,) = _read_struct(handle, module_stream[0], "<I")
            for index in range(count):
                entry = module_stream[0] + 4 + index * _MODULE_ENTRY_SIZE
                base, size, _checksum, _stamp, name_rva = _read_struct(handle, entry, "<QIIII")
                model.modules.append(Module(base, size, _read_minidump_string(handle, name_rva)))
            model.modules.sort(key=lambda module: module.base)

        for stream_type in (_STREAM_MEMORY_LIST, _STREAM_MEMORY64_LIST):
            memory_stream = streams.get(stream_type)
            if not memory_stream:
                continue
            if stream_type == _STREAM_MEMORY64_LIST:
                count, data_rva = _read_struct(handle, memory_stream[0], "<QQ")
                cursor = data_rva
                for index in range(count):
                    start, size = _read_struct(handle, memory_stream[0] + 16 + index * 16, "<QQ")
                    model.ranges.append(MemoryRange(start, size, cursor))
                    cursor += size
            else:
                (count,) = _read_struct(handle, memory_stream[0], "<I")
                for index in range(count):
                    start, size, rva = _read_struct(
                        handle, memory_stream[0] + 4 + index * _MEMORY_ENTRY_SIZE, "<QII"
                    )
                    model.ranges.append(MemoryRange(start, size, rva))

        exception_stream = streams.get(_STREAM_EXCEPTION)
        if exception_stream:
            # EXCEPTION_RECORD after the stream's thread id and alignment:
            # code, flags, record pointer, address, parameter count, spare.
            thread_id, _align, code, _flags, _record, address, _parameters, _spare = _read_struct(
                handle, exception_stream[0], "<IIIIQQII"
            )
            model.exception_thread = thread_id
            model.exception_code = code & 0xFFFFFFFF
            model.exception_address = address
            context_size, context_rva = _read_struct(handle, exception_stream[0] + 160, "<II")
            if context_size >= _RIP_OFFSET + 8:
                handle.seek(context_rva)
                context = handle.read(min(context_size, 0x400))
                (model.rsp,) = struct.unpack_from("<Q", context, _RSP_OFFSET)
                (model.rip,) = struct.unpack_from("<Q", context, _RIP_OFFSET)

    return model


def harvest_code_references(code: bytes, code_address: int) -> List[Tuple[int, bool]]:
    """Finds relative CALL targets and rip-relative MOV/LEA data targets.

    Mirrors ce::fault_neighborhood::HarvestCodeReferences; keep the two in
    step. Returns (address, is_code) pairs in scan order, deduplicated.
    """
    references: List[Tuple[int, bool]] = []
    seen: Set[int] = set()

    def append(target: int, is_code: bool) -> None:
        if target in seen:
            return
        seen.add(target)
        references.append((target, is_code))

    for index in range(0, max(0, len(code) - 4)):
        byte = code[index]
        if byte == 0xE8 and index + 5 <= len(code):
            (relative,) = struct.unpack_from("<i", code, index + 1)
            append(code_address + index + 5 + relative, True)
            continue
        if index + 7 > len(code):
            continue
        if code[index] not in (0x48, 0x49, 0x4C, 0x4D):
            continue
        if code[index + 1] not in (0x8B, 0x8D):
            continue
        if code[index + 2] & 0xC7 != 0x05:
            continue
        (displacement,) = struct.unpack_from("<i", code, index + 3)
        append(code_address + index + 7 + displacement, False)
    return references


def extract_strings(data: bytes, minimum: int = 6) -> List[str]:
    strings: List[str] = []
    current: List[str] = []
    for byte in data:
        if 0x20 <= byte < 0x7F:
            current.append(chr(byte))
        else:
            if len(current) >= minimum:
                strings.append("".join(current))
            current = []
    if len(current) >= minimum:
        strings.append("".join(current))
    return strings


def _describe_module(model: DumpModel, address: int) -> str:
    resolved = model.module_of(address)
    if resolved is None:
        return "unknown module"
    module, offset = resolved
    return f"{module.name} + {offset:#x}"


def _trap_description(code: bytes) -> str:
    if not code:
        return "not captured"
    if code[0] == 0xCC:
        return "int3 trap"
    if len(code) >= 2 and code[0] == 0x0F and code[1] == 0x0B:
        return "ud2 (illegal instruction trap)"
    if len(code) >= 2 and code[0] == 0xCD and code[1] == 0x03:
        return "int 3 trap"
    return "see bytes"


def _collect_references(model: DumpModel, points: Sequence[int]) -> List[Tuple[int, bool]]:
    references: List[Tuple[int, bool]] = []
    seen: Set[int] = set()
    windows: Set[int] = set()
    for point in points:
        window = model.window_of(point)
        if window is None or window[0] in windows:
            continue
        windows.add(window[0])
        for address, is_code in harvest_code_references(window[1], window[0]):
            if address in seen:
                continue
            seen.add(address)
            references.append((address, is_code))
    return references


def triage(model: DumpModel) -> str:
    lines: List[str] = []
    megabytes = model.file_size / 1e6
    lines.append(f"dump: {model.path}")
    lines.append(f"  {megabytes:.1f} MB, {len(model.modules)} modules, {len(model.ranges)} memory ranges")
    lines.append(
        f"exception: {model.exception_code:#010x} at {model.exception_address:#014x} "
        f"(thread {model.exception_thread:#x})"
    )
    lines.append(f"  resolves to: {_describe_module(model, model.exception_address)}")

    fault = model.read_memory(model.exception_address, 16)
    hex_bytes = " ".join(f"{byte:02X}" for byte in fault)
    lines.append(f"  instruction bytes: {hex_bytes or 'NOT CAPTURED'} ({_trap_description(fault)})")
    if model.rip and model.rip != model.exception_address:
        lines.append(f"  context rip {model.rip:#014x} differs from the exception address")

    lines.append(f"faulting thread stack: rsp {model.rsp:#014x}")
    stack = model.read_memory(model.rsp, 0x1000)
    code_pointers: List[Tuple[int, int]] = []
    for offset in range(0, max(0, len(stack) - 7), 8):
        (value,) = struct.unpack_from("<Q", stack, offset)
        if value < 0x10000:
            continue
        if model.module_of(value) is not None:
            code_pointers.append((offset, value))
    for offset, value in code_pointers[:16]:
        marker = " <- fault" if value == model.rip else ""
        lines.append(f"  [rsp+{offset:#06x}] {value:#014x} {_describe_module(model, value)}{marker}")
    if not code_pointers:
        lines.append("  no module code pointers found in captured stack bytes")

    harvest_points = [model.exception_address] + [value for _offset, value in code_pointers]
    references = _collect_references(model, harvest_points)
    lines.append(f"references harvested from captured code windows ({len(references)}):")
    for address, is_code in references[:32]:
        kind = "code" if is_code else "data"
        data = model.read_memory(address, 256)
        if not data:
            lines.append(f"  {kind} {address:#014x} MISSING from dump")
            continue
        strings = extract_strings(data)
        if strings:
            lines.append(f"  {kind} {address:#014x} captured: {strings[0][:80]!r}")
        else:
            preview = " ".join(f"{byte:02X}" for byte in data[:16])
            lines.append(f"  {kind} {address:#014x} captured: {preview}")
    if not references:
        lines.append("  (none - no code window captured around the fault or call sites)")

    if model.exception_code == _STATUS_BREAKPOINT:
        lines.append(
            "note: STATUS_BREAKPOINT - most are the application's own trap or a "
            "handled probe; check the instruction bytes and the referencing call site"
        )
    return "\n".join(lines)


def _build_self_test_dump() -> bytes:
    """A synthetic dump mirroring session 20261010_160937's shape."""
    game_base = 0x7FF7005C0000
    game_size = 0x6402000
    fault = 0x7FF7024D0AC3
    stack_pointer = 0x83EEFFF5C0
    code_window = fault - 0xC3  # the fault sits at offset 0xC3 like the real one
    string_address = 0x7FF706389FB4
    missing_callee = 0x7FF702DBF1F0
    return_address = 0x7FF7024D0EAC

    code = bytearray(0x100)
    code[0xC3] = 0xCC  # the trap
    code[0xC4:0xC9] = b"\x48\x83\xC4\x38\xC3"
    lea_displacement = string_address - (code_window + 0x17)
    code[0x10:0x17] = struct.pack("<BBBi", 0x48, 0x8D, 0x0D, lea_displacement)
    call_displacement = missing_callee - (code_window + 0x25)
    code[0x20:0x25] = struct.pack("<Bi", 0xE8, call_displacement)

    # The collector emits a reference window aligned down from the target, so
    # the captured range starts 4 bytes before the string.
    data = bytearray(0x50)
    message = b"GAME ERROR: device removed\0"
    data[0x04 : 0x04 + len(message)] = message

    stack = bytearray(0x80)
    struct.pack_into("<Q", stack, 0x38, return_address)

    module_names = [
        (game_base, game_size, "game.exe"),
        (0x7FFA430F0000, 0xACD000, "capture_hook_x64.dll"),
    ]
    name_blobs = [struct.pack("<I", len(name.encode("utf-16-le"))) + name.encode("utf-16-le")
                  for _base, _size, name in module_names]

    module_stream = bytearray(4 + len(module_names) * _MODULE_ENTRY_SIZE)
    struct.pack_into("<I", module_stream, 0, len(module_names))
    name_offset = 4 + len(module_names) * _MODULE_ENTRY_SIZE
    for index, (base, size, _name) in enumerate(module_names):
        struct.pack_into("<QIIII", module_stream, 4 + index * _MODULE_ENTRY_SIZE,
                         base, size, 0, 0, name_offset)
        name_offset += len(name_blobs[index])
    module_stream += b"".join(name_blobs)

    context = bytearray(0x400)
    struct.pack_into("<Q", context, _RSP_OFFSET, stack_pointer)
    struct.pack_into("<Q", context, _RIP_OFFSET, fault)

    exception_stream = bytearray(168)
    struct.pack_into("<II", exception_stream, 0, 0x640C, 0)
    struct.pack_into("<IIQQII", exception_stream, 8, _STATUS_BREAKPOINT, 0, 0, fault, 0, 0)

    memory_blobs = [
        (code_window, bytes(code)),
        (string_address & ~0xF, bytes(data)),
        (stack_pointer, bytes(stack)),
    ]
    memory_stream = bytearray(4 + len(memory_blobs) * _MEMORY_ENTRY_SIZE)
    struct.pack_into("<I", memory_stream, 0, len(memory_blobs))

    chunks = [
        bytes(module_stream),
        bytes(exception_stream),
        bytes(context),
        bytes(memory_stream),
    ] + [blob for _start, blob in memory_blobs]

    stream_count = 3
    directory_rva = 0x20
    cursor = directory_rva + stream_count * 12
    chunk_offsets: List[int] = []
    for chunk in chunks:
        chunk_offsets.append(cursor)
        cursor += len(chunk)

    struct.pack_into("<II", exception_stream, 160, len(context), chunk_offsets[2])
    for index, (start, blob) in enumerate(memory_blobs):
        struct.pack_into("<QII", memory_stream, 4 + index * _MEMORY_ENTRY_SIZE,
                         start, len(blob), chunk_offsets[4 + index])
    name_offset = 4 + len(module_names) * _MODULE_ENTRY_SIZE
    for index, blob in enumerate(name_blobs):
        struct.pack_into("<I", module_stream, 4 + index * _MODULE_ENTRY_SIZE + 20,
                         chunk_offsets[0] + name_offset)
        name_offset += len(blob)
    chunks[0] = bytes(module_stream)
    chunks[1] = bytes(exception_stream)
    chunks[3] = bytes(memory_stream)

    out = bytearray(cursor)
    struct.pack_into("<4sIII", out, 0, b"MDMP", 0xA0F4A793, stream_count, directory_rva)
    directory = [
        (_STREAM_MODULE_LIST, len(chunks[0]), chunk_offsets[0]),
        (_STREAM_EXCEPTION, len(chunks[1]), chunk_offsets[1]),
        (_STREAM_MEMORY_LIST, len(chunks[3]), chunk_offsets[3]),
    ]
    for index, (stream_type, data_size, rva) in enumerate(directory):
        struct.pack_into("<III", out, directory_rva + index * 12, stream_type, data_size, rva)
    for index, chunk in enumerate(chunks):
        out[chunk_offsets[index] : chunk_offsets[index] + len(chunk)] = chunk
    return bytes(out)


def self_test() -> int:
    payload = _build_self_test_dump()
    with tempfile.NamedTemporaryFile(suffix=".dmp", delete=False) as handle:
        handle.write(payload)
        path = handle.name
    try:
        model = parse_minidump(path)
        assert model.exception_code == _STATUS_BREAKPOINT, model.exception_code
        assert model.exception_address == 0x7FF7024D0AC3, hex(model.exception_address)
        assert model.exception_thread == 0x640C
        assert model.rip == model.exception_address
        module = model.module_of(model.exception_address)
        assert module is not None and module[0].name == "game.exe", module
        assert module[1] == 0x1F10AC3, hex(module[1])

        fault = model.read_memory(model.exception_address, 4)
        assert fault[:1] == b"\xCC", fault
        assert _trap_description(fault) == "int3 trap"

        stack = model.read_memory(model.rsp, 0x80)
        (saved_return,) = struct.unpack_from("<Q", stack, 0x38)
        assert saved_return == 0x7FF7024D0EAC, hex(saved_return)
        assert model.module_of(saved_return)[0].name == "game.exe"

        window = model.window_of(model.exception_address)
        assert window is not None, "faulting code window not captured"
        references = harvest_code_references(window[1], window[0])
        addresses = dict(references)
        assert addresses.get(0x7FF706389FB4) is False, addresses
        assert addresses.get(0x7FF702DBF1F0) is True, addresses

        message = model.read_memory(0x7FF706389FB4, 256)
        strings = extract_strings(message)
        assert strings and strings[0].startswith("GAME ERROR"), strings
        assert model.read_memory(0x7FF702DBF1F0, 16) == b""

        report = triage(model)
        assert "game.exe + 0x1f10ac3" in report.lower(), report
        assert "int3 trap" in report
        assert "GAME ERROR" in report
        assert "MISSING from dump" in report
    finally:
        os.unlink(path)
    print("dump_triage self-test: OK")
    return 0


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Post-mortem triage for CaptureEngine crash dumps")
    parser.add_argument("dump", nargs="?", help="path to a .dmp file")
    parser.add_argument("--self-test", action="store_true", help="run the built-in regression checks")
    args = parser.parse_args(argv)

    if args.self_test:
        return self_test()
    if not args.dump:
        parser.error("a dump path is required unless --self-test is given")
    try:
        model = parse_minidump(args.dump)
    except (OSError, ValueError) as error:
        print(f"dump_triage: {error}", file=sys.stderr)
        return 2
    print(triage(model))
    return 0


if __name__ == "__main__":
    sys.exit(main())
