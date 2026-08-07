# MIT License
#
# Copyright (c) 2026 aufkrawall
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

"""PGP trust for the FFmpeg dependency closure: keyring, imports, signatures.

Split out of ffmpeg_dependencies.py, which had reached the 800-line ceiling.
Trust is anchored on the full fingerprints pinned in the manifest, never on
whoever answers a keyserver: a key is only accepted once gpg reports that
fingerprint in the keyring, and a signature only once gpg reports VALIDSIG for
one of the pinned fingerprints.
"""

from __future__ import annotations

import os
import re
import stat
import subprocess
from typing import Callable, Mapping, Sequence, Set

from tools.dependency_build_policy import DependencyBuildError

Logger = Callable[[str], None]

# Armored public keys for every fingerprint pinned in the manifest. Vendored so a
# build never depends on a keyserver (or on dirmngr, which cannot start in the
# Actions runner context). The fingerprint check still gates trust.
#
# Fetch these from a keyserver that preserves user IDs (keyserver.ubuntu.com).
# keys.openpgp.org strips UIDs it has not verified, and gpg REFUSES a key with no
# UID ("new key but contains no user ID - skipped"), so such a blob silently never
# reaches the keyring - which is what killed release run 31207385807. Note that
# `gpg --show-keys` reports the pinned fingerprint of a UID-less key quite
# happily, so it is not sufficient validation; the test suite imports each key
# into an empty keyring instead.
PGP_KEY_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "pgp-keys")

# Keyservers are a fallback only, for a fingerprint with no vendored key.
KEYSERVERS = (
    "hkps://keys.openpgp.org",
    "hkps://keyserver.ubuntu.com",
)

# Keyring files only, never the whole gnupg directory: gpg-agent and keyboxd put
# sockets there, and a locked socket would make removal fail.
KEYRING_FILES = ("pubring.kbx", "pubring.gpg", "pubring.kbx~", "trustdb.gpg")


def reset_keyring(gnupg_dir: str) -> None:
    """Discard the imported keyring so a rebuild re-imports the vendored keys.

    This closes a real blind spot rather than tidying up. The release job deletes
    `ffmpeg_build` wholesale and so always imports into an empty keyring, while a
    local rebuild reset `prefix`/`recipes`/`staging` but kept `gnupg`, so the
    keyring check short-circuited on a key imported weeks earlier and the import
    path never executed. aom's vendored blob was therefore broken for as long as
    it existed and only an Actions run could see it. Re-importing costs a few
    local file reads.
    """
    for name in KEYRING_FILES:
        path = os.path.join(gnupg_dir, name)
        if os.path.isfile(path):
            os.chmod(path, stat.S_IREAD | stat.S_IWRITE)
            os.remove(path)


def verify_detached_signature(
    gpg_exe: str,
    artifact_path: str,
    signature_path: str,
    expected_fingerprints: Sequence[str],
    env: Mapping[str, str],
) -> None:
    """Verify a detached signature and require one of the pinned full fingerprints."""
    result = subprocess.run(
        [gpg_exe, "--batch", "--no-auto-key-retrieve", "--status-fd", "1", "--verify", signature_path, artifact_path],
        env=dict(env),
        capture_output=True,
        text=True,
    )
    valid_fingerprints: Set[str] = set()
    for line in result.stdout.splitlines():
        if not line.startswith("[GNUPG:] VALIDSIG "):
            continue
        valid_fingerprints.update(re.findall(r"\b[0-9a-fA-F]{40}\b", line))
    expected = {fingerprint.lower() for fingerprint in expected_fingerprints}
    if result.returncode != 0 or not expected.intersection(fingerprint.lower() for fingerprint in valid_fingerprints):
        details = (result.stderr + " " + result.stdout).strip()
        raise DependencyBuildError(
            f"Detached signature verification failed for {os.path.basename(artifact_path)} "
            f"(expected one of {', '.join(sorted(expected))}): {details}"
        )


def ensure_key_fingerprints(
    gpg_exe: str,
    gnupg_dir: str,
    env: Mapping[str, str],
    unix_path: Callable[[str], str],
    fingerprints: Sequence[str],
    subject: str,
    log: Logger,
) -> None:
    """Make every pinned fingerprint present in the keyring, or fail closed."""
    normalized = [fingerprint.lower() for fingerprint in fingerprints]
    if not normalized:
        return
    if not os.path.exists(gpg_exe):
        raise DependencyBuildError(f"Missing GPG executable required for {subject} signatures")
    os.makedirs(gnupg_dir, exist_ok=True)

    def has_fingerprint(fingerprint: str) -> bool:
        result = subprocess.run(
            [gpg_exe, "--batch", "--with-colons", "--list-keys", fingerprint],
            env=dict(env),
            capture_output=True,
            text=True,
        )
        return fingerprint in result.stdout.lower()

    for fingerprint in normalized:
        if has_fingerprint(fingerprint):
            continue
        vendored = os.path.join(PGP_KEY_DIR, f"{fingerprint.upper()}.asc")
        if os.path.exists(vendored):
            log(f"Importing pinned PGP key {fingerprint} from tools/pgp-keys")
            imported = subprocess.run(
                [gpg_exe, "--batch", "--import", unix_path(vendored)],
                env=dict(env),
                capture_output=True,
                text=True,
            )
            if has_fingerprint(fingerprint):
                continue
            # A vendored key that will not import is a repository bug, not
            # weather, so fail here with gpg's own reason instead of falling
            # through to a keyserver. Run 31207385807 fell through and died
            # reporting "No dirmngr", which pointed at the runner environment
            # when the real fault was aom's committed blob carrying no user ID.
            # The misleading message cost a release.
            raise DependencyBuildError(
                f"Vendored PGP key {fingerprint} for {subject} did not import: "
                f"{(imported.stderr + ' ' + imported.stdout).strip()}"
            )
        retrieved = False
        for keyserver in KEYSERVERS:
            log(f"Retrieving PGP key {fingerprint} from {keyserver}")
            result = subprocess.run(
                [gpg_exe, "--batch", "--keyserver", keyserver, "--recv-keys", fingerprint],
                env=dict(env),
                capture_output=True,
                text=True,
            )
            if result.returncode == 0 and has_fingerprint(fingerprint):
                retrieved = True
                break
            if result.stderr:
                log(f"PGP keyserver lookup failed: {result.stderr.strip()}")
        if not retrieved:
            raise DependencyBuildError(
                f"Could not retrieve and fingerprint-verify PGP key {fingerprint} for {subject}"
            )
