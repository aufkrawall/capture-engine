# Frame Generation Switching

Last cross-checked: 2026-04-11

Primary sources:
- `AGENTS.md`
- `tests/test_dx12_fg_trace_replay.cpp`
- `tests/test_overlay_fg_status_publication.cpp`
- `hook/common/dx12_overlay_policy.h`

## Scope
This page records current guardrails and tested transition families for no-FG, DLSS FG, and FSR FG switching. The goal is generic support across games, not a pile of title-specific hacks.

## Project Guardrails
- We must be careful not to break DLSS FG and FSR FG in GTA again when fixing any FG issue in Talos.
- Talos and GTA are validation families, not excuses for game-specific runtime hacks.
- Switching FG must work gracefully in both Talos and GTA validation scenarios, in all directions and combinations, without crashes, without lost overlay rendering, and with the correct visible FG status.
- Fixes must stay generic so DLSS FG and FSR FG support scales across multiple games instead of accumulating title-specific branches.

## Facts
- Current trace-replay tests explicitly cover:
  - Talos-style `off -> DLSS -> off -> FSR`
  - GTA-style `FSR -> DLSS` without a clean `off`
  - GTA-style DLSS suspension and resume during loading
  - startup coexistence using startup bypass first and normal routing after settle
- Current overlay status tests explicitly cover visible label switching between `FSR FG` and `DLSS FG`, plus clearing back to `FG` when frame generation turns off.
- Current DX12 overlay policy code treats a runtime-owned swapchain alone as insufficient proof of FSR FG. That matters because GTA can keep a runtime-owned swapchain during non-FSR windows, and misclassifying that state can break post-FSR or post-DLSS recovery.
- Current DX12 overlay policy comments repeatedly preserve queue and recovery choices specifically to avoid Talos device-removed paths and stale-overlay teardown behavior during mixed FSR and DLSS transitions.
- Effective FSR runtime mode is now also used as the guard for SL routing suppression. That prevents a stale SL hook from re-activating Present routing after FSR takeover.
- Runtime-owned native FSR FG must not use the old separate injected DX12 `FG overlay SUBMIT` path on the FFX-owned queue. That path was the one observed freezing in GTA V Enhanced.
- Native FSR FG now has a runtime-cooperative path: CE chains FFX's own `presentCallback` from `ffxConfigure()` and renders the overlay on the FFX-supplied command list / output surface instead of issuing a separate injected queue submission.
- The FFX callback bridge preserves the runtime's own default composition callback when the game does not provide one, then renders CE's overlay on top of the callback's output surface.
- Short-run GTA V Enhanced validation in `installed/captureengine/logs/20260411_233821` confirms the current runtime-owned native FSR path no longer reproduces the immediate crash with the overlay active: the log shows authoritative FFX takeover, `FG publication` switching to `runtime=FSR_FG active=1`, repeated `DX12: FFX present callback rendered overlay on runtime-owned FSR path` entries, and no non-zero device-removal, watchdog-fire, or crash markers.
- That confirmation is still scoped to a short manual run. It proves the current approach survives the previously failing GTA V Enhanced overlay + FSR FG case, but it is not yet a long-soak guarantee.
- GTA V Enhanced `FSR -> DLSS` can drive native FFX teardown through `ffxDestroyContext()` while the inline-hook trampoline is still in use. The `20260412_011505` crash bundle showed the destroy trampoline at `00007FF8011D0300` copying `test rcx, rcx; jne +0x0B` from `amd_fidelityfx_dx12.dll` without relocating that short branch. When GTA destroyed the FFX context during the handoff, the taken `jne` landed inside the trampoline's appended `FF 25` jump stub data and crashed with a null write at `00007FF8011D031B`.
- Inline-hook trampolines must therefore treat short control transfers that leave the copied block as relocations, not raw memcpy. This is a generic trampoline correctness invariant, not a GTA-specific rule.
- GTA V Enhanced DLSS FG startup can route `CreateSwapChainForHwnd` through EOS startup-overlay frames even when the live swapchain/queue actually belongs to Streamline. In `installed/captureengine/logs/20260412_014238`, CE classified the new DLSS startup swapchain for the game HWND as an EOS third-party overlay swapchain and skipped normal swapchain side-effects, then later saw `OFF->ON->OFF->ON` DLSS FG churn followed by `Present STALLED` warnings. The generic fix is to let Streamline frame-generation stack evidence override startup-overlay caller identity during create-swapchain classification, just as native FFX stack evidence already does for FSR.

## Current Practical Guidance
- Any FG fix should be reasoned through as a state-transition problem, not as a one-off title quirk.
- If a change touches queue ownership, runtime classification, startup bypass, post-FSR recovery, or metrics publication, assume both Talos-style and GTA-style regressions are possible until proven otherwise.
- Preserve generic signals such as runtime mode, queue provenance, swapchain ownership, and stack evidence. Do not replace those with per-title conditions.
- If a change would make the correct path depend on the executable name, stop and justify that carefully before proceeding.

## Regression Families Already Present
- `off -> DLSS -> off -> FSR`
- `FSR -> DLSS -> off`
- `DLSS suspend -> DLSS resume`
- `startup bypass -> normal routing`
- `FSR FG <-> DLSS FG` visible overlay label switching
- `DLSS FG -> off` visible overlay reset
- `native/runtime-owned FSR FG` must not emit injected `FG overlay SUBMIT` traffic on the FFX-owned queue
- `native/runtime-owned FSR FG` should render via the chained FFX `presentCallback` path instead of separate injected overlay queue work

## Regression Families Worth Expanding
- `off -> FSR -> off`
- `off -> DLSS -> off` with more routing detail than the visible-status tests currently cover
- `DLSS -> FSR`
- `DLSS -> FSR -> off`
- `FSR -> DLSS -> FSR`
- `FSR off while third-party startup overlay windows are still active`
- `heuristic FSR takeover while SL hook remains present`
- `FSR -> DLSS` with native FFX context destruction while CE inline hooks are active
- `startup-blocking overlay wraps DLSS FG swapchain creation`

## Open Questions / Stale-Risk
- Stale risk is high because FG switching behavior is spread across runtime classification, queue routing, startup coexistence, and visible metrics publication.
- `installed/captureengine/logs/20260411_233821` is only a short validation pass; longer GTA V Enhanced soak coverage is still worth re-checking after future queue-routing or callback-bridge changes.
- Any future inline-hook / trampoline rewrite can silently re-break native FFX teardown paths even if the higher-level FG state machine looks correct. Re-check `hook/wrappers/inline_hook.cpp` whenever a crash lands inside the `00007FF8011D....` trampoline pool.
- Re-check this page after changes to `dx12_overlay_policy.h`, FG transition tests, or overlay metrics publication behavior.
