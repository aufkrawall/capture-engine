# llm-wiki Log

### 2026-08-12 - Talos + DLSS FG + RTSS: CE's proactive Steam slot patch was severing Steam's chain (0.1.5938)

- Strange Brigade DX12 (no FG, no Streamline) is **fixed and user-confirmed** by 0.1.5937: all three overlays render. Talos with DLSS FG still lost RTSS, because the FG-interposer exception keeps CE's Present-entry prepend there (runtime-generated presents reach CE only through that hook — `DetourCreateSwapChainGlobal: Streamline present, skipping wrap` means CE never wraps Streamline's swapchain, and PostSL rendering is driven from `DetourPresent`).
- Session `20260812_022607`: `loadedOverlays=2 fgInterposer=1` -> prepend kept, as designed. Frame 1 trail `capture_hook_x64.dll > gameoverlayrenderer64.dll > RTSSHooks64.dll > capture_hook_x64.dll > sl.dlss_g.dll > sl.interposer.dll > Talos` — **game -> sl.dlss_g -> CE -> Steam -> RTSS, all three drawing**. From frame 2 the trail is CE -> Steam only; RTSS submitted 1 ECL for the whole session (Steam 77, sl.common 72, game 200).
- Root cause: at 02:27:10.571 — before the first overlay draw — `EnsureSteamNullCallbacksPatched` wrote CE's DXGI bypass `0x7FF882D10000` into **20 still-NULL Steam callback slots**, and every guarded invoke from then on reported `steamCallback=00007FF882D10000`, i.e. CE's own value. Those slots are Steam's hook-install outputs (`gameoverlayrenderer64+0x8da00` takes `&slot`; call sites test `cmpq $0, slot` right after), so pre-filling makes Steam skip its own install and chain to a raw `dxgi!Present` copy that skips everything below Steam — RTSS included. No AV, no VEH recovery, no bypass fallback fired in that session: the patch prevented nothing and only cut the chain.
- Fix 0.1.5938: `EnsureSteamNullCallbacksPatched` is deleted along with all three call sites. **Invariant: CE may inspect another tool's internal state, never speculatively write it.** The NULL dispatch it guarded against is covered by the two mechanisms that were already there and are sound — the callback-state gate refuses the invoke unless the recovery is armed, and `SteamOverlayInitVehHandler` resolves the exact faulting slot from the fault context. Slot discovery stays, read-only, to feed the gate.
- Also added: a metered Present-entry ownership watch (`InlineHook::IsInstalledEntryPatchIntact`). If a foreign tool re-hooks over CE's prepend — the second candidate mechanism, which CE cannot undo from its side while it must hold the entry for FG — the log now says so with the new owner instead of leaving an overlay disappearance to be re-diagnosed: `DetourPresent: foreign re-hook took the Present entry from CE #N (entry=... newTarget=... owner=...)`.
- Validation pending: Talos + DLSS FG + Steam + RTSS. Expect RTSS to keep submitting for the whole session and no `foreign re-hook took the Present entry` line. If that line DOES appear, the remaining mechanism is the re-hook capture, and the only fix left is giving CE a non-entry view of Streamline's generated presents.

### 2026-08-12 - ROOT CAUSE: CE must stay out of a Present entry two foreign overlays share (0.1.5934)

- Session `20260812_013241` (0.1.5933, hardcoded `RTSSHooks64+0x72F20` direct invoke): RTSS drew every frame (152 RTSS + 145 d3d11on12 ECLs vs 240 game) and **Steam's overlay never drew at all** (0 `gameoverlayrenderer64` ECLs). Each of the four preceding attempts just moved which overlay was excluded.
- The chain topology is now established from the ECL call trails. Frame 1 of `20260812_002958` / `_010529`: `capture_hook > gameoverlayrenderer64 > RTSSHooks64` — i.e. **game -> CE -> Steam -> RTSS -> real Present, all three drawing**. From frame 2 onward Steam's saved "next" no longer reaches RTSS (RTSS: exactly 1 ECL per session, Steam: 269/349/98). Steam's install helper (`gameoverlayrenderer64+0x8da00`, call sites at `+0x64a92`/`+0x64ae6` with `cmpq $0, <origSlot>` right after) writes its "original" slot from the entry bytes it finds; RTSS's Present handler (`RTSSHooks64+0x72F20`, disassembled) is a literal restore-original-bytes -> `call [entry]` -> re-patch cycle over the SAME shared entry.
- **Root cause:** two such tools compose; a third does not. Whichever of them (re-)hooks while CE's five-byte prepend is live records **CE** as its own "next", which silently drops the other overlay out of the chain. The damage is inside the foreign tools' saved-chain state, so no forwarding choice on CE's side can repair it — frozen trampoline, live entry, and hardcoded RTSS handler all failed, each excluding a different overlay.
- **Fix 0.1.5934:** `ce::overlay_compat::ShouldLeavePresentEntryToForeignOverlayChain(foreignEntryJump, loadedOverlayCount, fgInterposerLoaded)` — with **two or more** loaded overlays on the entry and no FG interposer, `InstallPresentInlineHooks` installs **no** entry patch at all and sets `dxgi_shared_s_presentEntryLeftToForeignChain`. `ShouldInstallSwapchainHooksWithThirdPartyOverlay` then also keeps the swapchain vtable pristine (Steam resolves its "next" from that slot). `HasPresentDetourHooks()` goes false, so `ShouldDelegateDX12PresentToDetourHook` stops delegating and `CWrapDXGISwapChain::Present` does the overlay/capture work and forwards through `m_pReal->Present` — the foreign chain is then byte-identical to a process without CE. `CallOriginalPresent`/`Present1` forward through the live entry in that mode (never a trampoline, saved target, or bypass).
- Deliberately unchanged: **one** foreign overlay (Talos/GTA/RoboCop = Steam only; seed bits `0x1000001` in `20260811_214252` are Steam + `sl.interposer`, and `sl.interposer` is not in the overlay subset) keeps the prepend and every validated FG/Steam routing path. An FG interposer (Streamline/NvPresent) also keeps it: runtime-generated presents never reach CE's wrapper, so the entry hook is CE's only view of them.
- Removed: the hardcoded `RTSSHooks64+0x72F20` signature resolver, the ±1 MB thunk scan, and the `useLiveEntry` heuristic. Pinned by `tests/test_dxgi_shared_part11.cpp` (no tool-specific handler resolution may return).
- Known limitation (logged, not fixed): a second overlay that loads **after** CE prepended cannot be un-prepended retroactively. `DllNotification: third-party overlay <name> joined a Present entry CE already prepended over` names that state once.
- Validation pending: Strange Brigade DX12 + Steam + RTSS + CE — all three overlays visible simultaneously. Expect `InstallPresentInlineHooks: 2 third-party overlays already share the Present entry ... CE stays out of the entry patch chain` and `CallOriginalPresent: foreign-chain entry forward #1`, with ECL trails showing game/RTSS/d3d11on12/gameoverlayrenderer64 all submitting.

### 2026-08-12 - RTSS + Steam: live-entry alone did not help; invoke RTSS's own thunk directly (0.1.5932)

- Build 0.1.5931 (live-entry forward) still starved RTSS (session `20260812_005530`): the entry jump is stably owned by Steam (Steam rehooks last; the live-entry target equals the saved external hook `0x7FF842DC0000` every frame), so CE → Steam → (Steam's saved "next") → RTSS drew exactly one frame and then only Steam's overlay submitted. Steam's lazy init drops RTSS from its chain; without the slot patches that init crashes (004407), with the patches it completes and starves RTSS.
- Fix 0.1.5932 (`e3085d89`): resolve RTSS's OWN FF25 hook thunk by scanning the executable region (±1 MB) around Steam's thunk for a payload pointer into `RTSSHooks64.dll`, and invoke it directly for non-Steam chains when Steam is loaded. RTSS's restore/rehook then reclaims the entry like the natural no-CE chain; Steam's handler is never entered (no NULL-callback crash, no init starvation). Falls back to live-entry/trampoline routing when no RTSS thunk is found (Steam-only games unchanged).
- Validation pending: Strange Brigade + Steam + CE ~30 s — RTSS OSD must persist; the hook log must show `Resolved RTSS Present thunk ... (near saved external hook ...)` and `direct RTSS Present thunk forward`. `ce_dx12_trace` flag remains set.
- Still open: whether Steam's overlay was actually visible in the user's no-CE baseline (it determines whether hiding Steam's overlay in the CE+RTSS+Steam configuration is acceptable).

### 2026-08-12 - RTSS + Steam: patches are mandatory (crash without them); follow the live Present entry (0.1.5931)

- Build 0.1.5930 removed Steam's NULL-callback slot pre-patching from the
  non-Steam (RTSS) chain and crashed (session `20260812_004407`, dump
  `StrangeBrigade_DX12.exe_2026-08-12_00-45-02.dmp`): RIP=0/RAX=0 during Steam's
  lazy init on frame 2, return address inside
  `gameoverlayrenderer64!VulkanSteamOverlayProcessCapturedFrame` — the classic
  Steam NULL Present-shaped callback. The crash handler caught it instead of
  `SteamOverlayInitVehHandler` (no VEH logs at all — open question why the
  guard's handler did not run/decline-log); the 39 MB dump took 37 s because
  `MiniDumpWriteDump` blocked on a Steam critical section.
- Fix 0.1.5931 (`5aca4a2e`): Steam slot pre-patching + VEH backstop restored on
  the non-Steam forward, AND the forward now follows the LIVE `dxgi!Present`
  entry (once CE's prepend is gone — RTSS's restore/rehook wipes it on frame 1)
  instead of the frozen install-time relay target, reproducing the natural chain
  that keeps RTSS's OSD alive without CE. Entry-driven chain: if RTSS's E9 owns
  the entry (the no-CE stable state), RTSS draws and Steam's handler is not
  entered at all; if Steam's E9 owns it, Steam runs patched + VEH-guarded.
- Validation pending with the user: Strange Brigade + Steam + CE ~30 s, RTSS OSD
  must persist and no crash; `ce_dx12_trace` flag is still set for attribution.

### 2026-08-12 - RTSS + Steam coexistence: classification fixed, OSD still vanishes (0.1.5927/5928)

- Session `installed/captureengine/logs/20260811_233748` (Strange Brigade DX12, Steam overlay + `RTSSHooks64.dll`): RTSS's OSD vanished after a brief moment while CE's overlay stayed, with both RTSS inject modes. The tracked-overlay cache reports the first loaded entry by list priority, so with Steam + RTSS loaded it names `gameoverlayrenderer64.dll` even though RTSS loaded later and owns the preserved `dxgi!Present` entry jump. CE then serviced RTSS's runtime thunk (`FF 25 00 00 00 00` + pointer into RTSSHooks64.dll) through the Steam guarded-invoke machinery.
- Live probes (dx12_test, session `20260811_235651`): RTSS-only + CE keeps RTSS's OSD visible indefinitely on the plain trampoline forward; the failure needs Steam loaded as well. RTSS's restore/rehook cycle also re-enters Steam's handler nested inside the forward (RTSS saved Steam's E9 as its "original" bytes).
- Fix 1 (commit `9c023489`): owner-based foreign-chain classification — resolve the thunk pointer to the owning DLL; unresolvable thunks fall back to last-load-order evidence recorded only from real load notifications. All Steam routing decisions (`IsSteamExternalChainTrampoline`, guarded invoke, forced bypass, startup pass, SL fast path, Present1) now use it. Non-Steam chains with Steam loaded keep Steam's NULL-callback patches + VEH around the bare forward. Install-time log: `External hook owner: ...`.
- Validation (session `20260812_001959`, 0.1.5928): classification works, but **RTSS OSD still disappears** — the Steam guarded machinery was NOT the cause. ECL-timing drops from ~3 ECLs/frame (game + RTSS, same as working dx12_test) to ~1 over ~10 s while the game keeps ~1200 fps: RTSS's per-frame submissions stop. Finer ECL trace sampling added (commit `f8f3ecd2`, every 64th call) and `ce_dx12_trace` flag is set; next runs will attribute the loss per module and test Steam-overlay-disabled.
- Tests: `tests/test_overlay_compat.cpp` (last-loaded, load-order decision), `tests/test_dxgi_shared_part11.cpp` (source-policy).

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
