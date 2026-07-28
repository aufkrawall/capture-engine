

def get_linux_msys2_packages() -> List[str]:
    return [
        LINUX_MSYS2_PACKAGES[0],
        LINUX_MSYS2_PACKAGES[1],
        LINUX_MSYS2_PACKAGES[2],
        get_linux_msys2_gtest_package_name(),
        *LINUX_MSYS2_PACKAGES[3:],
    ]


def get_linux_msys2_required_sentinels() -> List[str]:
    return LINUX_MSYS2_REQUIRED_SENTINELS + [os.path.join(get_linux_msys2_gtest_subdir("x64"), "lib", "libgtest.a")]


def get_linux_msys2_repo_url_for_package(pkg: str) -> str:
    if pkg.startswith("mingw-w64-clang-"):
        return MSYS2_REPO_URL
    if pkg.startswith("mingw-w64-x86_64-"):
        return MSYS2_MINGW64_REPO_URL
    return MSYS2_REPO_URL


def extract_linux_msys2_package(pkg_path: str, destination: str) -> None:
    tar_exe = shutil.which("tar")
    if not tar_exe:
        raise RuntimeError("Linux MSYS2 package extraction requires a 'tar' executable")

    extract_cmds = [
        [tar_exe, "-xf", pkg_path, "-C", destination],
        [tar_exe, "--zstd", "-xf", pkg_path, "-C", destination],
    ]
    errors = []
    for cmd in extract_cmds:
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode == 0:
            return
        errors.append(result.stderr.strip() or result.stdout.strip() or "unknown tar error")

    raise RuntimeError(f"Failed to extract {os.path.basename(pkg_path)}: {' | '.join(errors)}")


def linux_msys2_packages_complete(msys_linux_dir: str) -> bool:
    clang64_dir = os.path.join(msys_linux_dir, "clang64")
    include_dir = os.path.join(clang64_dir, "include")
    if not (os.path.isdir(include_dir) and os.listdir(include_dir)):
        return False

    for rel_path in get_linux_msys2_required_sentinels():
        if not os.path.exists(os.path.join(msys_linux_dir, rel_path)):
            return False

    return True


def download_msys2_packages_for_linux():
    """Download minimal MSYS2 packages for Linux cross-compilation.

    Only downloads Windows-specific libraries/headers not available in Arch repos.
    Toolchain (clang, mingw-w64) should be installed via system pacman.

    OPTIMIZATION: Uses marker file to skip extraction if already done.
    """
    if not IS_LINUX:
        return

    msys_linux_dir = os.path.join(BUILD_DIR, "msys64_linux")
    clang64_dir = os.path.join(msys_linux_dir, "clang64")
    marker_file = os.path.join(msys_linux_dir, ".packages_installed")

    # Check if packages already extracted (marker file + required sentinels present).
    if os.path.exists(marker_file) and linux_msys2_packages_complete(msys_linux_dir):
        log("MSYS2 packages already installed - skipping setup")
        return msys_linux_dir

    if os.path.exists(marker_file):
        log("MSYS2 package tree incomplete - refreshing setup")

    log("Setting up MSYS2 packages for Linux cross-compilation...")
    os.makedirs(msys_linux_dir, exist_ok=True)
    os.makedirs(os.path.join(clang64_dir, "include"), exist_ok=True)
    os.makedirs(os.path.join(clang64_dir, "lib"), exist_ok=True)

    repo_html_cache: Dict[str, str] = {}

    for pkg in get_linux_msys2_packages():
        try:
            repo_url = get_linux_msys2_repo_url_for_package(pkg)
            repo_html = repo_html_cache.get(repo_url)
            if repo_html is None:
                req = urllib.request.Request(repo_url, headers={"User-Agent": "Mozilla/5.0"})
                with urllib.request.urlopen(req, timeout=30) as response:
                    repo_html = response.read().decode("utf-8", errors="replace")
                repo_html_cache[repo_url] = repo_html

            # Find package file in repo (with version)
            log(f"Checking for {pkg} in MSYS2 repo...")
            pattern = rf'href="({re.escape(pkg)}[^"]*\.pkg\.tar\.zst)"'
            match = re.search(pattern, repo_html)
            if not match:
                raise RuntimeError(f"Could not find package {pkg} in {repo_url}")

            pkg_file = match.group(1)
            pkg_url = f"{repo_url}/{pkg_file}"
            pkg_path = os.path.join(msys_linux_dir, pkg_file)

            # Download if not exists
            if not os.path.exists(pkg_path):
                log(f"Downloading {pkg_file}...")
                urllib.request.urlretrieve(pkg_url, pkg_path)

            # Extract package
            log(f"Extracting {pkg_file}...")
            extract_linux_msys2_package(pkg_path, msys_linux_dir)

            log(f"Package {pkg} ready")

        except Exception as e:
            raise RuntimeError(f"Failed to prepare Linux MSYS2 package {pkg}: {e}") from e

    verify_msys2_ffmpeg_build_deps(msys_linux_dir)

    # Create marker file to skip on future builds
    with open(marker_file, "w") as f:
        f.write(datetime.datetime.now().isoformat())

    log("MSYS2 packages setup complete for Linux")
    return msys_linux_dir


def wsl_path_to_windows(path):
    r"""Convert WSL path (/mnt/c/...) to Windows path (C:\...)"""
    if IS_LINUX and path.startswith("/mnt/"):
        # /mnt/c/path -> C:\path
        parts = path.split("/")
        if len(parts) >= 3:
            drive = parts[2].upper()  # 'c' -> 'C'
            rest = "\\".join(parts[3:])
            return f"{drive}:\\{rest}"
    return path


def get_linux_msys2_dir():
    """Get the MSYS2 directory for Linux builds."""
    if not IS_LINUX:
        return MSYS2_DIR

    # Prefer existing MSYS2 from Windows if available
    if os.path.exists(MSYS2_DIR):
        return MSYS2_DIR

    # Otherwise use downloaded minimal setup
    return os.path.join(BUILD_DIR, "msys64_linux")


def get_linux_msys2_lib_dir(arch: str = "x64") -> str:
    msys2_dir = get_linux_msys2_dir()
    if arch == "x86":
        return os.path.join(msys2_dir, "mingw32", "lib")
    return os.path.join(msys2_dir, "clang64", "lib")


def get_linux_msys2_gtest_lib_dir(arch: str = "x64") -> str:
    return os.path.join(get_linux_msys2_dir(), get_linux_msys2_gtest_subdir(arch), "lib")


def get_host_msys2_dir():
    return get_linux_msys2_dir() if IS_LINUX else MSYS2_DIR


def resolve_msys2_gtest_link_inputs(lib_dir: str, *, prefer_static: bool = False) -> List[str]:
    """Resolve GoogleTest link inputs from an extracted MSYS2 MinGW lib dir."""

    def pick(candidates: List[str]) -> Optional[str]:
        for candidate in candidates:
            if os.path.exists(candidate):
                return candidate
        return None

    if prefer_static:
        gtest_main = pick(
            [
                os.path.join(lib_dir, "libgtest_main.a"),
                os.path.join(lib_dir, "libgtest_main.dll.a"),
            ]
        )
        gtest = pick(
            [
                os.path.join(lib_dir, "libgtest.a"),
                os.path.join(lib_dir, "libgtest.dll.a"),
            ]
        )
    else:
        gtest_main = pick(
            [
                os.path.join(lib_dir, "libgtest_main.dll.a"),
                os.path.join(lib_dir, "libgtest_main.a"),
            ]
        )
        gtest = pick(
            [
                os.path.join(lib_dir, "libgtest.dll.a"),
                os.path.join(lib_dir, "libgtest.a"),
            ]
        )

    if gtest_main and gtest:
        return [gtest_main, gtest]

    missing = []
    if not gtest_main:
        missing.append("libgtest_main(.dll.a/.a)")
    if not gtest:
        missing.append("libgtest(.dll.a/.a)")
    raise RuntimeError(f"Missing MSYS2 GoogleTest libraries in {lib_dir}: {', '.join(missing)}")


# ============================================================================
# ImGui Setup - REMOVED: No longer using ImGui
# Custom overlay renderer (custom_overlay) replaces ImGui
# ============================================================================

# --- FFmpeg Configuration ---
FFMPEG_URL = "https://git.ffmpeg.org/ffmpeg.git"
FFNVCODEC_URL = "https://git.videolan.org/git/ffmpeg/nv-codec-headers.git"
FFMPEG_DEPENDENCY_MANIFEST = os.path.join(PROJECT_ROOT, "tools", "ffmpeg_dependencies.json")
FFMPEG_DEPENDENCY_MANIFEST_DATA = load_dependency_manifest(FFMPEG_DEPENDENCY_MANIFEST)
FFMPEG_DEPENDENCY_PREFIX = dependency_prefix(PROJECT_ROOT)
FFMPEG_RUNTIME_DLL_PATTERNS = [
    "avcodec-*.dll",
    "avformat-*.dll",
    "avutil-*.dll",
    "swresample-*.dll",
    "swscale-*.dll",
]
WINDOWS_FFMPEG_RUNTIME_DEPS = manifest_runtime_dlls(FFMPEG_DEPENDENCY_MANIFEST_DATA)
WINDOWS_FFMPEG_OPTIONAL_RUNTIME_DEPS = manifest_runtime_dlls(FFMPEG_DEPENDENCY_MANIFEST_DATA, optional=True)
WINDOWS_SANITIZER_RUNTIME_DEPS = [
    "libclang_rt.asan_dynamic-x86_64.dll",
    "libc++.dll",  # Required by ASan runtime; shared with FFmpeg via libvpl-2.dll
]
LINUX_FFMPEG_RUNTIME_DEPS = [
    "libbz2-1.dll",
    "libxml2-16.dll",
    "libmodplug-1.dll",
    "libgme.dll",
    "libiconv-2.dll",
    "libc++.dll",
    "libva.dll",
    "libva_win32.dll",
    "libvpl-2.dll",
    "libopus-0.dll",
    "libwinpthread-1.dll",
    "libgcc_s_seh-1.dll",
    "libstdc++-6.dll",
]


def get_windows_ffmpeg_runtime_deps(runtime_bin: str) -> List[str]:
    """Return mandatory deps plus libcharset only when libiconv imports it."""
    deps = list(WINDOWS_FFMPEG_RUNTIME_DEPS)
    libcharset_name = "libcharset-1.dll"
    libcharset_path = os.path.join(runtime_bin, libcharset_name)
    if not os.path.exists(libcharset_path):
        return deps

    objdump_exe = os.path.join(MSYS2_DIR, "clang64", "bin", "llvm-objdump.exe")
    libiconv_path = os.path.join(runtime_bin, "libiconv-2.dll")
    if not os.path.exists(objdump_exe) or not os.path.exists(libiconv_path):
        raise RuntimeError("Cannot determine whether libiconv imports optional libcharset-1.dll")
    result = subprocess.run([objdump_exe, "-p", libiconv_path], capture_output=True, text=True, check=True)
    if libcharset_name.lower() in parse_pe_import_names(result.stdout):
        deps.append(libcharset_name)
        log("[FFmpeg] libiconv imports libcharset-1.dll; including the optional runtime DLL")
    else:
        log("[FFmpeg] libiconv does not import libcharset-1.dll; leaving it out of the bundle")
    return deps


def get_windows_ffmpeg_pkg_config_path(existing: str = "") -> str:
    paths = [
        os.path.join(FFMPEG_DEPENDENCY_PREFIX, "lib", "pkgconfig"),
        os.path.join(FFMPEG_DIR, "lib", "pkgconfig"),
    ]
    if existing:
        paths.append(existing)
    return os.pathsep.join(paths)


def to_unix(p):
    """Convert Windows path to MSYS2 Unix path."""
    p = p.replace("\\", "/")
    if len(p) >= 2 and p[1] == ":":
        drive = p[0].lower()
        return "/" + drive + p[2:]
    return p


def get_ffmpeg_runtime_dlls(ffmpeg_bin_src):
    dlls = []
    seen = set()
    for pattern in FFMPEG_RUNTIME_DLL_PATTERNS:
        for dll in sorted(glob.glob(os.path.join(ffmpeg_bin_src, pattern))):
            name = os.path.basename(dll).lower()
            if name in seen:
                continue
            seen.add(name)
            dlls.append(dll)
    return dlls


def resolve_ffmpeg_runtime_dll_names(ffmpeg_bin_src):
    """Resolve the current versioned FFmpeg DLL names from the install tree."""
    resolved = {}
    for prefix in ("avcodec", "avformat", "avutil", "swresample", "swscale"):
        matches = sorted(glob.glob(os.path.join(ffmpeg_bin_src, f"{prefix}-*.dll")))
        if not matches:
            raise RuntimeError(f"Missing FFmpeg runtime DLL for {prefix} in {ffmpeg_bin_src}")

        # A previous FFmpeg install can leave an older major-version DLL behind.
        # Keep the highest numeric suffix and let the destination sync remove the
        # stale copy instead of linking against a hard-coded historical version.
        def version_key(path):
            match = re.search(r"-(\d+)\.dll$", os.path.basename(path), re.IGNORECASE)
            return (int(match.group(1)) if match else -1, os.path.basename(path).lower())

        selected = max(matches, key=version_key)
        if len(matches) > 1:
            log(
                f"[FFmpeg] Multiple {prefix} runtime DLLs found; selecting {os.path.basename(selected)} "
                f"and removing stale versions from the bundle"
            )
        resolved[prefix] = os.path.basename(selected)
    return resolved


def sync_ffmpeg_runtime_dlls(
    ffmpeg_bin_src,
    ffmpeg_bin_dst,
    runtime_deps,
    extra_search_dirs,
    required_runtime_deps=False,
    private_runtime_root=None,
):
    resolved_names = resolve_ffmpeg_runtime_dll_names(ffmpeg_bin_src)
    ffmpeg_dlls = [os.path.join(ffmpeg_bin_src, name) for name in resolved_names.values()]

    dep_sources = {}
    for dep in runtime_deps:
        for search_dir in extra_search_dirs:
            src = os.path.join(search_dir, dep)
            if os.path.exists(src):
                dep_sources[dep] = src
                break

    missing_deps = [dep for dep in runtime_deps if dep not in dep_sources]
    if missing_deps:
        if required_runtime_deps:
            raise RuntimeError(
                "Required source-built FFmpeg runtime dependencies are missing: " + ", ".join(sorted(missing_deps))
            )
        log(
            "[FFmpeg] Optional runtime dependencies not found in configured search paths: "
            + ", ".join(sorted(missing_deps))
        )

    if private_runtime_root:
        for dependency, source in dep_sources.items():
            if not is_path_within(source, private_runtime_root):
                raise RuntimeError(
                    f"FFmpeg runtime dependency {dependency} resolved outside the private source prefix: {source}"
                )

    keep_names = {os.path.basename(dll).lower() for dll in ffmpeg_dlls}
    keep_names.update(dep.lower() for dep in dep_sources)

    os.makedirs(ffmpeg_bin_dst, exist_ok=True)
    for existing in glob.glob(os.path.join(ffmpeg_bin_dst, "*.dll")):
        existing_name = os.path.basename(existing).lower()
        if existing_name in keep_names:
            continue
        if safe_delete_file(existing):
            log(f"Removed stale FFmpeg/runtime DLL {os.path.basename(existing)}")
        else:
            log(f"WARNING: Could not remove stale FFmpeg/runtime DLL {os.path.basename(existing)}")

    copied_count = 0
    cached_count = 0
    for dll in ffmpeg_dlls:
        dst = os.path.join(ffmpeg_bin_dst, os.path.basename(dll))
        success, copied = safe_copy_file_if_changed(dll, dst)
        if not success:
            raise RuntimeError(f"Failed to copy {os.path.basename(dll)} to {ffmpeg_bin_dst}")
        copied_count += int(copied)
        cached_count += int(not copied)
        log(f"{'Copied' if copied else 'Kept'} {os.path.basename(dll)} in ffmpeg dir", detail=True)

    for dep in runtime_deps:
        src = dep_sources.get(dep)
        if not src:
            continue
        dst = os.path.join(ffmpeg_bin_dst, dep)
        success, copied = safe_copy_file_if_changed(src, dst)
        if not success:
            raise RuntimeError(f"Failed to copy runtime dependency {dep} to {ffmpeg_bin_dst}")
        copied_count += int(copied)
        cached_count += int(not copied)
        log(f"{'Copied' if copied else 'Kept'} runtime dep {dep} in ffmpeg dir", detail=True)

    log(f"FFmpeg runtime sync: {copied_count} copied, {cached_count} unchanged")

    if private_runtime_root:
        objdump_exe = os.path.join(MSYS2_DIR, "clang64", "bin", "llvm-objdump.exe")
        if not os.path.exists(objdump_exe):
            raise RuntimeError(f"Missing LLVM PE import checker: {objdump_exe}")
        verify_pe_import_closure(ffmpeg_bin_dst, objdump_exe, logger=log)


def remove_redundant_root_runtime_dlls(root_dir: str, runtime_deps: List[str]) -> None:
    for dep_name in sorted(set(runtime_deps)):
        dep_path = os.path.join(root_dir, dep_name)
        if not os.path.exists(dep_path):
            continue
        if safe_delete_file(dep_path):
            log(f"Removed redundant root runtime DLL {dep_name}")
        else:
            log(f"WARNING: Could not remove redundant root runtime DLL {dep_name}")


def sync_windows_sanitizer_runtime_dlls(target_dir: str) -> None:
    if IS_LINUX:
        return

    clang_bin = os.path.join(MSYS2_DIR, "clang64", "bin")
    os.makedirs(target_dir, exist_ok=True)
    for dll_name in WINDOWS_SANITIZER_RUNTIME_DEPS:
        if dll_name.lower() == "libclang_rt.asan_dynamic-x86_64.dll":
            # ASan is a developer-only build runtime.  It remains resolved
            # from the MSYS2 toolchain during sanitizer tests and must never
            # become a product-bundle dependency copied from clang64/bin.
            log("Leaving the MSYS2 ASan developer runtime out of the product bundle")
            continue
        if dll_name.lower() == "libc++.dll":
            src = os.path.join(FFMPEG_DEPENDENCY_PREFIX, "bin", dll_name)
        else:
            src = os.path.join(clang_bin, dll_name)
        if not os.path.exists(src):
            raise RuntimeError(f"Missing sanitizer runtime DLL: {src}")
        dst = os.path.join(target_dir, dll_name)
        if not safe_copy_file(src, dst):
            raise RuntimeError(f"Failed to copy sanitizer runtime DLL {dll_name} to {target_dir}")
        log(f"Copied sanitizer runtime DLL {dll_name}")


def remove_stale_windows_sanitizer_runtime_dlls(target_dir: str) -> None:
    if IS_LINUX:
        return

    for dll_name in WINDOWS_SANITIZER_RUNTIME_DEPS:
        dll_path = os.path.join(target_dir, dll_name)
        if not os.path.exists(dll_path):
            continue
        if safe_delete_file(dll_path):
            log(f"Removed stale sanitizer runtime DLL {dll_name}")
        else:
            log(f"WARNING: Could not remove stale sanitizer runtime DLL {dll_name}")


def get_msys_license_root():
    if IS_LINUX:
        return os.path.join(get_linux_msys2_dir(), "clang64", "share", "licenses")
    return os.path.join(FFMPEG_DEPENDENCY_PREFIX, "share", "licenses")


def get_amf_headers_license_path():
    return os.path.join(
        get_host_msys2_dir(),
        "clang64",
        "share",
        "licenses",
        "amf-headers",
        "LICENSE",
    )
