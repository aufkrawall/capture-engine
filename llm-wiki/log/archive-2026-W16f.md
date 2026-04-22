# llm-wiki Log — Archive 2026-W16f

### 2026-04-15 - Keep the first post-activation PostSL warm-up callbacks off the synthetic/bypass path too

- **Motivation**: The fresh GTA V Enhanced validation `installed/captureengine/logs/20260415_010931` on build `0.1.2275` proved the previous consumed-bootstrap-latch fix worked and moved the crash boundary further again. CE now survived the cached-swapchain ECL expiry callback, logged `DX12: PostSL synthetic startup activation complete`, `DX12: PostSL REACTIVATED`, and then even entered `DX12: PostSL warm-up after reactivation epoch=1 frame=1/15` and `frame=2/15`. But the crash still returned immediately after on the next `DetourPresent: Treating Streamline-originated Present as synthetic re-entrant #1`. That showed the remaining seam was no longer "startup still pending". PostSL was already activated. The bad seam was specifically the first post-activation callbacks while PostSL was active but still had not confirmed a successful render.

- **Fix**:
  1. `hook/common/hook_common.h` now declares `HookIsPostSLOverlayActiveButUnconfirmed()`, and `hook/apis/dx12_hook.cpp` exports it using the existing `g_PostSLOverlayActive` and `g_PostSLConfirmedRendering` state.
  2. `hook/common/dxgi_shared.h` widens `ShouldKeepSyntheticStartupStreamlinePresentOnNormalRoute()` again: once the one-shot startup bootstrap has been consumed, Streamline-originated startup Presents now stay off the synthetic/bypass path not only while activation is pending, but also while PostSL is already active and still unconfirmed.
  3. `hook/common/dxgi_shared.cpp` now uses that DX12-side unconfirmed-PostSL signal in both `DetourPresent` and `DetourPresent1`, so the first post-activation warm-up callbacks remain on the normal SL route too.
  4. `tests/test_dxgi_shared.cpp` updates `DXGISharedTest.WrapperBackedSyntheticStartupPresentCanStayOnNormalRouteInActiveMode` to cover both halves of the widened invariant, and `tests/test_stubs.cpp` now provides the small test-only stub for the new hook-common accessor.

- **Why this is generic**: Activation and confirmed rendering are not the same state. In this startup family, CE can finish activation and still spend the next few callbacks on PostSL warm-up before any GPU submit/confirmed render has happened. That phase is still half-armed: CE has not yet proven the first real PostSL-rendering callback is safe. Treating those warm-up callbacks as ordinary synthetic/bypass traffic reopens the same fragile Streamline Present seam one step later in the startup state machine. The generic safe invariant is therefore "until PostSL has confirmed rendering", not merely "until activation completed".

- **Verification**:
  - Re-checked `installed/captureengine/logs/20260415_010931/{session_manifest.txt,hook_debug.log,external_d249d67d-17d8-4c21-b872-6da225911223.dmp}`.
  - Analyzed `external_d249d67d-17d8-4c21-b872-6da225911223.dmp` with `cdb.exe`; it is still the same `dxgi!CDXGISwapChain::Present+0x5` / `capture_hook_x64` / `sl_dlss_g` family, but now after `PostSL REACTIVATED` and warm-up-entry rather than before activation.
  - Ran `python build.py --incremental --skip-updates --run-tests`; all 564 tests passed and the build version bumped to `0.1.2277`.

- Pages touched: `current.md`, `frame-generation-switching.md`, `log.md`.
- Source files checked/modified: `installed/captureengine/logs/20260415_010931/session_manifest.txt`, `installed/captureengine/logs/20260415_010931/hook_debug.log`, `installed/captureengine/logs/20260415_010931/external_d249d67d-17d8-4c21-b872-6da225911223.dmp`, `hook/common/hook_common.h`, `hook/common/dxgi_shared.h`, `hook/common/dxgi_shared.cpp`, `hook/apis/dx12_hook.cpp`, `tests/test_dxgi_shared.cpp`, `tests/test_stubs.cpp`.
- Stale-risk note: Fresh runtime validation on `0.1.2277` is required. If the next active run still crashes after entering PostSL warm-up, the remaining issue is likely no longer the synthetic/bypass routing seam at all and will move deeper into what the warm-up callback itself is doing on that first unconfirmed post-activation path.

### 2026-04-15 - Preserve the consumed startup bootstrap latch when only the startup window expires

- **Motivation**: The fresh GTA V Enhanced validation `installed/captureengine/logs/20260415_005509` on build `0.1.2274` got much further than the earlier active failures. CE rendered the normal DX12 overlay stably for about 10 seconds, then later survived a real Streamline runtime-owned handoff with one normal-routed startup-handoff Present, repeated `Keeping decisive synthetic Streamline startup Present on the normal SL route ...` lines through at least `#100`, and a cached-swapchain ECL-side callback completion: `DX12: ECL hook PostSL callback completed (cachedSwapchain=%p)`. The crash still returned immediately after, but the failure boundary changed: the very next post-expiry Streamline-originated Present again logged `Treating Streamline-originated Present as synthetic re-entrant #1`. Comparing that log against the code showed a concrete state-reset bug: `ClearStreamlineStartupTransitionWindow()` was still clearing both the timer and `streamlineStartupTopLevelPresentConsumed`, so the expiry path erased the exact one-shot bootstrap proof that `0.1.2274` depended on.

- **Fix**:
  1. `hook/common/dxgi_shared.h` now narrows `ClearStreamlineStartupTransitionWindow()` so it clears only the timer/window itself and preserves `streamlineStartupTopLevelPresentConsumed` across the post-expiry half-armed startup phase.
  2. `hook/common/dxgi_shared.h` now adds `ResetStreamlineStartupTransitionState()` for the true full-reset paths that should clear both the startup window and the consumed bootstrap latch.
  3. `hook/apis/dx12_hook.cpp` now uses that new full-reset helper only in real teardown / completed-startup reset paths, while keeping the ECL-side expiry callback on the narrower window-clear behavior.
  4. `tests/test_dxgi_shared.cpp` extends `DXGISharedTest.ExtendingStartupTransitionWindowDoesNotResetConsumedTopLevelBootstrap` so the test now also covers the new distinction directly: clearing the window preserves the consumed latch, while a full startup-state reset clears it.

- **Why this is generic**: The one-shot startup bootstrap latch and the startup transition timer encode two different facts. Timer expiry only means the startup grace window ended; it does not prove CE has returned to a pre-bootstrap state. As long as PostSL startup is still pending, CE remains in the same half-armed startup family and still needs the knowledge that the top-level bootstrap already ran. Splitting those semantics removes an accidental coupling that was broader than the real state machine.

- **Verification**:
  - Re-checked `installed/captureengine/logs/20260415_005509/{session_manifest.txt,hook_debug.log,external_deecd195-6850-446e-93f5-ab10806dd8a8.dmp}`.
  - Analyzed `external_deecd195-6850-446e-93f5-ab10806dd8a8.dmp` with `cdb.exe`; it is still the same `dxgi!CDXGISwapChain::Present+0x5` / `capture_hook_x64` / `sl_dlss_g` family, but now after the ECL-side callback-complete marker instead of before it.
  - Ran `python build.py --incremental --skip-updates --run-tests`; all 564 tests passed and the build version bumped to `0.1.2275`.

- Pages touched: `current.md`, `frame-generation-switching.md`, `log.md`.
- Source files checked/modified: `installed/captureengine/logs/20260415_005509/session_manifest.txt`, `installed/captureengine/logs/20260415_005509/hook_debug.log`, `installed/captureengine/logs/20260415_005509/external_deecd195-6850-446e-93f5-ab10806dd8a8.dmp`, `hook/common/dxgi_shared.h`, `hook/apis/dx12_hook.cpp`, `hook/apis/streamline_hook.cpp`, `tests/test_dxgi_shared.cpp`.
- Stale-risk note: Fresh runtime validation on `0.1.2275` is required. If the next active run still crashes after the same cached-swapchain callback completion point, then the remaining issue is no longer the consumed-bootstrap state reset and likely moves into what the callback path actually changed (or failed to change) before the first post-expiry Streamline-originated Present arrives.

### 2026-04-15 - Simplify the post-expiry normal-route guard to the actual half-armed startup condition

- **Motivation**: The next GTA V Enhanced validation `installed/captureengine/logs/20260415_004935` on build `0.1.2273` still reproduced the same tail almost exactly. The run showed one normal-routed startup-handoff Present, many normal-routed synthetic startup Presents, the cached-swapchain ECL-expiry callback, and then immediately `DetourPresent: Treating Streamline-originated Present as synthetic re-entrant #1` followed by the same crash family. That proved the added `startupHandoffInProgress`-style boundary was still too tied to startup-window bookkeeping: the expiry callback clears those bits before the first failing post-expiry Present arrives.

- **Fix**:
  1. `hook/common/dxgi_shared.h` simplifies `ShouldKeepSyntheticStartupStreamlinePresentOnNormalRoute()` back down to the actual surviving half-armed startup condition from the logs: observer mode must be off, the one-shot bootstrap must already be consumed, the caller must be inside Streamline, PostSL startup must still be pending, and the call must otherwise classify as synthetic/bypass.
  2. `hook/common/dxgi_shared.cpp` now uses that simplified helper in both `DetourPresent` and `DetourPresent1`, removing the extra runtime bit that the expiry callback was already clearing before the first post-expiry Present arrived.
  3. `tests/test_dxgi_shared.cpp` updates `DXGISharedTest.WrapperBackedSyntheticStartupPresentCanStayOnNormalRouteInActiveMode` to match the simplified signature.

- **Why this is generic**: The runtime evidence now points to a cleaner invariant than the previous startup-window bookkeeping: if startup bootstrap was consumed but PostSL startup is still pending, CE is still in a half-armed startup family and Streamline-originated Presents should stay off the synthetic/bypass path. That invariant is simpler, closer to the real failure signal, and less vulnerable to internal bookkeeping being cleared slightly before the runtime is actually safe.

- **Verification**:
  - Re-checked `installed/captureengine/logs/20260415_004935/{session_manifest.txt,hook_debug.log,external_4429c3bb-be7d-4a0f-9d0b-e9accf502dbe.dmp}`.
  - Ran `python build.py --incremental --skip-updates --run-tests`; all 564 tests passed and the build version bumped to `0.1.2274`.

- Pages touched: `current.md`, `frame-generation-switching.md`, `log.md`.
- Source files checked/modified: `installed/captureengine/logs/20260415_004935/session_manifest.txt`, `installed/captureengine/logs/20260415_004935/hook_debug.log`, `hook/common/dxgi_shared.h`, `hook/common/dxgi_shared.cpp`, `tests/test_dxgi_shared.cpp`.
- Stale-risk note: Fresh runtime validation on `0.1.2274` is required. If the next active run still crashes with the same tail, the next suspicion moves past Present routing and back into what the cached-swapchain callback itself is or is not doing before the next callback arrives.

### 2026-04-15 - Keep post-expiry half-armed Streamline startup Presents off the synthetic/bypass path too

- **Motivation**: The new GTA V Enhanced validations `installed/captureengine/logs/20260415_003239` on build `0.1.2271` and `installed/captureengine/logs/20260415_004226` on build `0.1.2272` proved the previous fixes were directionally right but still one seam too narrow. In both runs CE got much further: the promoted startup-handoff Present stayed on the normal route, many synthetic startup Presents also stayed on the normal route, and the ECL-expiry callback finally fired with `cachedSwapchain=%p`. But the very next post-expiry Streamline-originated Present still dropped back to `Treating Streamline-originated Present as synthetic re-entrant #1`, then the same external crash family returned. Neither run logged `PostSL synthetic startup activation complete`, `PostSL REACTIVATED`, or `Post-SL overlay SUBMIT`, so PostSL startup was still half-armed at the failing callback.

- **Fix**:
  1. `hook/common/dxgi_shared.h` widens `ShouldKeepSyntheticStartupStreamlinePresentOnNormalRoute()` again: once the one-shot startup bootstrap has been consumed, Streamline-originated startup Presents stay off the synthetic/bypass path only while startup is still half-armed and the overall startup-handoff family is still in progress.
  2. `hook/common/dxgi_shared.cpp` now passes `streamlineStartupHandoffInProgress` into that helper in both `DetourPresent` and `DetourPresent1`, keeping the post-expiry half-armed family on the normal SL route as well.
  3. `tests/test_dxgi_shared.cpp` updates `DXGISharedTest.WrapperBackedSyntheticStartupPresentCanStayOnNormalRouteInActiveMode` to cover the widened helper signature.

- **Why this is generic**: The runtime evidence now shows that the dangerous boundary is not just "before the startup window expires" but "until PostSL startup has actually completed". Clearing the startup window alone is not enough proof that the first post-expiry Streamline-originated Present is safe to send down the synthetic/bypass path. The generic safe invariant is to keep those calls on the normal SL route until the half-armed startup family has genuinely completed.

- **Verification**:
  - Re-checked `installed/captureengine/logs/20260415_003239/hook_debug.log` and `installed/captureengine/logs/20260415_004226/{session_manifest.txt,hook_debug.log,external_9a2ebc48-3bae-413b-9313-359a9de1b775.dmp}`.
  - Ran `python build.py --incremental --skip-updates --run-tests`; all 564 tests passed and the build version bumped to `0.1.2273`.

- Pages touched: `current.md`, `frame-generation-switching.md`, `log.md`.
- Source files checked/modified: `installed/captureengine/logs/20260415_003239/hook_debug.log`, `installed/captureengine/logs/20260415_004226/session_manifest.txt`, `installed/captureengine/logs/20260415_004226/hook_debug.log`, `hook/common/dxgi_shared.h`, `hook/common/dxgi_shared.cpp`, `tests/test_dxgi_shared.cpp`.
- Stale-risk note: Fresh runtime validation on `0.1.2273` is required. If the next active run still crashes, re-check whether the first post-expiry Streamline-originated Present really stayed off the synthetic/bypass path and whether PostSL startup still fails to log `activation complete` / `REACTIVATED` afterward.

### 2026-04-15 - Keep the decisive synthetic Streamline startup Present on the normal route and fix the live cached-swapchain ECL-expiry trigger

- **Motivation**: The next active GTA V Enhanced validation `installed/captureengine/logs/20260415_002110` on build `0.1.2270` showed the previous fix worked but also moved the failure to the next seam. The old promoted-handoff lines were gone; instead the trace showed `DetourPresent: Keeping Streamline startup-handoff Present on the normal SL route #1`, authoritative wrapper ECL progress `#1..#10`, then `DX12: PostSL gated callback deferred until startup transition window expires`, then exactly one `DetourPresent: Treating Streamline-originated Present as synthetic re-entrant #1`, then the same external crash family (`external_05d9b5aa-9293-42fa-837e-2663a9f3826f.dmp`) and CE's own early wrapper-only startup-stall dump `GTA5_Enhanced.exe_FREEZE_2026-04-15_00-22-44_852.dmp`. That proved the remaining toxic path had moved to the first decisive synthetic Streamline startup Present. The same session also showed the ECL-expiry callback was still executing the stale `nullSwapchain=1` block, so the intended cached-swapchain activation trigger had not actually replaced the live code path yet.

- **Fix**:
  1. `hook/common/dxgi_shared.h` now exposes `ShouldKeepSyntheticStartupStreamlinePresentOnNormalRoute()`, which is true only for the active pure-DLSS startup family where the startup window is still active, the one-shot bootstrap has already been consumed, PostSL startup is still pending, and the current Streamline-originated call would otherwise take the synthetic/bypass route.
  2. `hook/common/dxgi_shared.cpp` now uses that helper in both `DetourPresent` and `DetourPresent1`. In that exact family CE logs `Keeping decisive synthetic Streamline startup Present on the normal SL route ...` and clears the synthetic/bypass classification so the call stays on the normal SL route instead of spending the only decisive startup callback on the old bypass family.
  3. `hook/apis/dx12_hook.cpp` now updates the still-live ECL-expiry callback block: it uses `GetLastTrackedSwapchainForStartupActivation()`, logs `cachedSwapchain=%p`, invokes the PostSL callback with that cached swapchain, and tracks `s_callbackTriggeredWithCachedSwapchain` instead of the stale null-swapchain state.
  4. `tests/test_dxgi_shared.cpp` now adds `DXGISharedTest.WrapperBackedSyntheticStartupPresentCanStayOnNormalRouteInActiveMode` to lock the new routing seam in place.

- **Why this is generic**: Once the observer probes and the prior active refinement both show that special startup Present routing is the remaining reproduced trigger, the next generic split is to stop treating the first decisive synthetic startup Present as a bypass-only family as well. The runtime already has stronger startup proof at that point: startup-policy is armed, the one-shot bootstrap has been consumed, and authoritative wrapper progress is visible. Spending that only callback on the synthetic/bypass path is too fragile for any similar pure-DLSS startup family.

- **Verification**:
  - Re-checked `installed/captureengine/logs/20260415_002110/{session_manifest.txt,hook_debug.log,external_05d9b5aa-9293-42fa-837e-2663a9f3826f.dmp,GTA5_Enhanced.exe_FREEZE_2026-04-15_00-22-44_852.dmp}` and compared it against `installed/captureengine/logs/20260414_234144/hook_debug.log`.
  - Ran `python build.py --incremental --skip-updates --run-tests`; all 564 tests passed and the build version bumped to `0.1.2271`.

- Pages touched: `current.md`, `frame-generation-switching.md`, `log.md`.
- Source files checked/modified: `installed/captureengine/logs/20260415_002110/session_manifest.txt`, `installed/captureengine/logs/20260415_002110/hook_debug.log`, `hook/common/dxgi_shared.h`, `hook/common/dxgi_shared.cpp`, `hook/apis/dx12_hook.cpp`, `tests/test_dxgi_shared.cpp`.
- Stale-risk note: Fresh runtime validation on `0.1.2271` is required. If the next active GTA run still crashes, re-check whether the decisive synthetic startup Present really stayed off the bypass path and whether the ECL-expiry callback now logs `cachedSwapchain=%p` instead of `nullSwapchain=1`.

### 2026-04-15 - Remove active-mode top-level startup-handoff Present promotion and use cached swapchain for ECL startup activation

- **Motivation**: The fresh active GTA V Enhanced validation `installed/captureengine/logs/20260414_234144` on build `0.1.2269` reproduced the crash immediately after returning from the stable observer probes. The staged comparison made the remaining trigger much clearer: active mode logged `DX12: Streamline FG ON — installed gated PostSL callback`, `DX12: Streamline FG ON — pre-armed PostSL callback for startup routing`, then exactly one `Treating Streamline startup-handoff Present as top-level live Present #1` and `Promoted Streamline startup-handoff Present using bypass return path #1`. After that CE never reached `PostSL synthetic startup activation complete` or any `Post-SL overlay SUBMIT`; instead Streamline started writing repeated external dumps under `C:\ProgramData\NVIDIA\Streamline\GTA5_Enhanced\...\sl-sha-11cf43f.dmp`, CE mirrored them into the session, and `Present STALLED` appeared. The stable `observer_startup_present_only` run `20260414_232934` on the same code line had already shown that startup-policy mutation plus wrapper-queue ECL progress are stable when that one promoted Streamline startup-handoff Present is removed.

- **Fix**:
  1. `hook/common/dxgi_shared.cpp` no longer promotes the late large-gap Streamline startup-handoff Present to the top-level/bypass family in active mode. The one-shot `streamlineStartupTopLevelPresentConsumed` latch is still consumed so later wrapper-progress bookkeeping remains coherent, but the call itself now stays on the normal SL route.
  2. `hook/common/dxgi_shared.cpp` now logs the new intent explicitly in both `DetourPresent` and `DetourPresent1`: `Keeping Streamline startup-handoff Present on the normal SL route ... — top-level promotion disabled; relying on startup-policy + wrapper-progress activation`.
  3. `hook/common/dxgi_shared.h` drops the now-unused helper that preferred the bypass/original return path for the promoted top-level startup-handoff Present.
  4. `hook/apis/dx12_hook.cpp` now adds `GetLastTrackedSwapchainForStartupActivation()` so the ECL-side startup-window-expiry trigger can invoke the PostSL callback with the last tracked swapchain when that pointer still looks valid, instead of always forcing a null-swapchain callback that can only log and return.
  5. `tests/test_dxgi_shared.cpp` removes the obsolete promoted-startup-bypass helper test; the active/observer routing semantics now rely on the direct `ShouldTreatStreamlinePresentAsSyntheticReentrant()` and observer-split coverage instead.

- **Why this is generic**: The observer bisect already proved the one promoted Streamline startup-handoff Present is the remaining toxic seam in this startup family. Keeping that one call on CE's special top-level/bypass path is therefore too broad for active mode as well. The generic next step is to rely on the startup-policy window plus later wrapper-progress/ECL-backed activation machinery instead of forcing one special top-level Present through a path that repeatedly destabilizes Streamline before PostSL can even activate.

- **Verification**:
  - Re-checked `installed/captureengine/logs/20260414_234144/{session_manifest.txt,hook_debug.log,external_sl-sha-11cf43f.dmp}` and compared the trace against the stable observer-startup-present-only run `installed/captureengine/logs/20260414_232934/hook_debug.log`.
  - Ran `python build.py --incremental --skip-updates --run-tests`; all 563 tests passed and the build version bumped to `0.1.2270`.

- Pages touched: `current.md`, `frame-generation-switching.md`, `log.md`.
- Source files checked/modified: `installed/captureengine/logs/20260414_234144/session_manifest.txt`, `installed/captureengine/logs/20260414_234144/hook_debug.log`, `installed/captureengine/logs/20260414_232934/hook_debug.log`, `hook/common/dxgi_shared.h`, `hook/common/dxgi_shared.cpp`, `hook/apis/dx12_hook.cpp`, `tests/test_dxgi_shared.cpp`.
- Stale-risk note: Fresh runtime validation on `0.1.2270` is required. If the next active GTA run still crashes, re-check whether the startup-handoff Present is truly staying off the top-level/bypass path and whether the ECL-side activation trigger now reaches `PostSLOverlayRenderGated()` with a valid cached swapchain.

### 2026-04-14 - Validate narrowed observer-startup-present-only seam on build 0.1.2269

- **Motivation**: After narrowing `observer_startup_present_only` so all special Streamline Present routing stayed passive in observer mode, we needed a fresh GTA V Enhanced validation to confirm whether the remaining non-Streamline startup-Present probe pieces could still reproduce the crash family. The new session `installed/captureengine/logs/20260414_232934` on build `0.1.2269` provided that validation.

- **Result**:
  1. `installed/captureengine/logs/20260414_232934/session_manifest.txt` confirms the intended staged config was active: `overlay_observer_only=1`, `overlay_observer_policy_only=1`, and `overlay_observer_startup_present_only=1` on build `0.1.2269`.
  2. `hook_debug.log` still shows the expected startup-policy behavior: `Streamline Hook: FG state transition OFF->ON via SetOptions`, startup-window OFF suppression/extension, wrapper-queue `ECL captured SL wrapper queue ...` progress, and a later clean `ON->OFF via GetState` transition.
  3. The run stayed stable: no `Present STALLED`, no `CrashMirror:` line, and no dump artifact in the session folder.
  4. The log also shows what disappeared relative to the failing `0.1.2268` runs: no `Treating Streamline-originated Present as synthetic re-entrant`, no promoted startup-handoff Present, and no PostSL registration/reactivation/submits.
  5. `FFX Hook: No FFX modules found, hooks not installed` and the later module-enumeration diagnostic confirm the remaining non-Streamline startup bypass seam did not actually fire in this GTA run. That means the stable result specifically validates that removing special Streamline Present routing from the observer seam eliminated the reproduced crash family.

- **Why this matters**: The observer bisect now has a clean result. GTA's reproduced crash requires special Streamline Present routing during startup; startup-policy mutation by itself is stable, and the narrowed observer-startup-present-only seam is also stable when it preserves only the non-Streamline startup bypass family. The remaining gap has shifted back to active mode, where CE still restores pre-armed PostSL and special Streamline Present routing during startup.

- **Verification**:
  - Re-checked `installed/captureengine/logs/20260414_232934/{session_manifest.txt,hook_debug.log}`.
  - Compared the stable run against the prior stable policy-only reference `installed/captureengine/logs/20260413_215253/hook_debug.log` and the failing `0.1.2268` run `installed/captureengine/logs/20260414_231024/hook_debug.log`.

- Pages touched: `current.md`, `frame-generation-switching.md`, `log.md`.
- Source files checked/modified: `installed/captureengine/logs/20260414_232934/session_manifest.txt`, `installed/captureengine/logs/20260414_232934/hook_debug.log`, `installed/captureengine/logs/20260413_215253/hook_debug.log`, `installed/captureengine/logs/20260414_231024/hook_debug.log`.
- Stale-risk note: The next validation target is active mode. If a future active GTA run still crashes, re-check the pre-armed PostSL plus special Streamline Present routing family first; the observer probes now show that family, not the passive startup-policy path, is the remaining reproduced trigger.

### 2026-04-14 - Keep all special Streamline Present routing passive in observer-startup-present-only mode

- **Motivation**: The newer GTA V Enhanced staged validation `installed/captureengine/logs/20260414_231024` on build `0.1.2268` still crashed in `observer_startup_present_only` with the same family as the earlier runs: `Streamline Hook: FG state transition OFF->ON via SetOptions`, exactly one `DetourPresent: Treating Streamline-originated Present as synthetic re-entrant #1`, then a mirrored external dump `external_4ac1bf1f-bc60-4a0e-8915-56e6e40779a5.dmp` landing in the same `dxgi!CDXGISwapChain::Present+0x5` / `sl_dlss_g` breakpoint-corruption bucket. Comparing that run against the stable `observer_policy_only` reference `installed/captureengine/logs/20260413_215253` showed the staged seam was still broader than intended: `observer_startup_present_only` was still re-enabling special Streamline-originated Present routing, not just the remaining non-Streamline startup bypass probe.

- **Fix**:
  1. `hook/common/dxgi_shared.h` now exposes `ShouldAllowSpecialStreamlinePresentRouting()`, making the current staged split explicit: special Streamline synthetic/top-level Present routing is active only outside observer modes.
  2. `hook/common/dxgi_shared.cpp` now uses that helper in both `DetourPresent` and `DetourPresent1`, so observer-startup-present-only no longer re-enables the synthetic Streamline-originated Present bypass path. The only remaining staged startup-Present probe in observer mode is the non-Streamline family such as the FFX startup bypass.
  3. The old observer-only promoted-handoff skip helper and its dead branches were removed because the staged seam no longer allows promoted Streamline startup-handoff Presents at all.
  4. `tests/test_dxgi_shared.cpp` now covers the corrected split with `DXGISharedTest.ObserverModesKeepSpecialStreamlinePresentRoutingPassive`, while the existing FFX startup-bypass coverage remains.
  5. `common/config.cpp` and `hook/apis/dx12_hook.cpp` now describe/log `observer_startup_present_only` more precisely as keeping PostSL and special Streamline Present routing passive while preserving only the startup-policy plus non-Streamline startup-Present probe state.

- **Why this is generic**: The staged observer seam exists to isolate startup families cleanly, not to keep partially-proven risky routing alive. Once repeated GTA validations show that even the one synthetic Streamline-originated Present is enough to reintroduce the same crash family, the correct generic next split is to take all special Streamline Present routing back out of the observer seam and leave only the non-Streamline startup bypass family under test.

- **Verification**:
  - Re-checked `installed/captureengine/logs/20260413_215253/hook_debug.log`, `installed/captureengine/logs/20260413_235512/hook_debug.log`, and `installed/captureengine/logs/20260414_231024/{session_manifest.txt,hook_debug.log,external_4ac1bf1f-bc60-4a0e-8915-56e6e40779a5.dmp}`.
  - Analyzed `installed/captureengine/logs/20260414_231024/external_4ac1bf1f-bc60-4a0e-8915-56e6e40779a5.dmp` with `cdb.exe`; it still reports `MEMORY_CORRUPTION_LARGE_80000003_memory_corruption!dxgi.dll` at `dxgi!CDXGISwapChain::Present+0x5` via `capture_hook_x64` and `sl_dlss_g`.
  - Ran `python build.py --incremental --skip-updates --run-tests`; all 564 tests passed and the build version bumped to `0.1.2269`.

- Pages touched: `current.md`, `frame-generation-switching.md`, `log.md`.
- Source files checked/modified: `installed/captureengine/logs/20260413_215253/hook_debug.log`, `installed/captureengine/logs/20260414_231024/session_manifest.txt`, `installed/captureengine/logs/20260414_231024/hook_debug.log`, `installed/captureengine/logs/20260414_231024/external_4ac1bf1f-bc60-4a0e-8915-56e6e40779a5.dmp`, `hook/common/dxgi_shared.h`, `hook/common/dxgi_shared.cpp`, `tests/test_dxgi_shared.cpp`, `common/config.cpp`, `hook/apis/dx12_hook.cpp`.
- Stale-risk note: Fresh runtime validation on `0.1.2269` is required. If the next observer-startup-present-only run still freezes, the remaining suspect is no longer any special Streamline Present routing family; re-check the non-Streamline startup bypass path first.

### 2026-04-13 - Mirror externally handled crash dumps into the active CE session folder

- **Motivation**: The narrowed observer-startup-present-only runtime validation in `installed/captureengine/logs/20260413_231005` changed the failure family from a watchdog-captured freeze to a fast crash with no CE session-local `.dmp`, `crash.log`, or `crash_error.txt`. Investigation showed CE's VEH/UEF handler can miss crash paths that are handled internally by the runtime/game, while Rockstar was in fact writing an external dump under `C:\Users\TestUser\AppData\Local\Rockstar Games\GTAV Enhanced\CrashLogs\`. The newest external artifact, `51e9b489-70bb-4998-a4ec-254bdd858cbd.dmp` at `2026-04-13 23:11:47.519`, still lands in the same family: `sl_dlss_g` on a worker thread calling through `capture_hook_x64` into `dxgi!CDXGISwapChain::Present`, followed by the game's own unhandled-exception path. That confirmed the crash did not disappear; only CE's session-local dump capture path was being bypassed.

- **Fix**:
  1. `hook/main.cpp` now installs a generic inline hook on `dbghelp.dll!MiniDumpWriteDump` when `dbghelp.dll` is already loaded at inject time or when it loads later through the normal module-notification path.
  2. When the current process successfully writes a dump outside the active CE session directory, the hook now re-emits the same dump contents into the CE session folder as `external_<original-name>.dmp` by calling the original `MiniDumpWriteDump` trampoline on a CE-owned file handle. This avoids racing the original writer over file-copy semantics and keeps the hook generic instead of relying on Rockstar-specific paths.
  3. `common/crash_handler.h` / `.cpp` now expose `GetCrashDumpDirectory()` so the injected hook can safely resolve the current session dump directory without duplicating that state.
  4. `common/crash_dump_policy.h` now centralizes the small policy helpers for external dump mirroring: session-local path exclusion and stable mirrored dump naming.
  5. `tests/test_crash_dump_policy.cpp` now covers the new mirroring policy helpers.

- **Why this is generic**: The problem is not specific to GTA or Rockstar. Any runtime, middleware layer, or game can catch an internal failure and call `MiniDumpWriteDump` itself, bypassing CE's VEH/watchdog path. Mirroring successful in-process external dump writes back into the active CE session folder preserves CE's session bundle diagnostics without depending on who handled the exception.

- **Verification**:
  - Analyzed `C:\Users\TestUser\AppData\Local\Rockstar Games\GTAV Enhanced\CrashLogs\51e9b489-70bb-4998-a4ec-254bdd858cbd.dmp` with `analysis/analyze_dump.ps1`; the dump still points into the same `sl_dlss_g` / `capture_hook_x64` / `dxgi!CDXGISwapChain::Present` crash family.
  - Ran `& ".\tests\unit_tests.exe" --gtest_filter=CrashDumpPolicyTest.*` after the change; all 5 crash-dump policy tests passed.
  - Ran `python build.py --incremental --skip-updates --run-tests`; all 564 tests passed.

- Pages touched: `current.md`, `regression-testing-and-logging.md`, `log.md`.
- Source files checked/modified: `C:\Users\TestUser\AppData\Local\Rockstar Games\GTAV Enhanced\CrashLogs\51e9b489-70bb-4998-a4ec-254bdd858cbd.dmp`, `C:\Users\TestUser\AppData\Local\Rockstar Games\GTAV Enhanced\CrashLogs\crashcontext.log`, `hook/main.cpp`, `common/crash_handler.h`, `common/crash_handler.cpp`, `common/crash_dump_policy.h`, `tests/test_crash_dump_policy.cpp`.
- Stale-risk note: Fresh runtime validation is still required. Re-check this path if a future fast-crash run still produces only an external dump, if no `CrashMirror:` log lines appear after `dbghelp.dll` loads, or if another component starts writing dumps out-of-process instead of via in-process `MiniDumpWriteDump`.

### 2026-04-13 - Narrow observer-startup-present-only again after refined handoff probe still froze

- **Motivation**: The refined runtime validation in `installed/captureengine/logs/20260413_224823` still froze even though the staged observer probe now logged `Observer-startup-present-only probe skipping full DX12 promoted-handoff processing (HandleDX12ProcessFrame + WaitForOverlayCompletion)`. The hook trace still showed no PostSL registration/reactivation/submits, but it did show exactly one promoted Streamline startup-handoff Present followed by wrapper-only `ECL captured SL wrapper queue ...` progress and then `Present STALLED`. The new dump again landed in the same `sl_dlss_g` / `sl.interposer` threading-exception family. That proves the one promoted Streamline-originated startup-handoff Present is still toxic by itself in observer mode, even after removing CE's top-level DX12 frame-processing work from that call.

- **Fix**:
  1. `hook/common/dxgi_shared.cpp` no longer lets observer mode promote a Streamline-originated startup-handoff Present to the top-level live Present path. In observer mode, that would-be handoff Present stays on the synthetic/bypass route again.
  2. `hook/common/dxgi_shared.h` drops the old observer-only helper that explicitly re-enabled that top-level handoff promotion path, because the staged seam has been narrowed back away from that call entirely.
  3. `common/shared_defs.h`, `common/config.cpp`, and `hook/apis/dx12_hook.cpp` now describe `observer_startup_present_only` more precisely as preserving startup-policy plus the remaining non-Streamline startup-Present probe pieces, not the Streamline-originated promoted top-level handoff itself.
  4. `tests/test_dxgi_shared.cpp` removes the old observer-mode unit that expected a top-level startup-handoff promotion in this staged seam; the FFX startup-bypass coverage remains, since that is still part of the narrowed probe.
  5. `llm-wiki/current.md`, `llm-wiki/frame-generation-switching.md`, `llm-wiki/regression-testing-and-logging.md`, and this log now record the new runtime evidence from `20260413_224823` and the narrower staged meaning.

- **Why this is generic**: The staged observer seams are a diagnostic ladder, not a commitment that every active-mode bootstrap behavior must stay re-enabled in observer mode. Once runtime evidence shows that a single promoted Streamline-originated startup-handoff Present is already enough to reintroduce the freeze family even with PostSL and all top-level DX12 work removed, the correct generic next split is to take that call back out of the observer seam and keep only the remaining startup-Present pieces that have not yet been ruled out.

- **Verification**:
  - Re-checked `installed/captureengine/logs/20260413_224823/session_manifest.txt`, `inject.log`, `hook_debug.log`, `captureengine.log`, and `cdb_analysis.log`.
  - Confirmed the refined run still logged the new skip marker but froze with the same `APPLICATION_FAULT_e0000001_sl.interposer.dll!Unknown` dump family.

- Pages touched: `current.md`, `frame-generation-switching.md`, `regression-testing-and-logging.md`, `log.md`.
- Source files checked/modified: `installed/captureengine/logs/20260413_224823/session_manifest.txt`, `installed/captureengine/logs/20260413_224823/inject.log`, `installed/captureengine/logs/20260413_224823/hook_debug.log`, `installed/captureengine/logs/20260413_224823/captureengine.log`, `installed/captureengine/logs/20260413_224823/cdb_analysis.log`, `hook/common/dxgi_shared.h`, `hook/common/dxgi_shared.cpp`, `common/shared_defs.h`, `common/config.cpp`, `hook/apis/dx12_hook.cpp`, `tests/test_dxgi_shared.cpp`.
- Stale-risk note: Fresh runtime validation on the newest build is required. If the next observer-startup-present-only run still freezes, the remaining suspect is no longer CE's promoted top-level Streamline startup-handoff Present path, because observer mode now keeps that exact call synthetic again.

### 2026-04-13 - Refine observer-startup-present-only probe after GTA freeze reproduction

- **Motivation**: The first staged `Overlay.observer_startup_present_only=true` runtime validation in `installed/captureengine/logs/20260413_222933` reproduced the GTA V Enhanced DLSS FG freeze even though PostSL never reactivated. The hook log showed exactly one promoted startup-handoff Present, no early/PostSL callback install or use, no `Post-SL overlay SUBMIT`, then `Present STALLED`; the dump again landed in the same `sl_dlss_g` / `sl.interposer` threading-exception family. That proved the startup-handoff Present / top-level bootstrap path itself was still too broad for this staged seam.

- **Fix**:
  1. `hook/common/dxgi_shared.h` now makes the next refinement explicit through `ShouldSkipDX12ProcessFrameForObserverStartupPresentProbe()`: while `observer_startup_present_only` is active, the one promoted Streamline startup-handoff Present still re-establishes the top-level DXGI routing/bootstrap decision, but CE skips the full promoted-handoff DX12 processing path on that one call.
  2. `hook/common/dxgi_shared.cpp` now applies that refinement in both `DetourPresent` and `DetourPresent1`: the promoted startup-handoff Present still uses the staged top-level routing decision and the bypass/original DXGI return path, but it skips both `HandleDX12ProcessFrame()` and `DX12_WaitForOverlayCompletion()` on that one promoted Present while the staged probe is active.
  3. `tests/test_dxgi_shared.cpp` now covers the refined seam with `DXGISharedTest.ObserverStartupPresentOnlySkipsFullDX12ProcessFrameOnPromotedHandoffPresent`.
  4. `llm-wiki/current.md` and `llm-wiki/frame-generation-switching.md` now describe the refined seam precisely as skipping both `HandleDX12ProcessFrame()` and `DX12_WaitForOverlayCompletion()` on the promoted startup-handoff Present.

- **Why this is generic**: The staged observer seams are a reusable way to bisect startup instability without jumping straight from passive mode back to full active CE behavior. This refinement asks a narrower generic question than the earlier seam: is the crash triggered by the startup-handoff routing/bootstrap decision itself, or by the heavier top-level DX12 frame-processing work that CE previously performed on that one promoted Present? That is a generic diagnostic split for Streamline DLSS FG startup issues, not a GTA-specific workaround.

- **Verification**:
  - Re-checked the failing runtime evidence in `installed/captureengine/logs/20260413_222933/session_manifest.txt`, `inject.log`, `captureengine.log`, `hook_debug.log`, and `cdb_analysis.log`.
  - Ran `python build.py --incremental --skip-updates --run-tests` after the refinement.
  - All 563 tests passed.

- Pages touched: `current.md`, `frame-generation-switching.md`, `log.md`.
- Source files checked/modified: `installed/captureengine/logs/20260413_222933/session_manifest.txt`, `installed/captureengine/logs/20260413_222933/inject.log`, `installed/captureengine/logs/20260413_222933/captureengine.log`, `installed/captureengine/logs/20260413_222933/hook_debug.log`, `installed/captureengine/logs/20260413_222933/cdb_analysis.log`, `hook/common/dxgi_shared.h`, `hook/common/dxgi_shared.cpp`, `tests/test_dxgi_shared.cpp`.
- Stale-risk note: Fresh runtime validation on the latest build is still required. Re-check this seam first if the next trace still freezes, or if the log shows the promoted startup-handoff Present reappearing without the new `skipping full DX12 promoted-handoff processing` marker while `observer_startup_present_only` is active.

### 2026-04-13 - Add staged observer-startup-present-only DXGI startup-routing probe

- **Motivation**: The staged `Overlay.observer_policy_only=true` seam re-enabled Streamline startup-policy mutation while keeping DX12/PostSL/startup-Present behavior passive. The next bisect seam is narrower than full active mode: re-enable only the special DXGI startup Present routing family while PostSL callback install/use and rendering still stay dormant. That lets us test whether GTA V Enhanced's remaining DLSS FG freeze family lives in startup Present routing itself or only in the later PostSL callback/rendering path.

- **Fix**:
  1. `common/shared_defs.h`, `common/config.cpp`, `common/config.h`, `hook/common/hook_common.h`, `captureengine/main.cpp`, and `captureengine/inject_main.cpp` now expose `Overlay.observer_startup_present_only=true` as a new staged probe. It is only meaningful with `observer_only=true` and `observer_policy_only=true`, bumps the shared-memory version to 25, is parsed from config/overrides, is visible in the shared-memory/inject summary log, and is recorded in `session_manifest.txt` as `overlay_observer_startup_present_only`.
  2. `hook/common/dxgi_shared.h` now makes the new staged split explicit and testable: pure observer-only still suppresses special startup Present routing, while observer-startup-present-only can restore only the DXGI startup-routing family.
  3. `hook/common/dxgi_shared.cpp` now re-enables only the special startup Present routing family in that staged mode: the one-shot promoted Streamline startup-handoff Present can re-establish the top-level Present path again, and the FFX startup Present bypass can run again during the Streamline startup handoff window. The PostSL callback pointer remains null in observer modes, so the synthetic/re-entrant path still stays callback-dormant.
  4. `hook/apis/dx12_hook.cpp` still keeps observer modes passive for DX12/PostSL state management, but now logs the narrower observer-startup-present-only mode distinctly so traces show when startup Present routing is being probed while PostSL remains suppressed.
  5. Tests now cover the new split in `tests/test_config.cpp`, `tests/test_config_override.cpp`, `tests/test_shared_runtime_state.cpp`, `tests/test_streamline_runtime_policy.cpp`, and `tests/test_dxgi_shared.cpp`.

- **Why this is generic**: We still need to isolate the remaining active families without jumping straight from passive policy probing back to full active CE behavior. `observer_startup_present_only` lets us ask a precise generic question: does special startup Present routing alone destabilize the runtime even while PostSL callback install/use and rendering remain absent? That is a reusable staged-debug seam for future Streamline DLSS FG startup issues, not a GTA-specific hack.

- **Verification**:
  - Ran focused unit tests for config/shared-state and DXGI staged-routing seams.
  - Ran `python build.py --incremental --skip-updates --run-tests`.
  - All 562 tests passed.

- Pages touched: `current.md`, `frame-generation-switching.md`, `regression-testing-and-logging.md`, `log.md`.
- Source files checked/modified: `common/shared_defs.h`, `common/config.cpp`, `common/config.h`, `captureengine/main.cpp`, `captureengine/inject_main.cpp`, `hook/common/hook_common.h`, `hook/common/dxgi_shared.h`, `hook/common/dxgi_shared.cpp`, `hook/apis/dx12_hook.cpp`, `tests/test_config.cpp`, `tests/test_config_override.cpp`, `tests/test_shared_runtime_state.cpp`, `tests/test_streamline_runtime_policy.cpp`, `tests/test_dxgi_shared.cpp`.
- Stale-risk note: Fresh runtime validation with `[Overlay] observer_only=true`, `[Overlay] observer_policy_only=true`, and `[Overlay] observer_startup_present_only=true` is now needed. Re-check this staged seam first if logs unexpectedly show early/PostSL callback install/use, PostSL rendering/submits, or broad Streamline Present routing instead of only the one-shot startup-handoff / startup-bypass family while the new flag is active.

