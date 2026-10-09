# Inject Overlay Rendering

Last cross-checked: 2026-09-15 (adapter GPU load is the busiest engine, not the sum of concurrent engines; DX12 allocator-coupled glyph upload ownership; callback registry caching and hidden-callback GPU transparency; FSR-tagged pacing windows and PresentStart-to-screen attribution; application-source Present classification for proxy-swapchain frame generation; Streamline PCL marker capture, Vulkan layer-created queue loader data, optional LibreHardwareMonitor telemetry, marker-enhanced/fallback PC latency, actual display-change frame timing, split-renderer direct-child GPU telemetry provenance, DXGI/Vulkan presentation-color
contracts, HDR10 gamut/transfer correctness, per-monitor Windows SDR-white calibration, effective-monitor
inject-overlay DPI scaling, dynamic frame-time graph ceiling scaling, and runtime-owned FG UI transitions)

Primary sources:
- `captureengine/app/host_metrics.{h,cpp}`
- `captureengine/app/host_metrics_policy.h`
- `captureengine/sensors/sensor_service.cpp`
- `captureengine/sensors/sensor_plugin.{h,cpp}`
- `captureengine/sensors/sensor_bridge_host.{h,cpp}`
- `captureengine/sensors/sensor_bridge_lhm.{h,cpp}`
- `captureengine/sensors/sensor_selection_policy.h`
- `captureengine/sensors/pawnio_setup.{h,cpp}`
- `tools/build/build_lhm_plugin.py`
- `captureengine/sensors/clr_interop.{h,cpp}`
- `tools/build/build_{project_finalize,packaging}.py`
- `tools/licenses/LibreHardwareMonitor_NOTICE.txt`
- `captureengine/display_timing/display_timing_service.{h,cpp}`
- `captureengine/display_timing/display_timing_policy.h`
- `common/ipc/display_timing_shared.h`
- `common/ipc/shared_defs.h`
- `common/overlay/recording_indicator_policy.h`
- `hook/overlay/custom_overlay.{h,cpp}`
- `hook/overlay/custom_overlay_dx12.{h,cpp}`
- `hook/overlay/custom_overlay_dx12_render.cpp`
- `hook/d3d12/dx12_overlay_policy/upload_slot_guard.h`
- `hook/overlay/custom_font.cpp`
- `hook/overlay/overlay_adapter.{h,cpp}`
- `hook/d3d12/dx12_hook_types.h`
- `hook/d3d12/dx12_hook_types_impl.cpp`
- `hook/d3d12/dx12_hook_postsl_render_{route,submit}.cpp`
- `hook/metrics/performance_metrics.{h,cpp}`
- `hook/metrics/system_latency_metrics.h`
- `hook/metrics/system_latency_types.h`
- `hook/metrics/system_latency_windows.h`
- `hook/metrics/system_latency_frame_begin.h`
- `hook/metrics/system_latency_native_d3d.cpp`
- `hook/metrics/streamline_pcl_latency.h`
- `hook/streamline/streamline_hook_pcl.cpp`
- `hook/pacing/reflex_defs.h`
- `hook/present/{presentation_color,dxgi_presentation_color}.h`
- `hook/overlay/overlay_shader_{bytecode,spirv}.h`
- `hook/vulkan_layer/vulkan_presentation_color.h`
- `hook/vulkan_layer/vulkan_reflex_limiter.{h,cpp}`
- `hook/vulkan_layer/layer_overlay_queue.cpp`
- `hook/vulkan_layer/vulkan_loader_data.h`
- `hook/vulkan_layer/shaders/overlay_{solid,textured}.frag`
- `hook/metrics/system_metrics.{h,cpp}`
- `hook/overlay/overlay_layout_policy.h`
- `hook/overlay/legacy_overlay_cache.h`
- `hook/overlay/custom_overlay_dx{8,9,10}.{h,cpp}`
- `hook/overlay/custom_overlay_gl.{h,cpp}`
- `hook/d3d8/dx8_hook.cpp, hook/d3d9/dx9_hook.cpp, hook/ddraw/ddraw_hook.cpp, hook/opengl/opengl_hook.cpp`
- `hook/ddraw/ddraw_present_policy.h`
- `hook/ddraw/custom_overlay_d3d7.{h,cpp}`
- `hook/overlay/overlay_cpu_raster.{h,cpp}`
- `hook/ddraw/ddraw_native_overlay_damage.h`
- `hook/ddraw/ddraw_hook_overlay_composite.cpp`
- `hook/ddraw/ddraw_hook_overlay_route.cpp`
- `hook/ddraw/ddraw_hook_write_tracking.{h,cpp}`
- `hook/ddraw/ddraw_hook_capture_frame.cpp`
- `tests/test_ddraw_present_policy.cpp`
- `tests/test_legacy_d3d7_vtable_abi.cpp`
- `hook/ddraw/legacy_d3d_texture_bindings.h`
- `hook/ddraw/ddraw_hook_texture_bindings.cpp`
- `tests/test_legacy_d3d_texture_bindings.cpp`
- `tests/test_overlay_draw_bounds.cpp`
- `tests/test_overlay_system.cpp`
- `tests/test_host_metrics_policy.cpp`
- `tests/test_hardware_sensor_plugin.cpp`
- `tests/test_performance_metrics.cpp`
- `tests/test_system_latency_metrics.cpp`
- `tests/test_shared_runtime_state.cpp`
- `tests/test_vulkan_loader_data.cpp`
- `tests/test_dx12_upload_slot_guard.cpp`

## Summary

The inject overlay deliberately keeps the existing compact appearance and shared CPU-generated draw format. Solid geometry and textured glyphs remain batched into the existing small command set; the 2026-07-16 polish is a local visual-quality, layout-consistency, and legacy-hot-path change rather than a renderer redesign. The entire overlay stack — text, metrics, the PC-latency row, and the frame-time graph — is first-party code: API-native custom renderers, a GDI-rasterized custom font atlas, and in-repo precompiled shaders, with no Dear ImGui or other third-party overlay/UI library.

## Layout and row invariants (`overlay_layout_policy.h`, `overlay_adapter_render.cpp`)

- Frame Generation rows (`Base/Display` rates and `FG Status`) appear atomically when frame generation is active (`fgActive == true`).
- `Base/Display` under DLSS/FSR FG is **measured** (`PerformanceMetrics::RefreshMeasuredFGRates`, 2026-10-06): output
  from the median display interval, base from the application-Present cadence or, when that stream is stale (DLSS-G),
  the trusted Reflex marker cadence; base is capped at output. The runtime-reported figures (`g_FGCompat` frame
  history) only stand in when neither stream is arriving, and Smooth Motion keeps its own interposer rates. Reason:
  the frame history is fed only by `ProcessFrame` paths, which DLSS-G's PostSL route never runs, so Talos
  `20261006_194753` showed `66.6 / 133.2` for 35 s while the screen changed 48-53 times a second, and `33.3 / 133.2`
  with DLSS-G idle (`ON but NOT interpolating`); session `20261006_150600` had the same frozen values. Divergence is
  logged as `[Overlay] FG rates diverged from the runtime's figures` (state change, at most every 10 s); the latency
  sample line carries `reportedBaseFps=`/`reportedOutputFps=` beside the measured ones.
- Inactive FG never reserves phantom empty rows in `BuildOverlayRowMask`, and `OverlayAdapter::RenderContent` advances `cursorY` only when text is actually rendered. This prevents blank gap lines from appearing in the overlay when FG is toggled off or during teardown transitions.

## HDR presentation and color invariants

- Storage format is never treated as content metadata. DXGI `R10G10B10A2` can be SDR/Rec.709 or HDR10/PQ, and FP16 is scRGB only under the matching swapchain color-space contract. CE tracks successful `IDXGISwapChain3::SetColorSpace1` calls through exactly one publisher: the DXGI wrapper owns wrapped calls, while a separately installed inline hook owns unwrapped calls, refuses wrapper objects as hook targets, and publishes its atomic trampoline before the detour becomes live. The color path must never patch shared DXGI vtable slot 38; doing so composed the wrapper with its own detour and caused the Strange Brigade DX12 null-execute crash. State is retained as swapchain private data, unchanged repeated calls avoid another write/log, and an untracked swapchain uses DXGI's SDR default. Vulkan retains `VkSwapchainCreateInfoKHR::imageColorSpace` and resolves format plus color space together. Unsupported combinations fail closed instead of receiving an incorrectly encoded overlay.
- HDR state is published independently of overlay visibility, so hiding the overlay cannot change inject-video classification. D3D10/11, D3D12, Vulkan, screenshots, and runtime-owned Streamline/FFX UI/backbuffer routes consume the same presentation meaning. Cached runtime-owned UI renderers update HDR mode when a same-format target changes between SDR and HDR.
- DX12's secondary renderer is a separate `OverlayAdapter`: x64 descriptor-free, x86 Texture2D, normal backbuffer, offscreen-copy, and PostSL routes all use it. Immediately before each draw it must receive the cached presentation HDR decision plus the actual target format. Session `20260719_214733` proved that synchronizing only the primary adapter leaves this secondary adapter in SDR mode, writes sRGB endpoints directly into a PQ target, and makes a later correct HDR-to-SDR conversion pull overlay colors toward white. Transition-only logs publish the synchronized secondary contract.
- `OverlayBackendType::DX12` identifies an API, not a concrete renderer. The descriptor-free backend is a sibling
  of `CustomOverlay::DX12Backend`; it cannot receive that class's non-virtual calls or inline-upload retirement.
  `RendererBackend::AsTextureDX12Backend` advertises the concrete texture interface once at adapter binding;
  `OverlayAdapter::textureDX12Backend` retains it until teardown. Generic upload-slot calls remain virtual.
  Verified 2026-10-09: native sentinel-storage/rebind regressions and the real WARP format probe's texture/custom
  adapter lifecycle. `DX12 renderer binding` logs each binding; see `log/recent.md` for the startup fault evidence.
- The DX12 descriptor-free backend's pipelines are bound to a render-target format, and the format is the one of the
  back buffer written NOW, not the one tracked at init (`FrameProcessSession::RecordOverlayDraw` syncs
  `g_State.format` from `bb->GetDesc().Format` before `EnsureDescFreeBackendForDeviceAndFormat`). A game can move its
  swapchain between formats without CE seeing a resize (Steam/Rockstar-style overlays own the `ResizeBuffers` entry,
  so `InstallResizeReconciliationHooks` refuses a site). Witcher 3 DX12 does `R8G8B8A8` -> `R10G10B10A2` +
  `SetColorSpace1(HDR10)` -> `R8G8B8A8` + G22 while starting: session `20261008_184332` shows one `format=24
  colorSpace=12` frame held 288 ms, drawn through the `fmt=28` pipelines (D3D12 error #613 "pipeline state =
  R8G8B8A8_UNORM, render target format = R10G10B10A2_UNORM"; the 8-bit packed output is undefined into a 10:10:10:2
  target, hence the saturated fonts). `DX12DescFreeBackend::SetTargetFormat` keeps one pipeline pair per format (max
  4, never released before Shutdown because the GPU may still read them) and `DecideDescFreeBackendAction` rebuilds
  only on a device change. Diagnostics: `Back buffer format changed without a resize CE saw`, `DescFree: render target
  fmt a->b (pipelines created|cached pipelines)`, `DescFree backend retargeted fmt`. Tests:
  `test_dx12_descfree_target_format.cpp`, flow `FlowOverlayFormat.*` (the probe `CEFlow_ProbeDescFreeTargetFormats`
  drives the real backend into four formats under the debug layer and reads the texels back; with the retarget
  disabled it fails with error #613). Not covered: the x86 Texture2D backend (`InitDX12` fixes its format at first
  init). Last verified 2026-10-08 (unit + flow + debug layer; in-game run pending).
- Overlay source colors and the font atlas are sRGB/Rec.709. scRGB targets decode sRGB and scale linear values at `80 nits = 1.0`; HDR10 targets additionally transform linear Rec.709 to Rec.2020 before ST 2084 encoding. Omitting that gamut transform was the cause of over-saturated/wrong-hue HDR overlay colors. PQ inputs are clamped to the defined 0-10,000-nit domain.
- `[Overlay] hdr_paper_white=auto` resolves the target window's current monitor, reads `DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL`, and converts the Windows calibration with `(raw / 1000) * 80 nits`. It is cached per monitor and falls back to 203 nits only when Windows cannot report it. This aligns overlay white with Windows-mapped SDR UI rather than using a hard-coded 200-nit assumption. An explicit nit value remains available for deliberate calibration.
- The HDR shader adds only a small Rec.709-to-Rec.2020 matrix to the existing per-overlay-pixel transfer work. It does not add a full-frame pass, copy, readback, wait, per-frame allocation, or display-capability query.

## Shared visual and layout invariants

- Each rebuild captures one `FrameLayoutSnapshot`: FG activity/type/multiplier/rates, recording state/time/warning inputs, notification state, and one row-presence mask. The mask and its row count are the single source of truth for text, panel height, and graph placement.
- Recording hotkeys publish an atomic `Video`/`AudioOnly` intent in unused runtime-flag bits before controller readiness waits. With `overlay_enabled` and `show_recording` enabled, pending video renders amber `STARTING RECORDING...` and pending audio renders amber `STARTING AUDIO...`. Media clears pending when output becomes live, where the existing red `REC`/`AUDIO` timer takes over; live state has precedence over stale pending state, and pending never starts the timer.
- OFF/DLSS/FSR transitions, DLSS-to-FSR identity changes, 2x-to-4x changes, row configuration changes, recording changes, notifications, and temporary FG-space reservation changes invalidate the cached frame immediately. OFF stays compact; the two FG rows appear or disappear atomically.
- Pending/live recording-state transitions invalidate the frame cache. Layout measurement reserves the widest ordinary and pending recording labels, plus all known FG labels, 4x, four-digit Base/Display and FPS values, percentages, memory values/capacities, recording warnings, and notifications. Encoder warnings remain suppressed until established recording. Changing digit counts must not resize or clip an already-present row.
- The frame-time graph retains all 180 raw samples. Its vertical ceiling is dynamic: at least 50% headroom above the recent average, at least 2x the minimum, a 33 ms floor so the 30 FPS threshold stays visible, and about 15% padding below the lowest sample; the ceiling label refreshes at most every two seconds. X positions use exact endpoint interpolation instead of a rounded step plus edge clamping. The line uses bounded miter joins with a bevel fallback and a one-physical-pixel transparent AA fringe in the existing solid draw command.
- Glyph cells use measured GDI ink extents, two transparent texels around each cell, clipped rasterization, and `GdiFlush` before atlas reads. Text and shadow derive from one snapped physical-pixel origin. Font, colors, metrics, linear sampling, and the x86 DX12 solid-glyph-span path are unchanged.
- A DX12 overlay command allocator and the persistently mapped VB/IB storage recorded through it have one GPU lifetime. Normal and PostSL draws therefore force upload slot `N` to the already-proven-complete allocator slot `N`; both pools derive their ordinary 16-slot count from `kAllocatorCoupledUploadSlotCount`. PostSL additionally guards that slot with the exact `g_State.fence` value signaled after its ECL. Never rotate the descriptor-free uploads through a smaller independent ring or publish guard zero merely because DLSS-G is active: four uploads can wrap while any of the other allocator slots remain in flight, letting the CPU mix old and new glyph vertices. The `33 ms` scale-label capture from 2026-09-13 showed the first `3` as a box while the adjacent identical `3` remained correct, which rules out the source string and atlas entry and is the characteristic per-instance geometry tear.
- Inject-overlay scale is resolved once when its font atlas/backend is initialized from the nearest display's
  effective DPI (`GetDpiForMonitor(MDT_EFFECTIVE_DPI)`), with the shared legacy-DPI fallback. The target game
  window's awareness-dependent virtualized DPI must never be used: a DPI-unaware game can report 96 on a 150%
  display while its swapchain later changes from logical to physical resolution. The warm DX12 resize path
  intentionally preserves that font atlas, so correct initialization is the boundary that survives Alt+Tab and
  fullscreen recovery.
- RAM/VRAM never use fabricated capacity values. A valid used value renders even when total capacity is unavailable; RAM capacity is queried once with `GlobalMemoryStatusEx`, and unavailable GPU/VRAM telemetry renders as `--` rather than a false zero.
- Optional LibreHardwareMonitor temperature, power and fan values append compact ASCII suffixes (`C`, `W`, `RPM`) to the existing CPU/GPU rows. Clocks and voltage instead occupy their own `GPU Clocks` / `CPU Clocks` rows (`kRowGPUClocks`, `kRowCPUClocks`), with CPU clocks formatted as `5000 MHz (5200 MHz)` for the average and current maximum. The maximum includes every readable physical core, excludes bus/effective clocks, and follows the existing `cpu_core_clock` enable/disable selector. Aggregate-only backends omit an unavailable maximum. These rows exist because appending `MHz`/`V` to the usage rows would push the widest row - and with it the whole adaptive overlay width - well past the memory rows. A clock row is reserved only while its parent usage row is shown *and* at least one of its own sensors is readable, so an unelevated run never leaves a labelled blank line. Measurement and drawing consume the same cached formatting output, recomputed on a layout/text refresh rather than on every rendered frame, so values cannot clip merely because a sensor becomes valid.
- Only the leading load percentage carries `GetLoadColor`; the appended sensor readings are drawn separately in `Colors::SensorValue`. `FormatCpuMetricsValue`/`FormatGpuMetricsValue` return the byte offset where the readings start, and the renderer measures the full run and the suffix run to right-align both spans. Drawing the composite in one color turned temperature, power and fan red the moment GPU load crossed the 85% threshold, reading as though those sensors were themselves critical.
- Zero means "not readable" for every metric except the fan: a package reporting 0 C, 0 W, 0 MHz or 0 V is reporting nothing, while 0 RPM is a genuine stopped fan. The rule is enforced three times independently - the bridge's `policy::IsReportableReading`, `ParseSensorValue`'s `rejectZero`, and `IsSaneHostMetricsPublication`.

## Host telemetry and adapter identity

- GPU and VRAM polling is out of process and does not depend on whether the game uses DirectDraw, DX6/DX7, or a modern API. The old-API failure was adapter identification: the host previously ignored its target PID and required a nonzero hook-published LUID before initializing or filtering GPU counters.
- **A process with no GPU-engine counter instance is idle, not unreadable.** Windows publishes a `GPU Engine` instance for a process only while it has work on that engine. Marking a poll that found none as unavailable made the overlay's GPU row flap between a value and `--` at the poll rate whenever a title went briefly idle (Gothic II intro logos, session `20260914_190240`). `metrics_policy::GpuLoadIsReadable`: a resolved adapter plus a successful query is readable, and no matching instance means 0%. VRAM usage has the same instance behaviour but no truthful zero, so `VramUsageIsReadable` holds the previous reading across a bounded run of missing samples before giving up.
- A graphics-published adapter LUID is stamped with the publishing process ID. It wins when that PID is the selected game or a live direct child of it; the latter preserves the configured/injected parent as profile source while a split renderer owns final presentation. The sensor service resolves that parent relationship from the live process table instead of accepting any foreign publisher. When no trustworthy LUID is available, the host parses the target process's Windows `GPU Engine` PDH instances, selects the adapter with the highest non-video-engine load, and retains the prior process-derived adapter across a valid zero-load tie or a temporary missing sample. An ambiguous initial multi-adapter tie remains unavailable instead of guessing. This keeps multi-GPU selection deterministic without using API-specific guesses.
- **The adapter's GPU load is its busiest engine, never the sum of its engines.** Windows publishes one
  `\GPU Engine(*)\Utilization Percentage` instance per (process, adapter, physical engine), and those engines run
  concurrently - 3D, Compute and Copy can all be busy in the same microsecond - so adding them is not a fraction of
  elapsed time. CE summed every non-video instance and clamped the result to 100, which meant the reading sat on the
  clamp long before the GPU was busy. Frame generation is where that became unreadable: the generator's work lands on
  compute beside the game's raster on 3D, so Portal RTX `20260914_120049` stepping from 3x to 4x multi-frame
  generation - a measured drop from 47.8 to 36.0 base renders per second, a quarter of the render work - moved the
  reported load not at all. `metrics_policy::ResolveAdapterGpuLoadPercent` now sums within one engine, where
  processes genuinely time-share it, and takes the maximum across engines; `ParseGpuEngineKey` keys that grouping on
  the instance name from `phys_` onward so the engine's identity does not depend on which process used it. This is
  the aggregation Task Manager reports. Covered by `tests/test_host_metrics_policy.cpp`.
- Shared GPU usage, VRAM usage, and VRAM capacity have independent validity bits. A real 0% or 0 MB sample is therefore valid, while a missing/invalid counter remains unavailable. Adapter/source metadata and an even/odd publication sequence let the hook consume one coherent snapshot and clear old values when the source PID or adapter changes.
- LibreHardwareMonitor is never loaded into the controller, hook, Vulkan layer, or game. The bridge is first-party native code compiled into `captureengine.exe`: since 2026-09-03 there is no script, no interpreter, and no additional shipped file. The dedicated sensor service monitors the controller process lifetime, launches `captureengine.exe --sensor-bridge` suspended, assigns it to a kill-on-service-close job, and only then resumes it. The launch uses an explicit inherited-handle list, NUL stdin/stderr, a bounded stdout protocol, and a random named shutdown event. The bridge enables only the requested CPU/GPU visitors. `auto` GPU selection follows the device with the highest valid `GPU Core` load and retains the previous device across a tie; an ambiguous initial tie remains unavailable, while an exact identifier can pin another sensor. Apart from the derived CPU clock summary described below, within a device `auto` sensor selection is ranked and deterministic: exact preferred name, then the lowest-numbered instance of that name, then the previously selected identifier while it stays usable, and only then the highest current reading. The indexed and sticky tiers exist because the value comparison alone reselected a different sensor almost every poll on hardware that numbers its sensors - two idle `GPU Fan 1`/`GPU Fan 2` readings a few RPM apart alternated the reported fan and re-logged `Selected gpu_fan=` once per second. The native reader rejects malformed/non-finite/out-of-range output and expires a snapshot after `max(5 seconds, 3 * poll interval)`.
- The bridge role hosts the .NET Framework 4 runtime that ships with Windows (`mscoree!CLRCreateInstance` -> `ICLRMetaHost` -> `ICorRuntimeHost`) and drives the managed library through four frozen mscorlib COM contracts: `_AppDomain` slots 37/38 (`CreateInstance`/`CreateInstanceFrom`), `IObjectHandle` slot 3 (`Unwrap`), `_Object` slots 7/10 (`ToString`/`GetType`), and `_Type` slot 57 (`InvokeMember_3`). Name-based `IDispatch` is unusable in both directions: the CLR answers `GetIDsOfNames` with `E_NOTIMPL` for the mscorlib interfaces (their dispatch is typelib-backed and `mscorlib.tlb` is unregistered by default), and LibreHardwareMonitor's concrete hardware classes are internal, so their CCWs expose no class interface at all. `_Object::GetType` + `_Type::InvokeMember` reaches every public member regardless of COM visibility, and is the only mechanism `clr_interop.cpp` uses.
- Root hardware cannot be read from `IComputer.Hardware`: it is `IList<IHardware>`, a constructed generic type, and the runtime refuses to marshal one to a COM interface pointer (`InvalidOperationException`, HRESULT 0x80131509). The bridge instead binds the public `HardwareAdded`/`HardwareRemoved` events to two `System.Collections.Queue` instances via `Delegate.CreateDelegate` - relaxed parameter binding lets the `IHardware`-taking handler bind to `Queue.Enqueue(object)` - and drains them each poll. Below the roots, `IHardware.SubHardware` and `IHardware.Sensors` are plain arrays and marshal as SAFEARRAYs. `SensorType`/`HardwareType` ordinals are resolved from the loaded assembly's own metadata with `Enum.GetNames`/`Enum.Parse`, never hardcoded, and any `HardwareType` member whose name starts with `Gpu` counts as a GPU.
- Automatic CPU clock selection uses `captureengine/sensors/cpu_clock_policy.h`: physical `P-Core #N` readings alone form the hybrid average; conventional `Core #N` clocks form the all-core average. Bus/effective clocks and existing aggregates cannot contaminate it. Classified but unreadable P-cores do not fall back to E-cores or mixed aggregates. Aggregate-only backends and explicit sensor overrides remain supported. Synthetic identifiers report the averaging scope and readable core count through the existing selection-change diagnostic. Regression coverage: `tests/test_cpu_clock_policy.cpp` (2026-10-05).
- Sensor selection lives in `captureengine/sensors/sensor_selection_policy.h` as dependency-free logic with direct unit coverage in `tests/test_sensor_selection_policy.cpp`; the same header declares the ten-metric wire order (nine configured selectors plus the derived maximum CPU core clock), maxima and zero-rejection rule that `sensor_plugin.cpp`'s parser reads, so the emitter and the parser cannot drift.
- Since 2026-09-03 CaptureEngine installs the LibreHardwareMonitor closure itself instead of asking the user to assemble it. `tools/build/build_lhm_plugin.py` fetches the official v0.9.6 `LibreHardwareMonitor.zip` over HTTPS, verifies it against a pinned SHA-256 **and** byte size, and extracts exactly four hard-coded base names: `LibreHardwareMonitorLib.dll`, `System.Memory.dll`, `System.Numerics.Vectors.dll`, `System.Runtime.CompilerServices.Unsafe.dll`. Destinations are built from that constant list, never from archive member paths, so a traversal entry cannot escape the plugin directory; duplicate candidates, oversized members, non-PE payloads, and a Microsoft dependency that lost its Authenticode certificate are all refused rather than resolved. A verification failure is fatal; an unreachable network is not, because the integration is optional. `installed-files.json` records the per-file digests so a later build re-installs a tampered or stale file. Covered by `tools/tests/test_lhm_plugin.py`.
- The release archive's plugin allowlist is those four files plus the directory README. Everything a user adds locally - the GUI executable, PDBs, storage/SMBus helpers - is still excluded, and `tools/licenses/LibreHardwareMonitor_NOTICE.txt` now carries the MPL-2.0 source-availability statement that shipping the binary requires. The package allowlist excludes every locally added plugin DLL/notice, and `tools/licenses/LibreHardwareMonitor_NOTICE.txt` records MPL-2.0/source/third-party references and the combined-redistribution boundary.
- All telemetry readers first validate the shared-memory ABI's exact version, size, and layout fingerprint. ABI 48 added the display-timestamp stream, ABI 50 added optional hardware-sensor values, ABI 51 added final-output timing metadata, ABI 52 added `OverlayConfig::showSystemLatency`, ABI 53 scopes Vulkan/DLSS FG publications to their renderer process tree, ABI 54 adds the runtime `PresentStart` associated with each display-timing sample, and ABI 55 appends the hardware-sensor CPU/GPU core clock, GPU memory clock and GPU core voltage; ABI 68 adds an independently valid highest physical CPU core clock (2026-10-05). Version/fingerprint isolation prevents an old reader from interpreting shifted fields; range/finite/validity checks remain a second line of defense.
- Per-core load calculation rejects regressing kernel/user/idle counters, addition overflow, and idle-underflow before computing and clamping the busy percentage. This prevents a genuine counter discontinuity from becoming an unsigned multi-billion-percent value independently of ABI validation.
- RAM publication is independent of CPU load. The earlier `RAM: -` case came from copying RAM only when the CPU sample was greater than zero; a valid RAM sample now updates even when CPU is unavailable or exactly 0%.
- DirectDraw overlay-only runs publish the first DXGI adapter's LUID without creating a D3D9 helper device. A recording run replaces it with the exact adapter of its D3D11 capture device; PID inference covers startup and any path that cannot publish an exact LUID.

## Frame-time source and realtime publication

- `[Overlay] frametime_source=display_change` is the default. It derives frame time, FPS, lows, variance, graph samples, and stutter state from consecutive visible screen-change timestamps, so generated output frames and variable-refresh scanout cadence are represented. `presentation` retains the former application-presentation timestamp behavior.
- The sensor child owns the Windows graphics event session; no tracing or metadata decoding runs in an injected process. The session exists only while at least one injected target requests display timing. It associates runtime presents with graphics-queue submissions, direct-flip completion, multi-plane display completion, and generated-flip timestamps for either the selected source PID or its validated direct-child renderer.
- The session requests an 8 ms flush cadence. A 24 ms chronological reorder window merges generated and application-frame events whose delivery order differs from their screen timestamps. Duplicate or regressing timestamps are dropped, association tables are age-pruned, and each shared-memory target receives a monotonic stream. Event-buffer loss is counted in the shared diagnostics and logged at most once per ten seconds.
- ABI 48 carries a 512-slot single-producer/multi-consumer ring. Each DXGI or Vulkan overlay owns an independent cursor; the sensor never waits for readers. Generation changes reset stale history safely, slot sequence validation detects overwrite, and graph data is consumed on every overlay draw rather than at the text refresh interval.
- `PerformanceMetrics` keeps independent display-change and presentation series. Presentation history stays warm while display timing is selected. The requested display source becomes effective only after a healthy sample arrives and automatically falls back when collection is unavailable, denied, failed, or stale for two seconds. Source transitions are rate-limited in the hook log; the sensor records startup/access failures once.
- **A live, correctly-counting display stream is not automatically a screen clock.** Under variable refresh below the panel's cap the vertical-blank clock has no grid, and every deferred flip completion then reaches the overlay carrying the moment the driver latched the flip rather than the moment the screen changed. Each publication therefore says which it is, and `RefreshEffectiveSource` reports presentation timing for a stream that is not publishing screen times - see `display-change-timing.md` for the measurement (two Talos FSR-FG runs of one build reporting 4x and 2x the frame-time standard deviation the same frames had at `Present`, with correct means) and for the thresholds. `[Overlay] Frame timing source:` carries `screenTime=` and `screenTimeShare=` so a `display_change` request reporting `presentation` is explicable from the log alone.
- **A runtime present and the kernel present submission that carries it are not on the same thread.** D3D11 and D3D12 hand the packet to a runtime worker thread of the same process, so the present is keyed by process and the thread only refines the choice inside it (`SelectDisplaySubmissionPresent` in `display_timing_policy.h`: exact thread first, otherwise that process's oldest outstanding present). Measured on `dx11_test`: 865 runtime presents and 865 present-marked kernel submissions from two different threads of one process - an exact-thread-only rule associated zero of them, and the entire stream stayed empty while every overlay silently fell back to presentation timing.
- The service logs stage counters (`runtimePresents`/`submitAssociations`/`queued`/`published`/`suppressed`/`regressed`) every ten seconds - as a warning while nothing has been published, otherwise at debug level - because a session that runs but correlates nothing is otherwise indistinguishable from one that never started. `ProcessTrace` returning anything other than success or `ERROR_CANCELLED` marks the stream failed.
- Each published display sample also retains the runtime `PresentStart` selected by the same process/thread-aware reducer. The PC-latency marker path uses that association as its causal upper bound, so a newer marker submitted while an older DLSS-G frame is still queued cannot be paired with the older frame merely because its eventual screen timestamp is later. Samples without an association retain the timestamp-only fallback behavior.
- Measured on this hardware (144 Hz VRR, NVIDIA): `VSyncDPCMultiPlane` reports `FlipEntryCount=0` and `VSyncDPC` reports `FlipFenceId=0`, so the usable completions arrive on `HSyncDPCMultiPlane` and `MMIOFlipMultiPlaneOverlay`. Stale-risk/unverified: a display path that emits neither - only `VSyncDPCMultiPlane` with a zero entry count - would need the `InterruptTargetPresentId` route, which is deliberately not implemented because it can double-count against the submit-sequence route.

## PC latency source hierarchy

- `[Overlay] show_system_latency=true` adds one independent row. A fresh marker-enhanced sample renders as `PC Latency~`, the no-marker path renders as `Latency est.`, and an unavailable sample renders as `PC Latency --`; the labels deliberately do not imply an exact end-to-end instrument reading. When frame generation is configured and known but not actively producing extra displayed frames, the label adds `(FG idle)` (`PC Latency~ (FG idle)` or `Latency est. (FG idle)`) to disclose that the generator is not active instead of attributing no-FG latency to frame generation.
- The D3D preferred path consumes the game's real Streamline `slPCLSetMarker` SimulationStart/PresentStart calls when
  `sl.pcl.dll` is present, otherwise it queries `NvAPI_D3D_GetLatency` only from an already-loaded NVAPI module.
  Streamline PCL does not populate `NvAPI_D3D_GetLatency`: the latter reports markers submitted through
  `NvAPI_D3D_SetLatencyMarker`. CE therefore intercepts the existing PCL calls with a lock-free fixed-capacity history;
  it does not fabricate or inject a competing marker stream. A PCL history expires after two seconds so plugin unload,
  stopped traffic, or an integration without usable pairs returns naturally to native NVAPI or the fallback estimate.
  Vulkan continues to query `vkGetLatencyTimingsNV` when the game exposes `VK_NV_low_latency2`.
- The marker-enhanced path correlates each displayed frame with simulation-start and present-start markers in the
  display-timing QPC domain, bounded by the reducer's associated runtime `PresentStart` when available. A report's
  measured marker cadence is authoritative; nominal FG metadata is only a fallback when the report has no usable
  cadence. Native Reflex reports can also supply GPU-completion timing, but the public reports do not expose NVIDIA
  PCL's ETW input ping, so average input wait remains estimated and the tilde is retained. Marker reports arriving
  at output cadence during active frame generation are rejected because they cannot identify which frames are
  application-rendered, falling back cleanly to the estimated path.
- Without usable markers, the estimate is `present-to-display + frame-begin-to-present + average input wait`, all three
  in the same QPC-microsecond domain.

### Present/display pairing is the load-bearing part

- A displayed transition is attributed to the Present that caused it through the reducer's associated runtime
  `PresentStart`, and the newest observed Present at or before it is that same frame: the runtime emits `PresentStart`
  inside the `Present` call the wrapper already timestamped, so hook entry and ETW event interleave strictly per frame
  on the calling thread.
- **This is what makes render-ahead visible at all.** Matching a display to "the newest Present that preceded its screen
  time" collapses every queue depth to roughly one frame, because by the time a frame is scanned out the game has
  already called `Present` for the frames queued behind it. That queue is exactly what a low-latency mode removes, so
  before 0.1.6371 the estimate could not distinguish Reflex on from Reflex off at equal frame rate. Regression coverage:
  `RenderAheadQueueIsMeasuredThroughThePresentAssociation`, `ShallowerQueueReportsLowerLatencyAtIdenticalFrameRate`.
- Displays with no association keep the documented degraded timestamp-only behavior for both paths. The ratio is
  logged (`displays=` vs `associated=`) because a reading taken on the degraded path cannot be compared against one
  taken on the associated path.
- **The association only exposes a queue that lives below the runtime Present.** A frame-generation runtime that
  paces output from its own presenter thread keeps the game's frames in *its* queue, above DXGI: the associated
  `PresentStart` is then the generator's present, a few milliseconds before scanout, and `presentToDisplay` measures
  none of the wait. Session `20260904_034526` on Talos: 2-4 ms under FSR FG against 17-25 ms under DLSS FG at nearly
  identical application (21.0 / 23.2 ms) and output (11.2 / 11.0 ms) cadence. What covers that span is the measured
  generator hold, which needs the application-source Present stream below.

### The application-source Present stream

- Under frame generation the final-output Present stream is the generator's, so the application's own Present has to
  be classified separately (`ObserveApplicationPresent`). Without it `MatchApplicationPresentLocked` fails, the hold
  degrades to the modelled `(fgMultiplier - 1) x displayInterval` floor, and `ResolveWorkIntervalLocked` falls through
  to the FG runtime's *published* base FPS instead of measured cadence - three modelled terms in a row, none of them
  visible in the published number.
- Two producers exist, one per runtime topology. Streamline/DLSS-G presents through CE's own `ProcessFrame`, so
  `DX12_ObserveApplicationSourcePresentTiming` runs there under the `applicationSourcePresent` guard
  (`ShouldApplyDX12PrerenderLimitOnPresent`: the tracked game Present thread only). FidelityFX does not - the game
  presents into AMD's frame-generation swapchain proxy and AMD's presenter thread issues the real DXGI present - so
  the proxy Present detour classifies it directly (`DX12_ObserveFFXProxyApplicationSourcePresent`, outermost entry,
  before any routing decision: the measurement must not depend on which overlay-composition route is live). A
  same-frame duplicate from a synchronous passthrough is rejected by the 3 ms minimum application interval.
- The stream carries the frame-begin anchor with it, which is the larger half of the win: a game that calls its
  low-latency sleep under a proxy generator (Talos does, under FSR FG) becomes fully measured rather than
  hold-corrected. Measured on hardware: `frameBeginInterval` went from `0us` to `22268-23328us`, and
  `anchorToPresent` from a modelled `baseInterval + displayInterval` floor of 35 ms to a measured 42-55 ms.
- `generatorHold` is `measured` whenever the anchor was stepped back onto an observed application frame - in the
  anchored branch as well as the no-anchor one, since the step-back is what carries the hold in both. Only the
  `expectedGeneratorHoldUs` addition is a model.
- Stale-risk: any other generator that paces from its own thread (Intel XeSS-FG, AFMF, third-party proxy swapchains)
  has the same topology and no producer wired. `generatorHold=modelled` while `generationObserved=1` is the symptom.
- **A stream that stopped is not a cadence.** The application-interval median counts only while the newest application
  Present is within 250 ms of the newest present or display (`IsApplicationPresentStreamFreshLocked`). Witcher 3 under
  bridged DLSS-G (session `20261001_105517`) let the game's own Present reach DXGI during the generator's ~1.6 s
  startup only, at hitch cadence. The frozen 141 ms median broke both paths for the whole session: the estimate's
  base-interval bound (100 ms) rejected every sample (`rejected=... base=...`). The 28.9 ms PCL markers also read as
  output-rate (`markerTrusted=0`, `markerCadenceRejects` climbing). The chain log prints `appStream=fresh|stale`, and
  `applicationInterval` is the effective cadence after the gate. Regression:
  `StaleApplicationStreamDoesNotDisqualifyMarkersUnderGeneration`, `...DoesNotRejectEveryEstimateUnderGeneration`.

### The generator's queue depth

- Stepping the anchor back **one** application frame is the interpolation hold: the generator must hold a complete
  source frame to interpolate toward. That is not the same quantity as the game's queue depth, and the two coincide
  only when a low-latency mode pins the queue to one frame. Reflex does; nothing else does. So under DLSS-G the
  single-frame step was right, and under FSR FG - where Talos runs Reflex off, and NVIDIA's driver offers no
  Anti-Lag - the game runs ahead into FidelityFX's own queue and the frame on screen is as far behind as that queue
  is deep. Evidence that the queue is real and saturated: `[OVERLAY COST] FFX proxy Present ... runtimePresentAvgUs`
  measured 4857-9163 us of blocking per ~22 ms application frame, which is back-pressure from a full queue.
- The depth is counted by conservation, not inferred from timestamps: every application frame is displayed exactly
  `fgMultiplier` times, so the cumulative difference between the two streams is the number still in flight. Any
  timestamp-based rule would be circular with the assumption being tested.
- Only the *difference* carries the depth, so the count means something only from a point where the queue was empty.
  Two such points exist and both are observable: frame generation switching on (the runtime has produced nothing
  yet) and an application-present gap over 250 ms (the queue drained). The depth then emerges as the number of
  application frames issued before the first group reaches the screen.
- Conservation is invalid across a broken stream, so the count is dropped - back to the single-frame hold - on a
  display gap over 250 ms and on `NoteDisplayStreamGap()`, which `PerformanceMetrics::ConsumeDisplayTiming` calls
  when it falls behind the publication ring and skips sequences. An uncounted retirement would otherwise inflate the
  depth for the rest of the epoch, which is the dangerous direction; a missed application present deflates it, which
  degrades toward the floor.
- Guard rails: the step back never goes below one (so a measurement failure is exactly the previous behaviour),
  never above `kMaximumQueueDepth` = 8, and is trimmed while the stepped anchor is older than the correlator's
  250 ms interval bound. Published as `appQueue=` in the chain line - `0` means not measurable, never "empty".
- **A generator can discard frames, which breaks conservation in the dangerous direction.** GTA FSR FG
  (`20260925_233000`) shows no display for ~500 ms after switch-on while the game presents 25-35 frames; the count
  pinned at 8 and the published latency read 110-150 ms all session. `RejectImpossibleQueueCountLocked` now latches
  the seed unmeasurable once the count exceeds `kMaximumQueueDepth + 1` (the newest frame in transit) or displays
  over-retire by more than one frame; counted as `queueCountRejects=` in the chain line.
- **The DLSS-G marker cross-check is the regression test.** Under Reflex the depth must measure 2 (one queued frame
  plus the interpolation hold), which reproduces the previous step exactly, so a published DLSS FG value that moves
  means the count is wrong.
- Stale-risk: measured only in unit topologies so far. Hardware run pending; the numbers to read are `appQueue=` in
  the chain line and whether the DLSS FG cross-check still agrees within a few ms.

### Frame identity through a generator (FSR FG, 2026-10-06)

- Conservation infers which application frame a generator output carries; FSR 3.1 *names* it. The game calls
  `ffxConfigure` (FG) with `frameID` on the thread that then calls the proxy Present (Talos: both on the RHI thread),
  and AMD's present callback reports `frameID` for every output, generated frames carrying the ID of the real frame
  they interpolate toward. `Hooked_ffxConfigure` stores it thread-locally (`NoteGeneratorFrameConfigured`),
  `PerformanceMetrics::ObserveApplicationPresent` consumes it onto the application frame, the callback bridge stages it
  in `present_association` (`GeneratorFrameToken` = frameID + 1), and `ConsumeDisplayTiming` passes the token of the
  runtime Present behind each displayed transition to `ObserveDisplay`. `MatchGeneratorFrameLocked` then finds the
  application frame by identity; only when no token matches does `MatchApplicationPresentLocked` count.
- Why: conservation assumes every frame is displayed exactly `fgMultiplier` times. A skipped interpolation leaves the
  count one short each time and the step-back grows (`CountingTowardTheFrameDriftsWhereIdentityDoesNot`).
- A configure on another thread than the Present is deliberately not paired (thread-local slot): their order would be
  a guess. Such a game shows `idUnmatched=` growing and stays on conservation.
- Talos `20261006_194753`, second FSR period (`pacing_trace` 19:51:59-19:52:19): 1309 frame IDs, each exactly one
  real and one generated callback, 2613 displays - AMD's side conserved. The game spent 6.0 ms of each 13.7 ms frame
  blocked inside the proxy Present (`[OVERLAY COST] FFX proxy Present ... runtimeAvgUs=6029`), output pinned at the
  145 Hz panel: a full queue, so the 3-4 frame `appQueue` there may be real. `idQueue=` settles it on the next run.
- Diagnostics: `idQueue=` (median newer application frames already presented when an output of an older one went
  out), `idMatched=`, `idUnmatched=` in `[Overlay] PC latency chain`. Tests:
  `tests/test_system_latency_generator_identity.cpp`.

### Frame-begin anchor (`system_latency_frame_begin.h`, `system_latency_fallback.h`)

- The input-to-Present span is measured, not modelled, whenever a boundary that belongs to the presented frame is
  observable. Precedence per application frame (2026-10-06, `ResolveMeasuredAnchorLocked`):
  1. the game's own Streamline PCL SimulationStart, paired with its PresentStart by frame ID (`SimulationMarker`, fed
     from `Hooked_slPCLSetMarker` via `NoteMarkerFrameBegin`, also under FSR FG where the PCL report is discarded);
  2. a low-latency sleep return (`LowLatencySleepReturn`: `ReflexLimiter::EndGameSleepBoundary` for
     `slReflexSleep`/`NvAPI_D3D_Sleep`, plus `vkLatencySleepNV`) **only when it returned on the presenting thread**;
  3. the presenting thread's last Win32k input-message retrieval since its previous application Present
     (`InputRetrieval`, see `display-change-timing.md`);
  4. the median of recently (2 s) measured spans of neighbouring frames (`Learned`);
  5. otherwise one application interval (`Modelled`).
- **Why the thread check:** Unreal sleeps on the game thread and presents on the RHI thread, so the newest sleep before
  frame N's Present belongs to frame N+1 or N+2. Pairing it read whole frames too low - below even the modelled
  interval, which is itself only a floor for such a pipeline. Counted as `sleepOtherThread=` in
  `[Overlay] PC latency anchors`. A marker whose PresentStart is not newer than the previous application Present
  belongs to an earlier frame and is rejected (`markerStale=`). Tests: `tests/test_system_latency_anchors.cpp`.
- **No double count without FG (2026-10-06):** the gap between the hook's Present entry and the runtime PresentStart is
  CE's own in-call wait (limiter, flip-queue pacing). It is inside the modelled Present-to-Present interval, so it is
  added only to a learned/measured span or a generator hold, never to the model. Before, Strange Brigade at a 90 fps
  cap published 11.1 ms interval + 9.3 ms wait for a frame the game built in 1.8 ms (memory/log evidence
  `20260913_124032`).
- Present wrappers do not record a frame-begin boundary: Present is entered multiple times per displayed frame in
  several configurations (e.g. 2.3x in Talos), and under frame generation the Present that returns belongs to the
  generator's pacing thread rather than the application's frame.
- **Frame generation pacing hold.** Under frame generation, the generator holds an application frame behind the
  interpolated frames derived from it. Matching against the newest boundary at or before final-output Present would
  alias onto the next simulation frame that started while the generator was still holding the previous frame,
  falsely reporting lower latency. When generation is measured, the tracker correlates against the application-source
  Present and steps the simulation anchor back by the generator hold span.
- Mode and multiplier transitions trigger a measurement epoch reset (`ResetMeasurementsLocked`), preventing history
  from bleeding across FG 2x/3x/4x transitions or causing doubled/corrupted readings.
- VSync and backbuffer queueing need no separate term: a blocking `Present` is entered before the block and reaches the
  screen after it, so the wait is inside `present-to-display`, and a block that instead delays the wrapper's return
  moves the next frame's boundary. Unverified on hardware as of 2026-10-06 (the user reports no visible vsync cost), so
  `queuedAhead=`/`queuedAheadMax=` in `[Overlay] PC latency anchors` count the displays that reached the screen
  between a frame's runtime PresentStart and its own display: a vsync-capped rate must read about the flip-queue depth
  minus one, free-running VRR about zero. Test: `FramesQueuedAheadExposeAFullFlipQueue`.

### Failure modes and bounds

- NVIDIA's average-input-wait heuristic is unsupported below 10 FPS, so both paths fail closed there. Present-to-display samples over 250 ms, totals over 500 ms, incompatible timestamp domains, clock resets, and samples stale for more than two seconds also become unavailable instead of producing a plausible-looking number.
- Presentation cadence gaps > 250 ms (loading screens, pause menus, scene hitches) skip rolling cadence interval updates (`applicationPresentIntervals_`) to avoid cadence skew, but keep `applicationPresents_` and `presents_` updated so subsequent frames remain fresh.
- Causal application frame matching (`MatchApplicationPresentLocked`) rejects matches older than 250 ms across gaps, safely falling back to `baseIntervalUs` instead of inflating anchor spans. Generator hold is bounded to 250 ms, and detailed rejection causes (`p2d`, `base`, `total`) are tracked in `Diagnostics` and logged in `[Overlay] PC latency chain`.
- The 32-sample window publishes a symmetrically trimmed mean, discarding an eighth from each tail once at least 16
  samples are held. A single 250 ms telemetry poll can contribute an entire window, so one frame paired against the
  wrong Present - a full frame interval out - must not be able to move the published number.
- Both values add half the display's scanout (2026-10-06, `Tracker::ScanoutToCenterUs`): the screen-time event marks
  where scanout starts (VRR/vsync) or where a tearing flip lands in it, and a pixel at a uniformly random height is
  reached half a scanout period later on average. The period is the display mode's refresh (VRR scans out at that
  rate and only stretches the blank), published by the sensor in `SharedDisplayTiming::refreshPeriodUs` (ABI 70);
  `scanout=` in the chain line. USB/peripheral latency and pixel response are still excluded - neither is
  measurable from the PC. Native queries run at most four times per second, and fixed-capacity rings plus a Present-side try-lock keep telemetry work off the rendering critical path. Streamline logs one `PCL marker latency report available` transition; failed marker forwards are rate-limited.
- The native-report poll is gated on having either a graphics device **or** a registered supplemental provider. The
  Streamline PCL provider serves reports without a device, so gating on the device alone silently discarded the game's
  own markers whenever it was unresolved.

### Diagnostics

- `[Overlay] PC latency sample` is emitted on every source change and otherwise every five seconds, with the trimmed
  mean plus the window's median/min/max. A wide min/max spread is how a broken correlation announces itself.
- `[Overlay] PC latency chain` decomposes the most recent accepted sample: `frameBegin=` (`low-latency-sleep` or
  `modelled`), `anchorToPresent`, `presentToDisplay`, `inputWait`, `baseInterval`, `applicationInterval`,
  `frameBeginInterval`, `displayInterval`, `outputRatio`, `generationObserved`, `generatorHold`
  (`measured` against the application's own Present, `modelled` as one output interval, or `none`), `markerInterval`,
  `markerTrusted`, `markerAssociated`, running totals for displays observed, associated, unmatched, dropped, rejected,
  `markerCadenceRejects`, `epochResets`, and source changes.
- `[Overlay] PC latency sample` is logged on a source change, every 15 s, and (2026-10-06) whenever the published
  value moves into another 25 % band (`LatencyLogBand`, at most every 2 s; `trigger=source|value|period`,
  `bandMovesSkipped=`). Talos `20261006_203030` showed why: the user reported "Reflex ~60 instead of ~30 ms" and the
  15 s samples never caught the moment.
- After FSR FG is switched off, Talos keeps AMD's proxy swapchain (callback `generated=0 frameId=0`,
  `mirroredCurrent=1`). Identity matching is skipped without frame generation (`fgMultiplier < 2`), so those
  outputs no longer count as `idUnmatched` (it had grown to 10938). Talos `20261006_203030` also showed that the
  proxy left in place adds latency at a refresh-pinned menu with Reflex off: game blocked 5.3 ms per frame in the
  proxy Present (`[OVERLAY COST] FFX proxy Present ... runtimeAvgUs=5290`), present-to-display 32 ms against 18 ms
  before FSR in the same menu, estimate 46 ms against 29-31 ms. Talos switches Reflex off when FSR FG is enabled and
  does not restore it. At 75 fps GPU-bound gameplay the readings matched before and after the switch (estimate
  40.5/41.7 ms; Reflex on 46-48 before, 34 after).
- The chain line's `anchorToApp=` (median of the measured frame boundary to the application Present; measured
  anchors only) and `appToRuntime=` (application Present to the runtime Present carrying the frame) split a reading
  at the game's Present. The time before it belongs to the game's own pipeline, which a low-latency mode shortens. The
  time after it belongs to a generator, an interposer such as FSR's proxy, or CE's pacer (2026-10-06).
- `[Overlay] PC latency sample` also carries `fps=` and `gpu=`: a Reflex-on reading at a few percent GPU load is a
  menu or empty scene, not comparable with gameplay. Talos `20261006_194753`: every ~6 ms Reflex-on reading came with
  the overlay showing GPU 4-10 % at 31-50 W, every Reflex-off reading (35-37 ms) with gameplay load.
- `[Overlay] PC latency anchors` adds `markerOnPresentingThread=`/`markerOnOtherThread=` and `markerToPresent=`
  (median span from the PresentStart marker to the application Present it was paired with). The Reflex contract
  brackets the Present call, which only the presenting thread can do; a marker from elsewhere (an engine setting it
  where the render thread queues the present) would let "newest pair before Present" name a later frame and read low.
  Open as of 2026-10-06: Talos's Reflex-on 5.6-6.7 ms at 138 fps decomposes into 1.4-2.6 ms simulation-to-present and
  0.4 ms present-to-display, plausible only for a light scene; these counters decide whether its markers bracket the
  Present.
- `[Overlay] PC latency cross-check` prints the source that was **not** published whenever it also holds a fresh
  window. Both estimate the same quantity, so a large disagreement means one of the two correlations is wrong.
- Open question / stale-risk: the two sources are not interchangeable across a configuration change if the marker path
  is running on the degraded no-association branch. Games generally emit PCL markers only while their low-latency mode
  is enabled, so an A/B of low-latency on versus off is an A/B of two estimators; check `markerAssociated=` and the
  cross-check line before comparing the numbers.

## Legacy backend hot paths

- `RendererBackend::OnDrawDataChanged()` marks newly built geometry. Cached frames still submit a draw every Present but do not notify legacy backends or re-upload unchanged geometry.
- DX8/DX9/DX10 upload VB/IB data only after a rebuild, buffer recreation, or failed prior upload. A failed lock/map remains dirty and returns before draw submission, so stale geometry is never drawn.
- DX8 and DX9 lazily retain one full state-block object for the backend lifetime while still capturing and applying it around every overlay draw. Capture/apply failure discards the object for safe recreation; reset/shutdown releases it. Existing half-pixel placement, render-target safeguards, fixed-function state, and BeginScene/EndScene handling remain intact.
- DX10 remaps its constant buffer only when viewport size, HDR mode, or paper-white changes. Its complete pipeline save/restore remains intact.
- A valid OpenGL 2.1 fixed-function matrix path prefers client-side vertex/color/UV arrays and one `glDrawElements` per shared command. **Both** GL paths (legacy and the GL 3+ modern path real drivers select) own application state through `hook/opengl/gl_overlay_state_policy.h` (2026-09-24): capture before, restore after - enables (blend/depth/cull/scissor/stencil, fixed-function alpha test/lighting/fog/TEXTURE_2D where the profile has them), blend func/equation, color mask, polygon mode, active texture + unit-0 texture and sampler, program, VAO, array/element buffers, viewport; the legacy path additionally restores VAO 0's client-array pointer specification (glGetPointerv) and the font upload resets/restores pixel-unpack state. Before that fix the modern path restored almost nothing (depth test and culling stayed off). Fake-GL round-trip tests: `tests/test_gl_overlay_state_policy.cpp`. Capability decisions are per backend/context; a one-time error probe retains immediate mode for incompatible injected contexts. Per-Present error draining and success heartbeat logs were removed.
- **Direct3D 7 uses the application's active scene and never creates a synthetic one.** The adapter permanently owns the headless CPU renderer; a separate `CustomOverlay::D3D7Backend` sidecar converts its draw list to `D3DTLVERTEX` and issues `DrawIndexedPrimitive` immediately before the application's real `EndScene`. The old synthetic `BeginScene`/`EndScene` pair at Flip was the operation adjacent to the two identical Steam `gameoverlayrenderer.dll` failures. The sidecar is primed outside the active scene, reuses a managed ARGB8888 font surface, and draws only into a render target already observed in a real Flip/Blt presentation, so an offscreen texture pass cannot be stamped accidentally. `[Graphics] legacy_d3d_native_overlay` defaults on; overlay-excluded recording deliberately uses the CPU route so native pixels cannot leak into the recording.
- **Native device state is all-or-nothing.** A retained `D3DSBT_ALL` block captures and restores every application state around a draw. Capture, draw, or restore failure makes the sidecar unusable and enters a bounded exponential retry (one through 512 frames) rather than drawing stale geometry or leaving CE's blend/sampler state in the game. `d3d.h`/`d3dtypes.h` remain confined to one translation unit, and all hook-side D3D7 calls use ABI-pinned vtable slots under `LegacyD3DInternalScope`.
- **A Direct3D 7 state block does not own the textures it restores, so CE does.** The block records each stage's texture as a raw `IDirectDrawSurface7*` with no reference. Nothing in a Direct3D 7 process ever re-binds an old texture on its own, so an application releasing a still-bound surface is legal: the device keeps its internal texture object alive and the surface interface is simply never touched again. `ApplyStateBlock` breaks that - it replays the binding through `IDirect3DDevice7::SetTexture`, which reads the interface's freed `lpLcl` and faults. Gothic II session `20260916_000027` is exactly that: `DIRECT3DDEVICEI::SetTextureInternal+0x12`, `mov eax,[eax]` with EAX zero, reached through the runtime's internal state-replay dispatch (the saved EBX proves it was not the public `SetTexture` wrapper, whose EBX is the 0/1 multithread flag), seconds after the intro videos released the textures they had left bound. CE therefore hooks `IDirect3DDevice7::SetTexture` (ABI slot 35) and holds a reference to every binding for as long as the device does - bounded at one surface per stage, dropped the moment the application binds something else - which is the lifetime guarantee the state block assumes and DirectDraw does not provide. The sidecar refuses to prime unless both halves of that ownership hold: the device was seen at `IDirect3D7::CreateDevice` **and** the `SetTexture` interception is live on its vtable - seeing the creation alone would leave an empty shadow looking trustworthy if another overlay owned the slot. A failed install says so in the log, and the CPU composite keeps the overlay there. Gothic II session `20260916_004304` validated the fix: sidecar primed, `route=native-d3d7`, `nativeFail=0` over 5,994 flips at 144.0 fps with 11 us frame-time standard deviation, no crash and no dump, and an included screenshot completed mid-run. Stage 0 is also restored explicitly from that shadow after the block, so the sidecar does not depend on the block carrying textures at all. `hook/ddraw/legacy_d3d_texture_bindings.h`, `hook/ddraw/ddraw_hook_texture_bindings.cpp`, `tests/test_legacy_d3d_texture_bindings.cpp`.
- **Every non-native DirectDraw presentation uses a CPU raster, never D3D9Ex.** `overlay_cpu_raster.cpp` consumes the existing CPU draw list and font atlas, emits premultiplied BGRA, and blends only the overlay rectangle while the DirectDraw surface is locked. There is no helper GPU, upload, readback, `GetRenderTargetData`, or GPU wait. The cache compares exact quad/triangle snapshots: unchanged primitives are reused, and a moving graph repaints only the union of old/new primitive bounds even when the renderer merged it into the stable panel's command. Axis-aligned rectangles and glyphs use the pixel-centre fill path.
- **CPU composite state is proof scoped to each canonical surface.** Up to eight LRU entries retain the application backdrop and the exact bytes CE last wrote; RGB565/RGB555 comparisons retain their post-quantization value. Exact Lock/Blt/BltFast rectangles feed a bounded write tracker, while unknown/GDI writes conservatively mark the whole surface. A matching last composite proves an untouched pixel and makes repeat composites idempotent; a changed byte becomes the new application backdrop. Growth preserves overlap, shrinkage restores the vacated strip, an empty draw list restores the last rectangle, Flip swaps state with surface memory, and a copied blit copies it. New primary chains clear all byte-derived state so recycled COM addresses cannot inherit stale proof.
- **The locked surface is video memory, so the composite streams it once each way.** The read-modify-write is unavoidable - the backdrop proof needs the application's pixels - but touching them one at a time through the `Lock` pointer is not: every read is an uncached round trip and interleaved stores to the same addresses defeat write combining. Gothic II session `20260916_014133` measured `writeAvgUs=3210` with a 44 ms peak and `lockMaxUs=27109` on the application's render thread, inside its present, on the loading-screen route. Each row is now copied out with one linear `memcpy`, composed in ordinary cached memory by `ComposeCompositeSpan` (16-bit rows expand and repack around the same call), and copied back with another. The arithmetic is unchanged and `TheStagedSpanMatchesThePerPixelCompositeItReplaced` pins that against the loop it replaced. Unmeasured on hardware since the change.
- **Native-to-2D transitions retain exact damage, not a route timer.** Per-surface native state collects the rectangles that a later Lock/Blt/BltFast overwrote. Bounded damage is rerasterized only inside those rectangles without double-blending the still-current native pixels; a full covering write proves the old native overlay absent. Unknown or overflowed damage defers the composite rather than corrupting it. Overlay-excluded capture likewise defers one frame while native pixels may remain, allowing CFR to repeat the preceding clean image.
- **The overlay goes into the image the presentation publishes, never the image already being scanned out after the fact.** Flip composites its target before the call; an exact pass-through full-surface blit on a single-buffered primary composites its source; transforms such as color key, alpha, ROP, fill, rotation, or scaling composite the destination after the operation. Back-buffer writes wait for Flip. Front-buffer writes are already visible, but the first write after a Flip is treated as incidental and only a second consecutive write establishes the no-Flip loading-screen route. Partial updates outside the overlay avoid a lock. The per-ten-second presentation mix reports accepted/rejected paths, native draws/repairs, raster/lock/write timings, and full/partial/clean work.
- **The bootstrap keeps nothing alive: it wanted the vtables, and those belong to `ddraw.dll`.** It creates an `IDirectDraw7`, a primary surface and a D3D7 device on a throwaway window, patches the vtables those objects expose, and releases all of it - the patches outlive the instances. The primary surface used not to be released at all, which also kept the `IDirectDraw7` referenced, on a window the bootstrap destroys on its way out. Whether a second live `DDSCAPS_PRIMARYSURFACE` is what made Gothic II's post-alt-tab `CreateSurface` return `DDERR_UNSUPPORTEDMODE` four times in session `20260916_014133` is **not established** - the cheap experiment is one alt-tab with CE not injected - but the reference had no purpose either way. Both prototype sentinels are cleared with it: they are pointer comparisons against the application's surfaces, and DirectDraw may hand a freed surface's address back out, at which point CE would exclude one of the application's own surfaces from presentation tracking. `tests/test_ddraw_bootstrap_footprint.cpp`.
- **CE never keeps an application's presentation chain alive past the application's own references, and never outlives its Direct3D 7 device.** DirectDraw allows one primary per DirectDraw object, and a primary is gone only with its last reference. CE takes references into the chain: the CPU-prerender queue holds the surface it last presented (for a Flip, the primary itself), `directDrawOwner` holds the DirectDraw object, and the tracked D3D7 device (`TrackLegacyD3D7Device`) plus the native sidecar (`D3D7Backend`) hold the device. The chain reset used to run only after a *successful* primary creation, which those references made impossible: Gothic II `20260924_233030` re-created its primary after a focus loss four times, got `DDERR_PRIMARYSURFACEALREADYEXISTS` and quit through its own `Error-Message` box. `ReleaseDirectDrawChainBeforePrimaryCreation` now runs before every application primary creation (legacy, DirectDraw4 and DirectDraw7 detours, gated by `ce::ddraw_chain_lifetime::ShouldReleaseChainBeforeCreation`): sidecar first, then the queued surfaces, then the raw primary identities. **It must not release the device.** The first version did, and Gothic II `20260924_235830` faulted in `~CDirect3DDevice7 -> ~CDirect3DDeviceIDP2 -> D3DFree` (null read) reached from `ReleaseTrackedLegacyD3D7Device`: the game had already released the device and its surfaces, so CE's reference was the last one and the device was destroyed after its render target. A Direct3D 7 device must die before the surfaces it renders to. CE's device references therefore end inside the application's own last `IDirect3DDevice7::Release` (`ddraw_hook_device_lifetime.cpp`, vtable slot 2): the detour counts CE's references (pointer comparisons) before forwarding, and when the returned count equals them (`ApplicationReleasedLastDeviceReference`) it releases the sidecar (state block deleted on the live device), then the tracker, then the device's texture-binding shadow, and returns what the application would have seen. CE's own releases run under `LegacyD3DInternalScope` and pass through; the tracker releases a replaced device outside the identity lock and *through* the interception. Without a live Release interception CE neither tracks nor primes a device (CPU composite). The D3D7 `EndScene` detour re-tracks the device every frame. Diagnostics: `Application released its last reference to D3D7 device=... CE dropped its N of M reference(s)`, `Released CE's references into the previous presentation chain ... (presentationRefs= nativeSidecar=)` / `CE held no references into a previous chain`, `Application <api> primary creation FAILED hr= (<DDERR name>)`, `D3D7 device Release hook FAILED`, `Not tracking D3D7 device`. Gothic II itself also crashes on alt-tab without CE (user-verified 2026-09-24), so an alt-tab crash in Gothic is not by itself CE's; check the stack for `capture_hook`. Stale-risk: the post-alt-tab `DDERR_UNSUPPORTEDMODE` of `20260916_014133` is likely the same class but unverified; transient `AcquireLegacyD3D7Device` references on a thread other than the application's releasing one are not counted (none observed). `hook/ddraw/ddraw_chain_lifetime_policy.h`, `hook/ddraw/ddraw_hook_device_lifetime.cpp`, `hook/ddraw/ddraw_hook_overlay_route.cpp`, `tests/test_ddraw_chain_lifetime.cpp`. Last verified 2026-09-25 (unit + real-DirectDraw contract; in-game run with the Release interception pending).
- **DirectDraw export interception is independent of synthetic bootstrap.** A loaded D3D8/D3D9 module may postpone CE's probe object because third-party overlays can see that synthetic device, but it does not suppress the real `DirectDrawCreate`/`DirectDrawCreateEx` hooks. Actual DirectDraw evidence activates the hook even in mixed-module processes such as Gothic II.
- **A nested presentation is answered with DirectDraw's own implementation, reached past the foreign entry patch - never with CE's saved original, and never by dropping it.** CE hooks the surface vtable's Flip/Blt/BltFast slots and keeps the pointer it replaced. Gothic II session `20260916_021049` named what re-enters that detour, in the line the previous fix added: `Re-entered from gameoverlayrenderer.dll+0x76ACC; CE's saved original is DDRAW.dll+0x37C50; the entry point recorded before CE patched the slot was DDRAW.dll+0x37C50`. CE's saved original is genuine DirectDraw, so the two overlays are **not** holding each other's vtable pointer - the earlier reading of session `20260916_011148` was wrong. The other injector sits *below* CE: CE calls `DDRAW+0x37C50`, that injector owns the function's entry, and it re-issues the presentation through the surface vtable, which is CE's detour. That is also exactly why session `20260916_013230` recursed 32,768 levels in two milliseconds when CE answered the cycle by calling the entry point it had recorded before patching the slot: the same address, the same patch. **Returning `DD_OK` is not a safe default either.** The nested Flip in `20260916_021049` was on the *primary surface* - the game's real screen flip - so dropping one per frame left Gothic II showing its menu while the 3D scene ran with its audio. The answer is a bypass trampoline over the patched entry, built from the module's own on-disk bytes exactly as `DXGIShared::EnsurePresentBypassTrampoline` does for a patched `dxgi!Present`; the other overlay's handler has already drawn by then, so nothing is cut out of the chain. It runs at most once per outermost presentation, a re-entry from inside it is refused, and an entry with no `E9`/`FF 25` patch keeps the refusal with the reason logged - so the answer is bounded by construction and gated on an observed patch rather than on a theory of who wrote it. `reentryBypassed=`/`reentryDropped=` in the presentation mix count both outcomes; a growing `reentryDropped` is a frame that never reached the screen. `hook/ddraw/ddraw_hook_present_reentry.cpp`, `hook/ddraw/ddraw_hook_blit_classification.h`.
- **Stale-risk: only DXGI and DirectDraw answer a patched saved original.** The D3D8/D3D9 present paths call their saved originals directly. No session has shown a cycle there, and nothing was changed on a guess, but the shape - a co-resident injector patching the runtime's own entry below CE's vtable hook - is not specific to DirectDraw.
- **`DDLOCK_NOSYSLOCK` is never dropped, on any DirectDraw lock CE performs.** Without it a `DDLOCK_WAIT` lock takes the Win16 lock, and all of these run on the application's render thread inside its own present, in a process that also hosts a third-party overlay and a message pump. The composite and capture paths used to fall back to progressively more compatible flag sets ending without it; Gothic II session `20260916_005504` is the shape that costs - the composite lock began returning E_FAIL and the render thread never returned from the presentation a second later. Only the read-only hint is negotiable now. A lock CE cannot take is a frame CE does not composite and does not capture, which both callers already handled. `tests/test_ddraw_lock_flags.cpp`.
- **A presentation the runtime rejects must still be visible and must still count as a render loop.** `NotePresentationComplete` runs on success, so a title whose every `Flip` fails reported nothing at all: `20260916_005504` issued ~3,600 presentations over fifteen seconds with `renderLoopObserved=0` throughout and no mix line, which then made the freeze watchdog dump a false-positive dialog while the game ran and refuse to assert the freeze that followed. `NoteDirectDrawPresentationAttempt` runs after every classified presentation call returns: it arms the watchdog heartbeat regardless of the result, and reports a rejected presentation with its operation and HRESULT once per failure run, with a recovery line.
- **Capture first requests a read-only `DDLOCK_NOSYSLOCK` mapping and converts standard BGRA/RGB24/RGB565/RGB555 directly into the persistent D3D11 upload texture.** Drivers that reject the read-only hint receive a second attempt without it but never without `DDLOCK_NOSYSLOCK`; unusual masks use one persistent GDI DIB rather than allocating a full-frame bitmap per capture. Shared texture/fence exports are validated before publication, and a missing shared fence falls back to implicit synchronization.

## Vulkan compute-composite route (compute-only present queues)

- A reserved graphics queue is a layer-created dispatchable object: CE obtains it by calling the next
  `vkGetDeviceQueue` directly, below the loader trampoline that normally stamps an object's loader dispatch pointer.
  CE must preserve the `VK_LOADER_DATA_CALLBACK` from the device-create chain before advancing its link, invoke its
  `pfnSetDeviceLoaderData` on the queue, and validate that the queue now carries the parent device's dispatch key
  before registering or submitting it. Portal RTX session `20260901_071629` proved the failure signature: both x64
  dumps stopped in `SteamOverlayVulkanLayer64!vkQueueSubmit` during CE's initial font upload; the private queue still
  began with `ICD_LOADER_MAGIC` (`0x01CDC0DE`), so Steam found no device dispatch and jumped through a null slot.
  Older loaders without the callback use the loader-documented first-pointer copy from the parent device. A rejected
  callback, missing parent key, or post-callback mismatch disables only the reserved queue and leaves the existing
  synchronized borrowed-game-queue route available. The ready/warning log names the initialization outcome.
- When the game presents from a queue family without `VK_QUEUE_GRAPHICS_BIT`, the layer renders the overlay into a
  per-submission-slot offscreen image on a graphics queue and alpha-composites only its occupied rectangle onto the
  swapchain image from the present queue itself (`layer_overlay_compute.cpp`). The direct render-pass route would
  force a compute -> graphics -> compute round trip through the present dependency chain.
- **Every submission-ring slot must own a complete composite route.** Slot/target-image pair resources are indexed
  slot-major by `ComputeCompositeResourceIndex`, so resources sized for the ring's *initial* depth leave every
  appended slot out of range. Before 2026-08-31 those slots failed the compute route's own bounds check and silently
  fell through to the direct render-pass route, so the two routes alternated from present to present with the ring's
  period. Portal RTX session `20260831_054801` measured it: 6 images, 10 initial slots, 60 cached composites, the ring
  extended to 12 under 4x DLSS multi-frame generation, `Compute-present CPU summary` counting 2048 composites per
  15.9-17.1 s window against 143 Hz presents in `perf_metrics_28608.csv` - 83-90% of presents on one route.
- `GrowSubmissionRing` is therefore all-or-nothing: it appends the slot, then `AppendComputePresentSlot`, and pops the
  slot again if that fails. A declined growth falls back on the ring's existing bounded backpressure, which is the
  documented safe behaviour; it never produces a slot with half a route.
- Descriptor sets use one pool per slot, so extending the ring adds a pool instead of reallocating every existing set.
  The timestamp query pool still covers only the slots that existed at initialization, so both routes gate timestamp
  writes on `timestampSlotCapacity` rather than on the per-slot bookkeeping vectors, which do grow.
- **A composite is a blend, so it is not idempotent.** Compositing twice into one swapchain image blends the panel's
  own alpha onto itself and shows a more opaque overlay on that present - on screen, the overlay's translucency
  flickering rather than the overlay blinking. `ShouldSkipRepeatPresentComposite` suppresses the second composite when
  the image's acquire generation has not moved since CE last composited into it: a generated-output runtime may
  present one image several times without the application re-acquiring it, and an application may not alter a
  presented image before it re-acquires it, so an unchanged generation proves both the content and CE's overlay are
  still there. Generation `0` means CE observed no acquire at all and the guard fails open.
- That guard and the ring's slot-reuse proof both depend on the acquire generation being exact, so **both**
  `vkAcquireNextImageKHR` and `vkAcquireNextImage2KHR` are hooked and maintain it. An unhooked acquire would strand the
  ring at its safety bound (no slot can ever be proven reusable) and blind the repeat-present guard.
- Known cost, not yet addressed: each slot's offscreen target is a full-resolution image. At 4K/`R8G8B8A8` that is
  about 33 MB per slot, so the measured 12-slot ring holds roughly 400 MB. The per-growth log reports the running
  total so a pathological ring is visible in a session.
- A `VK_ERROR_DEVICE_LOST` from an overlay fence probe, wait/reset, or queue submit latches the per-device overlay
  state unavailable. Later presents perform no CE overlay GPU work, and cleanup destroys CE resources without an
  additional `vkDeviceWaitIdle` call. Session `20260901_174634` returned device-lost from three slot probes during an
  early Portal RTX close; the new latch reduces that to one decisive transition. The supplied full dump places the
  application's blocking thread inside DXVK Remix rather than CE, so this is defensive shutdown containment, not a
  claim that CE caused or fully fixes that external hang.

## Performance and diagnostics

### FSR callback performance and lifetime audit (2026-09-05)

Update 2026-09-07: `dx12_hook_ffx_callback_bridge.cpp` owns the registry. A per-thread
`callback_snapshot_cache.h` snapshot avoids its mutex while the callback generation is unchanged.
Identical configure writes do not advance the generation; replacement, removal and shutdown do.
The cache does not pin application callback lifetime: existing runtime callback drain/teardown
ordering remains required. Shutdown's registry size diagnostic now holds the registry mutex.
`tests/test_callback_snapshot_cache.cpp` covers stable reads, replacement/removal, distinct contexts
and independent caches, including a writer between the generation read and locked snapshot.

The callback GPU breadcrumb tail is conditional on CE draw or self-composition. With an original
app callback and the overlay hidden, CE adds no callback GPU commands. In 0.1.6500 only presentation
metrics continued in the hook: display consumption still depended on RenderOverlay. The bad-session
visibility comparison exposed that gap (`disp n=0`); host sensor collection preserved the evidence.
The callback metrics helper now consumes the shared display ring independently of rendering, before
optional presentation sampling. Its serialized cursor makes a visible renderer's second drain safe.
`[FSRCallbackWork]` marks that boundary for an on/off/on comparison in the same bad process.
Do not substitute a shared UI surface solely because an original callback exists: custom callbacks
need not consume it, and AMD's empty-UI copy path becomes a full-screen blend when UI is supplied.
The proposed substitution was rejected before product build. No pacing cure is claimed.

Sources: `hook/d3d12/dx12_hook_ffx_overlay_adapter.cpp`, `hook/overlay/custom_overlay_dx12_render.cpp`,
`custom_overlay_dx12_inline_upload.cpp`, `custom_overlay_dx12_retirement.cpp`,
`dx12_overlay_policy/inline_upload_slots.h`, `hook_cost_window.h`, and `hook/runtime/main_hookthread.cpp`.

- The official FFX callback remains preferred: draw into the supplied command list after the application's
  composition. No added presentation-queue ECL or queue Signal is needed. This avoids extra submissions;
  it does not make the CPU geometry work or GPU overlay draw free.
- A warm callback backend is device/format scoped and records into that supplied list. It does not need
  the global command-queue mutex or a fresh queue lookup. Cold initialization retains its queue while
  preparing the backend, and a changed device also invalidates the old device's RTV heap.
- The callback used to rotate its mapped vertex/index buffers modulo 16 without a completion proof.
  It now writes an inline `MARKER_OUT` after the draw and reuses a slot only after that slot's marker
  completes. Pending slots are never overwritten. A delayed GPU can grow the upload pool lazily to 128
  slots, without a CPU wait; exhaustion is explicitly diagnosed and refuses unsafe reuse. Ordinary
  externally fenced/forced allocator slots keep their existing ownership contract.
- Resource growth commits only after both allocation and mapping succeed, preserving the old mapping on
  failure. Initial VB/IB mapping failures also fail initialization rather than drawing uninitialized data.
- Replacing an adapter retains a backend with pending inline uploads. The existing hook service thread
  reclaims it after marker completion or device removal, including shaders/font/upload resources and a
  device lifetime pin. Neither the presenter nor callback waits for retirement; process-exit cleanup
  leaves driver-owned objects to the OS as before.
- Queue discovery remains suppressed while a runtime-owned presenter survives FG suspension. The original
  session resumed ~864 registrations/s on suspension even though AMD still owned the live path. Discovery
  is restored when ownership returns, and still runs when the primary game queue is unknown.
- An idle benchmark avoids per-output system-metric snapshots and config-string copies. Toggle delivery
  and active benchmark updates remain immediate.
- `[OVERLAY COST]` now reports exact disjoint per-thread windows of 600 calls, with `ceAvgUs`, `ceMaxUs`,
  forwarded-runtime costs, and `ceOver500Us` / `ceOver1ms`. This replaces contended cumulative counters whose
  startup maximum hid subsequent smaller stalls. The logger thread ID distinguishes callback workers.
- `talosnew` validated the interim inline-marker route and suspension discovery guard without exhaustion
  or source fallback. It predates the later history-atomic and cost-window work. No controlled Talos A/B
  has yet attributed the remaining start-to-start variance; do not label that symptom fixed from unit tests.
- Regression suites: `DX12InlineUploadSlotsTest`, `DX12UploadSlotGuardTest`,
  `Dx12EclQueueRegistrationPolicyTest`, `HookCostWindowTest`, and `DisplayPacingIntegrityTest`.

The following measurements are historical (2026-09-03), before this audit and the 2026-09-04 callback-route
changes. Keep their intervention caveats; they are not a current proof of attribution or current overhead.

### What CE costs a frame-generation game, and the instruments that found it (2026-09-03)

Measured with `dx12_fg_switch_test` at 3840x2160, vsync off, 2x FSR FG, `gpu_load=1200`, and every `[Stress]`
switch off (`dxgi_video_memory_query_stress`, `fsr_suspend_resume`, `fsr_present_callback_toggle_stress`). Base
frame time 5.27 ms without CaptureEngine, 7.38 ms with it - **2.11 ms per base frame**, reproduced across a dozen
runs.

**The app is GPU-bound, not main-thread bound.** An earlier reading of this scene called it main-thread bound
because its render thread sits at 100% of a core; that thread is *spinning inside the frame-generation runtime's
`Present`*, which is not the same thing. Without CE the PDH `GPU Engine` 3D counter for the process reads ~100%.
With CE, board power falls from **152 W to 119 W at an unchanged 2.9 GHz SM clock** while the frame rate falls
29%. Same clock, less power, fewer frames: the GPU is being **starved**, not loaded. `nvidia-smi --query-gpu=clocks.sm,power.draw` sampled once a second is enough to see it,
and needs no elevation.

**Where the time goes, measured by the app itself.** `testapp/dx12_fg_frame_phases.h` brackets the app's own call
sites (its `Present`, `ffxConfigure`, `ffxDispatch`, `ExecuteCommandLists`) and reports microseconds per frame in
the heartbeat; `testapp/dx12_fg_gpu_timer.h` adds D3D12 timestamp queries around the app's own command list. Both
read identically with and without an injected overlay, which is what makes them decisive:

| per base frame | no CE | with CE | delta |
| --- | --- | --- | --- |
| frame | 5267 us | 7382 us | **+2116** |
| inside the FFX proxy `Present` | 4437 us | 6466 us | **+2029** |
| `ffxConfigure` (2x per frame) | 2 us | 40 us | +38 |
| `ExecuteCommandLists` | 31 us | 41 us | +10 |
| `ffxDispatch` | 50 us | 56 us | +5 |
| the app's own command list, on the GPU | 4179 us | 4387 us | +209 |

So 96% of the loss is the game thread blocking longer inside the runtime's `Present`, and the app's own GPU work is
essentially unchanged. CE's own share of that `Present` call is 0 us (`[OVERLAY COST] FFX proxy Present`).

**What it is not.** `hook/fg/fg_cost_probe.h` (`CE_FG_COST_PROBE`, off by default) removes one CE behaviour per
bit so a frame-rate A/B can attribute the cost. None of these recovered anything:

| probe | what it removes | result |
| --- | --- | --- |
| `0x1` | the FFX present-callback bridge tail: compose, overlay, breadcrumbs, metrics | 135.9 fps |
| `0x20` | the DXGI present hook entirely (`ProcessFrame` provably never ran: 1 CSV row, not 8680) | 137.4 |
| `0x21` | both overlay draw sites at once | 137.3 |
| `0x40` | installing CE's present-callback bridge at all | 136.7 |
| `0x6F` | all of the above together | 137.9 |
| `0x80` | the ECL caller-module lookup (`GetModuleFileName` per submission) | 135.4 |
| `0x100` | the Streamline-UI ECL observers (a global mutex twice per submission) | 135.5 |
| `0x200` | the PostSL/Streamline startup block in the ECL detour | 135.4 |
| `0x800` | the per-call ECL wall-clock diagnostic and watchdog heartbeat | 135.4 |
| `0x2000` | hooking the queue's vtable during registration | 135.5 |
| `0x4000` | resolving and publishing the queue's device (limiter, LUID, `g_Device`) | 135.8 |
| `0x10000` | the sampler/anisotropy detours on the game's real device vtable | 135.7 |
| config | `[Overlay] enabled=false` | 136.3 |
| config | `[HardwareSensors] enabled=off` | 135.6 |

against 189-192 fps with no CE and 135.5 fps with CE unmodified.

**What `0x8000` isolates, so far.** Exactly one intervention recovers the frames: **`0x8000`, CE skips the queue
adoption block** - 183.1 fps, `Present` back to 4536 us, 1.92 of the 2.11 ms returned. The adjacent device-publish
probe (`0x4000`) and vtable-hook probe (`0x2000`) each recover nothing. The three probes that looked like answers
earlier (`0x10` ECL passthrough, `0x400` ECL early forward, `0x1000` registration suppressed) all share one side
effect - CE never reaches queue adoption - and that, not what they were nominally removing, is why they were fast.
The `0x8000` block suppresses the owning queue reference, `g_CommandQueue` publication, and downstream
initialization enabled by that state, so it does **not** yet prove that one pointer read is the mechanism.

Do not conflate that deterministic uncapped test-app cost with the random Talos bad-start pacing state.
`talosbadintheend` has the same queue-role snapshot and zero active-FG ECL registrations in its three good starts
and last bad start, while only the physical cadence after PresentStart changes. `0x8000` also prevents the whole
adoption block, so a production queue rewrite still needs a role/lifetime proof.

2026-09-07 ownership correction: `dx12_hook_queue_adoption.cpp` distinguishes ECL discovery from
explicit wrapper bindings. A different queue's submission on the same device no longer replaces
the established global queue. Initial discovery, proven device migration and explicit bindings
still work; the separate exact swapchain/FSR/Streamline queues retain their rendering roles.
Incoming GetDevice failure leaves the old pair intact. New queue/device references are acquired
before publication and replaced references are released after the queue mutex unlocks. This removes
the pre-FSR last-submitter-wins state and COM/publication churn observed across two Talos threads.
`Dx12EclQueueRegistrationPolicyTest` covers interleaved auxiliary submissions, explicit rebinding,
unknown identity and device migration. The older probe's performance gain and the random pacing
failure are not yet proven to be caused by this defect; hardware validation remains required.

**A real defect found on the way, fixed.** Under FSR FG the ECL detour re-ran full command-queue registration on
**1290 of 1290 submissions per second**: the "known queue" fast path compares against four pointers CE knows, and a
frame-generation runtime submits from its own internal queues, which are none of them - and each registration
re-pointed `g_CommandQueue`, so the queue that submitted next looked unknown again. Registration takes the global
command-queue mutex, calls `GetDesc`/`GetDevice` on the queue, re-points `g_CommandQueue` with COM AddRef/Release
on a driver object, and hooks the queue vtable (two `GetModuleFileName` calls under the loader lock).
`ce::dx12_overlay_policy::ShouldRegisterCommandQueueFromExecuteCommandLists` now says: registration is discovery,
so it runs while no runtime owns presentation or the primary game queue is unknown - the game's
render queue is the first DIRECT queue seen and predates any FG runtime, so an unrecognised queue under active FG
belongs to the runtime. ECL coverage does not depend on it (the detour is on the queue vtable, shared by every
queue of the device). `DX12 DIAG: ECL timing/1s` now reports `registrations=`, which is 0 after the fix and was
equal to the submission count before it. This did **not** recover the 2 ms.

**Callback-owned FSR ECL fast-forward.** The five-start `20260906_160321` reproduction again has four smooth
starts followed by a bad one even with active-FG registrations at zero and the housekeeping thread at normal
priority. Stable bad PID 21880 has PresentStart stddev 755 us versus physical-completion stddev 2457 us; the
clean starts are 263-321 us versus 745-858 us. Although registration is gone, the ECL detour still traversed CE's
diagnostics/classification/observers about 1500 times/s on AMD/game submission threads. App-callback native FSR
does not use ECL as an overlay, discovery, or timing transport: AMD's callback supplies the exact output and
command list. `ShouldTransparentForwardNativeFSRCallbackEcl` therefore forwards before those CE side effects
while that exact route owns presentation. The fast forward keeps the foreign-hook recursion breaker. Internal
no-callback FSR, Streamline/PostSL overlap, CE-owned submissions, device removal, and FSR-off discovery stay on
the full path. This is a generic low-interference optimization, not yet hardware proof that ECL traversal caused
the random downstream state. The fast forward also skips the per-frame command-list count, so on this route
`count==0` on every Present and ECL-based real/generated classification is blind. Inject capture therefore
takes the callback verdict staged for the Present it precedes on the same thread
(`present_association::ConsumePresentFrameVerdict`, `dx12_overlay_policy::IsPresentedFrameForCapture`);
without it no recording ever went live under callback-owned FSR FG (Talos `20260926_081620`). A known verdict
makes EVERY runtime output capturable, generated included (2026-09-30, see `cfr-capture-sync.md`); only the
no-verdict path still requires a counted submission. Other `isInterpolatedFrame` consumers still see the zero count.

**Method notes worth keeping.**

- Wall time is the wrong instrument for a hook that *forwards* a blocking call, but wall-minus-forwarded is the
  right one for a hook that *blocks*: cycles cannot see a thread waiting on a lock or a fence. `HookCpuCost` now
  reports both (`avgWallUs`/`usPerMs` next to `cyclesPerMs`).
- A probe that makes a hook return early can change far more than the line it removes. Every "this recovered the
  frames" result has to be checked for what else stopped happening - here, three separate bits all silently
  stopped queue discovery.
- The PDH `GPU Engine` utilisation counter is pinned near 92% in every configuration here and discriminates
  nothing; board power at a fixed clock does.
- `Get-Counter '\GPU Engine(pid_<pid>*)\Utilization Percentage'` must be sampled one shot at a time: a single
  `Get-Counter` stream expands the instance wildcard once and never sees a process that started after it.
- The test app's `[Stress]` switches must be off before any of this is a performance measurement, and vsync off or
  both sides sit on the cap. Back-to-back fullscreen runs need a settle delay or `ffxCreateContext` fails
  `RUNTIME_ERROR` and `CreateSwapChainForHwnd` returns `E_ACCESSDENIED`, and the app never enters FSR mode.


- Performance probes must launch old-API apps through CaptureEngine without recording. Those APIs cannot use the native zero-copy recording route, so a recording run would benchmark capture/conversion as well as the overlay.
- Short uncapped 4K probes on the current NVIDIA system measured valid native DX9 at roughly 7/8/10 us median/p95/p99 overlay time and DX9Ex at 2/3/6 us. OpenGL reported 49/87/262 us while running thousands of FPS, but the driver returned a 4.6 compatibility context despite the test asking for 2.1, so this exercised the modern backend rather than the legacy array path.
- Historical DirectDraw7 probes used about 45.7% of one CPU core with the overlay disabled versus about 64.7-68.6% enabled because the retired implementation transferred a 4K surface through D3D9Ex and blocked on GPU readback. The current native D3D7 path performs only the ordinary draw calls; the CPU fallback locks the aligned overlay/dirty rectangle and has no GPU dependency. Fresh runtime numbers are still required.
- On this machine, the DX6 test failed `QueryInterface(IDirect3D3)` (`0x80004002`) and fell back to DirectDraw; DX7 failed `IDirect3D7::CreateDevice` (`0x88760082`) and fell back to DirectDraw; DX8 device creation failed (`0x8876086C`) and the app fell back to GDI. Their poor pacing therefore primarily characterizes the fallback test apps, with measurable CE DirectDraw composite cost on top. The old zero/unavailable GPU and VRAM readings were a separate adapter-identity/validity bug fixed by the host-telemetry changes above, not an old-API sensor limitation.
- Historical no-recording DirectDraw7 smoke runs covered installed x64 and x86 binaries at 4K/150% scaling and validated the displayed telemetry, but used the retired D3D9Ex helper. The current overlay-only route publishes its adapter through DXGI and needs a fresh in-game validation.
- LibreHardwareMonitor 0.9.6 no longer uses WinRing0; it embeds PawnIO bytecode modules and reads the CPU temperature, package power and clock rails through the Microsoft-signed `PawnIO.sys` kernel driver. Both the driver **and** elevation are required - measured on a machine that has PawnIO installed and still read every CPU rail as exactly zero without elevation. `captureengine/sensors/pawnio_setup.cpp` detects the driver by its service key (`SYSTEM\CurrentControlSet\Services\PawnIO`), and when CPU metrics are requested but the driver is absent it offers installation once at startup (install / not now / don't ask again), persisted per user in `HKCU\Software\CaptureEngine`. CaptureEngine bundles the official Microsoft-signed installer (`plugins/LibreHardwareMonitor/PawnIO_setup.exe`) for full offline setup without package manager or runtime network dependencies. Before running under administrator elevation, CaptureEngine verifies the installer's Authenticode certificate and pinned SHA-256 digest (`1f519a22e47187f70a1379a48ca604981c4fcf694f4e65b734aaa74a9fba3032`) to prevent privilege escalation via local file tampering. Installation (`-install -silent`) and uninstallation (`-uninstall -silent` or registered `QuietUninstallString`) can be triggered from the system tray context menu (`Open config`, `Install PawnIO` / `Uninstall PawnIO` with confirmation, `Close`), the startup prompt, or elevated CLI commands (`--install-pawnio`, `--uninstall-pawnio`). When triggered unelevated, CLI and tray actions prompt for standard UAC elevation once, and successful installation offers a 1-click elevated relaunch so CPU sensors become active immediately.
- Debug sensor summaries include the complete CPU/max-core/GPU/VRAM/temperature/power/fan/validity snapshot, adapter LUID, publisher PID/direct parent/eligibility, and ABI signature so future field-shift or source-validity failures are diagnosable without inferring values from the rendered overlay. Selected LibreHardwareMonitor identifiers log only when they change; plugin filesystem paths and exception messages are not logged.

## Validation and stale-risk

- `VulkanLoaderDataTest` covers callback initialization, the old-loader parent-dispatch fallback, callback rejection,
  false-success validation, invalid objects, and the production ordering that initializes before queue registration.
  Portal RTX still needs a fresh runtime startup after installing the fixed build; the supplied failure session itself
  can establish the pre-fix call chain and object state, not post-fix hardware behavior.
- Portal RTX session `20260826_020732` runtime-validated the generic split-renderer overlay/crash fix but exposed the telemetry provenance gap: `hl2.exe` remained the correct profile/source PID while `NvRemixBridge.exe` owned Vulkan on RTX 5070 LUID `0xC88E`. The bridge published that exact LUID, but the sensor accepted only same-PID publishers and found no `hl2.exe` GPU-engine instances, leaving validity `0x0`. Direct-child publisher eligibility now follows the same process-lineage boundary as the Vulkan layer without changing config/source ownership. Fresh runtime validation of the numeric GPU/VRAM rows remains required.
- Focused deterministic coverage pins draw-data notifications versus cache hits, failed-upload dirtiness, DX8/DX9 state-block reuse structure, DX10 constant invalidation, OpenGL array/fallback selection and state sentinels, glyph gutters, graph geometry, text-origin snapping, dynamic row sequences, memory-value policy, and the one-to-one DX12 allocator/upload-slot mapping plus exact PostSL signal guard.
- Live 4K validation covered native DX9 plus DirectDraw7 x64/x86 and showed valid RAM consumption rather than the unavailable marker, with the full overlay and graph rendered. Required build `0.1.4989` completed x64/x86 hooks and test apps, Vulkan layers, packaging/import closure, PE hardening, and PDB checks. All 14 focused host-telemetry tests pass. The no-build gate passed the remaining 1,644 native tests; the sole excluded cursor-bitmap test depends on the shared `IDC_ARROW`, which was temporarily transparent while the ChatGPT Windows-control session was active, consistent with cursor substitution and unrelated to overlay telemetry.
- Deterministic `DDrawPresentPolicyTest`, `OverlayCpuRasterTest`, and `LegacyD3D7VTableAbiTest` coverage pins presentation targets, flip-front filtering, mixed-module bootstrap, RGB16 stability, native damage transitions, primitive-cache correctness, premultiplied raster output, and D3D7 vtable slots. A fresh Gothic II/SystemPack run with Steam loaded remains required to runtime-validate the no-synthetic-scene native route and loading-screen 2D transitions. True hardware validation of DX6/DX8 and a genuine OpenGL 2.1 implementation also remains outstanding because the local test apps fall back before reaching those devices.
- The native bridge was smoke-tested on the current Ryzen/NVIDIA machine (2026-09-03, non-elevated): `CE_LHM_READY 0.9.6.0`, real GPU temperature/power/fan/core clock/memory clock/voltage, `gpu_fan` pinned to `/gpu-nvidia/0/fan/1`, all CPU rails correctly unavailable, and exit code 0 through the shutdown event. Overlay-side rendering of the native bridge's values has not been re-checked on hardware.
- LibreHardwareMonitor 0.9.6 was smoke-tested from the four-file directory on the current Ryzen/NVIDIA machine (2026-09-02, nine-metric wire format, PowerShell bridge): the bridge reached ready state; emitted real GPU Core temperature, package power, fan RPM, core/memory clock and core voltage; pinned `gpu_fan` to `/gpu-nvidia/0/fan/1` on every sample instead of alternating with `/fan/2`; correctly reported CPU temperature, package power and core clock as unavailable rather than zero on the non-elevated run; and stopped through its event with exit code 0.
- **Unverified on hardware:** the overlay side of this change. The `GPU Clocks`/`CPU Clocks` rows, the load/sensor color split, and the elevation hint are deterministic-test covered but have had no in-game run. Elevated CPU temperature/power/clock readings and AMD/Intel GPU selection also remain runtime-validation stale-risk.
- The ABI-34 core built successfully into x64/x86 hooks and Vulkan layers as build `0.1.5028`; metadata `0.1.5029` passed the full native suite and Python self-tests. Final ABI-36 build `0.1.5032` and metadata/test gate `0.1.5033` passed. Ordinary-account Vulkan session `20260717_152124` resolved the hook-published adapter, published the correct 11,943 MB capacity, initialized/rendered the overlay, and completed inject recording with 534 output frames.
- DirectDraw no longer has a full-surface overlay transfer or GPU-readback boundary. Recording must still read and convert the presented frame by definition; overlay-only native D3D7 frames require no CPU surface access, and 2D/fallback frames touch only the overlay's exact dirty region.
- HDR shader/policy regressions are covered offline across DirectX and Vulkan, and both SPIR-V payloads are compiled and validated from their checked-in GLSL sources. The secondary-DX12 contract regression proves all four render sites synchronize HDR/format state, and packed 320-nit Rec.709 green round-trips through the production PQ/Rec.2020 contract without losing chroma. Per the user, fresh visual validation of SDR-R10, scRGB, HDR10/PQ, Streamline UI, and FFX UI/backbuffer routes remains manual; this change did not launch CaptureEngine, games, or interactive test applications.
- Direct rendering uses the APIs' ordinary source-alpha blend. On PQ targets, fixed-function blending interpolates encoded values rather than absolute luminance, so partially covered antialiasing edge pixels are not mathematically linear-light composites. Opaque overlay pixels have the intended luminance/gamut. Exact destination-aware PQ alpha would require sampling/copying the game backbuffer or a substantially different compositor, which conflicts with the no-full-frame-copy/no-wait performance boundary and is not implemented.
