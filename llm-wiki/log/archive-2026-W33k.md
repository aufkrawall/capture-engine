# llm-wiki Log Archive — 2026-W33k

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
