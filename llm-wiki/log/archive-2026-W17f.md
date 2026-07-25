# llm-wiki Log — Archive 2026-W17f

### 2026-04-23 - Fix overlay disappearing when entering GTA settings after stale runtime-owned swapchain cleanup

- **Motivation**: GTA V Enhanced `installed/captureengine/logs/20260423_042441` on build `0.1.2550` showed the overlay disappearing when entering the settings menu (which loads DLSS FG DLLs). The trace showed `PostSL first ECL submit approaching` repeatedly followed by `DX12: PostSL refusing no-wrapper virtual bootstrap during Streamline FG (queue=%p scQueue=0000000000000000 realQ=0000000000000000 realECL=%p)`.

- **Root cause analysis**:
  1. During normal gameplay, a late Epic EOS overlay created an authoritative Streamline swapchain handoff → `scQueue=000001221006B030` was captured.
  2. After 120 consecutive real frames on `origGame` with no FG active, CE cleared the stale runtime-owned no-FG swapchain: `Releasing stale runtime-owned Streamline no-FG swapchain queue 000001221006B030`. `g_SwapchainQueue` became `nullptr`.
  3. User enters settings menu. GTA loads DLSS FG DLLs. `slDLSSGSetOptions(ON)` fires, but no new authoritative swapchain handoff occurs — the settings menu reuses the original game queue.
  4. PostSL activates but `scQueue=null`, `realQ=null`, `slWrapper=0`. The queue selection falls back to `g_OriginalGameQueue`, but `selectedQueueIsSwapchainQueue=false` because `scQueue` is null.
  5. `ShouldAllowPostSLDirectVirtualBootstrapWithoutWrapper` returned false because its active-Streamline branch requires `selectedQueueIsSwapchainQueue && selectedQueueOrigECLMatchesRealECL`, and the inactive branch requires `!streamlineFGActive`. PostSL could never bootstrap.
  6. Pre-SL draws were suppressed (`Suppressing pre-SL draw during SL FG startup`), so the overlay was invisible.

- **Fix**:
  1. `hook/common/dx12_overlay_policy.h` — `ShouldAllowPostSLDirectVirtualBootstrapWithoutWrapper` now accepts an optional `selectedQueueIsOriginalGameQueue` parameter (default `false`). When `streamlineFGActive`, no SL wrapper queue, no real queue behind wrapper, and the selected queue is the original game queue, virtual bootstrap is allowed. This covers the case where the swapchain effectively lives on `origGame` after a stale runtime-owned cleanup.
  2. `hook/apis/dx12_hook.cpp` — Updated the PostSL ECL hook call site to pass `queue == g_OriginalGameQueue` as the new parameter.
  3. `hook/apis/dx12_hook.cpp` — After releasing a stale runtime-owned swapchain queue, `g_SwapchainQueue` is now restored to `g_OriginalGameQueue` (with `AddRef` and updated capture time) if `g_OriginalGameQueue` is valid. This keeps swapchain queue tracking consistent with the live queue topology so that later DLSS FG activation sees a valid `scQueue`.
  4. `tests/test_dxgi_shared.cpp` — Added `PostSLNoWrapperVirtualBootstrapAllowedOnOriginalGameQueueDuringStreamlineFG` to lock the new policy behavior.

- **Verification**:
  1. `python build.py --skip-updates` passed and produced build `0.1.2551`.
  2. Full `.	ests	ests	ests	ests.exe` ran 639 tests; all passed.

- **Files changed**: `hook/common/dx12_overlay_policy.h`, `hook/apis/dx12_hook.cpp`, `tests/test_dxgi_shared.cpp`, `llm-wiki/log/recent.md`

- **Stale risk**: Low. The new policy branch is tightly scoped: it only triggers during active Streamline FG when no wrapper or direct queue exists, and only for the original game queue. The `g_SwapchainQueue` restoration follows the same pattern already used in the FG→off teardown path (lines 11218-11221).

### 2026-04-23 - Fix overlay not appearing in GTA when controller pseudo-overlay steals foreground during post-startup-resume

- **Motivation**: GTA V Enhanced `installed/captureengine/logs/20260423_040032` on build `0.1.2548` showed the overlay disappearing even with no FG active. The trace reached `DX12: Resuming DX12 overlay after startup overlay windows settled`, then got stuck forever on `DX12: Keeping overlay rendering deferred after startup-overlay resume for GTA5_Enhanced.exe (remaining=0ms)`.

- **Root cause analysis**:
  1. After a late pre-FG Streamline no-FG startup handoff (via Epic EOS overlay), CE re-armed startup overlay compatibility.
  2. When trying to resume overlay rendering after startup windows settled, `GetForegroundWindow()` returned the controller's pseudo-overlay window (`0000000000040952`) instead of the game's swapchain window (`00000000003A09EC`).
  3. The pseudo-overlay window is zero-sized (`foregroundSize=0x0`) and belongs to a different process, so `IsUsableSameProcessForegroundWindow` returned false.
  4. `ResolveDX12OverlayStartupResumeForegroundWindowMetrics` then returned false because the exact swapchain window was not foreground AND no usable same-process foreground window existed.
  5. This caused the post-resume countdown to never start, and the overlay stayed deferred indefinitely even though the game was actively rendering on a valid 3840x2160 swapchain window.

- **Fix**:
  1. `hook/common/overlay_compat.h` — `ResolveDX12OverlayStartupResumeForegroundWindowMetrics` now falls back to the swapchain window itself when no usable same-process foreground window is available, as long as the swapchain window has a usable size (>=640x360). This handles the case where an external process window (e.g. the controller's pseudo-overlay) steals focus while the game continues rendering normally.
  2. `hook/apis/dx12_hook.cpp` — Added a diagnostic log when the fallback to the swapchain window occurs: `DX12: Startup-overlay resume falling back to swapchain window ... because foreground window ... is not a usable same-process window`.
  3. `tests/test_fps_limiter.cpp` — Updated `InvalidForegroundWindowDoesNotCreatePostResumeCandidate` to reflect the new fallback behavior (valid game window now produces a trackable candidate). Added `SwapchainWindowFallbackWhenForegroundIsExternal` to explicitly lock the external-foreground-steal scenario.

- **Verification**:
  1. `python build.py --skip-updates` passed and produced build `0.1.2550`.
  2. `.	ests	ests\unit_tests.exe --gtest-filter=OverlayCompatTest.*` passed all 19 focused tests, including the new `SwapchainWindowFallbackWhenForegroundIsExternal`.
  3. Full `.	ests\unit_tests.exe` ran 638 tests; all passed.

- **Files changed**: `hook/common/overlay_compat.h`, `hook/apis/dx12_hook.cpp`, `tests/test_fps_limiter.cpp`, `llm-wiki/log/recent.md`

- **Stale risk**: Low. The fallback is conservative (requires the swapchain window to still be >=640x360) and only applies to the post-startup-resume path. If a title legitimately has no usable game window during this phase, the old defer behavior is preserved.

### 2026-04-23 - Add FFX present callback stall detection fallback to prevent overlay disappearance during runtime-owned FSR

- **Motivation**: After a runtime-owned FSR takeover, the FFX present callback can stall and stop firing. Because the normal overlay GPU path is deferred while the runtime owns the swapchain, the overlay never gets rendered and appears to hang.

- **Fix**:
  1. `hook/apis/dx12_hook.cpp` — Added `g_LastFFXPresentCallbackTickMs` atomic timestamp updated on every entry to `DX12_RenderOverlayViaFFXPresentCallback`. Added `IsFFXPresentCallbackStalled()` helper with two thresholds: 2000ms if the callback previously fired but stopped, and 3000ms if the runtime has owned the swapchain since takeover without the callback ever firing.
  2. `hook/common/dx12_overlay_policy.h` — `ShouldSkipSeparateOverlayGpuWorkForRuntimeOwnedFrameGeneration` now accepts an optional `ffxPresentCallbackStalled` parameter. When true, the function returns false (do not skip overlay work), allowing normal overlay rendering as a fallback.
  3. `hook/apis/dx12_hook.cpp` — `ShouldSkipSeparateOverlayGpuWorkForCurrentSwapchain` now checks `IsFFXPresentCallbackStalled()` and passes it to the policy. When the stall fallback triggers, it logs a diagnostic (`DX12: FFX present callback appears stalled ... — allowing normal overlay rendering as fallback`). The stall state is also threaded through `SetSwapchainQueue` and `DX12_OnStreamlineFGStateChanged` so stale runtime-owned FSR ownership is not preserved when the callback path is dead.
  4. `tests/test_dxgi_shared.cpp` — Added `FFXPresentCallbackStallAllowsNormalOverlayRendering` to lock the new behavior.

- **Verification**:
  1. `python build.py --skip-updates` passed and produced build `0.1.2549`.
  2. `python build.py --skip-updates --tests-only --run-tests --gtest-filter=DXGISharedTest.*` passed all 179 focused tests.
  3. Full `.\tests\unit_tests.exe` ran 637 tests; all passed.

- **Files changed**: `hook/apis/dx12_hook.cpp`, `hook/common/dx12_overlay_policy.h`, `tests/test_dxgi_shared.cpp`, `llm-wiki/log/recent.md`

- **Stale risk**: The 2000ms/3000ms thresholds are heuristics. If a title legitimately pauses presents for >2 seconds (e.g., loading screen), the fallback could briefly allow normal overlay work when FSR is still active. The log will show the stall event, so future traces can verify timing. If this proves too aggressive in practice, the thresholds can be raised.

### 2026-04-23 - Fix overlay disappearance and DLSS GetState suppression after FSR->OFF->DLSS menu switches in Talos

- **Motivation**: Talos Principle Reawakened `installed/captureengine/logs/20260423_022624` on build `0.1.2544` showed that after starting with DLSS FG, switching to FSR FG, switching all FG off, then switching to DLSS again, the overlay disappeared in the game menu and never reappeared. The log tail showed a self-reinforcing stuck state: `Deferring overlay init because runtime-owned native FSR Present teardown window is active` and `Streamline Hook: Suppressing fresh GetState DLSS FG reactivation (persistentBlock=1)`.

- **Root cause analysis**:
  1. When FSR turned off, `DX12_OnNativeFSRFrameGenerationConfigured(false)` set `g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown = true` because `g_FGRuntimeOwnsSwapchain` was true. This flag was never cleared afterward.
  2. A new swapchain was then created (menu swapchain) on queue `000001CE24C46A80`, different from `origGame=000001CEBEDE9030`. `SetSwapchainQueue` kept `g_FGRuntimeOwnsSwapchain = true` because the queue was still runtime-owned.
  3. `HookHasRuntimeOwnedNativeFGPresentPath()` returned true due to the combination of `g_FGRuntimeOwnsSwapchain=true` and `g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown=true`, even though `runtime=STREAMLINE_NO_FG` and `apiFSR=0`.
  4. This made `ShouldSkipSeparateOverlayGpuWorkForRuntimeOwnedFrameGeneration` return true, deferring overlay init indefinitely.
  5. Separately, an earlier `slDLSSGSetOptions(OFF)` had set `g_BlockGetStateOnlyReactivationUntilExplicitSetOptions = true`. When FSR later took over authoritatively, `OnAuthoritativeFFXTakeover()` did NOT clear this stale DLSS-only block. After FSR->OFF, DLSS could not reactivate via `GetState` because the old persistent block outlived the FSR epoch.

- **Fix**:
  1. `hook/apis/dx12_hook.cpp` — In `SetSwapchainQueue()`, when the swapchain queue changes and FSR is no longer active (`!g_FGCompat.IsFSRFGApiActive()` and runtime mode != `kFSRFG`), the stale `g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown` flag is now cleared with a diagnostic log. This breaks the self-reinforcing cycle that deferred overlay init.
  2. `hook/apis/streamline_hook.cpp` — `OnAuthoritativeFFXTakeover()` now clears `g_BlockGetStateOnlyReactivationUntilExplicitSetOptions` and logs whether a stale block was removed (`clearedStaleBlock=%d`). An authoritative FFX takeover resets the entire FG session context; any old DLSS-only reactivation block from a previous epoch must not outlive the FSR phase.

- **Verification**:
  1. `python build.py --skip-updates` passed and produced build `0.1.2546` (hook DLL verified).
  2. `python build.py --skip-updates --tests-only --run-tests` passed all 636 unit tests.
  3. The focused DXGISharedTest and DX12FG transition sequence tests all passed, including `FSRToDLSSIsSwitchingAndMovesToPostSL`, `DLSSToFSRIsSwitchingAndLeavesPostSL`, and `OffToFSRToOffUsesRuntimeOwnedRecoveryThenSettles`.

- **Files changed**: `hook/apis/dx12_hook.cpp`, `hook/apis/streamline_hook.cpp`, `llm-wiki/log/recent.md`

- **Stale risk**: The new `SetSwapchainQueue` clear path is gated on `!IsFSRFGApiActive() && runtimeMode != kFSRFG`. If a title briefly reports `NO_FG` while FSR is still tearing down its present callback on the old queue, the flag might clear slightly early. The log will show the clear event, so future traces can verify timing. The `OnAuthoritativeFFXTakeover` clear is unconditional and safe because FFX takeover is by definition a new epoch.

### 2026-04-23 - Fix stale swapchain AV during FSR->DLSS transition and clear stale PDB cache

- **Motivation**: Talos Principle Reawakened `installed/captureengine/logs/20260423_013835` on build `0.1.2543` crashed with `0xC0000005` at `ntdll!RtlpWaitOnCriticalSection+0xb3` inside `dxgi!CDXGISwapChain::GetDesc+0x70` when switching from FSR FG to DLSS in the menu. The ECL heartbeat thread triggered `PostSLOverlayRenderGated` via `postSLCallback(activationSwapchain)`, where `activationSwapchain` came from `GetLastTrackedSwapchainForStartupActivation()` -> `g_LastSwapChain`. The swapchain had been destroyed by the runtime during the transition while `g_LastSwapChain` (raw, non-AddRef'd) still pointed to it. No `ProcessFrame` had run for ~10 seconds, so the pointer was never refreshed.

- **Fix**:
  1. `hook/apis/dx12_hook.cpp` — `GetLastTrackedSwapchainForStartupActivation()` now checks `g_LastProcessFrameTickMs`. If `ProcessFrame` hasn't run within 500ms, the cached swapchain is treated as stale and the function returns `nullptr` with a diagnostic log. This prevents handing a zombie swapchain to PostSL bootstrap.
  2. `hook/apis/dx12_hook.cpp` — The ECL hook expiry-driven startup activation path now explicitly skips the `postSLCallback` call when `activationSwapchain` is `nullptr`, and logs why. It still clears the startup transition window if it just expired to avoid re-entry loops.
  3. `hook/apis/dx12_hook.cpp` — `PostSLOverlayRender()` now guards the `GetDesc` bootstrap path with a null check on `pSwapChain`. If null, it logs and skips bootstrap instead of crashing.
  4. `build.py` — Added `clear_stale_hook_pdb_cache()` that removes cached `capture_hook_*.pdb` directories from `C:\ProgramData\dbg\sym` after every build. This fixes cdb loading stale PDBs and resolving mangled symbols.

- **Verification**:
  1. `python build.py --skip-updates` passed and produced build `0.1.2544` (hook DLL verified).
  2. `python build.py --skip-updates --tests-only --run-tests` passed all 636 unit tests.
  3. The stale PDB cache `C:\ProgramData\dbg\sym\capture_hook_x64.pdb` was removed during build.

- **Files changed**: `hook/apis/dx12_hook.cpp`, `build.py`, `llm-wiki/log/recent.md`

- **Stale risk**: The 500ms stale-swapchain threshold is a heuristic. If a legitimate startup activation occurs during a >500ms pause (e.g., loading screen), the ECL hook will defer until the next `ProcessFrame` refreshes the swapchain. This is considered safe because activation is not urgent during a pause, but a future trace should verify that overlay reappearance is not delayed unexpectedly after long stalls.

### 2026-04-22 - Extend stale-OFF runtime-state stabilization when a reactivated PostSL startup never reached the warmup proof threshold

- **Motivation**: GTA V Enhanced `installed/captureengine/logs/20260422_213335` on build `0.1.2539` showed that the earlier reactivation-reset fix was real but still not sufficient. The session now logs `DX12: PostSL reactivation reset confirmed-startup progress ...`, so epoch 2 no longer inherits epoch 1's startup counters. But epoch 1 still only reached two confirmed PostSL renders before stalling (`stallCount=74` by the epoch-2 reset), and epoch 2 then rendered cleanly through `Post-SL overlay SUBMIT #16` before the old post-settling stale-OFF window ended. Immediately after `DX12: PostSL confirmed startup rendering left runtime-state stabilization (stableFrames=13)`, CE dropped the buffered stale `slDLSSGSetOptions(OFF)` and the next `GetState` OFF still collapsed the session back to non-FG. The failure is therefore no longer "counters leaked across epochs"; it is now "a churned, reactivated startup can still inherit stale OFF churn from the older half-proven epoch after the old 9-12 window ends".

- **Fix**:
  1. `hook/common/dx12_overlay_policy.h` now defines the repo-wide confirmed PostSL warmup proof threshold (`GetConfirmedPostSLWarmupProofFrameThreshold() == 30`) and exposes `ShouldExtendConfirmedPostSLRuntimeStateStabilizationAfterReactivation(...)` so the rule is explicit and testable.
  2. `hook/apis/dx12_hook.cpp` now latches `g_PostSLExtendedRuntimeStateStabilizationForCurrentEpoch` whenever a reactivation interrupts a confirmed PostSL startup before that 30-frame proof threshold. The same file now resets that state on all fresh-start / teardown paths, uses it when evaluating `HookIsPostSLOverlayConfirmedButRuntimeStateStabilizing()`, and logs both `extendStaleOff=%d` in the reset message and a dedicated `extended runtime-state stabilization for churned startup ... proofThreshold=30` line when the longer window is armed.
  3. The runtime-state stabilization window is now dynamic per epoch: default epochs still stabilize only on frames 9-12, but churned reactivation epochs keep only the stale-OFF protection alive through frame 30. This intentionally does **not** widen the broader startup-routing / handoff-pending / cooldown logic.
  4. `hook/common/hook_common.h` now exports `HookGetPostSLRuntimeStateStabilizationLastFrame()`, and `hook/apis/streamline_hook.cpp` uses it so the one-shot `Suppressing first post-settling GetState OFF during runtime-state stabilization ... stableProtectionWindow=...` log reports the real active window instead of always printing `9-12`.
  5. `tests/test_dxgi_shared.cpp` now locks the new helper contract with `ChurnedPostSLReactivationExtendsRuntimeStateStabilizationToWarmupProofThreshold`, including the new `13..30` extended-window behavior.

- **Verification**:
  1. `python build.py --run-tests --tests-only --skip-updates --gtest-filter=DXGISharedTest.PostSL*` passed 31/31 focused DXGIShared tests on build `0.1.2540`, including the new `ChurnedPostSLReactivationExtendsRuntimeStateStabilizationToWarmupProofThreshold`.
  2. `python build.py --skip-updates` passed and produced build `0.1.2541` (`build/verification/latest_summary.txt` reports `success=1`).
  3. Full `.\tests\unit_tests.exe` ran 636 tests; 634 passed. The only failures were again `ProcessIPCTest.PollCommandCompletesPendingConnectionAcrossCalls` and `ProcessIPCTest.RecoversAfterClientDisconnect`, both failing at the initial named-pipe `Connect(1000)` / `firstClient.Connect(1000)` assertion before any DX12 / FG path is exercised.
  4. The full build log still carried the existing environment noise from failed pip bootstrap attempts (`pyright` / `flake8` / `black` metadata permission denied in `%LOCALAPPDATA%\\Temp`) and the existing x86 test-app temporary-object rename failures in `%LOCALAPPDATA%\\Temp`, but the main product build finished successfully and emitted build `0.1.2541`.

- **Files changed**: `hook/common/dx12_overlay_policy.h`, `hook/common/hook_common.h`, `hook/apis/dx12_hook.cpp`, `hook/apis/streamline_hook.cpp`, `tests/test_dxgi_shared.cpp`, `tests/test_stubs.cpp`, `llm-wiki/current.md`, `llm-wiki/log/recent.md`

- **Stale risk**: The next GTA rerun on the next local build should show `DX12: PostSL confirmed startup rendering entered runtime-state stabilization ( ... last=30 extended=1 ...)` for the churned epoch and should keep suppressing stale `GetState OFF` until that epoch has actually reached the same 30-frame proof threshold. If GTA still falls back to non-FG after that, the next seam is probably no longer stale startup OFF replay at all but a real runtime ON->OFF decision later in the session.

### 2026-04-22 - Restart PostSL confirmed-startup progress on reactivation so GTA pure-DLSS protection stays scoped to the latest epoch

- **Motivation**: GTA V Enhanced `installed/captureengine/logs/20260422_211004` on build `0.1.2537` showed that the earlier post-settling stale-OFF stabilization fix was real but still not sufficient. The session reached `DX12: PostSL CONFIRMED rendering`, `Post-SL overlay SUBMIT #1`, and the new stale-OFF suppression logs, but then the same pure-DLSS startup family reactivated a second time (`DX12: PostSL REACTIVATED (epoch=2 hadFSR=0)`) before FG fully settled. Because `g_PostSLStableFrameCount` and the related startup-progress counters were not reset on that reactivation edge, the frames-9-12 runtime-state stabilization window was effectively being measured across both epochs. In the GTA trace that let the protection window expire almost immediately after epoch 2, after which the buffered stale `slDLSSGSetOptions(OFF)` was dropped and the next `GetState` OFF collapsed the session back to non-FG. The same log also had no `slReflexSetConstants` activity and no active CE FPS cap, so the failing seam remained startup epoch accounting, not the older Reflex-hook family.

- **Fix**:
  1. `hook/common/dx12_overlay_policy.h` now exposes `ShouldResetPostSLStartupProgressOnReactivation(...)` so the reactivation rule is explicit and unit-testable.
  2. `hook/apis/dx12_hook.cpp` now resets `g_PostSLStableFrameCount`, `g_PostSLStallCounter`, and `g_PostSLRuntimeStateStabilizationLogged` whenever PostSL enters a new reactivation epoch, and logs `DX12: PostSL reactivation reset confirmed-startup progress ...` with the prior counters. The confirmed-render latch intentionally stays live; the fix is to restart the settling/stabilization counters per epoch, not to pretend the path was never confirmed.
  3. `tests/test_dxgi_shared.cpp` now covers the helper so future changes cannot silently reintroduce cross-epoch startup-progress leakage.

- **Verification**:
  1. `python build.py --run-tests --tests-only --skip-updates --gtest-filter=DXGISharedTest.PostSL*` passed 31/31 focused DXGIShared tests, including the new `PostSLReactivationRestartsConfirmedStartupProgressPerEpoch`.
  2. `python build.py --skip-updates` passed and produced build `0.1.2539` (`build/verification/latest_summary.txt` reports `success=1`).
  3. Full `.\tests\unit_tests.exe` ran 635 tests; 633 passed. The only failures were `ProcessIPCTest.PollCommandCompletesPendingConnectionAcrossCalls` and `ProcessIPCTest.RecoversAfterClientDisconnect`, both failing at the first `client.Connect(1000)` / `firstClient.Connect(1000)` assertion before any DX12 / FG path is exercised.

- **Files changed**: `hook/common/dx12_overlay_policy.h`, `hook/apis/dx12_hook.cpp`, `tests/test_dxgi_shared.cpp`, `llm-wiki/current.md`, `llm-wiki/log/recent.md`

- **Stale risk**: The next GTA rerun on build `0.1.2537+` / next local build should show that if pure-DLSS startup re-enters `PostSL REACTIVATED (epoch=2 hadFSR=0)`, the later stale-OFF protection window restarts from that epoch instead of expiring almost immediately. If GTA still fails after this fix, the next seam to revisit is not the per-epoch counter leak but why that second reactivation happens in the first place on the pure-DLSS startup family while Talos does not need it.

### 2026-04-22 - Add post-settling runtime-state stabilization phase to prevent GTA DLSS FG stale-OFF collapse after startup settling ends

- **Motivation**: GTA V Enhanced `installed/captureengine/logs/20260422_132835` on build `0.1.2534` showed that pure-DLSS startup now succeeds through the first nine confirmed PostSL submits, but collapses immediately when the buffered stale `slDLSSGSetOptions(OFF)` is dropped at the end of the confirmed-startup-settling window. The next `GetState` poll then drives `ON->OFF via GetState` and the game falls back to non-FG. The user confirmed no CE Reflex FPS cap was active in this repro.

- **Fix**:
  1. `hook/common/dx12_overlay_policy.h` adds `ShouldTreatConfirmedPostSLRenderingAsRuntimeStateStabilizing(...)` for frames 9-12 after first confirmed PostSL rendering, plus `GetConfirmedPostSLRuntimeStateStabilizationFirstFrame()` (9) and `GetConfirmedPostSLRuntimeStateStabilizationLastFrame()` (12).
  2. `hook/common/hook_common.h` exports `HookIsPostSLOverlayConfirmedButRuntimeStateStabilizing()`.
  3. `hook/apis/dx12_hook.cpp` implements the helper and adds one-shot stabilization begin/end logs.
  4. `hook/common/streamline_runtime_policy.h` threads `postSLConfirmedButRuntimeStateStabilizing` through all six startup-protected stale-OFF helpers.
  5. `hook/apis/streamline_hook.cpp` applies the guard at all four stale-OFF decision sites, adds `stabilizing=%d` to logs, adds a one-shot log for the first suppressed post-settling `GetState OFF`, and adds a Reflex activation path-state diagnostic.
  6. `tests/test_dxgi_shared.cpp` and `tests/test_streamline_runtime_policy.cpp` add focused boundary tests.
  7. Fixed missing `#include "../common/dx12_overlay_policy.h"` in `hook/apis/streamline_hook.cpp`.

- **Verification**: focused tests passed 214/214; full unit tests passed 634/634; rebuild passed producing `0.1.2537`.

- **Files changed**: `hook/common/dx12_overlay_policy.h`, `hook/common/hook_common.h`, `hook/common/streamline_runtime_policy.h`, `hook/apis/dx12_hook.cpp`, `hook/apis/streamline_hook.cpp`, `tests/test_dxgi_shared.cpp`, `tests/test_streamline_runtime_policy.cpp`, `tests/test_stubs.cpp`, `llm-wiki/current.md`, `llm-wiki/log/recent.md`

- **Stale risk**: The next GTA check on build `0.1.2537+` is that the pure-DLSS `all FG off -> DLSS FG` family no longer shows the immediate `ON->OFF via GetState` collapse right after `Post-SL overlay SUBMIT #9` and that DLSS FG stays enabled at FG FPS without a persistent pink tint. Talos should keep its already healthy behavior. If GTA still fails after this narrow stale-OFF fix, the next pass should revisit the active-limiter / Reflex interception path.

### 2026-04-22 - Remove Reflex limiter nvapi_QueryInterface inline hook entirely and fix SetSleepMode buffer overread

- **Motivation**: GTA V Enhanced `installed/captureengine/logs/20260422_022003` on build `0.1.2528` still showed the transient pink-tint DLSS FG enablement failure after the earlier "return original driver pointers from QueryInterface" fix. The same symptom did not occur in Talos, where the `nvapi_QueryInterface` inline hook already failed (RIP-relative out of range).

- **Comparison that narrowed the seam**:
  1. The earlier fix changed `ReflexDetour_QueryInterface` to return original `nvapi64.dll` pointers instead of wrapper pointers, but DLSS FG still failed in GTA with the same pink-tint symptom.
  2. Log analysis showed the game activated Reflex via `SetSleepMode` (~02:22:20.337), then deactivated it ~500ms later (~02:22:20.834), suggesting a validation failure. DLSS FG then ran without proper Reflex for ~2.5s, producing the pink tint, before Streamline explicitly called `slDLSSGSetOptions(OFF)`.
  3. The decisive difference from Talos is that the inline hook on `nvapi_QueryInterface` **succeeds** in GTA. Even though our detour now returns original driver pointers, the mere presence of the 14-byte JMP patch on the first bytes of `nvapi_QueryInterface` appears to be detected by Streamline/DLSS FG or the game's anti-tamper/integrity validation, causing it to abort Reflex integration shortly after startup.
  4. Additionally, `InterceptSetSleepMode` performed `lastSleepModeParams_ = *pParams;` where the game passed a 44-byte struct (`version=0x1002C`) but our `NV_SET_SLEEP_MODE_PARAMS` is 56 bytes. This overreads 12 bytes of adjacent stack memory. While `PushFpsLimit()` was inactive during this session (the FPS limiter trace shows `Apply: INACTIVE capReq=0`), the overread is a latent bug that would forward garbage to the driver if the limiter ever activates.

- **Root cause refinement**:
  1. The `nvapi_QueryInterface` inline hook itself — not just the return values — is the remaining root cause. Removing the hook entirely eliminates the detectable patch from the function prologue.
  2. The direct inline hooks on `NvAPI_D3D_SetSleepMode` and `NvAPI_D3D_Sleep` are installed **before** `HookNvAPIQueryInterface()` was called and remain active. Any caller that obtains the original pointer (whether from `QueryInterface` or cached) and calls it will still hit our inline hook at the original function address. Activation detection and FPS-limit overriding continue to work exactly as before.
  3. The buffer overread in `InterceptSetSleepMode` is fixed by copying only up to the caller's declared struct size (encoded in `version & 0xFFFF`) and zeroing the remainder of our larger buffer.

- **Fix**:
  1. `hook/common/reflex_limiter.h` now removes `HookNvAPIQueryInterface()` entirely: the call in `Init()`, the method definition, the `ReflexDetour_QueryInterface` static detour, and the `detourOrigQueryInterface_` member field. `Init()` now logs `Skipping nvapi_QueryInterface hook — relying on direct SetSleepMode/Sleep hooks` so future traces confirm the hook is absent.
  2. The same header now fixes `InterceptSetSleepMode` to perform a version-aware copy: `callerSize = pParams->version & 0xFFFF`, `copySize = min(callerSize, sizeof(NV_SET_SLEEP_MODE_PARAMS))`, `CopyMemory` for the prefix, and `ZeroMemory` for any trailing bytes we do not receive from the caller. A new diagnostic log (first 5 occurrences) reports `SetSleepMode struct size mismatch (caller=%u our=%u version=0x%08X)`.
  3. The existing version-mismatch log (first 5 occurrences) is kept unchanged for when `pParams->version != NV_SET_SLEEP_MODE_PARAMS_VER`.

- **Why this is generic**: This is not a GTA-only carve-out. Any title where Streamline or the runtime validates the integrity of `nvapi_QueryInterface` would see the same Reflex-init abort if CE leaves an inline hook patch on that function. The fix is generic because we rely on the already-installed direct hooks on the concrete Reflex entrypoints, which intercept every actual call regardless of how the caller obtained the pointer. The struct-size fix is also generic: any game that passes a smaller `NV_SET_SLEEP_MODE_PARAMS` than our declared size would previously have caused a buffer overread.

- **Verification**:
  - Re-checked GTA `installed/captureengine/logs/20260422_022003/{hook_debug.log,fps_limiter_trace.log,nvngx_debug.log}` and confirmed the decisive sequence: Reflex activation, rapid deactivation, pink-tint diagnostic, then explicit Streamline OFF.
  - Re-checked Talos `installed/captureengine/logs/20260421_165756_talosnocrash_multipleswitching/hook_debug.log` and confirmed the `nvapi_QueryInterface` inline hook fails with RIP-relative out-of-range, so Talos was unaffected.
  - Re-read `hook/common/reflex_limiter.h` and verified `HookNvAPIQueryInterface()`, `ReflexDetour_QueryInterface`, and `detourOrigQueryInterface_` are fully removed, and the version-aware copy is present in `InterceptSetSleepMode`.
  - Ran `python build.py --skip-updates`; full rebuild passed and `build/verification/latest_summary.txt` reports success for build `0.1.2532`.
  - Ran `& ".\tests\unit_tests.exe"`; all 632 tests passed, 0 failed.

- **Files changed**: `hook/common/reflex_limiter.h`, `llm-wiki/current.md`, `llm-wiki/log.md`

- **Stale risk**: The next GTA check on build `0.1.2532+` is that the pure-DLSS `all FG off -> DLSS FG` family no longer shows the transient pink tint and no longer falls back to non-FG FPS after enable. Talos should keep its already healthy behavior. If activation detection ever becomes unreliable in a title that caches function pointers before our inline hooks are installed, we may need a different hook mechanism (e.g. IAT hook on `GetProcAddress` for `nvapi64.dll`) rather than restoring the inline `nvapi_QueryInterface` patch.
