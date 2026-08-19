# llm-wiki Log Archive - 2026-08-17 .. 2026-08-18

### 2026-08-18 - Vulkan late inject, part 2: the resident layer was rejected on build number

Resident registration (0.1.6156) put the layer back into the process, and late inject still showed no
overlay. Session `20260818_231619`. Second root cause, fixed in 0.1.6162.

- **Same symptom, different cause.** Again no `vulkan_layer*.log`. But this time the layer *was*
  loaded: the 23:07 build reported `VK_LAYER_CE_overlay.dll is locked by another process` and renamed
  it, so a live Vulkan process held the 0.1.6154 image. The game had been started against an older
  CaptureEngine and kept that layer mapped.
- **Why it stayed dormant.** `ValidateDiscoveryInfo` required `GetBuildNumber() == GetCurrentBuildNumber()`.
  A resident layer from any other build therefore failed discovery validation outright - and because
  `IsLayerProcessWhitelistedByCurrentHost()` *and* `IsLayerDebugLoggingEnabled()` both gate on that
  same call, it could not read the whitelist, could not reach the host, and could not resolve
  `logsPath` to log a single line. Silent, and indistinguishable from "the layer was never loaded".
- **Reproduced deliberately** before fixing: started `vulkan_test.exe` against layer 6156, rebuilt CE
  to 6157, started CE. Hook injected, `perf_metrics` written, `DX11 check #1: vulkan=0`, and no
  `vulkan_layer.log` - the user's exact signature.
- **Fix.** `DiscoveryInfo` now carries `abiSignature` at offset 12 and validation checks the compiled
  layout instead of build identity; `buildNumber` is diagnostics only. The first 16 bytes are a
  cross-build contract with asserted offsets, and `ComputeSharedMemoryAbiSignature` now covers
  `sizeof(DiscoveryInfo)` plus the `processWhitelist`/`logsPath` offsets, so a reader that trusts the
  signature may safely parse the rest. `SHARED_MEMORY_VERSION` 42 -> 43 renames every mapping and
  event, which is what stops an already-running older inject process from handing us its smaller
  discovery section. **Consequence for future work: a semantic change to these shared fields must now
  bump `SHARED_MEMORY_VERSION`; the build number no longer separates builds.**
- **Silent failure is now impossible.** `LayerReportIncompatibleDiscovery` writes one line to
  `<layer dll dir>\logs\vulkan_layer_incompatible.log` naming both builds and both signatures when the
  magic matches but the layout does not. It is the only diagnostic that can run in that state.
- **Validated on hardware.** Layer build 6160 resident in a running Vulkan app, CE host 6161 started
  afterwards: `Layer IPC: Connected to Host PID`, `Vulkan layer reactivated`,
  `[InjectLifecycle] Late-initialized Vulkan swapchain`, `RenderOverlay: BeginFrame 3840x2160`,
  hook-side `vulkan=1` and `Vulkan layer ownership established`. Zero layer errors, and no
  incompatibility report (layouts matched, as they should across a plain rebuild).

### 2026-08-18 - Vulkan late inject never had a layer: the registration was ephemeral

"Late inject does not work in Strange Brigade Vulkan, our overlay does not appear." Session
`20260818_224257` (build 0.1.6154). Root cause found and fixed in 0.1.6156, validated end to end.

- **Proof the layer was simply absent.** The failing session directory has `captureengine.log`,
  `hook_debug.log`, `inject.log` - and no `vulkan_layer.log`, no `vulkan_layer_early.log`. Every
  working (early-inject) Strange Brigade Vulkan session has all three: `20260818_224029`,
  `sbvkfreeze`, `sbvkfalsefreezealert`. The hook DLL injected fine and installed DXGI/D3D12/D3D9/
  OpenGL hooks; `DX11 check #N` reported `vulkan=0` on every poll and `renderLoopObserved=0`.
- **Why.** `ScopedVulkanRegistration` (then in `main_internal.h`, commented "Ephemeral Registration
  (RAII)") registered the implicit layer at controller startup and unregistered it at shutdown. The
  Vulkan loader builds a process's layer chain once, inside `vkCreateInstance`. A title launched
  while CE was not running therefore never got `VK_LAYER_CE_overlay.dll` into its chain, and nothing
  the injector does afterwards can add one - CE's Vulkan present/overlay path lives in that layer
  DLL, not in the hook. Vulkan late injection was structurally impossible, not merely broken.
- **Fix.** `captureengine/main_vulkan_residency.h` (split out of `main_internal.h`, which hit the
  800-line ceiling) makes the registration resident: register + self-heal at startup, never
  unregister. `RepairOwnedRegistrations` now prunes only superseded CE entries and retains the live
  ones, removing the old delete-then-rewrite window during which a starting title would miss the
  layer. `SelectStaleOwnedEntries` is the pure, tested policy; foreign layers (Steam/OBS/RTSS/EOS)
  are never eligible.
- **Second bug, found while tracing the wake path.** `CheckAndInstallHooks` latched its Vulkan
  decision permanently (`!s_checkedForVulkan || s_vulkanActive`). The resident layer wakes only after
  CE signals its reactivation event, which lands after the hook thread weighed D3D evidence - and
  Strange Brigade Vulkan has `d3d12.dll` loaded, so the latch would stick at "not Vulkan" and the
  DXGI present path would fight the layer for the whole process. The gate now also re-opens on
  `vulkanLayerOwned`. Ownership only: re-opening on `vulkan-1.dll` presence would bring back the
  RoboCop DX12 regression.
- **Validated on hardware.** With zero CaptureEngine processes running, `vulkan_test.exe` loads
  `VK_LAYER_CE_overlay.dll` (impossible before the fix). Starting CE second: `vulkan_layer.log`
  appears, `Layer IPC: Connected to Host PID`, `Vulkan layer reactivated`, `InitializeOverlay`
  completes, and `RenderOverlay BeginFrame 3840x2160` runs per present. Hook side reports
  `vulkan=1` and `Vulkan layer ownership established, skipping D3D/DXGI hooks`. Zero layer errors.
- **Known remaining boundary (unchanged by this fix).** A device created while the layer was in
  passthrough has `captureInteropEnabled=false`, so *inject video capture* after a late wake still
  needs device/swapchain recreation; the overlay does not. Broadening external-memory extension
  injection to every Vulkan device system-wide was deliberately not done here - that is a
  much larger blast radius against the DLSS-FG/FSR-FG constraints.

### 2026-08-18 - The freeze dump was the freeze: Strange Brigade Vulkan

Reported as "Strange Brigade Vulkan randomly froze on start (it usually works)". It is CE's bug, and the
freeze detection was the bug - the game never froze. Session `20260818_190149` (build 0.1.6146), fixed in
0.1.6151.

- **Proof it was a false positive.** `perf_metrics_18680.csv` frame 3232 sits at `qpc_us=153871037` with
  `qpc_delta_us=6943` (144 FPS); the watchdog's own dump line reports `nowMs=153875`. The largest inter-frame
  gap in the entire 23.7 s session is 655 ms, at frame 3, during startup. The game was rendering normally
  4 ms before the dump started, and what the user experienced as the freeze was the dump.
- **Why it fired.** `DX12Hook::Init()` runs on the hook-install worker thread and armed the watchdog with
  `SetMonitoredThread(GetCurrentThreadId())` - tid 0x1304, a thread that installs hooks and never presents.
  The heartbeat is fed only from CE's D3D/DXGI present paths, and this game presents through the CE Vulkan
  layer, so it never moved: `elapsed=0.7s / 10.7s / 20.7s` then `Render thread frozen for 30 seconds`, exactly
  30.0 s after `FreezeWatchdog: Started`. The timer was measuring "CE never saw a present".
- **Why the Vulkan escape hatch did not save it.** `IsFrozen()` bailed out for `vulkan-1.dll` loaded **and**
  `d3d12.dll` **not** loaded. `d3d12.dll` is loaded in this process (`skipReason=d3d12.dll (DX12 game)` at
  19:03:28) - CE's own DX12 interop pulls it in for the layer's shared fence. A module-presence guess cannot
  answer "who renders here".
- **Why it looks random.** `CheckAndInstallHooks` latches the renderer decision on its first evaluation and
  `if (!s_checkedForVulkan || s_vulkanActive)` never revisits it once D3D is chosen. Whether `vulkan-1.dll` or
  `d3d12.dll` wins that race at injection time decides whether the watchdog is ever started. The latch itself
  is left alone: re-evaluating it is a hook-installation policy change with real overlay-regression risk, and
  the watchdog has to be correct regardless of which branch it lands in.
- **Fix 1 - evidence before accusation.** `ShouldAssertRenderThreadFreeze(renderLoopObserved, presentInFlight,
  forceMonitor, runtimePresentationMonitor)`: no timeout dump until CE has actually observed the render loop.
  Coverage for hangs that start before the first heartbeat is kept by the three no-heartbeat signals (a Present
  stuck in CE's hook, a removed device, an FG runtime owning presentation), and the dialog detector and explicit
  `RequestImmediateDump` sites are untouched.
- **Fix 2 - one liveness currency.** The Vulkan layer lives in another DLL and cannot call `Heartbeat()`, so it
  publishes `vulkanPresentTick` / `vulkanPresentThreadId`. `PollCrossApiPresentLiveness()` folds a present newer
  than 2 s into the ordinary heartbeat on each 500 ms poll, and claims the dump's target thread only while no
  D3D path owns it (DXVK presents through both). Elapsed time, the freeze gate and the dump target now mean the
  same thing on either API - and a Vulkan-only title gets real freeze detection, which it never had.
- **Fix 3 - the dump must not be the freeze.** The watchdog wrote an in-process `MiniDumpWriteDump` with every
  thread suspended while dbghelp reads each module's version resource through the Steam overlay's hooks. The
  crash worker already refuses that (20260817_052857: 61.6 s per `MiniDumpNormal`); the freeze path had not been
  taught the same lesson. It now takes the external helper first via `ShouldPreferExternalCrashDumpHelper` and
  skips the in-process fallback under a foreign overlay. The user killed the process mid-dump, which is why only
  a 0-byte `.dmp.inprogress` survives.
- **Fix 4 - stop naming the wrong thread.** `Init()` no longer claims the worker thread. `Heartbeat()` adopts its
  caller as the render thread, but only while no FG runtime owns presentation, so a Streamline/FFX presenter
  worker is never mistaken for the game's thread; DX12's provenance-checked source Present still outranks it.
  `expectedPresentThreadId` consumers read 0 (permissive) instead of a wrong tid until then.
- **The layer's evidence expires with the layer.** A latched "Vulkan presents were observed" flag would have
  reintroduced the same bug from the other end: the layer is another DLL, and a detach or IPC drop stops the
  tick in a way that is indistinguishable from a frozen Vulkan loop. `HasLiveRenderLoopEvidence` re-reads
  `vulkanLayerActive` on every check, so only a D3D heartbeat - proved from inside the hook module - is
  permanent.
- Tests: `FreezeWatchdogPolicyTest.NeverAssertsAFreezeBeforeAnyPresentWasObserved`,
  `PresentingEvidenceWithoutHeartbeatStillAllowsFreezeAssertions`, `CrossApiPresentLivenessSuppressesOnlyWhileFresh`,
  `HeartbeatsArmFreezeAssertionsAndAdoptTheRenderThread`, `VulkanLayerEvidenceExpiresWithTheLayer`,
  `FreezeDumpPrefersTheExternalHelperUnderAForeignOverlay`.
- Open: no hardware run yet on either a Vulkan title (the new detection path) or a DX12 title (that freeze
  detection still fires normally). The watchdog is still only *started* from `DX12Hook::Init()`, so a Vulkan game
  that wins the renderer-latch race has no watchdog at all - inconsistent, not harmful, and untouched here.

### 2026-08-18 - Screenshots on the present thread: a hard freeze, then the overlay in an overlay-free shot

Two defects on the same path, both in Gothic 1 Remake under DLSS MSFG 4x. Fixed in 0.1.6150.

- **The freeze** (session `20260818_172155`, `screenshot_include_overlay=false`). `SaveDX12TextureAsScreenshotRaw`
  ended in `WaitForSingleObject(fenceEvent, INFINITE)` after submitting the backbuffer copy. That runs on the
  present path, and under DLSS FG the copy goes on the game's own swapchain queue - which the runtime only drains
  once its presenter thread makes progress, and that thread cannot progress while the present call it drives sits
  blocked in our hook. Render thread wedged at 17:23:34.4, watchdog fired 30 s later. The same wait had already
  been visible as a **1.8 s stall** on the earlier successful shot at 17:22:58; it just had not closed the ring yet.
  Exactly the boundary the FFX work recorded in 2026-07 ("never do blocking work on the presenter thread"), DLSS-G
  edition.
- **Fix:** record, submit, `Signal`, hand the resources to the screenshot worker, return. The worker waits in
  1 s slices, checks `GetDeviceRemovedReason()` between them so a dead device cannot park it, and **strands rather
  than releases** resources a live GPU may still be reading. The producer reserves the worker's single slot
  *before* recording any GPU work, so a submitted copy can never be stranded by a busy queue - the only other exits
  would be the blocking wait we just removed or a use-after-free. Worker queue and task moved to
  `hook/common/screenshot_worker.{h,cpp}`; the Vulkan layer links an explicit source list and needed it added.
- **Then the overlay showed up anyway** (session `20260818_205615`, build 0.1.6148: `Saved (hook)`, no freeze, no
  GDI fallback - but the overlay was in the picture). The overlay-free capture sat at the top of
  `DX12_ProcessFrameExternal`, which is early enough only when `ProcessFrame` draws the overlay. Under FG it does
  not: `postSLCallback=1 postSLActive=1 skip=1`, and `DetourPresent` runs `ExecuteStartupRouting` - **which invokes
  the PostSL overlay draw** - before `ExecutePresentCore` reaches `ProcessFrame`. So the "pre-overlay" capture was
  post-overlay.
- **Fix:** whoever draws the overlay owns the ordering for both screenshot variants. `PostSLOwnsThisFramesOverlayDraw()`
  (renamed from `ShouldUseConfirmedPostSLForOverlayIncludedWork` - it was never only about the included case) now
  routes *both* into the PostSL submit chunk: overlay-free immediately ahead of the overlay list on the same queue,
  overlay-included after it. `ProcessFrame` yields both when that predicate holds.
- The chunk has **sixteen mutually exclusive submit branches** across four possible queues, so the capture is
  inserted before each rather than hoisted (hoisting would either duplicate the branch selection or guess the
  queue). `ScreenshotPresentThreadPolicyTest.EveryPostSLOverlaySubmitCopiesTheOverlayFreeFrameFirst` parses the
  file and fails if any submit is not immediately preceded by the capture call *for that same queue*, which is what
  keeps the arrangement from drifting.
- Unrelated diagnostics gap found on the way: `UpdateSharedMemoryFromConfig`'s summary hash omitted every overlay
  boolean, so flipping `screenshot_include_overlay` live published a new config and logged **nothing**. The
  publication line is the only record that a setting reached the hook; the four booleans are in the hash now.

### 2026-08-18 - One overlay-toggle press wiped the active profile: UE5, DLSS and graphics overrides all died

Session `20260818_164520` (Gothic 1 Remake, `G1R-Win64-Shipping.exe`, DLSS MFG 4x, `[Profile.gothicremake]`).
Fixed in 0.1.6144.

- **Symptom.** `16:46:22.062 [Controller] Overlay toggle hotkey handled`, then 84 ms later the hook restored
  all 35 installed CVar shadows with reason `configuration disabled`. The same instant, Streamline stopped
  overriding DLSS-G: `generatedFrames=1->3` every present up to `16:46:22.057`, then
  `slDLSSGSetOptions ... generated=1->1 ... override=0`. Not two bugs — one.
- **Root cause.** `inject_main.cpp`'s `ProcessCommand::ToggleOverlay` handler flipped
  `currentConfig.overlay.showOverlay` and republished `currentConfig` — the **base** config, with no
  `[Profile.*]` applied. `inject.log` shows it exactly: the publication at 16:45:27.981 carried
  `vsync=fifo cpuPrerender=1.00 srPreset=13 forceRR=1 ue5InternalAF=16 ue5InternalTextureMipBias=-2.00`; the one
  at 16:46:22.062 carried `vsync=default cpuPrerender=-1.00 srPreset=0 forceRR=0 ue5InternalAF=0`. `dlss_fg_factor=4x`
  went with it, which is why MFG dropped to 1x. The `ReloadConfig` handler already resolved the target
  (`ResolveActiveTargetConfig`); the toggle handler simply never did.
- **Fix.** The toggle is now a runtime `OverlayVisibilityOverride` carried *beside* the config instead of an edit
  of it, and **every** publication site (startup, injection callback, hook-source detection, reload, toggle) goes
  through one `PublishConfigLocked`, so `UpdateSharedMemoryFromConfig` is called from exactly one place — a
  structural invariant `ProcessIPCTest.OverlayToggleHotkeyIsWiredEndToEnd` now asserts by counting call sites.
- Two defects fell out of the same handler and are fixed with it. The press flips the **resolved** visibility, so
  a profile overriding `[Overlay] enabled` can no longer make the first press a no-op; and the override now
  survives republication, so an injection or hook-source event cannot silently snap the overlay back on. A config
  reload still clears it — the file is the declared state again.
- **A real race, not a theoretical one.** `InjectionManager::Inject` runs the `onInject` callback on a delayed
  injection worker thread, and that callback published too. Two threads entering `BeginWriteOverlayConfig` leaves
  the sequence even while one is still writing, so the hook could accept a torn `overlayConfig`; the shared
  `configSummaryHash` static was racy as well. One publication mutex makes the seqlock genuinely single-writer.
- Target identity for resolution: the last target the injector/hook-source path identified wins, with the live
  `GetSourcePid()` name as fallback and `ClearStaleHookSourceState` clearing it when the process dies. That
  closes the window between injection and the hook publishing its source PID, where a toggle used to fall back
  to the base config even with a target running.
- Publication state lives in a function-local-static `PublicationState`, not namespace-scope globals:
  `AppConfig`'s defaults allocate, and `bugprone-throwing-static-initialization` (correctly) fails lint on that.

### 2026-08-17 - Wukong exit crash: three separate bugs behind one "crashes on close"

Session `20260817_052857` (Black Myth: Wukong, DLSS FG, Steam overlay loaded). Fixed in 0.1.6143.

- **The crash: a use-after-free of `g_pLocalConfig` during `LdrShutdownProcess`.** `crash.log` names the address;
  it resolves to `GetRedirectedPath+0x282`, `movzx eax,[r12+4B0h]` where `r12 = g_pLocalConfig` — and
  `AppConfig::graphics` (+0x180) `+ GraphicsConfig::streamlineDllPath` (+0x330) is exactly 0x4B0, i.e. the
  `.empty()` on line 208 of `main_redirect.cpp`. So the pointer, not the string, was dead.
- Why it was dead: the exit path took `DllMain(DETACH, lpReserved != NULL)`, which set only `g_ProcessTerminating`
  and returned. `RequestHookShutdown()` was called **only** on the dynamic-unload branch, so `HookIsShuttingDown()`
  stayed false for the rest of teardown even though the five loader hooks already had the guard. Our own static
  destructors then ran (`PerfLogger: Shutdown` in `hook_debug.log` is the timestamp for that), and ~1 s later
  something still loading DLLs from its own detach path came through `HookedLoadLibraryEx*`.
- Fix, three layers: latch `RequestHookShutdown()` in the process-exit detach branch **and** in the attach-time
  `atexit` handler (LIFO puts it ahead of this module's globals, so ordering between the two is irrelevant), and
  give the config module storage that is never destroyed. The general rule: **anything CE owns for the process
  lifetime must outlive static destruction**, because a pinned image's hooks stay callable after the CRT is done.
- **The 2-minute hang: a benign exception classified as a crash.** CEF's exit-time
  `WTSUnRegisterSessionNotification` cancels an async RPC wait and rpcrt4 raises `RPC_S_CALL_CANCELLED`
  (`0x0000071A`). The VEH's "dump everything not known-benign" policy dumped it — twice, ~62 s each — and then
  had no dump budget left for the real AV. The filter now classifies by NTSTATUS severity: below error severity
  only the explicitly listed codes (breakpoint, UE5 `ensure`, the COM/DXGI set with their existing thresholds)
  are dump-worthy. Nothing is lost — an unhandled exception still re-enters via the top-level filter with
  `forceDump`.
- **Why a `MiniDumpNormal` took 61.6 s at all** (twice, to the millisecond — a fixed cost, not a lock): dbghelp
  reads every module's version resource, and with the Steam overlay loaded each query round-trips through its
  loader hooks while every other thread is suspended. This is the same hazard `crash_dump_policy.h` already
  documented for the fatal-exit path (`20260813_222058`) — it just had never been applied to the VEH worker. The
  worker now prefers the external `captureengine.exe --dump-helper` process and refuses the in-process fallback
  when a foreign overlay is loaded; the hook publishes both via `RegisterCrashDumpEnvironmentHooks`.
- Worth remembering for future dump reading: `GetExitCodeProcess` stops returning `STILL_ACTIVE` as soon as
  `RtlExitUserProcess` starts, so `[Inject] Tracked injected process exited ... exit=0x00000000` can be logged
  while the main thread is still running DLL detach — and an AV timestamped *after* it is not a contradiction.
- **Validated** on `20260817_055930` (user-confirmed clean exit): no dumps, no `0x0000071A`, no teardown freeze.
- Follow-up that turned out **not** to be a CE bug: Steam's overlay never drew in that session. It is missing
  without CE injected too — Steam ships the Wukong *Benchmark Tool* as a Tool, and the overlay is disabled for
  those. `gameoverlayrenderer64.dll` still loads (so `steam_overlay_loaded=1` and the leave-the-entry mode engages
  exactly as designed), which makes the state look identical to the Cyberpunk `20260816_154722` regression. The
  distinguishing evidence is the same in both, so it cannot separate them: `foreignJumpVisibleNow=0`,
  `g_externalOverlayHook=0`, per-frame `cmdLists=2` (game + CE, no third submitter). CE's own topology was
  healthy — entry left pristine, deep body hooks on `Present`/`Present1` at +14,
  `[OVERLAY LAYER] ... BELOW the foreign Present chain`, and the pre-fix session `20260817_052857` logged byte-for-byte
  the same decisions. **Before treating "Steam's overlay is missing" as a coexistence regression, check the app
  without CE injected first.**
