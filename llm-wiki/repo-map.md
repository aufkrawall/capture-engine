# Repo Map (code map)

Last cross-checked: 2026-08-06

Primary sources:
- top-level repo layout (verified against the working tree)
- `build.py` and `tools/build/*.py`
- `common/shared_defs.h`, `common/config.h`, `common/process_ipc.h`

## How to navigate this map

AGENTS.md points agents at `llm-wiki/index.md` for routing; this page is the
concrete file-level code map. After the semantic-unit conversion (2026-08-06)
every first-party C++ and Python file is a semantic unit <= 800 lines, no `.inl`
fragments remain, and `tools/file_size_baseline.json` / `tools/clang_tidy_baseline.json`
are empty. When a topic page cites an old monolithic file (e.g. `dx12_hook.cpp`,
`mediaengine.cpp`), the logic now lives in the per-area units listed below; line
anchors that predate the split are approximate.

## Core Tree

- `build.py`
  - Canonical build, lint, format, and test entry point; facade that execs the
    ordered semantic units in `tools/build/` into one module namespace. Contains
    `main()`, the CLI, and the gate logic (incremental / clean / verify / resume /
    no-build / lint / fuzz).
- `tools/build/`
  - Semantic build units executed by the `build.py` facade (order matters):
    | Unit | Role |
    | --- | --- |
    | `build_common.py` | shared constants, compile-commands path, sha256, PDB path, linker helpers |
    | `build_bootstrap.py` | verification context, MSYS2/Python-tool bootstrap, download+verify, file locking |
    | `build_io.py` | safe copy/remove, `run_command`/`run_logged_subprocess`, process helpers |
    | `build_fg_sdk.py` | FidelityFX/Streamline SDK headers + runtime DLL preparation |
    | `build_linux_msys2.py` | Linux-host MSYS2 package resolution/download |
    | `build_ffmpeg.py` | `FFmpegBuilder` + bundled runtime licenses |
    | `build_toolchain.py` | `compile_custom_ffmpeg`, MSYS2 env (`get_env`/`get_env_x86`), parallel job counts |
    | `build_compile_db.py` | `compile_commands.json`, clangd enrichment, atomic JSON, clang resource dir |
    | `build_tests.py` | unit tests, Python tool self-tests, integration tests, clang-tidy scope paths |
    | `build_preflight.py` | verify preflight (file-size baseline, compile-db snapshot), format |
    | `build_testapps.py` | `compile_testapps`, `TestAppCommand`, `add_task`/`make_cmd` task registry |
    | `build_vulkan_layer.py` | `compile_vulkan_layer` (x64/x86) |
    | `build_project.py` | `compile_project`: phases 1-5 (common, hook DLL, mediaengine, captureengine) |
    | `build_project_finalize.py` | `_finalize_project_build`: phases 6-8 (FG SDK, test apps, vulkan layer), licenses, PE hardening, packaging |
    | `build_packaging.py` | `captureengine.7z` / `testapps.7z` staging + validation |
    | `build_cli.py` | `main()` CLI dispatch and top-level orchestration |
- `tools/config/`
  - Committed tool configuration (clang-format, clang-tidy, clangd, editorconfig,
    flake8, pyright), consumed via explicit paths by the build and lint drivers.
- `common/`
  - Shared IPC, config, logging, ABI structs, and RAII helpers.
  - `shared_defs.h` - shared-memory ABI (current version `38`).
  - `config.h/.cpp` + `config_load*.cpp` (`config_load_core/audio/overlay/misc/whitelist.cpp`) -
    config model, loader, and themed section loaders (`ConfigReader`).
  - `process_ipc.h/.cpp` + `process_ipc_client.cpp` - private IPC channels.
- `captureengine/`
  - Host/controller logic: `main_controller.cpp`, `main_recording.cpp`, `main_vulkan.cpp`,
    `main_entry.cpp`, `main_internal.h`.
  - Injection: `injection.cpp`, `injection_manager.cpp`, `injection_inject.cpp`,
    `injection_security.cpp`, `inject_main.cpp`.
  - Recording/media orchestration: `media_main_encoder_0*.cpp` (session, loop start,
    WGC target, select, startup, emit, encode, health),
    `media_main_start*.cpp` (MediaProcessSession: Run/Init entry, loop, WGC target
    selection, shutdown; MediaProcessMain is a thin entry that runs the session),
    `media_main_internal.h`, `wgc_capture*.cpp` (impl, pool, gpu_timing, format units).
- `hook/`
  - `main.cpp` + `main_*.cpp` (dllmain, injection, install, loadlibrary, hookthread,
    redirect, ue5, overlay_detect, fatal hooks/dumps, external_dump) + `main_internal.h`.
  - `apis/` - per-API hook sets, de-inlined into semantic units:
    - DX12: `dx12_hook_main*.cpp` (present/FG orchestration), `dx12_hook_overlay*.cpp`,
      `dx12_hook_ffx*.cpp`, `dx12_hook_ecl*.cpp`, `dx12_hook_process_session*.cpp`
      (ProcessFrame), `dx12_hook_postsl_render*.cpp`, `dx12_hook_0_internal_helpers*.cpp`
      (1-11: state, exports, Steam/VA-space/focus helpers in helpers10), `dx12_hook.cpp`
      (1-line facade include), `dx12_hook_internal.h`.
    - DX11: `dx11_hook_present.cpp`, `dx11_hook_prerender.cpp`, `dx11_hook_internal.h`.
    - DX9: `dx9_hook_internal.h` + semantic units: `dx9_hook.cpp` (module
      lifecycle + inline IAT hooks), `dx9_hook_capture_{frame,direct_ring,init,
      gdi,ring,lifecycle}.cpp`, `dx9_hook_present.cpp` (present begin/end
      stages), `dx9_hook_present_detours.cpp`, `dx9_hook_state_detours.cpp`,
      `dx9_hook_device.cpp` (creation + hook install), `dx9_hook_pacing.cpp`,
      `dx9_hook_overlay.cpp`, `dx9_hook_helpers.cpp`, `dx9_hook_sampler_state.cpp`.
    - DX8/DDraw/OpenGL/Streamline/FFX/Layer: de-inlined internal headers
      (`dx8_hook_internal.h` etc.) + per-area `.cpp` units.
  - `common/` - overlay policy (`dx12_overlay_policy.h`, `streamline_runtime_policy.h`,
    `overlay_compat.h`), `dxgi_shared*.cpp` (central Present routing: hooks, present,
    present1, routing, steam, resize, original), `fg_session_state*.cpp`
    (state core + names + log units), `custom_overlay_*.cpp` (per-backend + internal
    headers + render units), `overlay_adapter*.cpp` (adapter + render + render_frame),
    `system_metrics*.cpp` (metrics + gpu unit), `reflex_limiter.h`, `fps_limiter.h`.
  - `wrappers/` - `dxgi_swapchain_wrap*.cpp` (wrap, present, lifetime, frame_latency),
    `hook_system.cpp`, `iat_hook.h`.
- `mediaengine/`
  - De-inlined `MediaEngine` class: `mediaengine_impl*.cpp` units (init/recording/
    frame pipeline/audio), `mediaengine_internal.h` (declarations only), `mediaengine.cpp` (facade).
  - `audio_*.cpp` - capture, loop, encoder (encode/flush), resampler, ring buffer,
    app-audio (activation/loop/monitor/queue units), process-loopback capture.
  - `video_encoder*.cpp` - encoder pipeline units (options, backend_options,
    configure, conversion, convert_bgra/shaders, encode, encode_input, finalize,
    format, framegrab, lifecycle, start, textures, write, codec, options_bitrate).
- `tests/`
  - Google Test sources (`test_*.cpp`, including source-policy tests that read the
    logical unit: stem + `<stem>_internal.h` + sorted `<stem>_*.cpp` siblings) and
    the built `unit_tests.exe` plus copied runtime DLLs.
- `testapp/`
  - Graphics test apps: `dx12_fg_switch_*.cpp`, `vulkan_fg_switch_*.cpp` (+
    `vulkan_fg_switch_test_internal.h`), `dx12_av_sync_test*.cpp` (util/manifest
    units), `dx12_fsr_fg_test*.cpp` (fsr unit), `dx11_test.cpp`, `vulkan_*.cpp`,
    `run_tests.py` (integration runner, facade over `run_tests_part_*.py`).
- `tools/refactor/`
  - `source_splitter.py` (facade over `source_splitter_common/lexer/scanner/split.py`),
    `gen_deinline.py`, `reapply.py`.

## Important Support and Output Paths

- `external/ffmpeg/` - local FFmpeg install root used by the custom Windows build flow.
- `tools/licenses/` - bundled third-party license texts and the FFmpeg vendor runtime notice.
- `tools/patches/ffmpeg/` - patch set applied during the custom FFmpeg build.
- `installed/captureengine/` - main runtime output directory for CaptureEngine
  binaries, logs, and `ffmpeg/` runtime DLLs.
- `installed/testapp/` - built graphics test applications plus local-only vendor runtime DLLs.
- `build/packages/` - automatically replaced `captureengine.7z` and `testapps.7z`
  artifacts. The latter contains no vendor DLLs.
- `compile_commands.json` - generated at the repo root by `build.py` for clangd/LSP
  (528 translation units after the semantic-unit split).
- `build/verification/` - per-run gate summaries (`latest_summary.txt`), manifests,
  and stage artifacts (`clang_tidy.log`, `flake8.log`, `pyright.log`, sanitizer logs).

## High-Risk / High-Value Files

- `common/shared_defs.h` - shared-memory ABI (version `38`, source-verified).
- `captureengine/injection.cpp` + `injection_manager.cpp` - host-side injection
  timing and delayed-injection logic.
- `captureengine/inject_main.cpp` - shared-memory setup, config reloads, and
  inject-overlay runtime handoff flags.
- `hook/main.cpp` + `main_injection.cpp` - hook bootstrap and wrapper-init decisions.
- `hook/common/dxgi_shared.cpp` (+ `dxgi_shared_present*.cpp`, `dxgi_shared_steam*.cpp`)
  - central Present routing and startup bypass behavior.
- `hook/apis/dx12_hook_main*.cpp` + `dx12_hook_0_internal_helpers*.cpp` - DX12
  present/FG/overlay policy, exports, Steam/VA-space helpers.
- `hook/common/dx12_overlay_policy.h` - dense policy helpers for DX12 overlay
  routing, startup coexistence, and FG transitions.
- `hook/common/streamline_runtime_policy.h` - Streamline PostSL startup/suspend
  policy helpers.
- `hook/common/overlay_compat.h` - third-party overlay and FFX module detection helpers.
- `mediaengine/mediaengine_impl*.cpp` + `mediaengine_internal.h` - capture/CFR/audio
  pipeline; the audio loop and pull phases live in `mediaengine_impl_7*`/`9*` units.
- `mediaengine/video_encoder*.cpp` - encoding pipeline (CFR, hardware encoders,
  HDR, frame-repeat) and `mediaengine/video_encoder_internal.h`.

## Practical Notes

- `build.py` compiles test sources even on non-test builds so `compile_commands.json`
  remains useful.
- `tests/unit_tests.exe` is intended to run directly after a successful build
  because required runtime DLLs are copied beside it.
- Source-policy tests (`tests/test_*.cpp`) read logical units (header + stem +
  sorted siblings); when splitting a unit, re-check anchors and cross-unit ordering.
- DX12 overlay, injection, and FG behavior spans `captureengine/`, `hook/`, and
  `tests/`; do not reason about one of those areas in isolation.
- Python facade fragments are excluded from standalone flake8/pyright analysis
  (`*_part_*.py`, `build_*.py`, `source_splitter_*.py`); the facades themselves
  (`build.py`, `source_splitter.py`) stay linted.
