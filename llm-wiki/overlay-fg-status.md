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
- The main DXGI/DX12 publication call sites still route through the planner-aware `PublishOverlayFGMetrics(..., FGActionPlan, ...)` helper rather than each caller independently deciding `(effectiveFGActive, runtimeMode)` ad hoc.
- That planner-aware helper now first asks DX12 for the latest preferred visible FG state. This lets the shared publisher keep using a common entrypoint while avoiding stale planner repaints when DX12 already computed a newer user-visible state.
- The preferred visible-state cache and the planner now share one publication-order sequence. When they disagree, the publisher lets the newer state win instead of unconditionally trusting the preferred cache. This preserves the old stale-plan fix while allowing a later planner `off` update to beat an older cached `DLSS FG` / `FSR FG` label during menu churn or shutdown-adjacent sequences.
- `DX12::ProcessFrame` still computes the most authoritative per-frame visible FG state (`currentFGActive`, `currentRuntimeMode`) and publishes it directly. It now also seeds the shared preferred-publication cache so later planner-based refreshes cannot repaint an older label after `ProcessFrame` already corrected it.
- `DX12_OnStreamlineFGStateChanged` and `DX12_RenderOverlayViaFFXPresentCallback` also seed that preferred-publication cache, so immediate transition/callback publications can reflect the latest visible state even before the next `ProcessFrame`.
- When the planner disagrees with DX12's preferred visible state, the shared helper now emits `FG publication preferred override: ... winner=... planner_sequence=... preferred_sequence=...` so future stale-label traces show exactly which refresh path tried to repaint an older state and which side was newer.
- That helper uses `ResolveOverlayFGMetricType()` to map `(effectiveFGActive, runtimeMode)` into the published FG type.
- If frame generation is active but the resolved published type is `0`, the helper logs an invariant violation.
- When FG is active, the helper currently publishes a multiplier of at least `2`.
- When FG is inactive, the helper resets published output FPS and base FPS to `0.0f` and resets the multiplier to `1`.
- The helper tracks the last published state and only emits the main publication log when the visible state actually changes.
- A stale visible FG label can still originate upstream from runtime classification drift even when `PublishOverlayFGMetrics()` itself behaves correctly. Talos `installed/captureengine/logs/20260416_192846` showed this explicitly: CE first published `runtime=STREAMLINE_NO_FG active=0`, then immediately re-detected heuristic `FSR_FG` from a lingering runtime-owned queue after `ffxConfigure(frameGenerationEnabled=0)` had already disabled the native FSR present-callback bridge.
- The current tree now treats an explicit native-FSR `frameGenerationEnabled=0` signal as authoritative during runtime-owned teardown: heuristic `FSR_FG` reactivation is suppressed until the runtime-owned swapchain ownership actually unwinds or native FSR explicitly turns back on. That keeps the visible overlay status from snapping back to stale `FSR FG` after a real `FSR_FG -> off` transition.
- Talos `installed/captureengine/logs/20260423_215138` and the immediate build-`0.1.2565` rerun `installed/captureengine/logs/20260423_220858` showed a different upstream family: publication freshness was already correct, but CE missed the final authoritative `DLSS FG -> off` edge entirely because `slDLSSGSetOptions` inline hooking failed in that session and the first fallback only covered the core Streamline DLLs, not the later owner module backing the actual feature export.
- The current tree now keeps the key Streamline FG exports reachable through four interception seams: inline hook on the export itself, wrapper substitution through `slGetFeatureFunction`, dynamic lookup hooks for direct `GetProcAddress` requests, and an owner-module direct-import fallback armed from the real returned feature-export pointer when export-inline patching fails. That means an explicit menu-side OFF can still clear the visible overlay state immediately even if the export-inline hook failed in that process.
- Current tests explicitly require:
  - `FSR FG` to publish as `FSR FG`
  - `DLSS FG` to publish as `DLSS FG`
  - switching between those modes to update the visible label immediately
  - transitioning back to `off` to clear the published FG status back to the baseline `FG` label
- `tests/test_overlay_fg_status_publication.cpp` now also covers the planner-driven publication overload directly, including both directions of the freshness rule: a stale planner `DLSS FG` publication overridden by the latest DX12-visible `FSR FG` / `off` state, and a newer planner `off` update beating an older cached preferred `DLSS FG` state.

## Practical Guidance
- Route visible FG status publication through the shared helper instead of duplicating mapping logic in multiple runtime paths.
- Treat the visible overlay state as user-facing correctness, not cosmetic best effort.
- When changing FG routing or detection, verify that label, multiplier, base FPS, and output FPS all move together instead of leaving a stale mixed state on screen.
- When a visible label is stale but `PublishOverlayFGMetrics()` is publishing the wrong runtime, fix the upstream runtime-state ownership/heuristic boundary instead of papering over the label mapping.
- When a visible label is stale and the log never shows the final authoritative OFF edge, inspect Streamline feature-hook coverage first (`slDLSSGSetOptions`, `slDLSSGGetState`, current `slReflexSetOptions`, and legacy `slReflexSetConstants`) before revisiting publication-order logic, and verify that the fallback was armed on the actual owner module of the failing export rather than only on `sl.interposer.dll` / `sl.common.dll`.
- Inactive `Off` and inactive `STREAMLINE_NO_FG` both publish FG metric type `0`; do not treat that label-level equivalence as an overlay divergence. Use `DoOverlayFGPublishedTypesDiffer(...)` when deciding whether planner-vs-visible publication differences deserve override diagnostics.
- If a runtime path can produce stale status for even a few frames, add a regression test before assuming the behavior is acceptable.

## Open Questions / Stale-Risk
- Stale risk is medium because publication correctness still depends on runtime classification and call ordering across multiple DX12 paths, but the shared preferred-visible-state cache plus shared publication-order sequence now remove the two obvious repaint seams, and the owner-module Streamline direct-import fallback removes the most recent "missed final OFF edge" observation gap from Talos `20260423_215138` / `20260423_220858`.
- Re-check this page after any change to `ResolveOverlayFGMetricType()`, `PublishOverlayFGMetrics()`, or DX12 overlay routing that publishes FG state.
