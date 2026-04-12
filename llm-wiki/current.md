# Current State

Last cross-checked: 2026-04-13

Primary sources:
- `llm-wiki/log.md`
- `build.py`
- `common/config.h`
- `common/config.cpp`
- `common/logging.cpp`
- `captureengine/main.cpp`
- `captureengine/inject_main.cpp`
- `captureengine/sensor_service.cpp`
- `hook/common/hook_common.h`
- `hook/common/hook_common.cpp`
- `hook/wrappers/inline_hook.cpp`

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
- Session bundles under `installed/captureengine/logs/<session>` now benefit from a compact `session_manifest.txt` entrypoint.
- The real `slDLSSGSetOptions(mode=OFF)` call is now suppressed during the DLSS FG startup transition window to prevent Streamline from de-initializing FG during fragile initialization. This is the newest and most important fix for the GTA V Enhanced DLSS FG freeze.

## Open Questions / Stale-Risk
- Re-check this page whenever logging controls, session bundle layout, or the wiki read-order guidance changes.
- Re-check after any future split of hook diagnostics into additional sidecar logs.
