# Regression Testing And Logging

Last cross-checked: 2026-04-21

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
- `tests/test_dx12_fg_trace_replay.cpp`
- `tests/test_dxgi_shared.cpp`
- `tests/test_overlay_fg_status_publication.cpp`
- `tests/test_fps_limiter.cpp`
- `tests/test_config.cpp`
- `tests/test_config_override.cpp`
- `tests/test_shared_runtime_state.cpp`

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
  - Startup-overlay resume diagnostics now also log whether CE is still waiting for a usable foreground window or intentionally tracking a same-process foreground window instead of the old swapchain HWND, which matters for late no-FG startup handoffs that can otherwise self-latch on `remaining=0ms`.
  - Native-FSR callback traces now also log callback-side HDR classification (`DX12: FFX present callback HDR check ... colorSpace=... isHDR=...`) and the FFX UI-composition contract (`premulAlpha=%d`) so visual FSR callback regressions can be distinguished from queue/routing failures.
- `hook/apis/streamline_hook.cpp`
  - Logs pure observer-only FG transition pass-through versus staged observer-policy-only startup-policy handling.
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
- If you touch native-FSR callback rendering, verify both callback-side color/composition contracts explicitly: 10-bit UNORM outputs must not be treated as HDR without probing the real display color space, and callback-side diagnostics should still reveal the resolved HDR decision and whether FFX requested premultiplied-alpha UI composition.
- If you touch native-FSR callback-backend lifecycle or any normal-overlay cleanup path that can run during FSR suspension windows, verify the temporary suspend/resume family explicitly too: a transient `ffxConfigure(frameGenerationEnabled=0)` while the runtime-owned swapchain still owns Present must not unnecessarily tear down the dedicated callback backend, GTA/Talos traces should clearly show whether CE preserved or re-used that backend, and a quick resume on the same runtime-owned path must not reopen the `amd_fidelityfx_dx12!ffxQuery` deadlock family.
- If you touch runtime-owned native-FSR overlay ownership or injected DX12 overlay suppression, verify the explicit native-FSR OFF teardown window too: while `HookHasRuntimeOwnedNativeFGPresentPath()` is still true, CE must not fall back to separate injected DX12 overlay GPU work on the runtime-owned FFX queue just because the temporary effective runtime label says `Off`. GTA/Talos traces should make that ownership visible: no resumed deadlock in `amd_fidelityfx_dx12!ffxQuery`, no cold callback-backend re-init on the same runtime-owned path, and no normal injected overlay reinit/submit path taking over that same FFX-owned queue during the explicit `enabled=0` teardown window unless ownership has truly ended.
- If you touch `hook/common/fg_session_state.*` or move a decision under the planner/session layer, verify both sides: focused unit coverage for the planner/session output and the existing reducer/replay/publication compatibility suites that still depend on the old public contract.
- If you use `Overlay.enabled=false` as a diagnosis baseline, verify from the logs that there is actually no DX12 overlay/PostSL/startup-policy activity. Hidden overlay and passive observer-only are not equivalent.
- If you touch `Overlay.observer_only`, `Overlay.observer_policy_only`, or `Overlay.observer_startup_present_only`, verify the intended split explicitly: pure observer-only still has no pre-FG overlay submits, no early/PostSL callback install/use, no special Streamline synthetic/startup Present routing, and no startup-window Streamline mutation; observer-policy-only may restore only the Streamline startup-policy family while DX12/PostSL/startup-Present behavior stays passive; observer-startup-present-only may further restore only the remaining non-Streamline startup-Present probe pieces while PostSL callback install/use and rendering still stay passive, and Streamline-originated startup-handoff Presents must stay synthetic in observer mode.
- If you touch watchdog ownership or dump-trigger conditions, verify that helper heartbeats do not silently retarget the monitored thread and that explicit stall conditions still produce an automatic dump.
- If you touch watchdog ownership or dump-trigger conditions, also verify that an immediate dump request targeting the current render/present thread is handled asynchronously instead of trying to suspend/capture that same thread inline.
- If you touch external crash capture or `MiniDumpWriteDump` interception, verify both dump families explicitly: CE's own VEH/watchdog dumps still write to the session folder, and externally handled dumps (for example Rockstar / Streamline crash paths) get mirrored into the same session folder as `external_<original-name>.dmp` instead of being stranded only in an external crash directory.
- If you touch Streamline stall detection, verify that top-level `Present1` traffic counts as forward progress alongside `Present`; otherwise `Present STALLED` can become a false positive on games/runtimes that switch entrypoints during DLSS activation.
- If you touch Streamline startup-window or state-signal logic, verify that the startup transition window is only armed by fresh activation/handoff edges and cannot be kept alive indefinitely by steady-state active `GetState` / `SetOptions` polls.
- If you touch startup-window extension or deferred-OFF churn handling, verify separately that extending the window does not reset the one-shot top-level startup-handoff Present latch; a later deferred OFF must not reopen a second promoted top-level Present in the same handoff.
- If you touch startup-window extension or deferred-OFF churn handling, also verify that the OFF-extension latch is still one-shot per churn burst. A later steady-state active `GetState` / `SetOptions` poll must not silently re-prime the extension latch and keep the startup window alive indefinitely during a post-FSR DLSS comeback.
- If you touch deferred startup-window OFF replay after a post-FSR comeback, verify both halves explicitly: half-armed explicit `SetOptions(ON)` post-FSR comebacks must keep stale OFF churn deferred past literal startup-window expiry, and once that comeback is already the live effective signal the stale buffered OFF must be dropped rather than replayed later into the recovered DLSS epoch.
- If you touch deferred startup-window OFF replay after a post-FSR comeback, also verify the short first-confirmed-startup-settling window explicitly: a stale `GetState` / `SetOptions(OFF)` churn edge must not tear the comeback back down immediately after the first `Post-SL overlay SUBMIT #1` while PostSL is still inside the confirmed-startup-settling guard.
- If you touch the confirmed-startup-settling guard itself, verify the exact boundary frame explicitly too: a post-FSR DLSS comeback that already reached `Post-SL overlay SUBMIT #8` must still remain startup-protected through that eighth confirmed frame, and must not immediately log `FG state transition ON->OFF via GetState` on the next stale runtime poll. Talos should continue past the same boundary into sustained PostSL submits.
- If you touch deferred startup-window OFF replay after a post-FSR comeback, verify the later `GetState`-only startup family too: once the shared DX12 policy already proves `safePostFSRBootstrapPath=1` on a fresh authoritative Streamline handoff, stale suppressed OFF churn must stay deferred past startup-window expiry for that comeback as well. Otherwise GTA can activate PostSL on the fresh handoff queue and then immediately replay the old OFF on the next line before first confirmed render.
- If you touch heuristic FSR detection or queue-change/ECL-pattern activation, verify the fresh authoritative Streamline startup-handoff family too: on a fresh post-FSR Streamline handoff queue, CE must not relabel the path as `FSR_FG` while Streamline still reports `runtime=STREAMLINE_NO_FG active=0`. GTA `20260421_161500` proved that stale heuristic FSR on the fresh Streamline handoff queue can poison the later DLSS comeback back into the old unsafe `OFF->ON via GetState` / `synthetic re-entrant #1` / external `sl-sha-11cf43f.dmp` family.
- If you touch post-FSR inactive recovery queue reuse, verify that a fresh authoritative Streamline handoff to a different non-origGame `scQueue` invalidates older `lastWorkingQ` proof from the previous DLSS epoch. Otherwise a failed later startup on the new queue can still make non-FG recovery reinitialize on the older queue and reopen `devRemoved=0x887A002B` / `ERR_GFX_STATE`.
- If you touch FG cooldown or late-ON preservation around post-FSR DLSS comeback, verify that an already active-but-unconfirmed PostSL startup path survives the remaining generic cooldown too. A later cooldown/scene-gap path must not drop `postSLActive` back to `0` while the same comeback is still half-armed, or GTA can restart the same comeback into a second `PostSL REACTIVATED` epoch before first confirmation and make the overlay look lost even though it eventually comes back.
- If you touch native-FSR/runtime-owned ownership latches together with a later explicit Streamline/DLSS comeback, verify that stale native-FSR Present ownership is really cleared once the explicit Streamline comeback has reclaimed the preserved non-origGame `scQueue`. GTA/Talos traces should not keep logging `runtime=DLSS_FG runtimeOwnedNativeFG=1` on that later explicit DLSS startup path just because the prior FFX teardown had not fully unwound yet.
- If you touch FFX hook refresh, export discovery, or periodic hook rescans, verify the stale-export family too: long late-session GTA/FSR teardown can leave the hook thread revalidating previously seen `ffxCreateContext` / `ffxDestroyContext` / `ffxConfigure` export addresses while the runtime is mutating or rebuilding those entrypoints. Validation must treat unreadable export addresses as normal churn and never read opcode bytes from them directly. Focused coverage should exercise the expected-detour, changed-detour, and unreadable-target cases.
- If you touch the startup-overlay post-resume gate, verify both candidate-foreground families explicitly: the exact swapchain window still works when it remains foreground, and a late no-FG startup handoff can also start the countdown on a usable same-process foreground window instead of latching forever on `Keeping overlay rendering deferred ... (remaining=0ms)`. The targeted diagnostics should make the active reason visible (`still waiting for a usable foreground window` vs `tracking usable same-process foreground window`).
- If you touch promoted Streamline startup-handoff Present routing, verify the active and staged observer paths separately: active mode may still need the selected handoff Present to reach the top-level CE processing path and return via the bypass/original DXGI Present path, but observer mode must not accidentally promote that Streamline-originated handoff Present once the staged seam has been narrowed back to the synthetic route.
- If you touch dormant PostSL startup activation, verify both startup families explicitly: pure DLSS startup must be able to promote PostSL even when Streamline only emits a very short synthetic Present burst, while post-FSR DLSS startup still waits for the safer wrapper/direct bootstrap path before PostSL fully activates.
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
