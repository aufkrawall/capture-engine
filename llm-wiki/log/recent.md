# llm-wiki Log

### 2026-10-01 - Witcher 3 + Steam: startup crash in Steam's CreateSwapChainForHwnd handler

- Session `20261001_042335`: AV executing 0x0 from `gameoverlayrenderer64!OverlayHookD3D3+0x14bc4` (`call r10`, r10=0),
  entered from `DetourCreateSwapChainForHwndInline` -> CE's prepend trampoline -> Steam relay `...038A`. Steam was
  loaded before CE (`active=gameoverlayrenderer64.dll`), CE installed 0.5 s after process start. Steam's per-hook original
  slots (relay stubs 0x40 apart) are all set except the `0380` stub CE captured: Steam abandoned that hook mid-install.
  Earlier sessions had CE injected before Steam (`active=none`) or Steam settled (`042507`, relay `02CA`, fine).
- Fix (0.1.6874): CSFH entry is sampled before patching; with a foreign jump and a deep hook placed, CE does not prepend.
  The deep hook runs `RunCreateSwapChainForHwndEntrySemantics` for CE-forwarded creates.
- `20261001_044010` (0.1.6874, no prepend): stack overflow, `OverlayHookD3D3+0x14bc4` recursing. Steam's CSFH original slot
  = `...03C0` while the entry jumps to `...038A`: Steam hooked CSFH twice, the second trampoline re-enters the first relay.
  The double install already existed at CE's first entry sample, 5 ms after CE's discovery factory ran Steam's
  CreateDXGIFactory1 handler on CE's hook thread. 0.1.6876: discovery factories bypass the foreign export patch
  (`GenuineCreateDXGIFactory1ForDiscovery`), like the temp swapchain and WARP device. Steam-internal cause is inferred,
  not proven; a clean run logs a foreign target ending `...02CA` in `is owned by a foreign patch`. Hardware run pending.
- `20261001_044939` (0.1.6876): no crash, but no Steam overlay. Steam had patched only the CreateDXGIFactory1 export when
  CE installed; CSFH read clean (`byte=0x40`), CE prepended, and Steam skipped the already-jumping entry when the game's
  factory creation ran its handler. `045157` (same build) worked: Steam's `E9 -> ...02CA` was already there. 0.1.6877:
  a loaded overlay owns the CSFH entry; deep hook at +16 with an RBP-restoring undo. Hardware run pending.

### 2026-10-01 - Witcher 3 `streamline_upgrade`: SR aliased in motion, DLSS-G generated nothing

- Session `20261001_040020` (0.1.6871): no crash. `sl.log` held 553x `ReflexNotDetectedAtRuntime ... -1 != N`,
  meaning no PRESENT marker ever arrived. 1.x sends Reflex markers and sleep as
  `slEvaluateFeature(nullptr, Reflex, frame, id=marker)`, and the bridge dropped every null-command-buffer
  evaluate. `slGetFeatureSettings(Reflex)` was refused, so the title kept Reflex mode 0.
- SR aliasing: 1.x `Boolean` is 1 byte (`sl.common` 1.5.6 compares bytes at +0x19c..0x1a3); the mirror read dwords,
  taking `cameraMotionIncluded` from `notRenderingGameFrames`. `V1Constants` is 432 bytes, not 456.
- Fix (0.1.6872): markers -> `slPCLSetMarker`, sleep -> `slReflexSleep`, Reflex settings answered, one token per
  frame index (`RecentFrameTokens`). New unit `streamline_bridge_reflex.cpp`.
- Validated in `20261001_041637`: 138 fps steady output from a 34.6 fps base (4x MFG), SR fine. Follow-up
  0.1.6873: FG off restores the title's Reflex mode instead of forcing off.

### 2026-10-01 - Witcher 3 `streamline_upgrade` startup crash: debug-layer setting reset the retained device

- Session `20261001_034038`: the retained bridge device was healthy at the 44.1 s probe and removed
  (`0x887A0007`) at the 47.5 s probe. There was no TDR. `witcher3+0x7f7a70` configures the debug layer
  (`ID3D12Debug5::SetEnableAutoName(TRUE)`) right before that probe, and only the bridge kept the probe
  device alive through it. The reset in runs four to six had the same cause.
- Fix (0.1.6871): `Bridged_D3D12GetDebugInterface` refuses the ID3D12Debug family with
  `DXGI_ERROR_SDK_COMPONENT_MISSING` while the device cache retains a device. Native creation moved
  to `streamline_bridge_native_device.cpp` (bridge unit was at 799 lines). Detail in
  `frame-generation/streamline-generation-bridge.md`. Hardware run pending.

### 2026-10-01 - Witcher 3 DX12 + Smooth Motion startup int 3: CE's hardware temp D3D11 device

- Session `20261001_032227`: `int 3` in NvPresent64 on CE's hook thread (0x4690), 7 s after launch.
  NvPresent64's Detours `DetourTransactionCommit` returned `ERROR_INVALID_OPERATION` while the game's
  main thread was inside `D3D12CreateDevice` (NVIDIA UMD init). CE's thread was in the `DX11Hook::Init`
  temp probe, creating a hardware D3D11 device. That probe ran because the game's own device creation
  mapped `d3d11.dll` (an NvPresent64 import) before `D3D12CreateDevice` returned.
- Fix: the DX11/D3D10 temp probe is WARP-only, as the DX12 bootstrap already was. A new unit test
  compares all WARP and hardware vtable slots for the D3D11 device, context and swapchain, and for the
  D3D10 device; it passed on this NVIDIA host with no skip. Detail in `present-interposers.md`.
- Same audit: the D3D10 sampler hook patched `ID3D10Device` slot 9 (`Draw`); it now patches slot 86.
- Pending: a hardware run of Witcher 3 DX12 with Smooth Motion. The DX11 classification in DX12
  processes is unchanged (open question).

### 2026-09-30 - Stable 0.1.6868 published with all five assets attested

- `release-stable.yml` run 36700494306 built exact commit `a058e822` with `attest=always` and
  `--verify --verify-clean`. Native/Python suites, x64 ASan/UBSan, PE/privacy checks, packaging,
  and the full 887-unit analyzer ratchet passed. Runtime integration, fuzz, and test-app execution
  remain outside this static action's coverage; x86 sanitizer runtime remains unavailable.
- All five published asset sizes and SHA-256 digests matched the runner outputs. Attestations verified
  against the release workflow, `refs/heads/main`, and the exact source/signer commit. The cleanup action
  succeeded for this release. No release or tag was overwritten; the two earlier attempts were cancelled.
- Post-publication promotion exposed `re.PatternError: bad escape` from a Windows path in notes:
  `promote_unreleased` passed the note text as a regex replacement template. A callable replacement now
  preserves every backslash literally, with coverage for Windows/UNC paths and backreference-like text.
  The published release section is promoted separately from this next-cycle tooling fix.
- Promoted notes match the published body exactly. The new promotion regression fails on the old
  implementation; 61 focused tool tests, lint, and combined product gate 0.1.6869 passed after the fix.
  All three attempt logs returned 404, and the one-job runner exited with no listener/worker left behind.

### 2026-09-30 - Stable release preflight: unused limiter helpers and host-timed GPU test

- Release preparation found `clang-diagnostic-unused-function` at 76 > 74: the new runtime-output suite
  included two unused manual rearm helpers from `test_fps_limiter_shared.h`. They now live in
  `test_fps_limiter_sleep_mode_recorder.h`, included only by `test_fps_limiter.cpp` and `test_fps_limiter_part2.cpp`.
- `GpuWorkRunningPastTheDeadlineGrowsTheReservation` also saturated its budget before supplying GPU delay
  on a busy host, despite its CPU-work override. The scenario now runs in the existing isolated clock/wait
  fixture in `test_fps_limiter_runtime_output_bursts.cpp`; production pacing is unchanged.
- Focused FPS-limiter tests and combined product gate 0.1.6868 passed after the move. Full-database clang-tidy
  passed and automatically tightened unused-function 74 -> 68 and unused-private-field 109 -> 4;
  existing formatter advisories remain outside the changed code. Workflow/changelog regressions passed.
- The cancelled attempt's cleanup action failed with `unexpected end of JSON input`; its log still returned 302.
  Direct deletion returned 204 and verification returned 404. Both workflow deletion paths now use curl,
  with regression tests that fail on the old scripts and preserve the independent missing-log check.
- The release runner's native suite passed, but `ReleaseLogCleanupPolicyTest` still pinned the old CLI
  command and blocked Python tool verification. Its wiring assertion now requires direct HTTP deletion;
  run the entire Python tool suite after workflow edits, including `privacy_paths`, before dispatching.
- The corrected wiring passed the complete native and Python tool suites through the no-build gate
  at product identity 0.1.6868; the corrected cleanup action also removed the second cancelled run's log.

### 2026-09-30 - Proven FSR outputs still escaped the FPS cap through duplicate filtering

- Review confirmed a 100 us burst skipped every later cadence slot under the general cap, including when a lower
  general cap won over capture sync. Native handoff's inactive dedup skipped the next output too.
- `ShouldUseDuplicatePresentWindow` now separates logical-frame duplication from cadence-lock contention:
  callback-proven runtime outputs bypass both duplicate filters while retaining the presenter's try-lock.
- Five deterministic burst tests exercise real `Apply`/`ApplyPostPresent` with isolated clock/wait fakes; the general
  cap, lower-general-cap recording, and native-handoff cases fail before the fix. Capture-sync and legacy duplicate
  behavior are controls. A policy matrix covers every site with FG on/off.
- Verified: combined incremental product gate 0.1.6867, full native suite (4,128 tests), Python tool self-tests, and
  binary checks passed. New burst fixture adds no compiler warnings; non-hook Reflex trampoline fields are marked
  intentionally unused without changing layout. Hardware burst validation remains pending.

### 2026-09-30 - FPS limiter modelled callback-owned FSR FG outputs as base frames

- Same session (`logs/20260930_032355`): the limiter's DXGI site sees every FSR runtime output, but counted as
  `kUniqueApplicationPresent`. Capture sync ran `effective=60` per output until media's handshake (~1.2 s, half
  display rate at every recording start), then `captureSource=base captureEq=240`; a general cap alone divided per
  output (120 -> 60 displayed; unit test fails on the old code).
- Fix (0.1.6866): `PresentSite::kRuntimeOutputPresent` from the verdict read at Present entry; final-output capture
  source; per-output present grid, rational render grid for the Reflex hybrid spin. Details in
  `frame-pacing-and-limiter.md`. Hardware validation pending: expect `captureSource=final site=3 group=120/1` from
  the first ACTIVE line and no retarget at the handshake.

### 2026-09-30 - FSR FG recordings had half the motion rate: capture skipped generated outputs

- `logs/20260930_032355` (0.1.6863, Talos, callback-owned FSR FG, capture sync basic 120): on screen 120 fps, but
  media `Inject Perf` showed `Input: 60 | Dup: 60` per second. The callback verdict admitted only
  `callback=application` Presents, although every runtime output (generated too) passes CE's Present on AMD's
  presenter thread with the displayed image in the backbuffer.
- Fix (0.1.6864): `dx12_overlay_policy::IsPresentedFrameForCapture` captures every callback-proven output; the
  no-verdict ECL-count gate is unchanged. Tests: `Dx12EclQueueRegistrationPolicyTest.CallbackProvenGeneratedOutputsAreCaptured`
  and a source check that the wrapper does not re-filter on `verdict.generated`.
- Hardware validation pending: expect `Input: ~120 | Dup: ~0` in the media log and
  `generatedOutputs`/`applicationOutputs` near equal in hook_debug.log. Open: the limiter still reports
  `captureSource=base` on this route (see `cfr-capture-sync.md`).

### 2026-09-30 - Review of v0.1.6772..HEAD: robustness follow-ups

- Reviewed the 68 commits after v0.1.6772 (source diffs only; the four risk-audit commits were not re-read). No
  correctness bug found; eight low-severity hazards fixed with regression tests:
  DX12 inject capture begins its transport generation BEFORE `Initialize()` closes the old handles
  (`ShouldBeginInjectTransportGeneration`); parked hidden-window queues are released when their window is destroyed
  (`Ledger::ForgetWhereWindowGone`, swept on every park and every 256th unmatched Present); the caller-module cache
  pins the module across its header read (`ResolveFromLoader(..., &pinned)`); the UTF-8 config cache confirms
  "racy" entries (written < 2.5 s ago) byte-for-byte (`IsRacyFileTimestamp`, git's racy-index rule); the failed-resize
  holder scan stops at 3 s (`kScanBudgetUs`); `UnregisterCrashPreDumpCallback` removes only its own callback.
- **False alarm worth remembering:** a first reading claimed the `Phase6Tail` overlay-free fallback capture could carry
  CE's overlay because of the independent below-foreign-chain composite. That route is dead code
  (`DecideBelowForeignChainFSRDeepDraw` is a stub returning `kUnavailable` since `cff7a507`); see the SUPERSEDED note in
  `dx12-overlay-third-party-coexistence.md`. Whether the FFX present callback's composite reaches the captured proxy
  back buffer stays the open hardware question from 2026-09-29.
- Release preflight caught `common/crash_dump_policy.h` at 801 lines (already over at HEAD, from `d5b878f0`): the WER
  adoption/registration helpers moved to `common/crash_dump_wer_policy.h` (still included by the old header).
- The first full `--verify` since 0.1.6772 (sanitizers are skipped by the per-change gate) found a real bug at HEAD:
  `SteamOverlayInitVehHandler` (process-wide, runs first for every exception) read a `thread_local`; loader worker
  threads (`LdrpProcessWork`) have no TLS block, so the read faulted inside the handler and recursed to stack
  overflow (silent 0xC0000005 under ASan, moved between tests). Fix: `ce::steam_recovery::g_armedThreads` (thread-id
  set, no TLS) is consulted first, and the handler is `no_sanitize("address")`. Lesson: any process-wide VEH must be
  TLS-free and uninstrumented until it knows the thread is one it armed.
- Deliberately unchanged: `IsUsableHostDirectory` still accepts UNC paths (`test_vulkan_layer_host_directory.cpp` pins
  it; a network-share install is legitimate and the pointer file has the staged layer's trust level); the duplicated
  rejected-timestamp block in `audio_capture_loop.cpp` / `app_audio_capture_loop.cpp` (refactoring audio-critical
  loops for a maintainability gain is not worth the risk without a hardware run).

### 2026-09-30 - Review fixes: full-queue disk timeout and FFX create-byte restoration lifetime

- Full mux queues now service `CancelExpiredOutputIo("backpressure")` outside the queue lock;
  a stalled writer no longer prevents the encoder from reaching the stop/join boundary.
- FFX create suspension/retargeting pins the old export image outside the breakpoint mutex,
  revalidates the binding, and restores while pinned. Missing images are never dereferenced.
- Three source regressions fail before the fixes; native `FFXExportLifetimeTest` proves last-owner
  unload cannot unmap a pinned export and rejection paths release their references. Combined incremental
  product build 0.1.6857, full native suite and Python tool self-tests passed; game validation pending.
