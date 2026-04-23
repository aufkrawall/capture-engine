# Overlay FG Status

Last cross-checked: 2026-04-23

Primary sources:
- `hook/common/overlay_metrics_publisher.cpp`
- `hook/common/dx12_overlay_policy.h`
- `tests/test_overlay_fg_status_publication.cpp`

## Scope
This page records how the current tree publishes visible FG status to the overlay.

## Facts
- The current shared helper for FG publication is `PublishOverlayFGMetrics()`.
- The main DXGI/DX12 publication call sites now publish from the planner-owned `FGActionPlan` rather than each caller independently deciding `(effectiveFGActive, runtimeMode)` ad hoc.
- `DX12::ProcessFrame` is an exception: it publishes its locally-computed `(currentFGActive, currentRuntimeMode)` instead of the raw global `FGActionPlan`. This ensures that per-frame suppression (e.g., the SL-off grace period) is reflected in the visible overlay status instead of leaving a stale label on screen. When the local state diverges from the plan, a diagnostic log line is emitted.
- That helper uses `ResolveOverlayFGMetricType()` to map `(effectiveFGActive, runtimeMode)` into the published FG type.
- If frame generation is active but the resolved published type is `0`, the helper logs an invariant violation.
- When FG is active, the helper currently publishes a multiplier of at least `2`.
- When FG is inactive, the helper resets published output FPS and base FPS to `0.0f` and resets the multiplier to `1`.
- The helper tracks the last published state and only emits the main publication log when the visible state actually changes.
- A stale visible FG label can still originate upstream from runtime classification drift even when `PublishOverlayFGMetrics()` itself behaves correctly. Talos `installed/captureengine/logs/20260416_192846` showed this explicitly: CE first published `runtime=STREAMLINE_NO_FG active=0`, then immediately re-detected heuristic `FSR_FG` from a lingering runtime-owned queue after `ffxConfigure(frameGenerationEnabled=0)` had already disabled the native FSR present-callback bridge.
- The current tree now treats an explicit native-FSR `frameGenerationEnabled=0` signal as authoritative during runtime-owned teardown: heuristic `FSR_FG` reactivation is suppressed until the runtime-owned swapchain ownership actually unwinds or native FSR explicitly turns back on. That keeps the visible overlay status from snapping back to stale `FSR FG` after a real `FSR_FG -> off` transition.
- Current tests explicitly require:
  - `FSR FG` to publish as `FSR FG`
  - `DLSS FG` to publish as `DLSS FG`
  - switching between those modes to update the visible label immediately
  - transitioning back to `off` to clear the published FG status back to the baseline `FG` label
- `tests/test_overlay_fg_status_publication.cpp` now also covers the planner-driven publication overload directly, so publication correctness is no longer tested only through the legacy helper-input path.

## Practical Guidance
- Route visible FG status publication through the shared helper instead of duplicating mapping logic in multiple runtime paths.
- Treat the visible overlay state as user-facing correctness, not cosmetic best effort.
- When changing FG routing or detection, verify that label, multiplier, base FPS, and output FPS all move together instead of leaving a stale mixed state on screen.
- When a visible label is stale but `PublishOverlayFGMetrics()` is publishing the wrong runtime, fix the upstream runtime-state ownership/heuristic boundary instead of papering over the label mapping.
- If a runtime path can produce stale status for even a few frames, add a regression test before assuming the behavior is acceptable.

## Open Questions / Stale-Risk
- Stale risk is medium to high because publication correctness depends on runtime classification and call ordering across multiple DX12 paths.
- Re-check this page after any change to `ResolveOverlayFGMetricType()`, `PublishOverlayFGMetrics()`, or DX12 overlay routing that publishes FG state.
