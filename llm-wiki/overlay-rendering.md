# Inject Overlay Rendering

Last cross-checked: 2026-09-02 (Streamline PCL marker capture, Vulkan layer-created queue loader data, optional LibreHardwareMonitor telemetry, marker-enhanced/fallback PC latency, actual display-change frame timing, split-renderer direct-child GPU telemetry provenance, DXGI/Vulkan presentation-color
contracts, HDR10 gamut/transfer correctness, per-monitor Windows SDR-white calibration, effective-monitor
inject-overlay DPI scaling, dynamic frame-time graph ceiling scaling, and runtime-owned FG UI transitions)

Primary sources:
- `captureengine/host_metrics.{h,cpp}`
- `captureengine/host_metrics_policy.h`
- `captureengine/sensor_service.cpp`
- `captureengine/sensor_plugin.{h,cpp}`
- `plugins/LibreHardwareMonitor/CaptureEngine.LibreHardwareMonitor.ps1`
- `tools/build/build_{project_finalize,packaging}.py`
- `tools/licenses/LibreHardwareMonitor_NOTICE.txt`
- `captureengine/display_timing_service.{h,cpp}`
- `captureengine/display_timing_policy.h`
- `common/display_timing_shared.h`
- `common/shared_defs.h`
- `common/recording_indicator_policy.h`
- `hook/common/custom_overlay.{h,cpp}`
- `hook/common/custom_font.cpp`
- `hook/common/overlay_adapter.{h,cpp}`
- `hook/common/performance_metrics.{h,cpp}`
- `hook/common/system_latency_metrics.h`
- `hook/common/system_latency_native_d3d.cpp`
- `hook/common/streamline_pcl_latency.h`
- `hook/apis/streamline_hook_pcl.cpp`
- `hook/common/reflex_defs.h`
- `hook/common/{presentation_color,dxgi_presentation_color}.h`
- `hook/common/overlay_shader_{bytecode,spirv}.h`
- `hook/vulkan_layer/vulkan_presentation_color.h`
- `hook/vulkan_layer/vulkan_reflex_limiter.{h,cpp}`
- `hook/vulkan_layer/layer_overlay_queue.cpp`
- `hook/vulkan_layer/vulkan_loader_data.h`
- `hook/vulkan_layer/shaders/overlay_{solid,textured}.frag`
- `hook/common/system_metrics.{h,cpp}`
- `hook/common/overlay_layout_policy.h`
- `hook/common/legacy_overlay_cache.h`
- `hook/common/custom_overlay_dx{8,9,10}.{h,cpp}`
- `hook/common/custom_overlay_gl.{h,cpp}`
- `hook/apis/{ddraw,dx8,dx9,opengl}_hook.cpp`
- `tests/test_overlay_system.cpp`
- `tests/test_host_metrics_policy.cpp`
- `tests/test_hardware_sensor_plugin.cpp`
- `tests/test_performance_metrics.cpp`
- `tests/test_system_latency_metrics.cpp`
- `tests/test_shared_runtime_state.cpp`
- `tests/test_vulkan_loader_data.cpp`

## Summary

The inject overlay deliberately keeps the existing compact appearance and shared CPU-generated draw format. Solid geometry and textured glyphs remain batched into the existing small command set; the 2026-07-16 polish is a local visual-quality, layout-consistency, and legacy-hot-path change rather than a renderer redesign. The entire overlay stack — text, metrics, the PC-latency row, and the frame-time graph — is first-party code: API-native custom renderers, a GDI-rasterized custom font atlas, and in-repo precompiled shaders, with no Dear ImGui or other third-party overlay/UI library.

## HDR presentation and color invariants

- Storage format is never treated as content metadata. DXGI `R10G10B10A2` can be SDR/Rec.709 or HDR10/PQ, and FP16 is scRGB only under the matching swapchain color-space contract. CE tracks successful `IDXGISwapChain3::SetColorSpace1` calls through exactly one publisher: the DXGI wrapper owns wrapped calls, while a separately installed inline hook owns unwrapped calls, refuses wrapper objects as hook targets, and publishes its atomic trampoline before the detour becomes live. The color path must never patch shared DXGI vtable slot 38; doing so composed the wrapper with its own detour and caused the Strange Brigade DX12 null-execute crash. State is retained as swapchain private data, unchanged repeated calls avoid another write/log, and an untracked swapchain uses DXGI's SDR default. Vulkan retains `VkSwapchainCreateInfoKHR::imageColorSpace` and resolves format plus color space together. Unsupported combinations fail closed instead of receiving an incorrectly encoded overlay.
- HDR state is published independently of overlay visibility, so hiding the overlay cannot change inject-video classification. D3D10/11, D3D12, Vulkan, screenshots, and runtime-owned Streamline/FFX UI/backbuffer routes consume the same presentation meaning. Cached runtime-owned UI renderers update HDR mode when a same-format target changes between SDR and HDR.
- DX12's secondary renderer is a separate `OverlayAdapter`: x64 descriptor-free, x86 Texture2D, normal backbuffer, offscreen-copy, and PostSL routes all use it. Immediately before each draw it must receive the cached presentation HDR decision plus the actual target format. Session `20260719_214733` proved that synchronizing only the primary adapter leaves this secondary adapter in SDR mode, writes sRGB endpoints directly into a PQ target, and makes a later correct HDR-to-SDR conversion pull overlay colors toward white. Transition-only logs publish the synchronized secondary contract.
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
- Inject-overlay scale is resolved once when its font atlas/backend is initialized from the nearest display's
  effective DPI (`GetDpiForMonitor(MDT_EFFECTIVE_DPI)`), with the shared legacy-DPI fallback. The target game
  window's awareness-dependent virtualized DPI must never be used: a DPI-unaware game can report 96 on a 150%
  display while its swapchain later changes from logical to physical resolution. The warm DX12 resize path
  intentionally preserves that font atlas, so correct initialization is the boundary that survives Alt+Tab and
  fullscreen recovery.
- RAM/VRAM never use fabricated capacity values. A valid used value renders even when total capacity is unavailable; RAM capacity is queried once with `GlobalMemoryStatusEx`, and unavailable GPU/VRAM telemetry renders as `--` rather than a false zero.
- Optional LibreHardwareMonitor values append compact ASCII suffixes (`C`, `W`, `RPM`) to the existing CPU/GPU rows; they never add rows or introduce a second layout path. Measurement and drawing consume the same cached formatting output, recomputed on a layout/text refresh rather than on every rendered frame, so values cannot clip merely because a sensor becomes valid. Temperature zero is unavailable; zero watts or zero RPM can be a valid stopped/idle reading.

## Host telemetry and adapter identity

- GPU and VRAM polling is out of process and does not depend on whether the game uses DirectDraw, DX6/DX7, or a modern API. The old-API failure was adapter identification: the host previously ignored its target PID and required a nonzero hook-published LUID before initializing or filtering GPU counters.
- A graphics-published adapter LUID is stamped with the publishing process ID. It wins when that PID is the selected game or a live direct child of it; the latter preserves the configured/injected parent as profile source while a split renderer owns final presentation. The sensor service resolves that parent relationship from the live process table instead of accepting any foreign publisher. When no trustworthy LUID is available, the host parses the target process's Windows `GPU Engine` PDH instances, selects the adapter with the highest non-video-engine load, and retains the prior process-derived adapter across a valid zero-load tie or a temporary missing sample. An ambiguous initial multi-adapter tie remains unavailable instead of guessing. This keeps multi-GPU selection deterministic without using API-specific guesses.
- Shared GPU usage, VRAM usage, and VRAM capacity have independent validity bits. A real 0% or 0 MB sample is therefore valid, while a missing/invalid counter remains unavailable. Adapter/source metadata and an even/odd publication sequence let the hook consume one coherent snapshot and clear old values when the source PID or adapter changes.
- LibreHardwareMonitor is never loaded into the controller, hook, Vulkan layer, or game. The dedicated sensor service monitors the controller process lifetime, launches the first-party Windows PowerShell 5.1 bridge suspended, assigns it to a kill-on-service-close job, and only then resumes it. The launch uses an explicit inherited-handle list, NUL stdin/stderr, a bounded stdout protocol, and a random named shutdown event. The bridge enables only the requested CPU/GPU visitors. `auto` GPU selection follows the device with the highest valid `GPU Core` load and retains the previous device across a tie; an ambiguous initial tie remains unavailable, while an exact identifier can pin another sensor. The native reader rejects malformed/non-finite/out-of-range output and expires a snapshot after `max(5 seconds, 3 * poll interval)`.
- The tested LibreHardwareMonitor 0.9.6 CPU/GPU closure is four user-supplied files from one release: `LibreHardwareMonitorLib.dll`, `System.Memory.dll`, `System.Numerics.Vectors.dll`, and `System.Runtime.CompilerServices.Unsafe.dll`. CaptureEngine installs and packages only its own bridge plus README. The package allowlist excludes every locally added plugin DLL/notice, and `tools/licenses/LibreHardwareMonitor_NOTICE.txt` records MPL-2.0/source/third-party references and the combined-redistribution boundary.
- All telemetry readers first validate the shared-memory ABI's exact version, size, and layout fingerprint. ABI 48 added the display-timestamp stream, ABI 50 added optional hardware-sensor values, ABI 51 added final-output timing metadata, ABI 52 added `OverlayConfig::showSystemLatency`, and ABI 53 scopes Vulkan/DLSS FG publications to their renderer process tree. Version/fingerprint isolation prevents an old reader from interpreting shifted fields; range/finite/validity checks remain a second line of defense.
- Per-core load calculation rejects regressing kernel/user/idle counters, addition overflow, and idle-underflow before computing and clamping the busy percentage. This prevents a genuine counter discontinuity from becoming an unsigned multi-billion-percent value independently of ABI validation.
- RAM publication is independent of CPU load. The earlier `RAM: -` case came from copying RAM only when the CPU sample was greater than zero; a valid RAM sample now updates even when CPU is unavailable or exactly 0%.
- The DirectDraw compatibility renderer publishes the D3D9Ex helper's default-adapter LUID immediately after helper creation, including overlay-only runs where recording never creates another modern capture device. PID inference covers startup and any path that cannot publish an exact LUID.

## Frame-time source and realtime publication

- `[Overlay] frametime_source=display_change` is the default. It derives frame time, FPS, lows, variance, graph samples, and stutter state from consecutive visible screen-change timestamps, so generated output frames and variable-refresh scanout cadence are represented. `presentation` retains the former application-presentation timestamp behavior.
- The sensor child owns the Windows graphics event session; no tracing or metadata decoding runs in an injected process. The session exists only while at least one injected target requests display timing. It associates runtime presents with graphics-queue submissions, direct-flip completion, multi-plane display completion, and generated-flip timestamps for either the selected source PID or its validated direct-child renderer.
- The session requests an 8 ms flush cadence. A 24 ms chronological reorder window merges generated and application-frame events whose delivery order differs from their screen timestamps. Duplicate or regressing timestamps are dropped, association tables are age-pruned, and each shared-memory target receives a monotonic stream. Event-buffer loss is counted in the shared diagnostics and logged at most once per ten seconds.
- ABI 48 carries a 512-slot single-producer/multi-consumer ring. Each DXGI or Vulkan overlay owns an independent cursor; the sensor never waits for readers. Generation changes reset stale history safely, slot sequence validation detects overwrite, and graph data is consumed on every overlay draw rather than at the text refresh interval.
- `PerformanceMetrics` keeps independent display-change and presentation series. Presentation history stays warm while display timing is selected. The requested display source becomes effective only after a healthy sample arrives and automatically falls back when collection is unavailable, denied, failed, or stale for two seconds. Source transitions are rate-limited in the hook log; the sensor records startup/access failures once.
- **A runtime present and the kernel present submission that carries it are not on the same thread.** D3D11 and D3D12 hand the packet to a runtime worker thread of the same process, so the present is keyed by process and the thread only refines the choice inside it (`SelectDisplaySubmissionPresent` in `display_timing_policy.h`: exact thread first, otherwise that process's oldest outstanding present). Measured on `dx11_test`: 865 runtime presents and 865 present-marked kernel submissions from two different threads of one process - an exact-thread-only rule associated zero of them, and the entire stream stayed empty while every overlay silently fell back to presentation timing.
- The service logs stage counters (`runtimePresents`/`submitAssociations`/`queued`/`published`/`suppressed`/`regressed`) every ten seconds - as a warning while nothing has been published, otherwise at debug level - because a session that runs but correlates nothing is otherwise indistinguishable from one that never started. `ProcessTrace` returning anything other than success or `ERROR_CANCELLED` marks the stream failed.
- Measured on this hardware (144 Hz VRR, NVIDIA): `VSyncDPCMultiPlane` reports `FlipEntryCount=0` and `VSyncDPC` reports `FlipFenceId=0`, so the usable completions arrive on `HSyncDPCMultiPlane` and `MMIOFlipMultiPlaneOverlay`. Stale-risk/unverified: a display path that emits neither - only `VSyncDPCMultiPlane` with a zero entry count - would need the `InterruptTargetPresentId` route, which is deliberately not implemented because it can double-count against the submit-sequence route.

## PC latency source hierarchy

- `[Overlay] show_system_latency=true` adds one independent row. A fresh marker-enhanced sample renders as `PC Latency~`, the no-marker path renders as `Latency est.`, and an unavailable sample renders as `PC Latency --`; the labels deliberately do not imply an exact end-to-end instrument reading.
- The D3D preferred path consumes the game's real Streamline `slPCLSetMarker` SimulationStart/PresentStart calls when
  `sl.pcl.dll` is present, otherwise it queries `NvAPI_D3D_GetLatency` only from an already-loaded NVAPI module.
  Streamline PCL does not populate `NvAPI_D3D_GetLatency`: the latter reports markers submitted through
  `NvAPI_D3D_SetLatencyMarker`. CE therefore intercepts the existing PCL calls with a lock-free fixed-capacity history;
  it does not fabricate or inject a competing marker stream. A PCL history expires after two seconds so plugin unload,
  stopped traffic, or an integration without usable pairs returns naturally to native NVAPI or the fallback estimate.
  Vulkan continues to query `vkGetLatencyTimingsNV` when the game exposes `VK_NV_low_latency2`.
- The marker-enhanced path correlates each displayed frame with simulation-start and present-start markers in the
  display-timing QPC domain. Native Reflex reports can also supply GPU-completion timing, but the public reports do not
  expose NVIDIA PCL's ETW input ping, so average input wait remains estimated and the tilde is retained.
- Without usable markers, the fallback combines observed Present-to-display time with one cadence-estimated simulation/render interval and average input wait. Both paths add full skipped base intervals for superseded frames and use the base-render cadence while frame generation is active, because generated output frames do not create new input-sampling points.
- NVIDIA's average-input-wait heuristic is unsupported below 10 FPS, so both paths fail closed there. Present-to-display samples over 250 ms, totals over 500 ms, incompatible timestamp domains, clock resets, and samples stale for more than two seconds also become unavailable instead of producing a plausible-looking number.
- Neither value includes USB/peripheral latency or the physical display's scanout/pixel-response delay. Native queries run at most four times per second, source-transition logs are rate-limited to ten seconds, and fixed-capacity rings plus a Present-side try-lock keep telemetry work off the rendering critical path. Streamline logs one `PCL marker latency report available` transition; failed marker forwards are rate-limited.

## Legacy backend hot paths

- `RendererBackend::OnDrawDataChanged()` marks newly built geometry. Cached frames still submit a draw every Present but do not notify legacy backends or re-upload unchanged geometry.
- DX8/DX9/DX10 upload VB/IB data only after a rebuild, buffer recreation, or failed prior upload. A failed lock/map remains dirty and returns before draw submission, so stale geometry is never drawn.
- DX8 and DX9 lazily retain one full state-block object for the backend lifetime while still capturing and applying it around every overlay draw. Capture/apply failure discards the object for safe recreation; reset/shutdown releases it. Existing half-pixel placement, render-target safeguards, fixed-function state, and BeginScene/EndScene handling remain intact.
- DX10 remaps its constant buffer only when viewport size, HDR mode, or paper-white changes. Its complete pipeline save/restore remains intact.
- A valid OpenGL 2.1 fixed-function matrix path prefers client-side vertex/color/UV arrays and one `glDrawElements` per shared command. VBO/EBO, VAO, active/client texture unit, client-array enables, matrix mode, viewport, texture, blend, depth, and cull state are restored. Capability decisions are per backend/context; a one-time error probe retains immediate mode for incompatible injected contexts. Per-Present error draining and success heartbeat logs were removed.
- DirectDraw/DX6/DX7 keep their compatibility architecture: lock/copy the full source surface, render through the D3D9Ex helper, and present the helper surface. They inherit the optimized DX9 backend, but the full-surface transfer is inherently much more expensive than a native in-device overlay and was not replaced in this targeted patch.

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

- Performance probes must launch old-API apps through CaptureEngine without recording. Those APIs cannot use the native zero-copy recording route, so a recording run would benchmark capture/conversion as well as the overlay.
- Short uncapped 4K probes on the current NVIDIA system measured valid native DX9 at roughly 7/8/10 us median/p95/p99 overlay time and DX9Ex at 2/3/6 us. OpenGL reported 49/87/262 us while running thousands of FPS, but the driver returned a 4.6 compatibility context despite the test asking for 2.1, so this exercised the modern backend rather than the legacy array path.
- The DirectDraw7 app used about 45.7% of one CPU core with the overlay disabled versus about 64.7-68.6% enabled in short probes. Disabling the graph did not materially improve it, confirming that the 4K full-surface compatibility composite, not graph geometry, dominates this route. These are diagnostic short runs, not a formal 10,000-frame acceptance baseline.
- On this machine, the DX6 test failed `QueryInterface(IDirect3D3)` (`0x80004002`) and fell back to DirectDraw; DX7 failed `IDirect3D7::CreateDevice` (`0x88760082`) and fell back to DirectDraw; DX8 device creation failed (`0x8876086C`) and the app fell back to GDI. Their poor pacing therefore primarily characterizes the fallback test apps, with measurable CE DirectDraw composite cost on top. The old zero/unavailable GPU and VRAM readings were a separate adapter-identity/validity bug fixed by the host-telemetry changes above, not an old-API sensor limitation.
- Final no-recording DirectDraw7 smoke runs covered both installed x64 and x86 binaries at 4K/150% scaling. Both visibly reported `RTX 5070`, numeric GPU load, about 1.76 GB VRAM usage of 11.66 GB, and about 13 GB RAM usage of 31.93 GB. In both sessions the sensor log first resolved LUID `0xBAB1` from the target PID, then atomically switched to `source=hook LUID` with `luidPublisherPid` equal to the DirectDraw process after the D3D9Ex helper became available.
- Debug sensor summaries include the complete CPU/max-core/GPU/VRAM/temperature/power/fan/validity snapshot, adapter LUID, publisher PID/direct parent/eligibility, and ABI signature so future field-shift or source-validity failures are diagnosable without inferring values from the rendered overlay. Selected LibreHardwareMonitor identifiers log only when they change; plugin filesystem paths and exception messages are not logged.

## Validation and stale-risk

- `VulkanLoaderDataTest` covers callback initialization, the old-loader parent-dispatch fallback, callback rejection,
  false-success validation, invalid objects, and the production ordering that initializes before queue registration.
  Portal RTX still needs a fresh runtime startup after installing the fixed build; the supplied failure session itself
  can establish the pre-fix call chain and object state, not post-fix hardware behavior.
- Portal RTX session `20260826_020732` runtime-validated the generic split-renderer overlay/crash fix but exposed the telemetry provenance gap: `hl2.exe` remained the correct profile/source PID while `NvRemixBridge.exe` owned Vulkan on RTX 5070 LUID `0xC88E`. The bridge published that exact LUID, but the sensor accepted only same-PID publishers and found no `hl2.exe` GPU-engine instances, leaving validity `0x0`. Direct-child publisher eligibility now follows the same process-lineage boundary as the Vulkan layer without changing config/source ownership. Fresh runtime validation of the numeric GPU/VRAM rows remains required.
- Focused deterministic coverage pins draw-data notifications versus cache hits, failed-upload dirtiness, DX8/DX9 state-block reuse structure, DX10 constant invalidation, OpenGL array/fallback selection and state sentinels, glyph gutters, graph geometry, text-origin snapping, dynamic row sequences, and memory-value policy.
- Live 4K validation covered native DX9 plus DirectDraw7 x64/x86 and showed valid RAM consumption rather than the unavailable marker, with the full overlay and graph rendered. Required build `0.1.4989` completed x64/x86 hooks and test apps, Vulkan layers, packaging/import closure, PE hardening, and PDB checks. All 14 focused host-telemetry tests pass. The no-build gate passed the remaining 1,644 native tests; the sole excluded cursor-bitmap test depends on the shared `IDC_ARROW`, which was temporarily transparent while the ChatGPT Windows-control session was active, consistent with cursor substitution and unrelated to overlay telemetry.
- True hardware/runtime validation of the DX6/DX7/DX8 native paths remains unavailable on the current driver because the test apps fall back before reaching those devices. A genuine OpenGL 2.1 implementation is also still needed to runtime-exercise the legacy array path; unit/source invariants currently cover it.
- LibreHardwareMonitor 0.9.6 was smoke-tested from the four-file directory on the current Ryzen/NVIDIA machine: the bridge reached ready state, emitted the real GPU Core temperature, and stopped through its event with exit code 0. The non-elevated run correctly omitted CPU temperature. AMD/Intel GPU selection and elevated CPU/power readings remain runtime-validation stale-risk; parser/config/layout/package behavior is deterministic-test covered.
- The ABI-34 core built successfully into x64/x86 hooks and Vulkan layers as build `0.1.5028`; metadata `0.1.5029` passed the full native suite and Python self-tests. Final ABI-36 build `0.1.5032` and metadata/test gate `0.1.5033` passed. Ordinary-account Vulkan session `20260717_152124` resolved the hook-published adapter, published the correct 11,943 MB capacity, initialized/rendered the overlay, and completed inject recording with 534 output frames.
- DirectDraw's full-surface transfer is the remaining known legacy cost boundary. Replacing it would be an architectural compatibility project, not a safe extension of this targeted polish.
- HDR shader/policy regressions are covered offline across DirectX and Vulkan, and both SPIR-V payloads are compiled and validated from their checked-in GLSL sources. The secondary-DX12 contract regression proves all four render sites synchronize HDR/format state, and packed 320-nit Rec.709 green round-trips through the production PQ/Rec.2020 contract without losing chroma. Per the user, fresh visual validation of SDR-R10, scRGB, HDR10/PQ, Streamline UI, and FFX UI/backbuffer routes remains manual; this change did not launch CaptureEngine, games, or interactive test applications.
- Direct rendering uses the APIs' ordinary source-alpha blend. On PQ targets, fixed-function blending interpolates encoded values rather than absolute luminance, so partially covered antialiasing edge pixels are not mathematically linear-light composites. Opaque overlay pixels have the intended luminance/gamut. Exact destination-aware PQ alpha would require sampling/copying the game backbuffer or a substantially different compositor, which conflicts with the no-full-frame-copy/no-wait performance boundary and is not implemented.
