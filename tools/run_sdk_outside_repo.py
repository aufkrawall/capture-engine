"""Run the SDK's headless client from a deployment outside the repository (libcengine M8, M19, M21).

Copies the installed package (binaries, ffmpeg, licenses, plugins, sdk) into a fresh folder below
build/sdk-outside-repo, adds build/samples/headless.exe under another name, and runs it from another working
directory: create, READY, --recordings video recordings (audio only with --audio-only) that must finalize with a
file, shutdown, destroy, repeated --rounds times in one process. By default each next recording starts right after
the previous stop (--no-overlap waits for each file), so two media sessions run at once. The staged config records
into the deployment's `recordings` folder, which must hold one file per recording. That runs once per helper mode
(libcengine DR-45, DR-49): `default` leaves the client without the isolation feature (in-process threads),
`isolated` passes --isolated-helpers, which sets CE_FEATURE_ISOLATED_HELPERS. Each mode gets its own deployment,
working directory and kill-on-close job; the modes run one after another, because a Windows session holds one runtime
at a time. `self` adds --self-role-host (CE_FEATURE_SELF_ROLE_HOST, 11 part 6): the client's own executable hosts
the helper processes, and its deployment has no captureengine.exe, so a helper log naming the client as its executable
proves the route.

Afterwards it reads the residue lines the runtime logs ([MediaTeardown] when the last in-process recording is gone,
[RuntimeTeardown] when a runtime is destroyed; 11, part 2) and prints them per round. With --strict-residue a round
after the first that leaves handles, threads, GDI or USER objects behind fails the mode (the first round is warm-up:
one-time driver and cache allocations).

It starts the real runtime and its helper processes, so it is opt-in and never part of a gate. Every process it
starts runs in a kill-on-close job, so nothing outlives the run, also on failure.

    python tools/run_sdk_outside_repo.py [--record-ms 3000] [--rounds 2] [--recordings 2] [--no-overlap]
                                         [--audio-only] [--timeout 600] [--helpers all|both|default|isolated|self]
                                         [--strict-residue] [--keep]

Exit status: 0 when every selected mode succeeded, else the first failing mode's status (the client's: 0 success,
1 failure, 2 another CaptureEngine runtime holds this Windows session).
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Tuple

ROOT = Path(__file__).resolve().parents[1]
PACKAGE_DIRECTORIES = ("ffmpeg", "licenses", "plugins", "sdk")
PACKAGE_PATTERNS = ("*.exe", "*.dll", "*.pdb", "*.json")
CLIENT_NAME = "sdk_outside_client.exe"
CONFIG_TEMPLATE = ROOT / "captureengine" / "config.ini.template"
# The deployment's own recording folder: the template's [Output] output_dir (the user's Videos folder) points here, so
# the run never writes into the user's folders and every file it records is found and removed with the deployment.
RECORDINGS_DIRECTORY = "recordings"
REAP_TIMEOUT_S = 30  # bound for reaping a client that the job has just ended
ROLE_HOST_LINE = re.compile(r"\[RoleHost\] helper executable=(\S+)")


@dataclass(frozen=True)
class HelperMode:
    """How a mode's client runs its helpers. The staged config.ini is the shipped template with [Runtime]
    isolated_helpers=auto, so the client's feature bits decide."""

    flags: Tuple[str, ...]  # client arguments that set the feature bits
    package_role_host: bool  # the deployment has captureengine.exe; False: the client hosts its helpers


HELPER_MODES: Dict[str, HelperMode] = {
    "default": HelperMode((), True),
    "isolated": HelperMode(("--isolated-helpers",), True),
    "self": HelperMode(("--isolated-helpers", "--self-role-host"), False),
}
MODE_GROUPS: Dict[str, Tuple[str, ...]] = {"all": ("default", "isolated", "self"), "both": ("default", "isolated")}
RESIDUE_LINE = re.compile(r"\[(MediaTeardown|RuntimeTeardown)\] residue (?!threads:)(.*)$")
RESIDUE_FIELD = re.compile(r"(\w+)=([+-]?[0-9.]+|n/a)")
COUNT_FIELDS = ("handles", "threads", "gdi", "user")


@dataclass
class Residue:
    session: str  # the runtime session folder (one per round)
    kind: str  # MediaTeardown or RuntimeTeardown
    fields: Dict[str, str]
    text: str


def selected_modes(choice: str) -> List[Tuple[str, HelperMode]]:
    """`all` yields default, isolated and self in that order; `both` the first two; a mode name only itself."""
    names = MODE_GROUPS.get(choice, (choice,))
    return [(name, HELPER_MODES[name]) for name in names]


def staged_config(template: str, recordings: Path) -> str:
    """The shipped template with only [Output] output_dir changed, to `recordings`."""
    lines = template.splitlines(keepends=True)
    section = ""
    replaced = False
    for index, line in enumerate(lines):
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            section = stripped[1:-1]
        elif section == "Output" and "=" in stripped and stripped.split("=", 1)[0].strip() == "output_dir":
            ending = line[len(line.rstrip("\r\n")):]
            lines[index] = f"output_dir={recordings}{ending}"
            replaced = True
    if not replaced:
        raise ValueError("the config template has no [Output] output_dir")
    return "".join(lines)


def stage(source: Path, client: Path, target: Path, package_role_host: bool = True) -> Path:
    """Copy the package (never logs, recordings or user configuration) and the renamed client into `target`; without
    `package_role_host` the package's captureengine.exe stays behind."""
    target.mkdir(parents=True, exist_ok=True)
    for name in PACKAGE_DIRECTORIES:
        if (source / name).is_dir():
            shutil.copytree(source / name, target / name)
    for pattern in PACKAGE_PATTERNS:
        for binary in source.glob(pattern):
            if not package_role_host and binary.stem.lower() == "captureengine":
                continue
            shutil.copy2(binary, target / binary.name)
    recordings = target / RECORDINGS_DIRECTORY
    recordings.mkdir()
    with open(CONFIG_TEMPLATE, encoding="utf-8", newline="") as template:
        config = staged_config(template.read(), recordings)
    with open(target / "config.ini", "w", encoding="utf-8", newline="") as staged:
        staged.write(config)
    shutil.copy2(client, target / CLIENT_NAME)
    return target / CLIENT_NAME


def client_arguments(record_ms: int, rounds: int, mode_flags: Tuple[str, ...] = (), video: bool = False,
                     recordings: int = 1, overlap: bool = False) -> List[str]:
    arguments = ["--record-ms", str(record_ms), "--rounds", str(rounds)]
    if video:
        arguments.append("--video")
    if recordings > 1:
        arguments += ["--recordings", str(recordings)]
        if overlap:
            arguments.append("--overlap")
    return arguments + list(mode_flags)


def parse_residue(session: str, line: str) -> Optional[Residue]:
    match = RESIDUE_LINE.search(line.rstrip())
    if not match:
        return None
    fields = dict(RESIDUE_FIELD.findall(match.group(2)))
    return Residue(session, match.group(1), fields, match.group(2))


def collect_residue(deployment: Path) -> List[Residue]:
    """Every residue line of every runtime session below the deployment's logs, in session order."""
    found: List[Residue] = []
    logs = deployment / "logs"
    if not logs.is_dir():
        return found
    for session in sorted(p for p in logs.iterdir() if p.is_dir()):
        for log in sorted(session.glob("*.log")):
            with open(log, encoding="utf-8", errors="replace") as handle:
                for line in handle:
                    residue = parse_residue(session.name, line)
                    if residue:
                        found.append(residue)
    return found


def helper_executables(deployment: Path) -> List[str]:
    """The executables the helper processes of every session ran from, as their [RoleHost] lines name them."""
    found: List[str] = []
    logs = deployment / "logs"
    if not logs.is_dir():
        return found
    for log in sorted(logs.rglob("*.log")):
        with open(log, encoding="utf-8", errors="replace") as handle:
            for line in handle:
                match = ROLE_HOST_LINE.search(line)
                if match:
                    found.append(match.group(1))
    return found


def residue_failures(residues: List[Residue]) -> List[str]:
    """Rounds after the first whose runtime or media teardown left counted objects behind."""
    failures: List[str] = []
    sessions = sorted({r.session for r in residues})
    warm = set(sessions[1:])
    for residue in residues:
        if residue.session not in warm:
            continue
        grown = [f"{name}={residue.fields[name]}" for name in COUNT_FIELDS
                 if name in residue.fields and residue.fields[name] not in ("n/a",) and float(residue.fields[name]) > 0]
        if grown:
            failures.append(f"{residue.session} {residue.kind}: " + " ".join(grown))
    return failures


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


def run_client(client: Path, arguments: List[str], working: Path, timeout: int) -> int:
    """Runs the client in its own kill-on-close job; returns its exit status (1 when it outlives `timeout`)."""
    job = KillOnCloseJob()
    try:
        process = subprocess.Popen(
            [str(client)] + arguments,
            cwd=working,
            creationflags=subprocess.CREATE_SUSPENDED if hasattr(subprocess, "CREATE_SUSPENDED") else 0x4,
        )
        try:
            job.adopt(process)
        except OSError:
            process.kill()  # suspended and outside the job: nothing else would end it
            raise
        status = ctypes.WinDLL("ntdll").NtResumeProcess(ctypes.wintypes.HANDLE(int(process._handle)))  # noqa: SLF001
        if status != 0:
            raise OSError(f"NtResumeProcess failed: 0x{status & 0xFFFFFFFF:08X}")
        try:
            return process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            print(f"The client did not finish within {timeout} s; the job ends it and its helpers.")
            job.close()  # the job kill ends the client and its helpers; reap the client below
            process.wait(timeout=REAP_TIMEOUT_S)
            return 1
    finally:
        job.close()  # kills anything the run started that is still alive


def run_mode(name: str, mode: HelperMode, args: argparse.Namespace, root: Path) -> int:
    deployment = Path(tempfile.mkdtemp(prefix=f"deploy-{name}-", dir=root))
    working = Path(tempfile.mkdtemp(prefix=f"cwd-{name}-", dir=root))  # a different working directory than the package
    client = stage(args.installed, args.client, deployment, mode.package_role_host)
    helpers = " ".join(mode.flags) if mode.flags else "the default helper transport"
    host = "captureengine.exe" if mode.package_role_host else "the client (no captureengine.exe deployed)"
    print(f"[{name}] deployment {deployment}; client with {helpers}; helper processes run from {host}")
    arguments = client_arguments(args.record_ms, args.rounds, mode.flags, not args.audio_only, args.recordings,
                                 not args.no_overlap)
    status = run_client(client, arguments, working, args.timeout)
    recordings = sorted(p for p in (deployment / RECORDINGS_DIRECTORY).rglob("*") if p.is_file())
    print(f"[{name}] client exit {status}; recordings found: {[str(p.relative_to(deployment)) for p in recordings]}")
    expected = args.rounds * args.recordings if args.record_ms > 0 else 0
    if status == 0 and len(recordings) != expected:
        print(f"[{name}] The client reported success, but {RECORDINGS_DIRECTORY} holds {len(recordings)} "
              f"recording(s) instead of {expected} (one per recording).")
        status = 1
    if not mode.package_role_host:
        hosts = sorted(set(helper_executables(deployment)))
        print(f"[{name}] helper executables: {hosts}")
        if status == 0 and hosts != [CLIENT_NAME]:
            print(f"[{name}] Expected every helper process to run from {CLIENT_NAME}.")
            status = 1
    residues = collect_residue(deployment)
    for residue in residues:
        print(f"[{name}] {residue.session} {residue.kind}: {residue.text}")
    if not residues:
        print(f"[{name}] no residue lines (logging off, or no runtime reached its teardown)")
    failures = residue_failures(residues)
    for failure in failures:
        print(f"[{name}] residue after warm-up: {failure}")
    if status == 0 and failures and args.strict_residue:
        status = 1
    if status == 0 and not args.keep:
        shutil.rmtree(deployment, ignore_errors=True)
        shutil.rmtree(working, ignore_errors=True)
    else:
        print(f"[{name}] Deployment kept at {deployment}")
    return status


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--installed", type=Path, default=ROOT / "installed" / "captureengine")
    parser.add_argument("--client", type=Path, default=ROOT / "build" / "samples" / "headless.exe")
    parser.add_argument("--record-ms", type=int, default=3000)
    parser.add_argument("--rounds", type=int, default=2)
    parser.add_argument("--recordings", type=int, default=2, help="recordings per round (per runtime)")
    parser.add_argument("--no-overlap", action="store_true",
                        help="wait for each recording's file before the next starts (default: start right away)")
    parser.add_argument("--audio-only", action="store_true", help="record audio only instead of video")
    parser.add_argument("--timeout", type=int, default=600, help="seconds for each client run (one per helper mode)")
    parser.add_argument("--helpers", choices=(*MODE_GROUPS, *HELPER_MODES), default="all",
                        help="helper modes to run: default (the runtime's transport), isolated "
                        "(CE_FEATURE_ISOLATED_HELPERS), self (isolated helpers hosted by the client's executable, "
                        "CE_FEATURE_SELF_ROLE_HOST), both (default and isolated) or all")
    parser.add_argument("--strict-residue", action="store_true",
                        help="fail a mode whose rounds after the first leave counted objects behind")
    parser.add_argument("--keep", action="store_true", help="keep the deployment folders (their logs) after success")
    return parser


def run(arguments: Optional[List[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(arguments)
    if args.recordings < 1:
        parser.error("--recordings must be at least 1")
    if not (args.installed / "cengine.dll").is_file() or not args.client.is_file():
        parser.error("build the product first (cengine.dll and build/samples/headless.exe)")

    root = ROOT / "build" / "sdk-outside-repo"
    root.mkdir(parents=True, exist_ok=True)
    results = [(name, run_mode(name, mode, args, root)) for name, mode in selected_modes(args.helpers)]
    print("summary: " + ", ".join(f"{name} exit {status}" for name, status in results))
    return next((status for _, status in results if status != 0), 0)


if __name__ == "__main__":
    sys.exit(run())
