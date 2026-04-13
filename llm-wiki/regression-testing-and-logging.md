# Regression Testing And Logging

Last cross-checked: 2026-04-13

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
- `tests/test_dx12_fg_trace_replay.cpp`
  - Transition-sequence tests for Talos-style and GTA-style FG behavior.
- `tests/test_overlay_fg_status_publication.cpp`
  - Visible FG label and multiplier publication behavior.
- `tests/test_fps_limiter.cpp`
  - Despite the name, this currently includes important overlay compatibility policy tests.
- `tests/test_config.cpp`, `tests/test_config_override.cpp`, `tests/test_shared_runtime_state.cpp`
  - Config, override, and shared-memory coverage for `Overlay.observer_only`, `Overlay.observer_policy_only`, and other runtime-visible overlay config fields.

## Useful Existing Logging Points
- `captureengine/injection.cpp`
  - Detailed delayed-injection logs for wait-loop start, D3D12 detection, fallback path, and injection attempt.
- `captureengine/main.cpp`
  - `session_manifest.txt` records `overlay_enabled`, `overlay_observer_only`, and `overlay_observer_policy_only` so passive-vs-staged-vs-active sessions are self-describing.
- `captureengine/inject_main.cpp`
  - Shared-memory config summary logs overlay enabled vs `observerOnly` / `observerPolicyOnly` alongside graphics override state.
- `captureengine/pseudo_overlay.cpp`
  - Logs pseudo-overlay suppression and resume during injected-overlay handoff.
- `hook/apis/dx12_hook.cpp`
  - Logs when observer-only suppresses early PostSL registration, PostSL callback execution, and DX12 overlay/PostSL transition management.
- `hook/apis/streamline_hook.cpp`
  - Logs pure observer-only FG transition pass-through versus staged observer-policy-only startup-policy handling.
- `hook/common/dxgi_shared.cpp`
  - Logs startup bypass and Streamline routing details, with explicit rate limiting in high-frequency paths.
- `hook/common/freeze_watchdog.cpp`
  - Logs watchdog startup, monitored thread selection, dialog-triggered dumps, freeze-triggered dumps, and explicit immediate dump requests.
- `hook/common/overlay_metrics_publisher.cpp`
  - Logs FG publication state changes and invariant violations.

## Practical Regression Checklist
- If you touch runtime classification, queue routing, startup bypass, or overlay publication, add or update unit tests in the closest policy or replay suite.
- If you use `Overlay.enabled=false` as a diagnosis baseline, verify from the logs that there is actually no DX12 overlay/PostSL/startup-policy activity. Hidden overlay and passive observer-only are not equivalent.
- If you touch `Overlay.observer_only` or `Overlay.observer_policy_only`, verify the intended split explicitly: pure observer-only still has no pre-FG overlay submits, no early/PostSL callback install/use, no special Streamline synthetic/startup Present routing, and no startup-window Streamline mutation; observer-policy-only may restore only the Streamline startup-policy family while DX12/PostSL/startup-Present behavior stays passive.
- If you touch watchdog ownership or dump-trigger conditions, verify that helper heartbeats do not silently retarget the monitored thread and that explicit stall conditions still produce an automatic dump.
- If you touch watchdog ownership or dump-trigger conditions, also verify that an immediate dump request targeting the current render/present thread is handled asynchronously instead of trying to suspend/capture that same thread inline.
- If you touch Streamline stall detection, verify that top-level `Present1` traffic counts as forward progress alongside `Present`; otherwise `Present STALLED` can become a false positive on games/runtimes that switch entrypoints during DLSS activation.
- If you touch Streamline startup-window or state-signal logic, verify that the startup transition window is only armed by fresh activation/handoff edges and cannot be kept alive indefinitely by steady-state active `GetState` / `SetOptions` polls.
- If you touch startup-window extension or deferred-OFF churn handling, verify separately that extending the window does not reset the one-shot top-level startup-handoff Present latch; a later deferred OFF must not reopen a second promoted top-level Present in the same handoff.
- If you touch promoted Streamline startup-handoff Present routing, verify both halves explicitly: the selected handoff Present must still reach the top-level CE processing path, but its return path must bypass Streamline's external Present hook instead of being fed back into Streamline a second time.
- If you touch dormant PostSL startup activation, verify both startup families explicitly: pure DLSS startup must be able to promote PostSL even when Streamline only emits a very short synthetic Present burst, while post-FSR DLSS startup still waits for the safer wrapper/direct bootstrap path before PostSL fully activates.
- If you touch the pure-DLSS startup-window gating, verify both layers separately: real PostSL rendering must still defer while the startup window is active, and the gated PostSL callback itself must also stay dormant for the top-level-handoff wrapper-progress family until the startup window expires. Otherwise CE can still perturb Streamline just by entering the callback and spending the only decisive callback on warm-up.
- If you touch the black-window DLSS startup diagnostics, verify that the pure-DLSS startup-handoff family can request an immediate dump from wrapper-only forward progress even when top-level Present traffic vanishes before the normal 30-frame `SetOptions` stall detector or the 30-second freeze watchdog timeout would fire.
- If you touch when the startup transition window is cleared, verify that the first successful PostSL startup submit does not immediately lose startup-churn protection; a transient `ON->OFF->ON` right after `Post-SL overlay SUBMIT #1` must stay on the dormant-callback path instead of triggering a full OFF teardown.
- If you touch pure-DLSS PostSL submission routing, verify that once the selected live swapchain queue already resolves to the real/original D3D12 ECL entrypoint, CE uses that direct path instead of falling back to a virtual `ExecuteCommandLists` submit on the same queue.
- If you touch runtime-owned queue hook policy, verify both sides explicitly: native/runtime-owned FSR queues still avoid the old timing-sensitive hook path, while authoritative Streamline runtime-owned swapchain queues still expose enough ECL tracking for later DLSS handoffs.
- If you touch create-swapchain queue capture or caller classification, verify that duplicate deep/inline/global capture of the same authoritative Streamline queue does not drop its hookable/runtime-owned identity, and also does not falsely re-arm the startup handoff window once the queue is already known.
- If you touch DXGI Present-hook refresh or repair logic, verify that later runtime-created live swapchains can still re-anchor Present/Present1 interception when the active Present path moves to a different vtable after startup.
- If you change how FG mode transitions are interpreted, verify both routing behavior and visible overlay status behavior.
- If you change injection or overlay handoff behavior, verify the runtime flags and pseudo-overlay suppression path.
- Prefer fast focused unit tests while iterating, then run broader coverage before considering the work complete.
- After build or test infrastructure changes, verify both direct `unit_tests.exe` execution and the `python build.py --run-tests` path.
- If you touch perf CSV durability, verify that `perf_metrics_*.csv` gains rows during long-running and hang/crash sessions instead of relying on a clean-process shutdown to flush buffered data.

## Useful Commands
```powershell
python build.py --run-tests --skip-updates
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
