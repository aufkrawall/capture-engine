# llm-wiki Index

Last cross-checked: 2026-07-18 (private same-image disposable process-loopback workers, protocol 4 private lifecycle state, transactional cursor advancement, producer/consumer epoch validation, and evidence-complete fatal publication, first-confirmed game-owned Reflex suspend during FSR-to-DLSS startup settling, single-preparation sanitizer/product builds, current-database lint with summary/detail artifacts and managed Python tools, validated object/link caches and deterministic validation-only profiles with unchanged production LTO, incremental/failure-resume builds, capture-sync/CFR phase preservation with variable-cadence fallback, exact shared-memory ABI 36/build isolation, session-scoped logs, telemetry/frame-ring corruption rejection, non-elevating Vulkan registration repair, full-depth Vulkan capture leases, and the existing current graphics/audio/build/security state; see multi-audio-capture.md, frame-generation/guardrails.md, overlay-fg-status.md, current.md, and log/recent.md)

Primary sources:
- `AGENTS.md`
- `build.py`
- `.clang-format`
- `.flake8`
- `pyrightconfig.json`
- `captureengine/*`
- `hook/*`
- `tests/*`

## Purpose
`llm-wiki` is the derived documentation layer for agents and maintainers. It collects repo knowledge that is useful to consult quickly, but it is not the concrete implementation.

## Trust Model
- Always consult `llm-wiki` before non-trivial work.
- Do not blindly trust it, especially after many changes or during active DX12 / FG churn.
- Cross-check important claims against code, tests, build scripts, config files, and current behavior.
- Update the relevant page and `log/recent.md` whenever you confirm drift, fill a gap, or change project behavior.
- Do not conflate the wiki with the project code. If the wiki and the code disagree, the code/tests/build scripts win.

## Recommended Read Order
- Start here to find the right page.
- Read `current.md` next for a compact current-state summary and routing.
- Read `log/recent.md` after that when you need the recent historical genesis for a changing area. For older entries, consult the relevant `log/archive-YYYY-Www*.md` file.
- For build and tooling questions, read `build.py.md` and `codestyle.md`.
- For DX12 overlay, injection, or FG work, read `dx12-injection-bootstrap.md`, `dx12-overlay-third-party-coexistence.md`, `frame-generation/guardrails.md`, `frame-generation/case-studies.md`, `overlay-fg-status.md`, and `regression-testing-and-logging.md`.

## Content Catalog
- `overlay-rendering.md`
  - Shared overlay layout/graph/font invariants, exact-ABI and sanity-checked telemetry, legacy DX8/DX9/DX10/OpenGL cache/state isolation, DirectDraw performance boundary, and runtime-validation stale-risk. Last verified 2026-07-17.
- `handoff-dx12-32bit-crash.md`
  - Fixed-state pickup note for the 32-bit DX12 inject-overlay crash/freeze: proven font-resource trigger, unconfirmed NVIDIA/WoW64 attribution, solid glyph-span text fix, runtime validation boundary, invariants, and source anchors. Last verified 2026-07-16.
- `current.md`
  - Compact current-state summary, current logging model, and token-efficient routing into the longer wiki pages.
- `codestyle.md`
  - Style/tooling rules, the no-whole-file-in-place-formatter constraint for existing sources, Python tooling config, and common tree conventions. Last verified 2026-07-12.
- `build.py.md`
  - Supported `build.py` flags, single-preparation sanitizer/product flow, current-database advisory lint, managed Python lint tools, summary/detail artifacts, validated object/link caches, deterministic non-LTO unit-test/test-app profiles with unchanged production LTO, incremental/resume/no-build workflow, isolated object variants, source-built dependency closure, PE verification, scoped hardening/DLL-search policy, sanitizer/FP/CPU policy, FFmpeg fingerprints, operational notes, and MinGW pitfalls. Last verified 2026-07-17.
- `screenshots.md`
  - Shared ABI 32 screenshot requests, raw payload v2, managed hook worker, strict payload validation, PNG/10-bit 4:4:4 AVIF encoding, color conversion, and atomic publication. Last verified 2026-07-15.
- `process-ipc.md`
  - Restricted private child channels plus exact shared-memory ABI publication/isolation, session log routing, shared-ring integrity, and malformed/incompatible-message rejection. Last verified 2026-07-17.
- `repo-map.md`
  - Top-level repo layout and subsystem ownership.
- `d3d9-capture.md`
  - Native classic-D3D9 device preservation, opportunistic shared-ring probing, D3D9Ex incompatibilities, synchronization/reset lifetime, diagnostics, and GPU-based WGC fallback. Last verified 2026-07-12.
- `dx12-injection-bootstrap.md`
  - Current DX12 injection timing, capture-method/injection independence, active inject-video publication gating, CFG-safe hook bootstrap, and pseudo-overlay handoff behavior. Last verified 2026-07-16.
- `dx12-overlay-third-party-coexistence.md`
  - Current DX12 overlay coexistence rules for third-party overlays such as Steam, Rockstar Social Club, and Epic EOS, including the audited x86 solid-text compatibility boundary. Last verified 2026-07-16.
- `dx11-forced-af.md`
  - Current Blackwell-safe D3D11 forced anisotropic filtering policy: immutable shader/SRV/sampler object caches, dirty-slot-only reconciliation, zero-lock clean draws, broader material coverage, state-boundary handling, diagnostics, and runtime-validation stale-risk. Last verified 2026-07-16.
- `cross-api-forced-af.md`
  - D3D10 creation-time, D3D9/D3D6-8 event-driven, and OpenGL parameter/storage-event forced AF architecture, safety policy, zero-draw-overhead invariants, diagnostics, and legacy runtime stale-risk. Last verified 2026-07-16.
- `dx12-forced-af.md`
  - Conservative-by-default creation-time DX12 sampler policy, early raw/factory-device interception, dynamic/static and precompiled root-signature 1.0-1.2 coverage, per-vtable chaining, diagnostics, and Kena/Blackwell validation requirements.
- `graphics-overrides-and-frame-pacing.md`
  - Cross-API sampler/config semantics, CPU/present queue-depth enforcement, adaptive rational FPS limiting, phase-preserving capture-sync/FG rules, diagnostics, and runtime validation matrix. Last verified 2026-07-17.
- `frame-generation-switching.md`
  - Stub pointing to `frame-generation/guardrails.md` (invariants, including current first-confirmed Reflex suspend and Streamline startup transport rules) and `frame-generation/case-studies.md` (chronological deep-dive). Last verified 2026-07-18.
- `vulkan-fg-switch-test.md`
  - Vulkan DLSS/FSR switch-app architecture, dual FidelityFX SDK constraint, immutable WSI ownership, cross-owner bridge/pre-retirement rules, backend-safe injected FFX hooking, DirectFlip/present-queue and Reflex-pacing boundaries, rendering inputs, diagnostics, and standalone/injected runtime validation. Last verified 2026-07-16.
- `overlay-fg-status.md`
  - Current visible FG status publication rules for the overlay, including authoritative first-confirmed Reflex-driven Streamline suspend edges. Last verified 2026-07-18.
- `debug-tools.md`
  - Available Windows debug tools and paths plus always-on DX12 present/ProcessFrame stage diagnostics, including wrapper-initialization overlap. Last verified 2026-07-17.
- `pseudo-overlay.md`
  - Controller-side pseudo-overlay for WGC capture: architecture, modes, foreground detection, process_list config parsing, known pitfalls (Trim charset, `;` comment skipping), debug logging, source anchors.
- `regression-testing-and-logging.md`
  - Regression coverage expectations, the incremental/clean/resume build decision rule, concise verification diagnostics, and deterministic capture runners, including the fast zero-drift gate, overload/long-soak profiles, HAGS-on 4K120 contention gate, and x86 DX12 solid-text/focus-visibility boundary. Last verified 2026-07-17.
- `multi-audio-capture.md`
  - Multi system/microphone/app capture, including encoder-latency-independent inject CFR source-clock targets, rate-limited worker scheduling evidence, process-loopback protocol 4 descriptor/byte rings with private lifecycle state and transactional cursors, immutable safe producer-format normalization, evidence-complete fatal transport propagation, polling-first activation, render-session recovery, ordered epochs, private disposable CaptureEngine worker isolation/recycle, the 60 ms CFR ingestion reservoir, exact finalization, and route-local compensation. Last verified 2026-07-18. Stale-risk: medium (fresh device/runtime validation remains necessary).
- `wgc-capture.md`
  - WGC and DXGI Desktop Duplication backends, including the max-rate variable-input CFR producer invariant, callback-rate aging, callback epochs, transactional handoff, FP16/R10 3 GB reservoir, startup prewarm, video-memory telemetry, OOM-only fallback, and crash-safe straight-alpha point-sampled single-stream DPI/HDR cursor composition. Last verified 2026-07-15. Stale-risk: medium.
- `cfr-capture-sync.md`
  - Shared WGC/DXGI/inject CFR and A/V invariants, including capture-sync/CFR selection phase preservation with varying-cadence fallback, contiguous screen-grab packet PTS under overload, packet-only video validation, encoder-latency-independent inject audio targeting, max-rate variable-input WGC production, transactional startup, 60 ms audio-ingestion look-ahead, 300 ms WGC/DXGI look-ahead, nearest monotonic source selection, causal inject transport, convergent per-output overload recovery, and exact decoded stop/tail accounting. Last verified 2026-07-17. Stale-risk: medium.
- `nvenc-encoding.md`
  - NVENC hardware-frame ownership, safe CFR repeat encoding, lookahead/multipass/split-AQ/B-reference policy, AV1 S12M metadata safety, and bundled FFmpeg patch invariants. Last verified 2026-07-17. Stale-risk: medium (fresh driver/GPU matrix remains necessary).
- `performance-priority.md`
  - `[Performance]` priorities, adapter-aware HAGS auto resolution, D3D11 priority persistence/readback, capture/encoder MMCSS QoS, and D3D12 queue constraints. Last verified 2026-07-12. Stale-risk: medium.
- `recording-output-paths.md`
  - Shared collision-safe video/audio/screenshot output reservation, identity-owned cleanup, atomic screenshot publication, executable-relative/default paths, and elevated mapped-drive handling. Last verified 2026-07-15. Stale-risk: low.
- `log.md`
  - Stub pointing to `log/recent.md` (recent activity) and `log/archive-YYYY-Www*.md` (weekly archives).

## Page Maintenance
- Each page should keep `Last cross-checked`, `Primary sources`, `Facts`, and `Open questions / stale-risk` current.
- Plan docs and old comments may be useful context, but they are secondary sources. Prefer live code, tests, config, and build scripts.
- Keep `current.md` compact and current. Move detailed history into the topical pages and `log/recent.md` (or the relevant archive) instead of expanding the compact entrypoint indefinitely.
