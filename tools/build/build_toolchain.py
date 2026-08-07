

def compile_custom_ffmpeg(skip_updates=False):
    """Build FFmpeg and its source dependency closure.

    Args:
        skip_updates: If True, reuse verified source-built outputs when they are current.
            Plain builds intentionally rebuild the complete source closure.
    """
    if IS_LINUX:
        log("Running on Linux/WSL - using MSYS2 FFmpeg (downloaded from repo)")
        return  # FFmpeg is downloaded as part of MSYS2 packages

    def dependency_log(message: str) -> None:
        log(message, detail="] EXEC:" in message)

    dependency_builder = SourceDependencyBuilder(
        project_root=PROJECT_ROOT,
        msys2_dir=MSYS2_DIR,
        manifest_path=FFMPEG_DEPENDENCY_MANIFEST,
        logger=dependency_log,
        runner=run_logged_subprocess,
    )
    try:
        full_source_rebuild = not skip_updates
        dependency_builder.ensure(force_rebuild=full_source_rebuild)
    except Exception as e:
        log(f"FFmpeg source dependency build failed: {e}")
        sys.exit(1)

    # Use internal builder after the complete source-built dependency closure
    # is available. The private prefix is intentionally first for pkg-config,
    # headers, and linker resolution.
    builder = FFmpegBuilder(
        root_dir=PROJECT_ROOT,
        msys_dir=MSYS2_DIR,
        install_dir=FFMPEG_DIR,
        dependency_prefix_path=dependency_builder.prefix,
    )
    builder.setup_dirs()

    # Check if FFmpeg repo exists and get current commit for tracking
    ffmpeg_repo = os.path.join(builder.repos_dir, "ffmpeg")
    commit_file = os.path.join(builder.build_root, "last_built_commit.txt")
    configuration_file = os.path.join(builder.build_root, "last_build_configuration.txt")
    current_configuration = ffmpeg_build_configuration_fingerprint()
    previous_configuration = ""
    if os.path.exists(configuration_file):
        with open(configuration_file, "r", encoding="utf-8") as f:
            previous_configuration = f.read().strip()

    # Plain builds are the explicit fresh-source path. --skip-updates retains the
    # verified-cache path and only rebuilds when required for correctness.
    needs_rebuild = full_source_rebuild

    if not os.path.exists(os.path.join(FFMPEG_DIR, "lib", "libavcodec.dll.a")):
        # No built FFmpeg - definitely need to build
        needs_rebuild = True
        log("FFmpeg not built yet - building...")
    elif previous_configuration != current_configuration:
        needs_rebuild = True
        log(
            "FFmpeg local build configuration changed "
            f"({previous_configuration[:8] if previous_configuration else 'unstamped'}"
            f" -> {current_configuration[:8]}) - rebuilding without changing the pinned source"
        )
    elif skip_updates and os.path.isdir(os.path.join(BIN_DIR, "ffmpeg")):
        # Prebuilt DLLs present and updates skipped - skip entirely
        log("FFmpeg DLLs present, --skip-updates: skipping FFmpeg build.")
        try:
            log("Copying FFmpeg DLLs...")
            ffmpeg_bin_src = os.path.join(FFMPEG_DIR, "bin")
            ffmpeg_bin_dst = os.path.join(BIN_DIR, "ffmpeg")
            runtime_deps = get_windows_ffmpeg_runtime_deps(dependency_builder.bin_dir)
            sync_ffmpeg_runtime_dlls(
                ffmpeg_bin_src,
                ffmpeg_bin_dst,
                runtime_deps,
                [dependency_builder.bin_dir],
                required_runtime_deps=True,
                private_runtime_root=dependency_builder.prefix,
            )
            log("Custom FFmpeg Setup Complete.")
        except Exception as e:
            log(f"FFmpeg Setup Failed: {e}")
            sys.exit(1)
        return
    elif not os.path.exists(ffmpeg_repo):
        # Repo doesn't exist - need to clone and build
        needs_rebuild = True
        log("FFmpeg repo not found - cloning and building...")
    else:
        # Check for updates
        update = not skip_updates
        _, updated = builder.git_clone(FFMPEG_URL, "ffmpeg", update=update, ref=FFMPEG_SOURCE_REF)

        if updated:
            needs_rebuild = True
            log("FFmpeg source updated - rebuilding...")
        elif full_source_rebuild:
            log("Plain python build.py requested a fresh FFmpeg source rebuild")
        else:
            # Check if last build matches current commit
            git_exe = builder.get_tool_path("git")
            current_commit = (
                subprocess.check_output(
                    [git_exe, "rev-parse", "HEAD"],
                    cwd=ffmpeg_repo,
                    env=builder.get_msys_env(),
                )
                .decode()
                .strip()
            )

            last_built = ""
            if os.path.exists(commit_file):
                with open(commit_file, "r") as f:
                    last_built = f.read().strip()

            if current_commit != last_built:
                needs_rebuild = True
                log(
                    f"FFmpeg commit changed ({last_built[:8] if last_built else 'none'}"
                    f" -> {current_commit[:8]}) - rebuilding..."
                )
            else:
                log(f"FFmpeg up to date ({current_commit[:8]}) - skipping build.")

    if not needs_rebuild:
        # Even if we don't rebuild, let's ensure DLLs are in bin/ffmpeg
        log("FFmpeg up to date, ensuring DLLs are in target ffmpeg dir...")
    else:
        log("Building Custom FFmpeg (this may take a while)...")
        try:
            builder.build_dependencies(update=not skip_updates)
            builder.build_ffmpeg(update=not skip_updates)

            # Save current commit hash for future comparison
            git_exe = builder.get_tool_path("git")
            current_commit = (
                subprocess.check_output(
                    [git_exe, "rev-parse", "HEAD"],
                    cwd=ffmpeg_repo,
                    env=builder.get_msys_env(),
                )
                .decode()
                .strip()
            )
            with open(commit_file, "w") as f:
                f.write(current_commit)
            with open(configuration_file, "w", encoding="utf-8") as f:
                f.write(current_configuration)
            log(f"Built FFmpeg commit: {current_commit[:8]}")
        except Exception as e:
            log(f"FFmpeg Build Failed: {e}")
            sys.exit(1)

    # Post-build (or if already built): Copy DLLs to bin/ffmpeg
    try:
        log("Copying FFmpeg DLLs...")
        ffmpeg_bin_src = os.path.join(FFMPEG_DIR, "bin")
        ffmpeg_bin_dst = os.path.join(BIN_DIR, "ffmpeg")
        runtime_deps = get_windows_ffmpeg_runtime_deps(dependency_builder.bin_dir)
        sync_ffmpeg_runtime_dlls(
            ffmpeg_bin_src,
            ffmpeg_bin_dst,
            runtime_deps,
            [dependency_builder.bin_dir],
            required_runtime_deps=True,
            private_runtime_root=dependency_builder.prefix,
        )

        log("Custom FFmpeg Setup Complete.")
    except Exception as e:
        log(f"FFmpeg Setup Failed: {e}")
        sys.exit(1)


def get_env():
    if IS_LINUX:
        compilers = get_mingw_compilers()
        if compilers is None:
            log("ERROR: MinGW compiler discovery failed on Linux host")
            sys.exit(1)
        env = os.environ.copy()
        apply_workspace_temp_environment(env)
        env["PATH"] = compilers["x64"]["bin"] + os.pathsep + env.get("PATH", "")
        env["CC"] = compilers["x64"]["cc"]
        env["CXX"] = compilers["x64"]["cxx"]
        env["PKG_CONFIG_PATH"] = ""
        env["DISABLE_CCACHE"] = "1"
        return env, compilers["x64"]["bin"]

    clang_bin = os.path.join(MSYS2_DIR, "clang64", "bin")
    usr_bin = os.path.join(MSYS2_DIR, "usr", "bin")
    env = os.environ.copy()
    apply_workspace_temp_environment(env)
    env["PATH"] = clang_bin + os.pathsep + usr_bin + os.pathsep + env.get("PATH", "")
    private_lib = os.path.join(FFMPEG_DEPENDENCY_PREFIX, "lib")
    env["PKG_CONFIG_PATH"] = os.pathsep.join(
        [
            os.path.join(private_lib, "pkgconfig"),
            os.path.join(MSYS2_DIR, "clang64", "lib", "pkgconfig"),
        ]
    )
    env.pop("CPLUS_INCLUDE_PATH", None)
    env["LIBRARY_PATH"] = os.pathsep.join([private_lib, os.path.join(MSYS2_DIR, "clang64", "lib")])
    env["CCACHE_DIR"] = os.path.join(MSYS2_DIR, ".ccache")
    env["DISABLE_CCACHE"] = "1"
    return env, clang_bin


def get_env_x86():
    if IS_LINUX:
        compilers = get_mingw_compilers()
        if compilers is None:
            log("ERROR: MinGW compiler discovery failed on Linux host")
            sys.exit(1)
        if not compilers["x86"]["cxx"]:
            raise RuntimeError("No mingw-w64 x86 compiler found on Linux host")
        env = os.environ.copy()
        apply_workspace_temp_environment(env)
        x86_bin = compilers["x86"]["bin"]
        env["PATH"] = x86_bin + os.pathsep + env.get("PATH", "")
        env["CC"] = compilers["x86"]["cc"]
        env["CXX"] = compilers["x86"]["cxx"]
        env["PKG_CONFIG_PATH"] = ""
        env["DISABLE_CCACHE"] = "1"
        return env, x86_bin

    # Cross-compile x86 from clang64: both clang64/bin (compiler) and
    # mingw32/bin (windres, pkg-config, runtime libs) must be on PATH.
    clang64_bin = os.path.join(MSYS2_DIR, "clang64", "bin")
    mingw32_bin = os.path.join(MSYS2_DIR, "mingw32", "bin")
    usr_bin = os.path.join(MSYS2_DIR, "usr", "bin")
    env = os.environ.copy()
    apply_workspace_temp_environment(env)
    env["PATH"] = clang64_bin + os.pathsep + mingw32_bin + os.pathsep + usr_bin + os.pathsep + env.get("PATH", "")
    env["PKG_CONFIG_PATH"] = os.path.join(MSYS2_DIR, "mingw32", "lib", "pkgconfig")
    env["CCACHE_DIR"] = os.path.join(MSYS2_DIR, ".ccache")
    env["DISABLE_CCACHE"] = "1"
    return env, mingw32_bin


def propagate_build_control_environment(source_env: Dict[str, str], target_env: Dict[str, str]) -> None:
    """Copy build-mode controls into independently constructed architecture environments."""
    for flag in (
        "FORCE_REBUILD",
        "CE_BUILD_JOBS",
        "CE_PRODUCTION_BUILD",
        "CE_SANITIZE",
        "CE_DISABLE_LTO",
        "DISABLE_CCACHE",
    ):
        if flag in source_env:
            target_env[flag] = source_env[flag]


def get_parallel_job_count(env: Dict[str, str], task_count: int) -> int:
    requested_workers = env.get("CE_BUILD_JOBS", "").strip()
    if requested_workers:
        try:
            workers = int(requested_workers)
        except ValueError:
            log(f"Warning: invalid CE_BUILD_JOBS={requested_workers!r}; using auto worker count")
            workers = cpu_count()
    else:
        workers = cpu_count()
    return max(1, min(workers, task_count or 1))


def ensure_dirs():
    os.makedirs(OBJ_DIR, exist_ok=True)
    os.makedirs(CAPTURE_BIN_DIR, exist_ok=True)
    os.makedirs(TESTAPP_BIN_DIR, exist_ok=True)
    os.makedirs(os.path.join(OBJ_DIR, "common"), exist_ok=True)
    os.makedirs(os.path.join(OBJ_DIR, "mediaengine"), exist_ok=True)
    os.makedirs(os.path.join(OBJ_DIR, "hook"), exist_ok=True)
    os.makedirs(os.path.join(OBJ_DIR, "captureengine"), exist_ok=True)
    os.makedirs(os.path.join(OBJ_DIR, "tests"), exist_ok=True)
    os.makedirs(os.path.join(OBJ_DIR, "external"), exist_ok=True)


def parse_dep_file(dep_path: str) -> List[str]:
    """Parse a GCC/Clang .d file."""
    deps = []
    try:
        with open(dep_path, "r") as f:
            content = f.read().replace("\\\n", " ").replace("\r", " ").strip()
            # Content is like "target: dep1 dep2 ...".
            # On Windows, drive letters also contain ':' ("C:/..."), so split on
            # the first ':' that is followed by whitespace.
            split_match = re.search(r":\s", content)
            if split_match:
                dep_part = content[split_match.end() :].strip()
                if dep_part:
                    # Preserve escaped spaces in dependency paths.
                    placeholder = "__CE_ESC_SPACE__"
                    dep_part = dep_part.replace("\\ ", placeholder)
                    files = dep_part.split()
                    deps = [f.strip().replace(placeholder, " ") for f in files if f.strip()]
    except Exception:
        pass
    return deps


@lru_cache(maxsize=None)
def compute_file_content_hash(path: str) -> str:
    with open(path, "rb") as f:
        return hashlib.md5(f.read()).hexdigest()[:16]


def looks_like_executable_image(path: str) -> bool:
    """Whether the OS could load this file as a program image.

    Toolchain probes spawn the compiler to ask it about itself. A file that is
    not a loadable image must never reach CreateProcess on Windows: the loader
    classifies it as a DOS/16-bit image and raises a hard error, which CSRSS
    answers with a modal "unsupported 16-bit application" box that blocks the
    build until somebody clicks it. Checking the magic bytes first keeps a
    truncated download, a placeholder path, or a corrupted toolchain a fast
    error instead of an indefinite stall.
    """
    resolved = path if os.path.isfile(path) else (shutil.which(path) or path)
    try:
        with open(resolved, "rb") as image:
            header = image.read(4)
    except OSError:
        return False
    # PE/COFF (also MS-DOS stubs), ELF, and shebang scripts are all launchable.
    return header[:2] in (b"MZ", b"#!") or header[:4] == b"\x7fELF"


@lru_cache(maxsize=None)
def compute_compiler_fingerprint(clang_exe: str) -> str:
    resolved_compiler = clang_exe if os.path.isfile(clang_exe) else shutil.which(clang_exe)
    if not resolved_compiler or not os.path.isfile(resolved_compiler):
        raise FileNotFoundError(f"Unable to fingerprint compiler: {clang_exe}")
    absolute_compiler = os.path.abspath(resolved_compiler)
    return f"{absolute_compiler}:{sha256_file(absolute_compiler)}"


def compute_build_signature(
    src: str, clang_exe: str, compile_flags: List[str], dependencies: Optional[List[str]] = None
) -> str:
    """Create a stable signature from source, project headers, compiler, and flags."""
    with open(src, "rb") as f:
        src_hash = hashlib.md5(f.read()).hexdigest()[:16]
    tool_fingerprint = "\n".join([compute_compiler_fingerprint(clang_exe)] + compile_flags)
    tool_hash = hashlib.md5(tool_fingerprint.encode("utf-8")).hexdigest()[:16]

    dependency_hash = hashlib.md5()
    project_root = os.path.normcase(os.path.abspath(PROJECT_ROOT))
    for dependency in sorted(set(dependencies or [])):
        absolute_dependency = os.path.normcase(os.path.abspath(dependency))
        try:
            if os.path.commonpath([project_root, absolute_dependency]) != project_root:
                continue
        except ValueError:
            continue
        if not os.path.isfile(absolute_dependency):
            continue
        dependency_hash.update(absolute_dependency.encode("utf-8", errors="surrogatepass"))
        dependency_hash.update(compute_file_content_hash(absolute_dependency).encode("ascii"))
    return f"{src_hash}:{tool_hash}:{dependency_hash.hexdigest()[:16]}"


LINK_CACHE_SCHEMA_VERSION = 1


@lru_cache(maxsize=None)
def compute_link_input_fingerprint(path: str, size: int, mtime_ns: int) -> str:
    del size, mtime_ns
    return sha256_file(path)


def fingerprint_link_input(path: str) -> str:
    stat_result = os.stat(path)
    return compute_link_input_fingerprint(os.path.abspath(path), stat_result.st_size, stat_result.st_mtime_ns)


@lru_cache(maxsize=8)
def get_link_resource_dir(clang_exe: str) -> Optional[str]:
    return detect_clang_resource_dir(os.environ.copy(), clang_exe)


def get_link_search_directories(command: List[str], clang_exe: str) -> List[str]:
    directories: List[str] = []

    def add_directory(path: str) -> None:
        normalized = os.path.abspath(path)
        if os.path.isdir(normalized) and normalized not in directories:
            directories.append(normalized)

    index = 1
    while index < len(command):
        argument = command[index]
        if argument == "-L" and index + 1 < len(command):
            add_directory(command[index + 1])
            index += 2
            continue
        if argument.startswith("-L") and len(argument) > 2:
            add_directory(argument[2:])
        if argument.startswith("--sysroot="):
            sysroot = argument.split("=", 1)[1]
            add_directory(os.path.join(sysroot, "lib"))
        index += 1

    compiler_dir = os.path.dirname(os.path.abspath(clang_exe))
    add_directory(os.path.join(compiler_dir, "..", "lib"))
    if IS_WINDOWS:
        add_directory(os.path.join(MSYS2_DIR, "clang64", "lib"))
        if is_x86_compile_command(command):
            add_directory(os.path.join(MSYS2_DIR, "mingw32", "lib"))
    resource_dir = get_link_resource_dir(clang_exe)
    if resource_dir:
        add_directory(os.path.join(resource_dir, "lib", "windows"))
    return directories


@lru_cache(maxsize=8)
def resolve_link_program_paths(compiler_exe: str) -> Tuple[str, ...]:
    """Linker binaries a link driven by this compiler can actually execute.

    Both halves matter. The sibling scan covers toolchains that ship their
    linker next to the driver, and the executable suffix belongs to the
    toolchain rather than the host: a cross toolchain staged from MSYS2
    packages carries `.exe` binaries on Linux too. The driver query covers the
    common cross layout where the real linker lives outside the driver's own
    bin directory - on Linux, guessing `<compiler_dir>/ld` would otherwise
    fingerprint the host ELF linker in /usr/bin instead of the cross linker.
    """
    resolved: List[str] = []
    compiler_dir = os.path.dirname(os.path.abspath(compiler_exe))
    for linker_name in ("ld.lld.exe", "lld-link.exe", "ld.exe", "ld.lld", "lld-link", "ld"):
        candidate = os.path.join(compiler_dir, linker_name)
        if os.path.isfile(candidate):
            resolved.append(os.path.abspath(candidate))

    if not looks_like_executable_image(compiler_exe):
        return tuple(dict.fromkeys(resolved))

    for program in ("ld", "ld.lld"):
        try:
            reported = subprocess.check_output(
                [compiler_exe, f"-print-prog-name={program}"],
                encoding="utf-8",
                errors="ignore",
                stderr=subprocess.DEVNULL,
            ).strip()
        except Exception:
            continue
        # Drivers echo the bare program name back when they cannot resolve it.
        if reported and reported != program and os.path.isfile(reported):
            resolved.append(os.path.abspath(reported))

    return tuple(dict.fromkeys(resolved))


def collect_link_dependency_paths(command: List[str], clang_exe: str, cwd: Optional[str] = None) -> List[str]:
    base_dir = os.path.abspath(cwd or PROJECT_ROOT)
    dependencies: set[str] = set()

    skip_path = False
    for argument in command[1:]:
        if skip_path:
            skip_path = False
            continue
        if argument == "-o":
            skip_path = True
            continue
        if argument.startswith("-"):
            continue
        candidate = argument if os.path.isabs(argument) else os.path.join(base_dir, argument)
        if os.path.isfile(candidate):
            dependencies.add(os.path.abspath(candidate))

    search_directories = get_link_search_directories(command, clang_exe)
    library_names = [argument[2:] for argument in command if argument.startswith("-l") and len(argument) > 2]
    implicit_runtime_names = [
        "c++",
        "c++abi",
        "unwind",
        "stdc++",
        "gcc",
        "gcc_eh",
        "winpthread",
        "pthread",
        "mingw32",
        "mingwex",
        "msvcrt",
        "ucrt",
    ]
    for library_name in library_names + implicit_runtime_names:
        for directory in search_directories:
            for filename in (
                f"lib{library_name}.a",
                f"lib{library_name}.dll.a",
                f"{library_name}.lib",
            ):
                candidate = os.path.join(directory, filename)
                if os.path.isfile(candidate):
                    dependencies.add(os.path.abspath(candidate))

    dependencies.update(resolve_link_program_paths(clang_exe))
    return sorted(dependencies, key=os.path.normcase)


def compute_link_signature(
    command: List[str], env: Dict[str, str], cwd: Optional[str] = None
) -> tuple[str, List[str]]:
    if not command:
        raise ValueError("Cannot fingerprint an empty link command")
    clang_exe = command[0]
    digest = hashlib.sha256()
    digest.update(f"schema={LINK_CACHE_SCHEMA_VERSION}\n".encode("ascii"))
    digest.update(compute_compiler_fingerprint(clang_exe).encode("utf-8", errors="surrogatepass"))
    digest.update(b"\0")
    for argument in command[1:]:
        digest.update(argument.encode("utf-8", errors="surrogatepass"))
        digest.update(b"\0")
    for variable in ("PATH", "LIB", "LIBRARY_PATH"):
        digest.update(variable.encode("ascii") + b"=")
        digest.update(env.get(variable, "").encode("utf-8", errors="surrogatepass"))
        digest.update(b"\0")

    dependencies = collect_link_dependency_paths(command, clang_exe, cwd)
    for dependency in dependencies:
        digest.update(os.path.normcase(dependency).encode("utf-8", errors="surrogatepass"))
        digest.update(b"\0")
        digest.update(fingerprint_link_input(dependency).encode("ascii"))
        digest.update(b"\0")
    return digest.hexdigest(), dependencies


def link_cache_manifest_path(output_path: str) -> str:
    return output_path + ".link-cache.json"


def load_link_cache_manifest(output_path: str) -> Optional[Dict[str, Any]]:
    try:
        with open(link_cache_manifest_path(output_path), "r", encoding="utf-8") as manifest_file:
            manifest = json.load(manifest_file)
        return manifest if isinstance(manifest, dict) else None
    except (OSError, json.JSONDecodeError):
        return None


def link_cache_manifest_matches(manifest: Optional[Dict[str, Any]], input_signature: str) -> bool:
    try:
        if not manifest or manifest.get("schema") != LINK_CACHE_SCHEMA_VERSION:
            return False
        if manifest.get("input_signature") != input_signature:
            return False
        for required_output, expected_hash in manifest["outputs"].items():
            if not os.path.isfile(required_output) or sha256_file(required_output) != expected_hash:
                return False
        return True
    except (OSError, KeyError, TypeError):
        return False


def validate_cached_link_output(output_path: str, env: Dict[str, str]) -> bool:
    try:
        manifest = load_link_cache_manifest(output_path)
        if not manifest:
            return False
        command = manifest["command"]
        cwd = manifest.get("cwd")
        current_signature, _ = compute_link_signature(command, env, cwd)
        return link_cache_manifest_matches(manifest, current_signature)
    except (OSError, ValueError, KeyError, TypeError):
        return False


def run_cached_link(
    command: List[str],
    env: Dict[str, str],
    output_path: str,
    *,
    required_outputs: Optional[List[str]] = None,
    cwd: Optional[str] = None,
    execute_command: Optional[List[str]] = None,
) -> bool:
    required = [os.path.abspath(path) for path in (required_outputs or [output_path])]
    output_path = os.path.abspath(output_path)
    manifest_path = link_cache_manifest_path(output_path)
    input_signature, dependencies = compute_link_signature(command, env, cwd)
    if env.get("FORCE_REBUILD") != "1" and link_cache_manifest_matches(
        load_link_cache_manifest(output_path), input_signature
    ):
        log(f"Link cache hit: {os.path.relpath(output_path, PROJECT_ROOT)}", detail=True)
        return False

    try:
        os.remove(manifest_path)
    except FileNotFoundError:
        pass
    # Long link lines can exceed the Windows command-line limit; callers may
    # supply a response-file equivalent. The cache signature stays keyed on the
    # full `command` so a shortened invocation cannot mask stale inputs.
    run_command(execute_command if execute_command is not None else command, env=env, cwd=cwd)
    missing_outputs = [path for path in required if not os.path.isfile(path)]
    if missing_outputs:
        raise RuntimeError("Link did not produce required output(s): " + ", ".join(missing_outputs))
    manifest = {
        "schema": LINK_CACHE_SCHEMA_VERSION,
        "command": command,
        "cwd": os.path.abspath(cwd) if cwd else None,
        "input_signature": input_signature,
        "input_count": len(dependencies),
        "outputs": {path: sha256_file(path) for path in required},
    }
    write_json_atomic(manifest_path, manifest)
    return True


def should_recompile(
    src: str,
    obj: str,
    dep_file: str,
    env: Dict[str, str],
    clang_exe: str,
    compile_flags: List[str],
) -> bool:
    # Check for force rebuild flag
    if env.get("FORCE_REBUILD") == "1":
        return True
    if not os.path.exists(obj):
        return True

    # Check source timestamp
    try:
        src_mtime = os.path.getmtime(src)
        obj_mtime = os.path.getmtime(obj)
        if src_mtime > obj_mtime:
            return True
    except OSError:
        return True  # Error accessing files, safer to recompile

    if not os.path.exists(dep_file):
        # If object exists but dep file is missing, recompile to generate dep file.
        return True
    deps = parse_dep_file(dep_file)
    if not deps:
        # Invalid/empty dep file can miss header changes and cause ABI skew.
        return True

    # Also check if source, project-header content, or compile settings changed.
    # Header content hashing prevents ABI-skewed objects even when a checkout or
    # restore gives a changed header an older timestamp.
    hash_file = obj + ".hash"
    try:
        signature = compute_build_signature(src, clang_exe, compile_flags, deps)
        if os.path.exists(hash_file):
            with open(hash_file, "r") as f:
                stored_hash = f.read().strip()
            if signature != stored_hash:
                return True  # Content and/or flags changed, recompile
        else:
            # No hash file, need to create one (first compile or old build)
            return True
    except Exception:
        # Incremental reuse is a correctness feature, not a best-effort optimization. If its
        # content signature cannot be proven, rebuild the object instead of trusting timestamps.
        return True

    # Check dependency timestamps as a cheap second signal, including toolchain
    # headers outside the project root that are intentionally not content-hashed.
    obj_mtime = os.path.getmtime(obj)
    for dep in deps:
        try:
            if os.path.exists(dep) and os.path.getmtime(dep) > obj_mtime:
                return True
        except OSError:
            return True  # Error accessing dependency, safer to recompile

    return False


def normalize_compile_command_arg(arg: str) -> str:
    """Normalize compile_commands argument paths for clangd compatibility."""
    if IS_WINDOWS:
        return arg.replace("\\", "/")
    return arg


def _find_first_existing_path(candidates: List[str]) -> Optional[str]:
    for candidate in candidates:
        if candidate and os.path.isdir(candidate):
            return candidate.replace("\\", "/")
    return None


def _append_unique_flag(arguments: List[str], flag: str) -> None:
    if flag and flag not in arguments:
        insert_at = len(arguments)
        if "-c" in arguments:
            insert_at = arguments.index("-c")
        arguments.insert(insert_at, flag)


def _has_flag_with_prefix(arguments: List[str], prefix: str) -> bool:
    return any(arg == prefix or arg.startswith(prefix) for arg in arguments)
