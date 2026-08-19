# llm-wiki Log

### 2026-08-19 - DOOM Eternal overlay vanished after 239 frames: the game presents from a compute-only queue

Session `20260819_030710` (build 0.1.6164, the build that fixed the startup hang). The overlay drew for
239 frames and then never again. Fixed in 0.1.6168.

- **What the log says, verbatim.** `Vulkan Layer: Overlay skipped on non-graphics present queue family 2`,
  five times from five different thread ids inside 15 ms, starting `03:07:34.452`. `perf_metrics_8696.csv`
  agrees exactly: `overlay_us` is 47-270 us up to frame 239 and 0-1 us for all 2800 frames after it - the
  early-out, every present, for the rest of the run.
- **Root cause.** idTech 7 presents from queue family 2, which on NVIDIA is compute + transfer with no
  `VK_QUEUE_GRAPHICS_BIT`, once the real render loop starts (the first ~240 frames go out on the graphics
  queue). The overlay is a render pass, so `RenderOverlay` recorded and submitted on whatever queue the
  present arrived on and simply gave up when that queue had no graphics support. Capture and screenshots
  were unaffected: they only need `VK_QUEUE_TRANSFER_BIT`, which family 2 has.
- **Fix: submit the overlay on a graphics queue instead of on the present queue.** Nothing else has to
  change - the overlay already signals a semaphore the rewritten present waits on, and semaphores are
  queue-agnostic. `ResolveOverlaySubmitTarget` picks, in order:
  1. the present queue, when it supports graphics (every title that worked before is bit-for-bit unchanged);
  2. a queue CE reserved for itself in `vkCreateDevice` by asking for one more queue than the game did in a
     graphics family (`BuildOverlayQueueReservation`). CE owns it outright, so the overlay submit runs
     beside the game's graphics work instead of queueing behind it: no serialization, no added latency;
  3. one of the game's own graphics queues, with every submission to that one queue - the game's and CE's -
     serialized through `ScopedBorrowedQueueSubmission`. AMD exposes a single graphics queue, so a game
     there can leave nothing to reserve, and borrowing is what keeps the overlay alive on that hardware.
- **Invariants that make the reservation safe.** CE never asks for more queues than the family exposes
  (that fails `vkCreateDevice` outright), never widens a protected queue-create entry, never requests a
  priority above the highest the game asked for, and falls back to the game's unmodified queue request and
  retries if the driver rejects the create anyway. The reserved index is one past the game's own, so the
  game can never receive CE's queue from `vkGetDeviceQueue`.
- **The borrow lock costs nothing when it is not in use.** `ShouldSerializeQueueSubmission` is one relaxed
  atomic load against a handle that stays `VK_NULL_HANDLE` unless tier 3 actually engaged, and the borrowed
  queue is published before the first borrowed submit so no game submission can race the decision. Lock
  order is always overlay-state -> borrowed-queue; the game's submit path takes only the latter.
- **Also fixed in passing.** The overlay backend's own init/upload queue was the game's graphics queue 0,
  submitted to from the layer with no external synchronization at all. It now prefers CE's reserved queue.
- **Known boundary (unverified).** CE's barriers use `VK_QUEUE_FAMILY_IGNORED`, i.e. no queue-family
  ownership transfer, which is exactly what the game itself does across the same boundary when it renders
  on one family and presents on another. A strict reading of the spec would want a release/acquire pair for
  an EXCLUSIVE swapchain whose last writer is the present family; doing it would cost two extra present-queue
  submits per frame on the critical path. `vkCreateSwapchainKHR` now logs `sharingMode=` so a real run can
  say which case a title is in. If a title ever shows corruption here, the lever is a CONCURRENT swapchain,
  not a per-frame transfer.
- **Open.** Real-game validation on DOOM Eternal (overlay present and stable at 138 fps, no stutter), plus
  a graphics-queue-presenting title to confirm tier 1 is unchanged.

### 2026-08-19 - DOOM Eternal Vulkan startup hang: CE's flip-queue pacing wait blocked NVIDIA's WSI presenter thread

Session `20260819_020933` (build 0.1.6163). A real deadlock, not a freeze-watchdog false positive - the
watchdog was right, and its dump contained the whole answer. Fixed in the same session.

- **The three-thread cycle, straight out of `crash_external_...FREEZE...dmp`.** Thread `0x2164`
  (`nvoglv64` presenter, under `gameoverlayrenderer64`) sat in
  `capture_hook_x64!DXGIShared::WaitBackbufferFrameLatency` -> `WaitForSingleObject(..., INFINITE)` on the
  frame-latency semaphore (handle `0x1d44`, `GrantedAccess=0x100000` - a DXGI waitable object) of swapchain
  `000001B5B77EB710`. Thread `0x5BE0` (window thread) was inside `Capture_vkDestroySwapchainKHR` ->
  `nvoglv64` -> `GetExitCodeThread`, joining exactly that presenter thread (`0x103`/`STILL_ACTIVE` in the
  args). Thread `0x5A90` (render thread) was in `SendMessage` to the window thread. One presented frame in
  `perf_metrics_20848.csv`, then nothing.
- **Why CE was there at all.** NVIDIA's Vulkan ICD implements `VkSwapchainKHR` on a DXGI flip swapchain.
  `hook_debug.log` shows `DetourCreateSwapChainForHwndGlobal ... caller=<DriverStore>/nvoglv64.dll
  BufferCount=2 SwapEffect=4`, CE adding `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT` for the profile's
  `backbuffer_count=2`, and `ApplyPresentFrameLatencyOverrides: SetMaximumFrameLatency(1)` on the ICD's
  chain. Both ran on a swapchain CE does not own.
- **CE had already decided not to do this.** At `02:19:13.785`, 11.5 s before the create:
  `CheckAndInstallHooks: Vulkan layer ownership established, skipping D3D/DXGI hooks` and
  `DXGIShared: Vulkan active (evidence-based), DXGI hooks will pass through`. The "pass-through" was not
  one: `DetourPresent1` calls `ApplyPresentFrameLatencyOverrides` *before* its `IsVulkanActive()` gate, and
  the gate returns through `CallOriginalPresent1`, which calls `WaitBackbufferFrameLatency` on the way out.
  The create/resize descriptor overrides had no such gate at all.
- **The wait was a re-regression.** Commit ccbdeac5 (2026-05-15) fixed this identical freeze by replacing
  `INFINITE` with a 16 ms ceiling; commit dd30a5b6 (2026-07-12) reverted it to `INFINITE` because 16 ms sits
  *below* a healthy wait and silently escaped the pacing whenever the game was GPU- or vblank-bound. Both
  are true, which is why the fix needed both halves.
- **Fix.** New `hook/common/present_pacing_policy.h` + `hook/common/dxgi_shared_present_pacing.cpp`
  (`ResolvePresentFrameLatencyOverride`, `WaitBackbufferFrameLatency`, `ApplyPresentFrameLatencyOverrides`
  moved out of the 778-line `dxgi_shared.cpp`). (1) All four presentation-policy sites now honour
  `IsVulkanActive()`; the create-time rule rides on the existing
  `ce::dx12_overlay_policy::ShouldApplySwapchainDescriptorOverridesForCreate`, which already preserved FG
  runtimes' descriptors byte-for-byte for the same reason. (2) One pacing-wait implementation,
  `DXGIShared::WaitFlipQueuePacingObject`, replaces three copies (shared `INFINITE`,
  `CallOriginalPresent`'s inlined 16 ms, `CWrapDXGISwapChain::WaitFrameLatency`'s `INFINITE`); ceiling
  1000 ms, above the slowest healthy wait and below any freeze, and a miss latches pacing off process-wide.
- **Dump targeting, also fixed.** The watchdog dumped with `targetTid=0` (`Heartbeat()` adopts a render
  thread, `HeartbeatFromHelperThread()` - what the DXGI path calls - deliberately does not), so
  `!analyze -hang` named the idle main thread and the deadlock was invisible until `~*kv`. A present that
  never returned *is* a thread stuck inside CE's hook: `ResolveFreezeDumpTargetThread` +
  `DXGIShared::GetThreadStuckInsideCePresentHook()` now name it.
- **The watchdog itself needed no change.** It armed on a real D3D present, its Vulkan cross-API liveness
  poll correctly found no recent tick, and it fired at 26 s because the alt-tab dropped the process out of
  the foreground while a present was in flight (`inFlightTimeout = min(timeout, 15 s)`). Every step was
  correct.
- **Open.** Real-game DOOM Eternal / Strange Brigade Vulkan launch validation, plus a D3D title with
  `backbuffer_count` set to confirm the pacing still paces.

### 2026-08-19 - Strange Brigade DX12 close crash: our diagnostic probe double-freed OptiScaler's chain

`STATUS_HEAP_CORRUPTION` (`0xC0000374`) on game exit with OptiScaler, Special K, ReShade and the Steam
overlay all injected. Session `sbdx12crashonclose` (build 0.1.6162). Ours, not theirs. Fixed in 0.1.6163.

- **The stack named it exactly.** `StrangeBrigade_DX12` -> `CWrapDXGISwapChain::Release` ->
  `~CWrapDXGISwapChain` (`dxgi_swapchain_wrap_lifetime.cpp:251`) -> `OptiScaler` -> `ucrtbase!free_base`
  -> `RtlFreeHeap` -> `RtlpHeapHandleError`. Line 251 was the `Release()` of the destructor's
  "post-destruction real refcount" probe.
- **Why it was fatal.** `hook_debug.log` shows `Deleting wrapper (real refs=4, wrapper refs=0)`, then the
  four promoted `IDXGISwapChain1..4` releases, then `Skipping real swapchain final wrapper release`. Those
  four *were* the chain's last references (an OptiScaler/ReShade-style proxy's refcount equals exactly
  CE's promoted refs), so the chain died mid-destructor. The probe then ran `AddRef` on the freed proxy —
  which succeeded, because a freed heap block stays `MEM_COMMIT` and keeps a plausible vtable, so neither
  the `VirtualQuery` check nor `ScopedAvGuard` (AV-only) saw anything wrong — and the paired `Release` ran
  OptiScaler's destructor a second time, freeing pointers it had already freed. The
  `post-destruction real refcount=` line never appears in the log: it crashed one call earlier.
- **The comment on `ShouldReleaseRealSwapchainWrapperReferenceDuringWrapperDestructor` already described
  this exact failure** for ReShade (session `20260813_012613`) and fixed the extra *base* release. The
  probe kept doing the same thing to the same corpse.
- **Fix is ownership, not detection.** The destructor takes one diagnostic reference before releasing
  anything (`ShouldHoldRealSwapchainDiagnosticReferenceDuringWrapperDestructor`: only when a promoted
  reference is held or the base release will run, never for the non-retaining Streamline wrapper), runs
  refcount/vtable/attribution diagnostics under it, then drops it last — and that `Release` return value
  is the authoritative residual pin count, recorded in the new `hook/common/swapchain_liveness.h` ledger.
  Nothing dereferences the chain afterwards.
- **Second instance of the same bug, also fixed.** `LogAccessDeniedSwapchainPinDiagnostics` probed the
  `dx12_hook_s_hwndSwapchainMap` pointers the same way. Those are raw by design (pinning is what causes
  the `E_ACCESSDENIED` it diagnoses) and *nothing* removes them when a chain dies, so it was probing
  corpses on the recovery path. It now reads the ledger and never dereferences a tracked pointer;
  `TrackSwapchainHwnd` drops a stale note when a live chain reuses an address.
- **No behaviour change for the overlay.** The chain still dies inside the same destructor, a few
  instructions later; CE takes and returns exactly one extra reference on a thread that already owns one.
- **Not validated on hardware yet** - needs a Strange Brigade DX12 close with the same tool stack.

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

### 2026-08-16 - Display gamma override, and a general "only safe in this engine state" guard

- `[UE5] display_gamma=default|srgb|1.0..3.0` (0.1.6139, shared ABI 42). Motivated by Talos shipping a
  piecewise-sRGB vs power-2.2 option that visibly does nothing - the kind of bug CE can route around, because it
  writes the CVar directly instead of going through the game's settings code.
- Carried as the `r.TonemapperGamma` value itself: negative untouched, 0 is UE's documented "default behavior"
  (piecewise sRGB/Rec709), positive is a pure power curve. One field, one predicate, no separate mode enum.
  Only the sRGB direction writes `r.HDR.Display.OutputDevice` - UE raises the device to explicit-gamma mapping on
  its own once the exponent is positive.
- **New: `ApplyGuard`.** `r.HDR.Display.OutputDevice` doubles as the HDR output selector (3-6 are ST-2084/ScRGB),
  so writing an SDR device over it would silently drop an HDR game out of HDR and ruin the capture. A spec can now
  carry a guard judged from *the value the game currently holds*, checked at every install site before any write.
  General mechanism, not a special case - use it whenever the engine's current state decides whether a write is
  safe. Under HDR the gamma option does nothing rather than degrading anything.
- Types read from the binary again (GPR = int32 for OutputDevice, `xmm2` = float for TonemapperGamma). That check
  has now caught two mistypes and prevented a third; it should be routine for every new spec.
- The ABI-name lockstep noted last time paid off immediately: bumping 41 -> 42 needed
  `SHARED_MEM_BASE_NAME`/`SHARED_MEM_DISCOVERY` moved in the same commit.

### 2026-08-16 - UE5 texture mip bias override, and two traps it walked into

- `[UE5] internal_texture_mip_bias=default|-15.0..15.0` drives `r.MipMapLODBias` (0.1.6138, shared ABI 41).
  Distinct from the `[Graphics]` sampler mip overrides: this changes what the engine asks for, so it also moves
  texture streaming.
- **The CVar is a float, verified not assumed.** The registration passes its default in `xmm2` (`0f57d2`,
  `xorps xmm2,xmm2`) - the float overload; an int default goes in a GPR. Same check should be used for any future
  spec: it is a two-minute scan and it is exactly the bug that made
  `r.Lumen.ScreenProbeGather.Temporal.MaxFramesAccumulated` silently unreachable for its whole life.
- **0 is a real value here**, not "leave alone", unlike every other numeric UE5 knob. The untouched state is a
  sentinel outside UE's -15..15 range; `IsTextureMipBiasRequested` is the one predicate that decides it.
- Two process notes the gate taught: new specs must be **appended** to `kSpecs` (`kTonemapperSharpenIndex` is
  positional), and an ABI bump must move `SHARED_MEM_BASE_NAME`/`SHARED_MEM_DISCOVERY` too - those literals are
  hardcoded while the event names are derived, so a partial bump compiles fine and only the name test catches it.

### 2026-08-16 - Show flag force bits: one bit proven safe on two engine versions, effect still unproven

- **Handoff note.** The `ShowFlag.*` force bits are the only open part of the UE5 override work. The full state,
  measured bit maps for 5.4.4 and 5.6.1, what is proven, and the ordered next steps now live in one block in
  `graphics-overrides-and-frame-pacing.md` ("`ShowFlag.*` force bits - state of knowledge"). Read that before
  touching it rather than reconstructing it from these entries.
- 0.1.6134 drives exactly **one** bit, `ShowFlag.Vignette`, recorded and restored, compare-exchange on the shared
  word. Validated on Talos 5.4.4 (`20260816_205827`, 32/32 verified) and Industria 2 Demo 5.6.1
  (`20260816_210946`, 34/34 verified after the game's own load-time writes were re-asserted). **No visual
  regression on either.** So the four-bit write that killed lighting in 0.1.6128 was not a uniform off-by-one -
  bit 13 alone is harmless, and the culprit is one of Grain/MotionBlur/SceneColorFringe.
- Still unproven: whether the write *disables* vignette. Neither title visibly uses vignette or grain, so
  "inert" and "correct but invisible" look identical. Needs a title that shows the effect.
- Two findings that close off the alternatives: UE ships **no vignette CVar at all** (whole-binary literal
  enumeration - only the show flag name and its localization key), and `r.Tonemapper.Quality` does remove vignette
  but only as part of a cumulative ladder that takes grain and the rest with it. **User decision: tonemapper
  quality must not be reduced for this.** A test already asserts the spec table never contains it; that assertion
  is load-bearing, not incidental.
- Also corrected: the masks are read through the console object's own pointer, never RIP-relative, which is why a
  full `.text` scan finds no apply site. An earlier reading of that absence as "compiled out of Shipping" was wrong.
- Cross-version data worth keeping: mask geometry and bit indices both differ by engine version (48 B/384 flags in
  5.4.4 vs 64 B/512 in 5.6.1; GI 12 vs 14, Vignette 13 vs 15). CE reads both per object, so nothing assumes them.

### 2026-08-16 - The bit map measurement lands, and the last unreachable CVar turns out to be mistyped

- `20260816_201740` (0.1.6131, Talos): lighting is back, 31/40 installed, **verified 31/31, re-asserted 0,
  retired 0**. The four show flags are recognised as force bits and reported, not written.
- `ProbeShowFlagBitNumbers` resolved 7 of 8: **Bloom 1, Tonemapper 3, AntiAliasing 4, TemporalAA 5,
  GlobalIllumination 12, Vignette 13, Grain 14, DirectLighting 28, MotionBlur 37, SceneColorFringe 46,
  Lighting 109** (`ShowFlag.DiffuseIndirect` is not registered in Talos). GI at 12 is exactly what the binary's
  name table predicted, one below Vignette's 13.
- That settles the direction of the 0.1.6128 defect: **the console-object side is completely self-consistent** -
  distinct indices, matching the engine's own declaration order. CE set bits 13/14/37/46 and the renderer dropped
  global illumination (12), so the discrepancy is entirely on the renderer's side of the mask, not in what CE
  read. Still not enough to write by; what is missing now is how the renderer indexes the mask, not what the
  objects say.
- The rate-limited dump also cracked `r.Lumen.ScreenProbeGather.Temporal.MaxFramesAccumulated`, unreachable in
  Talos across every session: its shadow at `object+0x58` is `0x41C80000` = **25.0f**. The variable is a *float*
  and the spec table declared it `Int32`, so the Int32 plausibility check refused it every time - correctly, since
  installing it would have written 10 (a denormal float, effectively zero) into a float global and turned temporal
  accumulation off. Fixed in 0.1.6132 by typing the spec `Float`; the checks stay fail-closed the other way too,
  because an int-shaped 10 or 25 reinterprets as a denormal and is refused rather than written.
- Lesson worth keeping: a spec-table type is a claim about the engine's registration, and the plausibility check
  is the only thing standing between a wrong claim and a corrupted variable. A CVar that is "found but never
  validated" in one title and fine in another is a type mismatch until proven otherwise.

### 2026-08-16 - Driving the show flag force bits removed all lighting from Talos; CE stops writing them

- 0.1.6128 shipped the bit-ref write. Talos then rendered with no lighting at all (`20260816_165501`), and the
  install lines say why it is not a discovery failure: `force0=00007FF73B3A16C0 force1=00007FF73B3A16F0`, exactly
  0x30 apart - two adjacent 48-byte (384-flag) bit arrays, identical across all four flags. The bit numbers are
  self-consistent too: Vignette=13, Grain=14, MotionBlur=37, SceneColorFringe=46, all distinct.
- The engine's own show flag name table in the Talos binary (`0x810d0e0` `GlobalIllumination`, `0x810d108`
  `Vignette`, `0x810d120` `Grain`) lists them in that exact order, so **`GlobalIllumination` is bit 12 - one below
  the first bit CE set**. Any off-by-one between the index a console object carries and the index the renderer
  reads the mask by lands precisely on global illumination, which is the observed symptom.
- So the masks *are* live in a Shipping build (that standing open question is now answered), the discovery is
  right, and the **mapping** is what is unproven - plausibly `SHOWFLAG_FIXED_IN_SHIPPING` flags compiled out of
  one side and not the other. Writing a bit means guessing which flag gets turned off, and the guess cost a
  broken frame.
- **0.1.6131:** bit references are classified, confirmed against a second flag, and *reported only*. No force bit
  is written, so the verify/restore/update branches for that mode are gone rather than left dead. Classification
  stays - it is what stops the old redirect from replacing the engine's mask pointer, the defect that made these
  overrides inert in the first place.
- Added `ProbeShowFlagBitNumbers`: eight extra show flag names resolved through the already-anchored map purely to
  log `name -> bit` once per session. If `GlobalIllumination` reports 12 next to `Vignette`'s 13, the
  console-object side is exactly the table order and the discrepancy is entirely renderer-side.
- Net effect on the user-visible bundle: unchanged from before 0.1.6128. Grain, motion blur and chromatic
  aberration ride on `r.FilmGrain` / `r.MotionBlurQuality` / `r.SceneColorFringeQuality` (all verified live);
  vignette remains the one post-processing effect with no working lever.

### 2026-08-16 - `ShowFlag.*` was never a value variable: four UE5 overrides had been writing the wrong structure

- Follow-up to the 7-minute Talos run (`20260816_014003`, 35/40 installed, verified 35/35, re-asserted 0). The
  four `ShowFlag.*` entries counted as installed were the ones with no evidence behind them, and the `prevLocal`
  diagnostic added for exactly that question answered it: all four objects reported the **identical**
  `object+0x58` qword `0x00007FF73B3A16F0` (`20260816_161158`), an address inside the exe's writable data.
- A per-variable `{game, render}` shadow pair cannot be identical across four different variables. These are
  `FConsoleVariableBitRef` - UE registers show flags as one bit in two process-wide force masks, so the first two
  qwords are `{Force0MaskPtr, Force1MaskPtr}`. CE's fixed `ref+0x50` model read `*Force0MaskPtr` (an all-clear
  mask, which is where the long-standing `prevValue=0` came from), decided it was a plausible value, and
  CAS-replaced the mask pointer with CE's 8-byte shadow. The engine then read CE's storage as the mask: the write
  reached nothing, and the object lost its route to the real mask. Inert in every title and engine version so far.
- **Fix (0.1.6128):** classify before writing. `hook/common/ue5_console_layout.h` decides between reference
  pointer / inline pair / bit reference from what the caller read, `hook/main_ue5_layout.cpp` does the reading and
  owns the three install modes, and an object matching nothing (or one shape twice) is left untouched with its
  first 0x80 bytes dumped. The reference shape is now accepted only when the shadow pair actually mirrors the
  global the pointer addresses - the check the ShowFlag objects fail.
- Bit writes are compare-exchange on the containing word so neighbouring show flags survive, restore puts back only
  CE's bit, and verification reads both force bits straight out of the engine's masks. Two gates keep the bit path
  off everything else: `ShowFlag.` names only, and commit only once a second flag confirms the same mask pair with
  a different bit index.
- Also fixed: a mid-session configuration change that newly requested CVars could never resolve them, because the
  registry resolver stays closed once it has finished with the previous request set (`ReopenConsoleRegistry`).
- **Open:** whether a Shipping build consults the force masks at all (these flags are `SHOWFLAG_FIXED_IN_SHIPPING`
  in UE). Practically that only exposes vignette. Also open: `r.Lumen.ScreenProbeGather.Temporal.MaxFramesAccumulated`
  in Talos, which now refuses with a byte dump instead of a bare "layout does not match" line - the next Talos run
  should identify it outright.
