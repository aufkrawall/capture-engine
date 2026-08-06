

class TestAppCommand(List[str]):
    """Test app command that remembers the architecture its call site selected.

    The architecture decides both the build environment and the object
    directory, so it must not be re-derived by inspecting the flags: Linux
    cross builds carry no architecture flag at all, which silently mapped every
    x86 test app onto the x64 object path.
    """

    def __init__(self, arguments: List[str], arch: str):
        super().__init__(arguments)
        self.arch = arch


def ensure_unique_testapp_objects(entries: List[Tuple[str, str]]) -> None:
    """Reject test app tasks that would compile into a shared object path.

    Two tasks sharing one object let parallel workers overwrite each other's
    object mid-link, which surfaces as an unrelated "file format not
    recognized" link failure on whichever task lost the race.
    """
    owners: Dict[str, str] = {}
    for desc, object_path in entries:
        normalized = os.path.normcase(os.path.abspath(object_path))
        previous_desc = owners.get(normalized)
        if previous_desc is not None:
            raise RuntimeError(
                f"Multiple test app tasks target the same object {normalized}: {previous_desc} and {desc}"
            )
        owners[normalized] = desc


def compile_testapps(env, x86_env, clang_exe, cflags):
    """Compile test applications using Clang (and x86 if available)"""
    log("Compiling Test Applications...")

    # x64 test apps intentionally exercise injection into an effectively
    # CFG-instrumented process. Only the broken i686 CRT/load-config path is
    # exempted below.
    cflags = make_cpp_cflags(TESTAPP_OPT_FLAGS_X64, compiler_exe=clang_exe)

    testapp_src_dir = os.path.join(PROJECT_ROOT, "testapp")
    testapp_bin_dir = TESTAPP_BIN_DIR
    os.makedirs(testapp_bin_dir, exist_ok=True)

    # Optional: also build 32-bit variants if 32-bit toolchain is available.
    x86_bin_dir = os.path.join(testapp_bin_dir, "x86")
    os.makedirs(x86_bin_dir, exist_ok=True)

    # Determine x86 compiler availability
    if IS_LINUX:
        clang_exe_x86 = get_compiler_exe("x86")
        have_x86 = clang_exe_x86 is not None
        x86_arch_flags: List[str] = []
    else:
        clang_exe_x86 = get_compiler_exe("x86")
        have_x86 = clang_exe_x86 is not None and os.path.exists(clang_exe_x86)
        x86_arch_flags = [
            "--target=i686-w64-mingw32",
            "--sysroot=" + os.path.join(MSYS2_DIR, "mingw32"),
            "-mstackrealign",
            "-stdlib=libstdc++",
        ]

    if env.get("CE_SANITIZE") == "1":
        have_x86 = False
        log("Sanitizer mode: skipping x86 test applications (ASan runtime unavailable)")

    cflags_x86 = [
        f
        for f in make_cpp_cflags(
            TESTAPP_OPT_FLAGS_X86,
            compiler_exe=clang_exe_x86,
            arch_flags=x86_arch_flags,
            suppress_microsoft_exception_spec=True,
            enable_cfg=False,
        )
        if f != "-flto"
    ]

    if not IS_LINUX and have_x86 and clang_exe_x86 is not None:
        clangd_x86_flags = _clangd_extra_flags_for_arch("x86", clang_exe_x86)
        x86_include_flags = [
            flag for flag in clangd_x86_flags if flag.startswith("-isystem") or flag.startswith("-resource-dir=")
        ]
        if x86_include_flags:
            cflags_x86.extend(x86_include_flags)

    x86_linker_prefix: List[str] = []
    if not IS_LINUX and have_x86 and clang_exe_x86 is not None:
        x86_std_lib_path = ""
        try:
            res = subprocess.check_output(
                [clang_exe_x86, "-print-libgcc-file-name", "--target=i686-w64-mingw32"],
                encoding="utf-8",
            ).strip()
            x86_std_lib_path = os.path.dirname(res)
        except Exception as e:
            log(f"Warning: Failed to find x86 test app lib path: {e}")
        if not x86_std_lib_path:
            x86_std_lib_path = os.path.join(MSYS2_DIR, "mingw32", "lib")
        x86_linker_prefix = [
            "-Wl,--kill-at",
            "-fuse-ld=lld",
            "-stdlib=libstdc++",
            "-static-libstdc++",
            "-rtlib=libgcc",
            "--unwindlib=libgcc",
            "-lpthread",
            "-L" + x86_std_lib_path,
        ]

    tasks = []
    fg_sdk_cflags = cflags + get_fg_sdk_include_flags()
    vulkan_fg_src = os.path.join(testapp_src_dir, "vulkan_fg_switch_test.cpp")
    vulkan_fg_shader_include = None
    if os.path.exists(vulkan_fg_src):
        vulkan_fg_shader_include = compile_vulkan_fg_shaders(env)
    vulkan_fg_cflags = cflags + get_vulkan_fg_sdk_include_flags()
    if vulkan_fg_shader_include:
        vulkan_fg_cflags.append("-I" + vulkan_fg_shader_include)

    def add_task(desc, cmd, cwd=None, task_env=None):
        arch = getattr(cmd, "arch", None) or ("x86" if is_x86_compile_command(cmd) else "x64")
        if task_env is None:
            task_env = x86_env if x86_env is not None and arch == "x86" else env
        tasks.append((desc, cmd, cwd, make_task_temp_environment(task_env, desc), arch))

    def make_cmd(compiler, flags, source, linker_flags, output, *, arch="x64"):
        effective_linker_flags = list(linker_flags) + list(LD_OPT_FLAGS)
        if arch == "x64":
            effective_linker_flags.extend(get_x64_linker_flags(compiler))
        else:
            effective_linker_flags.extend(get_x86_testapp_cfg_link_flags(compiler))
        append_windows_pdb_linker_flag(effective_linker_flags, output)
        sources = [source] if isinstance(source, str) else list(source)
        return TestAppCommand(
            [compiler] + flags + sources + effective_linker_flags + ["-o", output], arch
        )

    def make_cmd_x86(compiler, flags, source, linker_flags, output):
        return make_cmd(compiler, flags, source, x86_linker_prefix + list(linker_flags), output, arch="x86")

    vulkan_lib = get_linux_vulkan_import_lib_path("x64")
    vulkan_lib_x86 = get_linux_vulkan_import_lib_path("x86")
    if vulkan_lib is None:
        raise RuntimeError("Vulkan import library unavailable for required x64 test applications")

    # DX12 Test App
    dx12_src = os.path.join(testapp_src_dir, "dx12_test.cpp")
    dx12_exe = os.path.join(testapp_bin_dir, "dx12_test.exe")
    if os.path.exists(dx12_src):
        dx12_ldflags = [
            "-static",
            "-Wl,--subsystem,windows",
        ]
        dx12_ldflags.extend(
            [
                "-ld3d12",
                "-ldxgi",
                "-ld3dcompiler",
                "-lgdi32",
                "-luser32",
                "-lshcore",
                "-lavrt",
            ]
        )
        add_task(
            "dx12_test.exe",
            make_cmd(clang_exe, cflags, dx12_src, dx12_ldflags, dx12_exe),
        )

        if have_x86:
            dx12_exe_x86 = os.path.join(x86_bin_dir, "dx12_test.exe")
            add_task(
                "dx12_test.exe (x86)",
                make_cmd_x86(clang_exe_x86, cflags_x86, dx12_src, dx12_ldflags, dx12_exe_x86),
            )

    # DX12 + WASAPI A/V Sync Stimulus App
    av_sync_src = os.path.join(testapp_src_dir, "dx12_av_sync_test.cpp")
    av_sync_exe = os.path.join(testapp_bin_dir, "dx12_av_sync_test.exe")
    if os.path.exists(av_sync_src):
        av_sync_ldflags = list(dx12_ldflags)
        av_sync_ldflags.extend(["-lole32", "-luuid", "-lwinmm"])
        add_task(
            "dx12_av_sync_test.exe",
            make_cmd(clang_exe, cflags, av_sync_src, av_sync_ldflags, av_sync_exe),
        )

        if have_x86:
            av_sync_exe_x86 = os.path.join(x86_bin_dir, "dx12_av_sync_test.exe")
            add_task(
                "dx12_av_sync_test.exe (x86)",
                make_cmd_x86(clang_exe_x86, cflags_x86, av_sync_src, av_sync_ldflags, av_sync_exe_x86),
            )

    # FSR FG DX12 Test App
    fsr_fg_src = os.path.join(testapp_src_dir, "dx12_fsr_fg_test.cpp")
    fsr_fg_exe = os.path.join(testapp_bin_dir, "dx12_fsr_fg_test.exe")
    if os.path.exists(fsr_fg_src):
        fsr_fg_ldflags = list(dx12_ldflags)
        add_task(
            "dx12_fsr_fg_test.exe",
            make_cmd(clang_exe, fg_sdk_cflags, fsr_fg_src, fsr_fg_ldflags, fsr_fg_exe),
        )
        if have_x86:
            log("Skipping dx12_fsr_fg_test.exe (x86): FG SDK runtime DLLs are x64-only")

    # DLSS FG DX12 Test App
    dlss_fg_src = os.path.join(testapp_src_dir, "dx12_dlss_fg_test.cpp")
    dlss_fg_exe = os.path.join(testapp_bin_dir, "dx12_dlss_fg_test.exe")
    if os.path.exists(dlss_fg_src):
        dlss_fg_ldflags = list(dx12_ldflags)
        add_task(
            "dx12_dlss_fg_test.exe",
            make_cmd(clang_exe, fg_sdk_cflags, dlss_fg_src, dlss_fg_ldflags, dlss_fg_exe),
        )
        if have_x86:
            log("Skipping dx12_dlss_fg_test.exe (x86): FG SDK runtime DLLs are x64-only")

    # DLSS/FSR FG Switching DX12 Test App
    fg_switch_src = os.path.join(testapp_src_dir, "dx12_fg_switch_test.cpp")
    fg_switch_units = [
        os.path.join(testapp_src_dir, name)
        for name in (
            "dx12_fg_switch_config.cpp",
            "dx12_fg_switch_dred.cpp",
            "dx12_fg_switch_hud.cpp",
            "dx12_fg_switch_input.cpp",
            "dx12_fg_switch_common.cpp",
            "dx12_fg_switch_fsr.cpp",
            "dx12_fg_switch_streamline.cpp",
            "dx12_fg_switch_swapchain.cpp",
            "dx12_fg_switch_upscale.cpp",
            "dx12_fg_switch_render.cpp",
        )
    ]
    fg_switch_exe = os.path.join(testapp_bin_dir, "dx12_fg_switch_test.exe")
    if os.path.exists(fg_switch_src):
        fg_switch_ldflags = list(dx12_ldflags)
        fg_switch_sources = [fg_switch_src] + [
            unit for unit in fg_switch_units if os.path.exists(unit)
        ]
        add_task(
            "dx12_fg_switch_test.exe",
            make_cmd(clang_exe, fg_sdk_cflags, fg_switch_sources, fg_switch_ldflags, fg_switch_exe),
        )
        if have_x86:
            log("Skipping dx12_fg_switch_test.exe (x86): FG SDK runtime DLLs are x64-only")

    # DX11 Test App
    dx11_src = os.path.join(testapp_src_dir, "dx11_test.cpp")
    dx11_exe = os.path.join(testapp_bin_dir, "dx11_test.exe")
    if os.path.exists(dx11_src):
        dx11_ldflags = [
            "-static",
            "-Wl,--subsystem,windows",
        ]
        dx11_ldflags.extend(
            [
                "-ld3d11",
                "-ldxgi",
                "-lgdi32",
                "-luser32",
                "-lshcore",
                "-lavrt",
            ]
        )
        add_task(
            "dx11_test.exe",
            make_cmd(clang_exe, cflags, dx11_src, dx11_ldflags, dx11_exe),
        )

        if have_x86:
            dx11_exe_x86 = os.path.join(x86_bin_dir, "dx11_test.exe")
            add_task(
                "dx11_test.exe (x86)",
                make_cmd_x86(clang_exe_x86, cflags_x86, dx11_src, dx11_ldflags, dx11_exe_x86),
            )

    # DX9 / DX9Ex Test Apps
    dx9_src = os.path.join(testapp_src_dir, "dx9_test.cpp")
    dx9_exe = os.path.join(testapp_bin_dir, "dx9_test.exe")
    if os.path.exists(dx9_src):
        dx9_ldflags = [
            "-static",
            "-Wl,--subsystem,windows",
        ]
        dx9_ldflags.extend(
            [
                "-ld3d9",
                "-lgdi32",
                "-luser32",
                "-lavrt",
            ]
        )
        dx9ex_exe = os.path.join(testapp_bin_dir, "dx9ex_test.exe")
        dx9ex_cflags = list(cflags) + ["-DCE_TESTAPP_D3D9EX=1"]

        add_task("dx9_test.exe", make_cmd(clang_exe, cflags, dx9_src, dx9_ldflags, dx9_exe))
        add_task(
            "dx9ex_test.exe",
            make_cmd(clang_exe, dx9ex_cflags, dx9_src, dx9_ldflags, dx9ex_exe),
        )

        if have_x86:
            dx9_exe_x86 = os.path.join(x86_bin_dir, "dx9_test.exe")
            dx9ex_exe_x86 = os.path.join(x86_bin_dir, "dx9ex_test.exe")
            dx9ex_cflags_x86 = list(cflags_x86) + ["-DCE_TESTAPP_D3D9EX=1"]
            add_task(
                "dx9_test.exe (x86)",
                make_cmd_x86(clang_exe_x86, cflags_x86, dx9_src, dx9_ldflags, dx9_exe_x86),
            )
            add_task(
                "dx9ex_test.exe (x86)",
                make_cmd_x86(clang_exe_x86, dx9ex_cflags_x86, dx9_src, dx9_ldflags, dx9ex_exe_x86),
            )

    # DX10 Test App
    dx10_src = os.path.join(testapp_src_dir, "dx10_test.cpp")
    dx10_exe = os.path.join(testapp_bin_dir, "dx10_test.exe")
    if os.path.exists(dx10_src):
        dx10_ldflags = [
            "-static",
            "-Wl,--subsystem,windows",
            "-ld3d10",
            "-ldxgi",
            "-ld3dcompiler",
            "-lgdi32",
            "-luser32",
            "-lshcore",
            "-lavrt",
        ]
        add_task(
            "dx10_test.exe",
            make_cmd(clang_exe, cflags, dx10_src, dx10_ldflags, dx10_exe),
        )

        if have_x86:
            dx10_exe_x86 = os.path.join(x86_bin_dir, "dx10_test.exe")
            add_task(
                "dx10_test.exe (x86)",
                make_cmd_x86(clang_exe_x86, cflags_x86, dx10_src, dx10_ldflags, dx10_exe_x86),
            )

    # Vulkan Test App
    vulkan_src = os.path.join(testapp_src_dir, "vulkan_test.cpp")
    vulkan_exe = os.path.join(testapp_bin_dir, "vulkan_test.exe")
    if os.path.exists(vulkan_src):
        if vulkan_lib is not None:
            vulkan_ldflags = [
                "-static",
                "-Wl,--subsystem,windows",
                vulkan_lib,
                "-lgdi32",
                "-luser32",
                "-lshcore",
                "-lavrt",
            ]
            add_task(
                "vulkan_test.exe",
                make_cmd(clang_exe, cflags, vulkan_src, vulkan_ldflags, vulkan_exe),
            )
        elif IS_LINUX:
            log("Linux host: skipping vulkan_test.exe - Vulkan import library unavailable")

        if have_x86:
            vulkan_exe_x86 = os.path.join(x86_bin_dir, "vulkan_test.exe")
            if vulkan_lib_x86:
                vulkan_ldflags_x86 = [
                    "-static",
                    "-Wl,--subsystem,windows",
                    vulkan_lib_x86,
                    "-lgdi32",
                    "-luser32",
                    "-lshcore",
                    "-lavrt",
                ]
                add_task(
                    "vulkan_test.exe (x86)",
                    make_cmd_x86(
                        clang_exe_x86,
                        cflags_x86,
                        vulkan_src,
                        vulkan_ldflags_x86,
                        vulkan_exe_x86,
                    ),
                )
            else:
                log("Linux host: skipping vulkan_test.exe (x86) - Vulkan import library unavailable")

    # DLSS/FSR FG Switching Vulkan Test App (the Vulkan FidelityFX/Streamline runtimes are x64-only).
    vulkan_fg_exe = os.path.join(testapp_bin_dir, "vulkan_fg_switch_test.exe")
    if os.path.exists(vulkan_fg_src):
        if vulkan_lib is not None:
            vulkan_fg_ldflags = [
                "-static",
                "-Wl,--subsystem,windows",
                vulkan_lib,
                "-lgdi32",
                "-luser32",
                "-lshcore",
                "-lavrt",
                "-lversion",
            ]
            vulkan_fg_sources = [vulkan_fg_src] + [
                os.path.join(testapp_src_dir, name)
                for name in (
                    "vulkan_fg_switch_diagnostics.cpp",
                    "vulkan_fg_switch_streamline.cpp",
                    "vulkan_fg_switch_streamline_shutdown.cpp",
                    "vulkan_fg_switch_device.cpp",
                    "vulkan_fg_switch_fidelityfx.cpp",
                    "vulkan_fg_switch_fidelityfx_frame.cpp",
                    "vulkan_fg_switch_wsi.cpp",
                    "vulkan_fg_switch_renderer.cpp",
                    "vulkan_fg_switch_renderer_record.cpp",
                )
                if os.path.exists(os.path.join(testapp_src_dir, name))
            ]
            add_task(
                "vulkan_fg_switch_test.exe",
                make_cmd(
                    clang_exe, vulkan_fg_cflags, vulkan_fg_sources, vulkan_fg_ldflags, vulkan_fg_exe
                ),
            )
        elif IS_LINUX:
            log("Linux host: skipping vulkan_fg_switch_test.exe - Vulkan import library unavailable")
        if have_x86:
            log("Skipping vulkan_fg_switch_test.exe (x86): FidelityFX/Streamline Vulkan runtimes are x64-only")

    # OpenGL Test App
    opengl_src = os.path.join(testapp_src_dir, "opengl_test.cpp")
    opengl_exe = os.path.join(testapp_bin_dir, "opengl_test.exe")
    if os.path.exists(opengl_src):
        opengl_ldflags = [
            "-static",
            "-Wl,--subsystem,windows",
            "-lopengl32",
            "-lglu32",
            "-lgdi32",
            "-luser32",
            "-lshcore",
            "-lavrt",
        ]

        add_task(
            "opengl_test.exe",
            make_cmd(clang_exe, cflags, opengl_src, opengl_ldflags, opengl_exe),
        )

        if have_x86:
            opengl_exe_x86 = os.path.join(x86_bin_dir, "opengl_test.exe")
            add_task(
                "opengl_test.exe (x86)",
                make_cmd_x86(clang_exe_x86, cflags_x86, opengl_src, opengl_ldflags, opengl_exe_x86),
            )

    # Legacy OpenGL Test App
    opengl_legacy_src = os.path.join(testapp_src_dir, "opengl_legacy_test.cpp")
    opengl_legacy_exe = os.path.join(testapp_bin_dir, "opengl_legacy_test.exe")
    if os.path.exists(opengl_legacy_src):
        opengl_legacy_ldflags = [
            "-static",
            "-Wl,--subsystem,windows",
            "-lopengl32",
            "-lgdi32",
            "-luser32",
            "-lshcore",
            "-lavrt",
        ]

        add_task(
            "opengl_legacy_test.exe",
            make_cmd(
                clang_exe,
                cflags,
                opengl_legacy_src,
                opengl_legacy_ldflags,
                opengl_legacy_exe,
            ),
        )

        if have_x86:
            opengl_legacy_exe_x86 = os.path.join(x86_bin_dir, "opengl_legacy_test.exe")
            add_task(
                "opengl_legacy_test.exe (x86)",
                make_cmd_x86(
                    clang_exe_x86,
                    cflags_x86,
                    opengl_legacy_src,
                    opengl_legacy_ldflags,
                    opengl_legacy_exe_x86,
                ),
            )

    # DirectDraw7 Test App
    directdraw7_src = os.path.join(testapp_src_dir, "directdraw7_test.cpp")
    directdraw7_exe = os.path.join(testapp_bin_dir, "directdraw7_test.exe")
    if os.path.exists(directdraw7_src):
        directdraw7_ldflags = [
            "-static",
            "-Wl,--subsystem,windows",
            "-lddraw",
            "-ldxguid",
            "-lgdi32",
            "-luser32",
            "-lavrt",
        ]

        add_task(
            "directdraw7_test.exe",
            make_cmd(clang_exe, cflags, directdraw7_src, directdraw7_ldflags, directdraw7_exe),
        )

        if have_x86:
            directdraw7_exe_x86 = os.path.join(x86_bin_dir, "directdraw7_test.exe")
            add_task(
                "directdraw7_test.exe (x86)",
                make_cmd_x86(
                    clang_exe_x86,
                    cflags_x86,
                    directdraw7_src,
                    directdraw7_ldflags,
                    directdraw7_exe_x86,
                ),
            )

    # DX6 Test App
    dx6_src = os.path.join(testapp_src_dir, "dx6_test.cpp")
    dx6_exe = os.path.join(testapp_bin_dir, "dx6_test.exe")
    if os.path.exists(dx6_src):
        dx6_ldflags = [
            "-static",
            "-Wl,--subsystem,windows",
            "-lddraw",
            "-ldxguid",
            "-lgdi32",
            "-luser32",
            "-lavrt",
        ]

        add_task(
            "dx6_test.exe",
            make_cmd(clang_exe, cflags, dx6_src, dx6_ldflags, dx6_exe),
        )

        if have_x86:
            dx6_exe_x86 = os.path.join(x86_bin_dir, "dx6_test.exe")
            add_task(
                "dx6_test.exe (x86)",
                make_cmd_x86(
                    clang_exe_x86,
                    cflags_x86,
                    dx6_src,
                    dx6_ldflags,
                    dx6_exe_x86,
                ),
            )

    # DX7 Test App
    dx7_src = os.path.join(testapp_src_dir, "dx7_test.cpp")
    dx7_exe = os.path.join(testapp_bin_dir, "dx7_test.exe")
    if os.path.exists(dx7_src):
        dx7_ldflags = [
            "-static",
            "-Wl,--subsystem,windows",
            "-lddraw",
            "-ldxguid",
            "-lgdi32",
            "-luser32",
            "-lavrt",
        ]

        add_task(
            "dx7_test.exe",
            make_cmd(clang_exe, cflags, dx7_src, dx7_ldflags, dx7_exe),
        )

        if have_x86:
            dx7_exe_x86 = os.path.join(x86_bin_dir, "dx7_test.exe")
            add_task(
                "dx7_test.exe (x86)",
                make_cmd_x86(
                    clang_exe_x86,
                    cflags_x86,
                    dx7_src,
                    dx7_ldflags,
                    dx7_exe_x86,
                ),
            )

    # DX8 Test App
    dx8_src = os.path.join(testapp_src_dir, "dx8_test.cpp")
    dx8_exe = os.path.join(testapp_bin_dir, "dx8_test.exe")
    if os.path.exists(dx8_src):
        dx8_ldflags = [
            "-static",
            "-Wl,--subsystem,windows",
            "-lgdi32",
            "-luser32",
            "-lavrt",
        ]

        add_task(
            "dx8_test.exe",
            make_cmd(clang_exe, cflags, dx8_src, dx8_ldflags, dx8_exe),
        )

        if have_x86:
            dx8_exe_x86 = os.path.join(x86_bin_dir, "dx8_test.exe")
            add_task(
                "dx8_test.exe (x86)",
                make_cmd_x86(
                    clang_exe_x86,
                    cflags_x86,
                    dx8_src,
                    dx8_ldflags,
                    dx8_exe_x86,
                ),
            )

    # Execute all tasks in parallel
    if not tasks:
        return

    log("Test app compiler temp directories are isolated per task to avoid parallel temp-file collisions")

    def split_task_command(cmd):
        first_source = next(index for index, argument in enumerate(cmd) if argument.endswith(".cpp"))
        output_index = cmd.index("-o", first_source + 1)
        sources = [i for i in range(first_source, output_index) if cmd[i].endswith(".cpp")]
        return sources, output_index

    def testapp_object_path(cmd, arch, source_index):
        output_index = cmd.index("-o", source_index)
        output = cmd[output_index + 1]
        exe_stem = os.path.splitext(os.path.basename(output))[0]
        src_stem = os.path.splitext(os.path.basename(cmd[source_index]))[0]
        object_dir = os.path.join(OBJ_DIR, "testapps", arch)
        if src_stem == exe_stem:
            return os.path.join(object_dir, exe_stem + ".o")
        return os.path.join(object_dir, exe_stem + "__" + src_stem + ".o")

    def compile_app(t):
        desc, cmd, cwd, tenv, arch = t
        sources, output_index = split_task_command(cmd)
        compiler = cmd[0]
        compile_flags = cmd[1 : sources[0]]
        linker_flags = cmd[sources[-1] + 1 : output_index]
        output = cmd[output_index + 1]
        object_paths = []
        compiled = False
        for source_index in sources:
            object_path = testapp_object_path(cmd, arch, source_index)
            object_paths.append(object_path)
            os.makedirs(os.path.dirname(object_path), exist_ok=True)
            log(f"Building {desc} ({cmd[source_index]})...", detail=True)
            compiled = (
                compile_object(tenv, compiler, compile_flags, cmd[source_index], object_path)
                or compiled
            )

        # On Linux, log the resolved linker path for diagnostic purposes.
        if IS_LINUX:
            linker_info = _get_linux_cross_linker_info(compiler)
            if linker_info:
                if linker_info.startswith("not_found("):
                    log(
                        f"WARNING: Cross-linker not found for {compiler} - reported: {linker_info}",
                    )
                else:
                    log(f"Cross-linker: {linker_info}", detail=True)

        link_driver_flags = [
            flag
            for flag in compile_flags
            if flag.startswith(("--target=", "--sysroot=", "-stdlib=", "-fsanitize="))
            or flag in ("-m32", "-m64")
        ]
        link_command = [compiler] + link_driver_flags + object_paths + linker_flags + ["-o", output]
        required_outputs = [output]
        if IS_WINDOWS:
            required_outputs.append(pdb_path_for_binary(output))
        linked = run_cached_link(
            link_command,
            tenv,
            output,
            required_outputs=required_outputs,
            cwd=cwd,
        )
        state = "built" if compiled or linked else "cached"
        log(f"Test app {state}: {desc}", detail=True)
        return compiled, linked

    ensure_unique_testapp_objects(
        [
            (desc, testapp_object_path(command, arch, source_index))
            for desc, command, _, _, arch in tasks
            for source_index in split_task_command(command)[0]
        ]
    )

    for _, command, _, _, _ in tasks:
        compute_compiler_fingerprint(command[0])

    worker_count = get_parallel_job_count(env, len(tasks))
    log(f"Building {len(tasks)} Test Apps in parallel ({worker_count} workers)...")
    errors = []
    built_count = 0
    cached_count = 0
    failed_descriptions = []
    with ThreadPoolExecutor(max_workers=worker_count) as executor:
        futures = {executor.submit(compile_app, t): t[0] for t in tasks}
        for future in as_completed(futures):
            desc = futures[future]
            try:
                compiled, linked = future.result()
                if compiled or linked:
                    built_count += 1
                else:
                    cached_count += 1
            except BaseException as e:
                # A bare exception repr rarely names the failing app, so record
                # the task description: it is the only handle the log offers
                # when several apps build concurrently.
                log(f"ERROR: Test app failed to build: {desc} - {type(e).__name__}: {e}")
                failed_descriptions.append(desc)
                errors.append(e)

    if errors:
        raise RuntimeError(
            f"{len(errors)} test app(s) failed to build: {', '.join(failed_descriptions)};"
            f" first error: {errors[0]}"
        )
    log(f"Test app summary: {built_count} built, {cached_count} cached, {len(tasks)} total")

    stale_shader_sidecars = glob.glob(os.path.join(testapp_bin_dir, "vulkan_fg_*.spv"))
    if stale_shader_sidecars:
        raise RuntimeError(f"Vulkan FG runtime shader sidecars are forbidden: {stale_shader_sidecars}")
    if os.path.exists(vulkan_fg_src):
        if not os.path.exists(vulkan_fg_exe):
            raise RuntimeError("vulkan_fg_switch_test.exe was not produced")
        if IS_WINDOWS and not os.path.exists(pdb_path_for_binary(vulkan_fg_exe)):
            raise RuntimeError("vulkan_fg_switch_test.pdb was not produced")
