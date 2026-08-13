# llm-wiki Log

### 2026-08-13 - BUILD SPEED: packaging overlaps lint, sanitizer/vulkan object isolation, faster gate stages

- Retrospective over the 2026-08-13 gates: warm `--verify` ~175-240 s, force-rebuild ~627 s; two runs failed
  at the Vulkan layer link and forced `--verify --force-rebuild` reruns, and one failed verify flaked on the
  wall-clock-sensitive `FpsLimiterTest` timing tests (all five passed 4/4 standalone runs). The locked-file
  rename path (`.old.<ts>.<rand>` + reboot-delete scheduling) never caused duplicate builds; the ~5 s stall
  per locked file came from the handle.exe attribution probe (5 s timeout), now skipped when absent and capped
  at 1 s.
- Fixes: (1) `build_vulkan_layer.py` objects now live under `OBJ_DIR`, isolating the sanitizer child's
  ASan-instrumented layer objects from the product cache (the root cause of the failed-verify -> force-rebuild
  duplication); (2) `build_cli` schedules `package_build_outputs()` concurrently with the advisory lint pass
  (`should_package_outputs` policy, lock-guarded verification recording); (3) the product build releases the
  sanitizer child's reserved worker budget for tests/captureengine/testapps/vulkan once the child finishes;
  (4) the privacy scan is a single read/scrub/verify pass instead of two full passes; (5) clang-format checks
  only worktree-changed C++ sources (`collect_changed_lintable_cpp_sources`, git-diff + untracked, full-set
  fallback).
- Tests: `test_packaging` (deferred-packaging source pin + `should_package_outputs` policy),
  `test_verification_parallelism` (vulkan `OBJ_DIR` pin + budget-release pin), `test_build_lint_policy`
  (changed-source filter). Gate: the strict `--verify --verify-clean` failed once on the flaky timing tests
  (unrelated), then completed green as warm `--verify` on 0.1.6016 in 138 s; sanitizer cadence, 2175 native
  tests, Python self-tests, clang-tidy ratchet, and concurrent packaging all passed.
- Noted for later: `--resume` after a failed `--verify` needs `--verify` passed again or it only finishes the
  build/packaging without tests/lint/sanitizer (documented in build.py.md); the force-rebuild preflight still
  re-analyzes all 591 TUs (~147 s) because the version bump invalidates every preflight cache entry.
# llm-wiki Log

### 2026-08-13 - FIXED: DLSS-FG switch after FG-spam wedged the app at ~1 FPS + hidden overlay (stale upload-slot guards)

- Session `20260813_173453` (build 0.1.6011, dx12_fg_switch_test, manual dump): after FG-mode switching spam, the
  switch to DLSS FG dropped the app to ~1 FPS and hid the overlay. Dump: the present thread parked in
  `DX12DescFreeBackend::WaitForSlotGpuComplete` (`WaitForSingleObjectEx`) inside the PostSL overlay render; log:
  `DescFree: slot N GPU-completion wait timed out (guard=219..222 completed=0,1,2,...)` EVERY present.
- Root cause: the per-slot upload-ring guards store ABSOLUTE overlay-fence values. Overlay reinit (`InitOverlaySync`)
  releases the old fence and creates a new one, and the new fence object landed at the SAME virtual address as the
  released old one (ABA reuse). Both backends detected fence replacement by raw pointer comparison, so the lifetime
  change was missed: the stale guards (219-222) survived against the new fence (values restart at 0) and every wait
  burned the full 1 s liveness timeout, skipping the overlay draw. The timeout path never re-records the slot, so the
  wedge was permanent. (Not the 20260703_210021 AMD-suspend stall: there the fence NEVER advances; here it advances
  once per present but can never reach the stale guard values.)
- Fix: `UploadSlotGuardFenceBinding` (new `hook/common/dx12_overlay_policy/upload_slot_guard.h`) pins the bound fence
  with an owning COM reference, so a replacement fence can never reuse its address and any pointer change provably is
  a new lifetime; `RebindIfNeeded()` clears the per-slot guards. Wired into `DX12DescFreeBackend` and
  `CustomOverlay::DX12Backend`, replacing the raw `slotFence` members.
- Tests: `tests/test_dx12_upload_slot_guard.cpp` (fake-fence rebind/lifetime suite, ABA model, source pins).
  `--verify --force-rebuild` passed on 0.1.6013 (the plain `--verify` first hit the pre-existing Vulkan-layer
  sanitizer-object cache race, see below). OPEN: user re-run of the FG-spam -> DLSS sequence.

### 2026-08-13 - FIXED: switch-to-DLSS startup window blanks the overlay after long FG-switch spam

- Session `20260813_170318` (build 0.1.6009): after extreme FG-switch spam, EVERY switch to DLSS FG (FSR->DLSS
  and all-off->DLSS) briefly blanked the overlay. Between the FG-ON edge and the first PostSL callback
  (150-203 ms, "PostSL synthetic startup takeover - ProcessFrame dormant") the startup-handoff bypass presents
  drew nothing: the RTSS-style eager present-time draw was env-var-gated AND excluded FSR history, and PostSL
  had not confirmed yet.
- Fix: `ShouldEagerDrawOverlayBeforeStreamlineStartupBypass` — on those bypass presents the live overlay backend
  now draws present-time when it is initialized/sync'd and bound to the exact proxy queue (post-FSR prewarmed
  case) or under the explicit-enable pure-DLSS cold-start proof (GetState-only enables keep the GTA protection).
  PostSL confirmation immediately re-owns the overlay and the gate turns off; `HandleDX12ProcessFrame` keeps its
  internal same-queue safety check.
- Tests: `EagerDrawCoversStreamlineStartupBypassWindow` + source pin
  `StartupBypassEagerDrawConsultsLiveBackendProof` (`tests/test_dxgi_shared_part12.cpp`), test stub updated. Open:
  user re-run of the spam sequence on 0.1.6011.

- BUILD SYSTEM (pre-existing, needs its own fix): two consecutive `--verify` runs failed at the Vulkan layer link
  with undefined `__asan*` symbols - the sanitizer child compiles the SAME vulkan-layer sources concurrently and
  shares the per-object cache with the product build, and the cache key does not distinguish the sanitizer flag
  set, so the product build reused ASan-instrumented objects. `--verify --force-rebuild` recovers (both failures
  recovered this way; 0.1.6011 passed clean). Proper fix: include the full flag set in the vulkan-layer object
  cache key or give the sanitizer child an isolated cache store.

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
