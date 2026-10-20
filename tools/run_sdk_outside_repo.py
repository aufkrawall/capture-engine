"""Run the SDK's headless client from a deployment outside the repository (libcengine M8).

Copies the installed package (binaries, ffmpeg, licenses, plugins, sdk) into a fresh folder below
build/sdk-outside-repo, adds build/samples/headless.exe under another name, and runs it from another working
directory: create, READY, an audio-only recording that must finalize, shutdown, destroy, twice in one process.
It starts the real runtime and its helper processes (inject, media, ...), so it is opt-in and never part of a
gate. Every process it starts runs in a kill-on-close job, so nothing outlives the run, also on failure.

    python tools/run_sdk_outside_repo.py [--record-ms 3000] [--rounds 2] [--timeout 600] [--keep]

Exit status: the client's (0 success, 1 failure, 2 another CaptureEngine runtime holds this Windows session).
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import List, Optional

ROOT = Path(__file__).resolve().parents[1]
PACKAGE_DIRECTORIES = ("ffmpeg", "licenses", "plugins", "sdk")
PACKAGE_PATTERNS = ("*.exe", "*.dll", "*.pdb", "*.json")
CLIENT_NAME = "sdk_outside_client.exe"


def stage(source: Path, client: Path, target: Path) -> Path:
    """Copy the package (never logs, recordings or user configuration) and the renamed client into `target`."""
    target.mkdir(parents=True, exist_ok=True)
    for name in PACKAGE_DIRECTORIES:
        if (source / name).is_dir():
            shutil.copytree(source / name, target / name)
    for pattern in PACKAGE_PATTERNS:
        for binary in source.glob(pattern):
            shutil.copy2(binary, target / binary.name)
    template = ROOT / "captureengine" / "config.ini.template"
    shutil.copy2(template, target / "config.ini")
    shutil.copy2(client, target / CLIENT_NAME)
    return target / CLIENT_NAME


def client_arguments(record_ms: int, rounds: int) -> List[str]:
    return ["--record-ms", str(record_ms), "--rounds", str(rounds)]


class KillOnCloseJob:
    """A Windows job object whose processes die when the handle closes (the run's cleanup guarantee)."""

    JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x2000
    JobObjectExtendedLimitInformation = 9

    def __init__(self) -> None:
        self.kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        self.kernel32.CreateJobObjectW.restype = ctypes.wintypes.HANDLE
        self.handle = self.kernel32.CreateJobObjectW(None, None)
        if not self.handle:
            raise OSError(ctypes.get_last_error(), "CreateJobObjectW failed")
        info = (ctypes.c_byte * 144)()  # JOBOBJECT_EXTENDED_LIMIT_INFORMATION on x64
        ctypes.c_uint32.from_buffer(info, 16).value = self.JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE  # LimitFlags
        if not self.kernel32.SetInformationJobObject(
            ctypes.wintypes.HANDLE(self.handle), self.JobObjectExtendedLimitInformation, info, ctypes.sizeof(info)
        ):
            raise OSError(ctypes.get_last_error(), "SetInformationJobObject failed")

    def adopt(self, process: subprocess.Popen) -> None:
        if not self.kernel32.AssignProcessToJobObject(
            ctypes.wintypes.HANDLE(self.handle), ctypes.wintypes.HANDLE(int(process._handle))  # noqa: SLF001
        ):
            raise OSError(ctypes.get_last_error(), "AssignProcessToJobObject failed")

    def close(self) -> None:
        if self.handle:
            self.kernel32.CloseHandle(ctypes.wintypes.HANDLE(self.handle))
            self.handle = None


def run(arguments: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--installed", type=Path, default=ROOT / "installed" / "captureengine")
    parser.add_argument("--client", type=Path, default=ROOT / "build" / "samples" / "headless.exe")
    parser.add_argument("--record-ms", type=int, default=3000)
    parser.add_argument("--rounds", type=int, default=2)
    parser.add_argument("--timeout", type=int, default=600, help="seconds for the whole client run")
    parser.add_argument("--keep", action="store_true", help="keep the deployment folder (its logs) after success")
    args = parser.parse_args(arguments)
    if not (args.installed / "cengine.dll").is_file() or not args.client.is_file():
        parser.error("build the product first (cengine.dll and build/samples/headless.exe)")

    root = ROOT / "build" / "sdk-outside-repo"
    root.mkdir(parents=True, exist_ok=True)
    deployment = Path(tempfile.mkdtemp(prefix="deploy-", dir=root))
    working = Path(tempfile.mkdtemp(prefix="cwd-", dir=root))  # a different working directory than the package
    client = stage(args.installed, args.client, deployment)
    job = KillOnCloseJob()
    try:
        process = subprocess.Popen(
            [str(client)] + client_arguments(args.record_ms, args.rounds),
            cwd=working,
            creationflags=subprocess.CREATE_SUSPENDED if hasattr(subprocess, "CREATE_SUSPENDED") else 0x4,
        )
        job.adopt(process)
        ctypes.WinDLL("ntdll").NtResumeProcess(ctypes.wintypes.HANDLE(int(process._handle)))  # noqa: SLF001
        try:
            status = process.wait(timeout=args.timeout)
        except subprocess.TimeoutExpired:
            print(f"The client did not finish within {args.timeout} s; the job ends it and its helpers.")
            status = 1
    finally:
        job.close()  # kills anything the run started that is still alive
    recordings = sorted(p for p in deployment.rglob("*") if p.suffix.lower() in (".mp4", ".mkv", ".m4a", ".flac"))
    print(f"client exit {status}; recordings found: {[str(p.relative_to(deployment)) for p in recordings]}")
    if status == 0 and not recordings and args.record_ms > 0:
        print("The client reported success but no recording file exists below the deployment.")
        status = 1
    if status == 0 and not args.keep:
        shutil.rmtree(deployment, ignore_errors=True)
        shutil.rmtree(working, ignore_errors=True)
    else:
        print(f"Deployment kept at {deployment}")
    return status


if __name__ == "__main__":
    sys.exit(run())
