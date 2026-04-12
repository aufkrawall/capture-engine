# Regression Testing And Logging

Last cross-checked: 2026-04-12

Primary sources:
- `AGENTS.md`
- `build.py`
- `captureengine/injection.cpp`
- `captureengine/pseudo_overlay.cpp`
- `hook/common/dxgi_shared.cpp`
- `hook/common/overlay_metrics_publisher.cpp`
- `tests/test_dx12_fg_trace_replay.cpp`
- `tests/test_overlay_fg_status_publication.cpp`
- `tests/test_fps_limiter.cpp`

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

## Useful Existing Logging Points
- `captureengine/injection.cpp`
  - Detailed delayed-injection logs for wait-loop start, D3D12 detection, fallback path, and injection attempt.
- `captureengine/pseudo_overlay.cpp`
  - Logs pseudo-overlay suppression and resume during injected-overlay handoff.
- `hook/common/dxgi_shared.cpp`
  - Logs startup bypass and Streamline routing details, with explicit rate limiting in high-frequency paths.
- `hook/common/freeze_watchdog.cpp`
  - Logs watchdog startup, monitored thread selection, dialog-triggered dumps, freeze-triggered dumps, and explicit immediate dump requests.
- `hook/common/overlay_metrics_publisher.cpp`
  - Logs FG publication state changes and invariant violations.

## Practical Regression Checklist
- If you touch runtime classification, queue routing, startup bypass, or overlay publication, add or update unit tests in the closest policy or replay suite.
- If you touch watchdog ownership or dump-trigger conditions, verify that helper heartbeats do not silently retarget the monitored thread and that explicit stall conditions still produce an automatic dump.
- If you touch watchdog ownership or dump-trigger conditions, also verify that an immediate dump request targeting the current render/present thread is handled asynchronously instead of trying to suspend/capture that same thread inline.
- If you touch Streamline stall detection, verify that top-level `Present1` traffic counts as forward progress alongside `Present`; otherwise `Present STALLED` can become a false positive on games/runtimes that switch entrypoints during DLSS activation.
- If you touch Streamline startup-window or state-signal logic, verify that the startup transition window is only armed by fresh activation/handoff edges and cannot be kept alive indefinitely by steady-state active `GetState` / `SetOptions` polls.
- If you touch runtime-owned queue hook policy, verify both sides explicitly: native/runtime-owned FSR queues still avoid the old timing-sensitive hook path, while authoritative Streamline runtime-owned swapchain queues still expose enough ECL tracking for later DLSS handoffs.
- If you change how FG mode transitions are interpreted, verify both routing behavior and visible overlay status behavior.
- If you change injection or overlay handoff behavior, verify the runtime flags and pseudo-overlay suppression path.
- Prefer fast focused unit tests while iterating, then run broader coverage before considering the work complete.
- After build or test infrastructure changes, verify both direct `unit_tests.exe` execution and the `python build.py --run-tests` path.

## Useful Commands
```powershell
python build.py --run-tests --skip-updates
python build.py --sanitize --run-tests --skip-updates
python build.py --run-integration-tests --skip-updates
python build.py --full-integration --skip-updates

& ".\tests\unit_tests.exe" --gtest_list_tests
& ".\tests\unit_tests.exe"
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
- If a DX12 or FG bug fix lands without new coverage or better logs, this page should be revisited immediately.
