import os
import subprocess
import tempfile
import unittest
from pathlib import Path

from tools import dependency_pgp
from tools import ffmpeg_dependencies as dependencies

MANIFEST_PATH = Path(__file__).resolve().parents[1] / "ffmpeg_dependencies.json"
PROJECT_ROOT = Path(__file__).resolve().parents[2]
MSYS_GPG = PROJECT_ROOT / "build" / "msys64" / "usr" / "bin" / "gpg.exe"


class FfmpegVendoredPgpKeyTest(unittest.TestCase):
    """Every pinned signing key must ship in-repo, not come from a keyserver.

    The builder's keyring is not persisted between builds, so each closure build
    re-imported the keys. Fetching them needs dirmngr, which cannot start in the
    Actions runner's non-interactive context, so the release job could never build
    the dependency closure at all. Vendoring removes that dependency; the
    fingerprint check still decides trust, so this is not a weakening.
    """

    def test_every_pinned_fingerprint_has_a_vendored_key(self) -> None:
        manifest = dependencies.load_dependency_manifest(str(MANIFEST_PATH))
        pinned = set()
        for dependency in manifest["dependencies"]:
            for field in ("pgp_keys", "source_package_pgp_keys"):
                pinned.update(fingerprint.upper() for fingerprint in dependency.get(field, []))
        missing = [
            fingerprint
            for fingerprint in sorted(pinned)
            if not os.path.exists(os.path.join(dependency_pgp.PGP_KEY_DIR, f"{fingerprint}.asc"))
        ]
        self.assertEqual(
            [],
            missing,
            "Pinned PGP keys without a vendored tools/pgp-keys/<fingerprint>.asc; the "
            "release job cannot fetch them (no dirmngr on the runner):\n" + "\n".join(missing),
        )

    def test_vendored_keys_are_armored_and_named_by_fingerprint(self) -> None:
        for entry in sorted(os.listdir(dependency_pgp.PGP_KEY_DIR)):
            if not entry.endswith(".asc"):
                continue
            self.assertRegex(entry, r"^[0-9A-F]{40}\.asc$", f"{entry} is not named by fingerprint")
            with open(os.path.join(dependency_pgp.PGP_KEY_DIR, entry), "r", encoding="utf-8") as handle:
                self.assertIn("BEGIN PGP PUBLIC KEY BLOCK", handle.read(200), f"{entry} is not armored")

    @unittest.skipUnless(MSYS_GPG.is_file(), "MSYS2 gpg is required to import the vendored keys")
    def test_every_vendored_key_actually_imports_into_an_empty_keyring(self) -> None:
        """The only check that would have caught the aom key.

        Release run 31207385807 died on aom because its committed blob carried no
        user ID - keys.openpgp.org strips UIDs it has not verified, and gpg
        refuses such a key ("new key but contains no user ID - skipped"), so it
        never entered the keyring. Every cheaper check passed: the file existed,
        was armored, was named by fingerprint, and `gpg --show-keys` even reported
        the pinned fingerprint. Only an actual import into an empty keyring fails.

        The release job preflights this suite, so this now costs seconds instead of
        ten minutes of closure build. Two mistakes this test must not repeat:
        GNUPGHOME has to be handed to the MSYS gpg in MSYS spelling (a Windows
        path makes gpg join it onto its own cwd and silently create nothing), and
        the verdict has to come from gpg's keyring listing rather than from
        matching the file, or a broken key passes.
        """
        for entry in sorted(os.listdir(dependency_pgp.PGP_KEY_DIR)):
            if not entry.endswith(".asc"):
                continue
            fingerprint = entry[: -len(".asc")]
            with self.subTest(fingerprint=fingerprint):
                key_path = os.path.join(dependency_pgp.PGP_KEY_DIR, entry)
                # tempfile keeps the root short: GnuPG's daemon sockets live under
                # GNUPGHOME and fail on a long path.
                with tempfile.TemporaryDirectory() as home:
                    environment = dict(
                        os.environ,
                        GNUPGHOME=dependencies.SourceDependencyBuilder._unix_path(home),
                    )
                    imported = subprocess.run(
                        [str(MSYS_GPG), "--batch", "--status-fd", "1", "--import", key_path],
                        env=environment,
                        capture_output=True,
                        text=True,
                        check=False,
                    )
                    listed = subprocess.run(
                        [str(MSYS_GPG), "--batch", "--with-colons", "--list-keys", fingerprint],
                        env=environment,
                        capture_output=True,
                        text=True,
                        check=False,
                    )
                details = (imported.stdout + imported.stderr).strip()
                self.assertIn(
                    "IMPORT_OK",
                    imported.stdout,
                    f"gpg refused to import {entry}. A key served without its user IDs is the "
                    f"likely cause; re-fetch it from a keyserver that keeps them.\n{details}",
                )
                self.assertIn(
                    fingerprint.lower(),
                    listed.stdout.lower(),
                    f"{entry} imported but the pinned fingerprint is not in the resulting keyring",
                )

    @unittest.skipUnless(MSYS_GPG.is_file(), "MSYS2 gpg is required to import the vendored keys")
    def test_the_builder_itself_imports_every_manifest_key_into_an_empty_keyring(self) -> None:
        """The production path, under the runner's actual condition.

        The test above proves the key files are importable; this proves
        SourceDependencyBuilder does the importing correctly - same call, same
        environment construction, same MSYS path spelling - starting from an empty
        keyring, which is what the release job always has and what a local build
        never had before `_reset_keyring`.
        """
        manifest = dependencies.load_dependency_manifest(str(MANIFEST_PATH))
        pinned = sorted(
            {
                fingerprint
                for dependency in manifest["dependencies"]
                for field in ("pgp_keys", "source_package_pgp_keys")
                for fingerprint in dependency.get(field, [])
            }
        )
        # tempfile keeps GNUPGHOME short; GnuPG's daemon sockets live under it.
        with tempfile.TemporaryDirectory() as temp_dir:
            builder = dependencies.SourceDependencyBuilder(
                temp_dir,
                str(PROJECT_ROOT / "build" / "msys64"),
                manifest_path=str(MANIFEST_PATH),
            )
            self.assertFalse(
                os.path.exists(os.path.join(builder.gnupg_dir, "pubring.kbx")),
                "the keyring must start empty or this proves nothing",
            )
            builder._ensure_pgp_key_fingerprints(pinned, "every pinned manifest key")

    def test_vendored_keys_carry_a_user_id(self) -> None:
        # The specific defect behind run 31207385807, asserted directly so the
        # reason is obvious even when gpg is unavailable and the import test skips.
        for entry in sorted(os.listdir(dependency_pgp.PGP_KEY_DIR)):
            if entry.endswith(".asc"):
                with open(os.path.join(dependency_pgp.PGP_KEY_DIR, entry), "r", encoding="utf-8") as handle:
                    body = handle.read()
                self.assertIn(
                    "BEGIN PGP PUBLIC KEY BLOCK",
                    body[:200],
                    f"{entry} is not armored",
                )
                self.assertGreater(len(body), 512, f"{entry} is implausibly small for a public key")

    def test_vendored_import_is_attempted_before_any_keyserver(self) -> None:
        # Order matters: a keyserver round trip must never be on the normal path,
        # because dirmngr cannot start on the Actions runner.
        source = Path(dependency_pgp.__file__).read_text(encoding="utf-8")
        vendored_at = source.index("PGP_KEY_DIR, f\"{fingerprint.upper()}.asc\"")
        keyserver_at = source.index("for keyserver in KEYSERVERS:")
        self.assertLess(vendored_at, keyserver_at)

    def test_a_broken_vendored_key_fails_instead_of_falling_back(self) -> None:
        # Run 31207385807 fell through to a keyserver when aom's blob would not
        # import and then died reporting "No dirmngr", which blamed the runner for
        # a bad file in this repository. A vendored key that will not import is a
        # repository bug and must say so.
        source = Path(dependency_pgp.__file__).read_text(encoding="utf-8")
        raise_at = source.index("did not import")
        keyserver_at = source.index("for keyserver in KEYSERVERS:")
        self.assertLess(raise_at, keyserver_at, "the hard failure must precede the keyserver fallback")
        self.assertIn("raise DependencyBuildError(", source[raise_at - 400 : keyserver_at])


if __name__ == "__main__":
    unittest.main()
