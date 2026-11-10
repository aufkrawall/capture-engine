"""Toolchain identity, health probes and closure proofs for the release preflight."""

from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

from tools import toolchain_state as state

HEADERS_OLD = "mingw-w64-clang-x86_64-headers-14.0.0.r302.gd7f3c5201-1"
HEADERS_NEW = "mingw-w64-clang-x86_64-headers-14.0.0.r569.g34eeafb7d-1"


class ToolchainIdentityTest(unittest.TestCase):
    def test_packages_come_from_the_pacman_local_database(self) -> None:
        with tempfile.TemporaryDirectory() as msys:
            local = Path(msys, "var", "lib", "pacman", "local")
            for name in ("zstd-1.5.7-1", HEADERS_OLD):
                (local / name).mkdir(parents=True)
            (local / "ALPM_DB_VERSION").write_text("9", encoding="utf-8")
            self.assertEqual(state.toolchain_packages(msys), [HEADERS_OLD, "zstd-1.5.7-1"])
            self.assertFalse(state.pacman_busy(msys))
            (local.parent / "db.lck").write_text("", encoding="utf-8")
            self.assertTrue(state.pacman_busy(msys))
        self.assertEqual(state.toolchain_packages(os.path.join(msys, "gone")), [])

    def test_fingerprint_ignores_order_but_not_versions(self) -> None:
        self.assertEqual(state.fingerprint(["a-1-1", "b-2-1"]), state.fingerprint(["b-2-1", "a-1-1"]))
        self.assertNotEqual(state.fingerprint([HEADERS_OLD]), state.fingerprint([HEADERS_NEW]))

    def test_changes_name_the_packages_that_moved(self) -> None:
        # The 2026-11-01 libvpl failure, as the preflight now reports it.
        changes = state.package_changes([HEADERS_OLD, "doxygen-1.18.0-1", "gone-1.0-1"],
                                        [HEADERS_NEW, "doxygen-1.18.0-5", "new-pkg-2.0-1"])
        self.assertEqual(changes, [
            "doxygen 1.18.0-1 -> 1.18.0-5",
            "mingw-w64-clang-x86_64-headers 14.0.0.r302.gd7f3c5201-1 -> 14.0.0.r569.g34eeafb7d-1",
            "+new-pkg 2.0-1",
            "-gone 1.0-1",
        ])


class ClosureProofTest(unittest.TestCase):
    def setUp(self) -> None:
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.dir = temp.name

    def test_a_recorded_build_proves_exactly_its_inputs_and_toolchain(self) -> None:
        state.record_proof(self.dir, "dependency_closure", "inputs-a", [HEADERS_OLD], "release")
        toolchain = state.fingerprint([HEADERS_OLD])
        self.assertIsNotNone(state.find_proof(self.dir, "dependency_closure", "inputs-a", toolchain))
        self.assertIsNone(state.find_proof(self.dir, "dependency_closure", "inputs-b", toolchain))
        self.assertIsNone(state.find_proof(self.dir, "ffmpeg", "inputs-a", toolchain))
        self.assertIsNone(state.find_proof(self.dir, "dependency_closure", "inputs-a",
                                           state.fingerprint([HEADERS_NEW])))
        self.assertEqual(state.proof_packages(self.dir, toolchain), [HEADERS_OLD])

    def test_rerecording_moves_the_proof_to_the_newest_slot(self) -> None:
        state.record_proof(self.dir, "ffmpeg", "x", ["a-1-1"], "build")
        state.record_proof(self.dir, "ffmpeg", "y", ["a-1-1"], "build")
        state.record_proof(self.dir, "ffmpeg", "x", ["a-1-1"], "release")
        proofs = state.load_proofs(self.dir)
        self.assertEqual([(proof["inputs_sha256"], proof["origin"]) for proof in proofs],
                         [("y", "build"), ("x", "release")])
        latest = state.latest_proof(self.dir, "ffmpeg")
        assert latest is not None
        self.assertEqual(latest["inputs_sha256"], "x")

    @unittest.skipUnless(sys.platform == "win32", "Windows readers can deny file replacement")
    def test_an_open_legacy_proof_file_does_not_block_recording(self) -> None:
        legacy = Path(self.dir, "closure_proofs.json")
        legacy.write_text(json.dumps([{
            "kind": "ffmpeg", "inputs_sha256": "old-inputs",
            "toolchain_sha256": state.fingerprint([HEADERS_OLD]), "origin": "build",
        }]), encoding="utf-8")
        with legacy.open("rb"):
            state.record_proof(self.dir, "ffmpeg", "new-inputs", [HEADERS_NEW], "build")
        proofs = state.load_proofs(self.dir)
        self.assertEqual([proof["inputs_sha256"] for proof in proofs], ["old-inputs", "new-inputs"])

    @unittest.skipUnless(sys.platform == "win32", "Windows readers can deny file deletion")
    def test_a_reader_can_keep_a_superseded_record_open(self) -> None:
        state.record_proof(self.dir, "ffmpeg", "x", [HEADERS_OLD], "build")
        record, = Path(self.dir, "proofs").glob("*.json")
        with record.open("rb"):
            state.record_proof(self.dir, "ffmpeg", "x", [HEADERS_OLD], "release")
            self.assertTrue(record.exists())
            self.assertEqual([proof["origin"] for proof in state.load_proofs(self.dir)], ["release"])
        state.record_proof(self.dir, "ffmpeg", "y", [HEADERS_OLD], "build")
        self.assertFalse(record.exists())

    def test_history_is_bounded_and_preserves_the_last_package_inventory(self) -> None:
        state.record_proof(self.dir, "ffmpeg", "old", [HEADERS_OLD], "build")
        for index in range(state.PROOF_HISTORY_LIMIT + 5):
            state.record_proof(self.dir, "ffmpeg", f"inputs-{index}", ["a-1-1"], "build")
        self.assertEqual(len(state.load_proofs(self.dir)), state.PROOF_HISTORY_LIMIT)
        self.assertEqual(len(list(Path(self.dir, "proofs").glob("*.json"))), state.PROOF_HISTORY_LIMIT + 1)
        self.assertEqual(state.proof_packages(self.dir, state.fingerprint([HEADERS_OLD])), [HEADERS_OLD])

    def test_incomplete_proof_does_not_hide_a_complete_record(self) -> None:
        state.record_proof(self.dir, "ffmpeg", "x", ["a-1-1"], "build")
        Path(self.dir, "proofs", "99999999999999999999-interrupted.json").write_text('{"proof":', encoding="utf-8")
        self.assertEqual([proof["inputs_sha256"] for proof in state.load_proofs(self.dir)], ["x"])

    def test_corrupt_legacy_state_reads_as_empty(self) -> None:
        Path(self.dir, "closure_proofs.json").write_text("{not json", encoding="utf-8")
        self.assertEqual(state.load_proofs(self.dir), [])
        Path(self.dir, "closure_proofs.json").write_text(json.dumps({"a": 1}), encoding="utf-8")
        self.assertEqual(state.load_proofs(self.dir), [])

    def test_unknown_kinds_are_rejected(self) -> None:
        with self.assertRaises(ValueError):
            state.record_proof(self.dir, "everything", "x", [], "build")


class ToolProbeTest(unittest.TestCase):
    def test_missing_tools_fail_the_probe(self) -> None:
        with tempfile.TemporaryDirectory() as msys:
            probes = state.probe_tools(msys, [("usr", "bin", "make.exe")])
        self.assertEqual([(probe.tool, probe.ok, probe.detail) for probe in probes],
                         [("make.exe", False, "missing")])

    @unittest.skipUnless(sys.platform == "win32", "the fixture is a batch file")
    def test_a_tool_that_cannot_load_its_dlls_is_named_as_such(self) -> None:
        with tempfile.TemporaryDirectory() as msys:
            tool = Path(msys, "usr", "bin", "doxygen.cmd")
            tool.parent.mkdir(parents=True)
            tool.write_text("@exit /b -1073741515\r\n", encoding="ascii")
            ok = Path(msys, "usr", "bin", "tar.cmd")
            ok.write_text("@exit /b 0\r\n", encoding="ascii")
            probes = state.probe_tools(msys, [("usr", "bin", "doxygen.cmd"), ("usr", "bin", "tar.cmd")])
        self.assertFalse(probes[0].ok)
        self.assertIn("0xC0000135", probes[0].detail)
        self.assertTrue(probes[1].ok)


if __name__ == "__main__":
    unittest.main()
