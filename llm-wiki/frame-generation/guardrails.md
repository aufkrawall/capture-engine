# Frame Generation Switching

Last cross-checked: 2026-04-22

Primary sources:
- `AGENTS.md`
- `common/shared_defs.h`
- `common/config.cpp`
- `hook/common/hook_common.h`
- `hook/apis/dx12_hook.cpp`
- `hook/apis/streamline_hook.cpp`
- `hook/common/dxgi_shared.cpp`
- `hook/common/dx12_overlay_policy.h`
- `tests/test_dx12_fg_trace_replay.cpp`
- `tests/test_dxgi_shared.cpp`
- `tests/test_overlay_fg_status_publication.cpp`
- `installed/captureengine/config.ini`
- `installed/captureengine/logs/20260413_192027/hook_debug.log`

## Scope
This page records current guardrails and tested transition families for no-FG, DLSS FG, and FSR FG switching. The goal is generic support across games, not a pile of title-specific hacks.

## Project Guardrails
- We must be careful not to break DLSS FG and FSR FG in GTA again when fixing any FG issue in Talos.
- Talos and GTA are validation families, not excuses for game-specific runtime hacks.
- Switching FG must work gracefully in both Talos and GTA validation scenarios, in all directions and combinations, without crashes, without lost overlay rendering, and with the correct visible FG status.
- Fixes must stay generic so DLSS FG and FSR FG support scales across multiple games instead of accumulating title-specific branches.

## Facts
- The tree now has a shared FG session/planner layer in `hook/common/fg_session_state.{h,cpp}` that captures the current mixed DX12/Streamline/FFX FG state, computes a concrete `FGActionPlan`, and emits structured transition/session logs. It is currently used as the common authority for diagnostics and publication, while some low-level queue/route/transport execution still remains in the older DX12/DXGI code paths.
- `hook/common/dx12_fg_transition_model.cpp` is now a compatibility adapter over that planner-backed session snapshot instead of a separate competing reducer. Existing transition/replay tests still validate the same public transition contract, but the underlying snapshot now comes from the shared FG session layer.
- The planner currently sees explicit hook events from DX12, Streamline, FFX, and top-level Present observation for at least: authoritative Streamline startup handoff, authoritative FFX takeover, native-FSR configure on/off, PostSL callback install/remove, PostSL activation complete, PostSL first confirmed render, swapchain invalidation, Streamline runtime updates, and top-level Present observation.
- Current trace-replay tests explicitly cover:
  - Talos-style `off -> DLSS -> off -> FSR`
  - GTA-style `FSR -> DLSS` without a clean `off`
  - GTA-style DLSS suspension and resume during loading
  - startup coexistence using startup bypass first and normal routing after settle
- Current overlay status tests explicitly cover visible label switching between `FSR FG` and `DLSS FG`, plus clearing back to `FG` when frame generation turns off.
- Explicit native-FSR off via `ffxConfigure(frameGenerationEnabled=0)` is now treated as stronger evidence than queue-change/ECL heuristics during runtime-owned teardown. That prevents `FSR_FG -> off` from immediately re-latching heuristic `FSR_FG` on the lingering runtime-owned queue and leaving the overlay status stale.
## Open Questions / Stale-Risk
- Stale risk is high because FG switching behavior is spread across runtime classification, queue routing, startup coexistence, and visible metrics publication.
- `installed/captureengine/logs/20260411_233821` is only a short validation pass; longer GTA V Enhanced soak coverage is still worth re-checking after future queue-routing or callback-bridge changes.
- `Overlay.observer_only=true` has been validated as the clean passive baseline in `installed/captureengine/logs/20260413_203815`; the remaining runtime-validation gap is the staged `Overlay.observer_policy_only=true` probe to confirm whether Streamline startup-policy mutation alone can reintroduce the GTA V Enhanced freeze while PostSL/startup-Present behavior stays passive.
- Any future inline-hook / trampoline rewrite can silently re-break native FFX teardown paths even if the higher-level FG state machine looks correct. Re-check `hook/wrappers/inline_hook.cpp` whenever a crash lands inside the `00007FF8011D....` trampoline pool.
- Re-check this page after changes to FFX module-hook refresh, `ffxConfigure` parsing/interpretation, or the native-FSR callback bridge. `installed/captureengine/logs/20260416_140309` shows that losing FFX callback/API authority can now present as a pure visibility regression rather than a crash: the runtime-owned FSR swapchain stays active, but the overlay falls back into the passive `separate overlay GPU work is unsafe` path forever.
- Re-check this page after changes to `dx12_overlay_policy.h`, FG transition tests, or overlay metrics publication behavior.
