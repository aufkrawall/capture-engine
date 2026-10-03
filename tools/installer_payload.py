"""Pack and verify the Capture Engine setup payload.

The setup executable is a stub PE followed by compressed program files, an
index and a 64-byte footer. This module writes that container and reads it back
with an independent decoder. The byte layout is defined by
installer/payload_format.h, which the native setup parses; keep both in step
(tests/test_installer_policy.cpp pins the C++ side, tools/tests/
test_installer_payload.py the Python side).

Compression is LZMS from the Windows Compression API (cabinet.dll): the same
system component the setup decodes with, so nothing third-party ships in or
runs from the installer. Content is split into 1 MiB blocks; a block that does
not shrink is stored raw, which the decoder recognises by its stored length.
"""

from __future__ import annotations

import concurrent.futures
import ctypes
import os
import struct
import threading
import zlib
from dataclasses import dataclass
from typing import Callable, Iterable, List, Optional, Sequence, Tuple

FOOTER_MAGIC = b"CESETUP1"
FORMAT_VERSION = 1
FOOTER_SIZE = 64
BLOCK_SIZE = 1 << 20
MAX_FILES = 4096
MAX_TOTAL_BYTES = 8 << 30
MAX_PATH_CHARS = 240

METHOD_STORE = 0
METHOD_LZMS = 1

ALGORITHM_LZMS = 5
INFO_CLASS_BLOCK_SIZE = 1

_RESERVED_DEVICE_NAMES = {"con", "prn", "aux", "nul", "conin$", "conout$"} | {
    f"{prefix}{number}" for prefix in ("com", "lpt") for number in range(1, 10)
}


class PayloadError(RuntimeError):
    """The payload cannot be built or does not verify."""


def crc32(data: bytes, value: int = 0) -> int:
    return zlib.crc32(data, value) & 0xFFFFFFFF


def is_safe_relative_path(path: str) -> bool:
    """Same rules as ce::setup::IsSafeRelativePath in installer/payload_format.h."""
    if not path or len(path) > MAX_PATH_CHARS:
        return False
    try:
        path.encode("utf-8")
    except UnicodeEncodeError:
        return False
    for component in path.split("/"):
        if component in ("", ".", "..") or component[-1] in ". ":
            return False
        if any(ord(character) < 0x20 or character in '\\:*?"<>|' for character in component):
            return False
        if component.split(".", 1)[0].lower() in _RESERVED_DEVICE_NAMES:
            return False
    return True


# ---------------------------------------------------------------------------
# Windows Compression API
# ---------------------------------------------------------------------------


class _Cabinet:
    def __init__(self) -> None:
        if os.name != "nt":
            raise PayloadError("the setup payload is packed with the Windows Compression API; run on Windows")
        from ctypes import wintypes

        library = ctypes.WinDLL("cabinet.dll", use_last_error=True)
        handle_pointer = ctypes.POINTER(ctypes.c_void_p)
        size_pointer = ctypes.POINTER(ctypes.c_size_t)
        self.create_compressor = library.CreateCompressor
        self.create_compressor.argtypes = [wintypes.DWORD, ctypes.c_void_p, handle_pointer]
        self.set_information = library.SetCompressorInformation
        self.set_information.argtypes = [ctypes.c_void_p, wintypes.DWORD, ctypes.c_void_p, ctypes.c_size_t]
        self.compress = library.Compress
        self.compress.argtypes = [
            ctypes.c_void_p,
            ctypes.c_char_p,
            ctypes.c_size_t,
            ctypes.c_void_p,
            ctypes.c_size_t,
            size_pointer,
        ]
        self.close_compressor = library.CloseCompressor
        self.close_compressor.argtypes = [ctypes.c_void_p]
        self.create_decompressor = library.CreateDecompressor
        self.create_decompressor.argtypes = [wintypes.DWORD, ctypes.c_void_p, handle_pointer]
        self.decompress = library.Decompress
        self.decompress.argtypes = [
            ctypes.c_void_p,
            ctypes.c_char_p,
            ctypes.c_size_t,
            ctypes.c_void_p,
            ctypes.c_size_t,
            size_pointer,
        ]
        self.close_decompressor = library.CloseDecompressor
        self.close_decompressor.argtypes = [ctypes.c_void_p]


_cabinet: Optional[_Cabinet] = None
_cabinet_lock = threading.Lock()
_local = threading.local()


def _api() -> _Cabinet:
    global _cabinet
    with _cabinet_lock:
        if _cabinet is None:
            _cabinet = _Cabinet()
        return _cabinet


class _Handle:
    """Per-thread compressor/decompressor; the API handles are not thread safe."""

    def __init__(self, compressing: bool) -> None:
        self.api = _api()
        self.handle = ctypes.c_void_p()
        create = self.api.create_compressor if compressing else self.api.create_decompressor
        if not create(ALGORITHM_LZMS, None, ctypes.byref(self.handle)):
            raise PayloadError(f"cannot create the LZMS codec (error {ctypes.get_last_error()})")
        if compressing:
            block = ctypes.c_uint32(BLOCK_SIZE)
            # Best effort: the default block size still produces a valid stream.
            self.api.set_information(self.handle, INFO_CLASS_BLOCK_SIZE, ctypes.byref(block), 4)
        self.compressing = compressing

    def close(self) -> None:
        if self.handle:
            (self.api.close_compressor if self.compressing else self.api.close_decompressor)(self.handle)
            self.handle = ctypes.c_void_p()


def _thread_handle(compressing: bool) -> _Handle:
    attribute = "compressor" if compressing else "decompressor"
    handle = getattr(_local, attribute, None)
    if handle is None:
        handle = _Handle(compressing)
        setattr(_local, attribute, handle)
    return handle


def compress_block(block: bytes) -> Optional[bytes]:
    """LZMS-compress one block; None when it would not get smaller."""
    codec = _thread_handle(True)
    output = ctypes.create_string_buffer(len(block) + 4096)
    written = ctypes.c_size_t()
    if not codec.api.compress(codec.handle, block, len(block), output, len(output), ctypes.byref(written)):
        return None
    if written.value >= len(block):
        return None
    return output.raw[: written.value]


def decompress_block(stored: bytes, raw_size: int) -> bytes:
    codec = _thread_handle(False)
    output = ctypes.create_string_buffer(raw_size)
    written = ctypes.c_size_t()
    if not codec.api.decompress(codec.handle, stored, len(stored), output, raw_size, ctypes.byref(written)):
        raise PayloadError(f"block decode failed (error {ctypes.get_last_error()})")
    if written.value != raw_size:
        raise PayloadError("block decoded to an unexpected size")
    return output.raw[:raw_size]


# ---------------------------------------------------------------------------
# Container
# ---------------------------------------------------------------------------


@dataclass
class Entry:
    path: str
    method: int
    flags: int
    offset: int
    stored_size: int
    size: int
    crc: int


def _encode_file(content: bytes, compress: Callable[[bytes], Optional[bytes]]) -> Tuple[int, bytes]:
    """Return (method, stored bytes) for one file."""
    if not content:
        return METHOD_STORE, b""
    blocks: List[bytes] = []
    total = 0
    for start in range(0, len(content), BLOCK_SIZE):
        block = content[start : start + BLOCK_SIZE]
        packed = compress(block)
        stored = packed if packed is not None and len(packed) < len(block) else block
        blocks.append(struct.pack("<I", len(stored)) + stored)
        total += len(blocks[-1])
    if total >= len(content):
        return METHOD_STORE, content
    return METHOD_LZMS, b"".join(blocks)


def build_payload(
    files: Sequence[Tuple[str, bytes]],
    *,
    base_offset: int,
    compress: Optional[Callable[[bytes], Optional[bytes]]] = None,
    workers: Optional[int] = None,
) -> Tuple[bytes, List[Entry]]:
    """Encode files; returns the data area (to follow the stub) and its entries.

    `base_offset` is the stub size: entry offsets are absolute in the final file.
    """
    if not files:
        raise PayloadError("the setup payload would be empty")
    if len(files) > MAX_FILES:
        raise PayloadError("too many payload files")
    seen = set()
    for path, _ in files:
        if not is_safe_relative_path(path):
            raise PayloadError(f"unsafe payload path: {path}")
        if path.lower() in seen:
            raise PayloadError(f"duplicate payload path: {path}")
        seen.add(path.lower())
    if sum(len(content) for _, content in files) > MAX_TOTAL_BYTES:
        raise PayloadError("the payload exceeds the format limit")
    compressor = compress or compress_block

    def encode(item: Tuple[str, bytes]) -> Tuple[int, bytes]:
        return _encode_file(item[1], compressor)

    with concurrent.futures.ThreadPoolExecutor(max_workers=workers or min(16, os.cpu_count() or 4)) as pool:
        encoded = list(pool.map(encode, files))

    area = bytearray()
    entries: List[Entry] = []
    for (path, content), (method, stored) in zip(files, encoded):
        entries.append(Entry(path, method, 0, base_offset + len(area), len(stored), len(content), crc32(content)))
        area += stored
    return bytes(area), entries


def build_index(entries: Iterable[Entry]) -> bytes:
    index = bytearray()
    for entry in entries:
        encoded = entry.path.encode("utf-8")
        index += struct.pack("<H", len(encoded)) + encoded
        index += struct.pack(
            "<IIQQQII", entry.method, entry.flags, entry.offset, entry.stored_size, entry.size, entry.crc, 0
        )
    return bytes(index)


def build_footer(payload_offset: int, index_offset: int, index: bytes, total_size: int, file_count: int) -> bytes:
    body = FOOTER_MAGIC + struct.pack(
        "<IIQQQQIII",
        FORMAT_VERSION,
        0,
        payload_offset,
        index_offset,
        len(index),
        total_size,
        file_count,
        crc32(index),
        0,
    )
    return body + struct.pack("<I", crc32(body))


def assemble(stub: bytes, files: Sequence[Tuple[str, bytes]], **options) -> bytes:
    """The finished setup file: stub, data area, index, footer."""
    if not stub.startswith(b"MZ"):
        raise PayloadError("the setup stub is not a PE image")
    area, entries = build_payload(files, base_offset=len(stub), **options)
    index = build_index(entries)
    index_offset = len(stub) + len(area)
    total = sum(entry.size for entry in entries)
    footer = build_footer(len(stub), index_offset, index, total, len(entries))
    return stub + area + index + footer


# ---------------------------------------------------------------------------
# Reference decoder
# ---------------------------------------------------------------------------


def parse(data: bytes) -> List[Entry]:
    """Parse footer and index exactly as the setup does; raises PayloadError."""
    if len(data) < FOOTER_SIZE:
        raise PayloadError("file too small for a payload")
    footer = data[-FOOTER_SIZE:]
    if footer[:8] != FOOTER_MAGIC:
        raise PayloadError("no payload footer")
    version, _flags, payload_offset, index_offset, index_size, total, count, index_crc, _reserved = struct.unpack(
        "<IIQQQQIII", footer[8:60]
    )
    if version != FORMAT_VERSION:
        raise PayloadError("unsupported payload version")
    if crc32(footer[:60]) != struct.unpack("<I", footer[60:])[0]:
        raise PayloadError("payload footer checksum mismatch")
    footer_start = len(data) - FOOTER_SIZE
    if not (payload_offset <= index_offset <= footer_start and index_size == footer_start - index_offset):
        raise PayloadError("payload bounds are inconsistent")
    index = data[index_offset:footer_start]
    if crc32(index) != index_crc:
        raise PayloadError("payload index checksum mismatch")
    entries: List[Entry] = []
    cursor = 0
    for _ in range(count):
        (path_length,) = struct.unpack_from("<H", index, cursor)
        cursor += 2
        path = index[cursor : cursor + path_length].decode("utf-8")
        cursor += path_length
        method, flags, offset, stored, size, crc, _pad = struct.unpack_from("<IIQQQII", index, cursor)
        cursor += struct.calcsize("<IIQQQII")
        if not is_safe_relative_path(path):
            raise PayloadError(f"unsafe path in index: {path}")
        if method not in (METHOD_STORE, METHOD_LZMS):
            raise PayloadError("unknown storage method")
        if offset < payload_offset or offset + stored > index_offset:
            raise PayloadError(f"{path}: data range is outside the payload area")
        entries.append(Entry(path, method, flags, offset, stored, size, crc))
    if cursor != len(index):
        raise PayloadError("trailing bytes in the payload index")
    if sum(entry.size for entry in entries) != total:
        raise PayloadError("payload total size mismatch")
    return entries


def read_file(data: bytes, entry: Entry) -> bytes:
    stored = data[entry.offset : entry.offset + entry.stored_size]
    if entry.method == METHOD_STORE:
        content = stored
    else:
        out = bytearray()
        cursor = 0
        remaining = entry.size
        while remaining:
            raw = min(BLOCK_SIZE, remaining)
            (length,) = struct.unpack_from("<I", stored, cursor)
            cursor += 4
            if length == 0 or length > raw or cursor + length > len(stored):
                raise PayloadError(f"{entry.path}: malformed block")
            block = stored[cursor : cursor + length]
            cursor += length
            out += block if length == raw else decompress_block(block, raw)
            remaining -= raw
        if cursor != len(stored):
            raise PayloadError(f"{entry.path}: stored size mismatch")
        content = bytes(out)
    if len(content) != entry.size or crc32(content) != entry.crc:
        raise PayloadError(f"{entry.path}: content checksum mismatch")
    return content


def verify(data: bytes, expected: Optional[Sequence[Tuple[str, bytes]]] = None) -> List[Entry]:
    """Decode every file; with `expected`, also require exact content equality."""
    entries = parse(data)
    contents = {entry.path: read_file(data, entry) for entry in entries}
    if expected is not None:
        if sorted(contents) != sorted(path for path, _ in expected):
            raise PayloadError("payload file set differs from the staged tree")
        for path, content in expected:
            if contents[path] != content:
                raise PayloadError(f"{path}: payload content differs from the staged file")
    return entries
