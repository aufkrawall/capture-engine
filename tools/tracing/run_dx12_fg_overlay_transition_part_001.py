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
        # c_void_p.value is Optional; the constructor checks it is non-NULL, but
        # narrowing it here keeps string_at from being handed None if that ever
        # changes - it would otherwise fault instead of reporting the cause.
        bits = self.bits.value
        if bits is None:
            raise OSError("DIB section has no backing pixel pointer")
        return ctypes.string_at(bits, self.width * self.height * 4)

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
        output.write(
            struct.pack("<IiiHHIIiiII", dib_header_size, width, -height, 1, 32, BI_RGB, pixel_bytes, 0, 0, 0, 0)
        )
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
