# llm-wiki Index

Last cross-checked: 2026-07-11 (capture reliability audit plus durable startup-cached FFX export routing, exact owner-queue suspension visibility, protected-breakpoint retirement, and configure-transition dedupe; see wgc-capture.md, cfr-capture-sync.md, multi-audio-capture.md, frame-generation/guardrails.md, and log/recent.md)

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
- `handoff-dx12-32bit-crash.md`
  - Fixed-state pickup note for the 32-bit DX12 inject-overlay crash/freeze: root-cause isolation, solid glyph-span text fix, runtime validation logs, invariants, and source anchors.
- `current.md`
  - Compact current-state summary, current logging model, and token-efficient routing into the longer wiki pages.
- `codestyle.md`
  - Formatter-backed style rules, Python tooling config, and common tree conventions.
- `build.py.md`
  - Supported `build.py` flags, interactions, environment variables, operational notes, and MinGW cross-compile pitfalls (INITGUID/PKEY, `mediaengine/audio_capture.cpp`).
- `repo-map.md`
  - Top-level repo layout and subsystem ownership.
- `dx12-injection-bootstrap.md`
  - Current DX12 injection timing, hook bootstrap, and pseudo-overlay handoff behavior.
- `dx12-overlay-third-party-coexistence.md`
  - Current DX12 overlay coexistence rules for third-party overlays such as Steam, Rockstar Social Club, and Epic EOS.
- `dx11-forced-af.md`
  - Current Blackwell-safe D3D11 forced anisotropic filtering policy, per-context bootstrap, wrapper-context draw path, wrapper/vtable forwarding guard, streamed-SRV warm-up, shader-slot role probation/recovery, runtime enabled gating, candidate-resource registry/negative cache, runtime sampler/SRV tracking, diagnostics, and stale-risk.
- `frame-generation-switching.md`
  - Stub pointing to `frame-generation/guardrails.md` (invariants, including current Streamline startup transport rules) and `frame-generation/case-studies.md` (chronological deep-dive).
- `overlay-fg-status.md`
  - Current visible FG status publication rules for the overlay.
- `debug-tools.md`
  - Available Windows debug tools (cdb, windbg, Sysinternals) and their installed paths.
- `pseudo-overlay.md`
  - Controller-side pseudo-overlay for WGC capture: architecture, modes, foreground detection, process_list config parsing, known pitfalls (Trim charset, `;` comment skipping), debug logging, source anchors.
- `regression-testing-and-logging.md`
  - Regression coverage expectations, useful test files, logging expectations, and the deterministic `dx12_av_sync_test.exe`/stimulus analyzer/runner workflow for risky runtime changes, including the fast zero-drift quick gate, late app-source gate, fullscreen/topmost tear-free dual-lane stimulus capture, source-stall validation, adaptive WGC p5/p6/p7 high-entropy overload gate, real-session stutter/shutdown triage, scenario-local CE log snapshots, and strict system/app A/V checks.
- `multi-audio-capture.md`
  - Multi system audio (`[Audio.N]`), multi microphone (`[Microphone.N]`), and app audio capture, including worker-owned WASAPI lifecycle/reactivation, bounded validated packet delivery, no-wait final drain, and audio-only stop-tail preservation. Last verified 2026-07-11. Stale-risk: medium (runtime device matrices remain necessary).
- `wgc-capture.md`
  - WGC and DXGI Desktop Duplication backends, including callback epochs, atomic capture ownership, transactional retarget/rollback, first-frame-proven inject handoff, source/cursor epochs, exact-target fallback, shared pool/CFR machinery, and hardware-cursor-aware duplication. Last verified 2026-07-11. Stale-risk: medium (real-hardware multi-monitor/HDR/DirectFlip validation remains necessary).
- `cfr-capture-sync.md`
  - Shared WGC/inject CFR and A/V invariants, including inject texture-slot leases through synchronous copy, transactional fresh-frame metadata, scheduled encode-failure repeat recovery, source-epoch cache invalidation, exact stop/tail accounting, and deterministic validation. Last verified 2026-07-11. Stale-risk: medium.
- `performance-priority.md`
  - `[Performance]` config section: `process_priority` (media CPU), `gpu_priority` (CE D3D11 GPU thread), `gpu_scheduling_priority` (OBS-style D3DKMT media process GPU scheduling class), and `copy_queue_priority` (legacy-named D3D12 overlay DIRECT queue priority, not a COPY queue). Includes HAGS/D3D12 COPY queue findings, source anchors, validation needs, and open questions. Last verified 2026-06-30. Stale-risk: medium.
- `recording-output-paths.md`
  - MediaEngine recording output filename/directory behavior, including elevated-process mapped-drive handling by rewriting persistent mapped drive letters to UNC via `WNetGetConnectionW` or `HKCU\Network\<drive>\RemotePath`. Last verified 2026-06-30. Stale-risk: medium.
- `log.md`
  - Stub pointing to `log/recent.md` (recent activity) and `log/archive-YYYY-Www*.md` (weekly archives).

## Page Maintenance
- Each page should keep `Last cross-checked`, `Primary sources`, `Facts`, and `Open questions / stale-risk` current.
- Plan docs and old comments may be useful context, but they are secondary sources. Prefer live code, tests, config, and build scripts.
- Keep `current.md` compact and current. Move detailed history into the topical pages and `log/recent.md` (or the relevant archive) instead of expanding the compact entrypoint indefinitely.
