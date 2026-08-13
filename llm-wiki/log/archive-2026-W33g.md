# llm-wiki Log Archive (2026-08-13, shard W33g)

Entries are newest-first. Rotated out of `recent.md` on 2026-08-13 to keep it near
the 230-line rolling-memory ceiling.

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
