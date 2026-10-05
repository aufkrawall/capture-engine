# Repo Map (code map)

Last cross-checked: 2026-10-02 (subsystem relayout, `tools/refactor/relayout.py`)

How to find code:
- Every product module is `<module>/<subsystem>/`. Basenames are unique per module, so
  `git ls-files '*/<name>'` or a basename grep always finds a unit; tests use
  `ce::test_source::FindSource(module, name)` for the same reason.
- Includes are repo-root-relative (`#include "hook/overlay/custom_overlay.h"`); same-directory
  includes stay bare. Every compile gets `-I<repo>`; there are no per-directory include paths.
- A file named `<stem>.cpp` with `<stem>_internal.h` and `<stem>_*.cpp` siblings is one logical unit
  split by size; source-policy tests read it whole (`tests/source_fragment_reader.h`).
- Topic pages (via `index.md`) explain behavior; this page only says where things are.

## Binaries

| Binary | Sources |
| --- | --- |
| `capture_hook_{x64,x86}.dll` (injected into games) | `hook/**` except `vulkan_layer/`, `shaders/` (`build_common.hook_dll_sources`) |
| `VK_LAYER_CE_overlay[_x86].dll`, `VK_LAYER_CE_gate[_x86].dll` | `hook/vulkan_layer/` + a named hook-core subset (`build_vulkan_layer.py`) |
| `captureengine.exe` (roles: controller, inject child, media process, logger, sensor bridge, installer setup) | `captureengine/**` + `common/**` |
| `mediaengine.dll` (encode, mux, audio) | `mediaengine/**` |
| `captureengine_elevation_service.exe` | `elevationservice/` |
| setup / uninstaller | `installer/` (`tools/installer_payload.py` packs the payload) |
| `unit_tests.exe` | `tests/` + `common/` + `mediaengine/` + the hook core (`build_common.HOOK_TEST_LINKED_DIRS/SOURCES`) |
| graphics test apps | `testapp/` |

## hook/ - the injected runtime

| Dir | Owns |
| --- | --- |
| `runtime/` | DllMain and bootstrap (`main_*`), hook thread, host lifecycle, LoadLibrary/CreateProcess redirect, third-party overlay detection, fatal/external dumps, UE5 CVar redirection (`main_ue5*`), `HookLog` (`hook_common`), IPC client, freeze watchdog, `GraphicsHook` interface |
| `hooking/` | inline/IAT/vtable hook primitives, grouped thread-quiesced patch transactions (`hook_patch_transaction`, `inline_hook_entry_patch`), module pinning, export resolution |
| `wrappers/` | COM wrappers (DXGI factory/adapter/output/device/swapchain, D3D9/10/11 device + context), D3DKMT hook, Vulkan dispatch, Vulkan-on-DXGI FIFO present |
| `present/` | central DXGI Present routing (`dxgi_shared*`: hooks, present/present1, routing, Steam, resize), swapchain lifetime/flags/resize references, present heartbeat / stage cost / re-entry guard, interposer (Smooth Motion) tracking, Vulkan WSI tables |
| `d3d12/` | DX12 hook: ProcessFrame session (`dx12_hook_process*`; `stage1..5` = the former `Phase1..5`, still named so in older wiki text), PostSL render (`dx12_hook_postsl*`), ExecuteCommandLists (`_ecl*`), FFX overlay routes (`_ffx*`, `dx12_ffx_suspend_overlay`), Streamline FG transitions, queue adoption, swapchain create/tracking, overlay glue, `dx12_overlay_policy/` |
| `d3d11/`, `d3d9/`, `d3d8/`, `opengl/` | per-API hook: present, capture, overlay, sampler overrides |
| `ddraw/` | DirectDraw (all surface generations) + D3D6/7 devices, CPU composite, D3D7 sidecar, texture-stage shadow |
| `streamline/` | Streamline 1.x/2.x hooks, SL1->SL2 generation bridge (`streamline_bridge*`), DLSS-G options/OFF churn, PCL markers, OTA preferences |
| `ffx/` | FidelityFX API hooks (FSR FG): create/configure observation, context adoption, cached-pointer router |
| `ngx/` | NVNGX/DLSS hooks, DRS overrides (`dlss_fg_*`), OTA refusal, DLSS indicator, RTX Remix, RR hand-off |
| `fg/` | FG detection and the FG session state machine (`fg_session_state*`) |
| `overlay/` | custom overlay renderers per backend (`custom_overlay_*`), overlay adapter, layout and graph policy, font, `overlay_compat` (third-party module identity), shader bytecode |
| `pacing/` | FPS limiter (`fps_limiter*`, `fps_limiter_detail/`), Reflex limiter, pacing trace/health |
| `metrics/` | system and GPU metrics, PC latency (`system_latency_*`), perf logger, benchmark, hook CPU cost |
| `overrides/` | sampler/LOD/mip overrides, vsync and graphics-config resolution, UE5 CVar policy headers |
| `sharpen/` | CAS/RCAS sharpen for D3D11/D3D12 + shared GPU-timeline policy |
| `capture/` | inject capture (`shared_capture*`), screenshot hook/worker |
| `vulkan_layer/` | the Vulkan layer DLL and its negotiation gate (`layer_gate`, `layer_participation`) |

## captureengine/ - the host process

| Dir | Owns |
| --- | --- |
| `app/` | entry and role dispatch (`main_entry`), controller loop, recording control, Vulkan residency, tray, hotkeys, status overlay sync, host metrics, mediaengine loader |
| `injection/` | process discovery (WMI events + `process_start_poll` fallback), injection, OTA watchdog, inject child (`inject_main`), config publication and per-target prewarm |
| `media/` | media process (`media_main*`: session, loop, WGC target selection, emit/encode/health), WGC capture (`wgc_capture*`), DXGI duplication, screenshots, process-loopback worker host |
| `display_timing/` | ETW screen-change timing service (NVIDIA scheduled flips, Intel/AMD correlation, refresh bounds, health) |
| `sensors/` | sensor plugin and broker, LibreHardwareMonitor bridge (CLR host), PawnIO |
| `elevation/` | elevation service client/setup/runtime/removal, startup preferences and autostart |
| `diagnostics/` | external dump helper (+ WoW64 32-bit stacks), logger service |
| `pseudo_overlay/` | GDI pseudo overlay (desktop/status display) |
| (root) | `.rc`, manifest, icons, `resource.h`, `config.ini.template` |

## common/ - shared by every binary

| Dir | Owns |
| --- | --- |
| `config/` | config model (`application_profile.h` owns profile selectors/routing/matching) and INI loader (`config_load_*` per section, `config_ini_reader`, text encoding), live-stream and face-camera config |
| `ipc/` | shared-memory ABI (`shared_defs.h` + `shared_defs_detail/`, ABI version in `abi_constants_and_config.h`), private IPC channels (`process_ipc*`), elevation protocol, display-timing and A/V-sync channels |
| `capture/` | capture pipeline policy (`capture_policy/`, CFR grid, frame queue/timing, inject frame source/lease, retarget, reserved output, screen-grab privacy) |
| `crash/` | crash handler, dump writer, first-chance records, symbol store, WER adoption |
| `logging/` | `Log*` service logging, `log_meter.h` (cadence gates), log privacy |
| `overlay/` | inject-overlay and pseudo-overlay policies, recording indicator, hotkey matcher |
| `graphics/` | mip/sharpen policy, Vulkan layer registration and target list |
| `platform/` | paths, process identity, background window heartbeat (`window_heartbeat`), restricted child process, secure DLL loading, threading/RAII/ring-buffer primitives, build identity, byte-pattern scanner (hook-only) |
| `setup/` | startup and installer-setup argument policy |
| (root) | `build_version.h` (generated, untracked) |

## mediaengine/ - encode and mux (DLL)

| Dir | Owns |
| --- | --- |
| `engine/` | the `MediaEngine` class (`mediaengine.h` API; init, config, transport, timeline, recording start/stop, frame, audio thread/loop/pull units) |
| `audio/` | endpoint and app/process-loopback capture, encoder, resampler, ring buffer, latency probe, recovery/fault accounting, `audio_sync/` |
| `video/` | `video_encoder*` pipeline (options, configure, conversion, encode, write, finalize, streaming), face camera, cursor, metadata |
| `mux/` | Matroska timing, mux queue pressure, mux invariants |

## Build and tooling

`build.py` is a facade that execs the ordered units in `tools/build/` into one namespace:

| Unit | Role |
| --- | --- |
| `build_privacy.py` | profile-path redaction, `-ffile-prefix-map`, binary scrub, path-component scan |
| `build_common.py` | constants, compile flags (`make_cpp_cflags`), **source-tree layout helpers** (`module_sources`, `hook_dll_sources`, `hook_test_linked_sources`, `common_sources`, `find_module_source`), linker helpers |
| `build_bootstrap.py` | verification context, MSYS2/Python-tool bootstrap, download+verify, locking |
| `build_io.py` | copy/remove, `run_command`, build identity (`common/build_version.h`), failed-resume manifest |
| `build_fg_sdk.py` | FidelityFX/Streamline SDK headers and runtime DLLs |
| `build_linux_msys2.py` | Linux-host MSYS2 packages |
| `build_ffmpeg.py`, `build_toolchain.py` | FFmpeg build, MSYS2 env, job counts |
| `build_compile_db.py` | `compile_commands.json`, unit-test compile/link (`compile_tests`) |
| `build_tests.py` | unit tests, Python self-tests, integration tests, clang-tidy scope |
| `build_preflight.py` | verify preflight (file-size baseline, compile-db snapshot), format |
| `build_testapps.py`, `build_vulkan_layer.py`, `build_elevation_service.py`, `build_installer.py` | per-target builds |
| `build_project.py` | `compile_project`: common, hook DLL, mediaengine, captureengine |
| `build_project_finalize.py` | FG SDK, test apps, Vulkan layer, licenses, PE hardening, packaging |
| `build_corresponding_source.py`, `build_packaging.py` | LGPL corresponding source, 7z/installer staging, fuzz build |
| `build_cli.py` | CLI dispatch and top-level orchestration |

Other tooling:
- `tools/config/` - clang-format, clang-tidy, clangd, flake8, pyright configuration.
- FFmpeg dependency closure: `tools/ffmpeg_dependencies.{json,py}`, `dependency_build_policy.py`,
  `dependency_pgp.py` + `pgp-keys/`, `source_download.py`, `rehearse_dependency_closure.py`.
- `tools/refactor/` - one-shot refactoring scripts: `relayout.py` (this layout; mapping in
  `build/relayout_mapping.json` after a run), `preprocess_fingerprint.py` (proves a pure move left
  every TU's preprocessed output byte-identical), `syntax_check.py` (seconds-fast `-fsyntax-only` of changed
  product TUs), `remove_unused.py` (deletes compiler-proven dead statics from a clang_tidy.log), older
  splitter/de-inline scripts.
- `tools/analysis/`, `tools/tracing/` - capture A/V analysis, DX12/GPU tracing.
- Baselines: `tools/clang_tidy_baseline.json`, `tools/file_size_baseline.json`,
  `tools/source_line_length_baseline.json`.

Output paths: `installed/captureengine/` (runtime + `logs/<session>/`), `installed/testapp/`,
`build/packages/` (installer), `build/verification/` (gate summaries), `compile_commands.json` (repo root).

## High-risk areas (read the topic page first)

| Area | Where | Topic page |
| --- | --- | --- |
| Shared-memory ABI | `common/ipc/shared_defs*` | `process-ipc.md` |
| Injection and inject child | `captureengine/injection/`, `hook/runtime/main_host_lifecycle.cpp` | `dx12-injection-bootstrap.md`, `process-ipc.md` |
| Present routing | `hook/present/dxgi_shared*` | `overlay-rendering.md` |
| DX12 overlay + FG transitions | `hook/d3d12/`, `hook/d3d12/dx12_overlay_policy/`, `hook/streamline/streamline_runtime_policy.h` | `frame-generation/` |
| Hook patching | `hook/hooking/` (`inline_hook_entry_patch.cpp` writes live code) | `known-debt.md` |
| Third-party overlay identity | `hook/overlay/overlay_compat*`, `hook/runtime/main_overlay_detect.cpp` | `dx12-overlay-third-party-coexistence.md` |
| DLSS driver settings | `hook/ngx/ngx_drs_override*` | `frame-generation/dlss-driver-settings.md` |
| Vulkan layer | `hook/vulkan_layer/` (`layer_gate`, `layer_participation`, `layer_ipc`, `layer_sharpen*`) | `vulkan-*.md`, `post-processing-sharpen.md` |
| Display timing | `captureengine/display_timing/` | `display-change-timing.md` |
| CFR/audio pipeline | `mediaengine/engine/`, `mediaengine/audio/`, `common/capture/capture_policy/` | `cfr-capture-sync.md`, `multi-audio-capture.md` |
| Sensors | `captureengine/sensors/` | `overlay-rendering.md` |
| Elevation, installer | `elevationservice/`, `captureengine/elevation/`, `installer/` | `elevation-and-startup.md`, `installer.md` |

## Practical notes

- DX12 overlay, injection and FG behavior spans `captureengine/`, `hook/` and `tests/`; do not
  reason about one in isolation.
- `build.py` compiles test sources on non-test builds too, so `compile_commands.json` stays useful.
- `tests/unit_tests.exe` runs from the repo root (runtime DLLs are copied beside it).
- New hook units in `fg/ metrics/ overlay/ overrides/ pacing/ present/ sharpen/` link into the unit
  tests automatically; elsewhere add them to `HOOK_TEST_LINKED_SOURCES` if a test needs them.
- Python unit families (`build_*.py`, `<tool>_*.py`) are linted through their facades.
