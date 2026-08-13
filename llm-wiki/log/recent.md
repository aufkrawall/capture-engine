# llm-wiki Log

### 2026-08-13 - ROOT-CAUSE REFINEMENT + DUMP COVERAGE: FSR re-entry after FSR->OFF still failed E_ACCESSDENIED; fatal switch failures now always produce dumps

- Session `20260813_211734` (build 0.1.6023, dx12_fg_switch_test via Steam overlay + RTSS): the first OFF->FSR
  switch now succeeds (the live-entry-chain retry fixed that seam), but after FSR->OFF the SECOND OFF->FSR
  switch failed identically and the app exited cleanly — still no .dmp.
- New pin diagnostics at the failure: `E_ACCESSDENIED pin diagnostics ... chain=00000194A4DEFF00 committed=1
  refs=3` — the old OFF-phase native swapchain still had 3 live references after the game AND CE released
  everything (wrapper destructor "real refs=8" = CE base + 4 promoted + 3 foreign). The route diagnostics show
  the CreateSwapChainForHwnd entry holding ORIGINAL bytes with RTSS's vtable-slot handler as the immediate
  caller, so the remaining refs are foreign (RTSS/Steam per-swapchain bookkeeping), not CE's.
- Why the .dmp was missing: the failure path is a CLEAN exit (exit code 0, no exception), and
  `ShouldCapturePreTerminationDump` deliberately skips exit code 0 — the pre-termination dump hooks never fire.
- Fixes: (1) every exhausted CreateSwapChainForHwnd E_ACCESSDENIED recovery arm (deep + inline) now writes a
  session-local diagnostic minidump via `WriteSupplementalCrashDump`
  (`CaptureCreateSwapchainAccessDeniedExhaustedDump`, once per process, hint
  `swapchain_access_denied_exhausted.dmp`, synthetic exception code 0xE000EACC); (2) the test app writes its own
  fatal minidump (`WriteFatalSwitchDump` in testapp_common.h) and exits with 0xE000EACC instead of 0 on
  "Fatal switch failure"; (3) added bracketed pin probes (pre-cleanup / post-cleanup / post-entry-retry) plus a
  post-destruction wrapper refcount probe so the next failing run attributes the residual refs exactly.
- Tests: `AccessDeniedExhaustionWritesDiagnosticDumpAndBracketedPinProbes`
  (`tests/test_dxgi_shared_access_denied_dump.cpp`). `--verify` gate passed on 0.1.6027. OPEN: user re-run — if
  the second OFF->FSR still fails, the new dump plus probes will name the remaining pin holder.

### 2026-08-13 - FIXED: OFF->FSR FG switch in dx12_fg_switch_test via Steam overlay + RTSS failed E_ACCESSDENIED and exited (no dump)

- Session `20260813_200741` (build 0.1.6018, dx12_fg_switch_test started via Steam with Steam overlay and RTSS
  injected): the OFF->FSR FG switch failed. `ffxCreateContext(FG_SWAPCHAIN_HWND_DX12)` returned RUNTIME_ERROR
  because its internal `CreateSwapChainForHwnd` answered 0x80070005 (E_ACCESSDENIED); the app's native fallback
  create on the same HWND failed identically and the app exited cleanly ("Fatal switch failure: no swapchain after
  OFF request") — hence no dump. A CE-only run of the same build switches cleanly.
- Root cause: with foreign overlays present, the replacement create reaches the genuine DXGI create through CE's
  DEEP below-the-chain trampoline, which never enters the foreign overlay entry chain. An overlay whose
  CreateSwapChainForHwnd entry handler tracks the old swapchain per HWND (holding a reference until a replacement
  create arrives through that handler) then keeps the old chain alive; DXGI refuses the replacement with
  E_ACCESSDENIED through the full CE cleanup and every retry. CE's own refs were released (wrapper destruction +
  full recovery), so the residual pin is foreign.
- Fix: after CE's cleanup, the deep-hook E_ACCESSDENIED recovery retries once through the LIVE entry chain
  (`ShouldRetryAccessDeniedCreateThroughLiveEntryChain` policy plus a guarded below-the-chain pass-through), so
  the foreign overlay handlers see the replacement create and can release the old swapchain before the genuine DXGI
  create runs. Third-party-overlay callers and shutdown are excluded; a ReShade-style proxy factory keeps the
  trampoline-only retries (saved-slot vtable guard, sessions 20260813_004853/004923). Added rate-limited route
  diagnostics (immediate caller module + live entry bytes) and an E_ACCESSDENIED pin probe (old-chain refcount) so
  a residual foreign pin is provable on the next run.
- Build: the wrapper-internal and dx12-hook-internal headers both declared `ResolveCurrentProcessForeground` with
  default arguments, which broke any TU including both; the canonical declaration now lives in
  `hook/common/hook_common.h`. `--tests-only` builds now warn when modified hook/captureengine product sources
  are outside the tests-only compile set (shared `HOOK_DLL_EXCLUDED_SOURCES` exclusion list), so focused tests
  can no longer pass while the product sources never compiled.
- Tests: `AccessDeniedRetryThroughLiveEntryChainRequiresForeignChainAndSafeCaller`
  (`tests/test_dxgi_shared_part8.cpp`), `TestsOnlyCoverageWarningTest`
  (`tools/tests/test_build_tests_only_coverage.py`). `--verify --verify-clean` gate passed on 0.1.6023
  (strict clean rebuild, full native suite, Python self-tests, lint/tidy, ASan/UBSan). OPEN: user's manual
  re-run of the Steam-overlay + RTSS OFF->FSR switch; the full four-direction FG matrix.

### 2026-08-13 - FIXED: DLSS-FG re-enable after a mixed FSR/DLSS switch session hid the overlay forever (warm-resume proxy restoration)

- Session `20260813_192326` (build 0.1.6016, Talos1): after FSR FG -> DLSS FG -> menu OFF -> DLSS FG again, the
  inject overlay disappeared during 3D rendering and never came back. Log signature: the second DLSS epoch confirmed
  ~15k PostSL submits on the live DLSS-G proxy queue `00000237F829ABE0`; at the menu OFF edge the post-FSR teardown
  logged `Streamline FG OFF after FSR history — releasing stale swapchain queue …` and nulled `g_SwapchainQueue`
  although the make-before-break keep-alive kept rendering through the whole suspend on that exact queue. On re-ON
  the warm resume preserved confirmed rendering but had no queue left: `SelectPostSLBootstrapSubmitPath` returned
  `kReject` and every resumed frame logged `refusing SL wrapper bootstrap without direct path` (×1977) until exit.
- Root cause: `g_HadFSRFGPhase` is a session-lifetime latch, so the OFF-side "stale FSR queue" release ran for a
  queue that was actually the live DLSS-G proxy of the just-ended epoch. The existing ON-side warm-resume guard only
  prevents a fresh clear; it cannot resurrect a queue the OFF side already released.
- Fix: `ShouldRestoreSwapchainQueueFromPreservedConfirmedPostSLProxyOnWarmResume` restores `g_SwapchainQueue` from
  the preserved PostSL last-working queue (AddRef + capture time + exact last-successful swapchain identity +
  runtime ownership) at the top of the warm-resume branch in `dx12_hook_streamline_fg_transition.cpp`, before the
  ON-side stale-FSR-clear evaluation. The OFF-side release and non-FG recovery classification are unchanged, so the
  suspend interval keeps its proven keep-alive behavior; the resumed submit takes the proven selected-scQueue
  original-ECL path again.
- Tests: `WarmResumeRestoresPreservedConfirmedPostSLProxyQueue` (`tests/test_dxgi_shared_part7.cpp`) +
  `WarmResumeRestoresReleasedPostSLProxyBeforeStaleFSRClear` (`tests/test_dxgi_shared_part12.cpp`). `--verify` gate
  passed on 0.1.6018 (full native suite, Python self-tests, lint/tidy, ASan/UBSan). OPEN: user re-run of the exact
  FSR -> DLSS -> menu OFF -> DLSS sequence and the full four-direction switching matrix.

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
