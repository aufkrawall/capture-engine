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
PAWNIO_SETUP_NAME = "PawnIO_setup.exe"
PAWNIO_SETUP_SHA256 = "1f519a22e47187f70a1379a48ca604981c4fcf694f4e65b734aaa74a9fba3032"
PAWNIO_SETUP_URL = "https://github.com/namazso/PawnIO.Setup/releases/download/2.2.0/PawnIO_setup.exe"
PAWNIO_VERSION = "2.2.0"
DATE_ACCESSED = "2026-09-03"

LHM_PINNED_FILE_SHA256 = {
    "LibreHardwareMonitorLib.dll": "6ebc194316536ba61af5be24508ad9fcbb2ecc685e716c12e787c79530f66bf0",
    "System.Memory.dll": "d5e8e4866f9cfa66f7765660f84b210198893e55335487afe5ebda342c0e913d",
    "System.Numerics.Vectors.dll": "20c2fa81b8c70d651099d762954f285fd4f942e63b2d7217c145dab8d4b2f4c9",
    "System.Runtime.CompilerServices.Unsafe.dll": "08cbd7278b66f1e68425a82d4b97181a4130d93e3dd91831407aba7212ccdacf",
}
LHM_MANIFEST_NAME = "installed-files.json"


def lhm_cache_directory() -> str:
    return os.path.join(BUILD_DIR, "lhm_cache")  # noqa: F821 - shared build namespace


def lhm_plugin_directory() -> str:
    return os.path.join(BIN_DIR, "plugins", "LibreHardwareMonitor")  # noqa: F821 - shared build namespace


def lhm_repo_source_directory() -> str:
    return os.path.join(PROJECT_ROOT, "plugins", "LibreHardwareMonitor")  # noqa: F821 - shared build namespace


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
    if not isinstance(recorded, dict):
        return False
    if not set(LHM_PLUGIN_FILES).issubset(set(recorded)):
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


REPO_RAW_BASE_URL = "https://raw.githubusercontent.com/aufkrawall/capture-engine/main/plugins/LibreHardwareMonitor"


def _retrieve_missing_repo_file(source_dir: str, filename: str, expected_sha256: str) -> bool:
    """Retrieve a missing file from the project's GitHub repository if absent locally."""
    target_path = os.path.join(source_dir, filename)
    if os.path.isfile(target_path):
        if sha256_file(target_path).lower() == expected_sha256.lower():  # noqa: F821
            return True
    url = f"{REPO_RAW_BASE_URL}/{filename}"
    temporary = target_path + ".tmp"
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "CaptureEngine-Build/1.0"})
        with urllib.request.urlopen(req, timeout=60) as response, open(temporary, "wb") as sink:
            payload = response.read()
            sink.write(payload)
        digest = hashlib.sha256(payload).hexdigest().lower()
        if digest != expected_sha256.lower():
            if os.path.exists(temporary):
                os.remove(temporary)
            log(f"WARNING: Retrieved {filename} SHA-256 mismatch (got {digest}, expected {expected_sha256})")  # noqa: F821
            return False
        os.replace(temporary, target_path)
        log(f"Retrieved {filename} from capture-engine repository ({len(payload)} bytes)")  # noqa: F821
        return True
    except (OSError, urllib.error.URLError) as error:
        if os.path.exists(temporary):
            os.remove(temporary)
        log(f"Note: Could not retrieve {filename} from repository ({error})")  # noqa: F821
        return False


def _install_from_repo_source(source_dir: str, destination: str) -> dict:
    for filename in LHM_PLUGIN_FILES:
        src = os.path.join(source_dir, filename)
        if not os.path.isfile(src):
            expected = LHM_PINNED_FILE_SHA256.get(filename)
            if expected:
                _retrieve_missing_repo_file(source_dir, filename, expected)
        if not os.path.isfile(src):
            return {}

    pawnio_src = os.path.join(source_dir, PAWNIO_SETUP_NAME)
    if not os.path.isfile(pawnio_src):
        _retrieve_missing_repo_file(source_dir, PAWNIO_SETUP_NAME, PAWNIO_SETUP_SHA256)

    os.makedirs(destination, exist_ok=True)
    digests = {}
    for filename in LHM_PLUGIN_FILES:
        src = os.path.join(source_dir, filename)
        digest = sha256_file(src)  # noqa: F821
        expected = LHM_PINNED_FILE_SHA256.get(filename)
        if expected and digest.lower() != expected.lower():
            raise RuntimeError(f"Repository source file {filename} SHA-256 mismatch: expected {expected}, got {digest}")
        dst = os.path.join(destination, filename)
        if not safe_copy_file(src, dst):  # noqa: F821
            raise RuntimeError(f"Failed to copy {filename} to {destination}")
        if filename in LHM_MICROSOFT_SIGNED_FILES and not pe_has_authenticode_certificate(dst):  # noqa: F821
            os.remove(dst)
            raise RuntimeError(f"Dependency {filename} is not Authenticode signed; refusing it")
        digests[filename] = digest.lower()

    pawnio_src = os.path.join(source_dir, PAWNIO_SETUP_NAME)
    if os.path.isfile(pawnio_src):
        pawnio_digest = sha256_file(pawnio_src)  # noqa: F821
        if pawnio_digest.lower() != PAWNIO_SETUP_SHA256.lower():
            raise RuntimeError(
                f"PawnIO setup binary SHA-256 mismatch: expected {PAWNIO_SETUP_SHA256}, got {pawnio_digest}"
            )
        pawnio_dst = os.path.join(destination, PAWNIO_SETUP_NAME)
        if not safe_copy_file(pawnio_src, pawnio_dst):  # noqa: F821
            raise RuntimeError(f"Failed to copy {PAWNIO_SETUP_NAME} to {destination}")
        if not pe_has_authenticode_certificate(pawnio_dst):  # noqa: F821
            os.remove(pawnio_dst)
            raise RuntimeError(f"PawnIO installer {PAWNIO_SETUP_NAME} is not Authenticode signed; refusing it")
        digests[PAWNIO_SETUP_NAME] = pawnio_digest.lower()

    return digests


def install_lhm_plugin_binaries() -> None:
    """Install the pinned LibreHardwareMonitor runtime files, verifying each."""
    destination = lhm_plugin_directory()
    repo_source = lhm_repo_source_directory()
    digests = _install_from_repo_source(repo_source, destination)
    if not digests:
        archive_path = ensure_pinned_lhm_archive()
        digests = _install_archive_members(archive_path, destination)
    manifest = {
        "archive_sha256": LHM_ARCHIVE_SHA256,
        "archive_url": LHM_ARCHIVE_URL,
        "date_accessed": DATE_ACCESSED,
        "files": digests,
        "pawnio_setup_sha256": PAWNIO_SETUP_SHA256,
        "pawnio_setup_url": PAWNIO_SETUP_URL,
        "pawnio_version": PAWNIO_VERSION,
        "release": LHM_RELEASE_TAG,
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
