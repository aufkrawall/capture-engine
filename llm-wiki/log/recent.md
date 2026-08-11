# llm-wiki Log

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

### 2026-08-09 - Desktop screenshot after closing an injected game stalls 15 s and shows no overlay confirmation

- Session `logs/20260809_165328` (Robocop, build 0.1.5901): the in-game hook screenshot at 16:53:51 completed in
  ~1.4 s, Robocop exited at 16:54:00, and the first desktop screenshot at 16:54:07 blocked until 16:54:22
  (`[Screenshot] Hook request 2 failed (wait=258 status=1 error=0)`), after which 7 queued GDI screenshots fired in
  ~1.3 s with a single late `Screenshot saved!` warning.
- Root cause: the hook sets `sourcePid` in shared memory but lives inside the game process and dies with it, so
  `GetSourcePid()` stayed stale after the exit. `TryHookScreenshot` saw the stale non-zero PID, published a request,
  and waited the full 15 s hook timeout on an event no process would ever signal; the controller message loop,
  overlay restore, and screenshot notification were blocked the whole time.
- Fix: `captureengine/inject_main.cpp` detects the dead source PID in its main-loop monitor and clears
  `sourcePid`/`luidSourcePid` plus the screenshot request protocol state; `captureengine/screenshot.cpp`
  `TryHookScreenshot` verifies source-process liveness before publishing a request (dead source -> immediate desktop
  fallback with a clean protocol reset), and a failed/timed-out hook request now resets the full protocol state
  instead of leaving a stale Pending/Writing status that would make the next injected game's screenshot skip the
  hook path. Liveness uses `PROCESS_QUERY_LIMITED_INFORMATION`; sources that deny even that query are treated as
  alive, so protected games are never skipped. The 15 s timeout remains only as the backstop for a live but
  unresponsive hook. Coverage: `tests/test_inject_source_lifecycle.cpp`,
  `tests/test_screenshot_source.cpp` (dead-source skip, liveness helper, failure reset).

### 2026-08-09 - Steam overlay GUI invisible during DLSS FG: Steam must be serviced on the worker's displayed presents

- Session `logs/20260809_160835` (Talos, build 0.1.5900) validated the source-thread latch: `sourceTid=0x09CC`
  latched, `Invoking guarded Steam Present hook` fires continuously on the game thread (including during FG, #50 with
  `slFG=1`, #1000 at 16:09:07), and worker presents (T:5CF0) correctly skip. The user sees the Steam overlay
  startup animation but Shift+Tab does not open the GUI in-game. CE's own hotkeys are CTRL+9/8/0/- (not Shift+Tab),
  so input is not stolen.
- Root cause: during DLSS FG the frames actually displayed are the DLSS-G worker's generated-output presents (CE's
  PostSL overlay draws on those and is visible), while Steam's hook only runs on the game thread's source-frame
  presents - the runtime re-renders those buffers, so Steam's GUI is overwritten and never visible. The startup
  animation works because it renders pre-FG on directly displayed presents.
- Fix (build 0.1.5901): `ShouldInvokeSteamOnStreamlineWorkerPresent` in
  `hook/common/dxgi_shared_detail/types_and_state.h` allows `TryInvokeGuardedExternalSteamOverlayPresent` to service
  Steam from a DLSS-G worker Present only in steady state: `streamlineFGRunning && postSLConfirmedRendering &&
  !startupTransitionWindowActive && !postSLConfirmedButStartupSettling && !runtimeOwnedNativeFGPresentPath &&
  !fsrRuntimeActive && !fsrApiActive`. The 20260809_015416 stall happened during the startup transition window and
  stays strict; native-FSR paths keep the game-thread-only rule. The skip log now reports `workerSteam=` and the
  settling/startup fields. Coverage: `tests/test_dxgi_shared_part10.cpp`
  (`StreamlineWorkerMayServiceSteamOnlyInSteadyStateDLSSFG`,
  `GuardedSteamInvokeServicesSteadyStateStreamlineWorkers`).
- Runtime validation pending: Shift+Tab during DLSS FG must open the Steam GUI (now drawn on worker presents); watch
  for any worker stall inside Steam (the 015416 signature) - the startup-window gate is the main mitigation.

### 2026-08-09 - Steam overlay invisible in Streamline games: source-thread tracker could never latch

- Sessions `logs/robocopgoodnew` and `logs/talosgoodnew` (build 0.1.5899) confirmed the runtime override + overlay
  fixes and the absent exit dumps, but the user reported the Steam overlay no longer renders. All six retained DLSS
  sessions (0.1.5895 through 0.1.5899) show the same signature: `Skipping guarded Steam Present hook #N ... using
  bypass trampoline instead (... sourceTid=0x0000 ...)` and zero `Invoking guarded Steam Present hook` lines.
- Root cause: the 2026-08-09 source-thread provenance rule (session `20260809_015416` deadlock fix) requires a
  verified source Present thread before Steam may be invoked, but the tracker could never latch one in Streamline
  games. `applicationSourcePresent` was derived from `frameGenerationPresentationActive`, which includes
  `callerFromStreamlineModule`; the interposer forwards the game's own Present calls even while FG is off, so every
  present looked runtime-generated, `dx12_hook_g_GamePresentThreadId` stayed 0, and the guarded Steam invoke was
  permanently blocked -> Steam overlay never rendered (CE's overlay kept working via bypass/PostSL).
- Fix (build 0.1.5900): new `ce::dx12_overlay_policy::IsRuntimeGeneratedFrame` classification used for the
  `applicationSourcePresent` computation at both DetourPresent/DetourPresent1 sites. It deliberately excludes
  `callerFromStreamlineModule` (the wrapper forwards real frames pre-FG); only actual runtime-generated frames
  (no-callback FSR, Streamline FG active, runtime-owned swapchain, FFX caller, runtime-owned native FSR) disqualify a
  present. Pre-FG wrapper presents now establish the game Present thread (observed: single thread 0x3A54 for
  presents/ProcessFrame/PostSL in RoboCop), so during FG the guarded Steam invoke fires on the verified game thread
  while worker presents still fail closed to the bypass. The worker deadlock protection from `20260809_015416` is
  unchanged (workers can never be tracked). Coverage: `tests/test_dxgi_shared_part10.cpp`
  (`StreamlineWrapperPresentsAreSourceFramesForTheThreadTracker`,
  `PresentSourceClassificationExcludesStreamlineWrapperCaller`).
- Runtime validation pending: a new RoboCop/Talos session must show `Invoking guarded Steam Present hook` with
  `sourceTid=<game thread>` and the Steam overlay visible (Shift+Tab) during DLSS FG.

### 2026-08-09 - NVIDIA DLSS teardown 0xC000004B is not a crash; pre-termination dump policy exempts it

- RoboCop/Talos validation sessions `logs/robogood` and `logs/talosgood` (build 0.1.5898) confirmed the runtime
  override fix end to end: every sl.*/nvngx_dlss* module loads from the configured NPI folder (preload + redirect,
  including the NGX model-repository redirects `Redirecting 1B0_E658703.dll (NGX model sl_*_0)`), RoboCop now
  creates and evaluates NGX Feature 13 (RR) with preset E, SR preset M is applied, and the overlay renders
  continuously through PostSL (1180+ / 2085+ submits, all rendered=1, devRemoved=0, no crash markers).
- Both sessions ended with a `crash_external_fatal_exit_NtTerminateProcess_c000004b_*.dmp` (~130 MB) although the
  games exited cleanly (WMI exit code 0). The captured stack is deterministic in both games:
  `nvngx_dlss.dll` (NPI 310.7) -> `nvapi64.dll` -> `NtTerminateProcess(0xC000004B)`. 0xC000004B is
  `STATUS_PROCESS_IS_TERMINATING`: the DLSS snippet worker's losing teardown race against the game's clean
  ExitProcess(0) - not a crash. CE's pre-termination policy classified it as crash-like purely by the NTSTATUS
  severity bits.
- Fix (build 0.1.5899): `common/crash_dump_policy.h` adds `kProcessIsTerminatingExitCode = 0xC000004B` and exempts
  it in both `IsCrashLikeProcessExitCode` and `ShouldCapturePreTerminationDump` (including the active-FG fallback).
  Regression coverage in `tests/test_crash_dump_policy.cpp`.

### 2026-08-09 - DLSS/Streamline override DLLs never loaded in RoboCop; runtime preload + late-module IAT fix

- Sessions `installed/captureengine/logs/talosdlssoverride` and `robocopdlssoverride` (build 0.1.5895, identical
  profiles: `dlss_sr_preset=m`, `dlss_rr_preset=e`, `force_ray_reconstruction=on`, `dlss_debug_overlay=on`, all
  `*_dll_path=%USERPROFILE%\Programme\npi\sl`): Talos created/evaluated NGX Feature 13 (RR) and applied preset M;
  RoboCop created/evaluated ordinary Feature 1 (SR) and never even queried Feature 13 requirements - the RR override
  installed (`persistent r.NGX.DLSS.DenoiserMode=1 override installed ... score=130`) but the plugin still read SR.
  The HUD worked in both (registry spoof, runtime-independent).
- Root cause is loader coverage, not the RR CVar: only `sl.interposer.dll` ever produced a `Redirecting ...` line in
  either game. `InitializeKernel32Hooks` is a one-time IAT snapshot, so Streamline-internal loads (sl.common ->
  sl.dlss/sl.dlss_g + NGX snippets/core) run through the IAT of modules that load seconds later and were never
  patched; `LdrLoadDll` is owned by Steam's overlay and CE's chain-hook refuses overlay modules. RoboCop's D: install
  (updated 2026-08-09 04:07) ships an older Streamline stack and its own `nvngx_dlss.dll`/`nvngx_dlssg.dll`, so the
  effective runtime stayed old and preset M / RR never engaged. Talos ships a newer stack, masking the gap.
- Additional bypass found in the same RoboCop log: its FG plugin loads from the **NGX model repository**
  (`C:\ProgramData\NVIDIA\NGX\models\sl_dlss_g_0\versions\133888\files\1B0_E658703.dll`) - the driver-managed
  Streamline plugin store where every plugin file is literally named `1B0_E658703.dll` inside a per-model folder.
  The hashed base name carries no `sl.*` token, so the base-name redirect cannot match it; it is covered by a new
  model-folder mapping (`sl_dlss_g_0` -> `sl.dlss_g.dll` etc.) in `GetRedirectedPath`.
- Fix (build 0.1.5896): `PreloadConfiguredGraphicsRuntimeDlls()` loads the configured override stack at hook-thread
  start via the original loader entry (name-dedup makes later loads - including Streamline-internal ones - resolve to
  the override copies; already-loaded names are skipped). `PatchLoadLibraryIatForLateLoadedModule()` patches the
  kernel32 LoadLibrary* IAT of modules that load after the snapshot when overrides are configured, so Streamline-
  internal loads and the model-repository loads reach the redirect even without the preload. The
  LdrRegisterDllNotification callback now logs the resolved full path of every runtime-family and model-repository
  module load (`Loader: runtime module loaded: ...`), covering all load mechanisms. Classification and the model
  segment mapping live in `hook/common/graphics_runtime_module_policy.h`; unit tests `GraphicsRuntimeModulePolicy.*`
  in `tests/test_graphics_runtime_module_policy.cpp`.
- Coverage: incremental x64/x86 build + full unit suite + Python tool self-tests pass. Fresh Talos/RoboCop runtime
  validation is pending - the `Loader:` lines in a new RoboCop run must show the NPI paths for sl.interposer,
  sl.common, sl.dlss_g, nvngx_dlss.dll and nvngx_dlssg.dll.

### 2026-08-09 - RoboCop overlay drew one PostSL frame then starved in the confirmed-startup settling window

- Build 0.1.5894 session `installed/captureengine/logs/20260809_144640`: the late-handoff
  fallback worked (`Late-handoff PostSL startup activation - retaining live swapchain ...`,
  `PostSL CONFIRMED rendering`, `Post-SL overlay SUBMIT #1 ... rendered=1`), but after that
  single frame the overlay starved: every later present logged `drawObserved=0`, the warmup
  stall counter climbed past 1000, and `stableFrames` stayed at 1.
- Two root causes, both specific to how NVIDIA Streamline loads in RoboCop:
  1. **The runtime is obfuscated.** The dumps show Streamline's runtime loaded as
     `1B0_E658703.dll` (multiple instances) - a hashed name with none of the `sl.*` path
     tokens. `IsStreamlineModuleHandle` matched only by path, so
     `callerFromStreamlineModule=false` for every runtime-originated Present and the PostSL
     routing could not classify them. (Talos ships a normally-named `sl.dlss_g`, which is why
     it works there.)
  2. **The one-shot top-level bootstrap was never consumed.** The create-time
     fresh-authoritative-handoff arming never ran (origGame was unknown at swapchain create),
     so `streamlineStartupTopLevelPresentConsumed` stayed false. During the 8-frame
     confirmed-startup settling window the overlay is supposed to be drawn by the
     keep-startup normal route, but that route requires
     `streamlineStartupTopLevelPresentConsumed` (or post-FSR proof) - so nothing drew, the
     stable counter never advanced, and settling never ended. Circular starvation.
- Fixes (build 0.1.5895):
  - `IsStreamlineModuleHandle` now falls back to the Streamline plugin API exports
    (`slGetPluginFunction` / `slGetFeatureFunction`) with a small per-module cache, so the
    obfuscated runtime is recognized name-independently.
  - The late-handoff activation fallback now marks
    `streamlineStartupTopLevelPresentConsumed` when it retains the live swapchain, because
    the activation service effectively consumed the one-shot bootstrap. The settling window
    is then covered by the normal keep-startup route and the confirmed-standalone route
    takes over after frame 8.
- Talos impact: the module cache returns the same name-based results for normally-named
  modules, and the bootstrap-consumed arming only runs in the late-handoff fallback (which
  never fires when a retained swapchain exists). Focused and full unit/Python suites pass;
  fresh RoboCop runtime validation is pending.

### 2026-08-09 - RoboCop third crash: proactive scan truncated at 8 candidates, missing the real slot

- Build 0.1.5892 session `installed/captureengine/logs/20260809_143040` still crashed with
  the same RIP=0 signature at `gameoverlayrenderer64!OverlayHookD3D3+0x13e8f` even though the
  proactive patch ran and logged `Steam NULL-callback slot discovery: found 8 candidate
  slot(s)` plus six `patched slot ... -> DXGI bypass` lines. The crash slot (steam+0x167340,
  confirmed by dump disassembly: `mov rax,[rip+0xD348B]; ...; call rax` at 0x13eae/0x13ebd)
  was NOT patched: the candidate cap of 8 truncated the module scan before reaching it,
  because an earlier cluster of six NULL slots (steam+0x1668B0..0x166990) plus two more
  candidates consumed the budget. The VEH backstop also still produced no logs (the
  instrumented early-return paths never fired), so the unpatched slot crashed again.
- Fix: `kSteamNullCallbackMaxSlots` raised 8 -> 256 so the one-time module scan covers the
  whole image (the six-slot cluster at 0x1668B0..0x166990 is NOT the only family; the real
  faulting site at 0x167340 comes later), plus a rate-limited fail-open log when the cap is
  reached so a future truncation is visible. `tests/test_dxgi_shared_part12.cpp` gained
  `SteamNullCallbackScannerDoesNotTruncateAtSmallCap`, which pins the 143040 layout (earlier
  cluster + late faulting slot) and asserts the production cap stays >= 64.
- Product build 0.1.5893 (x64/x86) and the full unit/Python suites pass. Fresh RoboCop
  runtime validation is pending; the expected healthy log is a discovery count >= 9 with a
  patch line for steam+0x167340 (or whatever slot the current Steam build faults on).

### 2026-08-09 - RoboCop still crashed: VEH Steam recovery was shadowed, so NULL callback slots are now patched proactively

- Build 0.1.5891 session `installed/captureengine/logs/20260809_141705` still crashed the
  RHI thread with the identical RIP=0 signature (`gameoverlayrenderer64!OverlayHookD3D3+0x13e8f`
  -> `CallOriginalPresent` -> `DetourPresent` -> Streamline runtime -> game) even though the
  SL fast-path now installed `ScopedSteamNullCallbackRecoveryGuard`. The guard log appeared
  (`Guarded Steam Present hook installed Steam null-callback VEH recovery #1`), but no VEH
  handler log ever followed: the crash-time recovery was shadowed and never patched. The
  `external_sl-sha-68a19a3_*.dmp` is NVIDIA Streamline's own crash dump
  (`C:\ProgramData\NVIDIA\Streamline\...`), i.e. Streamline has its own exception handling in
  the chain; the repeated AVs and the later freeze are all downstream of the unpatched NULL call.
- The faulting slot in this Steam overlay build (gameoverlayrenderer64 2026-08-03) is at RVA
  **0x167340**, loaded by `mov rax,[rip+0xD348B]` immediately before `call rax` at
  `OverlayHookD3D3+0x13ebd` - not the legacy 0x1621d8 fallback. So the slot moves per Steam
  build and must be discovered from the code pattern.
- Fix: proactive NULL-callback slot patching (`hook/common/dxgi_shared_steam.cpp`). Before any
  Steam transport runs, `EnsureSteamNullCallbacksPatched(bypass)` scans the Steam overlay for
  the `48 8B 05 <disp32> ... FF D0` (x64) / `A1|8B 05 <abs32> ... FF D0` (x86) Present-shaped
  callback pattern, caches the slot addresses per module version, and patches any slot whose
  value is NULL or below 0x10000 to CE's DXGI bypass trampoline. It runs at the top of
  `TryInvokeGuardedExternalSteamOverlayPresent`, in the SL fast-path, and in the non-SL E9 path,
  all before Steam's hook is touched. The VEH recovery stays as a backstop and its silent
  early-return paths are now logged for future diagnosis.
- Talos safety: the patch only writes NULL/invalid slots; in working sessions (Talos) Steam's
  callback is already non-NULL so the function is a no-op, and it writes exactly the same bypass
  value the crash-time VEH recovery already uses. The source-thread provenance rule from
  `20260809_015416` is untouched.
- Coverage: `tests/test_dxgi_shared_part12.cpp` pins the pattern scanner (RoboCop x64 pattern,
  out-of-module rejection, missing-call rejection, lead-window/cap behavior, x86 A1/8B05
  patterns) and the wiring order (proactive patch precedes the callback read in the guarded
  helper, and precedes the E9 return in the SL fast-path). Product build 0.1.5892 (x64/x86) and
  the full unit/Python suites pass. Fresh RoboCop and Talos runtime validation is pending.
