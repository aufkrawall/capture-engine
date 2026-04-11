# llm-wiki Log

Last cross-checked: 2026-04-11

Purpose:
- Track wiki edits.
- Record which source files were checked.
- Leave a short stale-risk note for fast-moving areas.

Update rules:
- Add a new dated entry whenever wiki facts change in a meaningful way.
- Record the pages touched, why they changed, and the primary source files that were checked.
- If an area is churning, call that out explicitly so the next reader knows to re-check the code.

## Activity Timeline

### 2026-04-11 - Initial bootstrap
- Created `index.md`, `log.md`, `codestyle.md`, `build.py.md`, `repo-map.md`, `dx12-injection-bootstrap.md`, `dx12-overlay-third-party-coexistence.md`, `frame-generation-switching.md`, `overlay-fg-status.md`, and `regression-testing-and-logging.md`.
- Slimmed `AGENTS.md` so it now focuses on agent workflow, project constraints, and `llm-wiki` governance instead of carrying the full factual repo reference.
- Updated `.clang-format` comments to point at `llm-wiki/codestyle.md` instead of `AGENTS.md`.
- Cross-checked against `build.py`, `.clang-format`, `.flake8`, `pyrightconfig.json`, `common/shared_defs.h`, `captureengine/injection.cpp`, `captureengine/inject_main.cpp`, `captureengine/pseudo_overlay.cpp`, `hook/main.cpp`, `hook/common/dx12_overlay_policy.h`, `hook/common/overlay_compat.h`, `hook/common/dxgi_shared.cpp`, `hook/common/overlay_metrics_publisher.cpp`, `hook/wrappers/custom_hook.h`, `tests/test_dx12_fg_trace_replay.cpp`, `tests/test_overlay_fg_status_publication.cpp`, and `tests/test_fps_limiter.cpp`.
- Stale-risk note: the DX12 overlay / FG pages derive from fast-moving code paths and should be re-checked after any queue-routing, startup-bypass, runtime-classification, or FG-status-publication change.
