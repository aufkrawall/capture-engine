

def setup_fg_sdk_dependencies(skip_updates: bool = False, *, include_runtime_dlls: bool) -> None:
    """Download and extract FG SDK headers plus optional Windows runtime DLLs.

    Downloads FidelityFX-SDK and Streamline-SDK archives to a cache dir,
    then extracts the headers needed to cross-compile the FG test apps. Native
    Windows builds also extract the runtime DLLs into the test app directory.
    Respects --skip-updates to avoid re-downloading.
    """
    testapp_dir = TESTAPP_BIN_DIR
    if include_runtime_dlls:
        os.makedirs(testapp_dir, exist_ok=True)
    os.makedirs(FG_SDK_CACHE_DIR, exist_ok=True)
    os.makedirs(FG_SDK_INCLUDE_DIR, exist_ok=True)

    streamline_include_dir = os.path.join(FG_SDK_INCLUDE_DIR, "streamline")
    fidelityfx_include_dir = os.path.join(FG_SDK_INCLUDE_DIR, "fidelityfx")
    fidelityfx_vk_include_dir = os.path.join(FG_SDK_INCLUDE_DIR, "fidelityfx_vk_v1_1_4")
    streamline_header_probe = os.path.join(streamline_include_dir, "include", "sl.h")
    ffx_header_probe = os.path.join(
        fidelityfx_include_dir, "Kits", "FidelityFX", "framegeneration", "include", "ffx_framegeneration.h"
    )
    # Upscaler kit header probe: forces a header re-extract on installs that predate the FSR
    # super-resolution integration (the FG header alone used to satisfy the probe).
    ffx_upscale_header_probe = os.path.join(
        fidelityfx_include_dir, "Kits", "FidelityFX", "upscalers", "include", "ffx_upscale.h"
    )
    ffx_vk_header_probe = os.path.join(fidelityfx_vk_include_dir, "ffx_api", "vk", "ffx_api_vk.h")
    ffx_vk_dll = os.path.join(testapp_dir, "amd_fidelityfx_vk.dll")
    ffx_vk_license = os.path.join(testapp_dir, "FidelityFX-SDK-v1.1.4-LICENSE.txt")

    # -- Required DLLs by test app --
    # FSR FG + FSR upscaler (super resolution): core + companion AMD runtime DLLs
    fsr_dlls = [
        "amd_fidelityfx_framegeneration_dx12.dll",
        "amd_fidelityfx_loader_dx12.dll",
        "amd_fidelityfx_upscaler_dx12.dll",
        "amd_acs_x64.dll",
        "amd_ags_x64.dll",
    ]
    # Streamline interposer loads companion .dlls + NGX DLLs at load time;
    # we must extract ALL .dll files from the zip's x64 bin dir.
    sl_known_dlls = [
        "sl.interposer.dll",
        "sl.common.dll",
        "sl.dlss_g.dll",
        "sl.dlss.dll",
        "sl.reflex.dll",
        "nvngx_dlssg.dll",
    ]

    # _nvngx.dll comes from the NVIDIA driver (DriverStore), not from Streamline SDK zip.
    # Search dynamically in case the DriverStore path changes with driver updates.
    _nvngx_sys_paths = _find_nvngx_driverstore_paths() if include_runtime_dlls else []

    missing_fsr = (
        [d for d in fsr_dlls if not os.path.exists(os.path.join(testapp_dir, d))]
        if include_runtime_dlls
        else []
    )
    missing_sl = (
        [d for d in sl_known_dlls if not os.path.exists(os.path.join(testapp_dir, d))]
        if include_runtime_dlls
        else []
    )
    missing_nvngx = include_runtime_dlls and not os.path.exists(os.path.join(testapp_dir, "_nvngx.dll"))
    missing_headers = (
        not os.path.exists(streamline_header_probe)
        or not os.path.exists(ffx_header_probe)
        or not os.path.exists(ffx_upscale_header_probe)
    )
    missing_vk_headers = not os.path.exists(ffx_vk_header_probe)
    missing_vk_runtime = include_runtime_dlls and (
        not os.path.exists(ffx_vk_dll)
        or not os.path.exists(ffx_vk_license)
        or not pe_has_authenticode_certificate(ffx_vk_dll)
    )

    if (
        not missing_fsr
        and not missing_sl
        and not missing_nvngx
        and not missing_headers
        and not missing_vk_headers
        and not missing_vk_runtime
    ):
        log("FG SDK dependencies already present - skipping download")
        return
    log(
        f"FSR FG DLLs missing: {len(missing_fsr)}, Streamline DLLs missing: {len(missing_sl)}, "
        f"headers missing: {1 if missing_headers else 0}, "
        f"Vulkan FFX headers missing: {1 if missing_vk_headers else 0}, "
        f"Vulkan FFX runtime missing: {1 if missing_vk_runtime else 0}"
    )

    def _ensure_zip(url: str, zip_name: str, expected_sha256: Optional[str] = None) -> str:
        zip_path = os.path.join(FG_SDK_CACHE_DIR, zip_name)
        if os.path.exists(zip_path) and expected_sha256:
            actual_sha256 = sha256_file(zip_path)
            if actual_sha256.lower() != expected_sha256.lower():
                log(f"Cached {zip_name} failed SHA-256 verification; replacing it")
                os.remove(zip_path)
        if not os.path.exists(zip_path):
            log(f"Downloading {zip_name}...")
            temp_zip = zip_path + ".tmp"
            try:
                if os.path.exists(temp_zip):
                    os.remove(temp_zip)
                urllib.request.urlretrieve(url, temp_zip)
                os.replace(temp_zip, zip_path)
            finally:
                if os.path.exists(temp_zip):
                    os.remove(temp_zip)
            log(f"Downloaded {zip_name}")
        else:
            log(f"Using cached {zip_name}")
        if expected_sha256:
            actual_sha256 = sha256_file(zip_path)
            if actual_sha256.lower() != expected_sha256.lower():
                raise RuntimeError(f"SHA-256 mismatch for {zip_name}: expected {expected_sha256}, got {actual_sha256}")
            log(f"Verified {zip_name} SHA-256: {actual_sha256}")
        return zip_path

    def _extract_all_dlls_from_path(zip_path: str, inner_prefix: str, dest_dir: str) -> List[str]:
        """Extract every .dll under a given prefix path from a zip archive."""
        import zipfile

        extracted = set()
        with zipfile.ZipFile(zip_path, "r") as zf:
            for entry in zf.infolist():
                fname = os.path.basename(entry.filename)
                if fname.endswith(".dll") and entry.filename.startswith(inner_prefix) and fname not in extracted:
                    zf.extract(entry, dest_dir)
                    extracted_path = os.path.normpath(os.path.join(dest_dir, entry.filename))
                    dest_path = os.path.join(dest_dir, fname)
                    if os.path.exists(extracted_path) and extracted_path != dest_path:
                        safe_replace_or_rename(extracted_path, dest_path)
                    extracted.add(fname)
        for root, dirs, files in os.walk(dest_dir, topdown=False):
            for d in dirs:
                if d in {"bin", "Samples", "Kits"} or d.startswith("amd_"):
                    dir_path = os.path.join(root, d)
                    try:
                        if os.path.isdir(dir_path) and not os.listdir(dir_path):
                            os.rmdir(dir_path)
                    except OSError:
                        pass
        return list(extracted)

    def _download_and_extract(url: str, zip_name: str, archive_inner_path: str, dlls: List[str]) -> None:
        """Download a SDK zip, extract specific DLLs to testapp_dir."""
        zip_path = _ensure_zip(url, zip_name)

        # Extract only needed DLLs from the archive
        import zipfile

        missing_from_archive = [d for d in dlls if not os.path.exists(os.path.join(testapp_dir, d))]
        if not missing_from_archive:
            return

        log(f"Extracting {len(missing_from_archive)} DLL(s) from {zip_name}...")
        extracted = set()
        with zipfile.ZipFile(zip_path, "r") as zf:
            for entry in zf.infolist():
                fname = os.path.basename(entry.filename)
                if (
                    fname in missing_from_archive
                    and entry.filename.startswith(archive_inner_path)
                    and fname not in extracted
                ):
                    zf.extract(entry, testapp_dir)
                    extracted_path = os.path.normpath(os.path.join(testapp_dir, entry.filename))
                    dest_path = os.path.join(testapp_dir, fname)
                    if os.path.exists(extracted_path) and extracted_path != dest_path:
                        safe_replace_or_rename(extracted_path, dest_path)
                    extracted.add(fname)
        log(f"Extracted {len(extracted)} DLL(s) from {zip_name}")
        # Clean up nested subdirs left by partial extraction (only SDK artifact dirs)
        sdk_parent_dirs = {"bin", "Samples", "Kits"}
        for root, dirs, files in os.walk(testapp_dir, topdown=False):
            for d in dirs:
                if d in sdk_parent_dirs or d.startswith("amd_"):
                    dir_path = os.path.join(root, d)
                    try:
                        if os.path.isdir(dir_path) and not os.listdir(dir_path):
                            os.rmdir(dir_path)
                    except OSError:
                        pass

    def _extract_streamline_headers(zip_path: str) -> None:
        import zipfile

        extracted = 0
        with zipfile.ZipFile(zip_path, "r") as zf:
            for entry in zf.infolist():
                if entry.is_dir() or not entry.filename.startswith("include/"):
                    continue
                if not entry.filename.lower().endswith((".h", ".hpp")):
                    continue
                dest_path = os.path.join(streamline_include_dir, entry.filename)
                os.makedirs(os.path.dirname(dest_path), exist_ok=True)
                with zf.open(entry, "r") as src, open(dest_path, "wb") as dst:
                    shutil.copyfileobj(src, dst)
                extracted += 1
        log(f"Extracted {extracted} Streamline header(s)")

    def _extract_fidelityfx_headers(zip_path: str) -> None:
        import zipfile

        allowed_prefixes = (
            "Kits/FidelityFX/api/include/",
            "Kits/FidelityFX/framegeneration/include/",
            # ffx_upscale.h/.hpp only -- the per-implementation fsr3/ and gpu/ shader headers live
            # under upscalers/fsr3/include/ and are intentionally NOT matched by this prefix.
            "Kits/FidelityFX/upscalers/include/",
        )
        extracted = 0
        with zipfile.ZipFile(zip_path, "r") as zf:
            for entry in zf.infolist():
                if entry.is_dir() or not entry.filename.lower().endswith((".h", ".hpp")):
                    continue
                marker_index = entry.filename.find("Kits/FidelityFX/")
                if marker_index < 0:
                    continue
                rel_path = entry.filename[marker_index:]
                if not rel_path.startswith(allowed_prefixes):
                    continue
                dest_path = os.path.join(fidelityfx_include_dir, rel_path)
                os.makedirs(os.path.dirname(dest_path), exist_ok=True)
                with zf.open(entry, "r") as src, open(dest_path, "wb") as dst:
                    shutil.copyfileobj(src, dst)
                extracted += 1
        log(f"Extracted {extracted} FidelityFX header(s)")

    def _extract_fidelityfx_vulkan_package(zip_path: str) -> None:
        import zipfile

        extracted_headers = 0
        extracted_dll = False
        extracted_license = False
        with zipfile.ZipFile(zip_path, "r") as zf:
            for entry in zf.infolist():
                normalized = entry.filename.replace("\\", "/")
                if entry.is_dir():
                    continue
                if normalized.startswith("ffx-api/include/") and normalized.lower().endswith((".h", ".hpp")):
                    rel_path = normalized[len("ffx-api/include/") :]
                    dest_path = os.path.join(fidelityfx_vk_include_dir, *rel_path.split("/"))
                    os.makedirs(os.path.dirname(dest_path), exist_ok=True)
                    with zf.open(entry, "r") as src, open(dest_path, "wb") as dst:
                        shutil.copyfileobj(src, dst)
                    extracted_headers += 1
                elif include_runtime_dlls and normalized == "PrebuiltSignedDLL/amd_fidelityfx_vk.dll":
                    with zf.open(entry, "r") as src, open(ffx_vk_dll, "wb") as dst:
                        shutil.copyfileobj(src, dst)
                    extracted_dll = True
                elif include_runtime_dlls and normalized == "sdk/LICENSE.txt":
                    with zf.open(entry, "r") as src, open(ffx_vk_license, "wb") as dst:
                        shutil.copyfileobj(src, dst)
                    extracted_license = True
        if not os.path.exists(ffx_vk_header_probe):
            raise RuntimeError("FidelityFX 1.1.4 Vulkan API headers were not found in the pinned archive")
        if include_runtime_dlls:
            if not os.path.exists(ffx_vk_dll) or not pe_has_authenticode_certificate(ffx_vk_dll):
                raise RuntimeError("FidelityFX 1.1.4 Vulkan runtime is missing its Authenticode certificate")
            if not os.path.exists(ffx_vk_license):
                raise RuntimeError("FidelityFX 1.1.4 license was not found in the pinned archive")
        if include_runtime_dlls:
            log(
                f"Prepared FidelityFX 1.1.4 Vulkan package: {extracted_headers} headers, "
                f"DLL={'yes' if extracted_dll else 'cached'}, license={'yes' if extracted_license else 'cached'}"
            )
        else:
            log(f"Prepared FidelityFX 1.1.4 Vulkan headers: {extracted_headers} extracted")

    if missing_fsr:
        _download_and_extract(
            FFX_SDK_URL, FFX_SDK_ZIP_NAME, "Samples/Upscalers/FidelityFX_FSR/dx12/x64/Release/", fsr_dlls
        )

    # Download and extract Streamline DLLs if any missing
    if missing_sl:
        zip_path_sl = _ensure_zip(STREAMLINE_SDK_URL, STREAMLINE_SDK_ZIP_NAME)
        # Streamline: extract ALL .dll files under bin/x64/ (interposer needs companion DLLs + NGX)
        extracted_sl = _extract_all_dlls_from_path(zip_path_sl, "bin/x64/", testapp_dir)
        log(f"Extracted {len(extracted_sl)} Streamline DLL(s)")
        missing_sl = [d for d in sl_known_dlls if not os.path.exists(os.path.join(testapp_dir, d))]

    if not os.path.exists(streamline_header_probe):
        _extract_streamline_headers(_ensure_zip(STREAMLINE_SDK_URL, STREAMLINE_SDK_ZIP_NAME))

    if not os.path.exists(ffx_header_probe) or not os.path.exists(ffx_upscale_header_probe):
        _extract_fidelityfx_headers(_ensure_zip(FFX_SDK_SOURCE_URL, FFX_SDK_SOURCE_ZIP_NAME))

    if missing_vk_headers or missing_vk_runtime:
        _extract_fidelityfx_vulkan_package(_ensure_zip(FFX_VK_SDK_URL, FFX_VK_SDK_ZIP_NAME, FFX_VK_SDK_SHA256))
    elif include_runtime_dlls and not pe_has_authenticode_certificate(ffx_vk_dll):
        raise RuntimeError("Cached FidelityFX 1.1.4 Vulkan runtime has no Authenticode certificate")

    # Copy _nvngx.dll from NVIDIA driver DriverStore if not present
    nvngx_dest = os.path.join(testapp_dir, "_nvngx.dll")
    if include_runtime_dlls and not os.path.exists(nvngx_dest):
        for src in _nvngx_sys_paths:
            if os.path.exists(src):
                shutil.copy2(src, nvngx_dest)
                log("Copied _nvngx.dll from DriverStore")
                break
        else:
            log("_nvngx.dll not found in DriverStore (NVIDIA NGX not activated on this system)")

    # Final report
    if include_runtime_dlls:
        all_expected = fsr_dlls + sl_known_dlls + ["_nvngx.dll"]
        still_missing = [d for d in all_expected if not os.path.exists(os.path.join(testapp_dir, d))]
        if still_missing:
            log(f"Warning: some FG DLLs could not be extracted: {still_missing}")
        else:
            log("All FG SDK DLLs ready for test apps")
    else:
        log("All FG SDK headers ready for cross-compiled test apps")


def setup_fg_sdk_headers(skip_updates: bool = False) -> None:
    """Prepare only the SDK headers required to cross-compile FG test apps."""
    setup_fg_sdk_dependencies(skip_updates=skip_updates, include_runtime_dlls=False)


def setup_fg_sdk_dlls(skip_updates: bool = False) -> None:
    """Prepare SDK headers and Windows runtime DLLs for native test apps."""
    setup_fg_sdk_dependencies(skip_updates=skip_updates, include_runtime_dlls=True)


def setup_fg_sdk_for_host(skip_updates: bool = False) -> None:
    """Prepare compile-time SDK inputs and native-only runtime payloads."""
    if IS_LINUX:
        setup_fg_sdk_headers(skip_updates=skip_updates)
    else:
        setup_fg_sdk_dlls(skip_updates=skip_updates)


def safe_replace_or_rename(src: str, dst: str) -> None:
    """Rename or copy+delete src to dst, handling cross-device moves."""
    try:
        os.replace(src, dst)
    except OSError:
        shutil.copy2(src, dst)
        os.remove(src)


def get_fg_sdk_include_flags() -> List[str]:
    """Return generated SDK include paths used by the DX12 FG test apps."""
    streamline_include = os.path.join(FG_SDK_INCLUDE_DIR, "streamline", "include")
    fidelityfx_root = os.path.join(FG_SDK_INCLUDE_DIR, "fidelityfx", "Kits", "FidelityFX")
    return [
        "-I" + streamline_include,
        "-I" + os.path.join(fidelityfx_root, "api", "include"),
        "-I" + os.path.join(fidelityfx_root, "framegeneration", "include"),
        "-I" + os.path.join(fidelityfx_root, "upscalers", "include"),
    ]


def get_vulkan_fg_sdk_include_flags() -> List[str]:
    """Return the isolated Streamline 2.11.1 + FidelityFX 1.1.4 Vulkan include paths."""
    return [
        "-I" + os.path.join(FG_SDK_INCLUDE_DIR, "streamline", "include"),
        "-I" + os.path.join(FG_SDK_INCLUDE_DIR, "fidelityfx_vk_v1_1_4"),
    ]


def compile_vulkan_fg_shaders(env: Dict[str, str]) -> str:
    """Compile, validate, and embed Vulkan FG shaders. Returns the generated include directory."""
    shader_dir = os.path.join(PROJECT_ROOT, "testapp", "shaders")
    output_dir = os.path.join(OBJ_DIR, "vulkan_fg_shaders")
    os.makedirs(output_dir, exist_ok=True)
    glslang, spirv_val = get_vulkan_fg_shader_tools()

    shader_specs = [
        ("vulkan_fg_fullscreen.vert", "vert", "kFullscreenVertexSpirv"),
        ("vulkan_fg_scene.frag", "frag", "kSceneFragmentSpirv"),
        ("vulkan_fg_taa.frag", "frag", "kTaaFragmentSpirv"),
        ("vulkan_fg_ui.frag", "frag", "kUiFragmentSpirv"),
        ("vulkan_fg_compose.frag", "frag", "kComposeFragmentSpirv"),
        ("vulkan_fg_present.frag", "frag", "kPresentFragmentSpirv"),
    ]
    embedded: List[tuple[str, bytes]] = []
    compiled_count = 0
    for source_name, stage, symbol in shader_specs:
        source_path = os.path.join(shader_dir, source_name)
        spv_path = os.path.join(output_dir, source_name + ".spv")
        cache_path = spv_path + ".build-cache.json"
        if not os.path.exists(source_path):
            raise RuntimeError(f"Missing Vulkan FG shader source: {source_path}")
        compile_command = [glslang, "-V", "--target-env", "vulkan1.2", "-S", stage, source_path, "-o", spv_path]
        signature_digest = hashlib.sha256()
        signature_digest.update(sha256_file(source_path).encode("ascii"))
        signature_digest.update(fingerprint_link_input(glslang).encode("ascii"))
        signature_digest.update(fingerprint_link_input(spirv_val).encode("ascii"))
        signature_digest.update("\0".join(compile_command[1:]).encode("utf-8", errors="surrogatepass"))
        input_signature = signature_digest.hexdigest()
        cache_valid = False
        if env.get("FORCE_REBUILD") != "1" and os.path.isfile(spv_path):
            try:
                with open(cache_path, "r", encoding="utf-8") as cache_file:
                    cache = json.load(cache_file)
                cache_valid = (
                    cache.get("input_signature") == input_signature
                    and cache.get("output_sha256") == sha256_file(spv_path)
                )
            except (OSError, json.JSONDecodeError):
                cache_valid = False
        if not cache_valid:
            run_command(compile_command, env=env)
            write_json_atomic(
                cache_path,
                {
                    "input_signature": input_signature,
                    "output_sha256": sha256_file(spv_path),
                },
            )
            compiled_count += 1
        run_command([spirv_val, "--target-env", "vulkan1.2", spv_path], env=env)
        with open(spv_path, "rb") as src:
            payload = src.read()
        if not payload or len(payload) % 4 != 0:
            raise RuntimeError(f"Invalid SPIR-V payload generated for {source_name}")
        embedded.append((symbol, payload))

    header_lines = [
        "// Generated by build.py from validated GLSL. Do not edit.\n#pragma once\n\n#include <cstdint>\n\n",
        "namespace testapp::vkfg::shaders {\n",
    ]
    for symbol, payload in embedded:
        words = [int.from_bytes(payload[offset : offset + 4], "little") for offset in range(0, len(payload), 4)]
        header_lines.append(f"inline constexpr uint32_t {symbol}[] = {{\n")
        for offset in range(0, len(words), 8):
            chunk = ", ".join(f"0x{word:08x}u" for word in words[offset : offset + 8])
            header_lines.append(f"    {chunk},\n")
        header_lines.append("};\n")
    header_lines.append("}  // namespace testapp::vkfg::shaders\n")
    header_path = os.path.join(output_dir, "vulkan_fg_shaders.h")
    header_changed = write_text_atomic_if_changed(header_path, "".join(header_lines))
    log(
        f"Vulkan FG shaders: {compiled_count} compiled, {len(shader_specs) - compiled_count} cached, "
        f"{len(shader_specs)} validated, header {'updated' if header_changed else 'unchanged'}"
    )
    return output_dir


def check_python_lsp_tools() -> bool:
    """Check and install Python LSP/lint/format tools for better IDE support."""
    bootstrap_env = apply_workspace_temp_environment(os.environ.copy())
    bootstrap_ok = True

    user_scripts_dir = os.path.join(site.getuserbase(), "Scripts" if IS_WINDOWS else "bin")
    if user_scripts_dir not in bootstrap_env.get("PATH", ""):
        bootstrap_env["PATH"] = user_scripts_dir + os.pathsep + bootstrap_env.get("PATH", "")

    try:
        subprocess.run(
            [sys.executable, "-m", "pip", "--version"],
            capture_output=True,
            check=True,
            env=bootstrap_env,
        )
    except (subprocess.CalledProcessError, FileNotFoundError):
        log("pip not found. Bootstrapping with ensurepip...")
        try:
            run_logged_subprocess(
                [sys.executable, "-m", "ensurepip", "--upgrade"],
                check=True,
                env=bootstrap_env,
            )
        except subprocess.CalledProcessError as e:
            log(f"Warning: Failed to bootstrap pip: {e}")
            log("Python lint tooling cannot be prepared; the lint stage will report the missing tools.")
            return False

    tools = ["pyright", "flake8", "black"]

    for tool in tools:
        try:
            subprocess.run(
                [sys.executable, "-m", tool, "--version"],
                capture_output=True,
                check=True,
                env=bootstrap_env,
            )
        except (subprocess.CalledProcessError, FileNotFoundError):
            log(f"{tool} not found. Installing via pip...")
            try:
                cmd = [
                    sys.executable,
                    "-m",
                    "pip",
                    "install",
                    "--disable-pip-version-check",
                    "--upgrade-strategy",
                    "only-if-needed",
                ]
                if not is_virtual_environment():
                    cmd.append("--user")
                if IS_LINUX and not is_virtual_environment():
                    cmd.append("--break-system-packages")
                cmd.append(tool)
                run_logged_subprocess(cmd, check=True, env=bootstrap_env)
                log(f"{tool} installed successfully.")
            except subprocess.CalledProcessError as e:
                bootstrap_ok = False
                log(f"Warning: Failed to install {tool}: {e}")
                log(f"{tool} remains unavailable; the lint stage will report the missing managed tool.")
    return bootstrap_ok


def get_linux_ffmpeg_root() -> str:
    local_import_lib = os.path.join(FFMPEG_DIR, "lib", "libavcodec.dll.a")
    if os.path.exists(local_import_lib):
        return FFMPEG_DIR
    return os.path.join(get_linux_msys2_dir(), "clang64")


def find_ffmpeg_include_dir(ffmpeg_root: str) -> str:
    include_dir = os.path.join(ffmpeg_root, "include")
    if not os.path.isdir(include_dir):
        return include_dir

    if os.path.exists(os.path.join(include_dir, "libavcodec", "avcodec.h")):
        return include_dir

    for entry in sorted(os.listdir(include_dir)):
        candidate = os.path.join(include_dir, entry)
        if entry.startswith("ffmpeg") and os.path.isdir(candidate):
            return candidate

    return include_dir


def get_linux_ffmpeg_build_flags(env: Dict[str, str], pkg_config: Optional[str]) -> tuple[List[str], List[str]]:
    ffmpeg_root = get_linux_ffmpeg_root()
    ffmpeg_include = find_ffmpeg_include_dir(ffmpeg_root)
    ffmpeg_lib = os.path.join(ffmpeg_root, "lib")
    msys2_include_root = os.path.join(get_linux_msys2_dir(), "clang64", "include")
    env_ffmpeg = env.copy()
    env_ffmpeg["PKG_CONFIG_PATH"] = os.pathsep.join(
        [os.path.join(ffmpeg_lib, "pkgconfig"), env_ffmpeg.get("PKG_CONFIG_PATH", "")]
    ).rstrip(os.pathsep)

    pkgs = ["libavcodec", "libavformat", "libavutil", "libswresample", "libswscale"]
    ffmpeg_flags: List[str]
    if pkg_config:
        try:
            ffmpeg_flags_raw = run_command([pkg_config, "--cflags", "--libs"] + pkgs, env=env_ffmpeg).strip().split()

            ffmpeg_flags = []
            msys2_dir = get_linux_msys2_dir()
            for flag in ffmpeg_flags_raw:
                if flag in ["-ldl", "-lshaderc_shared"]:
                    continue
                if flag.startswith("-I/") and not os.path.exists(flag[2:]):
                    abs_path = os.path.join(msys2_dir, flag[2:].lstrip("/"))
                    if os.path.exists(abs_path):
                        flag = "-I" + abs_path
                elif flag.startswith("-L/") and not os.path.exists(flag[2:]):
                    abs_path = os.path.join(msys2_dir, flag[2:].lstrip("/"))
                    if os.path.exists(abs_path):
                        flag = "-L" + abs_path
                if flag == "-I" + msys2_include_root:
                    ffmpeg_flags.extend(["-idirafter", msys2_include_root])
                else:
                    ffmpeg_flags.append(flag)
        except Exception as e:
            log(f"pkg-config failed, using manual FFmpeg paths: {e}")
            ffmpeg_flags = ["-I" + ffmpeg_include, "-L" + ffmpeg_lib]
    else:
        ffmpeg_flags = ["-I" + ffmpeg_include, "-L" + ffmpeg_lib]

    ffmpeg_import_lib_names = [
        "libavformat.dll.a",
        "libavcodec.dll.a",
        "libswresample.dll.a",
        "libswscale.dll.a",
        "libavutil.dll.a",
    ]
    ffmpeg_import_libs = []
    for import_lib_name in ffmpeg_import_lib_names:
        import_lib_path = os.path.join(ffmpeg_lib, import_lib_name)
        if os.path.exists(import_lib_path):
            ffmpeg_import_libs.append(import_lib_path)

    if len(ffmpeg_import_libs) != len(ffmpeg_import_lib_names):
        ffmpeg_import_libs = [
            "-L" + ffmpeg_lib,
            "-lavformat",
            "-lavcodec",
            "-lswresample",
            "-lswscale",
            "-lavutil",
        ]

    return ffmpeg_flags, ffmpeg_import_libs


# MSYS2 packages to download for Linux builds (Windows-specific libs not in Arch repos)
LINUX_MSYS2_PACKAGES = [
    "mingw-w64-clang-x86_64-vulkan-headers",
    "mingw-w64-clang-x86_64-vulkan-loader",
    "mingw-w64-clang-x86_64-spirv-headers",
    "mingw-w64-clang-x86_64-gtest",
    "mingw-w64-clang-x86_64-ffmpeg",
    "mingw-w64-clang-x86_64-cppwinrt",
    "mingw-w64-clang-x86_64-headers",
    # FFmpeg dependencies (required by external/ffmpeg/bin DLLs)
    "mingw-w64-clang-x86_64-libxml2",
    "mingw-w64-clang-x86_64-bzip2",
    "mingw-w64-clang-x86_64-libmodplug",
    "mingw-w64-clang-x86_64-libgme",
    "mingw-w64-clang-x86_64-libva",
    "mingw-w64-clang-x86_64-libvpl",
    "mingw-w64-clang-x86_64-svt-av1",
    "mingw-w64-clang-x86_64-opus",
    "mingw-w64-clang-x86_64-libwinpthread",
    "mingw-w64-clang-x86_64-crt",
]

LINUX_MSYS2_REQUIRED_SENTINELS = [
    os.path.join("clang64", "share", "licenses", "crt", "COPYING.MinGW-w64-runtime.txt"),
]

MSYS2_REPO_URL = "https://repo.msys2.org/mingw/clang64"
MSYS2_MINGW64_REPO_URL = "https://repo.msys2.org/mingw/mingw64"


def get_linux_msys2_gtest_subdir(arch: str = "x64", compiler_exe: Optional[str] = None) -> str:
    if arch == "x86":
        return "mingw32"
    compiler = compiler_exe or get_compiler_exe(arch)
    return "clang64" if is_clang_compiler(compiler) else "mingw64"


def get_linux_msys2_gtest_package_name(arch: str = "x64") -> str:
    subdir = get_linux_msys2_gtest_subdir(arch)
    if subdir == "clang64":
        return "mingw-w64-clang-x86_64-gtest"
    if subdir == "mingw64":
        return "mingw-w64-x86_64-gtest"
    return "mingw-w64-i686-gtest"
