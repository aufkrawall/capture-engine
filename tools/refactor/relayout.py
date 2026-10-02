"""One-shot source-tree relayout: flat module directories -> subsystem directories.

Usage:
    python tools/refactor/relayout.py plan            # print the mapping as a tree
    python tools/refactor/relayout.py includes        # resolve includes against the OLD layout (dry run)
    python tools/refactor/relayout.py apply           # git mv + rewrite includes and path references

Contract: basenames never change, only directories. Every quoted include that names a
first-party file is re-spelled after the move: same directory -> bare name, otherwise
repo-root-relative ("hook/d3d12/dx12_hook_internal.h"). The resolver emulates clang's
quoted-include search (includer directory, then the TU's -I list in order) for every
translation unit in compile_commands.json and refuses ambiguous spellings.
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
from collections import defaultdict

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

SOURCE_EXTS = (".cpp", ".h", ".hpp", ".c", ".inl")

# Ordered (old-path regex, new directory). First match wins. Paths are repo-relative, '/'.
# A "detail" subdirectory (e.g. hook/common/fps_limiter_detail/) follows its parent header.
RULES: list[tuple[str, str]] = [
    # ---------------------------------------------------------------- hook/
    (r"^hook/main(_[a-z0-9_]+)?\.(cpp|h)$", "hook/runtime"),
    (r"^hook/main_ue5[a-z0-9_]*\.(cpp|h)$", "hook/runtime"),
    # hooking primitives
    (r"^hook/wrappers/(inline_hook|iat_hook|iat_import_table|vtable_hook|hook_patch_transaction|hook_system|custom_hook)", "hook/hooking"),
    (r"^hook/common/(hook_jump_policy|vtable_slot_owner|module_export_resolver|module_pin)", "hook/hooking"),
    # COM / API wrappers
    (r"^hook/wrappers/", "hook/wrappers"),
    # per-API hooks
    (r"^hook/apis/dx12_", "hook/d3d12"),
    (r"^hook/common/(dx12_|d3d12_device_creation_policy)", "hook/d3d12"),
    (r"^hook/apis/dx11_", "hook/d3d11"),
    (r"^hook/common/d3d10_vtable_slots", "hook/d3d11"),
    (r"^hook/apis/dx9_", "hook/d3d9"),
    (r"^hook/common/d3d9_capture_policy", "hook/d3d9"),
    (r"^hook/apis/dx8_", "hook/d3d8"),
    (r"^hook/apis/(ddraw_|legacy_d3d_|lod_helper)", "hook/ddraw"),
    (r"^hook/common/(ddraw_|legacy_d3d_|custom_overlay_d3d7)", "hook/ddraw"),
    (r"^hook/apis/opengl_", "hook/opengl"),
    (r"^hook/common/gl_overlay_state_policy", "hook/opengl"),
    (r"^hook/apis/graphics_hook\.h$", "hook/runtime"),
    # frame generation runtimes
    (r"^hook/apis/streamline_", "hook/streamline"),
    (r"^hook/common/streamline_(api_generation|compat|feature_retry_policy|runtime_policy)", "hook/streamline"),
    (r"^hook/apis/ffx_", "hook/ffx"),
    (r"^hook/common/ffx_", "hook/ffx"),
    (r"^hook/apis/(nvngx_|remix_)", "hook/ngx"),
    (r"^hook/common/(nvngx_|ngx_|dlss_indicator_spoof|dlssg_health_policy|nv_lod_spread_override|remix_frame_generation_policy|rr_handoff_gate)", "hook/ngx"),
    (r"^hook/common/fg_", "hook/fg"),
    # central present routing and swapchain lifetime
    (r"^hook/common/(dxgi_shared|dxgi_factory_policy|dxgi_color_space_hook_policy|dxgi_presentation_color|presentation_color|dxgi_video_memory_log_policy)", "hook/present"),
    (r"^hook/common/(present_|swapchain_|resize_|deferred_swapchain_create_ledger|backbuffer_reference_trace|steam_recovery_armed_threads)", "hook/present"),
    (r"^hook/common/(vulkan_dxgi_fifo_|vulkan_wsi_surface_table|vulkan_layer_metering_bridge|vulkan_renderer_policy)", "hook/present"),
    # overlay
    (r"^hook/common/(custom_overlay|overlay_|cached_overlay_renderer|custom_font|graph_scroll_policy|legacy_overlay_cache)", "hook/overlay"),
    # pacing, latency, metrics
    (r"^hook/common/(fps_limiter|reflex_limiter|reflex_defs|pacing_|capture_pacing)", "hook/pacing"),
    (r"^hook/common/(streamline_pcl_latency|system_metrics|system_latency_|performance_metrics|perf_logger|benchmark_|hook_thread_stage_cost|hook_cost_window|hook_cpu_cost)", "hook/metrics"),
    # graphics overrides
    (r"^hook/common/(sampler_override|mip_bias_range|ue5_|hook_common_graphics_config|hook_common_vsync|published_graphics_config)", "hook/overrides"),
    (r"^hook/common/sharpen_", "hook/sharpen"),
    # capture
    (r"^hook/capture/", "hook/capture"),
    (r"^hook/common/(capture_base|screenshot_)", "hook/capture"),
    # everything else under hook/common is runtime plumbing
    (r"^hook/common/", "hook/runtime"),
    # ------------------------------------------------------- captureengine/
    (r"^captureengine/(main|tray|hotkey_input_hook|status_overlay_sync|host_metrics|windows_gpu_scheduling|mediaengine_loader)", "captureengine/app"),
    (r"^captureengine/(injection|inject_|process_start_poll)", "captureengine/injection"),
    (r"^captureengine/(media_main|wgc_capture|dxgi_dup_capture|screenshot|screen_grab_privacy_runtime|process_loopback_worker_host|capture_cadence_diagnostics|encoder_loop_stage_cost|recording_manifest)", "captureengine/media"),
    (r"^captureengine/display_timing_", "captureengine/display_timing"),
    (r"^captureengine/(sensor_|clr_interop|pawnio_)", "captureengine/sensors"),
    (r"^captureengine/(elevation_|startup_|service_lifetime_wait)", "captureengine/elevation"),
    (r"^captureengine/(dump_helper|logger_service)", "captureengine/diagnostics"),
    (r"^captureengine/pseudo_overlay", "captureengine/pseudo_overlay"),
    # --------------------------------------------------------------- common/
    (r"^common/(config|live_stream_config|face_camera_config|benchmark_config)", "common/config"),
    (r"^common/(process_ipc|shared_defs|inject_transport_snapshot|elevation_|display_timing_shared|av_sync_latency_channel)", "common/ipc"),
    (r"^common/(crash_|wer_dump_adoption|wow64_stack_range_policy|cpp_exception_message)", "common/crash"),
    (r"^common/(logging|log_meter|log_privacy)", "common/logging"),
    (r"^common/(capture_|cfr_rational_grid|frame_queue|frame_timing|rate_window_utils|cursor_capture_state|reserved_capture_output|output_completion_notification|recording_lifecycle|wgc_pool_lease|inject_frame_|gpu_scheduling_policy|screen_grab_privacy)", "common/capture"),
    (r"^common/(inject_overlay_policy|pseudo_overlay_|recording_indicator_policy|hotkey_matcher|keyboard_hook_policy)", "common/overlay"),
    (r"^common/(mip_bias_limits|mip_mapping_policy|sharpen_policy|vulkan_layer_)", "common/graphics"),
    (r"^common/(startup_policy|installer_setup_policy)", "common/setup"),
    (r"^common/utils/", "common/platform"),
    (r"^common/", "common/platform"),
    # ----------------------------------------------------------- mediaengine/
    (r"^mediaengine/mediaengine", "mediaengine/engine"),
    (r"^mediaengine/(audio_|app_audio_|process_loopback|process_audio_session_monitor|process_tree_selection)", "mediaengine/audio"),
    (r"^mediaengine/(video_|face_camera_|cursor_|encode_geometry_policy)", "mediaengine/video"),
    (r"^mediaengine/(matroska_timing|mux_)", "mediaengine/mux"),
]

# Detail directories: <old dir> -> they move under the new directory of <parent file>.
DETAIL_DIRS = {
    "hook/common/dx12_overlay_policy": "hook/common/dx12_overlay_policy.h",
    "hook/common/dxgi_shared_detail": "hook/common/dxgi_shared.h",
    "hook/common/fps_limiter_detail": "hook/common/fps_limiter.h",
    "hook/common/overlay_compat_detail": "hook/common/overlay_compat.h",
    "hook/common/reflex_limiter_detail": "hook/common/reflex_limiter.h",
    "hook/common/sampler_override": "hook/common/sampler_override_utils.h",
    "hook/common/overlay_shader_bytecode": "hook/common/overlay_shader_bytecode.h",
    "hook/common/sharpen_shader_bytecode": "hook/common/sharpen_shader_bytecode.h",
    "hook/common/sharpen_shader_spirv": "hook/common/sharpen_shader_spirv.h",
    "common/shared_defs_detail": "common/shared_defs.h",
    "common/capture_policy": "common/capture_pipeline_policy.h",
    "mediaengine/audio_sync": "mediaengine/audio_sync_utils.h",
    "mediaengine/process_loopback": "mediaengine/process_loopback_capture.h",
}

# Directories whose files are not relaid out (they keep their place).
UNMOVED_PREFIXES = ("hook/vulkan_layer/", "hook/shaders/", "hook/.clang-format-ignore")
MODULE_ROOTS = ("hook/", "captureengine/", "common/", "mediaengine/")
UNMOVED_MODULE_FILES = {
    # assets/resources stay beside the .rc that references them by relative path
    "captureengine/captureengine.rc",
    "captureengine/captureengine.manifest",
    "captureengine/resource.h",
    "captureengine/config.ini.template",
    "captureengine/icon_idle.ico",
    "captureengine/icon_recording.ico",
    "captureengine/icon_shutdown.ico",
    "captureengine/.clang-format-ignore",
}


def git_files() -> list[str]:
    out = subprocess.run(["git", "ls-files"], cwd=ROOT, capture_output=True, text=True, check=True).stdout
    return [line for line in out.splitlines() if line]


def target_dir(path: str) -> str | None:
    for detail, parent in DETAIL_DIRS.items():
        if path.startswith(detail + "/"):
            parent_new = target_dir(parent)
            assert parent_new, f"detail parent {parent} unmapped"
            sub = os.path.relpath(os.path.dirname(path), os.path.dirname(detail)).replace("\\", "/")
            return parent_new + "/" + sub
    for pattern, new_dir in RULES:
        if re.search(pattern, path):
            return new_dir
    return None


def build_mapping() -> dict[str, str]:
    mapping: dict[str, str] = {}
    for path in git_files():
        if not path.startswith(MODULE_ROOTS) or path.startswith(UNMOVED_PREFIXES) or path in UNMOVED_MODULE_FILES:
            continue
        new_dir = target_dir(path)
        if new_dir is None:
            raise SystemExit(f"unmapped: {path}")
        new_path = new_dir + "/" + os.path.basename(path)
        if new_path != path:
            mapping[path] = new_path
    targets = defaultdict(list)
    for old, new in mapping.items():
        targets[new].append(old)
    clashes = {k: v for k, v in targets.items() if len(v) > 1}
    if clashes:
        raise SystemExit(f"target clashes: {clashes}")
    return mapping


# ------------------------------------------------------------------ include resolution

INCLUDE_RE = re.compile(r'^(\s*#\s*include\s*)"([^"]+)"', re.M)


def norm(path: str) -> str:
    return os.path.normpath(path).replace("\\", "/")


def rel(path: str) -> str:
    return norm(os.path.relpath(path, ROOT))


def tu_include_dirs(entry: dict) -> list[str]:
    args = entry.get("arguments") or entry["command"].split()
    dirs: list[str] = []
    it = iter(range(len(args)))
    for i in it:
        a = args[i]
        if a in ("-I", "-iquote", "-isystem") and i + 1 < len(args):
            dirs.append(args[i + 1])
            next(it, None)
        elif a.startswith("-I") or a.startswith("-iquote"):
            dirs.append(a[2:] if a.startswith("-I") else a[len("-iquote"):])
    return [norm(os.path.join(entry.get("directory", ROOT), d)) for d in dirs]


def resolve_includes() -> dict[tuple[str, str], set[str]]:
    """(includer repo-rel path, spelling) -> set of resolved absolute targets, over all TUs."""
    db = json.load(open(os.path.join(ROOT, "compile_commands.json"), encoding="utf-8"))
    resolved: dict[tuple[str, str], set[str]] = defaultdict(set)
    text_cache: dict[str, list[str]] = {}
    visited: set[str] = set()

    def includes_of(path: str) -> list[str]:
        if path not in text_cache:
            try:
                text = open(path, encoding="utf-8", errors="replace").read()
            except OSError:
                text = ""
            text_cache[path] = [m.group(2) for m in INCLUDE_RE.finditer(text)]
        return text_cache[path]

    for entry in db:
        tu = norm(os.path.join(entry.get("directory", ROOT), entry["file"]))
        idirs = tu_include_dirs(entry)
        seen: set[str] = set()
        stack = [tu]
        while stack:
            f = stack.pop()
            if f in seen:
                continue
            seen.add(f)
            for spelling in includes_of(f):
                hit = None
                for d in [os.path.dirname(f)] + idirs:
                    cand = norm(os.path.join(d, spelling))
                    if os.path.isfile(cand):
                        hit = cand
                        break
                if hit is None:
                    continue
                if rel(f).startswith(".."):
                    continue
                resolved[(rel(f), spelling)].add(hit)
                if not rel(hit).startswith(("..", "build/msys64", "external/")):
                    stack.append(hit)
        visited.update(rel(s) for s in seen)

    # Tracked sources no compile-database TU reaches (excluded or separately built files):
    # resolve them against the hook DLL's include list so they are re-spelled too.
    default_dirs = [norm(os.path.join(ROOT, d)) for d in
                    ("common", "hook/common", "hook/wrappers", "hook/apis", "hook/capture", "mediaengine")]
    for path in git_files():
        if not path.endswith(SOURCE_EXTS) or path in visited or path.startswith(("external/", "build/")):
            continue
        f = norm(os.path.join(ROOT, path))
        for spelling in includes_of(f):
            for d in [os.path.dirname(f)] + default_dirs:
                cand = norm(os.path.join(d, spelling))
                if os.path.isfile(cand):
                    resolved[(path, spelling)].add(cand)
                    break
    return resolved


def new_spelling(includer_new: str, target_new: str) -> str:
    if os.path.dirname(includer_new) == os.path.dirname(target_new):
        return os.path.basename(target_new)
    return target_new


def plan_include_rewrites(mapping: dict[str, str]) -> tuple[dict[str, dict[str, str]], list[str]]:
    resolved = resolve_includes()
    rewrites: dict[str, dict[str, str]] = defaultdict(dict)
    problems: list[str] = []
    for (includer, spelling), targets in sorted(resolved.items()):
        rel_targets = {rel(t) for t in targets}
        first_party = {t for t in rel_targets if not t.startswith(("..", "build/", "external/"))}
        if not first_party:
            continue
        if len(rel_targets) > 1:
            problems.append(f"ambiguous: {includer}: \"{spelling}\" -> {sorted(rel_targets)}")
            continue
        target = next(iter(first_party))
        includer_new = mapping.get(includer, includer)
        target_new = mapping.get(target, target)
        spelled = new_spelling(includer_new, target_new)
        if spelled != spelling:
            rewrites[includer][spelling] = spelled
    return rewrites, problems


# ------------------------------------------------------------------ apply

def apply(mapping: dict[str, str]) -> None:
    rewrites, problems = plan_include_rewrites(mapping)
    if problems:
        print("\n".join(problems))
        raise SystemExit("refusing: ambiguous include resolution")
    # 1) rewrite include spellings in place (old paths), then move.
    for includer, table in rewrites.items():
        path = os.path.join(ROOT, includer)
        with open(path, encoding="utf-8", newline="") as fh:
            text = fh.read()

        def sub(m: re.Match[str]) -> str:
            spelling = m.group(2)
            return f'{m.group(1)}"{table.get(spelling, spelling)}"'

        new_text = INCLUDE_RE.sub(sub, text)
        if new_text != text:
            with open(path, "w", encoding="utf-8", newline="") as fh:
                fh.write(new_text)
    for old, new in sorted(mapping.items()):
        os.makedirs(os.path.join(ROOT, os.path.dirname(new)), exist_ok=True)
        subprocess.run(["git", "mv", old, new], cwd=ROOT, check=True)
    with open(os.path.join(ROOT, "build", "relayout_mapping.json"), "w", encoding="utf-8") as fh:
        json.dump(mapping, fh, indent=1, sort_keys=True)
    print(f"moved {len(mapping)} files, rewrote includes in {len(rewrites)} files")


# ------------------------------------------------------------------ path references

PATH_RE = re.compile(r"(?<![A-Za-z0-9_./\\-])((?:hook|common|mediaengine|captureengine)/[A-Za-z0-9_./]+?\.(?:cpp|hpp|h|inl|c))\b")
# "a" / "b" / "c.cpp" (C++ std::filesystem) or "a", "b", "c.cpp" (os.path.join); first component may be
# wrapped as path("a").
CHAIN_RE = re.compile(r'"(hook|common|mediaengine|captureengine)"(\)?)((?:\s*[,/]\s*"[A-Za-z0-9_.]+")+)')
REFERENCE_GLOBS = (".py", ".cpp", ".h", ".md", ".json", ".txt", ".ps1", ".toml", ".cfg", ".ini", ".yml", ".yaml")


def rewrite_references(text: str, mapping: dict[str, str]) -> tuple[str, int]:
    count = 0

    def path_sub(m: re.Match[str]) -> str:
        nonlocal count
        new = mapping.get(m.group(1))
        if new is None:
            return m.group(0)
        count += 1
        return new

    def chain_sub(m: re.Match[str]) -> str:
        nonlocal count
        rest = m.group(3)
        parts = re.findall(r'"([A-Za-z0-9_.]+)"', rest)
        seps = re.findall(r'(\s*[,/]\s*)"', rest)
        old = "/".join([m.group(1)] + parts)
        new = mapping.get(old)
        if new is None:
            return m.group(0)
        count += 1
        new_parts = new.split("/")
        sep = seps[0] if seps else " / "
        first = f'"{new_parts[0]}"{m.group(2)}'
        return first + "".join(f'{sep}"{p}"' for p in new_parts[1:])

    text = CHAIN_RE.sub(chain_sub, text)
    text = PATH_RE.sub(path_sub, text)
    return text, count


def rewrite_paths(mapping: dict[str, str], extra_files: list[str]) -> None:
    files = [f for f in git_files() if f.endswith(REFERENCE_GLOBS) and not f.startswith("external/")]
    files = [f for f in files if f != "tools/refactor/relayout.py" and f != "CHANGELOG.md"]
    total = 0
    touched = 0
    for f in [os.path.join(ROOT, p) for p in files] + extra_files:
        try:
            with open(f, encoding="utf-8", newline="") as fh:
                text = fh.read()
        except (OSError, UnicodeDecodeError):
            continue
        new_text, n = rewrite_references(text, mapping)
        if n:
            with open(f, "w", encoding="utf-8", newline="") as fh:
                fh.write(new_text)
            total += n
            touched += 1
    print(f"rewrote {total} path references in {touched} files")


def main() -> None:
    mode = sys.argv[1] if len(sys.argv) > 1 else "plan"
    if mode == "paths":
        mapping = json.load(open(os.path.join(ROOT, "build", "relayout_mapping.json"), encoding="utf-8"))
        rewrite_paths(mapping, sys.argv[2:])
        return
    mapping = build_mapping()
    if mode == "plan":
        tree = defaultdict(list)
        for old, new in mapping.items():
            tree[os.path.dirname(new)].append(os.path.basename(new))
        for d in sorted(tree):
            print(f"{d}/ ({len(tree[d])})")
        print(f"total moved: {len(mapping)}")
    elif mode == "list":
        for old, new in sorted(mapping.items()):
            print(f"{old} -> {new}")
    elif mode == "includes":
        rewrites, problems = plan_include_rewrites(mapping)
        print("\n".join(problems))
        print(f"files with include rewrites: {len(rewrites)}; spellings: {sum(len(v) for v in rewrites.values())}")
    elif mode == "apply":
        apply(mapping)
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
