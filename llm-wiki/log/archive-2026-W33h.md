# llm-wiki Log Archive (2026-08-13, shard W33h)

Entries are newest-first. Rotated out of `recent.md` on 2026-08-14 to keep it near
the 230-line rolling-memory ceiling.

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
