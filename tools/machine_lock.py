"""Machine-wide named locks shared by every checkout, worktree and the release runner.

The release runner is the development PC: its workspace junctions `build/msys64` and
`external` to the developer tree, so a developer build and a release build share the
toolchain, the custom FFmpeg install under `external/ffmpeg`, and the shared caches.
AGENTS.md has long required "one compile or gate on the whole PC at a time"; this module
is what enforces it instead of relying on every caller to check the process list first.

On Windows the lock is a named mutex in the Global namespace with a permissive DACL and
a low mandatory label, so an elevated and a non-elevated build contend for the same
object instead of one of them failing to open it. The mutex is authoritative: the owner
file next to it is only a diagnostic so waiters can say *who* they are waiting for. A
holder that dies abandons the mutex, and the next waiter takes it over (and says so).

Elsewhere the lock is an flock() on a file under the shared cache root.
"""

from __future__ import annotations

import datetime
import json
import os
import sys
import time
from typing import Any, Callable, Dict, Optional, Sequence

Logger = Callable[[str], None]

_WAIT_OBJECT_0 = 0x00000000
_WAIT_ABANDONED = 0x00000080
_WAIT_TIMEOUT = 0x00000102
# Everyone may synchronize on and release the lock; low integrity label so an elevated
# creator does not lock out a medium-integrity build of the same user.
_LOCK_SDDL = "D:(A;;0x001F0001;;;WD)S:(ML;;NW;;;LW)"


class MachineLock:
    """A named, cross-process lock with holder diagnostics."""

    def __init__(self, name: str, owner_dir: str, *, logger: Optional[Logger] = None) -> None:
        if not name or any(character in name for character in "\\/:"):
            raise ValueError(f"lock name must be a plain token: {name!r}")
        self.name = name
        self.owner_path = os.path.join(owner_dir, f"{name}.owner.json")
        self._log = logger or (lambda _message: None)
        self._handle: Any = None
        self._file: Any = None
        self._owned = False
        self.abandoned_by_previous_holder = False

    # -- platform primitives -------------------------------------------------
    def _open(self) -> bool:
        if self._handle is not None or self._file is not None:
            return True
        if sys.platform == "win32":
            return self._open_windows()
        os.makedirs(os.path.dirname(self.owner_path), exist_ok=True)
        self._file = open(os.path.join(os.path.dirname(self.owner_path), f"{self.name}.lock"), "a+")
        return True

    def _open_windows(self) -> bool:
        import ctypes
        from ctypes import wintypes

        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        advapi32 = ctypes.WinDLL("advapi32", use_last_error=True)

        class SecurityAttributes(ctypes.Structure):
            _fields_ = [
                ("nLength", wintypes.DWORD),
                ("lpSecurityDescriptor", wintypes.LPVOID),
                ("bInheritHandle", wintypes.BOOL),
            ]

        descriptor = wintypes.LPVOID()
        advapi32.ConvertStringSecurityDescriptorToSecurityDescriptorW.argtypes = [
            wintypes.LPCWSTR, wintypes.DWORD, ctypes.POINTER(wintypes.LPVOID), wintypes.LPVOID,
        ]
        attributes = None
        if advapi32.ConvertStringSecurityDescriptorToSecurityDescriptorW(_LOCK_SDDL, 1, ctypes.byref(descriptor), None):
            attributes = SecurityAttributes(ctypes.sizeof(SecurityAttributes), descriptor, False)
        kernel32.CreateMutexW.restype = wintypes.HANDLE
        kernel32.CreateMutexW.argtypes = [wintypes.LPVOID, wintypes.BOOL, wintypes.LPCWSTR]
        handle = kernel32.CreateMutexW(ctypes.byref(attributes) if attributes else None, False,
                                       f"Global\\CaptureEngine.{self.name}")
        error = ctypes.get_last_error()
        if descriptor:
            kernel32.LocalFree(descriptor)
        if not handle:
            self._log(f"WARNING: machine lock '{self.name}' unavailable (CreateMutexW error {error}); "
                      "continuing without machine-wide serialization")
            return False
        kernel32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        kernel32.WaitForSingleObject.restype = wintypes.DWORD
        kernel32.ReleaseMutex.argtypes = [wintypes.HANDLE]
        kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
        self._kernel32 = kernel32
        self._handle = handle
        return True

    def _wait(self, timeout_seconds: float) -> bool:
        if self._handle is not None:
            result = self._kernel32.WaitForSingleObject(self._handle, int(max(0.0, timeout_seconds) * 1000))
            if result == _WAIT_ABANDONED:
                self.abandoned_by_previous_holder = True
                return True
            return result == _WAIT_OBJECT_0
        if sys.platform == "win32":
            return False
        import fcntl

        deadline = time.monotonic() + max(0.0, timeout_seconds)
        while True:
            try:
                fcntl.flock(self._file.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
                return True
            except BlockingIOError:
                if time.monotonic() >= deadline:
                    return False
                time.sleep(min(1.0, max(0.0, deadline - time.monotonic())))

    # -- public interface ----------------------------------------------------
    def holder(self) -> Optional[Dict[str, Any]]:
        """The last recorded holder, or None. Diagnostic only; the lock itself decides."""
        try:
            with open(self.owner_path, "r", encoding="utf-8") as owner_file:
                data = json.load(owner_file)
            return data if isinstance(data, dict) else None
        except (OSError, ValueError):
            return None

    def try_acquire(self) -> bool:
        """Take the lock without waiting. True also when the lock cannot be created at all."""
        if not self._open():
            return True
        if self._wait(0):
            self._owned = True
            self._record_owner()
            return True
        return False

    def acquire(self, *, description: Sequence[str] = (), progress_interval: float = 300.0) -> None:
        """Block until the lock is ours, logging who holds it on the first and every later interval."""
        if not self._open():
            return
        if not self._wait(0):
            started = time.monotonic()
            self._log(f"Waiting for the machine-wide '{self.name}' lock: {describe_holder(self.holder())}")
            while not self._wait(progress_interval):
                waited = int(time.monotonic() - started)
                self._log(f"Still waiting for '{self.name}' after {waited}s: {describe_holder(self.holder())}")
            self._log(f"Acquired the machine-wide '{self.name}' lock after {int(time.monotonic() - started)}s")
        if self.abandoned_by_previous_holder:
            self._log(f"Machine lock '{self.name}' was abandoned by a holder that exited without releasing it "
                      f"(last holder: {describe_holder(self.holder())})")
        self._owned = True
        self._record_owner(description)

    def release(self) -> None:
        """Give the lock back; a no-op for a lock this object never took."""
        if self._owned:
            self._owned = False
            try:
                os.remove(self.owner_path)
            except OSError:
                pass
            if self._handle is not None:
                self._kernel32.ReleaseMutex(self._handle)
            elif self._file is not None and sys.platform != "win32":
                import fcntl

                fcntl.flock(self._file.fileno(), fcntl.LOCK_UN)
        if self._handle is not None:
            self._kernel32.CloseHandle(self._handle)
            self._handle = None
        elif self._file is not None:
            self._file.close()
            self._file = None

    def _record_owner(self, description: Sequence[str] = ()) -> None:
        payload = {
            "pid": os.getpid(),
            "started": datetime.datetime.now().isoformat(timespec="seconds"),
            "workspace": os.getcwd(),
            "command": list(description)[:12],
        }
        try:
            os.makedirs(os.path.dirname(self.owner_path), exist_ok=True)
            temporary = f"{self.owner_path}.{os.getpid()}.tmp"
            with open(temporary, "w", encoding="utf-8") as owner_file:
                json.dump(payload, owner_file)
            os.replace(temporary, self.owner_path)
        except OSError:
            pass

    def __enter__(self) -> "MachineLock":
        self.acquire()
        return self

    def __exit__(self, *_exc: object) -> None:
        self.release()


def describe_holder(holder: Optional[Dict[str, Any]]) -> str:
    if not holder:
        return "holder unknown"
    command = " ".join(str(part) for part in holder.get("command") or [])
    if len(command) > 120:
        command = command[:117] + "..."
    return (f"pid {holder.get('pid', '?')} since {holder.get('started', '?')} in {holder.get('workspace', '?')}"
            + (f" ({command})" if command else ""))
