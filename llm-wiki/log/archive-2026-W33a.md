# llm-wiki Log Archive

### 2026-08-11 - Rebind session diagnostics when a resident hook is reactivated (missing perf_metrics CSV)

- Session `installed/captureengine/logs/20260811_212728` (build 0.1.5918): CE
  was restarted while Strange Brigade kept running, so the injector
  "Adopted resident hook ... for host reconnection". The new session dir had no
  `perf_metrics_*.csv` and no `fps_limiter_trace.log`; the hook's 5771 frames
  went into the previous session's CSV
  (`20260811_212708/perf_metrics_21120.csv`).
- Root cause: PerfLogger, the FPS limiter trace path, and the crash dump
  directory were bound once at HookThread/DllMain init and never re-bound when
  a replacement host reactivated the resident hook.
- Fix: `TryReactivateHookRuntime` now calls `RebindHookSessionDiagnostics()`:
  resolves the new DiscoveryInfo logs directory, updates
  `SetCrashDumpDirectory`, force-rebinds `PerfLogger::Init(..., true)`
  (finalizes the old CSV and starts a fresh frame sequence), and calls
  `FpsLimiter::ResetTraceLogPath()` so the next trace reopens in the new
  session. The Vulkan layer's reconnect path also force-rebinds its
  PerfLogger.
- Regression tests:
  `PerfLoggerTest.ForceRebindFinalizesOldCsvAndStartsFreshSequence`
  (functional file test) and
  `DXGISharedSourceTest.ResidentHookReactivationRebindsSessionDiagnostics`
  (source invariant).
- Source anchors: `hook/main_host_lifecycle.cpp`,
  `hook/common/perf_logger.{h,cpp}`, `hook/common/fps_limiter.h`,
  `hook/common/fps_limiter_detail/lifecycle.h`, `hook/common/hook_common.{h,cpp}`,
  `hook/vulkan_layer/layer_ipc.cpp`.

### 2026-08-11 - Fix false FSR_FG ECL-pattern latch on late inject (Strange Brigade DX12)

- Session `installed/captureengine/logs/20260811_211623` (build 0.1.5917):
  late-injected Strange Brigade DX12 (no DLSS FG, no FSR FG, no Streamline)
  rendered the overlay for only a few frames, then
  `DX12: FG detected via ECL count pattern (real=5, interp=12)` latched
  heuristic `FSR_FG` with `scQueue=null` and every later ProcessFrame hit
  `ProcessFrame — FSR FG active but scQueue=null, SKIPPING overlay`.
- Root cause: the ECL-pattern heuristic counted every zero-ECL present as an
  "interpolated" frame. During late injection the game queue's ECL hook is not
  live yet, so the first ~12 presents before the first counted real frame
  looked like interpolation evidence and tripped the 5-real/10-interp
  threshold on a non-FG game.
- Fix: zero-ECL presents now count as interpolation evidence only after a real
  frame has been observed and only once per real frame (interleaved cadence),
  and a latched heuristic deactivates after 120 consecutive real frames without
  interpolation evidence unless direct FFX API confirmation exists.
- Regression tests: `ECLPatternHeuristicDoesNotCountWarmupZeroECLFramesBeforeFirstReal`,
  `ECLPatternHeuristicRequiresCountThresholdsForDetection`,
  `HeuristicECLPatternDeactivatesAfterSustainedRealOnlyRun` in
  `tests/test_dxgi_shared_part5.cpp`.
- Source anchors: `hook/common/dx12_overlay_policy/fg_metrics_and_transitions.h`,
  `hook/apis/dx12_hook_process.cpp`.

### 2026-08-11 - Trace-level media-log reduction pass (sessions 20260811_032044, 20260810_224930)

- The WGC desktop sessions still produced ~18 lines/s in media logs at trace
  level (~23k lines / 21 min). Most remaining lines are deliberate 1 Hz health
  summaries (FPS/PERF/SMOOTHNESS, EncoderThread Alive, WGC Video memory
  periodic, CFR jitter budget, cursor timeline) and were left unchanged.
- Real remaining spam, all fixed log-only (no behavior change to media
  smoothness or audio sync):
  - `[WGC CFR] Low-source mode entered` repeated at up to ~3.6 Hz with zero
    "exited" lines (649/1203 per session): the held-mode policy enters, is
    reverted by the immediate-exit path (`!encoderTooSlowForTargetCurrent &&
    bufferedReserveRecovered`) on the next policy tick, and re-enters once the
    120 ms enter hold has elapsed. The "entered" log is now gated on the entry
    having a reason to hold (`encoderTooSlowForTargetCurrent ||
    !bufferedReserveRecovered`); flap entries stay silent and are still counted
    in the session summary (`lowSourceImmediateExits`), with source state still
    visible at 1 Hz in the CFR jitter-budget diagnostics.
  - `[VideoEncoder] QUEUE STATS` every 100 frames -> every 600 (~5 s at 120
    fps); the CRITICAL overflow line keeps its cadence-hit behavior.
  - `[VideoEncoder] Queuing audio pkt` every 100 packets -> every 500
    (~5-10 s at typical packet rates).
  - `[AppDiag] place`/`consume` were considered for 1 s -> 5 s but deliberately
    kept at their 1 s cadence: they are the audio-sync smoking-gun diagnostics
    for the app-track-silence failure class, and 1 s granularity costs only
    ~2 lines/s per app source.
- Estimated reduction: ~3-4k of ~23k lines per 21-min WGC recording, without
  touching the deliberate 1 Hz summaries or audio-sync smoking-gun content
  (AppDiag place/consume, Source Sync/Content, AppLatency WARNING, Drift debug).
- Open question (not changed): gating low-source-mode *entry* on the pipeline
  need (`encoderTooSlowForTargetCurrent || !bufferedReserveRecovered`) would
  remove the flap itself instead of only silencing its log; that is a behavior
  change to WGC CFR selection and needs runtime smoothness validation first.

### 2026-08-11 - Pseudo-overlay font size: anchor monitor effective DPI, not window DPI

- The pseudo-overlay font/circle scale was derived from `GetDpiForWindow(anchorWindow)`,
  which returns the anchor window's awareness-dependent DPI (always 96 for DPI-unaware
  apps, system DPI for system-aware apps, monitor DPI only for PM-aware apps). The font
  therefore changed size on the same monitor when the foreground app switched, jumped to
  the primary-monitor scale on a transient `GetDpiForWindow() == 0`, and started at a
  hardcoded 96 on high-DPI systems because `stickyAnchorDpi_ = 96` was truthy.
- Fix: `ResolveAnchorInfo` now resolves the scale from the anchor monitor's effective
  DPI (`GetDpiForMonitor(MDT_EFFECTIVE_DPI)` via `GetMonitorEffectiveDpi`), with
  `GetDpiForSystem()`/96 fallbacks; `stickyAnchorDpi_` removed. Pure policy in
  `common/pseudo_overlay_dpi_policy.h` + tests in `tests/test_pseudo_overlay_dpi.cpp`.
- Build-tooling: `find_process_locking_file` crashed the build with an uncaught
  PermissionError when `handle.exe` could not be spawned; it now degrades gracefully
  (advisory-only helper).

### 2026-08-10 - Trace-level logging metering pass (session 20260810_210407)

- Session `logs/20260810_210407` (build 0.1.5299, trace level) showed that even
  at verbose trace level the logs were dominated by pointless duplicates:
  hook_debug.log had ~13.9k lines in ~90 s (~9.3k/min) from `Post-SL overlay
  SUBMIT` (every frame after renderNum 1800, ~5.2k), inline-hook byte dumps
  (~2.5k), wrapper IAT retry scans (154 identical "Initializing..." lines per
  category), and FFX per-export "found at" lines (129 each); captureengine.log
  had ~1.9k identical `UpdateOverlay` lines; the media log had ~2.2k per-frame
  `Read handle for texIdx` lines.
- Fixes (log-only, no behavior change): state summaries now log on change
  (`UpdateOverlay`, inject `Read handle` per-slot); PostSL SUBMIT uses
  first-20 + every-600th cadence with a bounded dense window 1700-1900
  replacing the old crash-investigation "every frame after 1800"; wrapper/IAT
  retry scans log first 10 + every 300th (plus on summary change); FFX
  per-export lines log only for a new module and the module-level "Found
  module"/"Installing hooks" lines are metered (first 10 + every 300th) for
  modules that never become hookable (real nvngx_dlssg.dll has no FFX exports,
  so the old code re-logged them every 1 s scan - 516 copies in the 214302
  session); inline-hook per-instruction trace dumps limited to first 4 installs
  + every 100th; NVNGX `UpdatePresetHint` logs only on value change and
  `SetI: Overriding` is deduped.
- Added `common/log_meter.h` (`ce::log_meter::ShouldLogCadence`) +
  `tests/test_log_meter.cpp`; conventions documented in
  `regression-testing-and-logging.md` ("Log metering conventions").
- Session `logs/20260810_214302` (build 0.1.5904, trace level, ~8 min game)
  confirmed the pass: hook_debug.log 13.9k lines/90 s -> 8.2k lines/~8 min
  (~9.3k/min -> ~1k/min), UpdateOverlay duplicates gone (remaining lines are
  real warn-blink transitions), wrapper IAT 154x -> ~11x, inline trace detail
  174x -> ~18x, FFX per-export 129x -> gone; PostSL CONFIRMED/FG EVENT/fence
  health and all WARNs still present. Remaining ~1k/min is ~25 deliberately
  cadenced per-second/per-N-present diagnostics (ECL timing 1 s, fence/BB
  health %200, routing state %300, DetourPresent %100, ...).

### 2026-08-10 - README STALKER 2 profile example: add the missing video source

- The `[Profile.stalker]` example in `README.md` set `dll_injection=always` without `video_capture`, which under
  the canonical-profile contract resolves to no video route (overlay-only), so copying the example would not
  record video. Added `video_capture=inject`; `dll_injection=always` is now redundant there (inject capture
  already implies full injection) but harmless. The template's other `always` examples already name a working
  video source (`dxgi_dup` overlay example, `wgc` unsafe overlay-only example, `inject` unsafe full-inject
  example) and are unchanged. Docs invariant: any example with `dll_injection=always` must also name a video
  source, or it is an overlay-only profile.
