# llm-wiki Log Archive - 2026-08-19

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
