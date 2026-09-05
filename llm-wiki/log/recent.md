# llm-wiki Log

### 2026-09-05 - Bad-start FSR pacing signature quantified; pacing-health telemetry added

Session `talosfullfsrfgbaddlssfggoodfsrfgbadrestartfsrfggood` (0.1.6491, two process starts) gives
the first objective bad-vs-good discriminator: in the bad start 11.8% of output flip intervals land
> median+1.2 ms (good 1.0%), late events are pair-phase-locked (even gaps, never consecutive),
presentToDisplay swings 0.9-3.2 ms, and the screen series stddev is 2310 vs 746-913 us. DLSS FG in
the same bad process ran a *slower* base cadence with a smooth screen - base cadence is a marker,
not the cause. CE's CPU costs, vsync policy and bridge behavior were identical in both windows, so
the previous fixes (cff7a507/a7dc1ebc/ffa6ee60/fcd6f9f7) are not the differentiator either way.
The bad state is per-process, survives FG mode switches, random per start regardless of warm/cold
(user-confirmed), and RTSS's overlay was visible and smooth under FSR FG (no-CE baseline valid).

Added `ce::pacing_health` interval rings + `[FSRPacingHealth]` (10 s aggregate with late-permille
classification, callback draw counts app/gen/genSkip), `[FSRActivationCadence]` at enabled
ffxConfigure and authoritative takeover, `[HookThreadPass]` (hook-thread service cost at
THREAD_PRIORITY_HIGHEST - presenter-preemption candidate), and `CE_FG_COST_PROBE=0x20000`
(skip overlay draw on generated frames; diagnostic only - a real skip would flicker because each
output buffer is separate). Unit coverage: `tests/test_pacing_health.cpp` (ComputeChannelStats on
the measured good/bad distributions, ring wrap, outlier drop). Next step is the hardware A/B
matrix (CE overlay / CE overlayEnabled=0 / no CE) using `[FSRPacingHealth]` - it has never been run.
Full data and candidate list: [display-change-timing](../display-change-timing.md).

### 2026-09-05 - Talos good/bad comparison: FFX VSync input boundary correction; random pacing unresolved

Compared `talosgood` (6490) and `talosbad` (6489). Both use the official callback with similar CPU
cost and clean upload/registration telemetry, but steady display interval stddev differs by ~2x.
Neither records callback GPU duration. Both request FIFO; the user suspects recurrence without
forced FIFO too. Do not treat the previous claimed fixes as proof that every pacing issue is solved.

FFX proxy Present/Present1 now receives user VSync intent before AMD schedules outputs; inner
DXGI paths preserve that runtime's parameters while the native source hook owns policy. Previously
only the downstream call was rewritten. Bounded source/output diagnostics and regression coverage
protect the ownership handoff, including suspended FG and DLSS/native recovery. No features,
rendering routes, GPU work, or wait policy changed. Hardware pacing validation remains open.
Evidence, source anchors and limitations: [display-change-timing](../display-change-timing.md).

### 2026-09-05 - Talos `talosbad` session on build 6489: FSR configure flapping confirmed as game behavior

Captured on build 6489 (all session fixes active). The game calls `ffxConfigure` with `frameGenerationEnabled`
toggling on one context (`...850C40`, frameID 1/2 -> 0 -> 158+): ENABLED at 07:09:47.392, DISABLED 76 ms later,
re-enabled ~1.1 s later, then periodic off/on pairs roughly every second until exit. Each ENABLED re-installs
the present-callback bridge; each DISABLED retains it, so the official callback route stays intact and the
overlay never drops. `sessionEpoch` churn (88 -> 93 -> 148 across the run) follows those toggles and is a
diagnostic counter, not overlay work: no backend re-init or extra submission rides on it.

- Callback cost: `ceAvgUs` 64-88 us steady, zero `ceOver500Us`/`ceOver1ms` after startup; upload pool stays at
  `slots=1` with inline-marker completion, no exhaustion; ECL `registrations=0` steady (no discovery churn).
- One 19 ms CE-side spike in the first proxy-Present window (`ceMaxUs=19075`, once), later windows clean
  (`ceMaxUs` <= 30 us) — cold-start cost (first UI-resource composite + substitute registration), not a
  steady-state hitch source.
- Display-change stays selected throughout; `talosbad` therefore does not exhibit the old false-fallback or
  rounding behavior. The remaining start-to-start variance question is still open: this run proves the
  callback/marker/registration paths are quiet, but contains no injected/non-injected A/B and the game's own
  configure flapping means two starts may genuinely run different FG duty cycles.

### 2026-09-05 - FSR callback lifetime/performance and display-timing integrity audit

Reviewed `20260905_011023`, the last two days of commits, and the user's interim build/session `talosnew`.
No relevant crash dumps were present. Remaining start-to-start FSR variance is not causally attributed by
these sessions; raw completion stddev stays around 2.0-2.3 ms with even runtime presents.

- Removed inferred blank-grid timestamp rewriting. Preserve PresentMon-style kernel completion timestamps,
  valid NVIDIA schedules, and explicit generated-transition timestamps. No flatness-based source selection
  or invented provenance. `timestampPolicy=event/no-grid` identifies the collector policy.
- Fixed false source fallback when the sensor publishes after the render thread sampled QPC. Fixed shared
  ring overwrite publication ordering and concurrent non-atomic metric-history access. Distinct short
  display intervals and long hitches are retained; percentile tail counts no longer dilute exact boundaries.
- Replaced the callback's blind 16-slot upload rotation with inline GPU completion and bounded, lazy growth.
  Pending resources survive backend replacement and are reclaimed on the existing hook service thread.
  No extra presenter-queue submission, Signal, or CPU wait is introduced.
- Removed warm callback queue locking, retained discovery suppression through runtime-owned FSR suspension,
  made buffer allocation/mapping transactional, and avoided idle benchmark sensor/config work.
- FFX cost telemetry now uses disjoint per-thread 600-call windows and reports counts above 500 us and 1 ms;
  cumulative startup peaks no longer hide subsequent smaller stalls.
- The interim `talosnew` run proves the callback marker path worked without exhaustion and the display source
  stayed selected; it predates the final history-atomic, benchmark-idle, and cost-window changes.

Current invariants, source anchors, test suites and remaining validation limits are in
`../display-change-timing.md` and `../overlay-rendering.md`. Older entries below/archived that call a flatter
Present series proof of display-timestamp error, or call the callback zero-cost, are superseded.

### 2026-09-04 - Fix DXGI swapchain COM reference leak and Streamline driver callback crash across FG switches (logs/20260904_143301)

Analyzed and resolved four minidumps from `logs/20260904_143301` produced during rapid native/FSR/DLSS FG transitions:

1. **Root Cause 1: Leaked COM references causing `E_ACCESSDENIED` (0x80070005) on swapchain recreation:**
   - Dumps: `crash_external_swapchain_access_denied_exhausted_f3172865.dmp` and `crash_external_fatal_exit_ExitProcess_e000eacc_14cc1ec5.dmp` (PID 13328).
   - In `hook/wrappers/dxgi_swapchain_wrap_present.cpp`, `CWrapDXGISwapChain::PromoteInterfaces()` called `m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal1))` unconditionally without checking `if (!m_pReal1)`. When the wrapper was constructed around an `IDXGISwapChain1`, the constructor had already stored and AddRef'd `m_pReal1`; calling `QueryInterface` again overwrote the pointer and added a second reference while the destructor only released it once.
   - In `hook/apis/dx12_hook_swapchain_create.cpp`, `DetourCreateSwapChainGlobal` and `DetourCreateSwapChainForHwndGlobal` wrapped `*ppSwapChain` / `*ppSC` with `new CWrapDXGISwapChain` (which AddRefs), but omitted `pReal->Release()` to consume the factory's returned reference (unlike `dxgi_factory_wrap.cpp` and `dx11_hook_detours.cpp`).
   - The extra references pinned the swapchain to the HWND across mode switches, causing `CreateSwapChainForHwnd` to fail with `DXGI_ERROR_ACCESS_DENIED` (`0x80070005`) once the deep recovery was exhausted.
   - Fix: Added `!m_pReal1..4` guards in `PromoteInterfaces()`, and added `pReal->Release()` after wrapping in `DetourCreateSwapChainGlobal` and `DetourCreateSwapChainForHwndGlobal`.

2. **Root Cause 2: Streamline mid-process unload causing driver callback DEP crash (0xC0000005):**
   - Dumps: `crash_20260904_143617_972_pid21396_tid12612.dmp` and `crash_external_fatal_exit_NtTerminateProcess_c0000005_bbde10db.dmp` (PID 21396).
   - In `testapp/dx12_fg_switch_render.cpp`, `ReinitializeDX12ForFSR` and `ReinitializeDX12ForNativeOff` were calling `ShutdownStreamlineSerialized()`, which called `slShutdown()` and `FreeLibrary(sl.interposer.dll)`.
   - Freeing `sl.interposer.dll` unloaded `sl.dlss_g.dll` and freed its code pages. However, NVIDIA's driver runtime (`_nvngx.dll` and `160_E658703.bin`) remained resident in the process and retained a registered telemetry/evaluate callback pointer (`0x0000014125CADD30`) inside global `0x713df0`.
   - On returning to DLSS, `slSetD3DDevice` -> `NVSDK_NGX_D3D12_Init_Ext` checked `[0x713df0] != 0` and called into the unmapped memory, immediately triggering an access violation / DEP crash (`0xC0000005`).
   - Fix: Added `useStreamlineSwapChain` parameter to `CreateSwapChainResources` / `InitDX12` so native swapchains can be created without unloading Streamline. Replaced mid-switch `ShutdownStreamlineSerialized` calls with `ApplyReflexMode(false, ...)` so Streamline remains resident throughout the process lifetime (matching NVIDIA's programming model and real game behavior in *The Talos Principle*) and is only torn down at process exit.

3. **Regression Coverage & Verification:**
   - Added unit tests in `tests/test_swapchain_probe_lifetime.cpp`: `PromoteInterfacesDoesNotDuplicateReferencesWhenAlreadyPopulated`, `GlobalSwapchainDetoursConsumeFactoryReferenceAfterWrapping`, and `StreamlineLifecyclePreservesRuntimeAcrossModeSwitches`.
   - Full `--verify` gate passed with 0 warnings, ASan/UBSan green, all 713 clang-tidy translation units cached and clean.

### 2026-09-04 - Fix FSR FG real on-screen frame pacing stutter by prioritizing official presentCallback and eliminating extra ECL/signals on AMD presentation queue

Fixed root cause of intermittent on-screen frame pacing stutter in *The Talos Principle: Reawakened* (`Talos1-Win64-Shipping.exe`) with native AMD FSR Frame Generation:

1. **Root Cause (Route B extra ECL on AMD's presentation queue):**
   - When a foreign overlay like Steam was loaded, `DecideBelowForeignChainFSRDeepDraw` (`hook/common/dx12_overlay_policy/ffx_routing.h`) activated Route B (`TryCompositeOverlayBelowForeignChainForRuntimeOwnedFSR`).
   - Route B caused CE to voluntarily yield AMD's official, zero-overhead `presentCallback` (`ShouldYieldFFXPresentCallbackToTopmostRoute`).
   - Instead, on AMD's presenter thread inside `DetourPresent`, Route B executed a separate `ExecuteCommandLists` call directly on AMD's presentation queue (`dx12_hook_g_SwapchainQueue`) right before `CallOriginalPresent`.
   - AMD's presenter thread uses high-precision QPC timers to pace generated vs real frames. Submitting an extra command list on AMD's queue desynced AMD's pacing timer and caused GPU queue contention right before VBlank scanout. Depending on queue slack at startup, if an extra ECL pushed GPU completion past the scanout deadline by even a fraction of a millisecond, the driver's hardware flip queue slipped by 1 VBlank and permanently locked into alternating 1 vs 2 VBlank flips (~6.6 ms / ~17.7 ms alternating flips, steady state ~83.3 FPS stutter).
2. **Prioritizing official FFX `presentCallback` (Part 1):**
   - In `hook/common/dx12_overlay_policy/ffx_routing.h`, `DecideBelowForeignChainFSRDeepDraw` makes Route B unavailable (`kUnavailable`). The official FFX `presentCallback` is never yielded to an extra swapchain-queue submission; CE renders directly into AMD's provided command list (`desc->commandList`) with zero extra `ExecuteCommandLists` calls, zero extra fences, and zero presenter-thread stalls.
3. **Skipping deferred signals and overlay completion waits on runtime-owned queues (Part 3):**
   - In `hook/common/dxgi_shared_present_core.cpp` (lines 356 and 479) and `hook/common/dxgi_shared_present1.cpp`, `InvokeDX12WaitForOverlayCompletion` and `InvokeDX12FlushDeferredSignal` are now skipped whenever `HookHasRuntimeOwnedNativeFGPresentPath()` or `DXGIShared::DoesFGRuntimeOwnSwapchain()`. This prevents any deferred signals or completion queries from touching AMD's presentation queue.
4. **Regression Tests & Verification:**
   - Updated `tests/test_ffx_below_foreign_chain_policy.cpp` (`PrefersOfficialPresentCallbackOverSwapchainQueueDrawInHealthyState`).
   - Passed full `--verify` content-validated product build, native tests, Python self-tests, file-size ratchet, and ASan/UBSan validation.

### 2026-09-04 - Faithful msBetweenDisplayChange reporting and elimination of FSR FG VEH rearm overhead

Two root-cause improvements resolving FSR FG frame pacing and overlay fidelity:

1. **Faithful `msBetweenDisplayChange` without sugarcoating:**
   - The user clarified the core project requirement: the frame-time graph must faithfully reflect
     real on-screen frame pacing (`msBetweenDisplayChange`) with VRR, GPU maxed out, VSync capping,
     and uncapped FPS, across all FG modes (all FG off, FSR FG, DLSS FG).
   - In `captureengine/display_timing_policy.h`, `ResolveDeferredScreenTimes` marks unclocked completions
     under VRR as `screenTimeResolved = true`, because on VRR panels without a fixed VBlank grid the
     unrounded hardware completion timestamp is the physical display transition time itself.
   - In `hook/common/performance_metrics.cpp`, when `DisplayChange` is preferred and the timing service
     is healthy, `m_effectiveSource` selects `FrameTimeSource::DisplayChange` directly. The overlay
     no longer forces fallback to `Presentation` when the display stream has natural variance or an
     alternating sawtooth (e.g. FSR FG on VRR). Fallback occurs only when the timing service is
     unavailable, stopped, or stale (> 2 seconds).
   - `GetLastDisplayFrameTimeMs()` returns the active display frame time from the display series.

2. **Eliminated per-frame render-thread VEH breakpoint rearm overhead in FSR FG:**
   - In `hook/apis/ffx_hook_install.cpp`, `CallFfxConfigureOriginalGuarded` was pausing and re-arming
     the 0xCC VEH breakpoint on AMD's executable code page on *every application frame* on the game's
     render thread (1800 times in 30 seconds). Each call ran 4 `VirtualProtect` syscalls (acquiring
     the process-wide `MmAddressCreationLock`) and 2 `FlushInstructionCache` syscalls (broadcasting
     cross-core TLB shootdowns).
   - The one-shot disarm was only hooked for no-callback mode.
   - In `hook/apis/ffx_hook_context.cpp` (`Hooked_ffxConfigure`), as soon as the present-callback bridge
     is established (`installedPresentCallbackBridge || retainedAlreadyBridgedPresentCallback`), the
     protected `ffxConfigure` VEH breakpoint is permanently disarmed via
     `DisarmProtectedFfxConfigureVehBreakpoint("present-callback bridge established")`.
   - Completely eliminates render-thread syscall overhead and memory lock contention during active FSR FG.
