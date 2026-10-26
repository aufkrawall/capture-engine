import ctypes

# Build-time HLSL compilation for the media engine (mediaengine/video/precompiled_shaders.h).
#
# Recording start used to run d3dcompiler_47 for every runtime shader: about one
# second of CPU at the first recording of each app start, 0.65 s of it the
# retained-YUV overlay patch. The build now compiles the exact requests the
# media engine makes, with the same System32 compiler it loads at runtime, and
# embeds them in <obj>/generated/ce_precompiled_shaders.h. The runtime looks a
# request up by request_key() over its own source text, so a stale or missing
# entry only means a runtime compile, never wrong bytecode.

PRECOMPILED_SHADER_HEADER = "ce_precompiled_shaders.h"
D3DCOMPILE_OPTIMIZATION_LEVEL3 = 1 << 15

# Raw-string HLSL sources: (repo path, symbol).
SHADER_SOURCES = {
    "conversion": ("mediaengine/video/video_color_conversion_shader.h", "kRgbColorConversionShaderSource"),
    "packed": ("mediaengine/video/packed_p010_shader.h", "kPackedP010ShaderSource"),
    "patch": ("mediaengine/video/yuv_overlay_patch_shader.h", "kYuvOverlayPatchShaderSource"),
    "patch_packed_define": ("mediaengine/video/yuv_overlay_patch_shader.h", "kYuvOverlayPatchPackedDefine"),
    "cursor": ("mediaengine/video/cursor_renderer.cpp", "CURSOR_SHADER_SOURCE"),
    "face_camera": ("mediaengine/video/face_camera_shader.h", "kShaderSource"),
    "scaler": ("mediaengine/video/frame_scaler_shader.h", "kFrameScalerShaderSource"),
}

# Every PrepareShaderBytecode request of the media engine: (owner, concatenated sources, entries, flags).
# PrecompiledShaderTest fails when a call site asks for something this list does not produce.
SHADER_REQUESTS = [
    (
        "RGBConvert",
        ["conversion"],
        [("VS_Main", "vs_4_0"), ("PS_Main", "ps_4_0"), ("PS_P010Y", "ps_4_0"), ("PS_P010UV", "ps_4_0"),
         ("PS_YuvY", "ps_4_0"), ("PS_YuvUV", "ps_4_0")],
        0,
    ),
    (
        "RetainedYuv",
        ["conversion", "packed"],
        [("VS_Main", "vs_4_0"), ("PS_Main", "ps_4_0"), ("PS_P010Y", "ps_4_0"), ("PS_P010UV", "ps_4_0"),
         ("PS_YuvY", "ps_4_0"), ("PS_YuvUV", "ps_4_0"), ("PS_PackSdrP010", "ps_4_0"), ("PS_PackHdrP010", "ps_4_0")],
        0,
    ),
    ("PackedP010", ["conversion", "packed"], [("VS_Main", "vs_4_0"), ("PS_UnpackP010Y", "ps_4_0"),
                                              ("PS_UnpackP010UV", "ps_4_0")], 0),
    (
        "YuvOverlayPatch",
        ["conversion", "packed", "patch"],
        [("VS_Main", "vs_5_0"), ("PS_PatchLuma", "ps_5_0"), ("PS_PatchChroma", "ps_5_0")],
        0,
    ),
    (
        "YuvOverlayPatch packed",
        ["patch_packed_define", "conversion", "packed", "patch"],
        [("PS_PatchLuma", "ps_5_0"), ("PS_PatchChroma", "ps_5_0")],
        0,
    ),
    ("Scaler", ["scaler"], [("VS_Main", "vs_4_0"), ("PS_ScaleH", "ps_4_0"), ("PS_ScaleV", "ps_4_0")], 0),
    ("CursorRenderer", ["cursor"], [("VS_Main", "vs_4_0"), ("PS_Main", "ps_4_0")], 0),
    (
        "FaceCamera",
        ["face_camera"],
        [("VS_Main", "vs_4_0"), ("PS_Main", "ps_4_0")],
        D3DCOMPILE_OPTIMIZATION_LEVEL3,
    ),
]


def read_raw_shader_source(path: str, symbol: str) -> str:
    """The text of `symbol = R"(...)";` in a repo file, exactly as the compiler sees it."""
    with open(os.path.join(PROJECT_ROOT, path), "r", encoding="utf-8", newline="") as handle:
        text = handle.read()
    matches = re.findall(r"\b" + re.escape(symbol) + r'\b(?:\[\])?\s*=\s*R"\((.*?)\)";', text, re.S)
    if len(matches) != 1:
        raise RuntimeError(f"Shader source {symbol} must be one R\"(...)\" literal in {path} (found {len(matches)})")
    return matches[0]


def request_key(source: bytes, entry: str, target: str, flags: int) -> int:
    """FNV-1a 64 over source, entry, target and flags; mirrors ce::precompiled_shaders::RequestKey."""
    value = 0xCBF29CE484222325
    data = source + b"\0" + entry.encode() + b"\0" + target.encode() + b"\0" + flags.to_bytes(4, "little")
    for byte in data:
        value ^= byte
        value = (value * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return value


def collect_shader_requests() -> List[Tuple[str, bytes, str, str, int]]:
    """(owner, source, entry, target, flags), deduplicated by request key."""
    texts = {name: read_raw_shader_source(path, symbol) for name, (path, symbol) in SHADER_SOURCES.items()}
    requests: List[Tuple[str, bytes, str, str, int]] = []
    seen: Set[int] = set()
    for owner, parts, entries, flags in SHADER_REQUESTS:
        source = "".join(texts[part] for part in parts).encode("utf-8")
        for entry, target in entries:
            key = request_key(source, entry, target, flags)
            if key not in seen:
                seen.add(key)
                requests.append((owner, source, entry, target, flags))
    return requests


def system_shader_compiler_path() -> Optional[str]:
    if os.name != "nt":
        return None
    path = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", "d3dcompiler_47.dll")
    return path if os.path.isfile(path) else None


@lru_cache(maxsize=None)
def load_shader_compiler(compiler_path: str) -> Any:
    return ctypes.WinDLL(compiler_path)  # type: ignore[attr-defined]


def compile_hlsl(compiler_path: str, source: bytes, entry: str, target: str, flags: int) -> bytes:
    """D3DCompile through the system compiler, with the arguments PrepareShaderBytecode passes."""
    d3d_compile = load_shader_compiler(compiler_path).D3DCompile
    d3d_compile.restype = ctypes.c_long
    d3d_compile.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_void_p,
                            ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint, ctypes.c_uint,
                            ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_void_p)]
    blob = ctypes.c_void_p()
    errors = ctypes.c_void_p()
    hr = d3d_compile(source, len(source), None, None, None, entry.encode(), target.encode(), flags, 0,
                     ctypes.byref(blob), ctypes.byref(errors))

    def blob_method(pointer: ctypes.c_void_p, index: int, restype: Any) -> Any:
        vtable = ctypes.cast(ctypes.cast(pointer, ctypes.POINTER(ctypes.c_void_p))[0], ctypes.POINTER(ctypes.c_void_p))
        return ctypes.WINFUNCTYPE(restype, ctypes.c_void_p)(vtable[index])  # type: ignore[attr-defined]

    message = ""
    if errors.value:
        message = ctypes.string_at(blob_method(errors, 3, ctypes.c_void_p)(errors),
                                   blob_method(errors, 4, ctypes.c_size_t)(errors)).decode("utf-8", "replace")
        blob_method(errors, 2, ctypes.c_ulong)(errors)
    if hr != 0 or not blob.value:
        raise RuntimeError(f"HLSL compile failed: {entry}/{target} hr=0x{hr & 0xFFFFFFFF:08x} {message.strip()}")
    payload = ctypes.string_at(blob_method(blob, 3, ctypes.c_void_p)(blob), blob_method(blob, 4, ctypes.c_size_t)(blob))
    blob_method(blob, 2, ctypes.c_ulong)(blob)
    return payload


def render_precompiled_shader_header(compiled: List[Tuple[str, bytes, str, str, int, bytes]], compiler: str) -> str:
    lines = [
        "// Generated by build.py (tools/build/build_shaders.py) from the media engine's HLSL. Do not edit.\n",
        f"// Compiler: {compiler}\n",
        "#pragma once\n\n#include <cstddef>\n#include <cstdint>\n\n",
        "namespace ce::precompiled_shaders::generated {\n\n",
        "struct Entry {\n    uint64_t key;\n    size_t sourceLength;\n    const char* entry;\n    const char* target;\n",
        "    uint32_t flags;\n    const uint8_t* data;\n    size_t size;\n};\n\n",
    ]
    for index, (owner, _source, entry, target, _flags, payload) in enumerate(compiled):
        lines.append(f"// {owner} {entry}/{target}: {len(payload)} bytes\n")
        lines.append(f"inline constexpr uint8_t kShader{index}[] = {{\n")
        for offset in range(0, len(payload), 16):
            lines.append("    " + ", ".join(f"0x{byte:02x}" for byte in payload[offset : offset + 16]) + ",\n")
        lines.append("};\n")
    lines.append(f"\ninline constexpr size_t kEntryCount = {len(compiled)};\n")
    lines.append("inline constexpr Entry kEntries[] = {\n")
    for index, (_owner, source, entry, target, flags, _payload) in enumerate(compiled):
        key = request_key(source, entry, target, flags)
        lines.append(f'    {{0x{key:016x}ull, {len(source)}u, "{entry}", "{target}", {flags}u, kShader{index}, '
                     f"sizeof(kShader{index})}},\n")
    if not compiled:
        lines.append("    {0, 0, \"\", \"\", 0, nullptr, 0},  // no build-time compiler: every request compiles at runtime\n")
    lines.append("};\n\n}  // namespace ce::precompiled_shaders::generated\n")
    return "".join(lines)


def generate_precompiled_shaders(env: Dict[str, str]) -> str:
    """Compile the media engine's runtime shaders into the generated header; returns its directory."""
    header_path = os.path.join(GENERATED_INCLUDE_DIR, PRECOMPILED_SHADER_HEADER)
    cache_path = header_path + ".build-cache.json"
    requests = collect_shader_requests()
    compiler_path = system_shader_compiler_path()
    digest = hashlib.sha256()
    digest.update(sha256_file(compiler_path).encode("ascii") if compiler_path else b"no-compiler")
    for owner, source, entry, target, flags in requests:
        digest.update(f"{owner}\0{entry}\0{target}\0{flags}\0".encode())
        digest.update(hashlib.sha256(source).digest())
    signature = digest.hexdigest()
    if env.get("FORCE_REBUILD") != "1" and os.path.isfile(header_path):
        try:
            with open(cache_path, "r", encoding="utf-8") as cache_file:
                cache = json.load(cache_file)
            if cache.get("input_signature") == signature and cache.get("header_sha256") == sha256_file(header_path):
                log(f"Precompiled shaders: {len(requests)} cached ({PRECOMPILED_SHADER_HEADER} unchanged)")
                return GENERATED_INCLUDE_DIR
        except (OSError, json.JSONDecodeError):
            pass

    start = time.time()
    compiled: List[Tuple[str, bytes, str, str, int, bytes]] = []
    if compiler_path:
        for owner, source, entry, target, flags in requests:
            compiled.append((owner, source, entry, target, flags, compile_hlsl(compiler_path, source, entry, target,
                                                                                flags)))
        compiler = f"{compiler_path} sha256={sha256_file(compiler_path)}"
    else:
        compiler = "none (non-Windows build host); the media engine compiles at runtime"
        log("WARNING: no d3dcompiler_47.dll on this build host; media engine shaders compile at recording start")
    changed = write_text_atomic_if_changed(header_path, render_precompiled_shader_header(compiled, compiler))
    write_text_atomic_if_changed(
        cache_path, json.dumps({"input_signature": signature, "header_sha256": sha256_file(header_path)}, indent=2)
    )
    total = sum(len(item[5]) for item in compiled)
    log(
        f"Precompiled shaders: {len(compiled)}/{len(requests)} compiled ({total} bytes) in {time.time() - start:.1f}s, "
        f"header {'updated' if changed else 'unchanged'}"
    )
    return GENERATED_INCLUDE_DIR
