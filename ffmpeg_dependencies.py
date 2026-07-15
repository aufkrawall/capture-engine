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

from __future__ import annotations

import glob
import hashlib
import json
import os
import re
import shlex
import shutil
import stat
import subprocess
import urllib.request
import urllib.parse
from typing import Any, Callable, Dict, List, Mapping, Optional, Sequence, Set


CommandRunner = Callable[..., Any]
Logger = Callable[[str], None]


DEPENDENCY_BUILD_CONFIGURATION_VERSION = 2
DEPENDENCY_COMPILE_FLAGS = (
    "-march=x86-64 -mtune=generic -mguard=cf -fstack-protector-strong "
    "-D_FORTIFY_SOURCE=2 -ffunction-sections -fdata-sections"
)
DEPENDENCY_LINK_FLAGS = "-Wl,--gc-sections -Wl,--guard-cf"
DEPENDENCY_BUILD_POLICY_MARKER = "# captureproject source-dependency build policy"


class DependencyBuildError(RuntimeError):
    """Raised when a pinned FFmpeg dependency cannot be source-built safely."""


def remove_tree(path: str) -> None:
    """Remove an extracted package tree, including read-only Git objects."""

    def make_writable_and_retry(function: Callable[..., Any], entry: str, _: Any) -> None:
        os.chmod(entry, stat.S_IREAD | stat.S_IWRITE)
        function(entry)

    shutil.rmtree(path, onerror=make_writable_and_retry)


def load_dependency_manifest(manifest_path: str) -> Dict[str, Any]:
    with open(manifest_path, "r", encoding="utf-8") as manifest_file:
        manifest = json.load(manifest_file)

    if manifest.get("schema_version") != 1:
        raise DependencyBuildError(f"Unsupported FFmpeg dependency manifest schema: {manifest.get('schema_version')}")
    dependencies = manifest.get("dependencies")
    if not isinstance(dependencies, list) or not dependencies:
        raise DependencyBuildError("FFmpeg dependency manifest has no dependency list")

    names: Set[str] = set()
    for dependency in dependencies:
        if not isinstance(dependency, dict):
            raise DependencyBuildError("FFmpeg dependency manifest contains a non-object dependency")
        name = dependency.get("name")
        if not isinstance(name, str) or not name or name in names:
            raise DependencyBuildError(f"Invalid or duplicate dependency name: {name!r}")
        names.add(name)
        for key in (
            "upstream_source_url",
            "upstream_source_sha256",
            "source_package",
            "source_package_url",
            "source_package_sha256",
            "source_package_signature_url",
            "source_package_pgp_keys",
            "package_outputs",
        ):
            if not dependency.get(key):
                raise DependencyBuildError(f"Dependency {name} is missing manifest field {key}")
        for url_field in ("upstream_source_url", "source_package_url", "source_package_signature_url"):
            if not str(dependency[url_field]).startswith("https://"):
                raise DependencyBuildError(f"Dependency {name} has a non-HTTPS {url_field}")
        for hash_field in ("upstream_source_sha256", "source_package_sha256"):
            if not re.fullmatch(r"[0-9a-fA-F]{64}", str(dependency[hash_field])):
                raise DependencyBuildError(f"Dependency {name} has an invalid {hash_field}")
        for key_field in ("pgp_keys", "source_package_pgp_keys"):
            fingerprints = dependency.get(key_field, [])
            if not isinstance(fingerprints, list):
                raise DependencyBuildError(f"Dependency {name} has invalid {key_field}")
            for fingerprint in fingerprints:
                if not isinstance(fingerprint, str) or not re.fullmatch(r"[0-9a-fA-F]{40}", fingerprint):
                    raise DependencyBuildError(f"Dependency {name} has an invalid PGP fingerprint")
        package_outputs = dependency["package_outputs"]
        if not isinstance(package_outputs, list) or not all(isinstance(output, str) for output in package_outputs):
            raise DependencyBuildError(f"Dependency {name} has invalid package_outputs")

    return manifest


def dependency_manifest_fingerprint(manifest_path: str) -> str:
    """Fingerprint provenance and project-controlled dependency build policy."""
    digest = hashlib.sha256()
    with open(manifest_path, "rb") as manifest_file:
        digest.update(manifest_file.read())
    digest.update(b"\0captureproject-dependency-build-policy\0")
    digest.update(
        json.dumps(
            {
                "configuration_version": DEPENDENCY_BUILD_CONFIGURATION_VERSION,
                "compile_flags": DEPENDENCY_COMPILE_FLAGS,
                "link_flags": DEPENDENCY_LINK_FLAGS,
            },
            sort_keys=True,
            separators=(",", ":"),
        ).encode("utf-8")
    )
    return digest.hexdigest()


def inject_dependency_build_policy(pkgbuild_path: str, prefix: str, msys_lib: str) -> None:
    """Append policy after makepkg's config has replaced the caller's flags."""
    with open(pkgbuild_path, "r", encoding="utf-8") as pkgbuild_file:
        existing = pkgbuild_file.read()
    if DEPENDENCY_BUILD_POLICY_MARKER in existing:
        raise DependencyBuildError(f"Dependency PKGBUILD already contains the project build policy: {pkgbuild_path}")

    prefix_value = shlex.quote(prefix)
    msys_lib_value = shlex.quote(msys_lib)
    policy = f"""

{DEPENDENCY_BUILD_POLICY_MARKER}
_captureproject_prefix={prefix_value}
_captureproject_msys_lib={msys_lib_value}
CPPFLAGS+=" -I${{_captureproject_prefix}}/include"
CFLAGS+=" {DEPENDENCY_COMPILE_FLAGS} -I${{_captureproject_prefix}}/include"
CXXFLAGS+=" {DEPENDENCY_COMPILE_FLAGS} -I${{_captureproject_prefix}}/include"
LDFLAGS+=" {DEPENDENCY_LINK_FLAGS} -L${{_captureproject_prefix}}/lib -L${{_captureproject_msys_lib}}"
PKG_CONFIG_PATH="${{_captureproject_prefix}}/lib/pkgconfig:${{PKG_CONFIG_PATH:-}}"
CMAKE_PREFIX_PATH="${{_captureproject_prefix}}:${{CMAKE_PREFIX_PATH:-}}"
export CPPFLAGS CFLAGS CXXFLAGS LDFLAGS PKG_CONFIG_PATH CMAKE_PREFIX_PATH
"""
    with open(pkgbuild_path, "a", encoding="utf-8", newline="\n") as pkgbuild_file:
        pkgbuild_file.write(policy)


def parse_guard_cf_function_count(readobj_output: str) -> int:
    """Return the effective CFG target count or fail closed on partial metadata."""
    if "IMAGE_DLL_CHARACTERISTICS_GUARD_CF" not in readobj_output:
        raise DependencyBuildError("missing IMAGE_DLL_CHARACTERISTICS_GUARD_CF")
    table_match = re.search(
        r"^\s*GuardCFFunctionTable:\s+(0x[0-9A-Fa-f]+|\d+)",
        readobj_output,
        flags=re.MULTILINE,
    )
    count_match = re.search(
        r"^\s*GuardCFFunctionCount:\s+(0x[0-9A-Fa-f]+|\d+)",
        readobj_output,
        flags=re.MULTILINE,
    )
    if table_match is None or int(table_match.group(1), 0) == 0:
        raise DependencyBuildError("GuardCFFunctionTable is empty")
    count = int(count_match.group(1), 0) if count_match is not None else 0
    if count <= 0:
        raise DependencyBuildError("GuardCFFunctionCount is zero")
    if "CF_FUNCTION_TABLE_PRESENT" not in readobj_output:
        raise DependencyBuildError("GuardFlags lacks CF_FUNCTION_TABLE_PRESENT")
    return count


def dependency_prefix(project_root: str) -> str:
    return os.path.join(project_root, "ffmpeg_build", "dependencies", "prefix")


def manifest_runtime_dlls(manifest: Mapping[str, Any], optional: bool = False) -> List[str]:
    result: List[str] = []
    field = "optional_runtime_dlls" if optional else "runtime_dlls"
    for dependency in manifest["dependencies"]:
        for dll_name in dependency.get(field, []):
            if dll_name.lower() not in {name.lower() for name in result}:
                result.append(dll_name)
    return result


def parse_pe_import_names(objdump_output: str) -> Set[str]:
    """Extract imported DLL names from llvm-objdump -p output."""
    return {
        match.group(1).lower()
        for match in re.finditer(r"^\s*DLL Name:\s*(\S+)", objdump_output, flags=re.IGNORECASE | re.MULTILINE)
    }


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


WINDOWS_SYSTEM_DLLS = {
    "advapi32.dll",
    "avrt.dll",
    "bcrypt.dll",
    "crypt32.dll",
    "d3d11.dll",
    "d3d12.dll",
    "d3dcompiler_47.dll",
    "dxgi.dll",
    "gdi32.dll",
    "imm32.dll",
    "kernel32.dll",
    "ncrypt.dll",
    "ntdll.dll",
    "ole32.dll",
    "oleaut32.dll",
    "opengl32.dll",
    "psapi.dll",
    "runtimeobject.dll",
    "secur32.dll",
    "shell32.dll",
    "shlwapi.dll",
    "user32.dll",
    "version.dll",
    "winhttp.dll",
    "wininet.dll",
    "winmm.dll",
    "windowscodecs.dll",
    "wintrust.dll",
    "ws2_32.dll",
}


def is_windows_system_dll(dll_name: str) -> bool:
    normalized = dll_name.lower()
    return normalized in WINDOWS_SYSTEM_DLLS or normalized.startswith(("api-ms-win-", "ext-ms-win-"))


def is_path_within(path: str, root: str) -> bool:
    path_abs = os.path.normcase(os.path.abspath(path))
    root_abs = os.path.normcase(os.path.abspath(root))
    try:
        return os.path.commonpath([path_abs, root_abs]) == root_abs
    except ValueError:
        return False


def select_package_archive(
    recipe_dir: str,
    package_name: str,
    package_version: Optional[str] = None,
) -> Optional[str]:
    package_prefix = package_name + "-"
    if package_version:
        package_prefix += package_version + "-"
    matches = [
        path
        for path in glob.glob(os.path.join(recipe_dir, "**", "*.pkg.tar.zst"), recursive=True)
        if os.path.basename(path).startswith(package_prefix)
    ]
    return max(matches, key=os.path.getmtime) if matches else None


class SourceDependencyBuilder:
    """Build the Windows FFmpeg dependency closure from pinned MSYS2 recipes."""

    def __init__(
        self,
        project_root: str,
        msys2_dir: str,
        manifest_path: Optional[str] = None,
        logger: Optional[Logger] = None,
        runner: Optional[CommandRunner] = None,
    ) -> None:
        self.project_root = project_root
        self.msys2_dir = msys2_dir
        self.manifest_path = manifest_path or os.path.join(project_root, "ffmpeg_dependencies.json")
        self.manifest = load_dependency_manifest(self.manifest_path)
        self.logger = logger or (lambda message: None)
        self.runner = runner

        self.root = os.path.join(project_root, "ffmpeg_build", "dependencies")
        self.download_dir = os.path.join(self.root, "downloads")
        self.recipe_dir = os.path.join(self.root, "recipes")
        self.staging_dir = os.path.join(self.root, "staging")
        self.gnupg_dir = os.path.join(self.root, "gnupg")
        self.prefix = dependency_prefix(project_root)
        self.state_path = os.path.join(self.root, "build_state.json")

        self.bash_exe = os.path.join(msys2_dir, "usr", "bin", "bash.exe")
        self.gpg_exe = os.path.join(msys2_dir, "usr", "bin", "gpg.exe")
        self.tar_exe = os.path.join(msys2_dir, "usr", "bin", "tar.exe")
        self.clang_bin = os.path.join(msys2_dir, "clang64", "bin")
        self.usr_bin = os.path.join(msys2_dir, "usr", "bin")
        self.msys_lib = os.path.join(msys2_dir, "clang64", "lib")
        self.msys_pkgconfig = os.path.join(self.msys_lib, "pkgconfig")

    @property
    def bin_dir(self) -> str:
        return os.path.join(self.prefix, "bin")

    def _log(self, message: str) -> None:
        self.logger(f"[FFmpegDeps] {message}")

    @staticmethod
    def _unix_path(path: str) -> str:
        normalized = path.replace("\\", "/")
        if len(normalized) >= 2 and normalized[1] == ":":
            return "/" + normalized[0].lower() + normalized[2:]
        return normalized

    def _run(self, command: Sequence[str], cwd: str, env: Mapping[str, str]) -> None:
        self._log("EXEC: " + " ".join(command))
        if self.runner is not None:
            self.runner(command, cwd=cwd, env=dict(env), check=True, shell=False)
            return
        subprocess.run(list(command), cwd=cwd, env=dict(env), check=True, shell=False)

    def _build_environment(self) -> Dict[str, str]:
        env = os.environ.copy()
        path_entries = [self.clang_bin, self.usr_bin, self.bin_dir]
        env["PATH"] = os.pathsep.join(path_entries + [env.get("PATH", "")])
        env["MSYSTEM"] = "CLANG64"
        env["MINGW_ARCH"] = "clang64"
        env["MINGW_INSTALLS"] = "clang64"
        env["CHERE_INVOKING"] = "1"
        env["PKGDEST"] = self._unix_path(self.recipe_dir)
        env["SRCDEST"] = self._unix_path(self.download_dir)
        env["GNUPGHOME"] = self._unix_path(self.gnupg_dir)

        prefix_include = self._unix_path(os.path.join(self.prefix, "include"))
        prefix_lib = self._unix_path(os.path.join(self.prefix, "lib"))
        env["CFLAGS"] = f"-O3 {DEPENDENCY_COMPILE_FLAGS} -I{prefix_include}"
        env["CXXFLAGS"] = f"-O3 {DEPENDENCY_COMPILE_FLAGS} -I{prefix_include}"
        env["LDFLAGS"] = (
            f"-Wl,--gc-sections -Wl,--guard-cf -L{prefix_lib} -L{self._unix_path(self.msys_lib)}"
        )
        env["PKG_CONFIG_PATH"] = os.pathsep.join(
            [os.path.join(self.prefix, "lib", "pkgconfig"), self.msys_pkgconfig]
        )
        return env

    def _ensure_pgp_key_fingerprints(self, fingerprints: Sequence[str], subject: str) -> None:
        normalized_fingerprints = [fingerprint.lower() for fingerprint in fingerprints]
        if not normalized_fingerprints:
            return
        if not os.path.exists(self.gpg_exe):
            raise DependencyBuildError(f"Missing GPG executable required for {subject} signatures")

        os.makedirs(self.gnupg_dir, exist_ok=True)
        env = self._build_environment()

        def has_fingerprint(fingerprint: str) -> bool:
            result = subprocess.run(
                [self.gpg_exe, "--batch", "--with-colons", "--list-keys", fingerprint],
                env=env,
                capture_output=True,
                text=True,
            )
            return fingerprint in result.stdout.lower()

        keyservers = [
            "hkps://keys.openpgp.org",
            "hkps://keyserver.ubuntu.com",
        ]
        for fingerprint in normalized_fingerprints:
            if has_fingerprint(fingerprint):
                continue
            imported = False
            for keyserver in keyservers:
                self._log(f"Retrieving PGP key {fingerprint} from {keyserver}")
                result = subprocess.run(
                    [
                        self.gpg_exe,
                        "--batch",
                        "--keyserver",
                        keyserver,
                        "--recv-keys",
                        fingerprint,
                    ],
                    env=env,
                    capture_output=True,
                    text=True,
                )
                if result.returncode == 0 and has_fingerprint(fingerprint):
                    imported = True
                    break
                if result.stderr:
                    self._log(f"PGP keyserver lookup failed: {result.stderr.strip()}")
            if not imported:
                raise DependencyBuildError(
                    f"Could not retrieve and fingerprint-verify PGP key {fingerprint} for {subject}"
                )

    def _ensure_pgp_keys(self, dependency: Mapping[str, Any]) -> None:
        self._ensure_pgp_key_fingerprints(dependency.get("pgp_keys", []), str(dependency["name"]))

    @staticmethod
    def _sha256(path: str) -> str:
        digest = hashlib.sha256()
        with open(path, "rb") as input_file:
            for chunk in iter(lambda: input_file.read(1024 * 1024), b""):
                digest.update(chunk)
        return digest.hexdigest()

    def _download_file(self, url: str, destination: str) -> None:
        temporary_path = destination + ".tmp"
        self._log(f"Downloading {url}")
        try:
            with urllib.request.urlopen(url, timeout=180) as response:
                with open(temporary_path, "wb") as output_file:
                    shutil.copyfileobj(response, output_file)
            os.replace(temporary_path, destination)
        finally:
            if os.path.exists(temporary_path):
                os.remove(temporary_path)

    def _download_source_package(self, dependency: Mapping[str, Any]) -> str:
        os.makedirs(self.download_dir, exist_ok=True)
        archive_path = os.path.join(self.download_dir, dependency["source_package"])
        expected_hash = dependency["source_package_sha256"].lower()
        if os.path.exists(archive_path):
            actual_hash = self._sha256(archive_path)
            if actual_hash != expected_hash:
                raise DependencyBuildError(
                    f"Cached source package hash mismatch for {dependency['name']}: "
                    f"expected {expected_hash}, got {actual_hash}"
                )
            self._log(f"Using verified source package {dependency['source_package']}")
        else:
            self._download_file(dependency["source_package_url"], archive_path)
            actual_hash = self._sha256(archive_path)
            if actual_hash != expected_hash:
                raise DependencyBuildError(
                    f"Downloaded source package hash mismatch for {dependency['name']}: "
                    f"expected {expected_hash}, got {actual_hash}"
                )

        signature_path = archive_path + ".sig"
        if not os.path.exists(signature_path):
            self._download_file(dependency["source_package_signature_url"], signature_path)
        verify_env = self._build_environment()
        try:
            verify_detached_signature(
                self.gpg_exe,
                archive_path,
                signature_path,
                dependency["source_package_pgp_keys"],
                verify_env,
            )
        except DependencyBuildError:
            # A cached sidecar can be stale even when the content archive is
            # still valid. Refresh it once, then fail closed if it remains
            # unverifiable.
            self._download_file(dependency["source_package_signature_url"], signature_path)
            verify_detached_signature(
                self.gpg_exe,
                archive_path,
                signature_path,
                dependency["source_package_pgp_keys"],
                verify_env,
            )
        return archive_path

    def _download_upstream_source(self, dependency: Mapping[str, Any]) -> str:
        parsed_url = urllib.parse.urlparse(dependency["upstream_source_url"])
        source_name = os.path.basename(parsed_url.path)
        if not source_name:
            raise DependencyBuildError(f"Upstream source URL has no archive name for {dependency['name']}")
        archive_path = os.path.join(self.download_dir, source_name)
        expected_hash = dependency["upstream_source_sha256"].lower()
        if not os.path.exists(archive_path):
            self._download_file(dependency["upstream_source_url"], archive_path)
        actual_hash = self._sha256(archive_path)
        if actual_hash != expected_hash:
            raise DependencyBuildError(
                f"Upstream source hash mismatch for {dependency['name']}: "
                f"expected {expected_hash}, got {actual_hash}"
            )
        self._log(f"Using independently verified upstream source {source_name}")
        return archive_path

    def _extract_recipe(self, dependency: Mapping[str, Any], archive_path: str) -> str:
        dependency_recipe_root = os.path.join(self.recipe_dir, dependency["name"])
        if os.path.isdir(dependency_recipe_root):
            remove_tree(dependency_recipe_root)
        os.makedirs(dependency_recipe_root, exist_ok=True)
        env = self._build_environment()
        self._run(
            [
                self.tar_exe,
                "-xaf",
                self._unix_path(archive_path),
                "-C",
                self._unix_path(dependency_recipe_root),
            ],
            cwd=dependency_recipe_root,
            env=env,
        )
        package_builds = glob.glob(os.path.join(dependency_recipe_root, "**", "PKGBUILD"), recursive=True)
        if len(package_builds) != 1:
            raise DependencyBuildError(
                f"Expected one PKGBUILD for {dependency['name']}, found {len(package_builds)}"
            )
        inject_dependency_build_policy(
            package_builds[0],
            self._unix_path(self.prefix),
            self._unix_path(self.msys_lib),
        )
        return os.path.dirname(package_builds[0])

    def _extract_package(self, package_archive: str, dependency: Mapping[str, Any]) -> None:
        extract_root = os.path.join(self.staging_dir, dependency["name"], os.path.basename(package_archive))
        if os.path.isdir(extract_root):
            remove_tree(extract_root)
        os.makedirs(extract_root, exist_ok=True)
        env = self._build_environment()
        self._run(
            [
                self.tar_exe,
                "-xaf",
                self._unix_path(package_archive),
                "-C",
                self._unix_path(extract_root),
            ],
            cwd=extract_root,
            env=env,
        )

        target_root = os.path.join(extract_root, "clang64")
        if not os.path.isdir(target_root):
            candidates = [
                path
                for path in glob.glob(os.path.join(extract_root, "*"))
                if os.path.isdir(path) and os.path.isdir(os.path.join(path, "bin"))
            ]
            if len(candidates) != 1:
                raise DependencyBuildError(
                    f"Could not find the clang64 package root in {package_archive}"
                )
            target_root = candidates[0]
        os.makedirs(self.prefix, exist_ok=True)
        shutil.copytree(target_root, self.prefix, dirs_exist_ok=True)

    def _build_dependency(self, dependency: Mapping[str, Any]) -> List[str]:
        self._ensure_pgp_key_fingerprints(
            dependency["source_package_pgp_keys"],
            f"{dependency['name']} MSYS2 source package",
        )
        archive_path = self._download_source_package(dependency)
        recipe_dir = self._extract_recipe(dependency, archive_path)
        self._ensure_pgp_keys(dependency)
        self._download_upstream_source(dependency)
        for old_package in glob.glob(os.path.join(recipe_dir, "**", "*.pkg.tar.zst"), recursive=True):
            os.remove(old_package)

        env = self._build_environment()
        env["PKGDEST"] = self._unix_path(recipe_dir)
        command = [
            self.bash_exe,
            "-lc",
            "makepkg-mingw --nodeps --nocheck --cleanbuild --force --nocolor",
        ]
        self._run(command, cwd=recipe_dir, env=env)

        built_archives: List[str] = []
        for package_name in dependency["package_outputs"]:
            package_archive = select_package_archive(
                recipe_dir,
                package_name,
                dependency["version"],
            )
            if package_archive is None:
                raise DependencyBuildError(
                    f"makepkg did not produce {package_name} for {dependency['name']}"
                )
            self._extract_package(package_archive, dependency)
            built_archives.append(os.path.basename(package_archive))
        return built_archives

    def _is_complete(self, fingerprint: str) -> bool:
        if not os.path.isfile(self.state_path):
            return False
        try:
            with open(self.state_path, "r", encoding="utf-8") as state_file:
                state = json.load(state_file)
        except (OSError, ValueError):
            return False
        if state.get("build_fingerprint_sha256") != fingerprint:
            return False
        if state.get("build_configuration_version") != DEPENDENCY_BUILD_CONFIGURATION_VERSION:
            return False
        guard_cf_targets = state.get("guard_cf_targets")
        if not isinstance(guard_cf_targets, dict) or any(
            not isinstance(guard_cf_targets.get(dll_name), int) or guard_cf_targets[dll_name] <= 0
            for dll_name in manifest_runtime_dlls(self.manifest)
        ):
            return False
        return all(
            os.path.isfile(os.path.join(self.bin_dir, dll_name))
            for dll_name in manifest_runtime_dlls(self.manifest)
        )

    def _verify_runtime_guard_cf(self) -> Dict[str, int]:
        readobj_exe = os.path.join(self.clang_bin, "llvm-readobj.exe")
        if not os.path.isfile(readobj_exe):
            raise DependencyBuildError(f"Missing llvm-readobj required for CFG verification: {readobj_exe}")

        counts: Dict[str, int] = {}
        for dll_name in manifest_runtime_dlls(self.manifest):
            dll_path = os.path.join(self.bin_dir, dll_name)
            result = subprocess.run(
                [readobj_exe, "--file-headers", "--coff-load-config", dll_path],
                capture_output=True,
                text=True,
                check=False,
            )
            if result.returncode != 0:
                raise DependencyBuildError(
                    f"llvm-readobj failed while verifying CFG for {dll_name}: {result.stderr.strip()}"
                )
            try:
                counts[dll_name] = parse_guard_cf_function_count(result.stdout)
            except DependencyBuildError as error:
                raise DependencyBuildError(f"Source-built {dll_name} has ineffective CFG: {error}") from error
        return counts

    def _reset_outputs(self) -> None:
        for path in (self.prefix, self.recipe_dir, self.staging_dir, self.state_path):
            if os.path.isdir(path):
                remove_tree(path)
            elif os.path.exists(path):
                os.remove(path)
        os.makedirs(self.recipe_dir, exist_ok=True)
        os.makedirs(self.staging_dir, exist_ok=True)

    def ensure(self) -> str:
        """Build all pinned dependencies when the private prefix is incomplete."""
        fingerprint = dependency_manifest_fingerprint(self.manifest_path)
        if self._is_complete(fingerprint):
            try:
                self._verify_runtime_guard_cf()
            except DependencyBuildError as error:
                self._log(f"Private dependency prefix failed CFG revalidation: {error}")
            else:
                self._log(f"Private dependency prefix is current: {self.prefix}")
                return self.prefix

        self._log("Private dependency prefix is missing or stale; rebuilding all dependencies")
        self._reset_outputs()
        built_packages: List[str] = []
        for dependency in self.manifest["dependencies"]:
            self._log(f"Building {dependency['name']} {dependency['version']}")
            built_packages.extend(self._build_dependency(dependency))

        missing = [
            dll_name
            for dll_name in manifest_runtime_dlls(self.manifest)
            if not os.path.isfile(os.path.join(self.bin_dir, dll_name))
        ]
        if missing:
            raise DependencyBuildError(
                "Source builds completed without the expected runtime DLLs: " + ", ".join(sorted(missing))
            )
        guard_cf_targets = self._verify_runtime_guard_cf()

        os.makedirs(self.root, exist_ok=True)
        state = {
            "build_configuration_version": DEPENDENCY_BUILD_CONFIGURATION_VERSION,
            "build_fingerprint_sha256": fingerprint,
            "manifest_sha256": self._sha256(self.manifest_path),
            "toolchain_version": self.manifest["toolchain_version"],
            "built_packages": built_packages,
            "guard_cf_targets": guard_cf_targets,
        }
        temporary_state = self.state_path + ".tmp"
        with open(temporary_state, "w", encoding="utf-8", newline="\n") as state_file:
            json.dump(state, state_file, indent=2, sort_keys=True)
            state_file.write("\n")
        os.replace(temporary_state, self.state_path)
        self._log(f"Private source-built dependency prefix ready: {self.prefix}")
        return self.prefix


def verify_pe_import_closure(
    binary_dir: str,
    objdump_exe: str,
    logger: Optional[Logger] = None,
) -> Dict[str, Set[str]]:
    """Reject non-system imports that are not present in the shipped FFmpeg dir."""
    log_message = logger or (lambda message: None)
    binaries = sorted(glob.glob(os.path.join(binary_dir, "*.dll")))
    bundled_names = {os.path.basename(path).lower() for path in binaries}
    violations: Dict[str, Set[str]] = {}
    for binary in binaries:
        result = subprocess.run([objdump_exe, "-p", binary], capture_output=True, text=True, check=True)
        imports = parse_pe_import_names(result.stdout)
        unexpected = {
            name for name in imports if name not in bundled_names and not is_windows_system_dll(name)
        }
        if unexpected:
            violations[os.path.basename(binary)] = unexpected
    if violations:
        details = "; ".join(
            f"{binary}: {', '.join(sorted(imports))}" for binary, imports in sorted(violations.items())
        )
        raise DependencyBuildError(f"Unexpected non-system FFmpeg DLL imports: {details}")
    log_message(f"PE import closure verified for {len(binaries)} FFmpeg DLLs")
    return violations
