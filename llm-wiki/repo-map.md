# Repo Map (code map)

Last cross-checked: 2026-09-23

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
    | `build_privacy.py` | privacy helpers: profile spellings, `-ffile-prefix-map` flags, manifest/summary redaction, length-preserving binary scrub, path-component scan |
    | `build_common.py` | shared constants, compile-commands path, sha256, PDB path, linker helpers |
    | `build_bootstrap.py` | verification context, MSYS2/Python-tool bootstrap, download+verify, file locking |
    | `build_io.py` | safe copy/remove, `run_command`/`run_logged_subprocess`, process helpers, build-identity and failed-resume manifest validation |
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
    | `build_corresponding_source.py` | exact pinned/patched FFmpeg + LGPL libiconv corresponding-source staging |
    | `build_packaging.py` | `captureengine.7z` / `testapps.7z` staging + validation |
    | `build_cli.py` | `main()` CLI dispatch, failed-run verification-mode restoration, and top-level orchestration |
- `tools/config/`
  - Committed tool configuration (clang-format, clang-tidy, clangd, editorconfig,
    flake8, pyright), consumed via explicit paths by the build and lint drivers.
- FFmpeg dependency closure (standalone `tools/` units, driven by `build_toolchain.py`):
  | Unit | Role |
  | --- | --- |
  | `ffmpeg_dependencies.json` | the pinned manifest: versions, source URLs, SHA-256, PGP fingerprints, `package_outputs`, runtime DLLs, licenses, build order |
  | `ffmpeg_dependencies.py` | `SourceDependencyBuilder`: manifest validation/fingerprint, PGP + SHA-256 verification, recipe extraction, `makepkg-mingw`, CFG verification, PE import closure |
  | `dependency_build_policy.py` | the shell policy appended to each PKGBUILD: hardening/prefix flags, `pkgname` reduced to `package_outputs` (fail-closed), path-independent documentation output |
  | `dependency_pgp.py` | PGP trust: vendored-key import (hard failure, no keyserver fallback), keyring reset so rebuilds re-import, detached-signature verification against pinned fingerprints |
  | `source_download.py` | TLS trust from the toolchain CA bundle and bounded retry of transient download faults |
  | `rehearse_dependency_closure.py` | builds the closure with empty downloads/keyring at a runner-depth root, to catch release-only faults locally |
  | `pgp-keys/<FINGERPRINT>.asc` | vendored armored signing keys, so a build needs no keyserver (and no `dirmngr`). Fetch only from a keyserver that keeps user IDs — gpg refuses a UID-less key |
- `common/`
  - Shared IPC, config, logging, ABI structs, and RAII helpers.
  - `shared_defs.h` - shared-memory facade; `shared_defs_detail/abi_constants_and_config.h`
    owns the current ABI version (`57`).
  - `config.h/.cpp` + `config_load*.cpp` (`config_load_core/audio/overlay/misc/whitelist/ue5/streaming/face_camera.cpp`) -
    config model, loader, and themed section loaders (`ConfigReader`); `config_load_ue5.cpp` owns the
    whole `[UE5]` vocabulary, while `live_stream_config.*` and `config_load_streaming.cpp` own the
    fail-closed RTMP/RTMPS profile and `face_camera_config.h` owns face-camera parsing/layout policy.
  - `process_ipc.h/.cpp` + `process_ipc_client.cpp` - private IPC channels.
  - `crash_handler.cpp` + `crash_dump_writer.cpp` (vectored/unhandled filters, dump worker) +
    `crash_first_chance.{h,cpp}` (first-chance fault records, dispatch detection, continue handler) +
    `crash_symbol_store.{h,cpp}` (hard-linked per-session symbol archive) + `crash_dump_policy.h`.
  - `vulkan_layer_target_list.h` - which processes the implicit Vulkan layer may enter; the inject
    child persists the list to `HKCU\Software\CaptureEngine\VulkanLayerTargets` (REG_MULTI_SZ),
    the layer decides at negotiation.
- `captureengine/`
  - Host/controller logic: `main_controller.cpp`, `main_recording.cpp`, `main_vulkan.cpp`,
    `main_entry.cpp`, `main_internal.h`.
  - External crash-dump helper: `dump_helper.{h,cpp}` plus
    `dump_helper_wow64_stacks.{h,cpp}`, which supplies a WoW64 target's 32-bit thread stacks
    through dbghelp's memory callback (range arithmetic and caps in
    `common/wow64_stack_range_policy.h`). Without it an x64 helper records only the syscall
    thunk and no 32-bit caller can be recovered.
  - Injection: `injection.cpp`, `injection_manager.cpp`, `injection_wmi_events.cpp`,
    `injection_inject.cpp`, `injection_ota_watchdog.cpp`,
    `injection_security.cpp`, `inject_main.cpp`, `inject_config.cpp`,
    `inject_config_publication.cpp`, `inject_lifecycle.cpp`, `process_start_poll.{h,cpp}`; the WMI
    unit owns the event-driven ProcessStartTrace subscription and callback lifetime, while
    `process_start_poll` is the unelevated fallback (one `NtQuerySystemInformation` sweep per
    50 ms, feeding the same whitelist/delayed-injection path). There is no WMI polling query.
  - Recording/media orchestration: `media_main_encoder_0*.cpp` (session, loop start,
    WGC target, select, startup, emit, encode, health),
    `media_main_start*.cpp` (MediaProcessSession: Run/Init entry, loop, WGC target
    selection, shutdown; MediaProcessMain is a thin entry that runs the session),
    `media_main_internal.h`, `wgc_capture*.cpp` (pool, pool_budget, frame,
    session, state, queue, init, frame_pump units + `wgc_capture.cpp` facade).
- `hook/`
  - `main.cpp` + `main_*.cpp` (dllmain, injection, install, loadlibrary, hookthread,
    host_lifecycle, redirect, overlay_detect, fatal hooks/dumps, external_dump) + `main_internal.h`;
    validated typed persistent UE CVar shadow redirection is five units behind `main_ue5_internal.h`:
    `main_ue5.cpp` (policy/lifecycle/service pass), `main_ue5_scan.cpp` (literal + candidate discovery),
    `main_ue5_install.cpp` (install, refresh, read-back verification, restore), `main_ue5_memory.cpp`
    (process-memory/PE primitives), `main_ue5_registry.cpp` (resolution through UE's console-object map).
    `hook/common/ue5_cvar_override_policy.h` defines the cost-ranked graduated RR-quality ladder
    (`off/light/medium/high/full`), typed custom-CVar precedence, sharpen precedence, and the
    depth-of-field / DLSS Super Resolution / HDR specs, `ue5_redirect_plan.h` the redirect undo contract,
    `ue5_console_registry.h` the registry decoders. Topic page: `llm-wiki/ue5-cvar-overrides.md`.
  - `apis/` - per-API hook sets, de-inlined into semantic units:
    - DX12: `dx12_hook.cpp` (facade) + `dx12_hook_internal.h` + semantic units:
      `dx12_hook_main.cpp` (module lifecycle), `dx12_hook_fg_state.cpp` /
      `dx12_hook_fg_startup.cpp` / `dx12_hook_streamline_fg_transition.cpp` /
      `dx12_hook_focus_loss.cpp`, `dx12_hook_overlay*.cpp` (overlay, present,
      d3d11on12), `dx12_hook_ffx*.cpp` (FFX, UI composite/state, owner queue,
      proxy Present, final-batch topmost routing, callback-adapter prewarm, callback
      frame-metric ownership, warm route-edge retention; `dx12_hook_ffx_callback_bridge.cpp` owns
      generation-cached callback registry access), `dx12_ffx_suspend_overlay.cpp` (owner-queue
      renderer/state and exact-proxy lifetime),
      `dx12_hook_queue_adoption.cpp` (stable same-device discovery versus explicit queue binding,
      device/queue reference publication),
      `../common/pacing_trace.{h,cpp}` (bounded FSR event history and background suspect/manual saves),
      `../common/pacing_trace_boundary.h` (paired proxy/DXGI Present spans),
      `../common/pacing_trace_analysis.h` (background save summaries and coverage validation),
      `../common/present_heartbeat.h` (race-free monotonic Present-gap diagnostics),
      `../common/swapchain_create_recovery.h` (per-thread/HWND ownership across nested create hooks),
      `dx12_hook_ecl*.cpp` (ECL install/observation, including the callback-owned native-FSR
      transparent fast-forward and foreign-hook recursion break),
      `dx12_hook_process*.cpp` (process dispatch + session driver/phase1..phase5/
      draw_transition/draw_main/draw_submit/draw_tail), `dx12_hook_postsl_render*.cpp`
      (render driver, entry, gate, route, submit), `dx12_hook_helpers.cpp`,
      `dx12_hook_queue_method_resolution.cpp` +
      themed helper units (overlay_coverage, overlay_breadcrumbs, ffx_startup,
      fg_heuristics, postsl_queue, swapchain_create, swapchain_wrap_policy, observer, overlay_render,
      prerender, hook_install, screenshot, overlay_dedicated_queue,
      overlay_startup_compat, postsl_route, swapchain_tracking, swapchain_detours),
      `dx12_hook_types*.h/cpp`, `dx12_hook_internal_globals.cpp`, `dx12_hook_swapchain.cpp`,
      `dx12_hook_sharpen.cpp`.
    - DX11: `dx11_hook_present.cpp`, `dx11_hook_prerender.cpp`, `dx11_hook_device.cpp`,
      `dx11_hook_detours.cpp`, `dx11_hook_install.cpp`, `dx11_hook_screenshot.cpp`,
      `dx11_hook_overlay.cpp`, `dx11_hook_sharpen.cpp`, `dx11_hook_sampler_state.cpp`,
      `dx11_hook_sampler_override.cpp`, `dx11_hook_capture_{lifecycle,init,frame}.cpp`,
      `dx11_hook_helpers.cpp`, `dx11_hook_internal.h`.
    - DX9: `dx9_hook_internal.h` + semantic units: `dx9_hook.cpp` (module
      lifecycle + inline IAT hooks), `dx9_hook_capture_{frame,direct_ring,init,
      gdi,ring,lifecycle}.cpp`, `dx9_hook_present.cpp` (present begin/end
      stages), `dx9_hook_present_detours.cpp`, `dx9_hook_state_detours.cpp`,
      `dx9_hook_device.cpp` (creation + hook install), `dx9_hook_pacing.cpp`,
      `dx9_hook_overlay.cpp`, `dx9_hook_helpers.cpp`, `dx9_hook_sampler_state.cpp`.
    - DX8: `dx8_hook_capture_{lifecycle,init,frame,copy}.cpp`, `dx8_hook_detours.cpp`,
      `dx8_hook_helpers.cpp`, `dx8_hook_internal.h`.
    - DDraw: `ddraw_hook_capture_{lifecycle,init,frame}.cpp`, `ddraw_hook_capture.cpp` (presentation
      and capture ordering), `ddraw_hook_overlay_composite.cpp` + `ddraw_hook_composite_state.h`
      (per-surface CPU backdrop/composite state), `ddraw_hook_overlay_route.cpp` (persistent CPU
      adapter plus auxiliary native D3D7 sidecar and damage state), `ddraw_hook_write_tracking.{h,cpp}`
      (exact application-write history), `ddraw_hook_detours.cpp` (legacy and Surface7 detours),
      `ddraw_hook_detours_surface4.cpp` (the DirectDraw 4 generation's detours),
      `ddraw_hook_blit_classification.h` (the presentation classification, geometry readers and
      nested-presentation refusal all three generations share),
      `ddraw_hook_present_reentry.cpp` (recorded presentation entry points and the cycle report),
      `ddraw_hook_present_overrides.{h,cpp}` (typed Flip/Blt scheduling, vertical-blank pacing,
      and CPU presentation-depth ownership), `ddraw_hook_runtime_state.h` (narrow shared diagnostics/bootstrap
      state), `ddraw_hook_detours_surface_access.cpp` (Lock/DC access and
      direct-scanout presentation),
      `ddraw_hook_detours_legacy_d3d.cpp` (DX6/DX7 device interception),
      `ddraw_hook_texture_bindings.cpp` (CE-owned references to the application's D3D7 texture
      bindings, which is what makes the sidecar's state-block restore legal), `ddraw_hook_install.cpp`,
      `ddraw_hook_helpers.cpp`, `ddraw_hook_internal.h`; shared implementation/policy lives in
      `hook/common/{ddraw_present_policy,ddraw_native_overlay_damage,overlay_cpu_raster,
      legacy_d3d_texture_bindings}.h` plus
      `hook/common/overlay_cpu_raster.cpp`, while `hook/common/custom_overlay_d3d7.{h,cpp}` draws
      immediately before the application's real D3D7 `EndScene`.
    - OpenGL: `opengl_hook_capture.cpp` (detours, swap begin/end, overlay draw),
      `opengl_hook_install.cpp` (inline + IAT hook installation, `OpenGLHook::Init/Shutdown`),
      `opengl_hook_capture_{lifecycle,init,frame}.cpp`, `opengl_hook_internal.h`.
    - Streamline: `streamline_hook.cpp` + `streamline_hook_{helpers,state,startup,
      modules,originals,feature_fallback,install,resolve,dlssg,api,pcl}.cpp` +
      `streamline_hook_internal.h` + `streamline_inline_hook_batch.{h,cpp}` (one grouped
      quiescence for a module's core exports); `streamline_hook_pcl.cpp` captures the game's real
      SimulationStart/PresentStart PCL calls for D3D latency correlation without synthetic markers.
    - FFX: `ffx_hook.cpp` + `ffx_hook_{context,install}.cpp` + `ffx_hook_internal.h`.
    - Vulkan layer: `layer_capture.cpp` (facade) + `layer_capture_{d3d11_interop,
      textures,state,frame,capture}.cpp` + `layer_capture_internal.h`;
      `vulkan_present_boundary.h` (header-only) owns the async-present route detection
      (acquire-thread / submit-thread mismatch) with edge-triggered diagnostics and the
      limiter boundary-transition logs plus admission resets.
  - `common/` - overlay policy (`dx12_overlay_policy.h`, `streamline_runtime_policy.h`,
    `overlay_compat.h`), UE/NGX RR policies (`ue5_rr_override_policy.h`,
    `ngx_feature_lifecycle.h`), `dxgi_shared*.cpp` (central Present routing: hooks,
    hooks_present = entry/body inline-hook install and the leave-entry decision,
    hooks_present_vtable = swapchain vtable-slot ownership/repair/teardown, present,
    present1, routing, steam, resize, original),
    `present_interposer_cadence.h` + `present_interposer_tracking.cpp` (NVIDIA Smooth Motion:
    the interposer's private output chain measured against the application's present stream
    - see `present-interposers.md`), `fg_session_state*.cpp`
    (state core + names + log units), `custom_overlay_*.cpp` (per-backend + internal
    headers + render units), `overlay_adapter*.cpp` (adapter + render + render_frame),
    `custom_overlay_dx12_{render,inline_upload,retirement}.cpp` (callback draw recording, GPU-completed upload slots,
    and service-thread resource retirement; `custom_overlay_dx12_internal.h` groups the logical source),
    `system_metrics*.cpp` (metrics + gpu unit), `system_latency_metrics.h`
    (the PC-latency correlator; includes `system_latency_marker_reports.h`, the
    out-of-line marker path, at its end - mutual guarded include, either may be
    included first) + `system_latency_types.h`
    (snapshot/report/diagnostics vocabulary) + `system_latency_windows.h`
    (bounded rings + trimmed-mean sample window) + `system_latency_frame_begin.h`
    (process-wide present-return / low-latency-sleep-return anchor),
    `streamline_pcl_latency.h`
    (lock-free paired-marker history), `system_latency_native_d3d.cpp`
    (fresh Streamline PCL provider followed by already-loaded NVAPI), `reflex_limiter.h`, `fps_limiter.h` +
    `fps_limiter_policy.h` (pure pacing policy incl. `OutputGroupAdmission` ordinal classification and
    `NextRationalGroupIntervalTicks`) + `fps_limiter_detail/{apply,frame_pacing,lifecycle}.h`
    (out-of-line inline member definitions; apply.h owns the admission-epoch key, the boundary
    classification fast path, and the exact rational group cadence),
    `pacing_health_telemetry.{h,cpp}` (time-stamped/tagged FSR pacing windows and passive
    bad-start classifier), and `hook_thread_stage_cost.h` (disjoint housekeeping-stage costs).
  - `wrappers/` - `dxgi_swapchain_wrap*.cpp` (wrap, present, lifetime, frame_latency),
    `hook_system.cpp`, `iat_hook.*`, `vtable_hook.cpp`, `inline_hook*.cpp` (including
    two-phase grouped publication/commit), and `hook_patch_transaction.*` (single-target or
    grouped exact-range thread-quiesced code-patch transactions).
- `mediaengine/`
  - De-inlined `MediaEngine` class: `mediaengine.cpp` (facade) + semantic units:
    `mediaengine_audio_{helpers,thread,loop_poll,loop_commit,audio_pull,
    audio_pull_targets,audio_pull_encode_a,audio_pull_encode_b,audio_pull_encode_c,
    audio_pull_sync}.cpp`, `mediaengine_{init,config,transport,timeline,
    recording_start,recording_stop,frame}.cpp`, `mediaengine_internal.h`
    (declarations only).
  - `audio_*.cpp` - capture, loop, encoder (encode/flush), resampler, ring buffer,
    app-audio (activation/loop/monitor/queue units), process-loopback capture.
  - `video_encoder*.cpp` - encoder pipeline units (options, backend_options,
    configure, conversion, convert_bgra/shaders, encode, encode_input, finalize,
    face_camera, format, framegrab, lifecycle, start, streaming, textures, write,
    codec, options_bitrate); `face_camera_capture/renderer/shader.*` implement the
    asynchronous camera source and GPU composition layer.
- `tests/`
  - Google Test sources (`test_*.cpp`, including source-policy tests that read the
    logical unit: stem + `<stem>_internal.h` + sorted `<stem>_*.cpp` siblings) and
    the built `unit_tests.exe` plus copied runtime DLLs.
- `testapp/`
  - Graphics test apps: `dx12_fg_switch_*.cpp`, `vulkan_fg_switch_*.cpp` (+
    `vulkan_fg_switch_test_internal.h`), `dx12_av_sync_test*.cpp` (util/manifest
    units), `dx12_fsr_fg_test*.cpp` (fsr unit), `dx11_test.cpp`, `vulkan_*.cpp`,
    `run_tests.py` (integration runner, facade over `run_tests_{support,main}.py`).
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
  (552 translation units after the semantic-unit split).
- `build/verification/` - per-run gate summaries (`latest_summary.txt`), manifests,
  and stage artifacts (`clang_tidy.log`, `flake8.log`, `pyright.log`, sanitizer logs).

## High-Risk / High-Value Files

- `common/shared_defs.h` - shared-memory facade; the source-verified ABI version is `57` in
  `common/shared_defs_detail/abi_constants_and_config.h`.
- `captureengine/injection.cpp` + `injection_manager.cpp` + `injection_wmi_events.cpp` +
  `injection_inject.cpp` - ordered host-side startup/late injection, event-driven/polled process
  discovery, callback lifetime, duplicate-worker coalescing, resident target adoption, and deject
  acknowledgement.
- `captureengine/inject_lifecycle.cpp` + `hook/main_host_lifecycle.cpp` +
  `hook/vulkan_layer/layer_sharpen.cpp` (per-present recording) +
  `layer_sharpen_setup.cpp` (per-swapchain lifecycle) + `layer_sharpen_state.h` -
  the Vulkan half of the CAS/RCAS post-processing pass; see
  `post-processing-sharpen.md`. The cross-API policy, constants and D3D
  renderers are `common/sharpen_policy.h` and `hook/common/sharpen_*`. Two
  device-free policy headers carry the rules both backends share:
  `common/sharpen_policy.h` (when to run, in which colour space, at what strength)
  and `hook/common/sharpen_gpu_timeline.h` (what the GPU has finished with, what a
  change of submitting queue costs, what a failed submit does to a ring slot).
  `hook/vulkan_layer/layer_ipc.cpp` - host-stop, dormant, and target-specific
  reactivation lifecycle across host generations.
- `captureengine/inject_main.cpp` + `inject_config.cpp` + `inject_config_publication.cpp` - shared-memory
  setup/config reload orchestration, serialized resolved-config/discovery publication, and inject-overlay runtime
  handoff flags. The per-target profile prewarm lives on its own worker thread here; it must stay off the
  `ReloadConfig` handler's 1 s ack path (see `llm-wiki/process-ipc.md`).
- `captureengine/display_timing_service.cpp` + `display_timing_etw.h` (provider identity and real-time
  session plumbing) + `display_timing_nvidia.h` (NVIDIA scheduled-flip announcements) +
  `display_timing_correlation.h` (Intel/AMD FrameType correlation) + `display_timing_policy.h`
  (present/submission selection) + `display_timing_health.h` - the screen-change timestamp source behind
  `[Overlay] frametime_source=display_change` and the Present-to-display half of the PC-latency row; see
  `display-change-timing.md` and `overlay-rendering.md`.
- `captureengine/clr_interop.{h,cpp}` + `sensor_bridge_host.{h,cpp}` + `sensor_bridge_lhm.{h,cpp}` +
  `sensor_selection_policy.h` + `sensor_plugin.cpp` - the optional LibreHardwareMonitor integration. The bridge is a
  role of `captureengine.exe` (`--sensor-bridge`) that hosts the in-box .NET Framework runtime and drives the managed
  library through frozen mscorlib COM contracts; there is no script and no extra shipped file. Selection policy is
  dependency-free and unit-tested; see `overlay-rendering.md`.
- `hook/common/graph_scroll_policy.h` + `overlay_adapter_render.cpp` + `custom_overlay.cpp` - frame time graph
  scrolling: the cursor that advances one slot per drawn frame instead of per sample arrival, and the guard
  samples plus edge clipping that keep the plot filling its panel at any sub-slot offset.
- `hook/main.cpp` + `main_injection.cpp` - hook bootstrap and wrapper-init decisions.
- `hook/common/dxgi_shared.cpp` (+ `dxgi_shared_present*.cpp`, `dxgi_shared_steam*.cpp`)
  - central Present routing and startup bypass behavior.
- `hook/apis/dx12_hook_main.cpp` + `dx12_hook_fg_state.cpp` +
  `dx12_hook_fg_startup.cpp` + `dx12_hook_streamline_fg_transition.cpp` +
  `dx12_hook_focus_loss.cpp` + the themed `dx12_hook_*` helper units - DX12
  present/FG/overlay policy, exports, Steam/VA-space/focus helpers.
- `hook/common/dx12_overlay_policy.h` - dense policy helpers for DX12 overlay
  routing, startup coexistence, and FG transitions.
- `hook/common/streamline_runtime_policy.h` - Streamline PostSL startup/suspend
  policy helpers.
- `hook/common/overlay_compat.h` + `overlay_compat_detail/module_table.h` - loader-free
  hot-path identity for third-party overlays/injects and FFX modules.
- `hook/wrappers/inline_hook*.cpp` + `hook_patch_transaction.*` + `vtable_hook.cpp` +
  `iat_hook*.cpp` - foreign-chain preservation, ownership-only removal, callable-original
  publication, and exact-byte single/group thread-quiesced inline patching. The live-code
  writes themselves - CE's entry patch, its restore, and the quiescence-fallback retry that
  wraps both - are `inline_hook_entry_patch.cpp`; `inline_hook.cpp` owns the install/remove
  bookkeeping around them.
- `hook/common/ngx_drs_override{,_policy}.{h,cpp}` - `dlss_fg_preset`, `dlss_fg_mode`,
  `dlss_fg_fixed_count`, `dlss_fg_dynamic_max`, `dlss_fg_target_fps`: all five are
  driver-settings (DRS) keys, not NGX parameters, so this wraps the
  `NvAPI_DRS_GetSetting` pointer `nvngx_dlssg.dll` and `sl.common.dll` resolve and
  answers those reads process-locally. `_policy.h` is the pure, testable half.
- `mediaengine/mediaengine_*.cpp` + `mediaengine_internal.h` - capture/CFR/audio
  pipeline; the audio loop and pull phases live in the `mediaengine_audio_loop_*`
  and `mediaengine_audio_pull*` units.
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
  (`<tool>_*.py` unit families, `build_*.py`, `source_splitter_*.py`); the facades themselves
  (`build.py`, `source_splitter.py`) stay linted.
