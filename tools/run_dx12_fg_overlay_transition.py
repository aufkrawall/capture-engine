#!/usr/bin/env python3

"""Exercise and pixel-probe the DX12 FSR-FG -> DLSS-FG overlay transition."""

import argparse
import ctypes
import csv
import statistics
import struct
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = SCRIPT_DIR.parent
CAPTURE_BIN = PROJECT_ROOT / "installed" / "captureengine"
TESTAPP_BIN = PROJECT_ROOT / "installed" / "testapp"
CAPTURE_EXE = CAPTURE_BIN / "captureengine.exe"
TESTAPP_EXE = TESTAPP_BIN / "dx12_fg_switch_test.exe"
TESTAPP_LOG = TESTAPP_BIN / "dx12_fg_switch_test.log"
OUTPUT_ROOT = PROJECT_ROOT / "build" / "fg_overlay_transition_probe"

PROCESS_NAMES = ("captureengine.exe", "dx12_fg_switch_test.exe")
CAPTURE_WIDTH = 280
CAPTURE_HEIGHT = 280
ROI_LEFT = 8
ROI_TOP = 82
ROI_RIGHT = 232
ROI_BOTTOM = 246
ROI_STRIDE = 3

SRCCOPY = 0x00CC0020
CAPTUREBLT = 0x40000000
BI_RGB = 0
DIB_RGB_COLORS = 0


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [
        ("biSize", ctypes.c_uint32),
        ("biWidth", ctypes.c_int32),
        ("biHeight", ctypes.c_int32),
        ("biPlanes", ctypes.c_uint16),
        ("biBitCount", ctypes.c_uint16),
        ("biCompression", ctypes.c_uint32),
        ("biSizeImage", ctypes.c_uint32),
        ("biXPelsPerMeter", ctypes.c_int32),
        ("biYPelsPerMeter", ctypes.c_int32),
        ("biClrUsed", ctypes.c_uint32),
        ("biClrImportant", ctypes.c_uint32),
    ]


class BITMAPINFO(ctypes.Structure):
    _fields_ = [("bmiHeader", BITMAPINFOHEADER), ("bmiColors", ctypes.c_uint32 * 3)]


@dataclass
class Sample:
    elapsed_ms: float
    phase: str
    dark_ratio: float
    mean_luma: float
    edge_ratio: float


class DesktopProbe:
    def __init__(self, width, height):
        self.width = width
        self.height = height
        self.user32 = ctypes.WinDLL("user32", use_last_error=True)
        self.gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)

        self.user32.GetDC.argtypes = [ctypes.c_void_p]
        self.user32.GetDC.restype = ctypes.c_void_p
        self.user32.ReleaseDC.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
        self.gdi32.CreateCompatibleDC.argtypes = [ctypes.c_void_p]
        self.gdi32.CreateCompatibleDC.restype = ctypes.c_void_p
        self.gdi32.CreateDIBSection.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(BITMAPINFO),
            ctypes.c_uint,
            ctypes.POINTER(ctypes.c_void_p),
            ctypes.c_void_p,
            ctypes.c_uint,
        ]
        self.gdi32.CreateDIBSection.restype = ctypes.c_void_p
        self.gdi32.SelectObject.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
        self.gdi32.SelectObject.restype = ctypes.c_void_p
        self.gdi32.BitBlt.argtypes = [
            ctypes.c_void_p,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_void_p,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_uint32,
        ]
        self.gdi32.DeleteObject.argtypes = [ctypes.c_void_p]
        self.gdi32.DeleteDC.argtypes = [ctypes.c_void_p]

        self.screen_dc = self.user32.GetDC(None)
        self.memory_dc = self.gdi32.CreateCompatibleDC(self.screen_dc)
        info = BITMAPINFO()
        info.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
        info.bmiHeader.biWidth = width
        info.bmiHeader.biHeight = -height
        info.bmiHeader.biPlanes = 1
        info.bmiHeader.biBitCount = 32
        info.bmiHeader.biCompression = BI_RGB
        self.bits = ctypes.c_void_p()
        self.bitmap = self.gdi32.CreateDIBSection(
            self.screen_dc, ctypes.byref(info), DIB_RGB_COLORS, ctypes.byref(self.bits), None, 0
        )
        if not self.screen_dc or not self.memory_dc or not self.bitmap or not self.bits.value:
            self.close()
            raise OSError(ctypes.get_last_error(), "failed to initialize the GDI desktop probe")
        self.old_bitmap = self.gdi32.SelectObject(self.memory_dc, self.bitmap)

    def capture(self):
        ok = self.gdi32.BitBlt(
            self.memory_dc,
            0,
            0,
            self.width,
            self.height,
            self.screen_dc,
            0,
            0,
            SRCCOPY | CAPTUREBLT,
        )
        if not ok:
            raise OSError(ctypes.get_last_error(), "BitBlt failed")
        return ctypes.string_at(self.bits.value, self.width * self.height * 4)

    def close(self):
        if getattr(self, "memory_dc", None) and getattr(self, "old_bitmap", None):
            self.gdi32.SelectObject(self.memory_dc, self.old_bitmap)
        if getattr(self, "bitmap", None):
            self.gdi32.DeleteObject(self.bitmap)
            self.bitmap = None
        if getattr(self, "memory_dc", None):
            self.gdi32.DeleteDC(self.memory_dc)
            self.memory_dc = None
        if getattr(self, "screen_dc", None):
            self.user32.ReleaseDC(None, self.screen_dc)
            self.screen_dc = None

    def __enter__(self):
        return self

    def __exit__(self, _exc_type, _exc_value, _traceback):
        self.close()


def fail(message):
    print(f"error: {message}", file=sys.stderr)
    raise SystemExit(1)


def taskkill_processes(force=True):
    args = ["taskkill"]
    if force:
        args.append("/F")
    for process_name in PROCESS_NAMES:
        subprocess.run(
            args + ["/T", "/IM", process_name],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )


def is_process_running(process_name):
    result = subprocess.run(
        ["tasklist", "/FI", f"IMAGENAME eq {process_name}", "/NH"],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )
    return process_name.lower() in result.stdout.lower()


def ensure_no_lingering_processes():
    taskkill_processes(force=True)
    deadline = time.monotonic() + 2.0
    while time.monotonic() < deadline:
        if not any(is_process_running(name) for name in PROCESS_NAMES):
            return
        time.sleep(0.05)
    lingering = [name for name in PROCESS_NAMES if is_process_running(name)]
    if lingering:
        fail(f"could not stop lingering processes: {', '.join(lingering)}")


def close_process(process, process_name):
    if process and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=1.0)
        except subprocess.TimeoutExpired:
            subprocess.run(
                ["taskkill", "/F", "/T", "/PID", str(process.pid)],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                check=False,
            )
    deadline = time.monotonic() + 1.0
    while time.monotonic() < deadline and is_process_running(process_name):
        time.sleep(0.05)


def overlay_metric(frame):
    dark = 0
    edges = 0
    luma_sum = 0.0
    count = 0
    for y in range(ROI_TOP, ROI_BOTTOM, ROI_STRIDE):
        row = y * CAPTURE_WIDTH * 4
        for x in range(ROI_LEFT, ROI_RIGHT, ROI_STRIDE):
            offset = row + x * 4
            blue = frame[offset]
            green = frame[offset + 1]
            red = frame[offset + 2]
            if max(red, green, blue) < 48:
                dark += 1
            if y >= 118:
                neighbor = offset + ROI_STRIDE * 4
                edge_delta = (
                    abs(red - frame[neighbor + 2])
                    + abs(green - frame[neighbor + 1])
                    + abs(blue - frame[neighbor])
                )
                if edge_delta >= 36:
                    edges += 1
            luma_sum += red * 0.2126 + green * 0.7152 + blue * 0.0722
            count += 1
    edge_count = len(range(118, ROI_BOTTOM, ROI_STRIDE)) * len(range(ROI_LEFT, ROI_RIGHT, ROI_STRIDE))
    return dark / count, luma_sum / count, edges / edge_count


def write_bmp(path, bgra, width, height):
    pixel_bytes = width * height * 4
    file_header_size = 14
    dib_header_size = 40
    pixel_offset = file_header_size + dib_header_size
    with path.open("wb") as output:
        output.write(struct.pack("<2sIHHI", b"BM", pixel_offset + pixel_bytes, 0, 0, pixel_offset))
        output.write(struct.pack("<IiiHHIIiiII", dib_header_size, width, -height, 1, 32, BI_RGB, pixel_bytes, 0, 0, 0, 0))
        output.write(bgra)


def read_new_log_text(offset):
    try:
        size = TESTAPP_LOG.stat().st_size
        if size < offset:
            offset = 0
        with TESTAPP_LOG.open("rb") as log_file:
            log_file.seek(offset)
            data = log_file.read()
        return offset + len(data), data.decode("utf-8", errors="replace")
    except OSError:
        return offset, ""


def newest_capture_session(since_ns):
    candidates = []
    logs_root = CAPTURE_BIN / "logs"
    try:
        for path in logs_root.iterdir():
            if path.is_dir() and path.stat().st_mtime_ns >= since_ns:
                candidates.append(path)
    except OSError:
        return None
    return max(candidates, key=lambda path: path.stat().st_mtime_ns, default=None)


def phase_summary(samples, phase):
    phase_samples = [sample for sample in samples if sample.phase == phase]
    if not phase_samples:
        return None
    values = [sample.dark_ratio for sample in phase_samples]
    edge_values = [sample.edge_ratio for sample in phase_samples]
    return {
        "count": len(values),
        "minimum": min(values),
        "median": statistics.median(values),
        "maximum": max(values),
        "edge_minimum": min(edge_values),
        "edge_median": statistics.median(edge_values),
        "edge_maximum": max(edge_values),
    }


def run_probe(args):
    if not CAPTURE_EXE.exists():
        fail(f"captureengine.exe not found: {CAPTURE_EXE}")
    if not TESTAPP_EXE.exists():
        fail(f"dx12_fg_switch_test.exe not found: {TESTAPP_EXE}")

    ensure_no_lingering_processes()
    try:
        TESTAPP_LOG.unlink()
    except FileNotFoundError:
        pass
    OUTPUT_ROOT.mkdir(parents=True, exist_ok=True)
    run_dir = OUTPUT_ROOT / time.strftime("%Y%m%d_%H%M%S")
    run_dir.mkdir()
    start_wall_ns = time.time_ns()
    start = time.monotonic()
    samples = []
    events = {}
    phase = "startup"
    log_offset = 0
    minimum_frame = None
    minimum_dark_ratio = 1.0
    minimum_edge_ratio = 1.0
    phase_minimum_frames = {}
    phase_maximum_frames = {}
    phase_extremes = {}
    ce_process = None
    app_process = None

    try:
        ce_process = subprocess.Popen(
            [str(CAPTURE_EXE)], cwd=str(CAPTURE_BIN), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        )
        time.sleep(args.ce_lead_ms / 1000.0)
        app_process = subprocess.Popen(
            [
                str(TESTAPP_EXE),
                f"--duration={args.duration_seconds}",
                f"--auto-fsr-start={args.fsr_start_seconds}",
                f"--auto-dlss-start={args.dlss_start_seconds}",
                "--auto-return-fsr=3600",
                "--no-dlss-suspend-stress",
                "--no-dlss-off-stress",
            ],
            cwd=str(TESTAPP_BIN),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

        with DesktopProbe(CAPTURE_WIDTH, CAPTURE_HEIGHT) as probe:
            deadline = start + args.timeout_seconds
            while time.monotonic() < deadline and app_process.poll() is None:
                now = time.monotonic()
                log_offset, new_log = read_new_log_text(log_offset)
                if "Auto sequence clock reset" in new_log:
                    events.setdefault("ready", now)
                    phase = "off"
                if "Mode now FSR FG" in new_log:
                    events.setdefault("fsr", now)
                    phase = "fsr"
                if "Mode request: FSR FG -> DLSS FG" in new_log:
                    events.setdefault("transition", now)
                    phase = "transition"
                if "DLSS replacement passthrough Present completed" in new_log:
                    events.setdefault("replacement_present", now)
                if "Mode now DLSS FG" in new_log:
                    events.setdefault("dlss", now)
                    phase = "dlss"

                frame = probe.capture()
                dark_ratio, mean_luma, edge_ratio = overlay_metric(frame)
                samples.append(Sample((now - start) * 1000.0, phase, dark_ratio, mean_luma, edge_ratio))
                if phase in ("fsr", "transition", "dlss") and dark_ratio < minimum_dark_ratio:
                    minimum_dark_ratio = dark_ratio
                    minimum_frame = frame
                phase_minimum, phase_maximum = phase_extremes.get(phase, (2.0, -1.0))
                if edge_ratio < phase_minimum:
                    phase_minimum = edge_ratio
                    phase_minimum_frames[phase] = frame
                if edge_ratio > phase_maximum:
                    phase_maximum = edge_ratio
                    phase_maximum_frames[phase] = frame
                phase_extremes[phase] = (phase_minimum, phase_maximum)
                minimum_edge_ratio = min(minimum_edge_ratio, edge_ratio)
                time.sleep(args.sample_interval_ms / 1000.0)

        if app_process.poll() is None:
            fail(f"test app exceeded {args.timeout_seconds:.1f}s probe timeout")
    finally:
        close_process(app_process, "dx12_fg_switch_test.exe")
        close_process(ce_process, "captureengine.exe")
        ensure_no_lingering_processes()

    if minimum_frame:
        write_bmp(run_dir / "minimum_overlay_metric.bmp", minimum_frame, CAPTURE_WIDTH, CAPTURE_HEIGHT)
    for phase_name, frame in phase_minimum_frames.items():
        write_bmp(run_dir / f"{phase_name}_minimum_edge.bmp", frame, CAPTURE_WIDTH, CAPTURE_HEIGHT)
    for phase_name, frame in phase_maximum_frames.items():
        write_bmp(run_dir / f"{phase_name}_maximum_edge.bmp", frame, CAPTURE_WIDTH, CAPTURE_HEIGHT)
    with (run_dir / "samples.csv").open("w", newline="", encoding="utf-8") as output:
        writer = csv.writer(output)
        writer.writerow(("elapsed_ms", "phase", "dark_ratio", "mean_luma", "edge_ratio"))
        for sample in samples:
            writer.writerow(
                (
                    f"{sample.elapsed_ms:.3f}",
                    sample.phase,
                    f"{sample.dark_ratio:.6f}",
                    f"{sample.mean_luma:.3f}",
                    f"{sample.edge_ratio:.6f}",
                )
            )

    summaries = {phase_name: phase_summary(samples, phase_name) for phase_name in ("off", "fsr", "transition", "dlss")}
    baseline_medians = [
        summary["edge_median"]
        for phase_name, summary in summaries.items()
        if phase_name in ("fsr", "dlss") and summary
    ]
    baseline = min(baseline_medians, default=0.0)
    transition_start_ms = (events["transition"] - start) * 1000.0 if "transition" in events else 0.0
    dlss_start_ms = (events["dlss"] - start) * 1000.0 if "dlss" in events else samples[-1].elapsed_ms
    seam_end_ms = dlss_start_ms + args.dlss_seam_ms
    seam_samples = [
        sample for sample in samples if transition_start_ms <= sample.elapsed_ms <= seam_end_ms
    ]
    seam_minimum = min((sample.edge_ratio for sample in seam_samples), default=minimum_edge_ratio)
    loss_ratio = seam_minimum / baseline if baseline > 0.0 else 0.0
    visible = baseline >= args.minimum_baseline_edge_ratio
    passed = visible and loss_ratio >= args.minimum_retained_ratio and "fsr" in events and "dlss" in events

    print(f"probe duration: {(time.monotonic() - start):.3f}s; samples: {len(samples)}")
    for phase_name, summary in summaries.items():
        if summary:
            print(
                f"{phase_name:10s} n={summary['count']:4d} dark-ratio "
                f"min/median/max={summary['minimum']:.4f}/{summary['median']:.4f}/{summary['maximum']:.4f}; "
                f"edge min/median/max={summary['edge_minimum']:.4f}/{summary['edge_median']:.4f}/"
                f"{summary['edge_maximum']:.4f}"
            )
    print(
        f"transition/DLSS seam retained overlay-edge ratio: {loss_ratio:.3f} "
        f"(required {args.minimum_retained_ratio:.3f}; window through {args.dlss_seam_ms:.0f}ms after DLSS)"
    )
    print(f"result: {'PASS' if passed else 'FAIL'}")
    print(f"probe artifacts: {run_dir}")
    session = newest_capture_session(start_wall_ns)
    if session:
        print(f"captureengine session: {session}")
    return 0 if passed else 1


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--timeout-seconds", type=float, default=13.0)
    parser.add_argument("--duration-seconds", type=int, default=7)
    parser.add_argument("--ce-lead-ms", type=float, default=350.0)
    parser.add_argument("--fsr-start-seconds", type=int, default=1)
    parser.add_argument("--dlss-start-seconds", type=int, default=3)
    parser.add_argument("--sample-interval-ms", type=float, default=1.0)
    parser.add_argument("--minimum-baseline-edge-ratio", type=float, default=0.005)
    parser.add_argument("--minimum-retained-ratio", type=float, default=0.50)
    parser.add_argument("--dlss-seam-ms", type=float, default=750.0)
    args = parser.parse_args()
    if (
        args.timeout_seconds <= 0
        or args.duration_seconds <= 0
        or args.sample_interval_ms < 0
        or args.dlss_seam_ms < 0
    ):
        parser.error("timeouts, duration, and sample interval must be positive")
    return args


def main():
    try:
        ctypes.windll.user32.SetProcessDPIAware()
    except (AttributeError, OSError):
        pass
    return run_probe(parse_args())


if __name__ == "__main__":
    raise SystemExit(main())
