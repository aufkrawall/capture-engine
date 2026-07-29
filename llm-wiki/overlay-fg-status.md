# Overlay FG Status

Last cross-checked: 2026-07-29

Primary sources:
- `hook/common/overlay_metrics_publisher.cpp`
- `hook/common/overlay_metrics_planner_publisher.cpp`
- `hook/common/overlay_fg_metric_policy.h`
- `hook/common/dx12_overlay_policy.h`
- `hook/apis/dx11_hook.cpp`
- `hook/vulkan_layer/layer_overlay.cpp`
- `build.py`
- `hook/common/streamline_runtime_policy.h`
- `hook/apis/streamline_hook.cpp`
- `tests/test_overlay_fg_status_publication.cpp`
- `tests/test_streamline_runtime_policy.cpp`

## Scope
This page records how the current tree publishes visible FG status to the overlay.

## Facts
- The current shared helper for FG publication is `PublishOverlayFGMetrics()`.
- `PublishDetectedOverlayFGMetrics()` is the direct-render-path adapter: it snapshots the existing detector's active
  state, runtime mode, output/base FPS, and multiplier, then delegates to the same canonical publisher. It does not
  alter detection or runtime priority.
- NvPresent module detection disables the FG detector's default dormant mode. This is necessary because Smooth
  Motion has no DLSS/FFX API activation to wake pattern analysis, and its active state requires the existing
  confirmed 2x frame pattern rather than module presence alone. Strange Brigade DX12 session `20260729_182021`
  exposed the old contradiction by logging NvPresent followed by dormant pattern suppression and permanent
  `runtime=Off`.
- Strange Brigade rerun `20260729_183342` showed that NvPresent-generated DX12 frames are low-work rather than
  necessarily zero-work: steady sampling reported 56 total frames, 31 high-work frames, 130 output FPS, and 72
  high-work/base FPS, while individual generated Presents still had two command lists. NvPresent-specific 2x
  inference therefore accepts only a substantial two-population window near 2x (minimum 30 total and ten frames in
  each population, ratio 1.6x-2.4x) before the existing multi-sample confirmation can activate the label.
- Smooth Motion pattern evaluation is blocked by active Streamline/DLSS/FSR evidence, but not by an idle Streamline
  module. It continues while Smooth Motion is classified active, allowing a broken or superseded pattern to clear
  rather than latch indefinitely.
- Direct DX11 processing invokes that adapter before `ProcessDX11FrameWithOverlayOrdering()`. This covers the
  DX11-owned Present/wrapper paths even when the outer shared DXGI publisher is not the active route.
- Vulkan invokes the same adapter immediately after updating its `PerformanceMetrics` and before recording/waiting
  for the overlay draw. The Vulkan layer explicitly links the generic publisher object; it does not link the
  DX12 planner-specific publisher.
- DX12 retains its existing transition-aware direct/planner publication paths. Splitting the planner overload into
  `overlay_metrics_planner_publisher.cpp` does not change its preferred-state sequence or stale-plan arbitration.
- Visible type resolution now lives in `overlay_fg_metric_policy.h`. DX12's compatibility entrypoints delegate to
  that API-neutral mapping, so DX11, DX12, and Vulkan publish the same `DLSS FG`, `FSR FG`, `NVIDIA SM`, and inactive
  metric types.
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
- GTA `installed/captureengine/logs/20260715_141520` showed another upstream ownership failure: after repeated stable DLSS-G menu suspend/resume cycles, the generic startup stale-OFF proof latch suppressed a real inactive GetState plus every matching SetOptions OFF. Streamline reported `REFLEX-NOT-DETECTED` while CE kept DLSS-G logically ON, so status and routing could not mirror the runtime. Supplied post-FSR session `20260717_235919` proved the same mismatch can begin immediately after the first confirmed PostSL render while CE is still settling. A game-input Reflex deactivation now arms a one-shot authoritative OFF after that first confirmation; the next active-to-inactive runtime edge consumes it even during startup bookkeeping, while pre-confirmation Reflex churn remains protected. This decision uses the incoming game signal, not CE's optional forwarded manual-limiter interval.
- The current tree now treats an explicit native-FSR `frameGenerationEnabled=0` signal as authoritative during runtime-owned teardown: heuristic `FSR_FG` reactivation is suppressed until the runtime-owned swapchain ownership actually unwinds or native FSR explicitly turns back on. That keeps the visible overlay status from snapping back to stale `FSR FG` after a real `FSR_FG -> off` transition.
- Talos `installed/captureengine/logs/20260423_215138` and the immediate build-`0.1.2565` rerun `installed/captureengine/logs/20260423_220858` showed a different upstream family: publication freshness was already correct, but CE missed the final authoritative `DLSS FG -> off` edge entirely because `slDLSSGSetOptions` inline hooking failed in that session and the first fallback only covered the core Streamline DLLs, not the later owner module backing the actual feature export.
- The current tree now keeps the key Streamline FG exports reachable through four interception seams: inline hook on the export itself, wrapper substitution through `slGetFeatureFunction`, dynamic lookup hooks for direct `GetProcAddress` requests, and an owner-module direct-import fallback armed from the real returned feature-export pointer when export-inline patching fails. That means an explicit menu-side OFF can still clear the visible overlay state immediately even if the export-inline hook failed in that process.
- Talos `installed/captureengine/logs/20260425_002642` showed why the owner-module fallback also has to be discovered from loaded feature DLLs, not only from a later intercepted feature lookup: `slDLSSGSetOptions` inline patching failed, `slDLSSGGetState` stayed active in the 2D menu, and no explicit OFF reached CE while the game setting was already off. `hook/apis/streamline_hook.cpp` now scans all loaded `sl.*.dll` modules, including feature owners such as `sl.dlss_g.dll`, and retries direct-import fallbacks on later module scans. `hook/wrappers/iat_hook.cpp` now no-ops already-patched import slots so these retries are idempotent and cannot corrupt the saved original function pointer.
- Talos `installed/captureengine/logs/20260425_173428` showed a different stale-visible-state route during `FSR FG -> DLSS FG` in the 2D menu: CE did publish DLSS FG internally from `slDLSSGGetState`, but the overlay disappeared because PostSL stayed pending with `safeBootstrap=0`. The current DX12 policy now considers the fresh runtime-owned Streamline swapchain queue safe when it is also the live command queue and CE has a tracked ECL submit path for that queue. Fresh authoritative Streamline handoffs also become pending immediately, so the queue-change heuristic cannot briefly repaint the handoff as `FSR_FG` before DLSS FG wins.
- Talos `installed/captureengine/logs/20260425_181251` showed no final authoritative OFF evidence after a menu-side `DLSS FG -> all FG off` request: Streamline `GetState` remained active and PostSL kept submitting, so publication correctly retained the last known DLSS FG runtime state. The current diagnostics now log fresh `sl.*.dll` load inspection, `slGetFeatureFunction` lookup outcomes, returned-wrapper fallback use, proactive hook gaps, and sampled `slDLSSGGetState` options/state so the next repro can distinguish a missed hook seam from the game simply delaying the real Streamline OFF call until 3D rendering resumes.
- Talos `installed/captureengine/logs/20260425_191325` proved the next upstream stale-label seam: after a captured `slDLSSGSetOptions(ON)` on viewport `1`, the later menu-side off surfaced as an authoritative `slDLSSGGetState(optionsMode=off)` with capability and fence evidence on viewport `0`, not as another captured `SetOptions(OFF)`. The Streamline viewport aggregator now clears all cached DLSSG viewport runtime states on successful non-suppressed `SetOptions(OFF)` and on evidence-backed disabled `GetState` readbacks, so a stale active sibling viewport cannot keep the visible overlay label on `DLSS FG` in a 2D menu.
- Current tests explicitly require:
  - `FSR FG` to publish as `FSR FG`
  - `DLSS FG` to publish as `DLSS FG`
  - NVIDIA Smooth Motion to publish as `NVIDIA SM`
  - switching between those modes to update the visible label immediately
  - transitioning back to `off` to clear the published FG status back to the baseline `FG` label
  - NvPresent detection to wake pattern analysis without claiming Smooth Motion from module presence alone
  - Streamline module presence without confirmed FG to remain `STREAMLINE_NO_FG` after NvPresent detection
  - the observed 56-total/31-high-work NvPresent window to infer 2x while startup-skewed, all-high-work,
    insufficient-minority, module-absent, and 3x-shaped windows remain rejected
  - idle Streamline support to permit Smooth Motion confirmation while active Streamline/DLSS/FSR evidence blocks it
- Source-contract coverage requires both the direct DX11 and Vulkan render paths to publish detected status before
  overlay rendering, and requires the Vulkan layer build to link the generic publisher implementation.
- `tests/test_overlay_fg_status_publication.cpp` now also covers the planner-driven publication overload directly, including both directions of the freshness rule: a stale planner `DLSS FG` publication overridden by the latest DX12-visible `FSR FG` / `off` state, and a newer planner `off` update beating an older cached preferred `DLSS FG` state.
- Build `0.1.5249` passed the complete clean verification gate, including x64/x86 hook and Vulkan-layer builds, the
  full native suite, all Python tool self-tests, lint/ratchets, and x64 ASan/UBSan.

## Practical Guidance
- Route visible FG status publication through the shared helper instead of duplicating mapping logic in multiple runtime paths.
- Treat the visible overlay state as user-facing correctness, not cosmetic best effort.
- When changing FG routing or detection, verify that label, multiplier, base FPS, and output FPS all move together instead of leaving a stale mixed state on screen.
- When a visible label is stale but `PublishOverlayFGMetrics()` is publishing the wrong runtime, fix the upstream runtime-state ownership/heuristic boundary instead of papering over the label mapping.
- When a visible label is stale and the log never shows the final authoritative OFF edge, inspect Streamline feature-hook coverage first (`slDLSSGSetOptions`, `slDLSSGGetState`, current `slReflexSetOptions`, and legacy `slReflexSetConstants`) before revisiting publication-order logic, and verify that the fallback was armed on the actual owner module of the failing export rather than only on `sl.interposer.dll` / `sl.common.dll`.
- If a menu-side DLSS FG disable is not visible until leaving a 2D menu, first decide whether the log contains an authoritative disable edge. A successful non-suppressed `slDLSSGSetOptions(OFF)` is authoritative. A disabled `slDLSSGGetState` with options, known capability, and fence evidence is also authoritative enough to clear stale cached Streamline viewport state. A steady active `slDLSSGGetState` in menu rendering is not enough to clear the label by itself.
- After the current DLSS-G epoch has produced its first confirmed PostSL render, a game-input Reflex ON-to-OFF edge followed by an inactive GetState/SetOptions edge is authoritative suspend evidence and must not be reclassified as cold-start churn merely because CE is still settling/stabilizing. A Reflex bounce before PostSL confirmation remains non-authoritative. Keep CE's configured manual Reflex interval separate from this game-state decision.
- For this family, first check for `Streamline Hook: slGetFeatureFunction returned slDLSSGSetOptions`, `Using returned-pointer wrapper fallback`, `slDLSSGSetOptions forwarded/suppressed requested=off`, sampled `slDLSSGGetState observed ... optionsMode=off/on clearAll=...`, and `Cleared ... cached DLSSG viewport runtime state(s)` lines before changing publication logic.
- If a menu-side `FSR FG -> DLSS FG` switch detects DLSS FG but the overlay disappears, inspect the post-FSR bootstrap proof rather than only the label publisher. A fresh runtime-owned Streamline swapchain queue with a tracked submit path and matching live command queue is now valid bootstrap evidence; `safeBootstrap=0` in that shape is stale.
- Inactive `Off` and inactive `STREAMLINE_NO_FG` both publish FG metric type `0`; do not treat that label-level equivalence as an overlay divergence. Use `DoOverlayFGPublishedTypesDiffer(...)` when deciding whether planner-vs-visible publication differences deserve override diagnostics.
- If a runtime path can produce stale status for even a few frames, add a regression test before assuming the behavior is acceptable.

## Open Questions / Stale-Risk
- Stale risk is medium because publication correctness still depends on runtime classification and call ordering
  across the API paths. The direct DX11/Vulkan bridge and NvPresent dormant-mode wakeup are offline-tested; a fresh
  Strange Brigade x64 DX12 rerun plus native DX11 and Vulkan Smooth Motion validation remain required.
- Re-check this page after any change to `ResolveFGMetricType()`, `PublishOverlayFGMetrics()`,
  `PublishDetectedOverlayFGMetrics()`, the Vulkan layer source list, or DX12 overlay routing that publishes FG state.
