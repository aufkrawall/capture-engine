# llm-wiki Log Archive

### 2026-08-11 - Late inject must hook already-loaded Streamline feature exports (slDLSSGSetOptions) — Talos 4x still reported as 2x

- Session `logs/20260811_230524` (0.1.5924): the MultiFrameCount parameter
  read still latched `DLSS FG multiplier 0 -> 2`. The game's own log proves
  the 4x is conveyed by `slDLSSGSetOptions(numFramesToGenerate=3)`, NOT by any
  CreateFeature parameter: `ParseNGXParametersCreateTime` prints only
  `UserInterfaceRecompositionEnabled`, and `DLSS-G interpolation state changed
  ... numFramesToGenerate=3` fires on toggle edges without a CreateFeature.
  The game resolved slDLSSGSetOptions once at startup (before injection) and
  never re-resolves it, so CE's slGetFeatureFunction hook can never wrap it.
- Root cause: the startup path hooks feature exports when the app resolves
  them through slGetFeatureFunction (or at slSetD3DDevice); under late inject
  both already happened pre-injection, so `slDLSSGSetOptions`/Reflex exports
  stay unhooked and the whole Streamline FG state machine (multiplier,
  `g_StreamlineFGRunning`, Reflex signals) is dead.
- Fix (0.1.5925): `ScanLoadedStreamlineModules()` now proactively calls
  `TryResolveDLSSGFeatureHooks()` + `TryResolveReflexFeatureHooks()` after the
  loaded-module scan (runtime stable, no loader lock - the same safe point the
  startup path uses for its deferred lookup). The game's cached
  slDLSSGSetOptions pointer is then inline-hooked, so the next FG resume
  flows through `Hooked_slDLSSGSetOptions` -> `ApplyCombinedDLSSFGState` ->
  `SetDLSSFGMultiplier(4)` and `g_StreamlineFGRunning=true`. The NVNGX
  parameter reads from 0.1.5924 stay as secondary coverage for games that do
  set the param.
- FPS/latency audit (unchanged from 0.1.5924): perf CSV shows the game
  genuinely runs 4x MFG under CE (~130 fps output, clean 1+3 cadence); base
  ~26 fps means ~38 ms real-frame latency inherent to 4x MFG. Reflex is now
  also observable under late inject (0.1.5925 hooks sl.reflex exports).
- Regression tests: `LoadedModuleScanResolvesFeatureHooksAfterHookingModules`
  in `tests/test_streamline_runtime_policy_part2.cpp`;
  `ResolvesDLSSFrameGenerationMultiplierFromParameter` /
  `CreateFeatureFGBranchesResolveTheMultiplierParameter` in
  `tests/test_ngx_feature_lifecycle.cpp`.
- Source anchors: `hook/apis/streamline_hook_install.cpp`,
  `hook/apis/streamline_hook_resolve.cpp`, `hook/apis/nvngx_hook_feature.cpp`.

### 2026-08-11 - Late-inject DLSS FG resume crash: route overlay to the swapchain-owning queue (20260811_221202)

- Session `installed/captureengine/logs/20260811_221202` (build 0.1.5921)
  still crashed with the identical UE fatal (`Streamline/DLSSG present failed
  ... Reason: 887A002B`) although the 0.1.5921 dedicated-queue guard was in
  effect and logged `Dedicated overlay queue disabled for NVIDIA DLSS FG`.
  That proved the dedicated queue was NOT the (only) trigger: the submit
  went to `gameQ=1` on queue `000001958621A0C0` and still removed the device.
- Corrected root cause: `0x887A002B` is `DXGI_ERROR_ACCESS_DENIED` (verified
  against the Windows SDK `winerror.h`) - the backbuffer may only be drawn
  from the swapchain-owning queue. Talos uses separate render/present queues;
  at DLSS-FG resume the game's ECL traffic moves to the DLSS-G render queue
  (`g_CommandQueue` flips away from `origGame`), and with the Streamline latch
  missing under late injection, `DecideSwapchainOverlayRouting` fell through
  to the generic fallback (`scQueue ?: last ECL queue`) and submitted the
  overlay's backbuffer-drawing list on the render queue. The SL-latched
  healthy path routes pure DLSS to `origGame` (`kUseStreamlineOriginalQueue`).
- Fix (build 0.1.5922): `DecideSwapchainOverlayRouting` gained
  `plannerDLSSFGActive`; `IsDLSSFrameGenerationActive()` (planner
  `kDLSSFG`) is passed by both call sites (`dx12_hook_process_session_phase2.cpp`,
  `dx12_hook_overlay.cpp`) and the two Streamline branches treat it exactly
  like the SL latch, so the late-inject resume draws the overlay on
  `origGame` (swapchain owner) instead of the DLSS-G render queue. Non-FG,
  FSR, and SL-latched DLSS routing is unchanged (the parameter defaults to
  false and all other branches are untouched).
- Regression tests: `DX12SwapchainOverlayRoutingTreatsPlannerDLSSLikeStreamlineLatch`
  in `tests/test_dxgi_shared_part3.cpp`; the 0.1.5921 dedicated-queue tests
  (`tests/test_dxgi_shared_part14.cpp`) remain as defense-in-depth.
- Source anchors: `hook/common/dx12_overlay_policy/ffx_routing.h`,
  `hook/apis/dx12_hook_fg_heuristics.cpp`, `hook/apis/dx12_hook_process_session_phase2.cpp`,
  `hook/apis/dx12_hook_overlay.cpp`.

### 2026-08-11 - Fix late-inject DLSS FG resume device removal (Talos Alt+Tab crash 20260811_214252)

- Session `installed/captureengine/logs/20260811_214252` (build 0.1.5919):
  late-injected Talos (DLSS FG suspended), Alt+Tab back into the game resumed
  DLSS FG and UE5 fatal-exited (`STATUS_FATAL_APP_EXIT`). The UE log shows
  `Streamline/DLSSG present failed ... DXGI_ERROR_DEVICE_REMOVED with Reason:
  887A002B` right after `Engaging WAR4639162`; the crash dump stack ends in
  `sl.dlss_g` calling `RaiseException`.
- Root cause: with late injection, `sl.dlssg`/`sl.interposer` were already
  loaded before hook installation, so CE missed the Streamline FG signal and
  the runtime-ownership latch (`slFG=0`, `ownership=0`). The FG planner still
  classified DLSS_FG via the NVNGX `CreateFeature` hook, and at FG resume
  `EnsureDedicatedOverlayQueueForFGCompat` forced a sync reinit that created
  the dedicated overlay queue. The warm overlay backend's normal-route command
  list draws DIRECTLY to the swapchain backbuffer; the first such submit on the
  dedicated (non-owning) queue returns `DXGI_ERROR_ACCESS_DENIED (0x887A002B)`
  and removes the device - the documented `20260606_153428` failure mode, now
  reachable through the planner-only DLSS state.
- Fix (build 0.1.5921): `ShouldDisableDedicatedOverlayQueueForNvidiaFrameGeneration`
  disables the dedicated queue for NVIDIA DLSS FG in every detection state
  (Streamline latch OR planner `kDLSSFG`), so the FG-resume reinit stays
  single-queue on the live present queue like the healthy startup sessions.
  Defense in depth: `ShouldUseDedicatedQueueForOverlaySubmit` keeps the
  dedicated queue reserved for pure-offscreen lists; the two ProcessFrame
  submit sites (`DrawSubmitCoreTail`, `SubmitOverlayCommandList`) pass
  whether the recorded list touches the backbuffer and fall back to the game
  queue. Non-FG games are untouched (`actualFGActive=false` already disabled
  the queue); FSR and healthy DLSS paths were already disabled via the
  runtime-owned/Streamline latches.
- Regression tests: `tests/test_dxgi_shared_part14.cpp`
  (`DedicatedOverlayQueueDisabledForNvidiaDLSSFrameGeneration`,
  `DedicatedOverlayQueueSubmitRequiresOffscreenList`,
  `DedicatedOverlayQueueSubmitGuardsBackbufferLists`).
- Source anchors: `hook/apis/dx12_hook_overlay_dedicated_queue.cpp`,
  `hook/apis/dx12_hook_process_session_draw_tail.cpp`,
  `hook/apis/dx12_hook_overlay_render.cpp`,
  `hook/common/dx12_overlay_policy/fg_metrics_and_transitions.h`.
- **SUPERSEDED as the crash fix by 0.1.5922** (see the next entry): the
  dedicated-queue guard was necessary but not sufficient - session
  `20260811_221202` still crashed via the game-queue submit on the DLSS-G
  render queue. The guard stays in place as defense-in-depth.

### 2026-08-11 - Guard the Steam external-chain trampoline transport (DLSS->FSR switch crash 20260811_195131)

- Session `logs/20260811_195131` (build 0.1.5914): Talos starts fine with
  DLSS FG active, but DLSS FG -> FSR FG crashes on the fresh FSR swapchain.
- Root cause: CE's inline-hook trampoline was prepended over Steam's `E9`
  entry jump at `dxgi!Present`; such a trampoline re-issues the foreign entry
  jump, so `CallOriginalPresent`'s bare trampoline fast-path re-entered
  `gameoverlayrenderer64` with no NULL-callback VEH recovery, and Steam's lazy
  NULL rendering callback faulted on the new swapchain.
- Fix (build 0.1.5917): `TrampolineChainsToExternalOverlay` /
  `IsSteamExternalChainTrampoline` detect that transport (E9/FF25 entry,
  matching the preserved external hook target or any target outside dxgi.dll);
  Present routes it through `TryInvokeGuardedExternalSteamOverlayPresent`,
  Present1 and the shutdown path use the clean bypass.
- Regression tests: `DXGISharedSteamTrampolineChainTest` in
  `tests/test_dxgi_shared_part13.cpp` plus the source-order guard test in
  `tests/test_dxgi_shared_part11.cpp`.
- Invariant and source anchors: `dx12-overlay-third-party-coexistence.md`,
  "Build 0.1.5917" section.

### 2026-08-11 - Fix locked-read AV on the read-only DXGI class vftable (crash fallout 20260811_192706)

- Session `logs/20260811_192706` (build 0.1.5914): every CE crash dump plus
  the UE minidump crashes identically at
  `RepairVTableHooksIfNeeded::<lambda0>` — `lock cmpxchg` on
  `dxgi!CDXGISwapChain`'s class vftable inside the read-only dxgi image
  (0xC0000005 AV-WRITE).
- Root cause: commit e9fa1341's CAS refactor observed vtable slots with
  `InterlockedCompareExchangePointer(slot, nullptr, nullptr)`. A `lock cmpxchg`
  is a write even when used as a read, so it faults on the read-only page
  between VirtualProtect windows. Same latent pattern in
  `DetachOwnedVTableSlot` and the Steam phase-A vtable[8] save in
  `CallOriginalPresent`.
- Fix: vtable slot observation is a plain volatile read again; atomic CAS
  writes remain inside the existing VirtualProtect regions (foreign-slot
  preservation semantics unchanged).
- Regression tests: `tests/test_dxgi_shared_part13.cpp`
  (`DXGISharedVTableRepairTest`) runs repair and detach against a
  VirtualAlloc'd fake vtable locked to PAGE_READONLY; pre-fix the suite exits
  0xC0000005, post-fix both tests pass.
- Invariant and source anchors: `dx12-overlay-third-party-coexistence.md`,
  "Build 0.1.5914" section.

### 2026-08-11 - Cross-tool hook coexistence plus late-inject/resident-deject lifecycle

- Compatibility scope now explicitly includes ReShade, OptiScaler, Special K,
  RTSS custom hooks and Microsoft Detours, alongside the established Steam,
  Rockstar, EOS, Discord, Overwolf, Streamline, and FFX paths. Module identity is
  refreshed off the Present thread; the render path reads an atomic registry.
- Inline hooks prepend CE to an existing `E9` or x64 `FF 25` entry and preserve
  the exact foreign target as CE's predecessor. The x64 prepend rewrites only
  the five-byte entry through a near relay, preserving a Detours/RTSS
  trampoline's `target+5` continuation. Inline/deep patch writes suspend
  peer threads, reject instruction pointers inside the patch range, revalidate
  expected bytes, and fail closed. Deep-hook installation no longer exposes an
  INT3 transition window.
- Inline, deep, vtable, IAT, DXGI, input, and specialized temporary hook removal
  is ownership-based. CE restores only its live bytes/pointer; if a later tool
  followed or replaced CE, the foreign entry and CE chain storage remain valid.
  Proxy DLLs used by common graphics injectors are excluded from broad IAT scans.
- Startup injection behavior is retained. The startup scan also queues already
  running whitelisted DirectX/OpenGL targets. The globally installed Vulkan
  implicit layer stays dormant until its target-specific activation event.
- Host shutdown now signals a global stopping event. DirectX/OpenGL and Vulkan
  runtimes enter dormant pass-through, quiesce host-owned capture resources,
  acknowledge target-specific dormancy, and remain mapped until game exit.
  Remote `FreeLibrary` and hook self-unload are intentionally absent: wrappers,
  callbacks, foreign saved targets, and in-flight detours can retain CE addresses.
  Vulkan retains minimal forwarding/reactivation metadata and pins its image for
  its process-lifetime watcher.
- A new CaptureEngine generation signals retained per-target reactivation events.
  The resident runtime consumes the old wakeup before validating discovery and
  reconnecting, so a newer signal that arrives during the attempt is not lost.
  IPC publishes the new mapping atomically and retains old generations until
  process exit to protect already-entered detours from mapping use-after-free.
- All graphics entry paths gained dormant pass-through guards and host-disconnect
  resource cleanup. OpenGL context-owned deletion remains deferred to its owner
  context; Vulkan proc-address hooks stay stable across dormant/reactivated state.
- Third-party overlay pixels are captured when their natural draw order precedes
  CE's capture point. Inclusion is deliberately best effort: forcing private
  overlay handlers or GPU-work reordering would compromise coexistence.
- Focused regression gate passed for the DXGI behavior/source policies, overlay
  module detection, IAT filtering, lifecycle event/source contracts, NVIDIA LOD
  routing, and DLSS indicator pass-through suites.
  Full verification is pending.
