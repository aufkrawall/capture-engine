# Regression Testing And Logging

Last cross-checked: 2026-04-25

Primary sources:
- `AGENTS.md`
- `build.py`
- `common/config.cpp`
- `common/shared_defs.h`
- `captureengine/injection.cpp`
- `captureengine/main.cpp`
- `captureengine/inject_main.cpp`
- `captureengine/pseudo_overlay.cpp`
- `hook/apis/dx12_hook.cpp`
- `hook/apis/streamline_hook.cpp`
- `hook/common/dxgi_shared.cpp`
- `hook/common/overlay_metrics_publisher.cpp`
- `hook/wrappers/iat_hook.cpp`
- `tests/test_dx12_fg_trace_replay.cpp`
- `tests/test_dxgi_shared.cpp`
- `tests/test_overlay_fg_status_publication.cpp`
- `tests/test_fps_limiter.cpp`
- `tests/test_config.cpp`
- `tests/test_config_override.cpp`
- `tests/test_shared_runtime_state.cpp`
- `tests/test_streamline_runtime_policy.cpp`

## Core Expectations
- The repo is regression-paranoid. Fixes for one overlay or FG case are expected not to regress another one.
- When changing risky logic, add or update focused regression tests where feasible.
- Add enough debug logging to avoid making decisions from insufficient evidence.
- For Win32 failures, include `GetLastError()` when useful.
- For COM and D3D failures, include the `HRESULT` when useful.

## High-Value Existing Test Files
- `tests/test_fg_session_state.cpp`
  - Planner/session snapshot invariants, queue-role planning, and planner-side publication/authority coverage for the new FG session layer.
- `tests/test_dx12_fg_trace_replay.cpp`
  - Transition-sequence tests for Talos-style and GTA-style FG behavior.
- `tests/test_overlay_fg_status_publication.cpp`
  - Visible FG label and multiplier publication behavior.
- `tests/test_fps_limiter.cpp`
  - Despite the name, this currently includes important overlay compatibility policy tests.
- `tests/test_config.cpp`, `tests/test_config_override.cpp`, `tests/test_shared_runtime_state.cpp`
  - Config, override, and shared-memory coverage for `Overlay.observer_only`, `Overlay.observer_policy_only`, `Overlay.observer_startup_present_only`, and other runtime-visible overlay config fields.

## Useful Existing Logging Points
- `captureengine/injection.cpp`
  - Detailed delayed-injection logs for wait-loop start, D3D12 detection, fallback path, and injection attempt.
- `captureengine/main.cpp`
  - `session_manifest.txt` records `overlay_enabled`, `overlay_observer_only`, `overlay_observer_policy_only`, and `overlay_observer_startup_present_only` so passive-vs-staged-vs-active sessions are self-describing.
- `captureengine/inject_main.cpp`
  - Shared-memory config summary logs overlay enabled vs `observerOnly` / `observerPolicyOnly` / `observerStartupPresentOnly` alongside graphics override state.
- `captureengine/pseudo_overlay.cpp`
  - Logs pseudo-overlay suppression and resume during injected-overlay handoff.
- `hook/apis/dx12_hook.cpp`
  - Logs when observer-only suppresses early PostSL registration, PostSL callback execution, and DX12 overlay/PostSL transition management.
  - Publishes the currently discovered DX12 queue/swapchain device to native driver limiter consumers and logs the source, device, queue, and HookContext update/conflict state. This matters for explicit Reflex limiter mode, Anti-Lag 2, and XeLL paths that lazy-init from `HookContext`.
  - Startup-overlay resume diagnostics now also log whether CE is still waiting for a usable foreground window or intentionally tracking a same-process foreground window instead of the old swapchain HWND, which matters for late no-FG startup handoffs that can otherwise self-latch on `remaining=0ms`.
  - Native-FSR callback traces now also log callback-side HDR classification (`DX12: FFX present callback HDR check ... colorSpace=... isHDR=...`) and the FFX UI-composition contract (`premulAlpha=%d`) so visual FSR callback regressions can be distinguished from queue/routing failures.
- `hook/common/fps_limiter.h` / `hook/common/reflex_limiter.h`
  - Explicit Reflex limiter fallback logs now include whether a native device was available, and the Reflex limiter logs missing SetSleepMode/Sleep pointers, missing devices, SetSleepMode failures, and NvAPI Sleep failures with device/interval/game-active context.
- `hook/apis/streamline_hook.cpp`
  - Logs pure observer-only FG transition pass-through versus staged observer-policy-only startup-policy handling.
  - Streamline feature-hook discovery now scans all loaded `sl.*.dll` modules, not only `sl.interposer.dll` / `sl.common.dll`, so feature-owner DLLs such as `sl.dlss_g.dll` can arm direct-import fallbacks even when export-inline patching fails.
  - Fresh `sl.*.dll` load notifications now immediately inspect that module in addition to the periodic hook-thread scan. The log line records whether `slGetFeatureFunction`, `slSetD3DDevice`, DLSSG SetOptions/GetState, and Reflex feature hooks are ready after the load.
  - Direct-import fallback diagnostics now log owner-resolution failures and one-shot fallback-unavailable reasons. Use these lines to distinguish "no loaded module imports this feature directly yet" from a true hook-resolution failure.
  - `slGetFeatureFunction` diagnostics now log one-shot lookup outcomes for DLSSG and Reflex feature functions, including the original target, delivered target, hook readiness, and whether wrapper substitution happened. Returned-pointer wrapper fallbacks and proactive-resolution gaps are also logged once.
  - Logs current `slReflexSetOptions` and legacy `slReflexSetConstants` state changes, including incoming and forwarded `frameLimitUs`, CE limiter target, Streamline/DLSS/FSR runtime flags, and the resolved runtime mode. A nonzero incoming `frameLimitUs` is meaningful even when Reflex low-latency mode is off; CE override values must not be treated as game-owned Reflex activation.
  - Logs transition-level `slDLSSGSetOptions` requests with requested/forwarded mode, generated-frame override state, forwarded vs suppressed call state, result code, runtime mode, Streamline signal, and startup-protection flags. Use these lines to prove whether a final DLSSG OFF actually reached Streamline before chasing Reflex/NvAPI driver state.
  - Samples successful `slDLSSGGetState` calls with the supplied options mode/generated-frame request, runtime fence evidence, update decision, and whether `slDLSSGSetOptions` is hook-ready. This is intended for 2D-menu stale-label investigations where `GetState` may remain active even after the user changed menu settings.
  - Logs source-specific suppression when post-settling `slDLSSGGetState` OFF polls are held through the 30-frame PostSL warmup proof window. In GTA `20260425_000339`, Reflex/NVAPI hooks were inactive, while the decisive collapse was a GetState OFF immediately after frame-12 stabilization ended.
  - Loaded-module feature-owner scans now retry transient Toolhelp `ERROR_BAD_LENGTH` failures and log retry recovery. Talos `20260425_173428` showed this error early in the session, so a single failed module snapshot must not permanently suppress later `sl.*.dll` fallback discovery.
- `hook/common/dxgi_shared.cpp`
  - Logs startup bypass and Streamline routing details, with explicit rate limiting in high-frequency paths.
- `hook/common/freeze_watchdog.cpp`
  - Logs watchdog startup, monitored thread selection, dialog-triggered dumps, freeze-triggered dumps, and explicit immediate dump requests.
- `hook/main.cpp`
  - Logs installation of the `dbghelp.dll!MiniDumpWriteDump` mirror hook and successful/failed re-emission of externally handled dumps into the active session folder.
- `hook/common/overlay_metrics_publisher.cpp`
  - Logs FG publication state changes and invariant violations.
- `hook/common/fg_session_state.cpp`
  - Emits `FG SNAPSHOT`, `FG INVARIANT`, `FG EVENT`, `FG PLAN`, `FG PLAN DIFF`, `FG TRANSITION`, and `FG LEGACY DECISION` lines, and updates `session_manifest.txt` with planner/session metadata.

## Practical Regression Checklist
- After code changes, prefer the one-shot canonical verification command: `python build.py --verify --skip-updates`.
- After that run finishes, read `build/verification/latest_summary.txt` first and `build/verification/latest_manifest.json` second instead of scraping the full build log unless the summary says a step failed and you need the larger log.
- Do not leave a shell sitting in a passive watch loop waiting for that run to finish. Re-read `build/verification/latest_summary.txt` or `latest_manifest.json` directly when you need status; those files are the explicit completion contract for long-running verification/build work.
- If you need the full top-level log from the last verification/build run, use `build/verification/latest_build.log`; the nested sanitizer child now writes to its own dedicated log inside the same verification bundle.
- If you touch runtime classification, queue routing, startup bypass, or overlay publication, add or update unit tests in the closest policy or replay suite.
- If you touch Streamline Reflex integration, verify both the current `slReflexSetOptions` path and the legacy `slReflexSetConstants` path. GTA `installed/captureengine/logs/20260424_020300` had no constants traffic, so tests/logging that only exercise the legacy path are not enough.
- If you touch explicit Reflex limiter mode, verify that `fps_limiter_trace.log` does not merely report `limiter=reflex` while timer fallback is active. The trace should show either native/game sleep handoff or CE-owned NvAPI Sleep success, and fallback diagnostics should include `device=1`/`device=0` plus PushFpsLimit/Sleep status.
- If you touch native-FSR callback rendering, verify both callback-side color/composition contracts explicitly: 10-bit UNORM outputs must not be treated as HDR without probing the real display color space, and callback-side diagnostics should still reveal the resolved HDR decision and whether FFX requested premultiplied-alpha UI composition.
- If you touch native-FSR callback rendering or callback-side HDR setup, verify the weak-swapchain family too: once runtime-owned FSR callback rendering starts, the callback thread must not need to probe DXGI output state through a raw non-AddRef'd swapchain pointer that may already be stale. The callback path should be able to use cached trusted HDR/color-space state captured earlier from a live swapchain (`ProcessFrame` or `ffxConfigure(... swapChain=...)`), and the logs should make that explicit when it happens.
- If you touch native-FSR callback-backend lifecycle or any normal-overlay cleanup path that can run during FSR suspension windows, verify the temporary suspend/resume family explicitly too: a transient `ffxConfigure(frameGenerationEnabled=0)` while the runtime-owned swapchain still owns Present must not unnecessarily tear down the dedicated callback backend, GTA/Talos traces should clearly show whether CE preserved or re-used that backend, and a quick resume on the same runtime-owned path must not reopen the `amd_fidelityfx_dx12!ffxQuery` deadlock family.
- If you touch runtime-owned native-FSR overlay ownership or injected DX12 overlay suppression, verify the explicit native-FSR OFF teardown window too: while `HookHasRuntimeOwnedNativeFGPresentPath()` is still true, CE must not fall back to separate injected DX12 overlay GPU work on the runtime-owned FFX queue just because the temporary effective runtime label says `Off`. GTA/Talos traces should make that ownership visible: no resumed deadlock in `amd_fidelityfx_dx12!ffxQuery`, no cold callback-backend re-init on the same runtime-owned path, and no normal injected overlay reinit/submit path taking over that same FFX-owned queue during the explicit `enabled=0` teardown window unless ownership has truly ended.
- If you touch `hook/common/fg_session_state.*` or move a decision under the planner/session layer, verify both sides: focused unit coverage for the planner/session output and the existing reducer/replay/publication compatibility suites that still depend on the old public contract.
- If you use `Overlay.enabled=false` as a diagnosis baseline, verify from the logs that there is actually no DX12 overlay/PostSL/startup-policy activity. Hidden overlay and passive observer-only are not equivalent.
- If you touch `Overlay.observer_only`, `Overlay.observer_policy_only`, or `Overlay.observer_startup_present_only`, verify the intended split explicitly: pure observer-only still has no pre-FG overlay submits, no early/PostSL callback install/use, no special Streamline synthetic/startup Present routing, and no startup-window Streamline mutation; observer-policy-only may restore only the Streamline startup-policy family while DX12/PostSL/startup-Present behavior stays passive; observer-startup-present-only may further restore only the remaining non-Streamline startup-Present probe pieces while PostSL callback install/use and rendering still stay passive, and Streamline-originated startup-handoff Presents must stay synthetic in observer mode.
- If you touch watchdog ownership or dump-trigger conditions, verify that helper heartbeats do not silently retarget the monitored thread and that explicit stall conditions still produce an automatic dump.
- If you touch watchdog ownership or dump-trigger conditions, also verify that an immediate dump request targeting the current render/present thread is handled asynchronously instead of trying to suspend/capture that same thread inline.
- If you touch external crash capture or `MiniDumpWriteDump` interception, verify both dump families explicitly: CE's own VEH/watchdog dumps still write to the session folder, and externally handled dumps (for example Rockstar / Streamline crash paths) get mirrored into the same session folder as `external_<original-name>.dmp` instead of being stranded only in an external crash directory. If the target process handle is still usable, also verify the new supplemental CE-owned artifact path: the same externally handled crash should additionally yield `crash_external_<original-name>.dmp` in the session folder.
- If you touch Streamline stall detection, verify that top-level `Present1` traffic counts as forward progress alongside `Present`; otherwise `Present STALLED` can become a false positive on games/runtimes that switch entrypoints during DLSS activation.
- If you touch Streamline startup-window or state-signal logic, verify that the startup transition window is only armed by fresh activation/handoff edges and cannot be kept alive indefinitely by steady-state active `GetState` / `SetOptions` polls.
- If you touch startup-window extension or deferred-OFF churn handling, verify separately that extending the window does not reset the one-shot top-level startup-handoff Present latch; a later deferred OFF must not reopen a second promoted top-level Present in the same handoff.
- If you touch startup-window extension or deferred-OFF churn handling, also verify that the OFF-extension latch is still one-shot per churn burst. A later steady-state active `GetState` / `SetOptions` poll must not silently re-prime the extension latch and keep the startup window alive indefinitely during a post-FSR DLSS comeback.
- If you touch deferred startup-window OFF replay after a post-FSR comeback, verify both halves explicitly: half-armed explicit `SetOptions(ON)` post-FSR comebacks must keep stale OFF churn deferred past literal startup-window expiry, and once that comeback is already the live effective signal the stale buffered OFF must be dropped rather than replayed later into the recovered DLSS epoch.
- If you touch deferred startup-window OFF replay after a Streamline/DLSS startup more broadly, verify the pure-DLSS family too: a fresh explicit `all FG off -> DLSS FG` startup with `hadFSR=0` must not reactivate PostSL at startup-window expiry and then immediately replay a stale buffered `slDLSSGSetOptions(OFF)` on the next `GetState` poll before first confirmed render. GTA `20260421_204230` proved that the same stale-OFF seam can exist outside the older post-FSR family.
- If you touch SetOptions-owned Streamline runtime reduction or stale suppressed-OFF handling, verify that intentionally swallowed startup-protected `slDLSSGSetOptions(OFF)` churn does not still mutate CE's own live viewport/runtime state. GTA `20260421_213224` proved that this can be a separate seam from direct OFF replay: CE can log `Dropping stale suppressed slDLSSGSetOptions(OFF) ... startup is already stably active`, yet still later self-publish `runtime=STREAMLINE_NO_FG active=0` and tear the startup down if the local SetOptions reducer was already poisoned by the swallowed OFF.
- If you touch deferred startup-window OFF replay after a post-FSR comeback, also verify the short first-confirmed-startup-settling window explicitly: a stale `GetState` / `SetOptions(OFF)` churn edge must not tear the comeback back down immediately after the first `Post-SL overlay SUBMIT #1` while PostSL is still inside the confirmed-startup-settling guard.
- If you touch the confirmed-startup-settling guard itself, verify the exact boundary frame explicitly too: a post-FSR DLSS comeback that already reached `Post-SL overlay SUBMIT #8` must still remain startup-protected through that eighth confirmed frame, and must not immediately log `FG state transition ON->OFF via GetState` on the next stale runtime poll. Talos should continue past the same boundary into sustained PostSL submits.
- If you touch the post-settling runtime-state stabilization window, verify that stale OFF churn stays deferred through frames 9-12 after first confirmed PostSL render, and that the first post-settling `GetState OFF` is still suppressed during that window. GTA `20260422_132835` proved that dropping stale OFF immediately when settling ends can collapse the just-proven DLSS FG session. The stabilization phase intentionally does not widen broader startup-routing or cooldown behavior; it only extends Streamline stale-OFF protection.
- If you touch source-specific Streamline `GetState` OFF handling, verify the pure-DLSS frame 13-30 warmup-proof family separately from explicit `slDLSSGSetOptions(OFF)`: GetState OFF jitter may stay deferred until the PostSL warmup proof threshold, but deliberate SetOptions OFF suppression must not be widened by that rule. GTA `20260425_000339` proved that a GetState OFF at frame 13 can still collapse an otherwise submitting PostSL startup.
- If you touch Streamline feature-module discovery or direct-import fallbacks, verify that all `sl.*.dll` feature owners are considered and that repeated scans are idempotent. Talos `20260425_002642` proved that a 2D-menu DLSS FG disable can be missed if CE never observes the explicit `slDLSSGSetOptions(OFF)` edge and only polls active `slDLSSGGetState`.
- If you touch fresh module-load hooks, verify that the immediate load-notification inspection and the periodic hook-thread scan stay idempotent and agree on hook readiness. Talos `20260425_181251` still showed no Streamline OFF edge in the 2D menu, so future traces need to preserve the lookup/fallback/GetState diagnostics that prove whether the OFF call is missing or just bypassing one capture seam.
- If you touch deferred startup-window OFF replay after a post-FSR comeback, verify the later `GetState`-only startup family too: once the shared DX12 policy already proves `safePostFSRBootstrapPath=1` on a fresh authoritative Streamline handoff, stale suppressed OFF churn must stay deferred past startup-window expiry for that comeback as well. Otherwise GTA can activate PostSL on the fresh handoff queue and then immediately replay the old OFF on the next line before first confirmed render.
- If you touch heuristic FSR detection or queue-change/ECL-pattern activation, verify the fresh authoritative Streamline startup-handoff family too: on a fresh post-FSR Streamline handoff queue, CE must not relabel the path as `FSR_FG` while Streamline still reports `runtime=STREAMLINE_NO_FG active=0`. GTA `20260421_161500` proved that stale heuristic FSR on the fresh Streamline handoff queue can poison the later DLSS comeback back into the old unsafe `OFF->ON via GetState` / `synthetic re-entrant #1` / external `sl-sha-11cf43f.dmp` family.
- If you touch Streamline startup-handoff lifetime or explicit-activation provenance, verify the mixed-order post-FSR startup family too: a fresh authoritative Streamline handoff must stay `streamlineStartupHandoffPending=1` while synthetic startup is still half-armed, and a later real `slDLSSGSetOptions(ON)` must still upgrade the current live comeback to explicit provenance even if `GetState` surfaced `active=1` first. GTA `20260421_170319` proved that losing either half can recreate the same bad shape: false queue-change `FSR_FG` relabel on the fresh Streamline handoff queue, then `OFF->ON via GetState`, `Treating Streamline-originated Present as synthetic re-entrant #1`, and repeated external `sl-sha-11cf43f.dmp` dumps.
- If you touch post-FSR inactive recovery queue reuse, verify that a fresh authoritative Streamline handoff to a different non-origGame `scQueue` invalidates older `lastWorkingQ` proof from the previous DLSS epoch. Otherwise a failed later startup on the new queue can still make non-FG recovery reinitialize on the older queue and reopen `devRemoved=0x887A002B` / `ERR_GFX_STATE`.
- If you touch FG cooldown or late-ON preservation around post-FSR DLSS comeback, verify that an already active-but-unconfirmed PostSL startup path survives the remaining generic cooldown too. A later cooldown/scene-gap path must not drop `postSLActive` back to `0` while the same comeback is still half-armed, or GTA can restart the same comeback into a second `PostSL REACTIVATED` epoch before first confirmation and make the overlay look lost even though it eventually comes back.
- If you touch overlay reinit cooldown or dormant PostSL startup preservation, verify the pure-DLSS runtime-owned handoff family too: once synthetic PostSL startup is already half-armed, the overlay-reinit cooldown path itself must not force-clear that same startup and restart it into `PostSL REACTIVATED (epoch=2 ...)` before first confirmation. GTA `20260421_213224` proved that the main FG cooldown preservation rule alone is not sufficient if the parallel overlay-reinit cooldown branch still re-disables PostSL on the runtime-owned Streamline `scQueue`.
- If you touch PostSL bootstrap, dormant synthetic startup activation, or scene-gap cooldown logic, verify the fresh pure-DLSS runtime-owned handoff family too: once a fresh authoritative Streamline handoff has already moved pure-DLSS startup onto a new runtime-owned `scQueue`, PostSL must be able to rebuild torn-down overlay state on that new swapchain even if `ProcessFrame` is still active, and a large frametime gap must not inject a fresh scene-transition overlay cooldown while that same synthetic startup is still half-armed. GTA `20260421_223723` proved that otherwise CE can spend dozens of callback entries on `PostSL SKIP — state unavailable ... init=0 sync=0 ...` and then self-delay the first confirmed submit with `Scene transition detected ... cooldown 30 frames`, even though the startup would otherwise have confirmed much earlier.
- If you touch pure-DLSS startup-handoff lifetime, startup-settling protection, or scene-gap cooldown logic, verify the next narrower pure-DLSS family too: after a fresh runtime-owned Streamline handoff already reaches `PostSL CONFIRMED rendering` and the first `Post-SL overlay SUBMIT` lines, CE must still keep that same startup startup-protected through the short confirmed-startup-settling window. GTA `20260421_235555` proved that if CE clears the startup-handoff protection or allows a new scene-gap cooldown there, the next Streamline-originated Present can still fall back to `Treating Streamline-originated Present as synthetic re-entrant #1` and reopen the old patched-`dxgi!CDXGISwapChain::Present+0x5` crash family even though the first confirmed PostSL submits already succeeded.
- If you touch native-FSR/runtime-owned ownership latches together with a later explicit Streamline/DLSS comeback, verify that stale native-FSR Present ownership is really cleared once the explicit Streamline comeback has reclaimed the preserved non-origGame `scQueue`. GTA/Talos traces should not keep logging `runtime=DLSS_FG runtimeOwnedNativeFG=1` on that later explicit DLSS startup path just because the prior FFX teardown had not fully unwound yet.
- If you touch FFX hook refresh, export discovery, or periodic hook rescans, verify the stale-export family too: long late-session GTA/FSR teardown can leave the hook thread revalidating previously seen `ffxCreateContext` / `ffxDestroyContext` / `ffxConfigure` export addresses while the runtime is mutating or rebuilding those entrypoints. Validation must treat unreadable export addresses as normal churn and never read opcode bytes from them directly. Focused coverage should exercise the expected-detour, changed-detour, and unreadable-target cases.
- If you touch the startup-overlay post-resume gate, verify both candidate-foreground families explicitly: the exact swapchain window still works when it remains foreground, and a late no-FG startup handoff can also start the countdown on a usable same-process foreground window instead of latching forever on `Keeping overlay rendering deferred ... (remaining=0ms)`. The targeted diagnostics should make the active reason visible (`still waiting for a usable foreground window` vs `tracking usable same-process foreground window`).
- If you touch promoted Streamline startup-handoff Present routing, verify the active and staged observer paths separately: active mode may still need the selected handoff Present to reach the top-level CE processing path and return via the bypass/original DXGI Present path, but observer mode must not accidentally promote that Streamline-originated handoff Present once the staged seam has been narrowed back to the synthetic route.
- If you touch dormant PostSL startup activation, verify both startup families explicitly: pure DLSS startup must be able to promote PostSL even when Streamline only emits a very short synthetic Present burst, while post-FSR DLSS startup still waits for safe bootstrap evidence before PostSL fully activates. Safe post-FSR evidence can be the older wrapper/direct bootstrap path, or the newer 2D-menu proof where the fresh runtime-owned Streamline `scQueue` is also the live command queue and CE already tracks that queue's ECL submit path.
- If you touch post-FSR PostSL safe-bootstrap rules, verify the menu-only runtime-owned swapchain path too: after `FSR FG -> DLSS FG`, Streamline may already have moved to a fresh non-origGame `scQueue` and the live command queue may match it, but no SL wrapper queue may appear until 3D rendering resumes. In that shape, a tracked ECL submit path on the runtime-owned `scQueue` is the safety proof; `safeBootstrap=0` strands the overlay in the 2D menu.
- If you touch the pure-DLSS startup-window gating, verify both layers separately: real PostSL rendering must still defer while the startup window is active, and the gated PostSL callback itself must also stay dormant for the top-level-handoff wrapper-progress family until the startup window expires. Otherwise CE can still perturb Streamline just by entering the callback and spending the only decisive callback on warm-up.
- If you touch the black-window DLSS startup diagnostics, verify that the pure-DLSS startup-handoff family can request an immediate dump from wrapper-only forward progress even when top-level Present traffic vanishes before the normal 30-frame `SetOptions` stall detector or the 30-second freeze watchdog timeout would fire.
- If you touch when the startup transition window is cleared, verify that the first successful PostSL startup submit does not immediately lose startup-churn protection; a transient `ON->OFF->ON` right after `Post-SL overlay SUBMIT #1` must stay on the dormant-callback path instead of triggering a full OFF teardown.
- If you touch pure-DLSS PostSL submission routing, verify that once the selected live swapchain queue already resolves to the real/original D3D12 ECL entrypoint, CE uses that direct path instead of falling back to a virtual `ExecuteCommandLists` submit on the same queue.
- If you touch runtime-owned queue hook policy, verify both sides explicitly: native/runtime-owned FSR queues still avoid the old timing-sensitive hook path, while authoritative Streamline runtime-owned swapchain queues still expose enough ECL tracking for later DLSS handoffs.
- If you touch mixed Streamline/FFX create-swapchain queue classification, verify that current authoritative FFX ownership overrides stale Streamline provenance on the same non-origGame queue. Otherwise CE can accidentally keep a native/runtime-owned FSR queue on the `ExecuteCommandLists` detour path and reopen the old `amd_fidelityfx_dx12!ffxQuery` deadlock family even though the callback overlay bridge itself is correct.
- If you touch DXGI Streamline Present-routing detection or re-enable logic, verify the native-FSR/runtime-owned takeover window too: Streamline routing must stay disabled not only while the effective runtime label already says `FSR_FG`, but also during the explicit native-FSR OFF teardown window where FFX still owns presentation on the runtime-owned swapchain. Otherwise CE can silently re-attach Streamline's Present-hook chain in the middle of an FFX-owned handoff and reopen mixed-runtime Present conflicts even though queue-hook policy is already correct.
- If you touch create-swapchain queue capture or caller classification, verify that duplicate deep/inline/global capture of the same authoritative Streamline queue does not drop its hookable/runtime-owned identity, and also does not falsely re-arm the startup handoff window once the queue is already known.
- If you touch DXGI Present-hook refresh or repair logic, verify that later runtime-created live swapchains can still re-anchor Present/Present1 interception when the active Present path moves to a different vtable after startup.
- If you change how FG mode transitions are interpreted, verify both routing behavior and visible overlay status behavior.
- If you change injection or overlay handoff behavior, verify the runtime flags and pseudo-overlay suppression path.
- Prefer fast focused unit tests while iterating, then run broader coverage before considering the work complete.
- After build or test infrastructure changes, verify both direct `unit_tests.exe` execution and the `python build.py --run-tests` path.
- After build/test workflow changes, verify the focused iteration path too: `python build.py --run-tests --tests-only --skip-updates --gtest-filter=...` should stop before the expensive product builds and should show live compile/test progress instead of going silent for long periods.
- If you touch perf CSV durability, verify that `perf_metrics_*.csv` gains rows during long-running and hang/crash sessions instead of relying on a clean-process shutdown to flush buffered data.

## Useful Commands
```powershell
python build.py --verify --skip-updates
python build.py --run-tests --skip-updates
python build.py --run-tests --tests-only --skip-updates --gtest-filter=DXGISharedTest.*
python build.py --sanitize --run-tests --skip-updates
python build.py --run-integration-tests --skip-updates
python build.py --full-integration --skip-updates

& ".\tests\unit_tests.exe" --gtest_list_tests
& ".\tests\unit_tests.exe"
& ".\tests\unit_tests.exe" --gtest_filter=ConfigTest.ParseOverlayInclusionOptions:ConfigOverrideTest.SimpleOverride:SharedDefsTest.OverlayConfigSeqlockPublishesStableSnapshot
& ".\tests\unit_tests.exe" --gtest_filter=DX12FGTraceReplayTest.*
& ".\tests\unit_tests.exe" --gtest_filter=OverlayFGStatusPublicationTest.*

python .\testapp\run_tests.py --api dx9 --arch both --tests 1 --duration 5 --min-frames 60
python .\testapp\run_tests.py --api all --arch both --tests 1 --duration 5 --min-frames 60

powershell -ExecutionPolicy Bypass -File .\analysis\analyze_dump.ps1 .\installed\captureengine\logs\<session>\<dump>.dmp
```

## Dump Analysis Notes
- Windows-host builds now emit sidecar `.pdb` files via clang CodeView debug info plus `lld` PDB emission, so `cdb.exe`, WinDbg, and Visual Studio can load native symbols without switching the build to MSVC.
- `analysis/analyze_dump.ps1` now prefers dump-local archived symbols under `<dump-dir>\symbols\captureengine` first, then falls back to repo-local symbol locations in `installed/captureengine`, `tests`, and `installed/testapp`.
- The helper also adds a Microsoft symbol-server cache under `build/symbols-cache` by default so system DLL frames resolve more reliably.
- `SetCrashDumpDirectory()` now snapshots the current CE runtime PE/PDB set into `<session-log-dir>\symbols\captureengine\` so later dump analysis is not tied to whatever binaries happen to be installed after future rebuilds.
- Direct CE crash dumps now use richer `MiniDumpWriteDump` flag sets first, including thread info, unloaded modules, handle data, process-thread data, and full-memory metadata, with compatibility fallbacks if a target process rejects the richer combination.
- The injected hook now also mirrors successful in-process external `MiniDumpWriteDump` calls into the active CE session folder when the original dump path lives elsewhere. This is intended for fast-crash families that are handled by the game/runtime rather than CE's own unhandled-exception path.
- Legacy standalone `.dbg` files are not produced; `.pdb` is now the intended Windows symbol format in this repo.

## Logging Guidance
- Prefer logs that make transition and ownership changes reconstructible later.
- Keep logs specific enough to compare traces across regressions.
- Rate-limit logs in per-frame paths so diagnosis stays useful instead of turning into noise.
- When the runtime is inferring state from weak evidence, bias toward adding more diagnostics before adding more heuristics.

## Open Questions / Stale-Risk
- Stale risk is medium because tests and logs evolve along with the runtime.
- `Overlay.observer_only` and `Overlay.observer_policy_only` span config, shared-memory, DX12, DXGI, and Streamline hook paths, so stale risk increases quickly if only one side gets updated.
- If a DX12 or FG bug fix lands without new coverage or better logs, this page should be revisited immediately.
