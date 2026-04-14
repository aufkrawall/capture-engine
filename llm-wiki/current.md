# Current State

Last cross-checked: 2026-04-13

Primary sources:
- `llm-wiki/log.md`
- `build.py`
- `common/config.h`
- `common/config.cpp`
- `common/shared_defs.h`
- `common/logging.cpp`
- `captureengine/main.cpp`
- `captureengine/inject_main.cpp`
- `captureengine/sensor_service.cpp`
- `hook/apis/dx12_hook.cpp`
- `hook/apis/streamline_hook.cpp`
- `hook/common/dxgi_shared.cpp`
- `hook/common/hook_common.h`
- `hook/common/hook_common.cpp`
- `hook/wrappers/inline_hook.cpp`
- `hook/main.cpp`
- `common/crash_handler.cpp`
- `common/crash_dump_policy.h`

## Purpose
This page is the compact LLM entrypoint for current repo state. Read it before diving into long historical pages or `log.md`.

## Read This First
- For current build and logging controls, start here, then read `build.py.md` or `regression-testing-and-logging.md` if needed.
- For active DX12 / FG debugging, read this page, then `frame-generation-switching.md`, then the newest relevant `log.md` entry.
- Use `log.md` as the historical archive, not the first stop for every task.

## Current Logging Model
- `config.ini` now supports `log_level=off|error|warn|info|debug|trace`.
- `debug` is the normal debugging level and should be the default for day-to-day diagnosis.
- `trace` is the forensic level for very verbose internals such as inline-hook byte dumps and the per-frame CSV perf logger.
- Legacy `debug_logging=true/false` still works as a compatibility input and maps to `debug` / `off`.
- Legacy `perf_metrics_logging=true` still works as a compatibility input and escalates to `trace`.

## Current Token-Efficiency Rules
- Prefer compact current-state pages before historical changelogs.
- Keep high-value transition logs at `debug`.
- Move byte dumps, per-instruction traces, and similar forensic detail to `trace` or dedicated side logs.
- Prefer change logs and periodic summaries over steady-state heartbeat spam.
- Preserve historical genesis in detailed pages and `log.md`; do not flatten history into only a short summary.

## Current Hot Areas
- DX12 / FG startup churn and queue ownership remain high-risk and change frequently.
- Inline hook correctness is still a sensitive subsystem; use `trace` when debugging trampoline relocation or byte-level hook installs.
- Session bundles under `installed/captureengine/logs/<session>` now benefit from a compact `session_manifest.txt` entrypoint, including `overlay_enabled`, `overlay_observer_only`, `overlay_observer_policy_only`, and `overlay_observer_startup_present_only`.
- `Overlay.enabled=false` alone is not a strict DX12 non-interference baseline; older GTA DLSS FG runs still performed pre-FG overlay setup and PostSL/startup routing with the visible overlay hidden.
- `Overlay.observer_only=true` is now the preferred injected passive baseline for DLSS FG debugging. It keeps hooks, logging, and runtime FG telemetry alive while suppressing DX12 overlay rendering, PostSL callback install/use, special startup Present routing, and CE's Streamline startup-policy mutation. It still preserves Streamline transition heuristic cleanup (queue-change reset, teardown grace, false-heuristic clearing) so transient `GetState` OFF churn does not get misclassified as `FSR_FG`.
- `Overlay.observer_policy_only=true` is a staged probe that only applies with `Overlay.observer_only=true`. It keeps DX12 overlay rendering, PostSL callback install/use, and special Streamline synthetic/startup Present routing passive, but lets the Streamline hook keep the startup-policy family alive: startup transition window arming/extension, startup-window OFF suppression/flush, fresh-activation suppression, and FSR-owned enable preparation. This isolates startup-policy mutation from PostSL/startup-Present behavior.
- `Overlay.observer_startup_present_only=true` is a narrower staged probe that only applies with both `Overlay.observer_only=true` and `Overlay.observer_policy_only=true`. It keeps PostSL callback install/use and rendering passive. The earlier staged versions re-enabled special Streamline startup Present routing and still reproduced the GTA V Enhanced freeze in `installed/captureengine/logs/20260413_222933`, `20260413_224823`, `20260413_231005`, `20260413_235512`, and `20260414_231024`. Build `0.1.2269` narrows the staged meaning again so special Streamline Present routing stays passive in all observer modes; this seam now preserves only Streamline startup-policy mutation plus the remaining non-Streamline startup-Present probe pieces such as the FFX startup bypass. The fresh GTA V Enhanced validation `installed/captureengine/logs/20260414_232934` on `0.1.2269` stayed stable with no dump, no `Present STALLED`, no promoted/synthetic Streamline startup Present logs, and no FFX modules present, which confirms that removing special Streamline Present routing from the observer seam eliminated the reproduced crash family.
- The next active-mode validation `installed/captureengine/logs/20260414_234144` on `0.1.2269` froze/crashed again with `observer_only=0`. The failure is narrower than the older active family: CE still logs `DX12: Streamline FG ON — installed gated PostSL callback`, `DX12: Streamline FG ON — pre-armed PostSL callback for startup routing`, and exactly one `Treating Streamline startup-handoff Present as top-level live Present`, then Streamline starts writing repeated external dumps under `C:\ProgramData\NVIDIA\Streamline\...\sl-sha-11cf43f.dmp`, CE mirrors them into the session, and `Present STALLED` appears before any successful PostSL activation or submit. Build `0.1.2270` narrows active mode again: that one promoted top-level startup-handoff Present is no longer treated as a special top-level/bypass family, and the ECL-side startup-activation trigger now uses the last tracked swapchain instead of forcing a null-swapchain callback.
- The next active validation `installed/captureengine/logs/20260415_002110` on `0.1.2270` proved the promoted-handoff fix worked: the old `Treating Streamline startup-handoff Present as top-level live Present` / `Promoted Streamline startup-handoff Present using bypass return path` lines are gone. The crash moved to the very next seam instead: after wrapper-queue progress appears, CE logs exactly one `Treating Streamline-originated Present as synthetic re-entrant #1`, then the same crash family returns and the early pure-DLSS wrapper-only stall watchdog fires. That means the remaining toxic path is now the first decisive synthetic Streamline startup Present while PostSL is still half-armed. Build `0.1.2271` narrows active mode again: that first decisive synthetic startup Present now stays on the normal SL route instead of the synthetic/bypass path, and the live ECL-expiry trigger finally uses the cached swapchain instead of the stale null-swapchain callback block.
- The next active validations `installed/captureengine/logs/20260415_003239` on `0.1.2271` and `installed/captureengine/logs/20260415_004226` on `0.1.2272` progressed further again. In both runs the startup-handoff Present stayed on the normal route, a long burst of synthetic startup Presents also stayed on the normal route, and the ECL-expiry callback finally fired with `cachedSwapchain=%p`. But the very next post-expiry Streamline-originated Present still fell back into `Treating Streamline-originated Present as synthetic re-entrant #1`, then the same external crash family returned. Build `0.1.2273` widens the active guard again so once startup bootstrap is consumed, Streamline-originated startup Presents stay off the synthetic/bypass path until PostSL startup has actually completed, not just while the startup window is still active.
- The real `slDLSSGSetOptions(mode=OFF)` call is still suppressed during the active-mode DLSS FG startup transition window to prevent Streamline from de-initializing FG during fragile initialization.
- Fast-crash GTA V Enhanced runs can bypass CE's VEH/UEF dump path and still produce an external dump through `dbghelp!MiniDumpWriteDump`. The narrowed observer run produced `C:\Users\TestUser\AppData\Local\Rockstar Games\GTAV Enhanced\CrashLogs\51e9b489-70bb-4998-a4ec-254bdd858cbd.dmp` at `2026-04-13 23:11:47.519`, and that dump still points into the same `sl_dlss_g` / `capture_hook_x64` / `dxgi!CDXGISwapChain::Present` crash family.
- `hook/main.cpp` now installs a generic inline hook on `dbghelp.dll!MiniDumpWriteDump` when `dbghelp.dll` is already loaded or loads later. When the current process successfully writes a dump outside the active CE session directory, CE re-emits the same dump into the session folder as `external_<original-name>.dmp`, so externally handled crash paths still leave session-local artifacts.

## Open Questions / Stale-Risk
- Re-check this page whenever logging controls, session bundle layout, external-dump mirroring, or `Overlay.observer_only` / `Overlay.observer_policy_only` / `Overlay.observer_startup_present_only` semantics change.
- Re-check after any future split of hook diagnostics into additional sidecar logs, passive-baseline workflow changes, or DbgHelp hook ownership changes.
