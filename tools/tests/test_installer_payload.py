import os
import random
import struct
import unittest

from tools import installer_payload as payload

STUB = b"MZ" + b"\x00" * 126


def _files():
    rng = random.Random(7)
    compressible = (b"capture engine " * 200000)[: payload.BLOCK_SIZE * 2 + 12345]
    incompressible = bytes(rng.getrandbits(8) for _ in range(300000))
    return [
        ("captureengine.exe", compressible),
        ("config.ini", b"[Output]\r\noutput_dir=\r\n"),
        ("empty.bin", b""),
        ("ffmpeg/avcodec-63.dll", incompressible),
        ("plugins/LibreHardwareMonitor/README.txt", b"notes\n" * 100),
    ]


class PayloadLayoutTest(unittest.TestCase):
    """Byte offsets that installer/payload_format.h reads; changing one breaks setup."""

    def test_footer_layout_matches_the_native_parser(self):
        data = payload.assemble(STUB, [("a.txt", b"hello")], compress=lambda block: None)
        footer = data[-payload.FOOTER_SIZE :]
        self.assertEqual(len(footer), 64)
        self.assertEqual(footer[:8], b"CESETUP1")
        self.assertEqual(struct.unpack_from("<I", footer, 8)[0], 1)  # version
        self.assertEqual(struct.unpack_from("<I", footer, 12)[0], 0)  # flags
        self.assertEqual(struct.unpack_from("<Q", footer, 16)[0], len(STUB))  # payloadOffset
        index_offset = struct.unpack_from("<Q", footer, 24)[0]
        index_size = struct.unpack_from("<Q", footer, 32)[0]
        self.assertEqual(index_offset + index_size, len(data) - 64)
        self.assertEqual(struct.unpack_from("<Q", footer, 40)[0], 5)  # totalSize
        self.assertEqual(struct.unpack_from("<I", footer, 48)[0], 1)  # fileCount
        self.assertEqual(struct.unpack_from("<I", footer, 52)[0], payload.crc32(data[index_offset:-64]))
        self.assertEqual(struct.unpack_from("<I", footer, 60)[0], payload.crc32(footer[:60]))

    def test_index_entry_layout_matches_the_native_parser(self):
        data = payload.assemble(STUB, [("dir/a.txt", b"hello")], compress=lambda block: None)
        index_offset = struct.unpack_from("<Q", data, len(data) - 64 + 24)[0]
        index = data[index_offset:-64]
        (path_length,) = struct.unpack_from("<H", index, 0)
        self.assertEqual(index[2 : 2 + path_length], b"dir/a.txt")
        fixed = struct.unpack_from("<IIQQQII", index, 2 + path_length)
        method, flags, offset, stored, size, crc, reserved = fixed
        self.assertEqual((method, flags, offset, stored, size, reserved), (0, 0, len(STUB), 5, 5, 0))
        self.assertEqual(crc, payload.crc32(b"hello"))
        self.assertEqual(len(index), 2 + path_length + 40)

    def test_crc32_matches_the_zlib_check_value(self):
        self.assertEqual(payload.crc32(b"123456789"), 0xCBF43926)


class PathPolicyTest(unittest.TestCase):
    def test_accepts_ordinary_relative_paths(self):
        for path in ("captureengine.exe", "ffmpeg/avcodec-63.dll", "plugins/LibreHardwareMonitor/PawnIO_setup.exe"):
            self.assertTrue(payload.is_safe_relative_path(path), path)

    def test_rejects_anything_that_could_leave_the_installation_folder(self):
        for path in (
            "",
            "/etc/passwd",
            "..",
            "a/../b",
            "a//b",
            "./a",
            "a\\b",
            "C:/x",
            "a:b",
            "nul",
            "NUL.txt",
            "dir/com1.log",
            "trailing.",
            "trailing ",
            "a/b/",
            "x" * 241,
            "bad*name",
            "tab\tname",
        ):
            self.assertFalse(payload.is_safe_relative_path(path), repr(path))

    def test_builder_refuses_unsafe_and_duplicate_paths(self):
        with self.assertRaises(payload.PayloadError):
            payload.assemble(STUB, [("../evil.dll", b"x")], compress=lambda block: None)
        with self.assertRaises(payload.PayloadError):
            payload.assemble(STUB, [("a.txt", b"x"), ("A.TXT", b"y")], compress=lambda block: None)
        with self.assertRaises(payload.PayloadError):
            payload.assemble(b"not a pe", [("a.txt", b"x")], compress=lambda block: None)


class StoredRoundTripTest(unittest.TestCase):
    """Framing without the Windows codec, so it runs on every host."""

    def test_round_trip_with_stored_blocks(self):
        files = _files()
        data = payload.assemble(STUB, files, compress=lambda block: None)
        entries = payload.verify(data, files)
        self.assertEqual({entry.path for entry in entries}, {path for path, _ in files})
        self.assertTrue(all(entry.method == payload.METHOD_STORE for entry in entries))

    def test_corrupt_content_is_detected(self):
        data = bytearray(payload.assemble(STUB, [("a.bin", b"A" * 1000)], compress=lambda block: None))
        data[len(STUB) + 10] ^= 0xFF
        with self.assertRaises(payload.PayloadError):
            payload.verify(bytes(data))

    def test_damaged_index_footer_and_truncation_are_detected(self):
        good = payload.assemble(STUB, [("a.bin", b"A" * 1000)], compress=lambda block: None)
        index_offset = struct.unpack_from("<Q", good, len(good) - 64 + 24)[0]
        damaged_index = bytearray(good)
        damaged_index[index_offset + 3] ^= 0x01
        damaged_footer = bytearray(good)
        damaged_footer[-30] ^= 0x01
        for bad in (bytes(damaged_index), bytes(damaged_footer), good[:-1], good[: len(STUB)], b""):
            with self.assertRaises(payload.PayloadError):
                payload.verify(bad)

    def test_index_entry_pointing_outside_the_data_area_is_rejected(self):
        good = bytearray(payload.assemble(STUB, [("a.bin", b"A" * 1000)], compress=lambda block: None))
        index_offset = struct.unpack_from("<Q", good, len(good) - 64 + 24)[0]
        (path_length,) = struct.unpack_from("<H", good, index_offset)
        offset_field = index_offset + 2 + path_length + 8
        struct.pack_into("<Q", good, offset_field, 10**9)
        # Recompute the checksums so only the range check can reject it.
        index = bytes(good[index_offset:-64])
        struct.pack_into("<I", good, len(good) - 64 + 52, payload.crc32(index))
        struct.pack_into("<I", good, len(good) - 4, payload.crc32(bytes(good[-64:-4])))
        with self.assertRaises(payload.PayloadError):
            payload.parse(bytes(good))


@unittest.skipUnless(os.name == "nt", "LZMS comes from the Windows Compression API")
class CompressedRoundTripTest(unittest.TestCase):
    def test_round_trip_through_the_windows_codec(self):
        files = _files()
        data = payload.assemble(STUB, files)
        entries = {entry.path: entry for entry in payload.verify(data, files)}
        self.assertEqual(entries["captureengine.exe"].method, payload.METHOD_LZMS)
        self.assertLess(entries["captureengine.exe"].stored_size, entries["captureengine.exe"].size // 4)
        # Random data cannot shrink: it is stored, never inflated.
        self.assertEqual(entries["ffmpeg/avcodec-63.dll"].method, payload.METHOD_STORE)
        self.assertEqual(entries["empty.bin"].stored_size, 0)

    def test_multi_block_file_decodes_block_by_block(self):
        content = (b"0123456789abcdef" * 4096) * 100 + b"tail"  # 6.5 MiB: seven blocks
        data = payload.assemble(STUB, [("big.bin", content)])
        (entry,) = payload.verify(data, [("big.bin", content)])
        self.assertEqual(entry.method, payload.METHOD_LZMS)
        self.assertGreater(entry.size, payload.BLOCK_SIZE)

    def test_mixed_blocks_store_incompressible_ones_raw(self):
        rng = random.Random(11)
        random_block = bytes(rng.getrandbits(8) for _ in range(payload.BLOCK_SIZE))
        content = random_block + b"x" * payload.BLOCK_SIZE
        data = payload.assemble(STUB, [("mixed.bin", content)])
        payload.verify(data, [("mixed.bin", content)])


if __name__ == "__main__":
    unittest.main()
