import importlib.util
import struct
import sys
import unittest
from pathlib import Path

TRACING = Path(__file__).resolve().parents[1] / "tracing"


def load(name):
    spec = importlib.util.spec_from_file_location(name, TRACING / f"{name}.py")
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


etl_payload = load("etl_payload")
flood = load("dwm_flood_digest")
repro = load("run_flood_repro")

FREQ = 10_000_000  # QPC ticks per second


def event_info(properties):
    """A TRACE_EVENT_INFO buffer with the given (flags, name, in_type, count, length) top-level properties."""
    header = bytearray(etl_payload.PROPERTY_ARRAY_OFFSET + etl_payload.PROPERTY_INFO_SIZE * len(properties))
    struct.pack_into("<I", header, etl_payload.TOP_LEVEL_COUNT_OFFSET, len(properties))
    names = bytearray()
    for index, (flags, name, in_type, count, length) in enumerate(properties):
        offset = len(header) + len(names)
        names += name.encode("utf-16-le") + b"\0\0"
        base = etl_payload.PROPERTY_ARRAY_OFFSET + index * etl_payload.PROPERTY_INFO_SIZE
        struct.pack_into("<IIHHIHH", header, base, flags, offset, in_type, 0, 0, count, length)
    return bytes(header + names)


class PayloadDecodingTest(unittest.TestCase):
    def test_top_level_properties_decode_to_name_value_text(self):
        properties = etl_payload.parse_event_info(event_info([
            (0, "channelHandle", 8, 1, 4),
            (0, "hWnd", etl_payload.IN_TYPE_POINTER, 1, 8),
            (0, "name", etl_payload.IN_TYPE_UNICODE_STRING, 1, 0),
            (0, "count", 4, 1, 1),
            (etl_payload.PROPERTY_PARAM_COUNT, "items", 6, 3, 2),  # count taken from property 3
            (etl_payload.PROPERTY_STRUCT, "nested", 0, 1, 0),
        ]))
        self.assertEqual([p.name for p in properties], ["channelHandle", "hWnd", "name", "count", "items", "nested"])
        data = (struct.pack("<IQ", 2, 0x2608) + "dwm".encode("utf-16-le") + b"\0\0" + struct.pack("<B", 3)
                + struct.pack("<HHH", 7, 8, 9))
        text = etl_payload.decode_properties(properties, data, 8)
        self.assertEqual(text, "channelHandle=2 hWnd=0x2608 name='dwm' count=3 items=7,8,9 nested=<struct>")
        self.assertEqual(etl_payload.fields(text)["channelHandle"], "2")

    def test_a_truncated_payload_stops_the_decode_instead_of_inventing_values(self):
        properties = etl_payload.parse_event_info(event_info([(0, "a", 8, 1, 4), (0, "b", 10, 1, 8)]))
        self.assertEqual(etl_payload.decode_properties(properties, struct.pack("<I", 5), 8), "a=5 b=? ...")


def gta_like_digest():
    """The 2026-11-13 freeze in miniature: dwm (pid 1732) floods channel 2 from -3 s to +2 s around the game's
    (pid 28620) SwapChainBind at t=10 s; the new surface is realized when the flood ends."""
    digest = flood.FloodDigest(perf_freq=FREQ)

    def at(seconds):
        return int(seconds * FREQ)

    def add(seconds, pid, name, payload=""):
        digest.add(at(seconds), pid, 1, name, lambda: payload)

    add(0.0, 1732, "CompSurfInfo_ForceUpdateRealization/Start", "surfaceLuid=111 bindId=2")
    for tenth in range(0, 160):
        t = tenth / 10
        batches = 4500 if 7.0 <= t < 12.0 else 6
        add(t, 1732, "CHANNELBATCHES_PROCESSED/Info",
            f"channelHandle=2 processId=1732 numberOfBatchesProcessed={batches}")
        add(t, 1732, "CHANNELBATCHES_PROCESSED/Info", "channelHandle=3 processId=1732 numberOfBatchesProcessed=1")
    add(9.8, 28620, "SwapChainBind/Info", "hWnd=4522456")
    add(11.0, 1732, "CompSurfInfo_ForceUpdateRealization/Start", "surfaceLuid=111 bindId=2")
    add(12.0, 1732, "CompSurfInfo_ForceUpdateRealization/Start", "surfaceLuid=222 bindId=2")
    add(12.05, 28620, "IndependentFlip/Info", "SubmitSequence=1")
    return digest


class FloodDigestTest(unittest.TestCase):
    def test_flood_window_and_new_surface_realization_relative_to_the_switch(self):
        digest = gta_like_digest()
        self.assertEqual(flood.busiest_process(digest), "1732")
        buckets = flood.channel_buckets(digest, "1732")
        [(start, end, total, peak)] = flood.floods(buckets)
        self.assertEqual((start, end, total, peak), (70, 119, 50 * 4500, 4500))
        ref = flood.reference_qpc(digest, pid=28620)
        self.assertEqual(ref, int(9.8 * FREQ))
        self.assertEqual(flood.new_surface_realization(digest, ref), (12 * FREQ, "222"))
        self.assertEqual(flood.first_independent_flip(digest, 28620, ref), int(12.05 * FREQ))
        self.assertTrue(flood.busiest_threads(digest, start, end).startswith("p1732/t1=20"))
        text = flood.report(digest, pid=28620, from_s=-1, to_s=3)
        self.assertIn("flood -2.8s..+2.2s: 225000 batches, mean 45000/s, peak 45000/s", text)
        self.assertIn("new surface realized: +2.200s (surfaceLuid=222)", text)
        self.assertIn("first independent flip of p28620: +2.250s", text)
        self.assertIn("bind(p28620)", text)
        self.assertNotIn("realize(111)", text)  # the old surface is not news

    def test_given_switch_time_overrides_the_bind_and_json_round_trips(self):
        digest = gta_like_digest()
        self.assertEqual(flood.reference_qpc(digest, switch_qpc_us=5_000_000), 5 * FREQ)
        again = flood.FloodDigest.from_json(digest.to_json())
        self.assertEqual(flood.floods(flood.channel_buckets(again, "1732")),
                         flood.floods(flood.channel_buckets(digest, "1732")))
        self.assertEqual(flood.new_surface_realization(again, int(9.8 * FREQ)), (12 * FREQ, "222"))


REPRO_OUTPUT = """repro pid=27296 mode=clip bits=0x1 size=3840x2160 freq=10000000 origin=70000000 rate=56000
event spin_start qpc=70000000 foreground=1
event switch qpc=98000000 created_qpc=98000100 foreground=1
event spin_stop qpc=98000200
event end qpc=125000000 aborted=0
presents A=1655 B=225 failures=0 firstStatsB qpc=0 (+-1.0 ms after switch, presentCount=0) lastStatsHr=0x00000000
spin cycles=10 first=1 last=2 clip_failures=0(err=0)
spin per100ms: 5600 5600 5600
"""


class ReproHarnessTest(unittest.TestCase):
    def test_repro_output_parses_into_header_events_and_spin_buckets(self):
        parsed = repro.parse_repro_output(REPRO_OUTPUT)
        self.assertEqual((parsed["pid"], parsed["freq"], parsed["origin"]), (27296, FREQ, 70_000_000))
        self.assertEqual(parsed["events"]["switch"], 98_000_000)
        self.assertEqual(parsed["spin"], [5600, 5600, 5600])
        with self.assertRaises(ValueError):
            repro.parse_repro_output("nothing")

    def test_summary_names_onset_spin_stop_and_flood_end_against_the_switch(self):
        lines = repro.summarize_run(repro.parse_repro_output(REPRO_OUTPUT), gta_like_digest(), 1732)
        self.assertIn("spin_start", "\n".join(lines))
        self.assertEqual(lines[-1], "  SUMMARY new surface realized +2200 ms, spin stopped +0 ms, compositor flood "
                                    "ends +2200 ms; 225660 channel-2 batches for 16800 spinner cycles")


if __name__ == "__main__":
    unittest.main()
