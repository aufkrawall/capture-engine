import importlib.util
import unittest
from pathlib import Path

DIGEST_PATH = Path(__file__).resolve().parents[1] / "tracing" / "etl_stall_digest.py"
spec = importlib.util.spec_from_file_location("etl_stall_digest", DIGEST_PATH)
assert spec and spec.loader
digest = importlib.util.module_from_spec(spec)
spec.loader.exec_module(digest)

FREQ = 10_000_000  # QPC ticks per second


def qpc(us):
    return us * FREQ // 1_000_000


class EtlStallDigestTest(unittest.TestCase):
    def test_the_stage_that_stopped_shows_its_silence_in_qpc_microseconds(self):
        events = []
        # The game presents throughout; the kernel's flips stop between 2 s and 40 s.
        for ms in range(0, 45_000, 10):
            events.append((qpc(ms * 1000), "Microsoft-Windows-DXGI", "Present_Start", 6612))
            if ms < 2_000 or ms >= 40_000:
                events.append((qpc(ms * 1000), "Microsoft-Windows-DxgKrnl", "MMIOFlip", 6612))
        result = digest.summarize(events, FREQ)
        flips = result.streams[("Microsoft-Windows-DxgKrnl", "MMIOFlip")]
        presents = result.streams[("Microsoft-Windows-DXGI", "Present_Start")]
        self.assertEqual(digest.to_us(flips.gap_start, FREQ), 1_990_000)
        self.assertEqual(digest.to_us(flips.gap_end, FREQ), 40_000_000)
        self.assertEqual(digest.to_us(presents.gap, FREQ), 10_000)
        text = digest.format_digest(result, 1)
        self.assertIn("Microsoft-Windows-DxgKrnl / MMIOFlip", text)
        self.assertIn("gap=38010ms [1990000..40000000]", text)

    def test_process_and_window_filters(self):
        events = [(qpc(us), "P", "E", pid) for us, pid in ((100, 1), (200, 2), (300, 1), (5_000, 1))]
        by_pid = digest.summarize(events, FREQ, by_pid=True, pids={1}, window=(0, 1_000))
        self.assertEqual(list(by_pid.streams), [("P", "E", 1)])
        self.assertEqual(by_pid.streams[("P", "E", 1)].count, 2)
        self.assertEqual(by_pid.events, 2)


if __name__ == "__main__":
    unittest.main()
