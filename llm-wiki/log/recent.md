# llm-wiki Log
### 2026-08-13 - FIXED: FSR FG -> DLSS FG handoff keep-live + DLSS-off native-return one-shot proof consume

- Session `20260813_162959` (build 0.1.6007): the FSR->DLSS handoff preserved the exact prewarmed PostSL
  backend on its first Present, but the late [outer] SL-FG-OFF observer force-cleared it ("stale SL backbuffers")
  + armed a 60-frame cooldown; PostSL had to rebuild after a 203 ms dormancy -> visible blank. The preserved
  backend already has fresh RTVs for the new swapchain lifetime, so the outer teardown now keeps it live
  (`ShouldKeepOverlayLiveAcrossPrewarmedPostSLHandoffPreserve`; phase2 latches
  `exactPrewarmedPostSLHandoffBackendPreservedThisPresent`).
- Session `20260813_164314` (build 0.1.6008, user-validated): ZERO `INTERRUPTED/UNPROVEN` markers across the
  whole FSR/DLSS switch sequence, no crash, and every prewarmed handoff preserve kept the overlay visible.
  The same log exposed a pre-existing per-present reinit storm on the DLSS->OFF native return: the exact
  native-return proof was only consumed inside the immediate-reinit branch, so an ABA-equal pointer + the
  still-armed proof reprocessed the replacement on EVERY present (191 consecutive "processing it as a new
  lifetime" rounds, ~6.4 ms per present). The proof is now consumed once at the TOP of the replacement handler.
- Tests: `KeepsOverlayLiveAcrossPrewarmedPostSLHandoffPreserve` +
  `PrewarmedPostSLHandoffPreserveKeepsOverlayLiveAcrossLateOuterOff` (`tests/test_dxgi_shared_part12.cpp`),
  updated `AuthoritativeDLSSOffNativeReturnProofFeedsFirstMatchingPresent` (single one-shot consume,
  `tests/test_dxgi_shared_part3.cpp`), and the outer-off guard-chain pin update. `--verify --force-rebuild`
  passed on 0.1.6009 (clean full rebuild after a stale ASan-instrumented vulkan-layer cache entry poisoned the
  link; full native suite, Python self-tests, lint/tidy, ASan/UBSan). OPEN: full four-direction matrix.

### 2026-08-13 - FIXED: HookThread crashed in slGetFeatureFunction while the game unloaded the Streamline stack

- Session `20260813_160845` (build 0.1.6006, dx12_fg_switch_test): with the overlay-visibility fixes in place, the
  FG switch sequence crashed. Dump: DEP 0xC0000005 at `TryResolveReflexFeatureHooks`
  (streamline_hook_resolve.cpp:414) -> `sl_interposer!slGetFeatureFunction+0x162` -> unmapped memory; cdb resolves
  the crash address into `<Unloaded_sl.dlss_d.dll>+0x4d7e0`.
- Root cause: the proactive feature-resolution guard pinned only sl.interposer + the queried feature plugin, but the
  interposer's slGetFeatureFunction walks its registered plugin table, which still referenced a plugin that had already
  been unmapped. That unload had NO CE hook slots in its range, so it logged nothing and the generation snapshot
  (taken after the unload) could not detect it. A follow-up exception then hit sl.interposer itself after it unloaded.
- Fix (event-driven, no timers): every tracked sl.* unload sets `streamline_hook_g_StreamlineTeardownInFlight`; the
  next sl.* load clears it. Both feature-resolution paths and the query guard fail closed while it is set. The guard
  additionally pins EVERY loaded sl.* module (snapshot + LoadLibrary refcount) so no dispatch target can unmap
  mid-query, and returned function pointers are only hooked when no teardown was observed during the query and the
  pointer still belongs to a loaded module.
- Tests: `FeatureResolutionLatchesTeardownInFlightUntilNextStreamlineLoad` + updated
  `FeatureResolutionSkipsStreamlineTeardownRace` (`tests/test_streamline_runtime_policy_part2.cpp`). `--verify` gate
  passed on 0.1.6007 (full native suite, Python self-tests, lint/tidy, ASan/UBSan). OPEN: user re-run of the FSR ->
  DLSS -> FSR -> all-off sequence must keep the overlay visible AND crash-free.

### 2026-08-13 - FIXED (follow-up): FSR FG -> all-off still blanked the overlay when DLSS FG ran in between

- Session `20260813_155313` (build 0.1.6005): with DLSS FG used between FSR sessions, the final FSR -> all-off switch
  blanked the overlay again for 60 presents / 484 ms (`INTERRUPTED/UNPROVEN` present 728 -> `RESTORED` present 788).
  The keep-live never armed because the game-created recovery swapchain REUSED the previous swapchain's COM pointer
  address, so phase2 never processed the replacement and no recovery reinit ran before the outer SL-FG-OFF teardown
  force-cleared the overlay.
- Fix: the teardown end arms a one-shot exact lifetime proof (`dx12_hook_g_ExactGameSwapchainRecoverySwapchain`,
  armed with the game-created swapchain next to `PostNativeFSROffGameSwapchainRecoveryQueue`); phase1 feeds it into
  `ShouldProcessLogicalSwapchainReplacement` so an ABA-equal pointer is processed as a new lifetime, and phase2
  consumes the proof once. The existing recovery reinit + keep-live then rebuild the overlay in the same Present and
  veto the outer teardown (zero uncovered presents).
- Tests: `ExactGameSwapchainRecoveryLifetimeProofArmsFeedsAndIsConsumedOnce` +
  `ExactGameSwapchainRecoveryLifetimeProofClearedAtRecoveryQueueResetSites` (`tests/test_dxgi_shared_part12.cpp`),
  plus the outer-off guard-chain pin update. OPEN: user re-run confirmed the overlay stays visible on the final
  FSR -> all-off switch (no `INTERRUPTED/UNPROVEN` markers).

### 2026-08-13 - FIXED: FSR FG -> all-FG-off switch blanked the overlay for 60 presents (453 ms) at the end of the sequence

- Session `20260813_153118` (build 0.1.6003, dx12_fg_switch_test): the overlay briefly disappeared at the END of the
  FSR FG -> all-FG-off switching sequence. `[OVERLAY COVERAGE]` shows `INTERRUPTED/UNPROVEN` at present 1745 and
  `RESTORED ... missed=60 durationMs=453 gate=overlay-backend-uninitialized` at present 1805.
- Root cause: the game-created recovery swapchain correctly ended the runtime-owned native-FSR teardown and the first
  Present on it warm-reinited the overlay on the captured game queue, but on the SAME Present the late outer
  `g_StreamlineFGRunning` OFF edge (latched true across the whole FSR session) force-cleared `overlayInit` +
  `CleanupRTVs` ("stale SL backbuffers") and armed the generic 60-frame reinit cooldown, tearing down the just-built
  backend. The RTVs were NOT stale: they were rebuilt on the game's own native swapchain.
- Fix: `FrameProcessSession::nativeFSRGameSwapchainRecoveryReinitializedThisPresent` latches the phase2
  `ShouldReinitOverlayImmediatelyAfterGameSwapchainRecoveryFromNativeFSROff` decision, and the new
  `ShouldKeepOverlayLiveAcrossNativeFSRGameSwapchainRecovery` policy makes the phase5 outer OFF teardown keep the
  freshly rebuilt backend live and bypass the cooldown (mirrors the DLSS-off normal-return keep-live). A real runtime
  takeover, missing reinit proof, or removed device keeps the protective teardown.
- Tests: policy halves in `tests/test_dxgi_shared_part9.cpp`, source invariant in `tests/test_dxgi_shared_part12.cpp`,
  and the outer-off guard-chain source pin updated. `--verify` gate passed on 0.1.6005 (full native suite, Python
  self-tests, lint/tidy, ASan/UBSan). OPEN: needs the user's re-run of the FSR FG -> all-off switch; the full
  four-direction FG matrix still gates F2/F4.

### 2026-08-13 - FIXED: orphaned below-foreign-chain FSR deep-draw state froze Talos on the FSR->DLSS menu switch

- Session `20260813_142910` (build 0.1.5999, Talos + Steam overlay + official FFX FSR FG): the game raised a
  fatal D3D12RHI ensure right after switching FSR FG -> DLSS FG in the menu and then froze for 50+ s
  (FreezeWatchdog `Render thread frozen`, game dumps plus the assert dumps all decode to
  `WindowsD3D12Viewport.cpp:267` `.hr failed ... with error 80070005`, i.e. E_ACCESSDENIED).
- Root cause: the 0.1.5999 below-foreign-chain deep draw stores its `dx12_ffx_suspend_overlay` renderer state
  under the PRESENTED FFX swapchain (`sc=00000249E8878BC0`), but the FFX teardown path only retired states keyed
  by the registered game-facing proxy (`proxy=0000024A2F68FAB0` from the queue bindings). The deep-draw state —
  recorded command lists plus per-slot backbuffer refs — therefore survived the FFX swapchain teardown, the
  documented outstanding-reference boundary that makes the game's swapchain resize/present fail E_ACCESSDENIED.
  The freeze dump confirms the orphan: `dx12_hook_g_LastSwapChain` still equals the dead FFX swapchain while the
  queue bindings were already released.
- Fix: `ce::dx12_ffx_suspend_overlay::RetireAllForNativeFSRTeardown` retires every live suspend-overlay state at
  both native-FSR teardown boundaries — `DX12_PrepareForStreamlineEnableTransition` (gated by the new
  `ShouldRetireNativeFSRSuspendOverlayStatesBeforeStreamlineEnable` policy) and
  `DX12_UnregisterNativeFSRSwapchainPresentationQueue` (FFX context destruction). In-flight states are retained
  until their own GPU fence completes; there is no wait, reinit on the Present path, title branch, or FG change.
  Also added rate-limited failure logging to `DetourCreateSwapChainGlobal` for FG-runtime swapchain creates.
- Tests: policy halves in `tests/test_ffx_below_foreign_chain_policy.cpp` and the source-invariant
  `DXGISharedSourceTest.NativeFSRTeardownRetiresEverySuspendOverlayState` in `tests/test_dxgi_shared_part8.cpp`.
  Focused tests + full native suite pass on 0.1.6000. OPEN: needs the user's Talos FSR<->DLSS menu-switch
  re-validation with Steam overlay; the full four-direction matrix still gates F2/F4.

### 2026-08-13 - FIXED (targeted): CE's overlay composites on top of Steam/RTSS under native FSR FG (build 0.1.5999)

- Session `20260813_061015` (build 0.1.5995, Talos + Steam overlay + official FFX FSR FG) is the repro: the FFX
  present callback drew CE's overlay into the runtime output buffer, the runtime presented it through DXGI, and
  Steam's entry hook composited afterwards — Steam on top of CE. The deep body hook already ran below the foreign
  chain every present, but the runtime-owned guard skipped CE's separate draw there.
- Working theory (field validation still open): the 0.1.5970-0.1.5972 deep-site attempts "never landed" while the
  game queue was idle (save-load), and the 0.1.5972 device removal at the FSR-FG-off edge is consistent with the
  normal overlay backend's preserved/stale RTV target. The exact-target, in-flight-retaining FSR-suspend renderer
  (`dx12_ffx_suspend_overlay`) is the transport for this fix.
- Fix: `DecideBelowForeignChainFSRDeepDraw` + `DX12_CompositeOverlayBelowForeignChainForRuntimeOwnedFSR`
  (`hook/common/dx12_overlay_policy/ffx_routing.h`, `hook/apis/dx12_hook_ffx_owner_queue.cpp`). When CE is below
  a foreign Present chain with a live, un-stalled FFX present callback (not no-callback composition, not the
  explicit FSR-off teardown window, not protected startup), `DrawSkipAndCounters` draws a second topmost overlay
  onto the presented swapchain's exact current backbuffer on the swapchain-owning queue — the queue Steam's own
  ECL went through in the repro (`scQueue=000001A1FA0B1440`), so queue order is foreign overlay -> CE. The FFX
  callback draw stays as the guaranteed baseline (no yield, so the route can never hide the overlay); refusals
  (in-flight slot, bad buffer, gate) fall back to it. Diagnostic:
  `[OVERLAY LAYER] ... site=deep-body-below-foreign-chain-runtime-owned-fsr ...`.
- Tests: `tests/test_ffx_below_foreign_chain_policy.cpp` (all gate halves). Verify gate passed on 0.1.5999
  (full native suite, Python self-tests, lint/tidy, ASan/UBSan). OPEN: needs the user's Talos FSR-FG + Steam run
  to confirm topmost layering and no device removal across the full FG switch matrix; GTA's historical
  0x887A002B app-callback boundary must be re-checked there too.

### 2026-08-13 - ROOT CAUSE FOUND (SpecialK upstream bug): fake SHGetKnownFolderPath buffer freed by sl.interposer

- Session `20260813_051600` (build 0.1.5995): ReShade-only (2 runs) and ReShade+OptiScaler (1 run) worked;
  every SpecialK-involved run crashed, and none of the failing stacks contains a CE frame. Re-testing WITHOUT the
  user's `sl.*` DLL override (session `20260813_055907`, game's own interposer 2.11.1) reproduced the identical
  crash with the IDENTICAL freed pointer — the version skew was irrelevant.
- `SK+R+O` (2 runs, deterministic, ~5s in): `STATUS_HEAP_CORRUPTION` in `RtlFreeHeap` from
  `sl.interposer.dll` during `slInit` called by OptiScaler. The freed pointer is inside `SpecialK64.dll`'s
  `.data` (unique UTF-16 string `XYZ:\123\456\!#$%^@?|` at SK+0xC86FA0); WER bucket
  `HEAP_CORRUPTION_ACTIONABLE_BlockNotBusy_DOUBLE_FREE_sl.interposer.dll`.
- **Root cause (SpecialK's code, not CE's):** `SK_IsModuleLoaded`/`SHGetKnownFolderPath_Detour` in
  `src/diagnostics/debug_utils.cpp` (~line 5185, upstream HEAD `11f5ccb`, 2026-08-12) returns
  `static wchar_t fake_path[MAX_PATH] = L"XYZ:\\123\\456\\!#$%^@?|"` as the `SHGetKnownFolderPath` out-param when
  the caller is `sl.interposer`, `rfid == FOLDERID_ProgramData`, `dwFlags == 0`, `hToken == nullptr` and SK's VEH
  saw a previous first-chance exception on the thread (UE raises many during startup). The
  `SHGetKnownFolderPath` contract requires a CoTaskMem-owned buffer; the interposer (2.11 and 2.12) frees it via
  `CoTaskMemFree` -> `RtlFreeHeap` -> `STATUS_HEAP_CORRUPTION`. The fix is in SpecialK: `CoTaskMemAlloc` + copy
  instead of returning the static buffer.
- `SK-only` (1 run, ~22s in): AV writing 0x8 in `RtlEnterCriticalSection(NULL)` — game code called from an
  sl.interposer worker thread while the game loaded its SL plugins (dlss_g/reflex), then the render thread
  froze for 60s (FreezeWatchdog). Possibly the same fake-path machinery or a separate SK/SL startup race;
  re-test after the SHGetKnownFolderPath fix lands.
- Upstream SpecialK issue text prepared (repro + one-line fix); GitHub connector was unavailable to file it.
  Until SpecialK ships the fix, the user's custom SpecialK64.dll must be rebuilt with the patch, or CE would
  need an opt-in compatibility shim on `SHGetKnownFolderPath` for sl.interposer callers (tool-specific
  workaround, not yet implemented).

### 2026-08-13 - FIXED: ReShade proxy queue re-entry in the ECL/Signal trace hooks (Talos crash)

- Talos (DX12) + ReShade-only crashed on start twice, on both sides of the same layered chain
  `game -> CE -> ReShade proxy thunk -> CE (real queue) -> global original(real queue)`.
- Session `20260813_041416` (build 0.1.5990): the ECL recursion-break path called the global
  `oExecuteCommandLists` (= ReShade's proxy hook, the first queue vtable CE hooked) with the real queue
  behind the proxy; ReShade's non-recursive queue mutex threw `std::system_error(EDEADLK)` (verified via
  the throw-info/catchable-type decode in cdb).
- Session `20260813_050515` (build 0.1.5993): ECL was fixed but the same blind-global pattern remained in
  `DetourTraceCommandQueueSignal` (`oTraceCommandQueueSignal` = ReShade's Signal thunk); calling it with the
  real queue read `_orig` at `queue+0x10` and jumped through garbage vtable slot `-1` (AV at
  `reshade+0x112467`).
- Fix (builds 0.1.5991/0.1.5995): type-safe per-vtable original resolution — policy
  `hook/common/dx12_overlay_policy/ecl_recursion_break.h` classifies candidates by owning module, native
  D3D12 runtime ECL is only used for native-vtable queues, proxy queues only forward through their exact
  vtable original, and foreign/self hooks are never re-entered (recursion-depth bound drops instead of
  looping). Native originals are published eagerly (`TryPublishRealD3D12ECLCandidate` /
  `TryPublishRealD3D12SignalCandidate` from `DX12_HookQueueVTable`); Signal forwards per-vtable
  (`dx12_hook_g_CommandQueueSignalOriginalByVTable`) with live-slot/native/legacy fallbacks.
- Tests: `tests/test_dx12_ecl_recursion_break_policy.cpp` (policy + source pins). Verify gate passed on
  0.1.5995. Needs the user's Talos re-test with all tool combinations.

### 2026-08-13 - FIXED (refined): suspend only previously loaded tools' threads, not the whole process

- Build 0.1.5988 field results: session `20260813_033707` got the game running but crashed in `nvwgf2umx` on the
  first Present (driver read a garbage command-list state); session `20260813_033912` ran and exited cleanly, but the
  log shows the all-threads suspension GAVE UP ("could not suspend peer threads cleanly ... degraded") — the game
  constantly spawns threads, so the stable-snapshot requirement always fails, and the run succeeded without any
  suspension. Suspending arbitrary game/driver threads is unsafe and unreliable.
- Refined `ToolThreadSuspension` in `hook/main_thirdparty_load.cpp`: only threads whose start address lies inside a
  previously loaded tool module (recorded via `GetModuleInformation` after each successful load) are suspended, and
  enumeration uses two passes instead of a globally stable snapshot. Game and driver threads are never touched; the
  loader-quiescence probe with resume-and-retry remains. Still needs field validation of all three tools.

### 2026-08-13 - FIXED (structural): peer-thread suspension around every tool load after the first

- Session `20260813_031321` proved Special-K-last + quiescence wait is still insufficient: CE's hook thread held the
  loader lock in Special K's DllMain, whose inner LoadLibrary re-entered ReShade/Steam/OptiScaler loader hooks and
  blocked on OptiScaler's mutex, held by an OptiScaler background thread doing NEW loader work. Order and wait alone
  cannot win — both tools have recurring background loader activity.
- Structural fix in `hook/main_thirdparty_load.cpp`: before every tool load after the first, CE waits for loader
  quiescence and then SUSPENDS all other process threads (stable TH32CS_SNAPTHREAD enumeration + bounded
  post-suspension probe with resume-and-retry if a peer was caught inside the loader). Peers are resumed immediately
  after the LoadLibrary returns. Order back to Special K -> ReShade -> OptiScaler: with peers suspended, Special K's
  enumerator cannot hold its thread-hook critical section across a loader call, so OptiScaler's DllMain thread
  creation proceeds. `ShouldSuspendPeerThreadsForToolLoad` added to `hook/common/third_party_load_policy.h`;
  template/README/wiki updated.

### 2026-08-13 - FIXED (order finalized): Special K now loads LAST; quiescence wait alone was not enough

- Session `20260813_025615` (all three tools, Special-K-first + quiescence wait): CE's hook thread held the loader
  lock in OptiScaler's DllMain; OptiScaler's thread creation waited on Special K's critical section; Special K's
  enumerator thread held that section while blocked in `FreeLibraryAndExitThread`'s loader drain. The wait cannot
  fix Special-K-first because the enumerator starts NEW loader cycles at any time, so the overlap is a race, not a
  one-shot init transient.
- Final order: ReShade -> OptiScaler -> Special K. OptiScaler's DllMain thread creation runs before Special K's
  thread hook exists, and the existing loader-quiescence wait before Special K drains OptiScaler's startup loader
  work (nvapi init, update check), which is startup-only. Order constant, executor array, tests, and template/README/
  wiki text updated accordingly.

### 2026-08-13 - FIXED: all-three-tools crash in the DX11 temp-device probe (0.1.5985 -> next)

- Session `20260813_024327` (ReShade + OptiScaler + Special K): AV in
  `d3d11!CLayeredObject<CDevice>::CContainedObject::Release` with a garbage `this` (UTF-16 string fragment),
  called from CE's `DetectSwapChainAPITypeForDX11Hook` while releasing the device returned by
  `IDXGISwapChain::GetDevice`. CE's temp D3D11 device/swapchain were third-party proxy objects because the
  "saved original" `D3D11CreateDeviceAndSwapChain` entry had been patched by the tools; releasing through the
  mixed ReShade/OptiScaler/Steam wrapper chain forwarded a corrupted pointer.
- Fix: `hook/apis/dx11_hook.cpp` now bypasses the entry patch on `D3D11CreateDeviceAndSwapChain` (and the D3D10
  temp route's `D3D10CreateDevice`) with `InlineHook::CreateBypassTrampoline` before creating the temp device, so
  the probe operates on genuine d3d11 objects — same rule as the temp-DXGI-factory fix. Source-order test added
  to `tests/test_inject_capture_source_part2.cpp`.
