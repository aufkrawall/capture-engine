

def compile_vulkan_layer(env, clang_exe, cflags, arch):
    """Compile VK_LAYER_CE_overlay - Vulkan implicit layer for overlay and capture"""
    log(f"Compiling Vulkan Layer ({arch})...")

    vulkan_lib = get_linux_vulkan_import_lib_path(arch)
    if not vulkan_lib:
        if arch == "x86" and IS_LINUX:
            log("Linux host: skipping Vulkan Layer (x86) - Vulkan import library unavailable")
            return
        raise RuntimeError(f"Vulkan import library unavailable for required Vulkan Layer ({arch})")

    layer_dir = os.path.join(PROJECT_ROOT, "hook", "vulkan_layer")
    bin_dir = CAPTURE_BIN_DIR
    # Vulkan-layer objects must live under OBJ_DIR (which follows the isolated
    # sanitizer root) instead of PROJECT_ROOT/build/obj. The sanitizer cadence
    # child compiles the SAME layer sources with -fsanitize flags while the
    # product build runs; sharing one object directory let the product build
    # reuse ASan-instrumented objects, failing the layer link with undefined
    # __asan*/__ubsan* symbols and forcing a full --verify --force-rebuild
    # rerun (sessions 20260813_180126 and 20260813_180535).
    obj_dir = os.path.join(OBJ_DIR, arch, "vulkan_layer")
    os.makedirs(obj_dir, exist_ok=True)

    # Layer source files - split into layer/support and hook/common sources
    # hook/common sources are shared with the hook DLL and must use the same
    # optimization flags (HOOK_OPT_FLAGS_X64/X86) to maintain consistency.
    layer_only_sources = [
        os.path.join(layer_dir, "layer_main.cpp"),
        os.path.join(layer_dir, "vulkan_layer.cpp"),
        os.path.join(layer_dir, "vulkan_layer_state.cpp"),
        os.path.join(layer_dir, "vulkan_layer_hooks.cpp"),
        os.path.join(layer_dir, "vulkan_layer_capabilities.cpp"),
        os.path.join(layer_dir, "vulkan_final_output_capture.cpp"),
        os.path.join(layer_dir, "vulkan_layer_present.cpp"),
        os.path.join(layer_dir, "vulkan_layer_swapchain.cpp"),
        os.path.join(layer_dir, "vulkan_reflex_limiter.cpp"),
        os.path.join(layer_dir, "layer_ipc.cpp"),
        os.path.join(layer_dir, "layer_renderer_bootstrap.cpp"),
        os.path.join(layer_dir, "layer_overlay.cpp"),
        os.path.join(layer_dir, "layer_overlay_compute.cpp"),
        os.path.join(layer_dir, "layer_overlay_queue.cpp"),
        os.path.join(layer_dir, "layer_overlay_render.cpp"),
        os.path.join(layer_dir, "layer_sharpen.cpp"),
        os.path.join(layer_dir, "layer_sharpen_setup.cpp"),
        os.path.join(layer_dir, "layer_capture.cpp"),
        os.path.join(layer_dir, "layer_capture_d3d11_interop.cpp"),
        os.path.join(layer_dir, "layer_capture_textures.cpp"),
        os.path.join(layer_dir, "layer_capture_state.cpp"),
        os.path.join(layer_dir, "layer_capture_frame.cpp"),
        os.path.join(layer_dir, "layer_capture_capture.cpp"),
        os.path.join(layer_dir, "layer_bridge.cpp"),
        os.path.join(layer_dir, "layer_wsi_surface_bridge.cpp"),
        os.path.join(layer_dir, "layer_hooks.cpp"),
        # The Vulkan layer intentionally links a selected source set instead of all common objects.
        os.path.join(PROJECT_ROOT, "common", "build_identity.cpp"),
        os.path.join(PROJECT_ROOT, "common", "secure_dll_loading.cpp"),
    ]
    hook_common_sources = [
        os.path.join(PROJECT_ROOT, "hook", "common", "fg_detection.cpp"),
        os.path.join(PROJECT_ROOT, "hook", "common", "ipc_client.cpp"),
        os.path.join(PROJECT_ROOT, "hook", "common", "system_metrics.cpp"),
        os.path.join(PROJECT_ROOT, "hook", "common", "system_metrics_gpu.cpp"),
        os.path.join(PROJECT_ROOT, "hook", "common", "performance_metrics.cpp"),
        os.path.join(PROJECT_ROOT, "hook", "common", "pacing_health_telemetry.cpp"),
        # performance_metrics.cpp resolves a displayed transition back to the
        # frame-generation callback that produced it. The layer has no such
        # callback, so the ring stays empty here, but the symbol is required.
        os.path.join(PROJECT_ROOT, "hook", "common", "present_callback_association.cpp"),
        os.path.join(PROJECT_ROOT, "hook", "common", "overlay_metrics_publisher.cpp"),
        os.path.join(PROJECT_ROOT, "hook", "common", "nv_lod_spread_override.cpp"),
        os.path.join(PROJECT_ROOT, "hook", "common", "perf_logger.cpp"),
        # CustomOverlay system for full overlay rendering
        os.path.join(PROJECT_ROOT, "hook", "common", "sharpen_constants.cpp"),
        os.path.join(PROJECT_ROOT, "hook", "common", "custom_overlay.cpp"),
        os.path.join(PROJECT_ROOT, "hook", "common", "overlay_cpu_raster.cpp"),
        os.path.join(PROJECT_ROOT, "hook", "common", "custom_overlay_vk.cpp"),
        os.path.join(PROJECT_ROOT, "hook", "common", "custom_overlay_vk_render.cpp"),
        os.path.join(PROJECT_ROOT, "hook", "common", "custom_font.cpp"),
        os.path.join(PROJECT_ROOT, "hook", "common", "overlay_adapter.cpp"),
        os.path.join(PROJECT_ROOT, "hook", "common", "overlay_adapter_render.cpp"),
        os.path.join(PROJECT_ROOT, "hook", "common", "overlay_adapter_render_frame.cpp"),
        os.path.join(PROJECT_ROOT, "hook", "common", "benchmark_manager.cpp"),
        os.path.join(PROJECT_ROOT, "hook", "common", "benchmark_html_report.cpp"),
        os.path.join(PROJECT_ROOT, "hook", "common", "cached_overlay_renderer.cpp"),
        os.path.join(PROJECT_ROOT, "hook", "common", "screenshot_hook.cpp"),
        # Owns the screenshot task queue screenshot_hook.cpp hands work to.
        os.path.join(PROJECT_ROOT, "hook", "common", "screenshot_worker.cpp"),
    ]

    # Shared layer-specific flags (include paths, defines)
    layer_extra_flags = [
        "-I" + layer_dir,
        "-I" + os.path.join(PROJECT_ROOT, "common"),
        "-I" + os.path.join(PROJECT_ROOT, "hook", "common"),
        "-DVK_NO_PROTOTYPES",
        "-DIMGUI_IMPL_VULKAN_NO_PROTOTYPES",
        "-DVK_USE_PLATFORM_WIN32_KHR",
        "-DVK_LAYER_CE_OVERLAY",
    ]

    # Build separate cflags: hook/common sources use hook optimization flags
    if arch == "x64":
        hook_opt_flags = HOOK_OPT_FLAGS_X64
    else:
        hook_opt_flags = HOOK_OPT_FLAGS_X86

    hook_cflags = (
        make_cpp_cflags(
            hook_opt_flags,
            compiler_exe=clang_exe,
            suppress_microsoft_exception_spec=True,
            enable_cfg=arch == "x64",
        )
        + layer_extra_flags
    )
    layer_cflags = cflags + layer_extra_flags

    # x86 cross-compilation from clang64: need --target, --sysroot, and
    # -stdlib=libstdc++ because mingw32 only has libstdc++, not libc++.
    if arch == "x86":
        if not IS_LINUX and is_clang_compiler(clang_exe):
            log("[INFO] x86 Vulkan layer: adding cross-compilation flags (Clang on mingw32)")
            x86_cross_flags = [
                "--target=i686-w64-mingw32",
                "--sysroot=" + os.path.join(MSYS2_DIR, "mingw32"),
                "-stdlib=libstdc++",
            ]
            hook_cflags.extend(x86_cross_flags)
            layer_cflags.extend(x86_cross_flags)

    # Add Vulkan headers include path (from MSYS2 on Linux)
    if IS_LINUX:
        vulkan_include = os.path.join(get_linux_msys2_dir(), "clang64", "include")
        if os.path.exists(vulkan_include):
            hook_cflags.extend(["-idirafter", vulkan_include])
            layer_cflags.extend(["-idirafter", vulkan_include])

    layer_objs = []

    # helper to add sources
    def add_sources(sources, dest_obj_dir):
        added = []
        for src in sources:
            if not os.path.exists(src):
                log(f"Warning: Layer source not found: {src}")
                continue
            basename = os.path.splitext(os.path.basename(src))[0]
            obj = os.path.join(dest_obj_dir, basename + ".o")
            added.append((src, obj))
            layer_objs.append(obj)
        return added

    # Compile layer-specific sources with layer cflags
    layer_src_obj_pairs = add_sources(layer_only_sources, obj_dir)
    # Compile hook/common sources with hook cflags (consistent with hook DLL)
    hook_src_obj_pairs = add_sources(hook_common_sources, obj_dir)

    if not layer_src_obj_pairs and not hook_src_obj_pairs:
        log("Error: No layer sources found.")
        return

    # Run parallel compilation for each group with their respective flags
    total_compiled = 0
    total_skipped = 0
    if layer_src_obj_pairs:
        c, s = parallel_compile(env, clang_exe, layer_cflags, layer_src_obj_pairs)
        total_compiled += c
        total_skipped += s
    if hook_src_obj_pairs:
        c, s = parallel_compile(env, clang_exe, hook_cflags, hook_src_obj_pairs)
        total_compiled += c
        total_skipped += s
    compiled = total_compiled
    skipped = total_skipped
    if compiled > 0:
        log(f"Vulkan Layer ({arch}): compiled {compiled}, skipped {skipped}")

    # Link layer DLL
    if arch == "x64":
        layer_dll_name = "VK_LAYER_CE_overlay.dll"
    else:
        layer_dll_name = "VK_LAYER_CE_overlay_x86.dll"

    layer_dll = os.path.join(bin_dir, layer_dll_name)

    ldflags = [
        "-shared",
        "-static",
        vulkan_lib,
        "-lgdi32",
        "-luser32",
        "-lpdh",
        "-ldxgi",
        "-lshcore",
        "-lwinmm",
        "-lversion",
        "-o",
        layer_dll,
    ]

    ldflags.extend(LD_OPT_FLAGS)  # Keep hardening and debug info for crash dumps
    if arch == "x86" and IS_LINUX:
        ldflags.append("-Wl,--allow-multiple-definition")
    if arch == "x64":
        ldflags.extend(get_x64_linker_flags(clang_exe))  # High-entropy ASLR and supported CFG
    if arch == "x86":
        ldflags.append("-Wl,--kill-at")
        if not IS_LINUX:
            ldflags.extend(
                [
                    "--target=i686-w64-mingw32",
                    "--sysroot=" + os.path.join(MSYS2_DIR, "mingw32"),
                    "-fuse-ld=lld",
                    "-stdlib=libstdc++",
                    "-rtlib=libgcc",
                    "--unwindlib=libgcc",
                    "-lpthread",
                ]
            )

        # Re-add -static for proper linking
        if "-static" not in ldflags:
            ldflags.insert(0, "-static")

    append_windows_pdb_linker_flag(ldflags, layer_dll)

    # Use ccache for linking too if available
    ccache_exe = shutil.which("ccache", path=env["PATH"])
    if env.get("DISABLE_CCACHE"):
        ccache_exe = None

    if ccache_exe:
        cmd = [ccache_exe, os.path.basename(clang_exe)] + layer_objs + ldflags
    else:
        cmd = [clang_exe] + layer_objs + ldflags

    # Robust handling for locked DLLs (DataExchangeHost, explorer, etc.)
    if os.path.exists(layer_dll):
        if not safe_delete_file(layer_dll):
            # Even if we can't delete, we can still build if we renamed it
            # Check if the file still exists with original name
            if os.path.exists(layer_dll):
                log(f"[Warning] {os.path.basename(layer_dll)} is still locked, build may fail")
                # Check if locked and log helpful info
                if is_file_locked(layer_dll):
                    log("[Info] File is actively locked by another process")
                    locking = find_process_locking_file(layer_dll)
                    if locking:
                        log(f"[Info] Locking process: {locking}")

    try:
        run_command(cmd, env=env)
        log(f"Built: {layer_dll}")

        # Manifests are staged at runtime into %LOCALAPPDATA% / %PROGRAMDATA% by
        # CaptureEngine (common/vulkan_layer_registration.cpp). Do not leave JSON
        # manifests in bin_dir (installed/captureengine/) to ensure external processes
        # never map or lock bin_dir binaries through stale or legacy registry entries.
        manifest_name = "VK_LAYER_CE_overlay.json" if arch == "x64" else "VK_LAYER_CE_overlay_x86.json"
        manifest_path = os.path.join(bin_dir, manifest_name)
        if os.path.exists(manifest_path):
            try:
                os.remove(manifest_path)
            except OSError:
                pass

    except Exception as e:
        log(f"Error linking layer: {e}")
        raise
