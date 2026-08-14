# llm-wiki Log Archive (2026-08-13, shard W33i)

Entries are newest-first. Rotated out of `recent.md` on 2026-08-14 to keep it near
the 230-line rolling-memory ceiling.

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
