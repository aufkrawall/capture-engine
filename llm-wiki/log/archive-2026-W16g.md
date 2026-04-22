# llm-wiki Log — Archive 2026-W16g

### 2026-04-13 - Add staged observer-policy-only Streamline startup-policy probe

- **Motivation**: The clean observer-only GTA V Enhanced session `installed/captureengine/logs/20260413_203815` confirmed that the strict passive injected baseline no longer froze and no longer misclassified transient Streamline churn as heuristic `FSR_FG`. That established the next bisect seam: re-enable only Streamline startup-policy mutation while keeping DX12 overlay rendering, PostSL, and special startup Present routing passive, so we can determine whether the remaining GTA DLSS FG freeze family lives inside startup-policy mutation itself or only in the later PostSL/startup-Present path.

- **Fix**:
  1. `common/shared_defs.h`, `common/config.cpp`, `common/config.h`, `hook/common/hook_common.h`, `captureengine/main.cpp`, and `captureengine/inject_main.cpp` now expose `Overlay.observer_policy_only=true` as a new staged probe. It is only meaningful with `observer_only=true`, bumps the shared-memory version to 24, is parsed from config/overrides, is visible in the shared-memory/inject summary log, and is recorded in `session_manifest.txt` as `overlay_observer_policy_only`.
  2. `hook/apis/streamline_hook.cpp` now treats only `observer_only && !observer_policy_only` as the fully passive branch. When `observer_policy_only=true`, the Streamline hook re-enables the startup-policy family: startup transition window arm/extend, startup-window OFF suppression/flush, GetState-only activation suppression, and FSR-owned Streamline-enable preparation.
  3. `hook/apis/dx12_hook.cpp` still keeps observer modes passive for DX12/PostSL behavior, but `observer_policy_only` now preserves the startup transition window instead of clearing it during observer-mode cleanup. That keeps the startup-policy seam alive without re-enabling PostSL callback install/use or startup-handoff activation state.
  4. `hook/common/dxgi_shared.h` / `.cpp` now keep the FFX startup Present bypass disabled in all observer modes, so `observer_policy_only` does not accidentally re-enable the DXGI startup-Present path while probing only the Streamline startup-policy family.
  5. Tests now cover the new split in `tests/test_config.cpp`, `tests/test_config_override.cpp`, `tests/test_shared_runtime_state.cpp`, `tests/test_streamline_runtime_policy.cpp`, and `tests/test_dxgi_shared.cpp`.

- **Why this is generic**: We need a staged path between the strict passive baseline and full active CE behavior. `observer_policy_only` lets us ask a precise generic question: does Streamline startup-policy mutation alone destabilize the runtime, even when PostSL rendering/callbacks and special startup Present routing are still absent? That is a reusable debugging seam for any future Streamline DLSS FG startup issue, not a GTA-specific hack.

- **Verification**:
  - Ran `python build.py --incremental --skip-updates --run-tests`.
  - All 559 tests passed.
  - Build version bumped to `0.1.2261`.

- Pages touched: `current.md`, `frame-generation-switching.md`, `regression-testing-and-logging.md`, `log.md`.
- Source files checked/modified: `common/shared_defs.h`, `common/config.cpp`, `common/config.h`, `captureengine/main.cpp`, `captureengine/inject_main.cpp`, `hook/common/hook_common.h`, `hook/common/streamline_runtime_policy.h`, `hook/common/dxgi_shared.h`, `hook/common/dxgi_shared.cpp`, `hook/apis/dx12_hook.cpp`, `hook/apis/streamline_hook.cpp`, `tests/test_config.cpp`, `tests/test_config_override.cpp`, `tests/test_shared_runtime_state.cpp`, `tests/test_streamline_runtime_policy.cpp`, `tests/test_dxgi_shared.cpp`, `installed/captureengine/logs/20260413_203815/hook_debug.log`.
- Stale-risk note: Fresh runtime validation with `[Overlay] observer_only=true` and `[Overlay] observer_policy_only=true` is still needed. Re-check this staged seam first if logs unexpectedly show PostSL callback install/use, synthetic/startup Present routing, or FFX startup Present bypass activity while `observer_policy_only` is active.

### 2026-04-13 - Preserve Streamline heuristic cleanup in observer-only mode

- **Root cause**: The good observer-only GTA V Enhanced run `installed/captureengine/logs/20260413_200023` no longer froze, which confirmed the active DX12/PostSL/startup policy family as the likely crash trigger. But the same passive run exposed a separate correctness bug: after transient `Streamline Hook: FG state transition ON->OFF via GetState (observer-only pass-through)` churn at lines 714-716 and again 823-825, observer-only mode skipped the normal Streamline transition cleanup that active mode performs. That left the queue-change / ECL heuristics free to reclassify the still-Streamline-owned runtime as heuristic `FSR_FG`, producing `DX12: FG detected via queue change`, `DetourPresent: SL routing was active while FSR FG is active`, `DX12: Preparing for Streamline FG enable while live FSR runtime owns the swapchain`, and later `FG publication ... runtime=FSR_FG` even though the same session had already logged `FFX Hook: No FFX modules found` and `No AMD/FFX/FSR modules among 163 loaded`.

- **Fix**:
  1. `hook/common/streamline_runtime_policy.h` now exposes `ResolveObserverOnlyHeuristicCleanupForStreamlineSignalTransition()` so the passive cleanup behavior is explicit and unit-testable.
  2. `hook/apis/dx12_hook.cpp` now applies that cleanup even in observer-only mode: fresh Streamline ON clears stale teardown grace, Streamline OFF seeds teardown grace, both edges reset the queue-change heuristic, and false heuristic `FSR_FG` / `NVIDIA_SM` state is cleared before observer-only returns to its passive PostSL-disabled path.
  3. `tests/test_streamline_runtime_policy.cpp` now covers both observer-only cleanup directions.

- **Why this is generic**: Observer-only is meant to suppress CE's active overlay/PostSL/startup-policy interference, not to disable runtime-state hygiene. Any Streamline runtime that emits transient `ON->OFF->ON` churn can otherwise be misclassified as heuristic FSR purely because passive mode stopped doing the same heuristic cleanup that active mode already relies on.

- **Verification**:
  - Ran `python build.py --incremental --skip-updates --run-tests`.
  - All 557 tests passed.

- Pages touched: `current.md`, `frame-generation-switching.md`, `log.md`.
- Source files checked/modified: `hook/apis/dx12_hook.cpp`, `hook/common/streamline_runtime_policy.h`, `tests/test_streamline_runtime_policy.cpp`, `installed/captureengine/logs/20260413_200023/hook_debug.log`, `llm-wiki/current.md`, `llm-wiki/frame-generation-switching.md`, `llm-wiki/log.md`.
- Stale-risk note: Re-check observer-only semantics if future runs again show heuristic `FSR_FG` activation immediately after observer-only `Streamline ... ON->OFF via GetState` churn, especially if Streamline remains loaded and no FFX module evidence exists.

### 2026-04-13 - Documented observer-only passive baseline for injected DX12 / Streamline diagnosis

- **Root cause / diagnostic correction**: In the GTA V Enhanced bundle `installed/captureengine/logs/20260413_192027`, `[Overlay] enabled=false` was not a true no-overlay baseline. `installed/captureengine/config.ini` had the visible overlay disabled, but `hook_debug.log` still showed `DX12: ProcessFrame - first overlay render attempt` at line 576 plus `DX12: Streamline FG ON — installed gated PostSL callback` and `DX12: Streamline FG ON — pre-armed PostSL callback for startup routing` at lines 857-858 before the usual promoted startup-handoff Present and `Present STALLED`. Previous "overlay disabled" reproductions therefore still included CE DX12/PostSL/startup-policy interference.

- **Behavior now documented as current state**:
  1. `Overlay.observer_only=true` is the strict injected passive baseline. It keeps hooks, logging, and runtime FG telemetry alive while suppressing DX12 overlay rendering, early/PostSL callback install/use, special Streamline synthetic/startup Present routing, and CE startup-window Streamline policy mutation.
  2. `captureengine/main.cpp` and `captureengine/inject_main.cpp` now make passive-vs-active sessions self-describing through `overlay_enabled`, `overlay_observer_only`, and the inject config summary log.
  3. The older active-mode startup-window OFF-suppression fix remains important, but it is no longer the right "no CE interference" baseline for GTA DLSS FG diagnosis.

- **Why this is generic**: We need a true injected passive baseline to separate "hook presence alone causes the problem" from "overlay/PostSL/startup policy causes the problem" without disabling injection, logging, and runtime telemetry entirely. This is a generic FG-diagnosis tool, not a GTA-specific workaround.

- **Verification checked while updating docs**:
  - Re-checked the current observer-only implementation in `common/shared_defs.h`, `common/config.cpp`, `hook/common/hook_common.h`, `hook/apis/dx12_hook.cpp`, `hook/apis/streamline_hook.cpp`, `hook/common/dxgi_shared.cpp`, `captureengine/main.cpp`, and `captureengine/inject_main.cpp`.
  - Re-checked the motivating session evidence in `installed/captureengine/config.ini` and `installed/captureengine/logs/20260413_192027/hook_debug.log`.

- Pages touched: `current.md`, `frame-generation-switching.md`, `regression-testing-and-logging.md`, `log.md`.
- Source files checked: `common/shared_defs.h`, `common/config.cpp`, `common/config.h`, `hook/common/hook_common.h`, `hook/apis/dx12_hook.cpp`, `hook/apis/streamline_hook.cpp`, `hook/common/dxgi_shared.cpp`, `captureengine/main.cpp`, `captureengine/inject_main.cpp`, `tests/test_config.cpp`, `tests/test_config_override.cpp`, `tests/test_shared_runtime_state.cpp`, `installed/captureengine/config.ini`, `installed/captureengine/logs/20260413_192027/hook_debug.log`.
- Stale-risk note: Fresh runtime validation with `Overlay.observer_only=true` is still needed. Re-check these docs if the logs ever show pre-FG overlay submits, PostSL callback install/use, special startup Present routing, or startup-window Streamline policy mutation while observer-only is active.

### 2026-04-13 - Dump Thread Analysis: Present thread waiting on fence, Streamline thread deadlocked

- **Thread analysis of crash 172647**: Used cdb to enumerate all threads and analyze stacks of key threads.

- **Thread mapping**:
  - Thread 38 (0x6e08 = 28168): Watchdog thread stuck in MessageBoxW showing ERR_GFX_STATE dialog
  - Thread 39 (0x6e0c = 28172): Present thread (mentioned in hook_debug.log at line 882) stuck in `WaitForSingleObjectEx` - waiting on some synchronization object that never completes
  - Thread 37 ("sl.log"): Streamline interposer thread stuck in `sl_interposer!vkGetInstanceProcAddr` waiting on a condition variable via `msvcp140!_Cnd_wait`
  - Thread 48 (D3D Background Thread 0): D3D12Core background thread stuck waiting on condition variable

- **Key finding**: The present thread (39) is NOT inside sl_dlss_g as previously thought - it's waiting on a synchronization object. The Streamline thread (37) is stuck in a condition variable wait. This suggests a deadlock or wait starvation in Streamline's internal synchronization.

- **Exception info**: The stored exception is `e0000001` ("Blocked kernel") on thread 0x6e08. This is the watchdog thread that captured the dump when ERR_GFX_STATE was detected. The actual crash may have occurred on a different thread before the dump was captured.

- **Timeline confirmation**: Crash 172647 timeline verified:
  - 17:28:03.408 - GetState OFF->ON (startupWindowActive=1, consumed=0, wrapperProgress=0)
  - 17:28:03.425 - Synthetic re-entrant Present #1 detected (T:6E0C)
  - 17:28:03.876 - ERR_GFX_STATE detected (~468ms after GetState ON)

- **Talos comparison**: Talos successfully handles DLSS FG with no crash. Key difference: Talos completes PostSL activation (~741ms after GetState ON) and renders overlay successfully. In GTA, crash occurs ~468ms after GetState ON, before PostSL activation completes.

- **Stale-risk note**: The crash appears to be a deadlock between the present thread (waiting on a fence that never completes) and Streamline's internal threads (waiting on condition variables). This is NOT directly related to CE's PostSL callback - it's an internal Streamline synchronization issue. The ~500ms timing suggests an internal Streamline timeout or timer that fires during multi-device DLSS FG initialization.

### 2026-04-13 - Restore FRESH activation fix in ShouldDeferPostSLCallbackUntilStartupTransitionWindowExpires

- **Root cause**: An uncommitted local modification changed `hook/common/dx12_overlay_policy.h` line 1167 from `return startupActivationPending || postSLActive;` to `return postSLActive;`. This effectively removed the `startupActivationPending` check from the callback deferral logic.

- **Crash 170236 analysis**: Debug build with detailed PostSL logging showed the callback entered and PostSLOverlayRender returned successfully, yet ERR_GFX_STATE still appeared ~455ms later. This proved the crash was NOT inside PostSLOverlayRender itself.

- **Crash 172647 analysis (new)**: Build 0.1.2258, GetState ON at 17:28:03.408 (startupWindowActive=1, consumed=0, wrapperProgress=0). The PostSL callback was DEFERRED at line 1156 (consumed=0 triggered early return), so PostSLOverlayRender was NEVER called. ERR_GFX_STATE still appeared ~450ms after GetState ON (at 17:28:03.876). This proves the crash is NOT caused by whether our PostSL callback enters - it happens regardless of callback deferral.

- **CRITICAL CONCLUSION**: The crash is internal to Streamline's multi-device DLSS FG initialization. It occurs ~500ms after GetState returns ON, regardless of whether CE's PostSL callback runs, is deferred, or is never called. The crash targetTid=28168 is a different thread from the present thread T:6E0C, suggesting Streamline's internal threading is causing the issue, not CE's present path.

- **Fix**: Reverted the uncommitted modification in `hook/common/dx12_overlay_policy.h` to restore `return startupActivationPending || postSLActive;` at line 1167.

- **Additional changes retained**: The `dx12_hook.cpp` debug logging changes were retained: (1) Enhanced logging for GetState transition showing startup window state, consumed top-level bootstrap, and wrapper progress at the moment of GetState ON; (2) A `g_DeviceRemoved` safety check that skips PostSL callback if ERR_GFX_STATE has already been detected.

- **Verification**: Build completed successfully, version bumped to 0.1.2258.

- Pages touched: `log.md`.
- Source files checked/modified: `hook/common/dx12_overlay_policy.h`, `hook/apis/dx12_hook.cpp`.
- Stale-risk note: The crash is NOT preventable through callback deferral logic. The crash is a Streamline internal timing issue that occurs ~500ms after GetState ON. Potential approaches: (1) Investigate whether there's a way to delay or suppress GetState ON to prevent Streamline's multi-device initialization from starting; (2) Consider whether CE can detect and avoid triggering whatever causes the threading race in sl_dlss_g; (3) The crash is on a different thread (T:28168) than the present thread (T:6E0C), suggesting it's internal to Streamline's worker thread model.

### 2026-04-13 - Null swapchain guard and startup window clearing: Fix PostSL stall when ECL hook triggers callback directly

- **Root cause**: In GTA V Enhanced bundle `installed/captureengine/logs/20260413_023052`, even the two-pronged PostSL activation trigger fix (ECL hook + FlushSuppressedSetOptionsOffIfNeeded) still caused a freeze. The freeze timeline: (1) at 02:32:24.026, startup transition window armed; (2) at 02:32:24.159, DLSS FG ON via SetOptions; (3) at 02:32:25.576, Pure-DLSS startup stall detected (dormant=1312ms); (4) at 02:32:26.262, ECL hook detected window expiry and triggered PostSL callback directly with `postSLCallback(nullptr)`; (5) at 02:32:26.423, PostSL REACTIVATED with warm-up frame 1/15; (6) at 02:32:28.766, Present STALLED for 10 frames.

  The problem was that when ECL hook triggered `postSLCallback(nullptr)`, this called `PostSLOverlayRenderGated(nullptr)` → `PostSLOverlayRender(nullptr)`. In `PostSLOverlayRender`, the bootstrap section at line 6378 calls `pSwapChain->GetDesc()` with nullptr, which would crash. Even if guarded, the overlay state was never properly initialized with a null swapchain, causing PostSL to enter warm-up but never actually render (since the bootstrap/initialization failed silently). The "Present STALLED" occurred because PostSL was stuck in a state where it thought it was active but had never successfully rendered.

  Additionally, the startup transition window was not being explicitly cleared when the ECL hook triggered the callback, causing potential race conditions between threads about whether the window was still "active".

- **Fix**:
  1. `hook/apis/dx12_hook.cpp` ECL hook section now clears the startup transition window immediately when it triggers the PostSL callback directly (`DXGIShared::ClearStreamlineStartupTransitionWindow()`). This ensures the window is definitively cleared and won't cause deferred rendering on subsequent calls.
  
  2. `hook/apis/dx12_hook.cpp` `PostSLOverlayRenderGated()` now has a null swapchain guard: if `pSwapChain == nullptr`, it skips calling `PostSLOverlayRender(nullptr)` (which would cause bootstrap failure) and logs a message indicating it's waiting for the normal ProcessFrame path with a valid swapchain. This prevents the broken bootstrap/initialization that was causing the stall.
  
  3. `hook/apis/dx12_hook.cpp` adds comprehensive debug logging at key points:
     - When ECL hook detects window expiry and triggers callback (with window state info)
     - When PostSL callback is skipped due to null swapchain
     - When warmup completes and we're about to proceed to render submission (shows swapchain validity, overlayInit, syncInit state)
  
  4. `hook/apis/streamline_hook.cpp` `FlushSuppressedSetOptionsOffIfNeeded()` now also clears the startup transition window before triggering the PostSL callback (in both Case 1 and Case 2), matching the ECL hook behavior.

- **Why this is generic**: The null swapchain issue can occur whenever the PostSL callback is triggered outside the normal ProcessFrame path. The startup window must be explicitly cleared in all callback trigger paths to prevent race conditions. The debug logging improvements help diagnose future issues by providing visibility into swapchain validity and overlay state at critical transition points.

- **Verification**:
  - Build completed successfully (`python build.py --skip-updates`).
  - Build version bumped to 0.1.2251.

- Pages touched: `log.md`.
- Source files modified: `hook/apis/dx12_hook.cpp`, `hook/apis/streamline_hook.cpp`.
- Stale-risk note: The null swapchain guard prevents crashes but also means the ECL hook callback trigger is now essentially a no-op for activation completion — the actual activation will complete on the next normal ProcessFrame call. Verify this behavior is correct for all scenarios where the callback is triggered with nullptr. If future traces show the callback being triggered but no subsequent PostSL activation, check if the normal ProcessFrame path is being reached after the ECL hook trigger.

### 2026-04-13 - Two-pronged PostSL activation trigger: ECL hook catches window expiry even when ProcessFrame stalls

- **Root cause**: In GTA V Enhanced bundle `installed/captureengine/logs/20260413_015250` and `installed/captureengine/logs/20260413_021310`, the previous one-pronged fix (`FlushSuppressedSetOptionsOffIfNeeded` calling PostSL callback when suppressed OFF was flushed with activation pending) failed because the suppressed OFF was cleared by an ON re-arrival before the startup window expired. The specific failure sequence: (1) at 02:14:51.388, new authoritative swapchain handoff armed startup window; (2) at 02:14:51.522, OFF->ON via SetOptions arrived; (3) at 02:14:51.618, promoted handoff Present with `activationPending=1, postSLActive=1, bypass=1`; (4) at 02:14:51.919, OFF suppressed during startup window; (5) at 02:14:52.212, ON re-arrived and **cleared** the suppressed OFF (since Streamline never received the OFF); (6) ProcessFrame stopped running on T:5778 at ~02:14:50 (last ProcessFrame at 02:14:50.310); (7) startup window expired ~500ms after being armed; (8) `FlushSuppressedSetOptionsOffIfNeeded` was never called again (no more Present calls), so the callback was never triggered; (9) Streamline eventually timed out and sent ON->OFF via GetState at 02:15:04.485, 13 seconds after the promoted handoff.
- **Fix**:
  1. `hook/apis/streamline_hook.cpp` rewrites `FlushSuppressedSetOptionsOffIfNeeded()` into two cases: **Case 1** (suppressed OFF exists): forward it to Streamline, then trigger PostSL callback if `activationPending && callbackInstalled`. **Case 2** (no suppressed OFF, but startup window expired and activation pending): trigger PostSL callback directly. The Case 2 path covers the scenario where ON re-arrived and cleared the suppressed OFF before the window expired.
  2. `hook/apis/dx12_hook.cpp` adds window-expiry detection to the ECL hook (`DetourExecuteCommandLists`): a static variable `s_startupWindowWasActive` tracks whether the startup window was previously active; on each ECL call, if the window transitioned from active to inactive AND `activationPending && callbackInstalled`, the PostSL callback is triggered directly from the ECL context. This catches the window expiry even when ProcessFrame has stalled (which happens in the freeze — the ECL hook runs on T:5778 while ProcessFrame is not called).
  3. `hook/apis/streamline_hook.h` updates the doc comment to explain both the suppressed-OFF and no-suppressed-OFF cases.
  4. `tests/test_dxgi_shared.cpp` replaces the old `PostSLActivationPendingAndPostSLActiveBothTrueRequiresDirectCallbackOnFlush` test with `PostSLActivationPendingRequiresDirectCallbackOnFlushRegardlessOfPostSLActive` (tests activationPending alone is sufficient) and `PostSLDeferredCallbackStillNeedsActivationPendingTriggerOnFlush` (tests the deferred callback case with postSLActive=false).
- **Why this is generic**: The two-pronged approach covers both crash families: (a) suppressed OFF exists when window expires → Case 1 of `FlushSuppressedSetOptionsOffIfNeeded` handles it; (b) no suppressed OFF when window expires (ON re-arrived) → ECL hook catches the window transition on the present thread even when ProcessFrame has frozen. Both rely only on `activationPending` as the ground-truth condition for "PostSL activation has not completed" — the callback being installed (`callbackInstalled`) means there's something to trigger; the callback being deferred (postSLActive=false) is irrelevant to whether we should try.
- **Verification**:
  - Build completed successfully with all changes.
- Pages touched: `frame-generation-switching.md`, `log.md`.
- Source files modified: `hook/apis/streamline_hook.cpp`, `hook/apis/streamline_hook.h`, `hook/apis/dx12_hook.cpp`, `tests/test_dxgi_shared.cpp`.
- Stale-risk note: Re-check this family if the ECL hook flow, ProcessFrame call frequency, or the startup window arm/expiry mechanics change. The critical invariant is: when `activationPending && callbackInstalled` and the startup window has expired, the PostSL callback must be triggered. Verify both paths (DetourPresent periodic flush AND ECL hook transition detection) are still wired and functional.

### 2026-04-13 - Trigger PostSL callback directly when suppressed OFF is flushed with activation still pending

- **Root cause**: In GTA V Enhanced bundle `installed/captureengine/logs/20260413_002108`, the previous OFF suppression during the startup window was correctly in place, but the freeze still occurred when `FlushSuppressedSetOptionsOffIfNeeded()` forwarded the buffered OFF to Streamline after the startup window expired. The specific failure sequence: (1) at startup, `OFF->ON via SetOptions` was forwarded to Streamline; (2) one startup-handoff Present was promoted to top-level live, which bypassed the synthetic re-entrant path where `PostSLOverlayRenderGated()` normally fires; (3) PostSL callback was deferred by `ShouldDeferPostSLCallbackUntilStartupTransitionWindowExpires()` since the startup window was still active; (4) no synthetic Presents arrived after the promoted startup-handoff Present to drive PostSL activation; (5) `g_PostSLSyntheticStartupActivationPending` stayed `true` forever; (6) when `FlushSuppressedSetOptionsOffIfNeeded()` flushed the buffered OFF, Streamline received OFF without ON ever completing through the callback path — the overlay never activated, and the present thread stalled inside Streamline.
- **Fix**:
  1. `hook/common/dxgi_shared.h` now exposes `postSLSyntheticStartupActivationPending` in `SharedState` so `streamline_hook.cpp` can check whether PostSL activation is still pending.
  2. `hook/apis/dx12_hook.cpp` now references `DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending` instead of the old static `g_PostSLSyntheticStartupActivationPending`, and includes it in startup-window callback deferral logging.
  3. `hook/apis/streamline_hook.h` documents that `FlushSuppressedSetOptionsOffIfNeeded()` also calls the PostSL callback directly when activation is still pending.
  4. `hook/apis/streamline_hook.cpp` rewrites `FlushSuppressedSetOptionsOffIfNeeded()`: after forwarding the suppressed OFF to Streamline, if `activationPending && postSLActive`, it calls `g_PostSLOverlayRenderCallback(nullptr)` directly to trigger PostSL activation before Streamline processes the OFF signal.
  5. `hook/common/dxgi_shared.cpp` adds enhanced logging for startup-handoff Present paths including `activationPending` and `postSLActive` state.
  6. `tests/test_dxgi_shared.cpp` adds three regression tests: `PureDLSSStartupCallbackStaysDormantUntilStartupWindowExpires` now also covers `activationPending && postSLActive` both true; `PostSLSyntheticStartupActivationPendingTracksStartupleHandoffBypassState` exercises the new atomic flag; `PostSLActivationPendingAndPostSLActiveBothTrueRequiresDirectCallbackOnFlush` verifies the combined condition that drives the direct PostSL callback.
- **Why this is generic**: Any Streamline DLSS FG runtime can expose a startup family where the one promoted startup-handoff Present takes the top-level bypass path and no further synthetic Presents arrive during the startup window. In that family, PostSL activation is deferred indefinitely because no callback entry point fires. When the suppressed OFF is eventually flushed, Streamline receives the OFF without ON ever completing through PostSL, which destabilizes the FG pipeline. Triggering the PostSL callback directly in this scenario ensures CE can attempt activation completion before the OFF tears down Streamline's FG state.
- **Verification**:
  - Ran `python build.py --skip-updates --run-tests`. Build completed successfully and all 554 tests passed (3 new tests, 1 expanded).
- Pages touched: `frame-generation-switching.md`, `log.md`.
- Source files modified: `hook/common/dxgi_shared.h`, `hook/apis/dx12_hook.cpp`, `hook/apis/streamline_hook.h`, `hook/apis/streamline_hook.cpp`, `hook/common/dxgi_shared.cpp`, `tests/test_dxgi_shared.cpp`.
- Stale-risk note: Re-check this family whenever `FlushSuppressedSetOptionsOffIfNeeded()`, PostSL callback invocation, or startup-handoff Present bypass routing changes. If a future trace again shows a promoted startup-handoff Present followed by no PostSL activation and eventual Present thread stall, verify that the direct PostSL callback trigger in the flush path is still wired and that `postSLSyntheticStartupActivationPending` is correctly set/cleared.

### 2026-04-13 - Suppress slDLSSGSetOptions(OFF) forwarding to Streamline during DLSS FG startup transition window

- **Root cause**: In GTA V Enhanced bundle `installed/captureengine/logs/20260412_233910`, DLSS FG still froze (black window, watchdog dump) when the game later enabled DLSS FG. The crash dump placed the game's present thread inside `sl_dlss_g` throwing `std::_Throw_C_error` (threading/mutex family). The hook log showed a rapid `OFF->ON` activation at 23:40:50.408, the startup-handoff Present at 23:40:50.525, then `GetState(OFF)` and `SetOptions(OFF)` at 23:40:50.834 — both deferred internally by CE to keep `g_StreamlineFGRunning=ON`, but both also **forwarded to the real Streamline runtime via `g_Original_slDLSSGSetOptions`**. The real `sl_dlss_g` module received the OFF signal while its FG initialization was still in progress, entered a threading race, and threw the exception that froze the present thread. CE's internal deferral was correct, but the real Streamline call was not suppressed; Streamline started de-initializing FG while CE still routed Presents through the SL chain, creating a fatal inconsistent state.
- **Fix**:
  1. `hook/apis/streamline_hook.cpp` now suppresses the real `g_Original_slDLSSGSetOptions(viewport, adjustedOptions)` call when mode is OFF and the DLSS FG startup transition window is active. Instead of forwarding the OFF command to Streamline, the hook returns `kSlResultOk` immediately and buffers the suppressed OFF request (viewport + options) for later forwarding.
  2. When a subsequent `slDLSSGSetOptions(mode=ON)` arrives (the re-enable during churn), the buffered OFF is cleared because Streamline never received the OFF — the ON call is consistent with Streamline's current state.
  3. When the startup transition window expires while a buffered OFF is still pending (no re-enable arrived), the buffered OFF is forwarded to Streamline on the next `slDLSSGGetState` or `slDLSSGSetOptions` call, and also via a periodic flush in `DetourPresent`/`DetourPresent1`.
  4. `hook/common/streamline_runtime_policy.h` now exposes `ShouldSuppressSetOptionsOffDuringStartupTransitionWindow()` so the suppression policy is explicit and testable.
  5. `tests/test_streamline_runtime_policy.cpp` now covers the new policy seam.
- **Why this is generic**: This is not GTA-specific. Any Streamline DLSS FG runtime can emit rapid `OFF->ON` churn during startup initialization. If CE forwards the OFF signal to Streamline during this fragile window, `sl_dlss_g` can enter a threading race and throw an exception while the present thread is still inside Streamline's Present hook. Suppressing the real OFF call during the startup window prevents Streamline from starting de-initialization while its FG pipeline is still fragile, which is the correct generic behavior.
- **Verification**:
  - Ran `python build.py --incremental --skip-updates --run-tests`. Build completed successfully and all 552 tests passed (1 new `StreamlineRuntimePolicyTest.SuppressSetOptionsOffDuringStartupTransitionWindow` test added).
- Pages touched: `frame-generation-switching.md`, `log.md`.
- Source files checked/modified: `hook/apis/streamline_hook.cpp`, `hook/apis/streamline_hook.h`, `hook/common/streamline_runtime_policy.h`, `hook/common/dxgi_shared.cpp`, `tests/test_streamline_runtime_policy.cpp`, `tests/test_stubs.cpp`, `installed/captureengine/logs/20260412_233910/hook_debug.log`, `installed/captureengine/logs/20260412_233910/GTA5_Enhanced.exe_FREEZE_2026-04-12_23-40-51_899.dmp`.
- Stale-risk note: Re-check this family whenever `slDLSSGSetOptions` hook handling changes or when the startup transition window duration or deferral semantics change. If a future trace again shows `sl_dlss_g` throwing `_Throw_C_error` right after a startup OFF signal, verify first that the real OFF call is still being suppressed during the startup window.

