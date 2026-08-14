# llm-wiki Log

### 2026-08-14 - FIXED locally: stable FSR topmost completion ownership and one displayed-output timing observer

- Follow-up session `20260814_035452` tested 0.1.6051 and disproved the prior final claim. The two ~288-FPS blocks
  align exactly with the app-callback intervals (`16.173-22.138` and `28.152-34.146`): the FFX callback and CE's
  deep Present interception both advanced `PerformanceMetrics` for every displayed output. The paired timestamps
  produce alternating sub-millisecond/normal deltas, matching the user's spiky frame-time graph observation.
- Frame timing now has an independent exact owner. A deep-body Present observer suppresses the callback sample from
  the first output; the callback remains the fallback for the older runtime-owned topology that never re-enters CE
  through DXGI. FG-state publication and callback overlay rendering remain independent of that sampling decision.
- The same session's only steady no-callback ownership bounce occurred at `03:55:04.922`: one newly submitted marker
  was still in flight, so an all-markers-complete query erased already-established activation proof and temporarily
  restored the darker UI-resource baseline. Completion now latches for the current learned ECL-signature generation.
  The inline renderer rotates through any of 16 completed allocator/upload slots; each marker protects only its own
  slot, while a signature change or missed append resets the proof floor and requires a genuinely new probe.
- Focused policy/source-contract tests and the complete `--verify` gate pass on 0.1.6052 (full native suite, Python
  self-tests, lint ratchets, x64 ASan/UBSan, and package/privacy validation). Fresh hardware validation remains open.

### 2026-08-14 - SUPERSEDED: FSR topmost handoffs use marker-only activation before visible ownership

- User validation session `20260814_024908` (0.1.6049, Steam + RTSS) confirms the learned final-ECL-batch route
  keeps CE topmost under no-callback FSR FG. The log also showed two overlay owners per output: the FFX callback and
  the later topmost route each ran at about 144/s. The translucent background was therefore blended twice, while
  both paths advanced `PerformanceMetrics`, yielding about 288 FPS against an actual ~144/s CSV QPC cadence.
- Draw and frame-time ownership were initially transferred together. App-callback FSR keeps its callback baseline until one deep
  topmost submit succeeds, consumes exactly one proof at the next callback, and replenishes it only from the next
  successful deep Present; stopped/refused Presents therefore restore the callback without a timer or stale latch.
  No-callback FSR yields only after the existing inline completion marker proves the final-batch route.
- FG status publication is unchanged and remained correct in the source session (~72.6 base / ~145.2 output).
  Focused policy/source-contract tests cover proof selection, one-shot consumption, and the shared draw/FPS gate;
  the complete `--verify` gate passed on 0.1.6050 (native/Python tests, lint ratchets, x64 ASan/UBSan).
- Follow-up session `20260814_032638` (0.1.6050) visually confirmed topmost order and correct FPS but retained rare
  transition-only translucency flicker. `[OVERLAY DOUBLE-DRAW]` clustered exactly at topmost activation and routing
  edges: the first "proof" submission still rendered visibly over the UI/callback baseline, while callback-routing
  changes left the learned no-callback batch armed until the next Present observation.
- Both topmost families now start with a non-visible submission. The shared renderer records only its completion
  marker/fence when neither clear nor draw is requested—no RTV creation or target transition. No-callback FSR then
  requires marker completion plus successful CE-substitute retirement before proxy prework explicitly grants the
  final batch permission to draw; revocation precedes UI-baseline resumption. App-callback FSR arms its deep route
  with the same marker-only proof, and every callback-routing change synchronously clears both handoff epochs.

### 2026-08-14 - FIXED locally: retained Streamline-hook reconciliation and stateful DX12 hot-path diagnostics

- Clean validation session `20260814_014012` exposed diagnostic volume rather than a functional failure: 1,826
  low-level `already hooked by us` lines were paired with 1,827 Streamline `Failed to inline hook` lines after
  Streamline unload notifications cleared feature-level slots while the identical CE entry detours remained live.
- `InlineHook::TryGetInstalledTrampoline(...)` now validates CE's exact installed bytes with `ReadProcessMemory()`
  and returns the retained callable predecessor only for the same target/detour. Streamline rediscovery uses it to
  restore the original pointer, target slot, and installed flag once. Conflicting detours remain immediate failures;
  identical redundant low-level requests log the first four plus every 300th, and genuine repeated Streamline
  install failures log the first ten plus every 300th.
- Two independent per-frame no-op diagnostics were also reduced without hiding transitions: completed DX12 font
  uploads no longer log `No deferred upload needed`, while FSR proxy owner-queue HDR logs the initial samples, every
  format/color-space/support/HDR change, and a 600-call heartbeat. In the source session these paths produced 1,678
  and 1,197 stable-state lines respectively.
- Regression coverage pins retained-hook reconciliation, safe live-byte validation, failure cadence, removal of the
  font-upload no-op line, and state-change/heartbeat HDR diagnostics.

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
