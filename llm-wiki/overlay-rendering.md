# Inject Overlay Rendering

Last cross-checked: 2026-09-07 (callback registry caching and hidden-callback GPU transparency; FSR-tagged pacing windows and PresentStart-to-screen attribution; application-source Present classification for proxy-swapchain frame generation; Streamline PCL marker capture, Vulkan layer-created queue loader data, optional LibreHardwareMonitor telemetry, marker-enhanced/fallback PC latency, actual display-change frame timing, split-renderer direct-child GPU telemetry provenance, DXGI/Vulkan presentation-color
contracts, HDR10 gamut/transfer correctness, per-monitor Windows SDR-white calibration, effective-monitor
inject-overlay DPI scaling, dynamic frame-time graph ceiling scaling, and runtime-owned FG UI transitions)

Primary sources:
- `captureengine/host_metrics.{h,cpp}`
- `captureengine/host_metrics_policy.h`
- `captureengine/sensor_service.cpp`
- `captureengine/sensor_plugin.{h,cpp}`
- `captureengine/sensor_bridge_host.{h,cpp}`
- `captureengine/sensor_bridge_lhm.{h,cpp}`
- `captureengine/sensor_selection_policy.h`
- `captureengine/pawnio_setup.{h,cpp}`
- `tools/build/build_lhm_plugin.py`
- `captureengine/clr_interop.{h,cpp}`
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
- `hook/common/system_latency_types.h`
- `hook/common/system_latency_windows.h`
- `hook/common/system_latency_frame_begin.h`
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

## Layout and row invariants (`overlay_layout_policy.h`, `overlay_adapter_render.cpp`)

- Frame Generation rows (`Base/Display` rates and `FG Status`) appear atomically when frame generation is active (`fgActive == true`).
- Inactive FG never reserves phantom empty rows in `BuildOverlayRowMask`, and `OverlayAdapter::RenderContent` advances `cursorY` only when text is actually rendered. This prevents blank gap lines from appearing in the overlay when FG is toggled off or during teardown transitions.

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
- Optional LibreHardwareMonitor temperature, power and fan values append compact ASCII suffixes (`C`, `W`, `RPM`) to the existing CPU/GPU rows. Clocks and voltage instead occupy their own `GPU Clocks` / `CPU Clocks` rows (`kRowGPUClocks`, `kRowCPUClocks`), because appending `MHz`/`V` to the usage rows would push the widest row - and with it the whole adaptive overlay width - well past the memory rows. A clock row is reserved only while its parent usage row is shown *and* at least one of its own sensors is readable, so an unelevated run never leaves a labelled blank line. Measurement and drawing consume the same cached formatting output, recomputed on a layout/text refresh rather than on every rendered frame, so values cannot clip merely because a sensor becomes valid.
- Only the leading load percentage carries `GetLoadColor`; the appended sensor readings are drawn separately in `Colors::SensorValue`. `FormatCpuMetricsValue`/`FormatGpuMetricsValue` return the byte offset where the readings start, and the renderer measures the full run and the suffix run to right-align both spans. Drawing the composite in one color turned temperature, power and fan red the moment GPU load crossed the 85% threshold, reading as though those sensors were themselves critical.
- Zero means "not readable" for every metric except the fan: a package reporting 0 C, 0 W, 0 MHz or 0 V is reporting nothing, while 0 RPM is a genuine stopped fan. The rule is enforced three times independently - the bridge's `Test-SensorUsable`, `ParseSensorValue`'s `rejectZero`, and `IsSaneHostMetricsPublication`.

## Host telemetry and adapter identity

- GPU and VRAM polling is out of process and does not depend on whether the game uses DirectDraw, DX6/DX7, or a modern API. The old-API failure was adapter identification: the host previously ignored its target PID and required a nonzero hook-published LUID before initializing or filtering GPU counters.
- A graphics-published adapter LUID is stamped with the publishing process ID. It wins when that PID is the selected game or a live direct child of it; the latter preserves the configured/injected parent as profile source while a split renderer owns final presentation. The sensor service resolves that parent relationship from the live process table instead of accepting any foreign publisher. When no trustworthy LUID is available, the host parses the target process's Windows `GPU Engine` PDH instances, selects the adapter with the highest non-video-engine load, and retains the prior process-derived adapter across a valid zero-load tie or a temporary missing sample. An ambiguous initial multi-adapter tie remains unavailable instead of guessing. This keeps multi-GPU selection deterministic without using API-specific guesses.
- Shared GPU usage, VRAM usage, and VRAM capacity have independent validity bits. A real 0% or 0 MB sample is therefore valid, while a missing/invalid counter remains unavailable. Adapter/source metadata and an even/odd publication sequence let the hook consume one coherent snapshot and clear old values when the source PID or adapter changes.
- LibreHardwareMonitor is never loaded into the controller, hook, Vulkan layer, or game. The bridge is first-party native code compiled into `captureengine.exe`: since 2026-09-03 there is no script, no interpreter, and no additional shipped file. The dedicated sensor service monitors the controller process lifetime, launches `captureengine.exe --sensor-bridge` suspended, assigns it to a kill-on-service-close job, and only then resumes it. The launch uses an explicit inherited-handle list, NUL stdin/stderr, a bounded stdout protocol, and a random named shutdown event. The bridge enables only the requested CPU/GPU visitors. `auto` GPU selection follows the device with the highest valid `GPU Core` load and retains the previous device across a tie; an ambiguous initial tie remains unavailable, while an exact identifier can pin another sensor. Within a device, `auto` sensor selection is ranked and deterministic: exact preferred name, then the lowest-numbered instance of that name, then the previously selected identifier while it stays usable, and only then the highest current reading. The indexed and sticky tiers exist because the value comparison alone reselected a different sensor almost every poll on hardware that numbers its sensors - two idle `GPU Fan 1`/`GPU Fan 2` readings a few RPM apart alternated the reported fan and re-logged `Selected gpu_fan=` once per second. The native reader rejects malformed/non-finite/out-of-range output and expires a snapshot after `max(5 seconds, 3 * poll interval)`.
- The bridge role hosts the .NET Framework 4 runtime that ships with Windows (`mscoree!CLRCreateInstance` -> `ICLRMetaHost` -> `ICorRuntimeHost`) and drives the managed library through four frozen mscorlib COM contracts: `_AppDomain` slots 37/38 (`CreateInstance`/`CreateInstanceFrom`), `IObjectHandle` slot 3 (`Unwrap`), `_Object` slots 7/10 (`ToString`/`GetType`), and `_Type` slot 57 (`InvokeMember_3`). Name-based `IDispatch` is unusable in both directions: the CLR answers `GetIDsOfNames` with `E_NOTIMPL` for the mscorlib interfaces (their dispatch is typelib-backed and `mscorlib.tlb` is unregistered by default), and LibreHardwareMonitor's concrete hardware classes are internal, so their CCWs expose no class interface at all. `_Object::GetType` + `_Type::InvokeMember` reaches every public member regardless of COM visibility, and is the only mechanism `clr_interop.cpp` uses.
- Root hardware cannot be read from `IComputer.Hardware`: it is `IList<IHardware>`, a constructed generic type, and the runtime refuses to marshal one to a COM interface pointer (`InvalidOperationException`, HRESULT 0x80131509). The bridge instead binds the public `HardwareAdded`/`HardwareRemoved` events to two `System.Collections.Queue` instances via `Delegate.CreateDelegate` - relaxed parameter binding lets the `IHardware`-taking handler bind to `Queue.Enqueue(object)` - and drains them each poll. Below the roots, `IHardware.SubHardware` and `IHardware.Sensors` are plain arrays and marshal as SAFEARRAYs. `SensorType`/`HardwareType` ordinals are resolved from the loaded assembly's own metadata with `Enum.GetNames`/`Enum.Parse`, never hardcoded, and any `HardwareType` member whose name starts with `Gpu` counts as a GPU.
- Sensor selection lives in `captureengine/sensor_selection_policy.h` as dependency-free logic with direct unit coverage in `tests/test_sensor_selection_policy.cpp`; the same header declares the nine-metric wire order, maxima and zero-rejection rule that `sensor_plugin.cpp`'s parser reads, so the emitter and the parser cannot drift.
- Since 2026-09-03 CaptureEngine installs the LibreHardwareMonitor closure itself instead of asking the user to assemble it. `tools/build/build_lhm_plugin.py` fetches the official v0.9.6 `LibreHardwareMonitor.zip` over HTTPS, verifies it against a pinned SHA-256 **and** byte size, and extracts exactly four hard-coded base names: `LibreHardwareMonitorLib.dll`, `System.Memory.dll`, `System.Numerics.Vectors.dll`, `System.Runtime.CompilerServices.Unsafe.dll`. Destinations are built from that constant list, never from archive member paths, so a traversal entry cannot escape the plugin directory; duplicate candidates, oversized members, non-PE payloads, and a Microsoft dependency that lost its Authenticode certificate are all refused rather than resolved. A verification failure is fatal; an unreachable network is not, because the integration is optional. `installed-files.json` records the per-file digests so a later build re-installs a tampered or stale file. Covered by `tools/tests/test_lhm_plugin.py`.
- The release archive's plugin allowlist is those four files plus the directory README. Everything a user adds locally - the GUI executable, PDBs, storage/SMBus helpers - is still excluded, and `tools/licenses/LibreHardwareMonitor_NOTICE.txt` now carries the MPL-2.0 source-availability statement that shipping the binary requires. The package allowlist excludes every locally added plugin DLL/notice, and `tools/licenses/LibreHardwareMonitor_NOTICE.txt` records MPL-2.0/source/third-party references and the combined-redistribution boundary.
- All telemetry readers first validate the shared-memory ABI's exact version, size, and layout fingerprint. ABI 48 added the display-timestamp stream, ABI 50 added optional hardware-sensor values, ABI 51 added final-output timing metadata, ABI 52 added `OverlayConfig::showSystemLatency`, ABI 53 scopes Vulkan/DLSS FG publications to their renderer process tree, ABI 54 adds the runtime `PresentStart` associated with each display-timing sample, and ABI 55 appends the hardware-sensor CPU/GPU core clock, GPU memory clock and GPU core voltage. Version/fingerprint isolation prevents an old reader from interpreting shifted fields; range/finite/validity checks remain a second line of defense.
- Per-core load calculation rejects regressing kernel/user/idle counters, addition overflow, and idle-underflow before computing and clamping the busy percentage. This prevents a genuine counter discontinuity from becoming an unsigned multi-billion-percent value independently of ABI validation.
- RAM publication is independent of CPU load. The earlier `RAM: -` case came from copying RAM only when the CPU sample was greater than zero; a valid RAM sample now updates even when CPU is unavailable or exactly 0%.
- The DirectDraw compatibility renderer publishes the D3D9Ex helper's default-adapter LUID immediately after helper creation, including overlay-only runs where recording never creates another modern capture device. PID inference covers startup and any path that cannot publish an exact LUID.

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
- **The DLSS-G marker cross-check is the regression test.** Under Reflex the depth must measure 2 (one queued frame
  plus the interpolation hold), which reproduces the previous step exactly, so a published DLSS FG value that moves
  means the count is wrong.
- Stale-risk: measured only in unit topologies so far. Hardware run pending; the numbers to read are `appQueue=` in
  the chain line and whether the DLSS FG cross-check still agrees within a few ms.

### Frame-begin anchor (`system_latency_frame_begin.h`)

- The simulation/render span is measured, not modelled, whenever a low-latency frame boundary is observable.
  Only `LowLatencySleepReturn` (`ReflexLimiter::EndGameSleepBoundary` for Streamline `slReflexSleep` and NvAPI
  `NvAPI_D3D_Sleep`, plus `vkLatencySleepNV`) qualifies as an observable simulation-start boundary: games call it
  immediately before sampling input.
- Present wrappers do not record a frame-begin boundary: Present is entered multiple times per displayed frame in
  several configurations (e.g. 2.3x in Talos), and under frame generation the Present that returns belongs to the
  generator's pacing thread rather than the application's frame. When no low-latency sleep is active, CPU simulation
  work is modelled from the measured application-frame cadence.
- **Frame generation pacing hold.** Under frame generation, the generator holds an application frame behind the
  interpolated frames derived from it. Matching against the newest boundary at or before final-output Present would
  alias onto the next simulation frame that started while the generator was still holding the previous frame,
  falsely reporting lower latency. When generation is measured, the tracker correlates against the application-source
  Present and steps the simulation anchor back by the generator hold span.
- Mode and multiplier transitions trigger a measurement epoch reset (`ResetMeasurementsLocked`), preventing history
  from bleeding across FG 2x/3x/4x transitions or causing doubled/corrupted readings.
- VSync and backbuffer queueing need no separate term: a blocking `Present` is entered before the block and reaches the
  screen after it, so the wait is inside `present-to-display`, and a block that instead delays the wrapper's return
  moves the next frame's boundary.

### Failure modes and bounds

- NVIDIA's average-input-wait heuristic is unsupported below 10 FPS, so both paths fail closed there. Present-to-display samples over 250 ms, totals over 500 ms, incompatible timestamp domains, clock resets, and samples stale for more than two seconds also become unavailable instead of producing a plausible-looking number.
- Presentation cadence gaps > 250 ms (loading screens, pause menus, scene hitches) skip rolling cadence interval updates (`applicationPresentIntervals_`) to avoid cadence skew, but keep `applicationPresents_` and `presents_` updated so subsequent frames remain fresh.
- Causal application frame matching (`MatchApplicationPresentLocked`) rejects matches older than 250 ms across gaps, safely falling back to `baseIntervalUs` instead of inflating anchor spans. Generator hold is bounded to 250 ms, and detailed rejection causes (`p2d`, `base`, `total`) are tracked in `Diagnostics` and logged in `[Overlay] PC latency chain`.
- The 32-sample window publishes a symmetrically trimmed mean, discarding an eighth from each tail once at least 16
  samples are held. A single 250 ms telemetry poll can contribute an entire window, so one frame paired against the
  wrong Present - a full frame interval out - must not be able to move the published number.
- Neither value includes USB/peripheral latency or the physical display's scanout/pixel-response delay. Native queries run at most four times per second, and fixed-capacity rings plus a Present-side try-lock keep telemetry work off the rendering critical path. Streamline logs one `PCL marker latency report available` transition; failed marker forwards are rate-limited.
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

Sources: `hook/apis/dx12_hook_ffx_overlay_adapter.cpp`, `hook/common/custom_overlay_dx12_render.cpp`,
`custom_overlay_dx12_inline_upload.cpp`, `custom_overlay_dx12_retirement.cpp`,
`dx12_overlay_policy/inline_upload_slots.h`, `hook_cost_window.h`, and `hook/main_hookthread.cpp`.

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

**What it is not.** `hook/common/fg_cost_probe.h` (`CE_FG_COST_PROBE`, off by default) removes one CE behaviour per
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
the random downstream state.

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
- The DirectDraw7 app used about 45.7% of one CPU core with the overlay disabled versus about 64.7-68.6% enabled in short probes. Disabling the graph did not materially improve it, confirming that the 4K full-surface compatibility composite, not graph geometry, dominates this route. These are diagnostic short runs, not a formal 10,000-frame acceptance baseline.
- On this machine, the DX6 test failed `QueryInterface(IDirect3D3)` (`0x80004002`) and fell back to DirectDraw; DX7 failed `IDirect3D7::CreateDevice` (`0x88760082`) and fell back to DirectDraw; DX8 device creation failed (`0x8876086C`) and the app fell back to GDI. Their poor pacing therefore primarily characterizes the fallback test apps, with measurable CE DirectDraw composite cost on top. The old zero/unavailable GPU and VRAM readings were a separate adapter-identity/validity bug fixed by the host-telemetry changes above, not an old-API sensor limitation.
- Final no-recording DirectDraw7 smoke runs covered both installed x64 and x86 binaries at 4K/150% scaling. Both visibly reported `RTX 5070`, numeric GPU load, about 1.76 GB VRAM usage of 11.66 GB, and about 13 GB RAM usage of 31.93 GB. In both sessions the sensor log first resolved LUID `0xBAB1` from the target PID, then atomically switched to `source=hook LUID` with `luidPublisherPid` equal to the DirectDraw process after the D3D9Ex helper became available.
- LibreHardwareMonitor 0.9.6 no longer uses WinRing0; it embeds PawnIO bytecode modules and reads the CPU temperature, package power and clock rails through the Microsoft-signed `PawnIO.sys` kernel driver. Both the driver **and** elevation are required - measured on a machine that has PawnIO installed and still read every CPU rail as exactly zero without elevation. `captureengine/pawnio_setup.cpp` detects the driver by its service key (`SYSTEM\CurrentControlSet\Services\PawnIO`), and when CPU metrics are requested but the driver is absent it offers installation once at startup (install / not now / don't ask again), persisted per user in `HKCU\Software\CaptureEngine`. CaptureEngine bundles the official Microsoft-signed installer (`plugins/LibreHardwareMonitor/PawnIO_setup.exe`) for full offline setup without package manager or runtime network dependencies. Before running under administrator elevation, CaptureEngine verifies the installer's Authenticode certificate and pinned SHA-256 digest (`1f519a22e47187f70a1379a48ca604981c4fcf694f4e65b734aaa74a9fba3032`) to prevent privilege escalation via local file tampering. Installation (`-install -silent`) and uninstallation (`-uninstall -silent` or registered `QuietUninstallString`) can be triggered from the system tray context menu (`Open config`, `Install PawnIO` / `Uninstall PawnIO` with confirmation, `Close`), the startup prompt, or elevated CLI commands (`--install-pawnio`, `--uninstall-pawnio`). When triggered unelevated, CLI and tray actions prompt for standard UAC elevation once, and successful installation offers a 1-click elevated relaunch so CPU sensors become active immediately.
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
- The native bridge was smoke-tested on the current Ryzen/NVIDIA machine (2026-09-03, non-elevated): `CE_LHM_READY 0.9.6.0`, real GPU temperature/power/fan/core clock/memory clock/voltage, `gpu_fan` pinned to `/gpu-nvidia/0/fan/1`, all CPU rails correctly unavailable, and exit code 0 through the shutdown event. Overlay-side rendering of the native bridge's values has not been re-checked on hardware.
- LibreHardwareMonitor 0.9.6 was smoke-tested from the four-file directory on the current Ryzen/NVIDIA machine (2026-09-02, nine-metric wire format, PowerShell bridge): the bridge reached ready state; emitted real GPU Core temperature, package power, fan RPM, core/memory clock and core voltage; pinned `gpu_fan` to `/gpu-nvidia/0/fan/1` on every sample instead of alternating with `/fan/2`; correctly reported CPU temperature, package power and core clock as unavailable rather than zero on the non-elevated run; and stopped through its event with exit code 0.
- **Unverified on hardware:** the overlay side of this change. The `GPU Clocks`/`CPU Clocks` rows, the load/sensor color split, and the elevation hint are deterministic-test covered but have had no in-game run. Elevated CPU temperature/power/clock readings and AMD/Intel GPU selection also remain runtime-validation stale-risk.
- The ABI-34 core built successfully into x64/x86 hooks and Vulkan layers as build `0.1.5028`; metadata `0.1.5029` passed the full native suite and Python self-tests. Final ABI-36 build `0.1.5032` and metadata/test gate `0.1.5033` passed. Ordinary-account Vulkan session `20260717_152124` resolved the hook-published adapter, published the correct 11,943 MB capacity, initialized/rendered the overlay, and completed inject recording with 534 output frames.
- DirectDraw's full-surface transfer is the remaining known legacy cost boundary. Replacing it would be an architectural compatibility project, not a safe extension of this targeted polish.
- HDR shader/policy regressions are covered offline across DirectX and Vulkan, and both SPIR-V payloads are compiled and validated from their checked-in GLSL sources. The secondary-DX12 contract regression proves all four render sites synchronize HDR/format state, and packed 320-nit Rec.709 green round-trips through the production PQ/Rec.2020 contract without losing chroma. Per the user, fresh visual validation of SDR-R10, scRGB, HDR10/PQ, Streamline UI, and FFX UI/backbuffer routes remains manual; this change did not launch CaptureEngine, games, or interactive test applications.
- Direct rendering uses the APIs' ordinary source-alpha blend. On PQ targets, fixed-function blending interpolates encoded values rather than absolute luminance, so partially covered antialiasing edge pixels are not mathematically linear-light composites. Opaque overlay pixels have the intended luminance/gamut. Exact destination-aware PQ alpha would require sampling/copying the game backbuffer or a substantially different compositor, which conflicts with the no-full-frame-copy/no-wait performance boundary and is not implemented.
