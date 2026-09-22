# Changelog

## Unreleased

Changes since [v0.1.6772](https://github.com/aufkrawall/capture-engine/releases/tag/v0.1.6772).

### Fixed

- **Delayed keystrokes in other applications while a hotkey fired:** the global hotkey keyboard hook, which every keystroke on the desktop waits for, wrote a log line (with a disk flush under a process-wide lock) inside its callback. A slow flush, typically right when a recording starts, held keyboard input for every application. The hook thread now only counts. The controller does the logging.
- **Keyboard input could wait on busy game threads:** the hotkey hook thread now runs at time-critical priority, so a game's high-priority render threads can no longer keep it from answering while all cores are busy.
- **Silent loss of the hotkey keyboard hook:** Windows removes a keyboard hook without notice after repeated timeouts, which left hotkeys dead in games that suppress normal hotkeys (e.g. DOOM Eternal). A late answer is now detected, logged (`[Hotkey] Keyboard hook answered late`), and the hook is re-armed immediately; a removal Windows had already made is logged and repaired.
- **Keyboard input froze during a CaptureEngine crash dump:** writing the controller's crash dump suspends its threads, including the keyboard hook's, so every keystroke on the desktop waited on the hook timeout until the dump finished. The hook is now removed before the dump starts.

### Removed

- **Limiter helper process:** the separate limiter process had not paced anything since frame pacing moved into the game process, but it still ran a highest-priority thread pinned to CPU core 1 with a spin-wait, which could delay other threads on that core, including input processing of unrelated applications. Its request wait could also spin a core at full load. It is gone. Capture-synced recording no longer waits for it or fails with "limiter readiness failure". The FPS limiter itself is unchanged.
- **In-game window procedure hook:** the injected runtime no longer replaces the game window's message handler. It forwarded every window message (including every high-rate mouse input message) through a lock without using any of them, and made unloading riskier alongside other overlays.

## v0.1.6772

Changes since [v0.1.6652](https://github.com/aufkrawall/capture-engine/releases/tag/v0.1.6652).

### New

- **AMD FidelityFX CAS and RCAS post-processing sharpening (D3D11, D3D12, Vulkan):** added Contrast Adaptive Sharpening (`sharpen=cas`) and Robust Contrast Adaptive Sharpening (`sharpen=rcas`). Configurable via `sharpen_contrast` (adaptation sensitivity) and `sharpen_amount` (blend weight). Executes as a full-screen GPU pass prior to overlay composition, preserving clean overlay text. Supports SDR and HDR (using ST 2084 PQ for scRGB to protect specular highlights), includes sharpened output in screenshots, and supports live runtime tuning via configuration reloading without restarting the game.

- **Driver-level DLSS Multi-Frame Generation controls:** added `[DLSS]` configuration keys (`dlss_fg_mode`, `dlss_fg_fixed_count`, `dlss_fg_dynamic_max`, `dlss_fg_target_fps`) to override the driver-settings channel in-process. Enables fixed generation up to 5x/6x or dynamic cadence targeting display refresh rate (`dlss_fg_target_fps=max_refresh`) without modifying global driver profiles.

- **DLSS Frame Generation V-Sync override:** extended `vsync_mode` to intercept and answer the driver-level V-Sync query read by the DLSS-G runtime, ensuring forced synchronization settings apply correctly above Streamline swapchain proxies.

- **NVIDIA NGX over-the-air update control (`ngx_ota`):** added `[DLSS] ngx_ota=off|on` to intercept and suppress background `nvngx_update.exe` launches and clear Streamline OTA preferences during `slInit`, preventing remote OTA updates from overriding locally configured DLL versions.

- **NVIDIA NGX diagnostic logging (`ngx_log`):** added `[DLSS] ngx_log=off|on|verbose` to route NGX runtime diagnostic logs directly into the session directory for diagnosing DLL override and Streamline plugin resolution.

- **Automated GitHub release notes generation & changelog tooling:** added `tools/manage_changelog.py` to validate formatting, prevent tag/release note drift, and automate synchronized GitHub release note publication during stable release workflows.


### Improved

- **Vulkan implicit layer registration decoupling:** the implicit layer manifest is now staged in CaptureEngine's runtime directory rather than build paths. Stale `baseDir` entries from prior builds are neutralized on startup, and orphaned machine-wide (HKLM) registrations are explicitly warned on when running unelevated.

- **Unelevated process monitoring CPU reduction:** replaced periodic WMI process table queries (previously twice per second) with direct native Windows process queries, eliminating sustained background WMI service CPU overhead.

- **Early recording cancellation handling:** stopping a recording during media pipeline startup now cleanly marks the session cancelled in the manifest and overlay rather than hanging indefinitely on "Finalizing recording...". Audio latency calibration is now cached per session to eliminate startup latency on subsequent recordings.

- **Recording startup telemetry:** the controller now logs the confirmed live timestamp and measured pipeline startup latency rather than assuming immediate recording start upon request dispatch.

- **Hook installation diagnostics under NVIDIA Smooth Motion:** logged hook installations and removals now report when fallback thread-suspension heuristics were used, clarifying hook status when driver background threads prevent strict thread quiescence.

- **Startup performance diagnostics accuracy:** unified startup timing accumulation across controller subsystems so `[StartupPerf]` logs accurate elapsed durations rather than uninitialized or system uptime values.

- **Developer LSP, code style, and editor tooling integration:** restored canonical root configuration files (`.clangd`, `.clang-format`, `.clang-tidy`, `.editorconfig`, `pyrightconfig.json`) to enable in-editor clangd diagnostics, automatic 4-space K&R code formatting, and EditorConfig support across all IDEs. Retained compilation database entries for unbuilt translation units across partial and test-only builds so `clangd` maintains full-codebase indexing and IntelliSense during routine test loops. Enabled C++ standard library indexing, all-scopes symbol completions, and block-end inlay hints, and configured safe header insertion policies to prevent Windows SDK include ordering issues. Configured `.vscode/settings.json` to use the project's bundled MSYS2 Clang 22 toolchain, added recommended workspace extensions, synchronized Python type-checking exclusions with internal facade units to eliminate false diagnostic squiggles, and added HLSL shader file associations.


### Fixed

- **Games failed to start with a driver error when `backbuffer_count` was set:** Strange Brigade (DX12) aborted during startup with "Can't recover from driver error. Error Code 80070057" and never rendered a frame. `backbuffer_count` adds a frame-latency waitable object to the swap chain, which DirectX requires the game to repeat on every buffer resize; the correction that hid it from the game was skipped whenever another overlay (for example Steam) owned the present entry, so the game's own resize was rejected. The flag is now hidden consistently on every path, and it is only requested when that correction is guaranteed to exist.

- **Anisotropic filtering and mip bias silently did nothing in DirectX 12 games:** forced AF and negative mip bias never reached titles that resolved Direct3D 12 before CaptureEngine attached, because the sampler and root-signature overrides were installed only when CaptureEngine observed device creation itself. They are now installed during injection setup, which covers devices the game had already created. A session that still reaches no sampler now says so explicitly in the log instead of failing silently.

- **Debug log could drop lines without saying so:** under heavy logging a few entries were discarded with no record anywhere, making any gap impossible to tell from "nothing happened". Dropped lines are now counted and reported, log buffer overflow is reported, and the buffer is drained again immediately instead of after a pause whenever it was found full.

- **Performance CSV was cut off mid-row in Unreal Engine titles:** games that exit by terminating themselves — routine in UE5 — skipped the only code that closed `perf_metrics_*.csv`, so the buffered tail was lost and the final row was truncated. The file is now finalized from the process-termination path.

- **Buffer count override could shrink a swap chain the game asked to leave alone:** a resize that passes no buffer count means "keep the current one" in DirectX; CaptureEngine substituted the configured depth there and reallocated the chain.

- **Strange Brigade / Steam module injection crash:** fixed startup access violation crash caused by concurrent CaptureEngine hook threads racing to modify memory page protection while patching adjacent Import Address Table (IAT) entries during Steam overlay library loading.

- **Live configuration reload freeze & overlay blackout:** fixed in-game overlay disappearing and potential game render thread deadlocks when saving `config.ini`. Configuration reload queries now respond immediately with asynchronous background cache warming, preventing false IPC timeout respawns.

- **Recording finalization freeze:** fixed a deadlock where stopping a recording while privacy blackout focus checks were executing could leave the media worker hung indefinitely waiting on a shared lock.

- **Windows Error Reporting dialog suppression:** properly configured WER error modes so game crashes write diagnostic minidumps directly to the session folder without hanging on a modal "program has stopped working" prompt.

- **Crash on virtual desktop focus transition:** fixed a crash caused by caching an unmarshaled COM interface pointer across thread apartment teardown during window focus changes.

- **DirectX 12 sharpen queue drain:** fixed fence drain and ComPtr lifecycle issues during overlay teardown when sharpening was active.

- **DirectX 12 sharpen crash during DLSS Frame Generation toggles:** properly synchronized command queue transitions when enabling/disabling DLSS Frame Generation mid-game, preventing GPU resource recycling hazards and frame tearing.

- **Sharpening mode dynamic switch crash:** fixed crash when switching between CAS and RCAS at runtime caused by releasing pipeline state objects while previous GPU frames were still executing.

- **Vulkan sharpening command buffer starvation:** fixed an issue where failed or aborted queue submissions left command buffers permanently marked in-flight, which eventually disabled the sharpening pass for the remainder of the session.

- **The Witcher 3 (DX11) with NVIDIA Smooth Motion:** fixed startup crash caused by compositing onto the interposer's transient output swapchain buffers. Also resolved severe overlay flickering and corrected FPS counter to reflect game render rate rather than interposer presentation rate.

- **`__fastfail` crash dump capture:** unhandled fatal crashes and security check terminations (`0xC0000409`) that bypass in-process SEH/VEH handlers are now captured from WER crash dumps into the session directory, and legacy invalid registry configuration paths are automatically cleaned up.

- **Early-startup Streamline DLL override race:** armed the DLL redirection loader hook directly in `DllMain` using pre-published injector configuration, preventing early-loaded modules like `sl.common.dll` from bypassing overrides before the background hook thread finishes reading configuration.

- **Steam overlay hook layering reliability:** added automatic retries when hooking present body calls during initial game thread creation, ensuring CaptureEngine layers correctly beneath the Steam overlay.

- **Render thread stutter during Reflex / PCL hook resolution:** bounded retry attempts for Streamline functions not exported by specific library builds, preventing periodic 2.5-second render thread hitches.

- **False frame generation health warnings in auto/dynamic modes:** suppressed false-positive activation warnings when DLSS Frame Generation dynamically chooses not to interpolate frames.

- **DLSS Dynamic MFG status detection:** fixed false "dynamic MFG not supported" overlay warnings by gating state field reads on the game's actual DLSS-G struct version.

## v0.1.6652

Changes since [v0.1.6261](https://github.com/aufkrawall/capture-engine/releases/tag/v0.1.6261).

### New

- **DirectDraw / Direct3D 7 games are supported** (Gothic II and other legacy titles). Overlay, recording,
  screenshots and the `[Graphics]` overrides (V-Sync, anisotropic filtering, mip mapping) now work there. The
  overlay draws with the game's own Direct3D 7 device (`legacy_d3d_native_overlay=on`) and falls back to a CPU
  compositor for 2D frames, loading screens and unusual surface formats - neither route does a GPU readback on
  the game's render thread.
- **Screenshots can save HDR and SDR from one capture.** The new default `[Screenshot] color_space=both`
  publishes an HDR shot twice: as a native 10-bit BT.2020/PQ AVIF and as a tone-mapped SDR PNG, under one name
  that differs only by extension. Both encodes run at the same time, so the pair costs little more than the AVIF
  alone, and a capture that is not in HDR still saves exactly one PNG.
- **NVIDIA Smooth Motion is recognised.** CaptureEngine detects it, reports its status with the real base and
  output frame rates, draws the overlay on Smooth Motion's own output flip - topmost above Steam and RTSS, and
  not interpolated with the game frame - and applies `vsync_mode=fifo` there.
- **FFmpeg messages now reach the session log.** Encoder, muxer and RTMP diagnostics previously went to a
  discarded stderr. They are logged with stream keys redacted, so live streaming keeps full diagnostics instead
  of being silenced to protect the key.

### Improved

- **Lower input lag from the FPS limiter.** It used to spend its entire wait after the game had already finished
  the frame, so a finished frame aged in the present hook - 9.3 ms of it in Strange Brigade at a 90 fps cap. The
  game is now released ahead of the deadline, with a reservation learned from real overruns and from GPU
  completion times so smoothness is not traded away for latency. Recording with capture sync deliberately keeps
  the old placement, because a missed deadline there costs a repeated frame in the file.
- **GPU load is readable under frame generation.** The overlay summed the 3D, Compute and Copy engines and
  clamped the total to 100%, which parked the reading on the clamp whenever FG was running. It now reports the
  busiest engine, the same way Task Manager does.
- **The GPU and VRAM rows stop flashing `--`** while a game briefly has no GPU work. Windows removes the counter
  instance there, which means idle, not unreadable.
- **Faster, steadier game startup.** Four variable-cost stalls are gone: a machine-wide thread snapshot taken for
  every installed hook, a throwaway WARP Direct3D 12 device created in games that never use DX12, and two
  unbounded window-title queries on the render thread. Hook installation dropped from about 1.25 s to 0.65 s and
  the worst single hitch from 533 ms to 15 ms.
- **Crash dumps from 32-bit games now contain 32-bit stacks.** The dump helper is 64-bit and had been recording
  only the WoW64 side, which holds nothing but a syscall thunk - the same gap Task Manager's own dumps have.
- **A freeze the game explains itself is recorded with stacks instead of 30 MB.** When the stuck thread is simply
  running its own error dialog, the freeze is still detected and named in the log, just without a full memory dump.
- **Privacy blackout follows virtual desktops.** `black_when_no_fullscreen_focus` now rejects cloaked windows,
  Task View and shell windows, and keeps monitor capture tied to the process that established fullscreen focus.
- **Cheaper Vulkan capture:** per-frame allocations removed from the present path, and a swapchain that moves to
  another queue family mid-run re-learns its settings instead of keeping the ones chosen at startup.

### Fixed

- **Gothic II (DirectDraw/SystemPack):** fixed injection crashing at startup on this title's older import tables
  and on relocated bypass code; fixed the overlay flickering every frame, disappearing during loading screens and
  darkening itself on repeated draws; fixed a crash in the intro videos caused by the overlay re-binding a texture
  the game had already released; fixed a full game freeze caused by a hook cycle between CaptureEngine and the
  Steam overlay, and the frozen picture that the first fix for it produced; fixed V-Sync and anisotropic-filtering
  overrides doing nothing in this game's borderless mode; fixed screenshots never completing in-game; fixed the
  overlay compositor costing most of the frame rate; and fixed a VRAM reading of 8.7 exabytes.
- **Portal RTX (RTX Remix):** fixed heavy stutter with `vsync_mode=fifo` under DLSS multi-frame generation -
  CaptureEngine was taking the generator's own flip scheduling away. Fixed the same stutter at 4x MFG, where the
  generated batch no longer fits the display's refresh rate: the rendered rate is now bounded by the panel, so
  2x/3x/4x all pace cleanly on a 144 Hz display. Fixed clean exits writing a 183 MB dump.
- **DOOM Eternal:** fixed the window going black for the rest of the session after a swapchain change (overlay
  objects were released too late, and their present-wait semaphores too early). Fixed CaptureEngine pushing NVIDIA's
  Vulkan driver off its native present path - visible as the Windows volume popup drawing over the fullscreen game.
- **Strange Brigade:** fixed the frame rate running at ~130 fps against a 90 fps cap while the limiter reported a
  perfect 90. Fixed a first-frame crash when the driver's Smooth Motion was enabled.
- **Frame generation and the overlay:** fixed overlay text rendering as blank boxes under DLSS 4x MFG, and fixed
  screenshots and overlay-free recording picking the wrong frame while a game had DLSS FG temporarily suspended.
- **Forced V-Sync in Vulkan titles:** the layer's bridge functions were never actually exported, so the
  vertical-blank correction could never run. It is verified against the shipped DLL now.
- **32-bit games:** fixed every `[UE5]` console-variable override being silently inert in 32-bit Unreal titles,
  and a weakened sanity check on the legacy Direct3D 9 path.
- **Crash and freeze reporting:** dumps are written from a copy of the exception state instead of stack memory
  that may already have been reused; a second crash can no longer start a second dump worker on top of the first;
  the crash handler can no longer deadlock on itself; and the freeze watchdog no longer accuses Vulkan and
  DirectDraw games it never observed presenting.
- **Capture:** an injected frame that names a process other than the capture session's own is dropped instead of
  being opened.

## v0.1.6261

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
