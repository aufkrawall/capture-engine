"""Machine-wide build lock: one build.py on the PC at a time, with an attributable holder."""

# build.py executes its fragments via exec; pyright cannot see their attributes.
# pyright: reportAttributeAccessIssue=false

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
import uuid
from pathlib import Path
from unittest import mock

from tools.machine_lock import MachineLock, describe_holder

PROJECT_ROOT = Path(__file__).resolve().parents[2]

_CHILD = """
import sys
sys.path.insert(0, sys.argv[1])
from tools.machine_lock import MachineLock
lock = MachineLock(sys.argv[2], sys.argv[3])
taken = lock.try_acquire()
print("taken" if taken else "busy")
if sys.argv[4] == "release":
    lock.release()
"""


def _child(name: str, owner_dir: str, mode: str = "release") -> str:
    result = subprocess.run(
        [sys.executable, "-c", _CHILD, str(PROJECT_ROOT), name, owner_dir, mode],
        capture_output=True,
        text=True,
        check=True,
    )
    return result.stdout.strip()


class MachineLockTest(unittest.TestCase):
    def setUp(self) -> None:
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.owner_dir = temp.name
        # A unique name per test keeps real builds and parallel test runs out of the fixture.
        self.name = f"selftest-{uuid.uuid4().hex[:12]}"

    def test_rejects_names_that_would_escape_the_namespace(self) -> None:
        for bad in ("", "a\\b", "a/b", "Global:x"):
            with self.assertRaises(ValueError):
                MachineLock(bad, self.owner_dir)

    def test_a_held_lock_is_busy_for_another_process_and_free_after_release(self) -> None:
        lock = MachineLock(self.name, self.owner_dir)
        self.assertTrue(lock.try_acquire())
        try:
            self.assertEqual(_child(self.name, self.owner_dir), "busy")
            holder = lock.holder()
            self.assertIsNotNone(holder)
            assert holder is not None
            self.assertEqual(holder["pid"], os.getpid())
        finally:
            lock.release()
        self.assertIsNone(lock.holder(), "release must remove the holder record")
        self.assertEqual(_child(self.name, self.owner_dir), "taken")

    @unittest.skipUnless(sys.platform == "win32", "mutex abandonment is a Windows semantic")
    def test_a_holder_that_exits_without_releasing_hands_the_lock_over(self) -> None:
        # A waiter keeps the mutex object alive while the holder dies, exactly as a
        # build waiting on a crashed one does; with no open handle the object would
        # simply vanish with its owner.
        lock = MachineLock(self.name, self.owner_dir)
        self.assertTrue(lock._open())
        self.assertEqual(_child(self.name, self.owner_dir, mode="keep"), "taken")
        messages = []
        lock._log = messages.append
        try:
            lock.acquire(description=["build.py", "--verify"])
            self.assertTrue(lock.abandoned_by_previous_holder)
            self.assertTrue(any("abandoned" in message for message in messages))
        finally:
            lock.release()

    def test_waiting_names_the_holder(self) -> None:
        lock = MachineLock(self.name, self.owner_dir)
        lock._record_owner(["build.py", "--incremental", "--run-tests"])
        description = describe_holder(lock.holder())
        self.assertIn(f"pid {os.getpid()}", description)
        self.assertIn("--incremental --run-tests", description)
        self.assertEqual(describe_holder(None), "holder unknown")
        os.remove(lock.owner_path)

    def test_unavailable_lock_does_not_block_the_build(self) -> None:
        lock = MachineLock(self.name, self.owner_dir)
        with mock.patch.object(MachineLock, "_open", return_value=False):
            self.assertTrue(lock.try_acquire())
            lock.acquire()
        lock.release()


class BuildLockPolicyTest(unittest.TestCase):
    def setUp(self) -> None:
        import build

        self.build = build

    def test_top_level_runs_take_the_lock_and_the_sanitizer_child_does_not(self) -> None:
        with mock.patch.dict(os.environ, {"CE_BUILD_LOCK": "1"}):
            self.assertTrue(self.build.machine_build_lock_required(["--verify", "--skip-updates"]))
            self.assertTrue(self.build.machine_build_lock_required(["--no-build", "--lint"]))
            self.assertFalse(self.build.machine_build_lock_required(["--run-tests", "--sanitize-regression-child"]))
            self.assertFalse(self.build.machine_build_lock_required(["--help"]))

    def test_explicit_opt_out_is_honored(self) -> None:
        with mock.patch.dict(os.environ, {"CE_BUILD_LOCK": "0"}):
            self.assertFalse(self.build.machine_build_lock_required(["--verify"]))

    def test_build_lock_lives_in_the_shared_cache(self) -> None:
        lock_dir = self.build.shared_cache_path(self.build.PROJECT_ROOT, "locks", msys2_dir=self.build.MSYS2_DIR)
        self.assertTrue(lock_dir.endswith(os.path.join("shared-cache", "locks"))
                        or os.environ.get("CE_SHARED_CACHE_DIR"))


if __name__ == "__main__":
    unittest.main()
