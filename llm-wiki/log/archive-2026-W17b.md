# llm-wiki Log — Archive 2026-W17b

### 2026-04-21 - Keep post-FSR explicit DLSS comebacks startup-protected through the full intended eight confirmed PostSL frames so GTA cannot fall back to no-FG immediately after submit #8

- **Motivation**: GTA V Enhanced `installed/captureengine/logs/20260421_144254_gtanocrash_allfgofftofsrfgontodlsesfgfail` on build `0.1.2501` no longer hit the older stale-OFF-at-expiry seam. The explicit post-FSR DLSS comeback on fresh `scQueue=000002538D784660` now reaches `PostSL REACTIVATED`, `PostSL CONFIRMED rendering`, updates `lastWorkingQueue`, and submits visible PostSL overlay frames. But GTA still silently loses DLSS FG afterward and falls back to `STREAMLINE_NO_FG` with no crash.

- **Comparison that narrowed the seam**:
  1. GTA and Talos both recover far enough to reach `Post-SL overlay SUBMIT #1` on the preserved post-FSR `scQueue`.
  2. Talos `installed/captureengine/logs/20260420_215301_talosnocrash_multipleswitching` keeps going cleanly past `Post-SL overlay SUBMIT #8` into sustained PostSL traffic.
  3. GTA instead reaches `Post-SL overlay SUBMIT #8`, then CE immediately logs the stale-OFF drop and the next stale `GetState` OFF churn drives `DX12_OnStreamlineFGStateChanged(false)` plus `Streamline Hook: FG state transition ON->OFF via GetState`.

- **Root cause refinement**:
  1. The earlier confirmed-startup-settling protection was one frame too short at the exact boundary. `ShouldTreatConfirmedPostSLRenderingAsStartupSettling(...)` used `< 8`, so the eighth confirmed frame was already treated as fully settled.
  2. The GTA `0.1.2501` trace proves stale OFF churn can survive all the way to that boundary. Once protection ended at submit `#8`, the buffered stale OFF was dropped and the next stale `GetState` OFF churn immediately collapsed the still-live comeback into `STREAMLINE_NO_FG`.
  3. Talos shows the target behavior for the same broad family: the recovered post-FSR DLSS comeback continues past submit `#8` rather than collapsing exactly at the settling-window boundary.

- **Fix**:
  1. `hook/common/dx12_overlay_policy.h` now keeps `ShouldTreatConfirmedPostSLRenderingAsStartupSettling(...)` true for `stablePostSLFrameCount <= 8`, so the ninth confirmed PostSL frame becomes the first fully settled one.
  2. `hook/apis/streamline_hook.cpp` now expands the stale-OFF drop diagnostic to include `confirmed=%d settling=%d`, so traces reveal whether CE dropped buffered OFF churn only after the comeback was truly settled.
  3. `tests/test_dxgi_shared.cpp` now renames and extends the boundary test to `DXGISharedTest.ConfirmedPostSLStartupRoutingProtectsThroughFirstEightFrames`.

- **Why this is generic**: This is not a GTA-only workaround. The generic invariant is: if CE's shared confirmed-startup-settling policy says the first eight confirmed PostSL frames are still startup-protected, stale OFF churn must not be allowed to survive until the exact boundary after submit `#8` and then collapse the same recovered post-FSR DLSS comeback on the very next runtime poll.

- **Verification**:
  - Re-checked GTA `installed/captureengine/logs/20260421_144254_gtanocrash_allfgofftofsrfgontodlsesfgfail/{hook_debug.log,session_manifest.txt}` and confirmed the exact boundary: `Post-SL overlay SUBMIT #8`, then stale-OFF drop, then `Streamline FG OFF` / `ON->OFF via GetState`.
  - Re-checked Talos `installed/captureengine/logs/20260420_215301_talosnocrash_multipleswitching/hook_debug.log` and confirmed it continues cleanly past `Post-SL overlay SUBMIT #8` into long-running PostSL traffic.
  - Ran `python build.py --run-tests --tests-only --skip-updates --gtest-filter="DXGISharedTest.ConfirmedPostSLStartupRoutingProtectsThroughFirstEightFrames:StreamlineRuntimePolicyTest.StartupProtectedPostFSRComebackKeepsOffChurnDeferredWhileHalfArmed:StreamlineRuntimePolicyTest.StartupProtectedPostFSRComebackDropsStaleSuppressedOffChurnOnceActive:StreamlineRuntimePolicyTest.CombinedRuntimeStateDefersHalfArmedStartupProtectedPostFSRComebackOffAfterWindowExpiry"`; the focused suite passed.
  - Ran `python build.py --skip-updates`; full rebuild passed and `build/verification/latest_summary.txt` reports success for build `0.1.2503`.

- **Files changed**: `hook/common/dx12_overlay_policy.h`, `hook/apis/streamline_hook.cpp`, `tests/test_dxgi_shared.cpp`, `llm-wiki/current.md`, `llm-wiki/frame-generation-switching.md`, `llm-wiki/regression-testing-and-logging.md`, `llm-wiki/log.md`

- **Stale risk**: Fresh runtime validation is still required. The next GTA check on build `0.1.2503+` is that the post-FSR explicit DLSS comeback on `scQueue=000002538D784660` stays alive past `Post-SL overlay SUBMIT #8` and does not immediately log `FG state transition ON->OFF via GetState`. Talos should continue to sustain PostSL submits across the same boundary.

### 2026-04-21 - Keep stale OFF churn deferred for safe-bootstrap post-FSR GetState comebacks too, and invalidate older PostSL last-working queue proof when a fresh post-FSR Streamline handoff moves to a different queue

- **Motivation**: GTA V Enhanced `installed/captureengine/logs/20260421_021720_gtacrash_fsgrtodlssfgtoallfgofftodlssfg` on build `0.1.2499` still crashed at the end of the switching sequence `FSR_FG -> DLSS_FG -> all FG off -> DLSS_FG`, even after the earlier explicit-comeback stale-OFF and stale-native-ownership fixes from `0.1.2499-pre`. The new dump family is not the old native-FSR `ffxQuery` deadlock. The hook log ends with `DX12: Reinit SUBMIT #1 ... devRemoved=0x887A002B`, and both dumps only show GTA's own `ERR_GFX_STATE` dialog/breakpoint path afterward.

- **Comparison that narrowed the seam**:
  1. The earlier explicit post-FSR DLSS comeback on the same GTA session still succeeds on `scQueue=0000021591765620`, confirms rendering, and updates `lastWorkingQ=0000021591765620`.
  2. The final failing `all FG off -> DLSS FG` enable is a later distinct startup family on a fresh authoritative Streamline handoff queue `00000216DEBBA880`, not reuse of the older queue. CE captures the new `scQueue`, initializes the overlay backend on it, and reaches the half-armed startup state (`pending=1 unconfirmed=1`) on that fresh queue.
  3. At startup-window expiry CE already considers that later comeback safe enough to activate via the ECL-expiry helper: `DX12: ECL hook detected startup transition window expiry ...`, then `DX12: PostSL REACTIVATED (epoch=3 ...)` on the new queue.
  4. But the very next line still replays the old suppressed `slDLSSGSetOptions(OFF)` (`Streamline Hook: Forwarding suppressed slDLSSGSetOptions(OFF) via GetState — startup window expired`), which tears the new comeback back down before first confirmed render, releases `scQueue=00000216DEBBA880`, and later lets non-FG recovery reinitialize on the older prior-epoch `lastWorkingQ=0000021591765620`.

- **Root cause refinement**:
  1. The stale-OFF rule was still too narrow semantically even after the earlier startup-settling extension. It only treated explicit `SetOptions(ON)` post-FSR comebacks as startup-protected past literal startup-window expiry.
  2. GTA's final failing sequence proves there is a second generic family: a post-FSR `GetState` comeback on a fresh authoritative Streamline handoff can still reach the shared `safePostFSRBootstrapPath=1` proof and therefore legitimately activate PostSL via the existing expiry-driven ECL helper, yet the stale-OFF replay path still did not trust that same proof and immediately forwarded the stale OFF.
  3. The same trace also exposed a queue-proof drift amplifier. After a newer authoritative Streamline handoff had already moved to `scQueue=00000216DEBBA880`, the older successfully confirmed queue proof from the previous DLSS epoch (`lastWorkingQ=0000021591765620`) was still left intact. Once the newer comeback tore down before first confirmation, the later post-FSR inactive recovery could therefore fall back onto stale proof from the older epoch and hit `DEVICE_REMOVED` on the first reinit submit.

- **Fix**:
  1. `hook/common/streamline_runtime_policy.h` now renames the stale-OFF helpers from the older explicit-only wording to the stronger startup-protected wording: `ShouldKeepOffChurnDeferredForStartupProtectedPostFSRComeback(...)` and `ShouldDropSuppressedOffChurnForStartupProtectedPostFSRComeback(...)`.
  2. Those helpers now accept `safePostFSRBootstrapPath`, and `hook/apis/streamline_hook.cpp` now threads `HookHasSafePostFSRBootstrapPath()` through all four deferred-OFF seams (`ApplyCombinedStreamlineRuntimeState`, `Hooked_slDLSSGGetState`, `Hooked_slDLSSGSetOptions`, and `FlushSuppressedSetOptionsOffIfNeeded()`). The logs now print both `explicit=%d` and `safeBootstrap=%d` so traces reveal which proof source kept the comeback startup-protected.
  3. `hook/common/dx12_overlay_policy.h` now adds `ShouldInvalidatePostSLLastWorkingQueueOnFreshPostFSRStreamlineHandoff(...)`, and `hook/apis/dx12_hook.cpp` now clears `g_PostSLLastWorkingQueue` when a fresh post-FSR authoritative Streamline handoff has already moved to a different non-origGame `scQueue`. That prevents older DLSS-epoch queue proof from surviving a newer authoritative Streamline handoff.
  4. `tests/test_streamline_runtime_policy.cpp` now covers the safe-bootstrap `GetState` comeback family in the renamed startup-protected helper tests, and `tests/test_dxgi_shared.cpp` now adds `DXGISharedTest.FreshPostFSRStreamlineHandoffInvalidatesOnlyStaleLastWorkingQueueProof`.

- **Why this is generic**: This is not a GTA-only carve-out and not a special case for one timestamp after startup expiry. The generic rule is: if the repo's shared DX12 policy already proved that a post-FSR Streamline comeback has a safe bootstrap topology, stale OFF churn must not be replayed into that same half-armed comeback merely because the runtime never surfaced an explicit `SetOptions(ON)` edge. The same proof that allows expiry-driven PostSL activation must also be strong enough to keep the comeback startup-protected until it either confirms or truly goes inactive. Separately, a fresh authoritative Streamline handoff to a different runtime-owned queue invalidates older PostSL last-working queue proof from the prior DLSS epoch.

- **Verification**:
  - Re-checked GTA `installed/captureengine/logs/20260421_021720_gtacrash_fsgrtodlssfgtoallfgofftodlssfg/{hook_debug.log,GTA5_Enhanced.exe_FREEZE_2026-04-21_02-20-52_080.dmp,external_85a767e2-ec78-49cd-975a-73910f2f6b00.dmp}`.
  - Confirmed the decisive runtime edge: the final fresh `GetState` comeback on `scQueue=00000216DEBBA880` reaches `pending=1 unconfirmed=1`, then expiry-triggered `PostSL REACTIVATED (epoch=3 ...)`, then immediately forwards the stale suppressed OFF and later reinitializes on the older `lastWorkingQ=0000021591765620` before `devRemoved=0x887A002B`.
  - Ran `& ".\tests\unit_tests.exe" --gtest_filter='StreamlineRuntimePolicyTest.*:DXGISharedTest.DX12SwapchainOverlayRoutingPrefersValidatedLastWorkingQueueDuringPostFSRInactiveRecovery:DXGISharedTest.ReusesValidatedLastWorkingQueueForResumedDLSSDuringPostFSRInactiveRecovery:DXGISharedTest.FreshPostFSRStreamlineHandoffInvalidatesOnlyStaleLastWorkingQueueProof'`; the focused suite passed.
  - Ran `python build.py --skip-updates`; full rebuild passed and produced build `0.1.2501`.

- **Files changed**: `hook/common/streamline_runtime_policy.h`, `hook/common/dx12_overlay_policy.h`, `hook/apis/streamline_hook.cpp`, `hook/apis/dx12_hook.cpp`, `tests/test_streamline_runtime_policy.cpp`, `tests/test_dxgi_shared.cpp`, `llm-wiki/current.md`, `llm-wiki/frame-generation-switching.md`, `llm-wiki/regression-testing-and-logging.md`, `llm-wiki/log.md`

- **Stale risk**: Fresh runtime validation is still required. The next GTA check on build `0.1.2501+` is that the later `all FG off -> DLSS FG` comeback on the fresh `scQueue=00000216DEBBA880` no longer forwards the stale suppressed OFF immediately after expiry-driven `PostSL REACTIVATED`, and that a later post-FSR inactive recovery no longer reinitializes on the older prior-epoch `lastWorkingQ` after a newer authoritative Streamline handoff has already moved to a different queue. Talos should keep its already healthy post-FSR DLSS behavior.

### 2026-04-21 - Keep explicit post-FSR DLSS comebacks startup-protected through the short first-confirmed PostSL settling window and clear stale native-FSR Present ownership before the comeback

- **Motivation**: GTA V Enhanced `installed/captureengine/logs/20260420_231951_gtanocrash_overlaydisappearonfsrfgtodlssfg` still showed a user-visible `FSR_FG -> DLSS_FG` regression even after the older stale-OFF and cooldown fixes. The comeback was much healthier than the earlier `0.1.2468` and `0.1.2493` slices: GTA preserved the fresh Streamline `scQueue=000002308C26C5F0`, logged the real `OFF->ON via SetOptions`, later reached `DX12: PostSL REACTIVATED (epoch=1 ...)`, and finally confirmed rendering on that preserved queue with `origECL=1 realECL=1 sameQueue=1`. But the first visible submit was still too late, and the same comeback then tore itself back down immediately afterward.

- **Comparison that narrowed the seam**:
  1. Talos `installed/captureengine/logs/20260420_215301_talosnocrash_multipleswitching` reaches the matching post-FSR DLSS family quickly: explicit `OFF->ON via SetOptions`, normal-route startup Presents with `callbackOnNormal=1`, `postsl-activation-complete`, `PostSL REACTIVATED (epoch=1 ...)`, ECL-driven visible startup progress, then sustained `Post-SL overlay SUBMIT #1..` on the preserved `scQueue` with no immediate OFF edge.
  2. GTA reaches the same preserved Streamline `scQueue` and the same later successful queue/probe state, but before activation completes it still logs `DetectSLPresentHook: ... runtime=DLSS_FG runtimeOwnedNativeFG=1`, which means the stale native-FSR Present-ownership latch from the prior FFX teardown is surviving into the explicit DLSS comeback.
  3. After GTA finally confirms the first PostSL render (`Post-SL overlay SUBMIT #1` at `23:22:40.004`), the very next runtime update logs `Streamline Hook: FG state transition ON->OFF via GetState` at `23:22:40.021`, disabling PostSL and publishing `runtime=Off` while the comeback is still only on its first confirmed-startup-settling frame.

- **Root cause refinement**:
  1. The older deferred-OFF rule was still too narrow. It protected the explicit post-FSR DLSS comeback only while it was literally half-armed/unconfirmed. The GTA trace proves that is not sufficient: the first confirmed PostSL submit can still be inside the startup-settling window where stale OFF churn should remain suppressed.
  2. The same trace also exposed a second ownership-drift seam: an explicit Streamline/DLSS comeback can already have the preserved fresh Streamline `scQueue`, yet the stale native-FSR explicit-off ownership latch can still survive and keep the path classified as `runtimeOwnedNativeFG=1` until much later than the real topology warrants.

- **Fix**:
  1. `hook/common/streamline_runtime_policy.h` now extends `ShouldKeepOffChurnDeferredForHalfArmedExplicitPostFSRComeback(...)` and `ShouldDropSuppressedOffChurnForExplicitPostFSRComeback(...)` with the short confirmed-but-still-startup-settling state, not just the earlier half-armed/unconfirmed state.
  2. `hook/apis/streamline_hook.cpp` now threads `HookIsPostSLOverlayConfirmedButStartupSettling()` through all deferred-OFF replay and suppression sites (`ApplyCombinedStreamlineRuntimeState`, `Hooked_slDLSSGGetState`, `Hooked_slDLSSGSetOptions`, and `FlushSuppressedSetOptionsOffIfNeeded()`) and updates the diagnostics from `half-armed` to the stronger `startup-protected` wording with `settling=%d`.
  3. `hook/common/dx12_overlay_policy.h` now adds `ShouldClearStaleNativeFGPresentOwnershipOnExplicitStreamlineComeback(...)`, and `hook/apis/dx12_hook.cpp` now clears the stale native-FSR explicit-off Present-ownership latch on an explicit post-FSR Streamline comeback when the preserved non-origGame `scQueue` is already the fresh authoritative Streamline startup handoff.
  4. `tests/test_streamline_runtime_policy.cpp` now covers the new startup-settling OFF-churn rule, and `tests/test_dxgi_shared.cpp` now adds `DXGISharedTest.ExplicitStreamlineComebackClearsOnlyStaleNativeFGPresentOwnership`.

- **Why this is generic**: This is not a GTA-only workaround and not a special case for one timestamp after the first submit. The generic rule is that a real explicit post-FSR DLSS comeback remains startup-protected until it has both activated and survived the short first-confirmed-startup-settling window; stale OFF churn must not tear it down immediately after `Post-SL overlay SUBMIT #1`. Likewise, once the preserved non-origGame `scQueue` already belongs to the fresh explicit Streamline comeback, a stale native-FSR Present-ownership latch from the prior FFX teardown must not keep classifying that later DLSS startup path as still `runtimeOwnedNativeFG=1`.

- **Verification**:
  - Re-checked GTA `installed/captureengine/logs/20260420_231951_gtanocrash_overlaydisappearonfsrfgtodlssfg/hook_debug.log` and confirmed both decisive signals: `runtime=DLSS_FG runtimeOwnedNativeFG=1` before activation completes, and the immediate `ON->OFF via GetState` at `23:22:40.021` right after the first confirmed PostSL submit at `23:22:40.004`.
  - Re-checked Talos `installed/captureengine/logs/20260420_215301_talosnocrash_multipleswitching/hook_debug.log` and confirmed the matching post-FSR DLSS family does not show either of those two GTA-only drifts: no stale native-FSR ownership on the explicit DLSS startup path and no immediate OFF edge right after the first confirmed PostSL submit.
  - Ran `python build.py --run-tests --tests-only --skip-updates --gtest-filter="StreamlineRuntimePolicyTest.ExplicitPostFSRComebackKeepsOffChurnDeferredWhileHalfArmed:StreamlineRuntimePolicyTest.ExplicitPostFSRComebackDropsStaleSuppressedOffChurnOnceActive:StreamlineRuntimePolicyTest.CombinedRuntimeStateDefersHalfArmedExplicitPostFSRComebackOffAfterWindowExpiry:DXGISharedTest.ExplicitStreamlineComebackClearsOnlyStaleNativeFGPresentOwnership:DXGISharedTest.ActivePostSLStartupAlsoStaysActiveDuringRemainingFGCooldown:DXGISharedTest.VisibleOverlayCanContinueECLDrivenStartupProgressUntilFirstConfirmation:DXGISharedTest.FFXSwapchainTakeoverStillClearsStaleStreamlineOwnershipWhenFfxIsAuthoritative"`; the focused suite passed.
  - Ran `python build.py --skip-updates`; full rebuild passed and `build/verification/latest_summary.txt` reports success on build and compile-commands generation for build `0.1.2499`.

- **Files changed**: `hook/apis/dx12_hook.cpp`, `hook/apis/streamline_hook.cpp`, `hook/common/dx12_overlay_policy.h`, `hook/common/streamline_runtime_policy.h`, `tests/test_dxgi_shared.cpp`, `tests/test_streamline_runtime_policy.cpp`, `llm-wiki/current.md`, `llm-wiki/frame-generation-switching.md`, `llm-wiki/regression-testing-and-logging.md`, `llm-wiki/log.md`

- **Stale risk**: Fresh runtime validation is still required. The next GTA check is that a post-FSR explicit DLSS comeback on build `0.1.2499+` no longer logs `runtime=DLSS_FG runtimeOwnedNativeFG=1` on the later explicit Streamline startup path, and also does not log an immediate `FG state transition ON->OFF via GetState` right after the first confirmed `Post-SL overlay SUBMIT #1`. Talos should keep its already healthy one-epoch post-FSR DLSS behavior.

### 2026-04-20 - Start the startup-overlay resume countdown from the real usable same-process foreground candidate so GTA no-FG startup cannot self-latch forever on `remaining=0ms`

- **Motivation**: GTA V Enhanced `installed/captureengine/logs/20260420_225446_gtanocrash_startedwithnofgoverlaydoesnotshow` on build `0.1.2495` still lost the visible overlay when the game started with all FG off, even though GTA with FG on from the start `installed/captureengine/logs/20260420_225021_gtanocrash_startingwithfgvariousfgswitching` and Talos `installed/captureengine/logs/20260420_215301_talosnocrash_multipleswitching` were already healthy comparisons.

- **Comparison that narrowed the seam**:
  1. The failing GTA no-FG session and the same-build working GTA comparison both reach the same early healthy pre-FG state: normal DX12 overlay backend init on `origGame`, repeated `Reinit SUBMIT #1..#50`, and `Stable DX12 overlay rendering observed`.
  2. Both GTA sessions then hit the same later authoritative Streamline no-FG startup handoff on an EOS-mediated runtime-owned queue, re-arm startup compatibility, and temporarily block draws while the runtime-owned queue is unstable.
  3. Both sessions also later log `Overlay allowed during startup-overlay compatibility - queue topology stable enough for barrier-free mode` and then `Resuming DX12 overlay after startup overlay windows settled`.
  4. The decisive divergence is only after that edge. The failing no-FG session then logs nothing but repeated `DX12: Keeping overlay rendering deferred after startup-overlay resume for GTA5_Enhanced.exe (remaining=0ms)` until the stale runtime-owned no-FG queue is cleared. The working GTA comparison instead counts down `remaining=100ms -> 6ms`, then reaches `Priming DX12 overlay resources before first GTA overlay draw`, `Startup compat resource-prime settle complete`, and `Startup overlay probe complete - rendering stably`.

- **Root cause refinement**:
  1. The older `2026-04-17` same-process-foreground relaxation was directionally correct but still had one circular dependency in `ShouldDelayOverlayInitAfterStartupResumeCompat(...)`.
  2. That code only considered a same-process foreground window after `elapsedSinceResumeReady >= kStartupOverlayPostResumeSettleMs`.
  3. But `elapsedSinceResumeReady` itself only grows once CE has already picked a stable resume window and started `s_resumeStableSinceMs`.
  4. In the failing GTA no-FG session, the exact swapchain `OutputWindow` was no longer the foreground window after the late startup handoff. A usable same-process foreground window did exist, but it was the only viable candidate. Because the gate refused to treat it as a candidate until the timer had already elapsed, the code reset `s_resumeStableSinceMs` every frame and the countdown never actually started. That is why the trace could latch forever on `remaining=0ms` instead of converging into the old GTA resource-prime / first-draw path.

- **Fix**:
  1. `hook/common/overlay_compat.h` now adds `ResolveDX12OverlayStartupResumeForegroundWindowMetrics(...)`, which resolves the metrics and ownership of the real post-resume candidate window first instead of making candidate selection depend on an already-running timer.
  2. `hook/apis/dx12_hook.cpp` now uses that helper in `ShouldDelayOverlayInitAfterStartupResumeCompat(...)`, so the countdown starts on either the exact swapchain window or a usable same-process foreground window as soon as CE has a viable candidate.
  3. The hook now logs both branches explicitly: `DX12: Startup-overlay resume still waiting for a usable foreground window ...` when there is no viable candidate yet, and `DX12: Startup-overlay resume tracking usable same-process foreground window ...` when CE intentionally starts the countdown on a same-process foreground window instead of the old swapchain HWND.
  4. `tests/test_fps_limiter.cpp` now adds `OverlayCompatTest.SameProcessForegroundWindowCanDrivePostResumeCountdown` plus `OverlayCompatTest.InvalidForegroundWindowDoesNotCreatePostResumeCandidate`, alongside the earlier `OverlayCompatTest.UsableSameProcessForegroundWindowIsAcceptedAfterResumeSettle` coverage.

- **Why this is generic**: This is not a GTA-only exception and not a relaxation of the earlier startup-safety rule. The generic invariant is: after a late startup-overlay/runtime-owned handoff, CE must defer until it has a real usable foreground candidate, but once that candidate exists the countdown must be able to start on it immediately. Otherwise any title that changes from the original swapchain HWND to another same-process foreground window during startup overlay unwind can self-latch forever in the resume-defer state even with no blocker left.

- **Verification so far**:
  - Re-checked GTA no-FG `installed/captureengine/logs/20260420_225446_gtanocrash_startedwithnofgoverlaydoesnotshow/hook_debug.log` and confirmed the failing shape: repeated `remaining=0ms` lines after `Resuming DX12 overlay after startup overlay windows settled`, with no later `Priming DX12 overlay resources before first GTA overlay draw`.
  - Re-checked same-build GTA comparison `installed/captureengine/logs/20260420_225021_gtanocrash_startingwithfgvariousfgswitching/hook_debug.log` and confirmed the healthy countdown plus the later resource-prime / probe-complete sequence.
  - Re-checked Talos `installed/captureengine/logs/20260420_215301_talosnocrash_multipleswitching/hook_debug.log` as the broad healthy comparison that does not reproduce this startup-overlay seam.
  - Ran `python build.py --run-tests --tests-only --skip-updates --gtest-filter="OverlayCompatTest.*"`; the focused overlay-compat suite passed.

- **Files changed**: `hook/common/overlay_compat.h`, `hook/apis/dx12_hook.cpp`, `tests/test_fps_limiter.cpp`, `llm-wiki/current.md`, `llm-wiki/frame-generation-switching.md`, `llm-wiki/regression-testing-and-logging.md`, `llm-wiki/log.md`

- **Stale risk**: Fresh runtime validation is still required. The next GTA no-FG startup check on build `0.1.2496+` should no longer latch forever on `Keeping overlay rendering deferred ... (remaining=0ms)`. It should either log the new `still waiting for a usable foreground window` reason until a candidate appears, or log `tracking usable same-process foreground window ...`, count down, and then reach `Priming DX12 overlay resources before first GTA overlay draw` plus `Startup overlay probe complete - rendering stably` again.

### 2026-04-20 - Keep an already active half-armed post-FSR DLSS PostSL startup alive across the remaining generic FG cooldown so GTA does not restart the same comeback into a second reactivation epoch

- **Motivation**: GTA V Enhanced `installed/captureengine/logs/20260420_222504_gtanocrash_fsrfgtodlssfgoverlaydisappear` on build `0.1.2493` no longer crashes on `FSR_FG -> DLSS_FG`, but the visible overlay still disappears for much longer than Talos during the comeback. The GTA trace proves the comeback itself is structurally valid: CE preserves the fresh post-FSR Streamline `scQueue=000002237054E120`, clears the unsafe `GetState`-only suppressions on explicit `slDLSSGSetOptions(ON)`, publishes `runtime=DLSS_FG active=1`, later logs `DX12: ECL hook detected startup transition window expiry with pending PostSL activation ...`, and eventually reaches `DX12: PostSL CONFIRMED rendering via re-entrant Present` plus `DX12: Post-SL overlay SUBMIT #1`. So the queue/bootstrap/ownership path is not fundamentally broken.

- **Talos comparison that narrowed the seam**:
  1. Talos `installed/captureengine/logs/20260420_215301_talosnocrash_multipleswitching` on the same broad tree also performs the post-FSR `FSR_FG -> DLSS_FG` comeback and quickly reaches `postsl-activation-complete`, `PostSL REACTIVATED (epoch=1 ...)`, `PostSL CONFIRMED rendering`, and sustained `Post-SL overlay SUBMIT` traffic.
  2. GTA instead logs `DX12: FG transition cooldown complete — preserving half-armed synthetic PostSL startup state until confirmed render (slFG=1, reinit path)`, then immediately logs `DX12: Scene transition detected (gap=46254ms) during FG — overlay cooldown 30 frames`.
  3. During that later cooldown window GTA's routing diagnostics show `postSLCallback=1 postSLActive=0 skip=1 sceneCool=30`, and only after that does it log a second `DX12: PostSL REACTIVATED (epoch=2 hadFSR=1 ...)` before finally confirming rendering.

- **Root cause refinement**:
  1. The previous preservation helper `ShouldPreserveConfirmedPostSLDuringFGCooldown(...)` was too narrow. It only protected a PostSL path that had already reached full confirmation.
  2. But GTA's failing no-crash session shows a weaker yet still important state before that point: the same post-FSR DLSS comeback had already activated PostSL into the live half-armed startup state, then the remaining generic FG cooldown / scene-gap bookkeeping was still allowed to set `g_PostSLOverlayActive=false` again because confirmation had not happened yet.
  3. Once that happened, GTA's still-live comeback lost the active PostSL path it had already earned, dropped back to `unconfirmed=0`, and had to pay a second full `PostSL REACTIVATED` + warm-up cycle on the same preserved `scQueue` before first visible rendering resumed.
  4. This is not the older stale-OFF replay family and not a queue-ownership failure. It is a narrower local lifecycle mismatch: a generic later cooldown path could still override the current half-armed PostSL startup path even though the same live comeback was already in progress and should have been preserved until first confirmation.

- **Fix**:
  1. `hook/common/dx12_overlay_policy.h` now adds `ShouldPreserveActivePostSLDuringFGCooldown(...)`, making the stronger rule explicit and testable: while Streamline is still running, a PostSL path that is already either confirmed or active-but-unconfirmed must stay alive across the remaining generic FG cooldown.
  2. `hook/apis/dx12_hook.cpp` now uses that stronger helper in both FG cooldown seams that previously only preserved confirmed PostSL: the outer late-ON preservation block and the inner ProcessFrame cooldown countdown.
  3. The cooldown diagnostic now logs `DX12: FG cooldown preserving active PostSL path ... confirmed=%d unconfirmed=%d` so future traces show whether CE intentionally kept an already active startup path alive rather than only preserving fully confirmed PostSL.
  4. `tests/test_dxgi_shared.cpp` now adds `ActivePostSLStartupAlsoStaysActiveDuringRemainingFGCooldown` alongside the older confirmed-only cooldown preservation coverage.

- **Why this is generic**: This is not a GTA-only workaround and not a special case for one gap duration. The generic invariant is: once the current post-FSR DLSS comeback has already activated PostSL into the live half-armed startup state, later generic cooldown bookkeeping must not demote that same path back to inactive before first confirmation. Otherwise CE can restart the same live comeback into another reactivation epoch, stretching the invisible-overlay window and making recovery look broken even though queue ownership, routing, and activation provenance were already correct.

- **Verification**:
  - Re-checked GTA `installed/captureengine/logs/20260420_222504_gtanocrash_fsrfgtodlssfgoverlaydisappear/hook_debug.log` and confirmed the extra restart family: first half-armed cooldown preservation at `22:27:23.007`, later scene-gap cooldown at `22:27:23.035`, routing diagnostics with `postSLActive=0` during that cooldown, then a second `PostSL REACTIVATED (epoch=2 ...)` before first visible submit.
  - Re-checked Talos `installed/captureengine/logs/20260420_215301_talosnocrash_multipleswitching/hook_debug.log` and confirmed the matching post-FSR DLSS comeback reaches `postsl-activation-complete`, `PostSL REACTIVATED (epoch=1 ...)`, `PostSL CONFIRMED rendering`, and `Post-SL overlay SUBMIT #1` without the extra restart family.
  - Ran `python build.py --run-tests --tests-only --skip-updates --gtest-filter="DXGISharedTest.ConfirmedPostSLStaysActiveDuringRemainingFGCooldown:DXGISharedTest.ActivePostSLStartupAlsoStaysActiveDuringRemainingFGCooldown:DXGISharedTest.SyntheticStartupStateStaysHalfArmedUntilConfirmedRender:DXGISharedTest.VisibleOverlayCanContinueECLDrivenStartupProgressUntilFirstConfirmation"`; the focused policy suite passed.

- **Files changed**: `hook/common/dx12_overlay_policy.h`, `hook/apis/dx12_hook.cpp`, `tests/test_dxgi_shared.cpp`, `llm-wiki/current.md`, `llm-wiki/frame-generation-switching.md`, `llm-wiki/regression-testing-and-logging.md`, `llm-wiki/log.md`

- **Stale risk**: Fresh runtime validation is still required. The next GTA check is that `FSR_FG -> DLSS_FG` on build `0.1.2494+` no longer logs the first half-armed `FG transition cooldown complete — preserving ...` line followed by a later second `PostSL REACTIVATED (epoch=2 ...)` on the same comeback, and instead reaches first confirmation/submit from the first active startup epoch. Talos should keep its existing one-epoch post-FSR DLSS recovery behavior.

### 2026-04-20 - Keep separate injected DX12 overlay work suppressed for the full runtime-owned native-FSR Present ownership window so GTA cannot leave the callback-owned path during explicit OFF teardown

- **Motivation**: GTA V Enhanced `installed/captureengine/logs/20260420_215628_gtacrash_fsrfgcrashonresume` on build `0.1.2491` still froze on menu-driven native-FSR suspend/resume even after the earlier warm-callback-backend preservation fix. The newer session no longer shows the old bad marker from `0.1.2489`: there is only one `DX12: FFX present callback initialized overlay adapter for runtime-owned FSR ...` line for the whole session, and the log now clearly prints `DX12: ProcessFrame - preserving native FSR present-callback overlay backend during normal overlay cleanup ...` during the explicit `enabled=0` suspend window. So the old backend-churn seam was real and fixed. But the new dump `GTA5_Enhanced.exe_2026-04-20_21-59-01.dmp` still lands in the native-FSR deadlock family: full-thread `cdb` inspection again shows the `AMD FSR Interpolation Thread` and `AMD FSR Presenter Thread` blocked in `amd_fidelityfx_dx12!ffxQuery`.

- **Talos comparison that narrowed the seam**:
  1. Talos `installed/captureengine/logs/20260420_215301_talosnocrash_multipleswitching` on the same build survives repeated mixed switching and native-FSR suspension/resume with no dump.
  2. Talos still exercises the same structural explicit `ffxConfigure(frameGenerationEnabled=0)` runtime-owned teardown window and later `enabled=1` resume family.
  3. The crucial GTA-only difference is not callback-backend reinit anymore. It is that GTA's failing session spends that runtime-owned explicit-OFF window having already rebuilt the normal injected DX12 overlay on the same FFX-owned queue (`[Overlay] Initializing DX12 backend ... queue=000001DB002444D0`) while the callback backend is only being preserved passively. After FSR re-enables at `frameID=1615+`, the trace ends immediately with no resumed `DX12: FFX present callback rendered overlay on runtime-owned FSR path` line.

- **Root cause refinement**:
  1. `HookHasRuntimeOwnedNativeFGPresentPath()` already modeled the stronger ownership truth for SL Present routing and callback-backend preservation: the runtime-owned native-FG Present path still owns presentation not only while `runtime=FSR_FG`, but also during the explicit native-FSR OFF teardown window.
  2. But the separate injected DX12 overlay suppression logic was still too narrow. `ShouldSkipSeparateOverlayGpuWorkForRuntimeOwnedFrameGeneration(...)` and `ShouldDisableDedicatedOverlayQueueForRuntimeOwnedFrameGeneration(...)` only suppressed injected overlay GPU work when authoritative native FSR was already active or the effective runtime label already used FSR.
  3. During the explicit `ffxConfigure(frameGenerationEnabled=0)` teardown window, that meant CE could preserve the dedicated callback backend correctly, keep SL routing disabled correctly, yet still fall back to the normal injected DX12 overlay path on the same runtime-owned FFX queue because the temporary runtime label had already dropped to `Off`.
  4. GTA's `originalPresent=resolvedPresent=null` callback family appears sensitive to that ownership split. The runtime-owned FFX queue is still the Present owner during the explicit OFF teardown window, so letting the normal injected overlay path reassert separate GPU work there can still reopen the `amd_fidelityfx_dx12!ffxQuery` deadlock family even though the callback backend itself stayed warm.

- **Fix**:
  1. `hook/common/dx12_overlay_policy.h` now extends `ShouldSkipSeparateOverlayGpuWorkForRuntimeOwnedFrameGeneration(...)` and `ShouldDisableDedicatedOverlayQueueForRuntimeOwnedFrameGeneration(...)` with `runtimeOwnedNativeFGPresentPath`, making the stronger ownership rule explicit and testable.
  2. `hook/apis/dx12_hook.cpp` now feeds `HookHasRuntimeOwnedNativeFGPresentPath()` into those policy seams, so the explicit native-FSR OFF teardown window stays treated as unsafe for separate injected DX12 overlay GPU work even while the temporary effective runtime label says `Off`.
  3. `ShouldBridgeOverlayViaFFXPresentCallback(...)` now also follows `HookHasRuntimeOwnedNativeFGPresentPath()` instead of only the literal `runtime=FSR_FG` label, so the overlay remains logically attached to the callback-owned path for the whole runtime-owned native-FG Present ownership window.
  4. `tests/test_dxgi_shared.cpp` now extends the dedicated-queue and separate-overlay-GPU-work policy coverage so the explicit runtime-owned native-FSR OFF teardown window is locked in as part of the suppressed path.

- **Why this is generic**: This is not a GTA-only workaround and not a special case for `originalPresent=null`. The generic invariant is: if the runtime-owned native-FG Present path still owns presentation, CE must keep overlay ownership on that same runtime-cooperative callback path until that ownership truly ends. The explicit `ffxConfigure(frameGenerationEnabled=0)` teardown window is still part of that ownership window. Falling back to the normal injected DX12 overlay path there splits overlay ownership across two paths on the same FFX-owned queue and can reintroduce the native-FSR `ffxQuery` deadlock family.

- **Verification**:
  - Re-checked GTA `installed/captureengine/logs/20260420_215628_gtacrash_fsrfgcrashonresume/{hook_debug.log,GTA5_Enhanced.exe_2026-04-20_21-59-01.dmp}`.
  - Re-checked Talos `installed/captureengine/logs/20260420_215301_talosnocrash_multipleswitching/hook_debug.log` as the same-build no-crash comparison.
  - Ran `analysis/analyze_dump.ps1` plus full-thread `cdb.exe` stack inspection and confirmed the GTA dump still shows the native-FSR `amd_fidelityfx_dx12!ffxQuery` worker-thread family (`AMD FSR Interpolation Thread`, `AMD FSR Presenter Thread`) even though the old second callback-backend init is gone.
  - Ran `python build.py --run-tests --tests-only --skip-updates --gtest-filter="DXGISharedTest.*"`; the focused DXGI/policy suite passed, including the updated runtime-owned native-FSR overlay-ownership coverage.

- **Files changed**: `hook/apis/dx12_hook.cpp`, `hook/common/dx12_overlay_policy.h`, `tests/test_dxgi_shared.cpp`, `llm-wiki/current.md`, `llm-wiki/frame-generation-switching.md`, `llm-wiki/regression-testing-and-logging.md`, `llm-wiki/log.md`

- **Stale risk**: Fresh runtime validation is still required. The next GTA check is that a menu-driven native-FSR suspend/resume on build `0.1.2492+` no longer rebuilds or reuses the normal injected DX12 overlay path on the runtime-owned FFX queue during the explicit `enabled=0` teardown window, does resume back into `DX12: FFX present callback rendered overlay on runtime-owned FSR path` after `frameID=1615+`, and stays out of the `amd_fidelityfx_dx12!ffxQuery` deadlock family. Talos should keep surviving the same mixed switching family with the same generic ownership rule.

### 2026-04-20 - Preserve the warm native-FSR callback backend across temporary suspend/resume windows so GTA does not cold-reinit it back into the `ffxQuery` deadlock family

- **Motivation**: GTA V Enhanced `installed/captureengine/logs/20260420_171341_gtacrash_fsrfgcrashonsuspend` on build `0.1.2489` still froze when the game temporarily suspended native FSR FG for the menu and then resumed it a fraction of a second later. The same session's initial FSR bring-up on the `originalPresent=resolvedPresent=null` fallback-copy callback path is healthy up front, but after the suspend burst (`17:15:20.230-17:15:20.932`) GTA logs a second `DX12: FFX present callback initialized overlay adapter for runtime-owned FSR ...` at `17:15:20.980` immediately before the later freeze. The dump `GTA5_Enhanced.exe_2026-04-20_17-15-53.dmp` again shows the `AMD FSR Interpolation Thread`, `AMD FSR Presenter Thread`, and another GTA worker thread blocked in `amd_fidelityfx_dx12!ffxQuery`.

- **Talos comparison that narrowed the seam**:
  1. Talos `installed/captureengine/logs/20260420_171636_talosnocrash_fsrfgnocrashonsuspend` on the same build survives repeated menu-driven suspend/resume with no dump and continuous recovery.
  2. Talos still exercises the same transient `ffxConfigure(frameGenerationEnabled=0)` teardown window plus later resume on the runtime-owned FSR path, but its hook log only ever shows one `DX12: FFX present callback initialized overlay adapter ...` line for the whole session.
  3. That means the GTA-only divergence is not merely "native FSR got suspended" or "originalPresent was null". The sharper mismatch is callback-backend lifecycle churn on resume: GTA cold-reinitializes the dedicated callback backend again during the temporary resume family, Talos keeps the proven warm backend alive across that same kind of transient suspension.

- **Root cause refinement**:
  1. `hook/apis/dx12_hook.cpp` already has a dedicated backend for the runtime-owned native-FSR callback path (`g_FFXPresentOverlayAdapter` / `g_FFXPresentRtvHeap`) so it does not inherit the normal injected overlay backend.
  2. But generic normal-overlay cleanup still ran through the shared `CleanupOverlay()` path, and that helper unconditionally shut down the dedicated native-FSR callback backend too.
  3. During a temporary native-FSR suspension where the runtime-owned swapchain still owns presentation, that coupling is too broad. The normal pre-SL overlay path may legitimately rebuild its own sync state while `HookHasRuntimeOwnedNativeFGPresentPath()` is still true, but that does **not** mean the runtime-owned callback backend itself became invalid.
  4. GTA's null-runtime-composition fallback path appears sensitive to that needless cold callback-backend reinit on quick resume; Talos's no-crash trace shows the safe generic target state instead: keep the warm backend alive and reuse it when native FSR resumes on the same runtime-owned path.

- **Fix**:
  1. `hook/common/dx12_overlay_policy.h` now adds `ShouldPreserveFFXPresentCallbackBackendDuringNormalOverlayCleanup(...)`, making the intended rule explicit and testable.
  2. `hook/apis/dx12_hook.cpp` now lets `CleanupOverlay(...)` optionally preserve `g_FFXPresentOverlayAdapter` / `g_FFXPresentRtvHeap` while still cleaning the normal overlay sync state. Full cleanup paths still tear the callback backend down; only the transient normal-overlay cleanup family can preserve it.
  3. The ProcessFrame stale-normal-overlay cleanup path now uses that preserve mode while `HookHasRuntimeOwnedNativeFGPresentPath()` is still true, and logs `preserving native FSR present-callback overlay backend during normal overlay cleanup ...` so future suspend/resume traces show the decision directly.
  4. `EnsureOverlayAdapterReadyForFFXPresentCallback(...)` now also logs `Reusing FFX present callback overlay adapter ...` on the warm-backend path, so future GTA/Talos traces can distinguish warm reuse from a cold callback-backend init.
  5. `tests/test_dxgi_shared.cpp` now adds `NormalOverlayCleanupPreservesFFXCallbackBackendOnlyWhileRuntimeOwnedNativeFGPresentPathPersists`.

- **Why this is generic**: This is not a GTA-only workaround and not a blanket "never clean up" rule. The generic invariant is: as long as the runtime-owned native-FG Present path still owns presentation, normal pre-SL overlay cleanup must not accidentally tear down the dedicated runtime-owned callback backend. That backend should stay warm across transient suspend/resume windows and only be cold-reset on real swapchain/device/format boundaries or full cleanup paths.

- **Verification**:
  - Re-checked GTA `installed/captureengine/logs/20260420_171341_gtacrash_fsrfgcrashonsuspend/{hook_debug.log,GTA5_Enhanced.exe_2026-04-20_17-15-53.dmp}`.
  - Re-checked Talos `installed/captureengine/logs/20260420_171636_talosnocrash_fsrfgnocrashonsuspend/hook_debug.log` as the same-build no-crash comparison.
  - Used `analysis/analyze_dump.ps1` plus `cdb.exe` thread inspection and confirmed the GTA dump still lands in the native-FSR `amd_fidelityfx_dx12!ffxQuery` worker-thread family (`AMD FSR Interpolation Thread`, `AMD FSR Presenter Thread`, and GTA thread `0x5A00`).
  - Ran `python build.py --run-tests --tests-only --skip-updates --gtest-filter="DXGISharedTest.*"`; the focused DXGI/policy suite passed, including the new callback-backend preservation test.
  - Ran `python build.py --skip-updates`; full rebuild passed and produced build `0.1.2491`.

- **Files changed**: `hook/apis/dx12_hook.cpp`, `hook/common/dx12_overlay_policy.h`, `tests/test_dxgi_shared.cpp`, `llm-wiki/current.md`, `llm-wiki/frame-generation-switching.md`, `llm-wiki/regression-testing-and-logging.md`, `llm-wiki/log.md`

- **Stale risk**: Fresh runtime validation is still required. The next GTA check is that a menu-driven native-FSR suspend/resume on build `0.1.2491+` no longer logs a second `DX12: FFX present callback initialized overlay adapter ...` for the same runtime-owned path, instead logs the new preserve/reuse lines, and stays out of the `amd_fidelityfx_dx12!ffxQuery` deadlock family. Talos should continue to show the same single-init warm-backend behavior across repeated suspend/resume windows.

