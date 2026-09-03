"""Verified acquisition of the optional LibreHardwareMonitor runtime files.

CaptureEngine's own bridge is compiled into captureengine.exe, but it drives a
third-party managed library that CaptureEngine does not build. This module
fetches exactly that library from the official release and installs the four
files the CPU/GPU bridge needs, so the integration works out of the box instead
of asking every user to assemble the directory by hand.

Nothing here trusts the network. The archive is pinned by URL, byte size and
SHA-256; only four hard-coded file names are ever taken out of it; archive
member paths are never used to build a destination path; and the three
Microsoft dependencies must still carry an Authenticode certificate after
extraction. Any mismatch raises instead of installing a file.
"""

import hashlib
import json
import os
import urllib.error
import urllib.request
import zipfile


LHM_RELEASE_TAG = "v0.9.6"
LHM_ARCHIVE_URL = (
    "https://github.com/LibreHardwareMonitor/LibreHardwareMonitor/releases/download/"
    "v0.9.6/LibreHardwareMonitor.zip"
)
LHM_ARCHIVE_NAME = "LibreHardwareMonitor-v0.9.6.zip"
# Digest and size published by GitHub for this exact release asset. Both are
# checked, so a truncated or substituted download cannot reach extraction.
LHM_ARCHIVE_SHA256 = "086d9f1b5a99e643edc2cfaaac16051685b551e4c5ac0b32a57c58c0e529c001"
LHM_ARCHIVE_SIZE = 6632626
LHM_ARCHIVE_MAXIMUM_BYTES = 32 * 1024 * 1024
# Guards against a decompression bomb: every member the allowlist accepts is a
# small managed assembly, and the largest is well under two megabytes.
LHM_MEMBER_MAXIMUM_BYTES = 8 * 1024 * 1024

# The complete CPU/GPU closure. The archive also contains a GUI executable,
# plotting libraries, storage/SMBus helpers and PDBs; none of them are extracted,
# so the plugin directory can only ever hold what the bridge actually loads.
LHM_PLUGIN_FILES = (
    "LibreHardwareMonitorLib.dll",
    "System.Memory.dll",
    "System.Numerics.Vectors.dll",
    "System.Runtime.CompilerServices.Unsafe.dll",
)
# Microsoft signs its three dependency assemblies. LibreHardwareMonitorLib.dll
# is unsigned upstream, so the pinned archive digest is its only guarantee - and
# the reason that digest is not optional.
LHM_MICROSOFT_SIGNED_FILES = frozenset(
    {
        "System.Memory.dll",
        "System.Numerics.Vectors.dll",
        "System.Runtime.CompilerServices.Unsafe.dll",
    }
)
LHM_MANIFEST_NAME = "installed-files.json"


def lhm_cache_directory() -> str:
    return os.path.join(BUILD_DIR, "lhm_cache")  # noqa: F821 - shared build namespace


def lhm_plugin_directory() -> str:
    return os.path.join(BIN_DIR, "plugins", "LibreHardwareMonitor")  # noqa: F821 - shared build namespace


def _lhm_manifest_path() -> str:
    return os.path.join(lhm_plugin_directory(), LHM_MANIFEST_NAME)


def read_lhm_installed_manifest() -> dict:
    try:
        with open(_lhm_manifest_path(), "r", encoding="utf-8") as source:
            manifest = json.load(source)
    except (OSError, ValueError):
        return {}
    return manifest if isinstance(manifest, dict) else {}


def lhm_plugin_files_are_current() -> bool:
    """Whether every pinned file is installed and still byte-identical."""
    manifest = read_lhm_installed_manifest()
    if manifest.get("release") != LHM_RELEASE_TAG or manifest.get("archive_sha256") != LHM_ARCHIVE_SHA256:
        return False
    recorded = manifest.get("files")
    if not isinstance(recorded, dict) or set(recorded) != set(LHM_PLUGIN_FILES):
        return False
    directory = lhm_plugin_directory()
    for filename, expected_digest in recorded.items():
        path = os.path.join(directory, filename)
        if not os.path.isfile(path) or sha256_file(path) != expected_digest:  # noqa: F821
            return False
    return True


def _download_pinned_archive(destination: str) -> None:
    if not LHM_ARCHIVE_URL.startswith("https://"):
        raise RuntimeError("Refusing to fetch the LibreHardwareMonitor archive over a non-HTTPS URL")
    temporary = destination + ".tmp"
    digest = hashlib.sha256()
    written = 0
    try:
        if os.path.exists(temporary):
            os.remove(temporary)
        with urllib.request.urlopen(LHM_ARCHIVE_URL, timeout=120) as response, open(temporary, "wb") as sink:
            while True:
                chunk = response.read(1024 * 1024)
                if not chunk:
                    break
                written += len(chunk)
                if written > LHM_ARCHIVE_MAXIMUM_BYTES:
                    raise RuntimeError("LibreHardwareMonitor archive exceeded its expected size; refusing it")
                digest.update(chunk)
                sink.write(chunk)
        if written != LHM_ARCHIVE_SIZE:
            raise RuntimeError(
                f"LibreHardwareMonitor archive size mismatch: expected {LHM_ARCHIVE_SIZE}, got {written}"
            )
        if digest.hexdigest().lower() != LHM_ARCHIVE_SHA256:
            raise RuntimeError(
                "LibreHardwareMonitor archive SHA-256 mismatch: "
                f"expected {LHM_ARCHIVE_SHA256}, got {digest.hexdigest().lower()}"
            )
        os.replace(temporary, destination)
    finally:
        if os.path.exists(temporary):
            os.remove(temporary)


def ensure_pinned_lhm_archive() -> str:
    """Return a cached archive path whose bytes match the pinned digest."""
    cache_directory = lhm_cache_directory()
    os.makedirs(cache_directory, exist_ok=True)
    archive_path = os.path.join(cache_directory, LHM_ARCHIVE_NAME)
    if os.path.exists(archive_path):
        if sha256_file(archive_path) == LHM_ARCHIVE_SHA256:  # noqa: F821
            log(f"Using cached {LHM_ARCHIVE_NAME}")  # noqa: F821
            return archive_path
        log(f"Cached {LHM_ARCHIVE_NAME} failed SHA-256 verification; replacing it")  # noqa: F821
        os.remove(archive_path)
    log(f"Downloading LibreHardwareMonitor {LHM_RELEASE_TAG} runtime files...")  # noqa: F821
    _download_pinned_archive(archive_path)
    log(f"Verified {LHM_ARCHIVE_NAME} SHA-256: {LHM_ARCHIVE_SHA256}")  # noqa: F821
    return archive_path


def _select_archive_members(archive: zipfile.ZipFile) -> dict:
    """Map each allowlisted file name to its single archive member.

    Selection is by base name against a fixed list, and the member's own path is
    never used for anything but reading. A duplicate base name is rejected rather
    than resolved, so an archive that carries two candidates cannot silently pick
    the wrong one.
    """
    selected: dict = {}
    for member in archive.infolist():
        if member.is_dir():
            continue
        name = os.path.basename(member.filename.replace("\\", "/"))
        if name not in LHM_PLUGIN_FILES:
            continue
        if name in selected:
            raise RuntimeError(f"LibreHardwareMonitor archive contains more than one {name}")
        if member.file_size > LHM_MEMBER_MAXIMUM_BYTES:
            raise RuntimeError(f"LibreHardwareMonitor archive member {name} is larger than expected")
        selected[name] = member
    missing = [name for name in LHM_PLUGIN_FILES if name not in selected]
    if missing:
        raise RuntimeError(f"LibreHardwareMonitor archive is missing expected file(s): {', '.join(missing)}")
    return selected


def _install_archive_members(archive_path: str, destination: str) -> dict:
    os.makedirs(destination, exist_ok=True)
    digests: dict = {}
    with zipfile.ZipFile(archive_path, "r") as archive:
        for name, member in _select_archive_members(archive).items():
            # The destination is built from the allowlist constant, never from
            # the archive, so no member path can escape this directory.
            target = os.path.join(destination, name)
            payload = archive.read(member)
            if len(payload) != member.file_size or len(payload) > LHM_MEMBER_MAXIMUM_BYTES:
                raise RuntimeError(f"LibreHardwareMonitor archive member {name} did not decompress as declared")
            if not payload.startswith(b"MZ"):
                raise RuntimeError(f"LibreHardwareMonitor archive member {name} is not a Windows executable image")
            temporary = target + ".tmp"
            with open(temporary, "wb") as sink:
                sink.write(payload)
            os.replace(temporary, target)
            if name in LHM_MICROSOFT_SIGNED_FILES and not pe_has_authenticode_certificate(target):  # noqa: F821
                os.remove(target)
                raise RuntimeError(f"LibreHardwareMonitor dependency {name} is not Authenticode signed; refusing it")
            digests[name] = hashlib.sha256(payload).hexdigest()
    return digests


def install_lhm_plugin_binaries() -> None:
    """Install the pinned LibreHardwareMonitor runtime files, verifying each."""
    destination = lhm_plugin_directory()
    archive_path = ensure_pinned_lhm_archive()
    digests = _install_archive_members(archive_path, destination)
    manifest = {
        "release": LHM_RELEASE_TAG,
        "archive_sha256": LHM_ARCHIVE_SHA256,
        "archive_url": LHM_ARCHIVE_URL,
        "files": digests,
    }
    with open(_lhm_manifest_path(), "w", encoding="utf-8") as sink:
        json.dump(manifest, sink, indent=2, sort_keys=True)
        sink.write("\n")
    log(  # noqa: F821
        f"Installed LibreHardwareMonitor {LHM_RELEASE_TAG} runtime files ({len(digests)} verified)"
    )


def ensure_lhm_plugin_binaries() -> bool:
    """Make the pinned runtime files current; returns whether they are present.

    A verification failure is fatal, because installing an unverified binary is
    exactly what this module exists to prevent. An unreachable network is not:
    the sensor integration is optional and degrades to native usage telemetry,
    so an offline build continues without it rather than failing outright.
    """
    if lhm_plugin_files_are_current():
        return True
    try:
        install_lhm_plugin_binaries()
        return True
    except OSError as error:  # urllib.error.URLError derives from OSError
        log(  # noqa: F821
            "WARNING: Could not fetch the optional LibreHardwareMonitor runtime files "
            f"({error}); the build continues without them and hardware sensors stay unavailable"
        )
        return False
