# llm-wiki Log

### 2026-08-14 - FIXED locally: early Steam + RTSS FG switch crashed in proactive Streamline lookup and stranded overlay startup

- Session `20260814_012102` (0.1.6045, Steam overlay + RTSS, FG switched before CE overlay initialization) has a
  genuine CE crash dump: the HookThread executed an unmapped address inside the just-unloaded `sl.reflex.dll` via
  `sl_interposer!slGetFeatureFunction -> TryResolveReflexFeatureHooks`, while the app thread was in `slShutdown`.
- Root cause: `ScopedStreamlineFeatureQueryGuard` correctly pinned every loaded `sl.*` module, but its lexical scope
  ended at the proactive-scan `if` block. Its destructor released every pin before the first feature query. Both
  DLSS-G and Reflex guards now live for the complete resolver function, covering every query and pointer validation.
- Independent visibility root cause in the same session: an early official FFX startup swapchain armed the protected
  pre-configure quiesce latch; the app then successfully enabled DLSS-G without ever issuing enabled `ffxConfigure`.
  The abandoned provisional FFX latch kept overlay sync idle forever. A successful explicit Streamline enable now
  retires the staged FFX queue/startup state only when authoritative FSR is not active, before DLSS ON publication.
- Regression coverage pins the guard scope, successful-result wiring, FSR-active exclusion, and full FFX-startup
  cleanup. Focused Streamline/DXGI policy tests pass; full `--verify` passed on 0.1.6046 (native tests, Python tool
  self-tests, clang-tidy/file-size ratchets, x64 ASan/UBSan, product binary verification, and packaging).

### 2026-08-14 - FIXED locally in CaptureEngine: preserve DX12 COM identity below Steam + RTSS; isolate D3D11 probes

- Scope correction: the earlier switch-app persistent-initial-FFX-proxy change was a workaround in the test client,
  not a CaptureEngine compatibility fix. It has been fully reverted. Session `20260814_004913` proved it also did
  not survive an OFF teardown followed by FSR re-entry.
- Both session dumps are controlled diagnostics, not memory AVs. The exhaustion dump is official FFX -> CE global
  factory hook -> Steam -> RTSS -> CE deep create -> DXGI returning `E_ACCESSDENIED`; the second dump is the app's
  deliberate `ExitProcess(0xE000EACC)` after that failed switch.
- Joint engine root cause: CE had already installed complete Present/Present1 deep hooks below the two-overlay entry
  chain, but still returned a retaining `CWrapDXGISwapChain`. That redundant proxy changed COM identity and mirrored
  release traffic above Steam/RTSS. Its teardown left four foreign refs on the real FFX chain. During the same run,
  RTSS command submissions stopped while Steam submissions continued, consistent with the extra wrapper transport
  perturbing the foreign chain it was meant to preserve.
- Fix: with two or more loaded overlays and complete deep-body interception, both DX12 creation paths and the wrapped
  factory return the original DX12 swapchain unchanged. CE still renders/captures through the deep hooks after every
  foreign overlay. Incomplete deep coverage, D3D11/10, single-overlay, and special Streamline runtime paths keep their
  established fallback behavior.
- A second engine bug amplified the collision: RTSS loaded D3D11On12, HookThread ran CE's D3D11 temp-swapchain probe,
  and the globally hooked factory wrapped/tracked that internal probe as a DX12 game swapchain. Internal discovery is
  now thread-locally marked and bypasses DX12 side effects; successful D3D12 Present-device evidence is published
  immediately so transitive `d3d11.dll` loading cannot start the DLL-only fallback in an active DX12 process.
- Regression tests: pure topology policy plus source invariants for both global/wrapped factory return paths, the
  thread-local probe bypass, and early actual-D3D12 publication. Focused tests passed; full `--verify` passed on
  0.1.6045 (2,183 native tests, Python tool self-tests, clang-tidy/file-size ratchets, and ASan/UBSan).

### 2026-08-14 - FIXED: dump-path freeze + FSR re-entry pin — helper-first dumps, FSR swapchain keep-alive, lifetime attribution

- Session `20260813_222058` (0.1.6029, dx12_fg_switch_test via Steam overlay + RTSS): the second OFF->FSR failed
  E_ACCESSDENIED again, and the app's own fatal IN-PROCESS dump then froze the render thread (cdb:
  `dbgcore!MiniDumpWriteDump -> GetFileVersionInfoW -> gameoverlayrenderer64` blocked; 0-byte `.dmp.inprogress`;
  the FreezeWatchdog killed the app after 30 s). Re-entering MiniDumpWriteDump inside the game's dump call
  deadlocks against Steam's hooked version APIs, so every nested/in-process dump is unsafe with foreign overlays.
- Fixes: (1) the session mirror is now a plain stream copy of the game's COMPLETED dump file — never a nested
  MiniDumpWriteDump; the rich supplemental dump goes through the external helper; (2) CE + testapp in-process
  fallbacks are refused while third-party overlay modules are loaded (policy
  `ShouldUseInProcessMiniDumpFallbackAfterExternalHelperFailure`); (3) the testapp's `WriteFatalSwitchDump`
  prefers the external helper and keeps the light in-process fallback only for overlay-free runs. Validated: the
  fatal path now yields `crash_external_dx12_fg_switch_test_fatal_switch_*.dmp` without freezing.
- Pin root cause (proven locally with a standalone dxgi probe): `CreateSwapChainForHwnd` returns 0x80070005 while
  ANY chain for the HWND is still alive (also from a different factory), and the residual references never drain
  within reasonable time after the game+CE released everything. New lifetime attribution hooks (CE-only gated)
  attribute every remaining AddRef/Release to the calling module; the permanent holder is foreign per-chain
  bookkeeping (Steam/RTSS) plus one internal ref. Retry-waiting cannot release them — no timing bandaid accepted.
- Fix (proper): the switch app keeps the FSR swapchain/context alive across OFF<->FSR (real-FSR3-game lifecycle:
  FG off = passthrough Present through the FFX proxy; re-entry = `ffxConfigure` only). No replacement create, so
  the E_ACCESSDENIED wall is never hit. Locally validated OFF->FSR->OFF->FSR->OFF (all ok=1, same chain pointer,
  clean presents). The FSR->DLSS direction still recreates the native chain and remains blocked by the pin.
- Diagnostics added: vtable-slot ownership logging (AddRef/Release/Present/ResizeBuffers owner module),
  create/post-destruction refcount probes, and per-module AddRef/Release attribution. The first attribution
  implementation dropped AddRef/Release for unknown chains on the shared vtable and corrupted dxgi's heap
  (0xC0000409 in dxgi telemetry); the hooks now ALWAYS forward to the saved original.
- Tests: `CrashDumpPolicyTest.InProcessDumpFallbackRefusedWithForeignOverlayLoaded` + source-invariant pins for
  the copy-based mirror, helper-first app dump, keep-alive branch, and attribution forward invariant.
  `--verify` gate passed on 0.1.6039. OPEN: user re-run via Steam + RTSS — FSR->OFF->FSR->OFF must switch cleanly
  without recreation; the next run's attribution names the exact foreign holder for the FSR->DLSS direction.

### 2026-08-13 - FIXED (freeze): the new exhaustion minidump ran in-process and froze the app ~36 s; dumps now go through the external helper (DLSS switch also proved the 1-foreign-ref pin)

- Session `20260813_220022` (build 0.1.6027, dx12_fg_switch_test via Steam overlay + RTSS, switch to DLSS FG):
  Streamline's `linkSwapchainToCmdQueue` failed E_ACCESSDENIED ("Zugriff verweigert"), the app's native fallback
  failed identically, and the process appeared to FREEZE for ~36 s before the user killed it. This time the session
  kept dumps: `dx12_fg_switch_test.exe_2026-08-13_22-00-56.dmp` (FreezeWatchdog) and
  `crash_external_swapchain_access_denied_exhausted_f3172865.dmp` (the new exhaustion dump).
- Freeze root cause (cdb-confirmed): the exhaustion dump ran MiniDumpWriteDump IN-PROCESS on the render thread with
  the quick-assert flags (DataSegs/IndirectlyReferencedMemory/FullMemoryInfo) - a 114 MB capture took ~36 s and
  tripped the FreezeWatchdog. Render-thread stack: `DeepHookCreateSwapChainForHwnd ->
  CaptureCreateSwapchainAccessDeniedExhaustedDump -> WriteSupplementalCrashDump -> HookedMiniDumpWriteDump ->
  dbgcore!MiniDumpWriteDump`; the create chain below proves Steam AND RTSS both processed this create
  (`DetourCreateSwapChainForHwndGlobal -> gameoverlayrenderer64!OverlayHookD3D3 -> RTSSHooks64 -> DeepHook`).
- Pin evidence (same family as FSR): `SwapChain: post-destruction real refcount=1` - exactly ONE foreign reference
  pins the old chain after the game and CE released everything; pre-cleanup probe refs=2 (incl. probe), post-cleanup
  and post-entry-retry probes show no tracked chains. The live-entry retry ran RTSS's handler again without
  releasing it.
- Fix: `CaptureCreateSwapchainAccessDeniedExhaustedDump` moved to the main dump layer (`hook/main_fatal_dump.cpp`)
  and now prefers the EXTERNAL dump helper (`captureengine.exe --dump-helper` - the game's threads are never
  suspended for a large capture), falling back to a minimal in-process MiniDumpNormal-class dump when the helper is
  unavailable. The test app's own fatal dump is now MiniDumpNormal+ThreadInfo+UnloadedModules instead of the heavy
  data-segment scan.
- Tests: `AccessDeniedExhaustionWritesDiagnosticDumpAndBracketedPinProbes` pins the external-helper-first order and
  the light fallback (`tests/test_dxgi_shared_access_denied_dump.cpp`). `--verify` gate passed on 0.1.6029.
  OPEN: user re-run - no freeze expected on the failure path; the dump plus probes should finally attribute the
  single remaining foreign reference.
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
