# Changelog

## Unreleased

Changes since [v0.1.6143](https://github.com/aufkrawall/capture-engine/releases/tag/v0.1.6143).

### New

- Added in-game benchmark recording, an overlay benchmark HUD, and interactive HTML reports. The `benchmark` hotkey
  (`CTRL+7` by default) starts, stops, or clears a run; `[Benchmark]` controls an optional start delay, a fixed run
  duration, and the output directory (empty means a `benchmarks` folder beside the executable). Reports use the same
  live frame metrics the overlay already collects.
- Added `[Overlay] frametime_source=display_change` (the new default): frame time, FPS, 1% lows, variance, graph
  samples, and stutter state can come from real screen-change timestamps instead of application presents, so
  generated frames and variable-refresh scanout are included. A dedicated timing service owns the tracing work and
  publishes a lock-free ring that each DXGI and Vulkan overlay consumes independently; the overlay falls back to
  presentation timing whenever the display stream is unavailable, denied, failed, or stale.
- Added NVIDIA scheduled-flip decoding for display-change timing. Deferred flip completions are corrected with the
  driver's scheduled screen-time announcement; the payload is decoded positionally and continuously revalidated so
  an unrecognised or moved field yields no correction instead of a wrong one. This removes the DLSS 4 MFG
  presentation sawtooth that made smooth generated output look stuttery, validated in Talos at MFG 1x/2x/3x/4x.
- Added injected-overlay PC latency. `PC Latency~` uses D3D/Vulkan Reflex/PCL frame markers plus measured display
  timing, while `Latency est.` provides a frame-cadence fallback without markers. Both account for dropped frames
  and frame-generation base cadence, fail closed below the heuristic's supported rate, and exclude
  peripherals/scanout.
- Added an opt-in USB/webcam face-camera overlay for WGC/DXGI and inject capture. Camera ingest is nonblocking and
  latest-frame-only; a one-draw D3D11 compositor provides configurable placement, size, crop, mirroring, opacity,
  rectangle/rounded/circle masks, borders, SDR/HDR mapping, and moving camera content on CFR repeated game frames.
- Added opt-in, stream-only YouTube, Twitch, and custom RTMP/RTMPS output. It reuses CaptureEngine's CFR/audio
  timing, selects a low-latency H.264/AAC compatibility profile on the configured hardware backend, redacts stream
  keys, and stops the session on bounded network/queue failure instead of sacrificing A/V synchronization.
- Added optional LibreHardwareMonitor polling for CPU/GPU temperature, package power, fan RPM, core clocks, and
  voltages in the existing overlay rows. The runtime files are now installed by the build from a pinned,
  digest-verified archive instead of being copied in by hand; the PowerShell bridge was replaced by a native CLR
  host, and an unresolvable hardware scope now fails loudly instead of publishing zeros.
- Added bundled PawnIO setup with integrity verification. When CPU sensors are requested and the kernel driver is
  missing, CaptureEngine offers a single elevated Windows Package Manager install prompt (or the project page);
  install and removal are elevated product commands (`--install-pawnio` / `--uninstall-pawnio`) rather than
  user-editable scripts beside the executable.
- Added log privacy filtering. Shared logs mask the Windows account name in user-profile paths and collapse
  user-configured capture/screenshot output paths to a root prefix plus leaf; game process names, PIDs, timestamps,
  and hardware model stay logged. CE's crash-dump content map and the deliberate no-redaction containment policy
  are documented.

### Improved

- Rebalanced `ray_reconstruction_optimal_settings` by cost into a strict `off|light|medium|high|full` ladder:
  `light` applies the reconstruction and pre-smoothing passes RR replaces, `medium` adds full-resolution reflection
  tracing plus every cost-free stabilizer and engine-default floor, `high` adds the paid sampling that is visibly
  worth it (virtual-shadow ray counts and local resolution, the screen-probe octahedron lattice, radiance-cache
  probe resolution), and `full` keeps the maximum screen-probe ray count, the radiance-cache probe budget, and
  full-resolution short-range AO on UE 5.6+ (`on` still aliases `full`).
  `r.Lumen.ScreenProbeGather.StochasticInterpolation` is now `1` at every level - the cheaper stochastic path and
  the signal a ray-reconstruction denoiser expects - and inserting the new level renumbered the shared-memory
  preset byte and moved `SHARED_MEMORY_VERSION` accordingly.
- Reduced CaptureEngine's cost and interference in frame-generation games. The command-queue detour no longer
  re-registers the FG runtime's own queues on every submission, overlay work stays off the present critical path,
  hidden-overlay GPU work is isolated, and CE now measures its own per-hook cost with forwarded runtime blocking
  subtracted. In the validated FG scene the overlay costs about 7 us of GPU time per output frame, and CE's own
  CPU in the hottest hooks is about 1.4% of one core.
- Made FSR frame-generation pacing less invasive and explainable: bounded pacing-episode traces capture
  automatically on health regressions or manually on demand, preserve context across trace boundaries, decompose
  displayed frames against the callback that produced them, and record the rate-loss table. CE no longer adopts
  the runtime's queue, keeps display telemetry alive while the overlay is hidden, and reduces contention in
  callback-owned pacing.
- Improved frame-generation capture and limiter robustness: DLSS frame-generated output captures smoothly, DLSS
  MFG capture clock drift and capture-phase liveness are fixed, inject CFR recovers after display phase shifts,
  Reflex limiter recovery and pacing are stable, and CFR encoder overload recovery is faster.
- Applied native Vulkan present timing and the FFX VSync intent before output scheduling. Forced FIFO now follows
  the swapchain's own presentation contract instead of CE adding a second rate limiter, `VK_NV_present_metering`
  is withheld where it would override FIFO, and CaptureEngine no longer force-overrides variable-refresh presents
  with fixed vertical-blank pacing.
- Made split-renderer setups work correctly: Vulkan profile inheritance and GPU telemetry attribution now follow
  the real renderer child instead of the host process.
- Made the tray and elevation flow more reliable: the context menu opens above the Windows taskbar, startup stays
  responsive under delayed shell startup, admin restart hands over cleanly, a second instance no longer collides
  with a running one, and PawnIO uninstallation tears down cleanly.
- Cleaned up diagnostics and background state: duplicate per-frame trace logging on the game render thread was
  removed (about 135 lines/s in Talos under FSR FG), and orphaned CE display-timing ETW sessions left by killed
  instances are reclaimed before they exhaust the machine-wide session budget and silently degrade display timing
  and PC latency to fallbacks.
- The overlay frame-time graph now scrolls by drawn frames instead of sample arrival, so it animates smoothly under
  frame generation instead of stepping like a lower frame rate; metric values themselves are unchanged.

### Fixed

- **Portal RTX (RTX Remix):** fixed `vsync_mode=fifo` under DLSS multi-frame generation. The layer withholds
  `VK_NV_present_metering`, propagates native FIFO before Streamline DLSS-G, corrects the final DXGI FIFO present,
  and fixes a FIFO present crash. A 143 Hz display now receives a paced FIFO stream rather than the metering-driven
  ~172 fps burst.
- **Portal RTX (RTX Remix):** fixed `general_limiter_mode=reflex` applying the frame-generation divisor twice: a
  130 fps cap with 3x DLSS MFG displayed about 43 fps. The driver-owned low-latency interval is now fed the final
  output rate.
- **Portal RTX (RTX Remix):** fixed an FPS-cap escape where generated callbacks were mistaken for new output groups,
  letting the game run at ~146-167 fps against a 130 fps cap. Output-group admission is now deterministic and
  ordinal instead of a time-window guess.
- **Portal RTX (RTX Remix):** fixed overlay flicker and a stale FG multiplier under 4x DLSS MFG by keeping the
  Vulkan overlay on one composite route, growing the submit ring when a group has no reusable slot, and mirroring
  the live DLSS-G state on every present; a stale semaphore/fence reuse bug that could re-signal a still-pending
  present was fixed as well.
- **Portal RTX (RTX Remix):** fixed clean exits writing a 191 MB pre-termination dump and spending 2.5 s in it; the
  fallback now recognises an application ending itself, while genuine crashes still dump.
- **RTX Remix:** fixed override and Vulkan pacing regressions, frame-generation scheduling, and late
  frame-generation control.
- **Talos:** fixed PC-latency estimation under FSR FG, which reported about 45 ms against 70 ms of real Reflex/PCL
  markers. CE now classifies the application's own Present inside AMD's proxy and measures the generator's real
  in-flight queue depth instead of assuming one frame; the anchored generator hold is reported as measured.
- **Talos:** fixed frame-time variance that flipped between runs of the same build. The overlay no longer treats a
  flip-latch timestamp as displayed frame time or mixes screen times with latch times: display timing is selected
  only for a stream that actually resolves screen times, and presentation timing is used otherwise.
- **Talos Reawakened:** fixed DLSS overrides being skipped after a stale `NvRemixBridge.exe` renderer claim from
  Portal RTX; renderer claims are now scoped to the client that published them.
- **Frame generation (all supported titles):** fixed blank gap lines across FG switching and keep-alive, FSR FG
  frame-pacing stutter on AMD's presentation queue, a swapchain COM reference leak and Streamline runtime state
  loss across FG mode switches, and DLSS MFG capture clock drift/liveness so recorded generated output stays
  smooth. DLSS final-output declarations are complete, and the overlay reports the live DLSS-G multiplier even
  when no override is configured.
- **PC-latency overlay:** fixed idle mislabeling and survival across FG switches, 4x-vs-2x and 2x-vs-3x/4x
  reporting inversions, transition outlier spikes, doubled latency after a DLSS FG -> FSR FG switch, fallback
  recovery over hitches and unconstrained present rates, and async frame-generation correlation. Streamline PCL
  reports are ignored while FSR FG is active.
- **Display timing / VRR:** fixed `msBetweenDisplayChange` and the overlay frame-time source on variable-refresh
  displays: unclocked flip-latch sawtooth is rejected, valid VRR completions are accepted, vsync-deferred
  completions are rounded onto the blank they reach the screen at, flat but partially unlabelled display streams
  are accepted, and an FSR-FG -> DLSS-FG regime change recovers within one measurement window instead of averaging
  the old stream in for several seconds.
- **Vulkan / DX12 integration:** fixed nested DXGI swapchain recovery and its timed retry amplification, DX12
  execution discovery replacing established same-device queues, late D3D12 bootstrap and injection startup
  perturbing FSR FG, a protected FFX startup latch escaping its swapchain, a proven-queue DLSS startup blank, and
  a recursion in the NGX proxy hook path.
- **Vulkan layer:** scoped Vulkan and DLSS state to the renderer process that owns it, fixed stale renderer claims
  silencing the next game's overrides, and fixed queue-loader data plus resume verification.
- **NVIDIA LOD-spread override:** fixed `nv_lod_spread_fix=on` becoming a silent no-op on 32-bit Vulkan/OpenGL
  titles under newer drivers. The branch is neutralized by zeroing its relative displacement instead of writing a
  two-byte NOP pair that can tear at that alignment; already-patched encodings are still recognised.
- **Sensors and shutdown:** fixed a face-camera teardown deadlock and stopped unreadable sensor values from being
  published as zeros.

## v0.1.6143

Changes since [v0.1.6142](https://github.com/aufkrawall/capture-engine/releases/tag/v0.1.6142).

### New

- Added a Streamline 1.x-to-2.x upgrade bridge behind `streamline_upgrade=on`. This feature is still
  work-in-progress and currently non-functioning: it does not yet produce a working upgrade, so enabling it
  is not expected to restore Streamline features in a bridged game. The mechanism loads a complete
  user-supplied 2.x plugin set as a second, CE-owned runtime, repoints the game's `sl.interposer` import slots
  at it in memory (nothing on disk is renamed or patched), and translates the game's own 1.x Streamline calls
  onto the 2.x ABI - including measured 1.x feature-constant layouts, Reflex translation, and synthesized
  Reflex activation plus per-frame Reflex sleeps. Existing plumbing holds device continuity across adapter
  resets, reuses proven D3D12 devices, resolves adapters by fresh LUID, falls back safely for capability
  probes, serializes legacy teardown before upgrade, takes over NGX identity cleanly, and fixes tag lifetime
  and FG option deduplication.
- Expanded `[UE5]` overrides with `depth_of_field`, `dlss_super_resolution`, `dlss_super_resolution_quality`,
  `hdr_output`, `hdr_peak_luminance`, `hdr_paper_white`, `hdr_ui_luminance`, `hdr_min_luminance`, and
  `hdr_color_gamut`: `depth_of_field=off|on` writes `r.DepthOfFieldQuality`; `dlss_super_resolution=on` sets
  the NVIDIA plugin's `r.NGX.DLSS.Enable` plus the engine levers that route rendering through the third-party
  temporal upscaler (`r.NGX.Enable`, `r.TemporalAA.Upscaler`, `r.AntiAliasingMethod=2`), with quality selected
  through UE's screen percentage; the `hdr_*` settings drive `r.HDR.EnableHDROutput` and the
  `r.HDR.Display.*`/`r.HDR.UI.*` parameters in the nits and gamut the engine documents. None of them can add a
  missing plugin, invent HDR output on an SDR display, or create depth of field a game never configured.
- Expanded `ray_reconstruction_optimal_settings` into graduated `off|light|medium|full` presets: `light`
  applies four temporal/reconstruction settings (`r.SSR.Temporal=0`, `r.Lumen.Reflections.Temporal=0`,
  `r.Lumen.Reflections.BilateralFilter=0`, `r.Lumen.Reflections.ScreenSpaceReconstruction=0`), `medium` adds
  `r.Lumen.Reflections.DownsampleFactor=1`, and `full` adds the remaining former bundle values; the legacy
  `on` spelling remains an alias for `full`. Presets no longer enforce `r.NGX.DLSS.DenoiserMode=1` (select the
  RR denoiser explicitly via `force_ray_reconstruction=on`). Added `custom_cvar_overrides` /
  per-app `UE5.custom_cvar_overrides` for typed final-value overrides of individual UE5 CVars; valid entries
  take precedence over all presets and dedicated options.

### Improved

- Made overlay rendering cheap under DOOM Eternal's Vulkan "present from compute": overlay submits land on
  the game's own graphics queue instead of the compute present queue, the compute-present overlay hot path
  avoids redundant work, and CE diagnostics moved off the present critical path.
- Stopped CE from destabilizing Vulkan games structurally: no longer blocks the runtime's own presenter thread,
  no longer shrinks a swapchain below what a blocking acquire guarantees, and no longer takes stdout away
  from the game.
- Explained failing D3D12 device creation with its actual cause instead of repeating a bare HRESULT.

### Fixed

- Fixed UE5 CVar overrides and the Streamline DLSS-G override silently vanishing after a single overlay-toggle
  hotkey press: toggling republished the base config without the target profile's contributions.
- Fixed overzealous freeze detection: the watchdog armed on DX12-hook install but its heartbeat only moved on
  CE's D3D/DXGI present paths, so pure Vulkan titles were declared frozen exactly 30 s in and the in-process
  dump itself was the stall users saw; freeze claims now require live render-loop evidence across APIs, and
  dumps go through the external helper process.
- Fixed screenshots freezing games using Streamline (an infinite fence wait sat on the present path) and
  fixed overlay exclusion on screenshots not being applied there because the capture point ran after the
  PostSL overlay draw.
- Fixed Vulkan late injection producing no overlay: the implicit layer registration now stays resident so a
  title launched while CaptureEngine was closed still gets the layer, and layer discovery compatibility is
  judged by layout instead of an exact build-number match.
- Fixed hotkeys doing nothing while games like DOOM Eternal were foreground: such titles register their
  raw-input keyboard with `RIDEV_NOHOTKEYS`, suppressing `WM_HOTKEY` for everyone; CE now delivers recording,
  screenshot, and overlay hotkeys itself.
- Fixed the Vulkan layer failing device creation for applications it cannot attribute (Red Dead Redemption 2
  takes physical devices from `vkEnumeratePhysicalDeviceGroups`): device-group entry points feed the ownership
  map, resolution falls back through loader dispatch keys, and unresolvable instances forward the
  application's own `VkDeviceCreateInfo` instead of returning `VK_ERROR_INITIALIZATION_FAILED`, so dormant
  non-whitelisted installs no longer break device-group applications.
- Fixed Witcher 3 Remastered crashing under injection by adding Streamline 1.x hooking support: the API
  generation is established from generation-exclusive exports before any ABI-sensitive hook installs, so 1.x
  interposers get correct signatures (command-buffer-first `slEvaluateFeature`, enum-based `slSetTag`)
  instead of CE assuming the 2.x shapes.
- Fixed a potential game crash/freeze on close caused by the injected hook: process exit took the
  `DLL_PROCESS_DETACH` branch without ever requesting hook shutdown, so loader hooks kept resolving redirects
  into already-destroyed globals.
- Fixed a crash window where a transient d3d11.dll probe load committed CE to a full DX11 install and the
  module vanished mid-init: modules CE patches and calls are now pinned via `GetModuleHandleEx`.
- Fixed `STATUS_HEAP_CORRUPTION` on close with OptiScaler/Special K/ReShade/Steam overlay injected: the
  swapchain destructor's post-destruction refcount probe touched a chain whose last references it had just
  released.
- Fixed the inject overlay not using Windows' effective monitor DPI scale factor under some conditions:
  overlay geometry now scales from `GetDpiForMonitor(MDT_EFFECTIVE_DPI)` instead of a DPI-awareness-dependent
  window DPI.

## v0.1.6142

Changes since the last stable release [v0.1.5299](https://github.com/aufkrawall/capture-engine/releases/tag/v0.1.5299).

### New

- Added `[ThirdParty]` configuration to load ReShade, OptiScaler, and Special K from user-supplied DLL paths
  (`reshade_dll_path`, `optiscaler_dll_path`, `specialk_dll_path`) so the tools work without copying their DLLs into
  each game folder; all three can be active at once and load in the order Special K, ReShade, OptiScaler.
- Added persistent `[UE5]` overrides for injected x64 games: `force_ray_reconstruction`,
  `ray_reconstruction_optimal_settings`, `disable_post_processing_effects`, `tonemapper_sharpen`,
  `internal_fps_limit`, and `internal_anisotropic_filtering`. They redirect validated game-thread/render-thread
  CVar shadows in memory (never Engine.ini or game files), stay authoritative across map, scalability, and config
  reloads, and skip missing CVars from older UE/plugin builds safely. `internal_texture_mip_bias` shifts which
  mip level all 2D textures sample from via `r.MipMapLODBias`; `display_gamma` selects `r.TonemapperGamma`
  with sRGB/Rec709 or a pure power-curve exponent and is guarded so `r.HDR.Display.OutputDevice` is only
  written on SDR devices. Added "internal_texture_mip_bias" and "display_gamma" to the per-app profile example in the default config template.
- Added per-application `[ThirdParty]` override keys (`reshade_dll_path`, `optiscaler_dll_path`,
  `specialk_dll_path`) that take precedence over the global paths.

### Improved

- Made build and verification gates faster by isolating sanitizer Vulkan objects and overlapping packaging with lint.
- Packaged a default `testappconfig.ini` into the test-app archive folders.
- Reduced diagnostic log spam from the injected overlay and media pipeline with rate-limited, trace-level logging.
- Extended the UE5 console-registry scanner with data-pointer redirects so Lumen CVars resolve correctly in
  UE 5.6 and across UE versions; every proven element region is now covered, not just the first.
- Made the UE5 console-registry sweep resumable so large heap scans do not freeze partial results; absence
  conclusions now require a complete enumeration of all committed private RW regions.
- Prevented CE's DLL redirect from duplicating a loaded Streamline runtime instance: the hook-slot retarget
  now refuses to move a live forward pointer to a second mapped image, keeping each runtime's plugin set
  coherent.
- Decided Present-entry ownership from the loaded overlay module rather than a single byte sample, and applied
  the Streamline override all-or-nothing anchored on sl.common.
- Retried the deferred temp-swapchain Present-hook install via the guarded system-DXGI route on every service
  pass so late-injection installs do not stall when the device signal never arrives.
- Made the PostSL keep-alive submit attribute its draw to the enclosing present, preventing a false
  uncovered-present count during FG-toggle transitions.

### Fixed

- Fixed the injected overlay disappearing under FSR FG and DLSS FG, including during the DLSS toggle-ON startup
  window and after warm DLSS-FG resume; the overlay now stays visible across every FG-mode switch
  (off <-> FSR FG <-> DLSS FG) with stable topmost ownership through handoffs.
- Hardened the FG-switching matrix against blanks, lost overlay rendering, and crashes; fatal E_ACCESSDENIED switch
  failures now dump through the external helper process instead of freezing the game in-process for ~36 s.
- Fixed overlay coexistence with Steam, RTSS, and other overlays: CaptureEngine now intercepts Present below the
  foreign overlay chain, classifies the chain owner (Steam vs RTSS), validates foreign hook targets, and invokes
  RTSS's own Present handler directly instead of re-patching its callback slots.
- Fixed crashes and deadlocks around third-party tool loading: ReShade proxy-queue re-entry, the ReShade
  factory-proxy crash in the temp-swapchain install, swapchain wrapper base-reference over-release on game close, and
  the startup loader deadlock when Special K, ReShade, and OptiScaler load together (Special K now loads last, peer
  threads are suspended around tool loads, and loads run on the loader work queue).
- Fixed the pseudo-overlay font and circle scale to track the anchor monitor's DPI instead of the
  DPI-awareness-dependent window DPI, so text no longer resizes when the foreground app's DPI awareness changes.
- Fixed a DX12 startup crash in which the third-party-overlay Present-hook deferral was never made real: the
  deferred temp-swapchain Present hook now waits for the game's own D3D12 device and installs inside the same
  startup window, resolving the intermittent Steam-overlay recursion and the CaptureEngine access violation.
- Fixed FSR heuristic FG: an authoritative `ffxConfigure` OFF now vetoes the heuristic, preventing it from
  composing into AMD's UI resource after frame generation was explicitly disabled.
- Stopped re-attempting console-registry CVars whose layout CaptureEngine cannot drive, avoiding repeated failed
  writes and scan retries.
- Fixed frame timing under native FSR FG by ticking from the FFX present callback and submitting the overlay on the
  queue the FG runtime actually flushes.
- Fixed DLSS FG integration: the overlay no longer uses a dedicated overlay queue for NVIDIA DLSS FG, late-inject
  overlay submits route to the swapchain-owning queue, the FG multiplier is reported from MultiFrameCount, the
  Streamline multiplier stays latched across CreateFeature, and already-loaded DLSS-G/Reflex exports resolve at late
  injection.
- Fixed CodeQL-flagged format-argument and multiplication-overflow defects in overlay and device code.
- Fixed GCC compilation of the FFX hook header by ordering the `template` keyword before `inline`.
