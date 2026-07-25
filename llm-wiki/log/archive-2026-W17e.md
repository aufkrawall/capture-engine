# llm-wiki Log — Archive 2026-W17e

### 2026-04-23 - Quiet build-only runs and fix x86 test-app temp/file handling so `python build.py --skip-updates` no longer emits pip noise or x86 rename failures

- **Motivation**: The repo's normal build verification still reported two distracting failure families after the Streamline/Talos work landed:
  1. optional Python tooling bootstrap kept trying to install `pyright` / `flake8` / `black` during ordinary build-only runs, producing permission-denied noise under temp storage;
  2. x86 test-app builds kept dying on `unable to rename temporary ...o.tmp -> ...o` when the test-app helper used a separate one-step x86 path.

- **Root cause analysis**:
  1. `build.py` called `check_python_lsp_tools()` on every local run, not just the lint / format / default-quality flows that actually need it.
  2. The Windows x86 test-app helper was using the wrong compiler context (`mingw32/bin/clang++.exe`) instead of the same clang64 cross-driver pattern that the rest of the x86 build already uses.
  3. The one-step test-app path also reused broad temp state, which made the compiler's temp-object renames fragile during parallel x64/x86 switching.
  4. Once the x86 driver was aligned to the main x86 build, the remaining failures shifted from temp renames to missing x86 runtime/search-path flags, confirming the test-app helper was not actually carrying the same x86 link context as the main build.

- **Fix**:
  1. `build.py` now only bootstraps the optional Python tooling when the active mode actually wants quality checks (`default_quality_mode`, `--verify`, `--lint`, or `--format`). Plain `--skip-updates` build-only runs now stay quiet.
  2. Windows x86 test apps now use the same clang64 cross-driver, x86 target/sysroot flags, and x86 runtime selection as the main x86 build.
  3. Each test-app task gets its own temp subdirectory under `build/tmp/testapps/`, so parallel x64/x86 jobs no longer collide on compiler temp files.
  4. The x86 test-app helper now carries the explicit x86 `libgcc` / `libstdc++` linker setup that the main x86 build already relies on, and logs the temp-dir isolation once so future traces show the safety boundary.

- **Verification**:
  1. `python build.py --skip-updates` passed cleanly and produced build `0.1.2574`; `build/verification/latest_summary.txt` reports `success=1`.
  2. The run no longer emitted the old pip bootstrap permission noise.
  3. The run no longer emitted the x86 compiler temp rename failures, and the test-app phase completed with all x64/x86 test apps built.

- **Files changed**: `build.py`, `llm-wiki/build.py.md`, `llm-wiki/current.md`, `llm-wiki/log/recent.md`

- **Stale risk**: Low. This is a build-system hygiene fix, not a runtime behavior change. The main thing to re-check later is whether any future x86 toolchain package churn changes the clang64 cross-driver's include/runtime defaults again.

### 2026-04-23 - Escalate the Streamline fallback to the real feature-owner DLL so menu-side DLSS-off cannot bypass CE after an export-inline failure

- **Motivation**: Talos Principle Reawakened `installed/captureengine/logs/20260423_220858` on build `0.1.2565` still reproduced the user's stale-label bug immediately after the first direct-import fallback attempt. The user again enabled `DLSS FG`, disabled it from the options menu, stayed inside the menu, and the overlay still reported `DLSS FG`.

- **Root cause analysis**:
  1. The new build definitely loaded the first fallback: the session logs `Registered dynamic hooks for slGetFeatureFunction, slSetD3DDevice, slDLSSGSetOptions, slDLSSGGetState, and slReflexSetConstants`.
  2. But the same trace still logs `Failed to inline hook slDLSSGSetOptions` and `Failed to inline hook slDLSSGGetState`, and it only reports `Installed hooks for sl.interposer.dll` with no `Installed direct import fallback ...` line afterward.
  3. The session still reaches `FG state transition OFF->ON via SetOptions` at `22:09:29.192`, which proves CE did see one wrapped `SetOptions` call. But by the end of the session the trace again contains only repeated `streamline-getstate-runtime-update ... runtime=DLSS_FG active=1 explicit=1` and `Post-SL overlay SUBMIT` lines, with no later OFF edge.
  4. That means the first fallback idea was only half-complete: it covered the core Streamline DLLs (`sl.interposer.dll` / `sl.common.dll`), but the actual failing `slDLSSGSetOptions` / `slDLSSGGetState` pointers live in the later feature-owner module behind the returned export address. The later menu-side OFF could therefore still bypass CE through that owner DLL even though the wrapper path had already seen an earlier ON.

- **Fix**:
  1. `hook/apis/streamline_hook.cpp` now records a per-feature "owner-module fallback attempted" target for `slDLSSGSetOptions`, `slDLSSGGetState`, and `slReflexSetConstants`.
  2. When `MaybeHook...` sees a real feature-export pointer and export-inline hooking fails, CE now resolves the owning DLL of that pointer via `GetModuleHandleEx(... FROM_ADDRESS ...)` / `GetModuleFileNameA(...)`.
  3. CE then installs the direct-import fallback against that real owner module instead of waiting on the core Streamline module names alone. New logs now say `Direct import fallback unavailable for ... owner=... target=...` when even that owner-module path cannot be armed, so future traces show the remaining seam directly.

- **Verification**:
  1. `python build.py --skip-updates --tests-only --run-tests --gtest-filter=StreamlineRuntimePolicyTest.*` passed all 38 focused tests and produced build `0.1.2566`.
  2. `python build.py --skip-updates` passed and produced build `0.1.2567`; `build/verification/latest_summary.txt` reports `success=1`.
  3. The build still logged the repo's existing pip-bootstrap permission noise plus `%LOCALAPPDATA%\\Temp` rename failures while compiling x86 test apps, but the main build summary/manifest still marked the product build successful.

- **Files changed**: `hook/apis/streamline_hook.cpp`, `llm-wiki/current.md`, `llm-wiki/overlay-fg-status.md`, `llm-wiki/frame-generation/guardrails.md`, `llm-wiki/log/recent.md`

- **Stale risk**: Medium-low. The new change stays generic and still does not alter FG routing or disable any runtime path. Fresh Talos validation is still required after build `0.1.2567` to confirm the later owner-module fallback now catches the menu-side `DLSS FG -> off` edge and that the existing explicit-OFF reducer keeps stale `GetState` reactivation suppressed until a real explicit re-enable.

### 2026-04-23 - Catch direct Streamline feature imports so final DLSS-off edges still reach the overlay when export inline hooks fail

- **Motivation**: Talos Principle Reawakened `installed/captureengine/logs/20260423_215138` on build `0.1.2563` reproduced the user-visible stale-label bug in a narrower sequence than the earlier mixed-runtime churn cases: the game started with all FG off, the user enabled `DLSS FG`, then disabled all FG again from the options menu and closed the game there without returning to active 3D gameplay. The overlay still showed `DLSS FG` at the end even though all FG was off.

- **Root cause analysis**:
  1. The earlier same-day publication-order fixes were already in place, so this was no longer just a planner-vs-visible-cache repaint race.
  2. `hook_debug.log` showed the last decisive active publication at `21:52:12.894` (`runtime=DLSS_FG active=1`) and then only repeated `streamline-getstate-runtime-update ... runtime=DLSS_FG active=1 explicit=1` plus `Post-SL overlay SUBMIT` lines near shutdown. There was no later authoritative `active=0` publication to clear the label.
  3. The same session logged `Streamline Hook: Failed to inline hook slDLSSGSetOptions at 00007FFE54664920`.
  4. CE already had wrapper fallback coverage when a caller resolved the feature through `slGetFeatureFunction`, but this session never logged `Intercepted slDLSSGSetOptions lookup`, which means the game's menu-side disable likely reached Streamline through a direct import or dynamic lookup path that bypassed both the failed export-inline hook and the existing wrapper-substitution path.
  5. Once CE missed that final explicit OFF edge, the overlay publication layer had no newer state to publish. The stale `DLSS FG` label was therefore a real observation gap upstream of the publisher, not another label-mapping bug.

- **Fix**:
  1. `hook/apis/streamline_hook.cpp` now registers dynamic hooks for `slDLSSGSetOptions`, `slDLSSGGetState`, and `slReflexSetConstants` in addition to the existing `slGetFeatureFunction` / `slSetD3DDevice` coverage.
  2. The same file now adds `InstallFeatureImportFallbackIfPresent(...)`, which installs direct-import fallback IAT hooks for those feature exports whenever a loaded Streamline module exports them, while still preserving the real export pointer in the original-function slot.
  3. New high-signal logs now say `Streamline Hook: Installed direct import fallback for ... via ...` so future traces immediately show whether CE had a generic recovery path after an export-inline failure.

- **Verification**:
  1. `python build.py --skip-updates --tests-only --run-tests --gtest-filter=StreamlineRuntimePolicyTest.*` passed all 38 focused tests and produced build `0.1.2564`.
  2. `python build.py --skip-updates` passed and produced build `0.1.2565`; `build/verification/latest_summary.txt` reports `success=1`.
  3. There is not yet a dedicated unit test that emulates PE import tables for this new direct-import fallback seam. The current unit harness covers the Streamline runtime-policy layer, while this change itself is a Windows module-hooking integration path, so runtime logs remain the main diagnostic proof for this exact family.

- **Files changed**: `hook/apis/streamline_hook.cpp`, `llm-wiki/current.md`, `llm-wiki/overlay-fg-status.md`, `llm-wiki/frame-generation/guardrails.md`, `llm-wiki/log/recent.md`

- **Stale risk**: Medium-low. The change is generic and intentionally narrow: it improves how CE observes Streamline feature calls when a specific export-inline hook fails, without changing FG routing or disabling any runtime path. Fresh Talos and GTA validation is still required after build `0.1.2565` to confirm that menu-side `DLSS FG -> off` now logs and clears the overlay immediately even when `slDLSSGSetOptions` export inline hooking fails.

### 2026-04-23 - Harden overlay FG publication chronology so a newer planner OFF can beat an older cached visible DLSS FG state

- **Motivation**: Talos Principle Reawakened `installed/captureengine/logs/20260423_212713` on build `0.1.2557` still ended with the user reporting `DLSS FG` visible in the overlay after a long menu-side switching sequence that should have ended with all FG off. The trace did not clearly show a final authoritative FG-off after the last DLSS re-enable, but it did expose a remaining weakness in the previous same-day publication fix: once a preferred visible state had been cached, planner-driven refreshes had no way to tell whether that preferred state was newer or older than the planner snapshot they were about to publish.

- **Root cause analysis**:
  1. The previous fix introduced a DX12-exported preferred visible-state cache and made planner-driven publications always override the planner with that preferred state when the two disagreed.
  2. That solved the original stale-plan repaint family, where a later planner refresh could repaint stale `DLSS FG` after `ProcessFrame` had already corrected the visible state.
  3. But it left the inverse seam open: if the planner later moved to `off` and the preferred cache was still holding an older `DLSS FG` state, planner-driven publishers would keep trusting the stale preferred cache forever.
  4. The new Talos menu repro is exactly the kind of shutdown-adjacent churn where that second seam matters, especially because the session also still logged `Failed to inline hook slDLSSGSetOptions`, so some final visibility corrections may depend on which state source updated most recently rather than on every path seeing a perfectly matched OFF edge.

- **Fix**:
  1. `hook/common/hook_common.h` + `hook/apis/dx12_hook.cpp` — Added a shared overlay-publication sequence allocator and extended the preferred visible-state cache with a comparable `sequence` field. Every preferred-state update now records one globally ordered publication sequence and logs it (`FG publication preferred state: ... sequence=...`).
  2. `hook/common/overlay_metrics_publisher.cpp` — Planner state changes now also allocate from that same publication sequence. When the planner and preferred visible state disagree, the shared publisher now lets the newer sequence win instead of unconditionally trusting the preferred cache. Override logs now say which side won and include both sequence numbers.
  3. `tests/test_overlay_fg_status_publication.cpp` + `tests/test_stubs.cpp` — Regression coverage now locks both directions:
     - a stale planner `DLSS FG` publication is overridden by a newer preferred visible `FSR FG` / `off` state
     - a newer planner `off` update beats an older cached preferred `DLSS FG` state

- **Verification**:
  1. `python build.py --skip-updates --tests-only --run-tests --gtest-filter=OverlayFGStatusPublicationTest.*` passed all 8 focused tests and produced build `0.1.2561`.
  2. The focused run still logged the repo's existing pip-bootstrap permission noise in `%LOCALAPPDATA%\\Temp`, but the actual build/test step completed successfully.

- **Files changed**: `hook/common/hook_common.h`, `hook/apis/dx12_hook.cpp`, `hook/common/overlay_metrics_publisher.cpp`, `tests/test_overlay_fg_status_publication.cpp`, `tests/test_stubs.cpp`, `llm-wiki/current.md`, `llm-wiki/overlay-fg-status.md`, `llm-wiki/log/recent.md`

- **Stale risk**: Medium-low. The change stays tightly scoped to publication ordering and debug logging, not FG routing or queue ownership. Runtime validation is still important because this area depends on real call ordering across Streamline, FFX, DXGI, and menu/shutdown churn.

### 2026-04-23 - Fix stale DLSS FG overlay repaint after complex mixed-runtime switching by letting planner publications prefer DX12's visible state

- **Motivation**: Talos Principle Reawakened `installed/captureengine/logs/20260423_193106` on build `0.1.2555` still ended with the overlay showing `DLSS FG` even though the final state was "all FG off" after a long mixed sequence (`DLSS FG -> FSR FG -> off -> DLSS FG -> off`). The earlier same-day `ProcessFrame` fix was already in the tree, so the remaining seam had to come from a later publication path repainting stale state after `ProcessFrame` had already corrected it.

- **Root cause analysis**:
  1. `DX12::ProcessFrame` already published the locally-computed visible state, including SL-off suppression and the `STREAMLINE_NO_FG` / `off` distinction.
  2. However, several later refresh paths still published the raw planner state through `PublishOverlayFGMetrics(..., FGActionPlan, ...)`: `DXGIShared::DetourPresent`, `DX12_RenderOverlayViaFFXPresentCallback`, `DX12_OnStreamlineFGStateChanged`, and the synthetic `PostSL` refresh.
  3. If one of those later refreshes ran after `ProcessFrame` without a new per-frame correction, it could repaint an older planner view such as stale `DLSS FG` back onto the overlay.
  4. The same Talos session also logged `Failed to inline hook slDLSSGSetOptions`, which makes it even more important that shared publication paths prefer the latest DX12-visible state instead of assuming the planner publication order will always self-correct on the next frame.

- **Fix**:
  1. `hook/common/hook_common.h` + `hook/apis/dx12_hook.cpp` — Added a shared DX12-exported preferred overlay-publication state cache. `DX12::ProcessFrame` now updates it with the locally-computed visible FG state, and the Streamline transition callback plus native-FSR callback path update it too so immediate refreshes have a current visible state even before the next `ProcessFrame`.
  2. `hook/common/overlay_metrics_publisher.cpp` — The planner-driven publication overload now asks DX12 for that preferred visible state and overrides stale planner publications when they disagree. New logs (`FG publication preferred override: ...`) make future repaint races explicit in traces.
  3. `tests/test_overlay_fg_status_publication.cpp` + `tests/test_stubs.cpp` — Added regression coverage for planner-driven publication overriding a stale planner `DLSS FG` state with the latest DX12-visible `FSR FG` and then `off`.

- **Verification**:
  1. `python build.py --skip-updates --tests-only --run-tests --gtest-filter=OverlayFGStatusPublicationTest.*` passed all 7 focused tests and produced build `0.1.2556`.
  2. `python build.py --skip-updates` passed and produced build `0.1.2557`; `build/verification/latest_summary.txt` reports `success=1`.
  3. The full build still carried the existing pip-bootstrap permission noise and x86 test-app `%LOCALAPPDATA%\\Temp` rename failures, but the verification manifest marked the main build successful and produced the expected product artifacts.

- **Files changed**: `hook/apis/dx12_hook.cpp`, `hook/common/hook_common.h`, `hook/common/overlay_metrics_publisher.cpp`, `tests/test_overlay_fg_status_publication.cpp`, `tests/test_stubs.cpp`, `llm-wiki/current.md`, `llm-wiki/overlay-fg-status.md`, `llm-wiki/log/recent.md`

- **Stale risk**: Medium. The fix is intentionally small and only changes publication-state preference, not FG detection/routing, but this area still depends on call ordering across multiple DX12 / DXGI refresh paths and should be re-checked after future planner/publication churn.

### 2026-04-23 - Fix overlay erroneously showing DLSS FG after complex transitions by publishing locally-computed FG state in ProcessFrame

- **Motivation**: Talos Principle Reawakened `installed/captureengine/logs/20260423_175551` on build `0.1.2554` showed the overlay erroneously displaying `DLSS FG` (and briefly `FSR FG`) during a complex FG mode-switching sequence even though all FG was off. The log captured `DX12::ProcessFrame` publishing `runtime=FSR_FG active=1` while the local routing state correctly showed `fgActive=0 runtime=STREAMLINE_NO_FG`.

- **Root cause analysis**:
  1. `DX12::ProcessFrame` computes a locally-suppressed `currentFGActive`/`currentRuntimeMode` (e.g., during the 300-frame SL-off grace period that suppresses heuristic false-positives from departing DLSS FG).
  2. However, the overlay publication block at the end of `ProcessFrame` called `GetLatestFGActionPlan()` and published the raw global detection state, which still reflected the stale heuristic.
  3. Because `DetourPresent` calls `UpdateDXGIPresentMetricsAndPublish` (also using the raw plan) *before* `HandleDX12ProcessFrame`, the final per-frame state was whatever `ProcessFrame` published. As long as `ProcessFrame` kept publishing the stale plan every frame, the overlay stayed wrong for the entire grace period.

- **Fix**:
  1. `hook/apis/dx12_hook.cpp` — `ProcessFrame` now publishes `currentFGActive`/`currentRuntimeMode` directly via `PublicationInput` instead of the raw `FGActionPlan`.
  2. Added a divergence log (`DX12::ProcessFrame overlay divergence: plan(...) vs local(...)`) whenever the local suppressed state disagrees with the global plan, making future grace-period issues self-explanatory from logs alone.
  3. Also fixed a secondary edge case where `currentSLFGRunning && !currentFGActive` forced `currentFGActive=true` but left `currentRuntimeMode=kOff`, which would trigger an invariant violation inside `PublishOverlayFGMetrics`. The force now also sets `currentRuntimeMode=kDLSSFG` when the current mode is not already an active FG mode.

- **Verification**:
  1. `python build.py --skip-updates` passed and produced build `0.1.2555`.
  2. Full `tests\unit_tests.exe` ran 641 tests; all passed.

- **Files changed**: `hook/apis/dx12_hook.cpp`, `llm-wiki/overlay-fg-status.md`, `llm-wiki/log/recent.md`

- **Stale risk**: Low. The change is strictly scoped to the publication block inside `ProcessFrame`. No detection, routing, or transition logic is affected.

### 2026-04-23 - Fix PostSL startup deadlock after FSR→DLSS transition by relaxing safe-bootstrap and callback-on-normal-route gates

- **Motivation**: Talos Principle Reawakened `installed/captureengine/logs/20260423_155313` on build `0.1.2553` showed the overlay disappearing during a complex FG mode-switching sequence (`DLSS FG → FSR FG → all FG off → DLSS FG`). After the final DLSS reactivation at `15:54:32.595`, CE entered `DetourPresent: Post-FSR startup normal-route bypass` with `startupPending=1` and never exited; no `Post-SL overlay SUBMIT` or `ProcessFrame` occurred for the remainder of the session (~15+ seconds) despite `runtime=DLSS_FG active=1`.

- **Root cause analysis**:
  1. When Streamline FG turned OFF after FSR history, `g_RealD3D12ECL` was cleared during deferred non-FG overlay reinit (`ShouldPreserveRealECLForDelayedPostFSRNonFGRecovery` returned false).
  2. When DLSS turned ON again, `postSLSyntheticStartupActivationPending` was set to true, but `g_RealD3D12ECL` remained null.
  3. `HookHasSafePostFSRBootstrapPath()` depends on `ShouldDelayPostSLActivationUntilSafeBootstrapPath`, which unconditionally returned `true` when `hasRealD3D12ECL=false`, even if `hasSLWrapperQueue=true`.
  4. Because `safePostFSRBootstrapPath` was false, `ShouldInvokePostSLCallbackWhileKeepingStreamlinePresentOnNormalRoute` also required `explicitSetOptionsActivation` (which was false in this session) to invoke the PostSL callback on the normal route.
  5. The Present path kept bypassing without ever invoking the callback, and the ECL fallback (`ShouldTriggerExpiryDrivenECLPostSLStartupActivation`) was also gated by `safePostFSRBootstrapPath`, so it never fired either.
  6. Result: `postSLSyntheticStartupActivationPending` stayed true forever, `startupHalfArmed` stayed true, and every Present kept bypassing indefinitely.

- **Fix**:
  1. `hook/common/dx12_overlay_policy.h` — `ShouldDelayPostSLActivationUntilSafeBootstrapPath` now allows proceeding when `hasSLWrapperQueue=true` even if `hasRealD3D12ECL=false`. PostSL can bootstrap through the SL wrapper path (which then captures the real queue behind it on the first submit).
  2. `hook/common/dxgi_shared.h` — `ShouldInvokePostSLCallbackWhileKeepingStreamlinePresentOnNormalRoute` no longer requires `explicitSetOptionsActivation`. For post-FSR comebacks, `safePostFSRBootstrapPath` alone is sufficient to invoke the callback on the normal route. This aligns the callback gate with the ECL fallback, which already did not require explicit SetOptions.
  3. `hook/common/dxgi_shared.cpp` — Added skip-callback diagnostic logs in `DetourPresent` and `DetourPresent1` when the normal route keeps the startup present but decides not to invoke PostSL, making future deadlock diagnosis self-explanatory.
  4. `tests/test_dxgi_shared.cpp` — Updated `PostSLActivationWaitsForSafeBootstrapPathAfterFSRPhase` and `ConfirmedStartupSettlingCanStillInvokePostSLWithoutSyntheticBypass` to match the relaxed policy expectations.

- **Verification**:
  1. `python build.py --skip-updates` passed and produced build `0.1.2554`.
  2. Full `tests\unit_tests.exe` ran 641 tests; all passed.

- **Files changed**: `hook/common/dx12_overlay_policy.h`, `hook/common/dxgi_shared.h`, `hook/common/dxgi_shared.cpp`, `tests/test_dxgi_shared.cpp`, `llm-wiki/log/recent.md`

- **Stale risk**: Low. The relaxation is tightly bounded: it only affects post-FSR sessions where the SL wrapper queue is already captured but realECL has not yet been reprobed, and it only removes the `explicitSetOptionsActivation` requirement from the Present-path callback gate (the ECL path already lacked it). The core safety invariant — `safePostFSRBootstrapPath` must still be true — is unchanged.

### 2026-04-23 - Fix overlay disappearance after FSR→OFF→DLSS transitions in Talos by clearing stale heuristicFSRFGActive on authoritative FSR off

- **Motivation**: Talos Principle Reawakened `installed/captureengine/logs/20260423_152359` on build `0.1.2552` showed the overlay disappearing during a complex FG mode-switching sequence (`DLSS FG → FSR FG → all FG off → DLSS FG`). The log showed `runtime=FSR_FG` overlay deferral/skips from ~15:24:44 through ~15:24:53, even though FSR was explicitly turned off and Streamline reported `STREAMLINE_NO_FG active=0`.

- **Root cause analysis**:
  1. During the active FSR phase, `UpdateHeuristicFSRFGState(true, "queue-change")` latched `heuristicFSRFGActive=true` based on queue-change detection.
  2. When FSR was turned off via the API (`SetFSRFGActive(false)`), `heuristicFSRFGActive` was **not** cleared. The authoritative off signal only reset `fsrFGApiActive` and `fsrFGMultiplier`.
  3. `ClassifyRuntimeMode()` in `fg_runtime_state.h` checks `heuristicFSRFGActive` before `dlssFGApiActive`. With the stale heuristic still true and `streamlineFGSignal=false`, the mode resolved to `kFSRFG`.
  4. `ShouldSkipSeparateOverlayGpuWorkForRuntimeOwnedFrameGeneration()` returns true when `runtimeMode == kFSRFG`, causing `ProcessFrame` to skip overlay init and draw indefinitely.
  5. The secondary effect was that `SetDLSSFGActive(true)` could also be blocked if the stale heuristic survived into the DLSS reactivation window, because the existing suppression logic returns early when `heuristicFSRFGActive && !streamlineFGSignal`.

- **Fix**:
  1. `hook/common/fg_detection.cpp` — `SetFSRFGActive(false)` now explicitly clears `heuristicFSRFGActive` with a diagnostic log: `FG: Clearing heuristic FSR FG state — authoritative FSR FG turned OFF`. This ensures that any heuristic latched during an earlier queue-change window is invalidated when the authoritative API says FSR is off.
  2. `tests/test_fps_limiter.cpp` — Added two regression tests:
     - `AuthoritativeFSROffClearsStaleHeuristic`: verifies that `SetFSRFGActive(false)` clears a pre-existing heuristic flag.
     - `StaleHeuristicFSRClearedByAuthoritativeFSROffAllowsDLSSReactivation`: verifies the full FSR→off→DLSS sequence works correctly.

- **Verification**:
  1. `python build.py --skip-updates` passed and produced build `0.1.2553`.
  2. Full `.	ests	ests.exe` ran 641 tests; all passed.

- **Files changed**: `hook/common/fg_detection.cpp`, `tests/test_fps_limiter.cpp`, `llm-wiki/log/recent.md`

- **Stale risk**: Low. The fix is a single clear inside the authoritative FSR OFF path. No other callers are affected. The `SetDLSSFGActive(true)` suppression logic was intentionally left unchanged to preserve the existing heuristic-priority-over-transient-DLSS behavior; the stale heuristic is now prevented at the source (`SetFSRFGActive(false)`) rather than by weakening the DLSS guard.

### 2026-04-23 - Fix overlay disappearance after FSR→DLSS→OFF transitions in Talos by allowing primary game queue to satisfy deferral policy

- **Motivation**: Talos Principle Reawakened `installed/captureengine/logs/20260423_044543` on build `0.1.2551` showed the overlay disappearing during a long FSR/DLSS FG mode-switching sequence. After FSR turned off, then DLSS turned off, the log showed repeated `Deferring overlay init until command queue settles after recent Streamline teardown (scQ=null cmdQ=primaryQ origQ=... lastWorkingQ=null slOffGrace=...)` for ~4 seconds, with the overlay never recovering.

- **Root cause analysis**:
  1. When FSR turned off, CE released the stale `scQueue`, seeded `slOffGrace=600`, and deferred overlay reinit.
  2. For every subsequent `ProcessFrame`, `ShouldDeferOverlayInitUntilCommandQueueSettlesAfterRecentStreamlineTeardown` evaluated `hasSwapchainQueue=false` and `hasPostSLLastWorkingQueue=false`.
  3. The function returned `true` immediately—**before** evaluating the `commandQueueMatchesPrimaryGameQueue` escape hatch. Because `lastWorkingQ` was null, the overlay was locked out for the full 600-frame grace window.
  4. `lastWorkingQ` was null because it had been cleared during an earlier FSR→DLSS ON transition (`ShouldInvalidatePostSLLastWorkingQueueOnFreshPostFSRStreamlineHandoff`), and during the DLSS epoch PostSL never activated (`safeBootstrap=0`, no real queue behind wrapper, no SL wrapper queue), so `lastWorkingQ` was never repopulated.
  5. When DLSS later turned OFF, there was no `lastWorkingQ` to preserve and no `scQueue`, leaving the recovery path stranded despite the primary game queue being perfectly valid.

- **Fix**:
  1. `hook/common/dx12_overlay_policy.h` — `ShouldDeferOverlayInitUntilCommandQueueSettlesAfterRecentStreamlineTeardown` now evaluates `commandQueueMatchesPrimaryGameQueue` **even when `hasSwapchainQueue` is false**. If the current command queue matches the primary game queue, deferral is skipped. This prevents indefinite deferral when the only proven-safe queue is the primary game queue.
  2. `hook/apis/dx12_hook.cpp` — Added a one-shot diagnostic log that fires when the primary-queue escape hatch allows overlay init despite missing `scQueue` and `lastWorkingQ`, making future post-FSR recovery traces self-explanatory.
  3. `tests/test_dxgi_shared.cpp` — Added `EXPECT_FALSE` test case in `PostFSRStreamlineTeardownWithoutSwapchainQueueWaitsForLiveNonWrapperQueue` for the new primary-queue behavior.

- **Crash context**:
  - The same Talos session also crashed with exception `0x00008000` inside `sl_dlss_g` (Streamline DLSS FG) on the present thread, called via `sl_interposer`. This is a Streamline internal threading/mutex exception, not directly caused by CE code. The rapid ON/OFF toggling stresses Streamline's fragile multi-device initialization/teardown. The overlay-deferral fix may mitigate it indirectly by restoring normal GPU work promptly instead of leaving the game in a deferral loop for hundreds of frames.

- **Verification**:
  1. `python build.py --skip-updates` passed and produced build `0.1.2552`.
  2. Full `.	ests	ests.exe` ran 639 tests; all passed.

- **Files changed**: `hook/common/dx12_overlay_policy.h`, `hook/apis/dx12_hook.cpp`, `tests/test_dxgi_shared.cpp`, `llm-wiki/log/recent.md`

- **Stale risk**: Low. The new branch is tightly scoped: it only triggers when both `scQueue` and `lastWorkingQ` are null, and only if the current queue is the primary game queue. If a title uses a different queue during teardown, the old deferral behavior is preserved.
