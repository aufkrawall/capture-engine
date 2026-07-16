#!/usr/bin/env python3

"""Exercise and pixel-probe a DX12 FG overlay transition."""

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
BRIGHT_ROI_TOP = 115
BRIGHT_ROI_BOTTOM = 241

SRCCOPY = 0x00CC0020
CAPTUREBLT = 0x40000000
BI_RGB = 0
DIB_RGB_COLORS = 0
WM_KEYDOWN = 0x0100
WM_KEYUP = 0x0101
VK_OFF = ord("1")
VK_DLSS = ord("2")
VK_FSR = ord("3")

EXTENDED_ABA_STEPS = (
    ("cycle_fsr_1", VK_FSR, "Mode now FSR FG", "cycle-fsr-1", 1.25),
    ("cycle_off_1", VK_OFF, "Mode now OFF", "cycle-off-1", 0.60),
    ("cycle_dlss_1", VK_DLSS, "Mode now DLSS FG", "cycle-dlss-1", 0.15),
    ("cycle_off_2", VK_OFF, "Mode now OFF", "cycle-off-2", 0.35),
    ("cycle_fsr_2", VK_FSR, "Mode now FSR FG", "cycle-fsr-2", 0.15),
    ("cycle_off_3", VK_OFF, "Mode now OFF", "cycle-off-3", 0.25),
    ("cycle_dlss_2", VK_DLSS, "Mode now DLSS FG", "cycle-dlss-2", 0.15),
    ("cycle_off_final", VK_OFF, "Mode now OFF", "cycle-off-final", 0.35),
)


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
    bright_rg_ratio: float


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


def post_key_to_process(process_id, virtual_key):
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    matching_windows = []
    enum_proc_type = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
    user32.EnumWindows.argtypes = [enum_proc_type, ctypes.c_void_p]
    user32.EnumWindows.restype = ctypes.c_bool
    user32.GetWindowThreadProcessId.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32)]
    user32.GetWindowThreadProcessId.restype = ctypes.c_uint32
    user32.IsWindowVisible.argtypes = [ctypes.c_void_p]
    user32.IsWindowVisible.restype = ctypes.c_bool
    user32.PostMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_size_t, ctypes.c_ssize_t]
    user32.PostMessageW.restype = ctypes.c_bool

    @enum_proc_type
    def collect_window(hwnd, _lparam):
        owner_process_id = ctypes.c_uint32()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner_process_id))
        if owner_process_id.value == process_id and user32.IsWindowVisible(hwnd):
            matching_windows.append(hwnd)
            return False
        return True

    user32.EnumWindows(collect_window, 0)
    if not matching_windows:
        return False
    hwnd = matching_windows[0]
    return bool(
        user32.PostMessageW(hwnd, WM_KEYDOWN, virtual_key, 0)
        and user32.PostMessageW(hwnd, WM_KEYUP, virtual_key, 0)
    )


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
    bright_rg = 0
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
            if BRIGHT_ROI_TOP <= y < BRIGHT_ROI_BOTTOM and (red >= 120 or green >= 120):
                bright_rg += 1
            luma_sum += red * 0.2126 + green * 0.7152 + blue * 0.0722
            count += 1
    edge_count = len(range(118, ROI_BOTTOM, ROI_STRIDE)) * len(range(ROI_LEFT, ROI_RIGHT, ROI_STRIDE))
    bright_count = len(range(BRIGHT_ROI_TOP, BRIGHT_ROI_BOTTOM, ROI_STRIDE)) * len(
        range(ROI_LEFT, ROI_RIGHT, ROI_STRIDE)
    )
    return dark / count, luma_sum / count, edges / edge_count, bright_rg / bright_count


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
    bright_rg_values = [sample.bright_rg_ratio for sample in phase_samples]
    return {
        "count": len(values),
        "minimum": min(values),
        "median": statistics.median(values),
        "maximum": max(values),
        "edge_minimum": min(edge_values),
        "edge_median": statistics.median(edge_values),
        "edge_maximum": max(edge_values),
        "bright_rg_minimum": min(bright_rg_values),
        "bright_rg_median": statistics.median(bright_rg_values),
        "bright_rg_maximum": max(bright_rg_values),
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
    extended_step_index = 0
    extended_step_key_sent = False
    extended_step_anchor = None

    try:
        ce_process = subprocess.Popen(
            [str(CAPTURE_EXE)], cwd=str(CAPTURE_BIN), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        )
        time.sleep(args.ce_lead_ms / 1000.0)
        long_sequence = args.sequence in ("extended-aba-cycle", "repeat-pure-dlss")
        app_duration = max(args.duration_seconds, 18) if long_sequence else args.duration_seconds
        probe_timeout = max(args.timeout_seconds, 18.0) if long_sequence else args.timeout_seconds
        app_args = [
            str(TESTAPP_EXE),
            f"--duration={app_duration}",
            "--auto-return-fsr=3600",
            "--no-dlss-suspend-stress",
        ]
        if args.sequence == "fsr-to-dlss":
            app_args.extend(
                (
                    f"--auto-fsr-start={args.fsr_start_seconds}",
                    f"--auto-dlss-start={args.dlss_start_seconds}",
                    "--no-dlss-off-stress",
                )
            )
        else:
            app_args.extend(
                (
                    "--auto-fsr-start=3600",
                    "--auto-dlss-start=3600",
                    "--no-dlss-off-stress",
                )
            )
        app_process = subprocess.Popen(
            app_args,
            cwd=str(TESTAPP_BIN),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

        with DesktopProbe(CAPTURE_WIDTH, CAPTURE_HEIGHT) as probe:
            deadline = start + probe_timeout
            while time.monotonic() < deadline and app_process.poll() is None:
                now = time.monotonic()
                log_offset, new_log = read_new_log_text(log_offset)
                if "Auto exit after" in new_log or "Exiting (total frames rendered:" in new_log:
                    events.setdefault("app_exit", now)
                    break
                if "Auto sequence clock reset" in new_log:
                    events.setdefault("ready", now)
                    phase = "off"
                    if extended_step_anchor is None:
                        extended_step_anchor = now
                if "Mode now FSR FG" in new_log:
                    if args.sequence == "dlss-off-fsr" and "off_after" in events:
                        events.setdefault("fsr_after", now)
                        phase = "fsr-after"
                    else:
                        events.setdefault("fsr", now)
                        phase = "fsr"
                if args.sequence == "fsr-to-dlss" and "Mode request: FSR FG -> DLSS FG" in new_log:
                    events.setdefault("transition", now)
                    phase = "transition"
                if (
                    args.sequence in ("dlss-to-off", "dlss-off-fsr", "repeat-pure-dlss")
                    and "Mode request: OFF -> DLSS FG" in new_log
                ):
                    if args.sequence == "repeat-pure-dlss" and "off_after" in events:
                        events.setdefault("dlss_again_transition", now)
                        events.setdefault("transition", now)
                        phase = "dlss-again-transition"
                    else:
                        events.setdefault("dlss_transition", now)
                        phase = "dlss-transition"
                if "DLSS replacement passthrough Present completed" in new_log:
                    events.setdefault("replacement_present", now)
                if "Mode now DLSS FG" in new_log:
                    if args.sequence == "repeat-pure-dlss" and "off_after" in events:
                        events.setdefault("dlss_again", now)
                        phase = "dlss-again"
                    else:
                        events.setdefault("dlss", now)
                        phase = "dlss"
                if args.sequence == "dlss-to-off" and "Mode request: DLSS FG -> OFF" in new_log:
                    events.setdefault("transition", now)
                    phase = "transition"
                if args.sequence in ("dlss-off-fsr", "repeat-pure-dlss") and "Mode request: DLSS FG -> OFF" in new_log:
                    phase = "off-transition"
                if (
                    args.sequence in ("dlss-to-off", "dlss-off-fsr", "repeat-pure-dlss")
                    and "Mode now OFF" in new_log
                    and "dlss" in events
                ):
                    events.setdefault("off_after", now)
                    phase = "off-after"
                if args.sequence == "dlss-off-fsr" and "Mode request: OFF -> FSR FG" in new_log:
                    events.setdefault("transition", now)
                    phase = "fsr-transition"

                if args.sequence == "extended-aba-cycle" and extended_step_index < len(EXTENDED_ABA_STEPS):
                    event_name, virtual_key, completion_text, step_phase, delay_seconds = EXTENDED_ABA_STEPS[
                        extended_step_index
                    ]
                    if extended_step_key_sent and completion_text in new_log:
                        events[event_name] = now
                        phase = step_phase
                        extended_step_index += 1
                        extended_step_key_sent = False
                        extended_step_anchor = now
                    if (
                        extended_step_index < len(EXTENDED_ABA_STEPS)
                        and not extended_step_key_sent
                        and extended_step_anchor is not None
                    ):
                        event_name, virtual_key, _completion_text, step_phase, delay_seconds = EXTENDED_ABA_STEPS[
                            extended_step_index
                        ]
                        if now - extended_step_anchor >= delay_seconds:
                            if not post_key_to_process(app_process.pid, virtual_key):
                                fail(f"could not post extended-cycle key for {event_name}")
                            events[f"{event_name}_key"] = now
                            if event_name == "cycle_off_final":
                                events["transition"] = now
                            phase = f"{step_phase}-transition"
                            extended_step_key_sent = True

                if (
                    args.sequence in ("dlss-to-off", "dlss-off-fsr", "repeat-pure-dlss")
                    and "ready" in events
                    and "dlss_key" not in events
                    and now - events["ready"] >= args.dlss_start_seconds
                ):
                    if not post_key_to_process(app_process.pid, VK_DLSS):
                        fail("could not post the DLSS key to the test app window")
                    events["dlss_key"] = now
                    phase = "dlss-transition"
                if (
                    args.sequence in ("dlss-to-off", "dlss-off-fsr", "repeat-pure-dlss")
                    and "dlss" in events
                    and "off_key" not in events
                    and now - events["dlss"] >= args.off_after_seconds
                ):
                    if not post_key_to_process(app_process.pid, VK_OFF):
                        fail("could not post the all-FG-off key to the test app window")
                    events["off_key"] = now
                if (
                    args.sequence == "dlss-off-fsr"
                    and "off_after" in events
                    and "fsr_key" not in events
                    and now - events["off_after"] >= args.fsr_after_seconds
                ):
                    if not post_key_to_process(app_process.pid, VK_FSR):
                        fail("could not post the FSR key to the test app window")
                    events["fsr_key"] = now
                    events.setdefault("transition", now)
                    phase = "fsr-transition"
                if (
                    args.sequence == "repeat-pure-dlss"
                    and "off_after" in events
                    and "dlss_again_key" not in events
                    and now - events["off_after"] >= args.second_dlss_after_seconds
                ):
                    if not post_key_to_process(app_process.pid, VK_DLSS):
                        fail("could not post the second DLSS key to the test app window")
                    events["dlss_again_key"] = now
                    events.setdefault("transition", now)
                    phase = "dlss-again-transition"

                frame = probe.capture()
                dark_ratio, mean_luma, edge_ratio, bright_rg_ratio = overlay_metric(frame)
                samples.append(
                    Sample((now - start) * 1000.0, phase, dark_ratio, mean_luma, edge_ratio, bright_rg_ratio)
                )
                if phase in ("fsr", "transition", "dlss", "off-after", "fsr-transition", "fsr-after") and dark_ratio < minimum_dark_ratio:
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
                target_sequence_complete = (
                    args.sequence == "extended-aba-cycle" and "cycle_off_final" in events
                ) or (
                    args.sequence == "repeat-pure-dlss" and "dlss_again" in events
                )
                target_completion_time = events.get("cycle_off_final", events.get("dlss_again"))
                if (
                    target_sequence_complete
                    and target_completion_time is not None
                    and (now - target_completion_time) * 1000.0 >= args.seam_ms
                ):
                    events["probe_complete"] = now
                    break
                time.sleep(args.sample_interval_ms / 1000.0)

        if app_process.poll() is None and "app_exit" not in events and "probe_complete" not in events:
            fail(f"test app exceeded {probe_timeout:.1f}s probe timeout")
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
        writer.writerow(("elapsed_ms", "phase", "dark_ratio", "mean_luma", "edge_ratio", "bright_rg_ratio"))
        for sample in samples:
            writer.writerow(
                (
                    f"{sample.elapsed_ms:.3f}",
                    sample.phase,
                    f"{sample.dark_ratio:.6f}",
                    f"{sample.mean_luma:.3f}",
                    f"{sample.edge_ratio:.6f}",
                    f"{sample.bright_rg_ratio:.6f}",
                )
            )

    phase_names = (
        "off", "dlss-transition", "fsr", "transition", "dlss", "off-transition", "off-after",
        "fsr-transition", "fsr-after", "dlss-again-transition", "dlss-again",
    ) + tuple(step[3] for step in EXTENDED_ABA_STEPS) + tuple(f"{step[3]}-transition" for step in EXTENDED_ABA_STEPS)
    summaries = {phase_name: phase_summary(samples, phase_name) for phase_name in phase_names}
    if args.sequence == "extended-aba-cycle":
        baseline_samples = [
            sample
            for sample in samples
            if "cycle_dlss_2" in events
            and "cycle_off_final_key" in events
            and (events["cycle_dlss_2"] - start) * 1000.0
            <= sample.elapsed_ms
            <= (events["cycle_off_final_key"] - start) * 1000.0
        ]
        baseline = statistics.median(sample.bright_rg_ratio for sample in baseline_samples) if baseline_samples else 0.0
    elif args.sequence == "repeat-pure-dlss":
        baseline_summary = summaries.get("dlss")
        baseline = baseline_summary["bright_rg_median"] if baseline_summary else 0.0
    else:
        baseline_phase = "fsr" if args.sequence == "fsr-to-dlss" else "dlss"
        baseline_summary = summaries.get(baseline_phase)
        baseline = baseline_summary["bright_rg_median"] if baseline_summary else 0.0
    transition_start_ms = (events["transition"] - start) * 1000.0 if "transition" in events else 0.0
    if args.sequence == "fsr-to-dlss":
        target_event = "dlss"
    elif args.sequence == "dlss-to-off":
        target_event = "off_after"
    elif args.sequence == "extended-aba-cycle":
        target_event = "cycle_off_final"
    elif args.sequence == "repeat-pure-dlss":
        target_event = "dlss_again"
    else:
        target_event = "fsr_after"
    target_start_ms = (
        (events[target_event] - start) * 1000.0 if target_event in events else samples[-1].elapsed_ms
    )
    seam_end_ms = target_start_ms + args.seam_ms
    seam_samples = [
        sample for sample in samples if transition_start_ms <= sample.elapsed_ms <= seam_end_ms
    ]
    seam_minimum = min((sample.bright_rg_ratio for sample in seam_samples), default=0.0)
    loss_ratio = seam_minimum / baseline if baseline > 0.0 else 0.0
    required_final_retained_ratio = (
        args.minimum_initial_retained_ratio if args.sequence == "repeat-pure-dlss" else args.minimum_retained_ratio
    )
    visible = baseline >= args.minimum_baseline_bright_rg_ratio
    seam_complete = bool(samples) and samples[-1].elapsed_ms >= seam_end_ms
    if args.sequence == "fsr-to-dlss":
        required_events = ("fsr", "dlss")
    elif args.sequence == "dlss-to-off":
        required_events = ("dlss", "off_after")
    elif args.sequence == "extended-aba-cycle":
        required_events = tuple(step[0] for step in EXTENDED_ABA_STEPS)
    elif args.sequence == "repeat-pure-dlss":
        required_events = ("dlss", "off_after", "dlss_again")
    else:
        required_events = ("dlss", "off_after", "fsr_after")
    passed = (
        visible
        and seam_complete
        and seam_minimum >= args.minimum_overlay_bright_rg_ratio
        and loss_ratio >= required_final_retained_ratio
        and all(
        event in events for event in required_events
        )
    )

    initial_dlss_seam = None
    if (
        args.sequence in ("dlss-to-off", "dlss-off-fsr", "repeat-pure-dlss")
        and "dlss_key" in events
        and "dlss" in events
    ):
        initial_start_ms = (events["dlss_key"] - start) * 1000.0
        initial_target_ms = (events["dlss"] - start) * 1000.0
        initial_end_ms = initial_target_ms + args.seam_ms
        initial_samples = [
            sample for sample in samples if initial_start_ms <= sample.elapsed_ms <= initial_end_ms
        ]
        initial_minimum = min((sample.bright_rg_ratio for sample in initial_samples), default=0.0)
        initial_loss_ratio = initial_minimum / baseline if baseline > 0.0 else 0.0
        initial_complete = bool(samples) and samples[-1].elapsed_ms >= initial_end_ms
        initial_dlss_seam = (initial_minimum, initial_loss_ratio, initial_complete)
        passed = (
            passed
            and initial_complete
            and initial_minimum >= args.minimum_overlay_bright_rg_ratio
            and initial_loss_ratio >= args.minimum_initial_retained_ratio
        )

    session = newest_capture_session(start_wall_ns)
    strict_log_markers = (
        "[OVERLAY VISIBILITY] INTERRUPTED/UNPROVEN",
        "[OVERLAY COVERAGE] uncovered streak STARTED",
        "DEVICE REMOVED",
        "DEVICE_REMOVED",
        "0x887A002B",
        "REFLEX-NOT-DETECTED",
    )
    strict_log_counts = {marker: 0 for marker in strict_log_markers}
    required_log_markers = ()
    if args.sequence == "repeat-pure-dlss":
        required_log_markers = (
            "Armed exact prewarmed PostSL handoff backend for its first Present",
            "First exact prewarmed PostSL handoff Present preserved its ready overlay backend",
        )
    required_log_counts = {marker: 0 for marker in required_log_markers}
    if session:
        hook_log = session / "hook_debug.log"
        if hook_log.exists():
            hook_log_text = hook_log.read_text(encoding="utf-8", errors="replace")
            strict_log_counts = {marker: hook_log_text.count(marker) for marker in strict_log_markers}
            required_log_counts = {marker: hook_log_text.count(marker) for marker in required_log_markers}
        else:
            passed = False
    else:
        passed = False
    passed = passed and all(count == 0 for count in strict_log_counts.values())
    passed = passed and all(count > 0 for count in required_log_counts.values())

    print(f"probe duration: {(time.monotonic() - start):.3f}s; samples: {len(samples)}")
    for phase_name, summary in summaries.items():
        if summary:
            print(
                f"{phase_name:10s} n={summary['count']:4d} dark-ratio "
                f"min/median/max={summary['minimum']:.4f}/{summary['median']:.4f}/{summary['maximum']:.4f}; "
                f"edge min/median/max={summary['edge_minimum']:.4f}/{summary['edge_median']:.4f}/"
                f"{summary['edge_maximum']:.4f}; bright-RG min/median/max="
                f"{summary['bright_rg_minimum']:.4f}/{summary['bright_rg_median']:.4f}/"
                f"{summary['bright_rg_maximum']:.4f}"
            )
    print(
        f"{args.sequence} final seam retained overlay bright-RG ratio: {loss_ratio:.3f} "
        f"(required {required_final_retained_ratio:.3f}; window through {args.seam_ms:.0f}ms after target)"
    )
    if initial_dlss_seam:
        initial_minimum, initial_loss_ratio, initial_complete = initial_dlss_seam
        print(
            f"off-to-dlss initial seam retained overlay bright-RG ratio: {initial_loss_ratio:.3f} "
            f"(minimum={initial_minimum:.4f}; required ratio {args.minimum_initial_retained_ratio:.3f} and "
            f"absolute {args.minimum_overlay_bright_rg_ratio:.4f})"
        )
        if not initial_complete:
            print("off-to-dlss initial seam incomplete before the test app exited")
    if not seam_complete:
        observed_until_ms = samples[-1].elapsed_ms - target_start_ms if samples else 0.0
        print(
            f"seam incomplete: observed {max(0.0, observed_until_ms):.0f}ms after target "
            f"before the test app exited (required {args.seam_ms:.0f}ms)"
        )
    print(
        "strict runtime log gates: "
        + ", ".join(f"{marker}={count}" for marker, count in strict_log_counts.items())
    )
    if required_log_counts:
        print(
            "required runtime handoff proofs: "
            + ", ".join(f"{marker}={count}" for marker, count in required_log_counts.items())
        )
    print(f"result: {'PASS' if passed else 'FAIL'}")
    print(f"probe artifacts: {run_dir}")
    if session:
        print(f"captureengine session: {session}")
    return 0 if passed else 1


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--sequence",
        choices=("fsr-to-dlss", "dlss-to-off", "dlss-off-fsr", "extended-aba-cycle", "repeat-pure-dlss"),
        default="fsr-to-dlss",
    )
    parser.add_argument("--timeout-seconds", type=float, default=13.0)
    parser.add_argument("--duration-seconds", type=int, default=7)
    parser.add_argument("--ce-lead-ms", type=float, default=350.0)
    parser.add_argument("--fsr-start-seconds", type=int, default=1)
    parser.add_argument("--dlss-start-seconds", type=int, default=3)
    parser.add_argument("--off-after-seconds", type=int, default=1)
    parser.add_argument("--fsr-after-seconds", type=int, default=1)
    parser.add_argument("--second-dlss-after-seconds", type=float, default=1.0)
    parser.add_argument("--sample-interval-ms", type=float, default=1.0)
    parser.add_argument("--minimum-baseline-bright-rg-ratio", type=float, default=0.02)
    parser.add_argument("--minimum-overlay-bright-rg-ratio", type=float, default=0.01)
    parser.add_argument("--minimum-initial-retained-ratio", type=float, default=0.20)
    parser.add_argument("--minimum-retained-ratio", type=float, default=0.50)
    parser.add_argument("--seam-ms", "--dlss-seam-ms", dest="seam_ms", type=float, default=750.0)
    args = parser.parse_args()
    if (
        args.timeout_seconds <= 0
        or args.duration_seconds <= 0
        or args.sample_interval_ms < 0
        or args.seam_ms < 0
        or args.off_after_seconds <= 0
        or args.fsr_after_seconds <= 0
        or args.second_dlss_after_seconds <= 0
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
