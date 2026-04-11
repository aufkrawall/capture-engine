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

## Regression Families Worth Expanding
- `off -> FSR -> off`
- `off -> DLSS -> off` with more routing detail than the visible-status tests currently cover
- `DLSS -> FSR`
- `DLSS -> FSR -> off`
- `FSR -> DLSS -> FSR`
- `FSR off while third-party startup overlay windows are still active`

## Open Questions / Stale-Risk
- Stale risk is high because FG switching behavior is spread across runtime classification, queue routing, startup coexistence, and visible metrics publication.
- Re-check this page after changes to `dx12_overlay_policy.h`, FG transition tests, or overlay metrics publication behavior.
