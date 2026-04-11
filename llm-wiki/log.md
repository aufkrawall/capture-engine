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

### 2026-04-11 - Fix FSR FG + overlay freeze (SL routing re-enablement bug)
- **Root cause**: When FSR FG activated in GTA V Enhanced, `DisableSLPresentRouting()` correctly set `s_slRoutingActive = false`, but `DetectSLPresentHook()` on the next Present call immediately re-enabled it because the SL E9 JMP was still present at `oPresent`. This caused Present to go through SL's hook chain while FSR FG owned the swapchain, deadlocking the render thread inside `amd_fidelityfx_dx12!ffxQuery`.
- **Fix**: Added FSR FG state checks at three layers:
  1. `DetectSLPresentHook()` now refuses to re-enable SL routing when the effective runtime mode is FSR.
  2. DetourPresent SL detection guard skips `DetectSLPresentHook()` when the effective runtime mode is FSR.
  3. DetourPresent1 SL detection guard also skips when the effective runtime mode is FSR.
  4. Safety latch in the Present/Present1 routing paths: if SL routing is somehow active while the effective runtime mode is FSR, force-disable it and route through the trampoline directly.
- **Evidence**: Freeze dump from `GTA5_Enhanced.exe_FREEZE_2026-04-11_20-45-30_438.dmp` showed render thread T:3538 stuck in `amd_fidelityfx_dx12!ffxQuery`. Hook debug log showed SL routing DISABLED at 20:44:57.077 then re-ACTIVE at 20:44:57.230 (153ms later), contradicting the FFX takeover.
- Pages touched: `log.md`, `frame-generation-switching.md`.
- Source files checked: `hook/common/dxgi_shared.cpp` (lines 698-742 DetectSLPresentHook, 981-991 Present SL check, 1173-1204 Present routing, 1303-1304 Present1 SL check, 1431-1439 Present1 routing, 2234-2242 DisableSLPresentRouting), `hook/apis/dx12_hook.cpp` (line 1445 DisableSLPresentRouting call site), `hook/common/fg_detection.h`, `hook/common/fg_runtime_state.h`.
- Regression coverage: `tests/test_fg_runtime_state.cpp` now asserts heuristic FSR classification is treated as FSR for routing guards.
- Stale-risk note: SL routing suppression during FSR FG is now a critical invariant. Any change to SL routing logic or FG runtime classification must verify this invariant holds.
