# llm-wiki Log Archive: 2026-W33j

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
