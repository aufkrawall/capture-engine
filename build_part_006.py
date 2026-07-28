

def copy_bundled_runtime_licenses(licenses_dst, ffmpeg_bin_dst):
    if not os.path.isdir(licenses_dst) or not os.path.isdir(ffmpeg_bin_dst):
        return

    license_root = get_msys_license_root()
    build_time_license_specs = [
        (get_amf_headers_license_path(), "MIT_AMF-Headers.txt"),
    ]
    license_specs = [
        (
            "libiconv-2.dll",
            [
                (
                    os.path.join(license_root, "libiconv", "COPYING.LIB"),
                    "LGPLv2.1_libiconv.txt",
                ),
            ],
        ),
        (
            "libcharset-1.dll",
            [
                (
                    os.path.join(license_root, "libiconv", "libcharset", "COPYING.LIB"),
                    "LGPLv2.1_libcharset.txt",
                ),
            ],
        ),
        (
            "libbz2-1.dll",
            [
                (
                    os.path.join(license_root, "bzip2", "LICENSE"),
                    "BSL-1.0_bzip2.txt",
                ),
            ],
        ),
        (
            "libgme.dll",
            [
                (
                    os.path.join(license_root, "libgme", "license.txt"),
                    "Expat_libgme.txt",
                ),
            ],
        ),
        (
            "libmodplug-1.dll",
            [
                (
                    os.path.join(license_root, "libmodplug", "LICENSE"),
                    "LGPLv2.1_libmodplug.txt",
                ),
            ],
        ),
        (
            "libvpl-2.dll",
            [
                (
                    os.path.join(license_root, "libvpl", "LICENSE"),
                    "MIT_libvpl.txt",
                ),
            ],
        ),
        (
            "libopus-0.dll",
            [
                (
                    os.path.join(license_root, "opus", "COPYING"),
                    "BSD-3-Clause_libopus.txt",
                ),
            ],
        ),
        (
            "libc++.dll",
            [
                (
                    os.path.join(license_root, "libc++", "LICENSE"),
                    "Apache-2.0_with_LLVM-exception_llvm-runtime.txt",
                ),
            ],
        ),
        (
            "libunwind.dll",
            [
                (
                    os.path.join(license_root, "libunwind", "LICENSE"),
                    "Apache-2.0_with_LLVM-exception_llvm-runtime.txt",
                ),
            ],
        ),
        (
            "libva.dll",
            [
                (
                    os.path.join(license_root, "libva", "COPYING"),
                    "MIT_libva.txt",
                ),
            ],
        ),
        (
            "libva_win32.dll",
            [
                (
                    os.path.join(license_root, "libva", "COPYING"),
                    "MIT_libva.txt",
                ),
            ],
        ),
        (
            "libSvtAv1Enc-4.dll",
            [
                (
                    os.path.join(license_root, "svt-av1", "LICENSE"),
                    "BSD-3-Clause-Clear_svt-av1.txt",
                ),
                (
                    os.path.join(license_root, "svt-av1", "PATENTS.md"),
                    "AOM-Patent-License-1.0_svt-av1.txt",
                ),
            ],
        ),
        (
            "libaom.dll",
            [
                (
                    os.path.join(license_root, "aom", "LICENSE"),
                    "BSD-2-Clause_libaom.txt",
                ),
                (
                    os.path.join(license_root, "aom", "PATENTS"),
                    "AOM-Patent-License-1.0_libaom.txt",
                ),
            ],
        ),
        (
            "libwinpthread-1.dll",
            [
                (
                    os.path.join(license_root, "libwinpthread", "COPYING"),
                    "Mingw-w64-runtime_libwinpthread.txt",
                ),
            ],
        ),
        (
            "libgcc_s_seh-1.dll",
            [
                (
                    os.path.join(license_root, "crt", "COPYING.MinGW-w64-runtime.txt"),
                    "Mingw-w64-runtime_libgcc.txt",
                ),
            ],
        ),
        (
            "libstdc++-6.dll",
            [
                (
                    os.path.join(license_root, "crt", "COPYING.MinGW-w64-runtime.txt"),
                    "Mingw-w64-runtime_libstdcxx.txt",
                ),
            ],
        ),
    ]

    bundled_dlls = {entry.lower() for entry in os.listdir(ffmpeg_bin_dst) if entry.lower().endswith(".dll")}
    copied_license_names = set()
    mapped_runtime_dlls = set()

    # AMF is compiled from SDK headers but loads the driver-provided runtime at
    # run time, so no bundled DLL exists that could trigger this notice.
    for src, dst_name in build_time_license_specs:
        if not os.path.exists(src):
            raise RuntimeError(f"Missing bundled build-time license source: {src}")
        dst = os.path.join(licenses_dst, dst_name)
        if not safe_copy_file(src, dst):
            raise RuntimeError(f"Failed to copy bundled build-time license {dst_name}")
        copied_license_names.add(dst_name.lower())
        log(f"Copied bundled build-time license {dst_name}")

    for dll_name, outputs in license_specs:
        dll_name_lower = dll_name.lower()
        if dll_name_lower not in bundled_dlls:
            continue
        mapped_runtime_dlls.add(dll_name_lower)
        for src, dst_name in outputs:
            if not os.path.exists(src):
                raise RuntimeError(f"Missing bundled runtime license source: {src}")
            dst_name_lower = dst_name.lower()
            if dst_name_lower in copied_license_names:
                continue
            dst = os.path.join(licenses_dst, dst_name)
            if not safe_copy_file(src, dst):
                raise RuntimeError(f"Failed to copy bundled runtime license {dst_name}")
            copied_license_names.add(dst_name_lower)
            log(f"Copied bundled runtime license {dst_name}")

    known_ffmpeg_prefixes = (
        "avcodec-",
        "avdevice-",
        "avfilter-",
        "avformat-",
        "avutil-",
        "swresample-",
        "swscale-",
    )
    for dll_name in sorted(bundled_dlls):
        if dll_name.startswith(known_ffmpeg_prefixes):
            continue
        if dll_name in mapped_runtime_dlls:
            continue
        log(f"WARNING: Bundled runtime DLL {dll_name} has no configured license copy rule")


class FFmpegBuilder:
    def __init__(self, root_dir, msys_dir, install_dir, license_mode="gpl", dependency_prefix_path=None):
        self.root = root_dir
        self.msys = msys_dir
        self.install_dir = install_dir
        self.license_mode = license_mode  # 'gpl' or 'lgpl'

        self.build_root = os.path.join(self.root, "ffmpeg_build")
        self.repos_dir = os.path.join(self.build_root, "repos")
        self.working_dir = os.path.join(self.build_root, "working")

        # Output dirs
        self.prefix = to_unix(install_dir)
        self.win_prefix = install_dir
        self.dependency_prefix = dependency_prefix_path or dependency_prefix(root_dir)
        self.dependency_unix_prefix = to_unix(self.dependency_prefix)
        self.license_mode = "lgpl"  # Changed to LGPL per user request

    def _vulkan_import_lib(self, arch: str) -> Optional[str]:
        return get_linux_vulkan_import_lib_path(arch)

    def setup_dirs(self):
        for d in [self.build_root, self.repos_dir, self.working_dir, self.install_dir]:
            os.makedirs(d, exist_ok=True)

    def get_msys_env(self):
        env = os.environ.copy()

        # Add MSYS2 tools to path
        msys_bin = [
            os.path.join(self.msys, "clang64", "bin"),
            os.path.join(self.msys, "usr", "bin"),
            os.path.join(self.install_dir, "bin"),
        ]
        env["PATH"] = os.pathsep.join(msys_bin + [env["PATH"]])

        # Compiler flags
        pkg_config = self.get_tool_path("pkg-config")

        # Add MSYS2 paths for system libs (vpl, etc.)
        msys_inc = to_unix(os.path.join(self.msys, "clang64", "include"))
        msys_lib = to_unix(os.path.join(self.msys, "clang64", "lib"))
        msys_pkgconfig = os.path.join(self.msys, "clang64", "lib", "pkgconfig")
        dependency_inc = to_unix(os.path.join(self.dependency_prefix, "include"))
        dependency_lib = to_unix(os.path.join(self.dependency_prefix, "lib"))
        dependency_pkgconfig = os.path.join(self.dependency_prefix, "lib", "pkgconfig")

        env["CC"] = "clang"
        env["CXX"] = "clang++"
        env["CFLAGS"] = (
            f"-O3 -mguard=cf -fstack-protector-strong -D_FORTIFY_SOURCE=2 "
            f"-ffunction-sections -fdata-sections -I{dependency_inc} "
            f"-I{self.prefix}/include -I{msys_inc}"
        )
        env["CXXFLAGS"] = (
            f"-O3 -mguard=cf -fstack-protector-strong -D_FORTIFY_SOURCE=2 "
            f"-ffunction-sections -fdata-sections -I{dependency_inc} "
            f"-I{self.prefix}/include -I{msys_inc}"
        )
        env["LDFLAGS"] = f"-Wl,--gc-sections -Wl,--guard-cf " f"-L{dependency_lib} -L{self.prefix}/lib -L{msys_lib}"
        env["PKG_CONFIG"] = f"{pkg_config} --static"
        # pkg-config here is a native Windows binary, so it expects Windows-style paths.
        # Using /c/... MSYS paths makes the NVCodec probe fail to locate ffnvcodec.pc.
        env["PKG_CONFIG_PATH"] = os.pathsep.join(
            [dependency_pkgconfig, os.path.join(self.win_prefix, "lib", "pkgconfig"), msys_pkgconfig]
        )
        env["MSYSTEM"] = "CLANG64"  # Ensure we are treated as MinGW-Clang

        return env

    def get_tool_path(self, tool_name):
        """Find tool in MSYS2 directories."""
        # Check clang64/bin first, then usr/bin, then ucrt64/bin
        search_dirs = [
            os.path.join(self.msys, "clang64", "bin"),
            os.path.join(self.msys, "usr", "bin"),
            os.path.join(self.msys, "ucrt64", "bin"),
        ]

        exe = tool_name + ".exe"
        for d in search_dirs:
            p = os.path.join(d, exe)
            if os.path.exists(p):
                return p

        return tool_name  # Fallback to path lookup

    def run(self, cmd, cwd=None, env=None, check=True):
        # Always pass a list to subprocess.run to avoid shell=True injection risk.
        if isinstance(cmd, list):
            cmd_list = cmd
            cmd_str = " ".join(cmd_list)
        else:
            cmd_list = shlex.split(cmd, posix=False)
            cmd_str = cmd
        log(f"[FFmpeg] EXEC: {cmd_str}", detail=True)
        try:
            if env is None:
                env = os.environ.copy()
            if env and "PATH" not in env:
                env["PATH"] = os.environ["PATH"]

            run_logged_subprocess(cmd_list, cwd=cwd, env=env, check=check, shell=False)
        except subprocess.CalledProcessError as e:
            log(f"[FFmpeg] FAILED: {cmd_str}")
            raise e

    def git_clone(self, url, name, update=True):
        """Clone or update a git repository. Returns (path, updated) tuple."""
        dest = os.path.join(self.repos_dir, name)
        git_exe = self.get_tool_path("git")
        env = self.get_msys_env()

        if not os.path.exists(dest):
            log(f"[FFmpeg] Cloning {name}...")
            self.run([git_exe, "clone", "--depth", "1", url, dest], env=env)
            return dest, True  # New clone = always needs build

        if not update:
            log(f"[FFmpeg] Using existing {name} (--skip-updates)")
            return dest, False

        # Get current commit before update
        old_commit = subprocess.check_output([git_exe, "rev-parse", "HEAD"], cwd=dest, env=env).decode().strip()

        # Fetch and reset to latest
        log(f"[FFmpeg] Checking for updates to {name}...")
        try:
            self.run(
                [git_exe, "fetch", "--depth", "1", "origin"],
                cwd=dest,
                env=env,
                check=False,
            )
            self.run(
                [git_exe, "reset", "--hard", "origin/HEAD"],
                cwd=dest,
                env=env,
                check=False,
            )
        except Exception as e:
            log(f"[FFmpeg] Warning: Could not update {name}: {e}")
            return dest, False

        # Get new commit
        new_commit = subprocess.check_output([git_exe, "rev-parse", "HEAD"], cwd=dest, env=env).decode().strip()

        if old_commit != new_commit:
            log(f"[FFmpeg] Updated {name}: {old_commit[:8]} -> {new_commit[:8]}")
            return dest, True
        else:
            log(f"[FFmpeg] {name} is up to date ({new_commit[:8]})")
            return dest, False

    def rmtree_onerror(self, func, path, exc_info):
        import stat
        import time

        if not os.access(path, os.W_OK):
            try:
                os.chmod(path, stat.S_IWRITE)
                func(path)
                return
            except Exception:
                pass

        ex = exc_info[1]
        if isinstance(ex, PermissionError) or isinstance(ex, OSError):
            log(f"[FFmpeg] Locked file: {path}. Retrying...")
            for i in range(5):
                try:
                    time.sleep(1.0)
                    if not os.access(path, os.W_OK):
                        os.chmod(path, stat.S_IWRITE)
                    func(path)
                    return
                except Exception:
                    pass
            log(f"[FFmpeg] FAILED to remove: {path}")
            raise

    def build_dependencies(self, update=True):
        log("[FFmpeg] Building Dependencies...")
        # 1. FFNVCodec
        nv_dir, _ = self.git_clone(FFNVCODEC_URL, "ffnvcodec", update=update)
        make_exe = self.get_tool_path("make")
        self.run(
            [make_exe, f"PREFIX={self.prefix}", "install"],
            cwd=nv_dir,
            env=self.get_msys_env(),
        )

    def build_ffmpeg(self, update=True):
        """Build FFmpeg. Returns True if build was performed."""
        log("[FFmpeg] Building FFmpeg...")
        src_dir, updated = self.git_clone(FFMPEG_URL, "ffmpeg", update=update)
        build_dir = os.path.join(self.working_dir, "ffmpeg")

        if os.path.exists(build_dir):
            shutil.rmtree(build_dir, onerror=self.rmtree_onerror)
        shutil.copytree(src_dir, build_dir)

        # Apply custom patches (LGPL 2.1) from patches/ffmpeg/
        patches_dir = os.path.join(PROJECT_ROOT, "patches", "ffmpeg")
        if os.path.isdir(patches_dir):
            git_exe = self.get_tool_path("git")
            patch_files = sorted(f for f in os.listdir(patches_dir) if f.endswith(".patch"))
            patch_paths = [os.path.join(patches_dir, patch_file) for patch_file in patch_files]
            normalized_targets = normalize_custom_patch_targets(build_dir, patch_paths)
            if normalized_targets:
                log(
                    f"[FFmpeg] Normalized LF line endings for {len(normalized_targets)} custom patch "
                    f"target(s): {', '.join(normalized_targets)}"
                )
            for pf in patch_files:
                patch_path = os.path.join(patches_dir, pf)
                log(f"[FFmpeg] Applying patch: {pf}")
                self.run(
                    [git_exe, "apply", "--verbose", patch_path],
                    cwd=build_dir,
                    env=self.get_msys_env(),
                )
            if patch_files:
                log(f"[FFmpeg] Applied {len(patch_files)} patch(es)")

        env = self.get_msys_env()
        make_exe = self.get_tool_path("make")
        bash_exe = self.get_tool_path("bash")

        # Define msys_lib for extra-ldflags
        msys_lib = to_unix(os.path.join(self.msys, "clang64", "lib"))
        dependency_lib = to_unix(os.path.join(self.dependency_prefix, "lib"))

        conf = [
            bash_exe,
            "./configure",
            f"--prefix={self.prefix}",
            "--target-os=mingw32",
            "--enable-shared",
            "--disable-static",  # SHARED BUILD
            # '--pkg-config-flags="--static"',
            "--arch=x86_64",
            # Linking fixes
            # We explicitly link dependent C++ libraries to ensure they are available to avcodec.dll
            # libvpl (for QSV) often needs -lvpl -lstdc++ and system libs
            "--extra-libs=-lc++ -lvpl -lstdc++ -lole32 -lgdi32 -luuid",
            # Toolchain
            "--extra-libs=-lc++",
            # Toolchain - Use MSYS2 Clang
            "--cc=clang",
            "--cxx=clang++",
            "--ar=llvm-ar",
            "--nm=llvm-nm",
            "--ranlib=llvm-ranlib",
            # Optimization
            # AAC NMR and multiple FFmpeg DSP/psychoacoustic paths use NaN/Inf
            # sentinels. -ffast-math makes those undefined and can silently
            # invalidate the encoder's quality decisions.
            "--extra-cflags=-O3 -flto -mguard=cf",
            "--extra-cxxflags=-O3 -flto -mguard=cf",
            "--extra-ldflags=-flto -O3 -Wl,--guard-cf",
            f"--extra-ldflags=-L{dependency_lib} -L{msys_lib}",
            # Keep the FFmpeg build redistributable under LGPLv2.1+.
            # The extra components used here (FFNVCodec headers, AMF headers,
            # oneVPL/libvpl, MediaFoundation, Windows HW accel APIs) do not
            # require enabling GPL, version3, or nonfree mode.
            "--disable-gpl",
            # Components
            "--disable-doc",
            "--disable-programs",
            "--enable-ffmpeg",
            "--enable-ffprobe",
            "--disable-avdevice",
            "--disable-avfilter",
            "--disable-zlib",
            "--disable-bzlib",
            "--disable-lzma",
            "--disable-alsa",  # Linux audio not available on Windows
            # Hardware
            "--enable-d3d11va",
            "--enable-dxva2",
            "--enable-nvenc",
            "--enable-nvdec",
            "--enable-vulkan",
            "--enable-amf",
            "--enable-libvpl",  # QSV
            "--enable-mediafoundation",
            "--enable-libaom",  # AOM AV1 encoder (10-bit 4:4:4 AVIF screenshots)
            "--enable-libsvtav1",  # SVT-AV1 encoder (fast AV1, BSD license)
            "--enable-libopus",  # Opus audio encoder with correct packetization support
            # Tuning
            "--disable-encoders",
            "--disable-decoders",
            "--disable-muxers",
            "--disable-demuxers",
            "--disable-parsers",
            "--disable-bsfs",
            "--disable-protocols",
            "--enable-protocol=file",
            "--enable-bsf=hevc_metadata,av1_metadata",
            "--enable-muxer=mp4,matroska,mov,flv,ts,avif",
            "--enable-demuxer=concat,matroska,mov,mp4",
            # SW Encoders (Audio)
            "--enable-encoder=aac,libopus,flac,alac,pcm_s16le,pcm_s24le,pcm_f32le",
            "--enable-decoder=aac,opus,flac,alac,pcm_s16le,pcm_s24le,pcm_f32le",
            "--enable-parser=aac,opus,flac",
            # HW Encoders
            "--enable-encoder=h264_nvenc,hevc_nvenc,av1_nvenc",
            "--enable-encoder=h264_amf,hevc_amf,av1_amf",
            "--enable-encoder=h264_qsv,hevc_qsv,av1_qsv,vp9_qsv",
            "--enable-encoder=h264_mf,hevc_mf,av1_mf",  # MediaFoundation
            # FFmpeg configure component names use underscores even though the
            # runtime encoder name exposed by libavcodec is "libaom-av1".
            "--enable-encoder=libaom_av1",  # AOM AV1 (HDR 4:4:4 still images)
            "--enable-encoder=libsvtav1",  # SVT-AV1 (for AVIF screenshots)
            # HW Decoders
            "--enable-decoder=h264,hevc,av1,vp9,mjpeg",
            "--enable-decoder=libaom_av1",  # Deterministic 10-bit 4:4:4 AVIF verification/decoding
            "--enable-decoder=h264_qsv,hevc_qsv,av1_qsv,vp9_qsv",
            "--enable-decoder=h264_cuvid,hevc_cuvid,vp9_cuvid,av1_cuvid",
            "--enable-hwaccel=h264_nvdec,hevc_nvdec,av1_nvdec",
            "--enable-hwaccel=h264_d3d11va,hevc_d3d11va,av1_d3d11va",
        ]

        self.run(conf, cwd=build_dir, env=env)
        self.run([make_exe, f"-j{cpu_count()}"], cwd=build_dir, env=env)
        self.run([make_exe, "install"], cwd=build_dir, env=env)


FFMPEG_BUILD_CONFIGURATION_VERSION = 10


def ffmpeg_build_configuration_fingerprint():
    """Track local configure/patch inputs independently of the upstream commit."""
    digest = hashlib.sha256(f"configure-v{FFMPEG_BUILD_CONFIGURATION_VERSION}\n".encode("ascii"))
    digest.update(b"dependency-manifest\n")
    digest.update(dependency_manifest_fingerprint(FFMPEG_DEPENDENCY_MANIFEST).encode("ascii"))
    patches_dir = os.path.join(PROJECT_ROOT, "patches", "ffmpeg")
    if os.path.isdir(patches_dir):
        for patch_name in sorted(name for name in os.listdir(patches_dir) if name.endswith(".patch")):
            digest.update(patch_name.encode("utf-8"))
            with open(os.path.join(patches_dir, patch_name), "rb") as patch_file:
                digest.update(patch_file.read())
    return digest.hexdigest()
