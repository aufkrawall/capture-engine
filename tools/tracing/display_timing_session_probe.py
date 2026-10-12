#!/usr/bin/env python3
"""Compare ETW writer isolation with a bounded native DX12 renderer.

Raw traces contain process/GPU data. Keep the output private and out of Git.
Each case owns its trace and children; existing collector sessions are untouched.
"""

import argparse
import collections
import csv
import ctypes
from ctypes import wintypes
import os
from pathlib import Path
import subprocess
import time
import uuid

ROOT = Path(__file__).resolve().parents[2]
REALTIME = 0x00000100
NO_PER_PROCESSOR = 0x10000000
INDEPENDENT = 0x08000000
KERNEL_PROVIDER = "802ec45a-1e99-4b83-9920-87c98277ba9d"
RUNTIME_PROVIDER = "ca11c036-0102-4a2d-a6ad-f03cfed5d3c9"


class Guid(ctypes.Structure):
    _fields_ = [("a", wintypes.DWORD), ("b", wintypes.WORD), ("c", wintypes.WORD),
                ("data", ctypes.c_ubyte * 8)]

    @classmethod
    def parse(cls, value):
        return cls.from_buffer_copy(uuid.UUID(value).bytes_le)


class Wnode(ctypes.Structure):
    _fields_ = [("BufferSize", wintypes.DWORD), ("ProviderId", wintypes.DWORD),
                ("HistoricalContext", ctypes.c_uint64), ("TimeStamp", ctypes.c_int64),
                ("Guid", Guid), ("ClientContext", wintypes.DWORD), ("Flags", wintypes.DWORD)]


class Properties(ctypes.Structure):
    _fields_ = [("Wnode", Wnode)] + [(name, wintypes.DWORD) for name in (
        "BufferSize", "MinimumBuffers", "MaximumBuffers", "MaximumFileSize", "LogFileMode",
        "FlushTimer", "EnableFlags", "AgeLimit", "NumberOfBuffers", "FreeBuffers", "EventsLost",
        "BuffersWritten", "LogBuffersLost", "RealTimeBuffersLost",
    )] + [("LoggerThreadId", wintypes.HANDLE), ("LogFileNameOffset", wintypes.DWORD),
          ("LoggerNameOffset", wintypes.DWORD)]


class Descriptor(ctypes.Structure):
    _fields_ = [("Ptr", ctypes.c_uint64), ("Size", wintypes.ULONG), ("Type", wintypes.ULONG)]


class EnableParameters(ctypes.Structure):
    _fields_ = [("Version", wintypes.ULONG), ("EnableProperty", wintypes.ULONG),
                ("ControlFlags", wintypes.ULONG), ("SourceId", Guid),
                ("EnableFilterDesc", ctypes.POINTER(Descriptor)), ("FilterDescCount", wintypes.ULONG)]


class Etw:
    def __init__(self):
        self.api = ctypes.WinDLL("advapi32")
        self.api.StartTraceW.argtypes = [ctypes.POINTER(ctypes.c_uint64), wintypes.LPCWSTR,
                                         ctypes.POINTER(Properties)]
        self.api.StartTraceW.restype = wintypes.ULONG
        self.api.ControlTraceW.argtypes = [ctypes.c_uint64, wintypes.LPCWSTR,
                                           ctypes.POINTER(Properties), wintypes.ULONG]
        self.api.ControlTraceW.restype = wintypes.ULONG
        self.api.EnableTraceEx2.argtypes = [ctypes.c_uint64, ctypes.POINTER(Guid), wintypes.ULONG,
                                            ctypes.c_ubyte, ctypes.c_uint64, ctypes.c_uint64,
                                            wintypes.ULONG, ctypes.POINTER(EnableParameters)]
        self.api.EnableTraceEx2.restype = wintypes.ULONG

    def enable(self, handle, provider, keywords, ids):
        payload = ctypes.create_string_buffer(
            bytes((1, 0)) + len(ids).to_bytes(2, "little") +
            b"".join(value.to_bytes(2, "little") for value in ids))
        descriptor = Descriptor(ctypes.addressof(payload), len(payload) - 1, 0x80000200)
        parameters = EnableParameters()
        parameters.Version = 2
        parameters.EnableProperty = 0x10
        parameters.EnableFilterDesc = ctypes.pointer(descriptor)
        parameters.FilterDescCount = 1
        guid = Guid.parse(provider)
        status = self.api.EnableTraceEx2(handle, ctypes.byref(guid), 1, 5, keywords, 0, 0,
                                         ctypes.byref(parameters))
        if status:
            raise ctypes.WinError(status)


def system_tracerpt():
    buffer = ctypes.create_unicode_buffer(32768)
    api = ctypes.WinDLL("kernel32")
    api.GetSystemDirectoryW.argtypes = [wintypes.LPWSTR, wintypes.UINT]
    api.GetSystemDirectoryW.restype = wintypes.UINT
    length = api.GetSystemDirectoryW(buffer, len(buffer))
    if not length or length >= len(buffer):
        raise RuntimeError("Cannot resolve the Windows system directory")
    path = Path(buffer.value) / "tracerpt.exe"
    if not path.is_file():
        raise RuntimeError("The Windows tracerpt tool is unavailable")
    return path


def stop_child(child):
    if child is not None and child.poll() is None:
        child.terminate()
        try:
            child.wait(timeout=5)
        except subprocess.TimeoutExpired:
            child.kill()
            child.wait(timeout=5)


def wait_for_consumer_file(trace, child):
    api = ctypes.WinDLL("kernel32", use_last_error=True)
    api.FindFirstChangeNotificationW.argtypes = [wintypes.LPCWSTR, wintypes.BOOL, wintypes.DWORD]
    api.FindFirstChangeNotificationW.restype = wintypes.HANDLE
    api.FindNextChangeNotification.argtypes = [wintypes.HANDLE]
    api.FindCloseChangeNotification.argtypes = [wintypes.HANDLE]
    api.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    api.WaitForSingleObject.restype = wintypes.DWORD
    notification = api.FindFirstChangeNotificationW(str(trace.parent), False, 0x1 | 0x8)
    if notification == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        # Native directory notifications provide readiness; five seconds is a failure bound.
        deadline = time.monotonic() + 5
        while not trace.exists():
            if child.poll() is not None:
                raise RuntimeError("The trace consumer exited before creating its output")
            remaining_ms = int((deadline - time.monotonic()) * 1000)
            if remaining_ms <= 0 or api.WaitForSingleObject(notification, remaining_ms) != 0:
                raise RuntimeError("The trace consumer did not create its output")
            if not api.FindNextChangeNotification(notification):
                raise ctypes.WinError(ctypes.get_last_error())
    finally:
        api.FindCloseChangeNotification(notification)


def probe(etw, tracerpt, renderer, output, label, independent, duration):
    name = f"CE_Diagnostic_Display_{os.getpid()}_{label}"
    encoded = (name + "\0").encode("utf-16-le")
    storage = ctypes.create_string_buffer(ctypes.sizeof(Properties) + len(encoded))
    properties = Properties.from_buffer(storage)
    properties.Wnode.BufferSize = len(storage)
    properties.Wnode.Flags = 0x20000
    properties.Wnode.ClientContext = 1
    properties.BufferSize = 64
    properties.MinimumBuffers = 4
    properties.MaximumBuffers = 16
    properties.LogFileMode = REALTIME | NO_PER_PROCESSOR | (INDEPENDENT if independent else 0)
    properties.FlushTimer = 1
    properties.LoggerNameOffset = ctypes.sizeof(Properties)
    ctypes.memmove(ctypes.addressof(storage) + properties.LoggerNameOffset, encoded, len(encoded))
    handle = ctypes.c_uint64()
    status = etw.api.StartTraceW(ctypes.byref(handle), name, ctypes.byref(properties))
    if status:
        raise ctypes.WinError(status)
    consumer = None
    try:
        etw.enable(handle.value, KERNEL_PROVIDER, 1, (178, 180, 116, 259, 17, 273, 382, 386))
        etw.enable(handle.value, RUNTIME_PROVIDER, 0x8000000000000002, (42, 55))
        trace = output / f"{label}.csv"
        consumer = subprocess.Popen(
            [str(tracerpt), "-rt", name, "-of", "CSV", "-en", "Unicode", "-o", str(trace),
             "-summary", str(output / f"{label}_summary.txt"), "-y"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        wait_for_consumer_file(trace, consumer)
        result = subprocess.run(
            [str(renderer), "640", "360", "0", "--windowed", f"--duration={duration}",
             "--auto-fsr-start=3600", "--auto-dlss-start=3600", "--auto-return-fsr=3600",
             "--no-fsr-suspend-stress", "--no-dlss-suspend-stress", "--no-dlss-off-stress"],
            cwd=renderer.parent, capture_output=True, timeout=duration + 15)
        (output / f"{label}_renderer.txt").write_bytes(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(f"Renderer failed: {result.returncode}; inspect its private output")
    finally:
        stop_status = etw.api.ControlTraceW(handle.value, name, ctypes.byref(properties), 1)
        if consumer is not None:
            try:
                stdout, stderr = consumer.communicate(timeout=5)
                (output / f"{label}_consumer.txt").write_bytes(stdout + stderr)
            finally:
                stop_child(consumer)
        if stop_status:
            raise ctypes.WinError(stop_status)
    with trace.open(encoding="utf-16", errors="replace", newline="") as stream:
        rows = csv.DictReader(stream)
        counts = collections.Counter((row["  Event Name"].strip(), int(row["     Event ID"]))
                                     for row in rows if row["  Event Name"].strip() != "LostEvent")
    print(f"{label}: independent={int(independent)} counts={dict(counts)}", flush=True)
    return counts


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--duration", type=int, default=3, choices=range(1, 11))
    parser.add_argument("--output", type=Path, default=ROOT / "build" / "display_timing_session_probe")
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("This diagnostic requires Windows")
    renderer = ROOT / "installed" / "testapp" / "dx12_fg_switch_test.exe"
    if not renderer.is_file():
        parser.error("Build the declared native test app before running this diagnostic")
    output = args.output.resolve() / f"run-{os.getpid()}-{uuid.uuid4().hex[:8]}"
    output.mkdir(parents=True, exist_ok=False)
    etw = Etw()
    tracerpt = system_tracerpt()
    probe(etw, tracerpt, renderer, output, "baseline", False, args.duration)
    isolated = probe(etw, tracerpt, renderer, output, "independent", True, args.duration)
    probe(etw, tracerpt, renderer, output, "baseline_again", False, args.duration)
    if not isolated[("Microsoft-Windows-DXGI", 42)]:
        raise RuntimeError("The isolated trace received no DXGI Present events; inspect the private artifacts")


if __name__ == "__main__":
    main()
