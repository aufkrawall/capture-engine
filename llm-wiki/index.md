# llm-wiki Index

Last cross-checked: 2026-05-30 (updated: build 0.1.3608 / tests 0.1.3609 - x86 DX12 focus-loss overlay-fence pacing)

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
  - Regression coverage expectations, useful test files, and logging expectations for risky runtime changes.
- `multi-audio-capture.md` (planned)
  - Multi system audio (`[Audio.N]`) and multi microphone (`[Microphone.N]`) capture. Device ID selection via `GetDevice` + friendly-name fallback. Config format, default track numbering, and codec inheritance from `[Audio]`.
- `wgc-capture.md`
  - Windows Graphics Capture device/copy path, explicit 10-bit high-precision policy, CFR startup sync barrier, adaptive overcapture, WGC CFR audio continuity/stop-tail policy, callback locking model, and `[WGC Perf]` telemetry. Last verified 2026-05-07. Stale-risk: medium.
- `cfr-capture-sync.md`
  - Shared WGC/inject CFR timeline and audio sync invariants, packet-level duration diagnostics, stop-drain rules, and the inject timer-rebase debt fix. Last verified 2026-05-13. Stale-risk: medium.
- `performance-priority.md`
  - `[Performance]` config section: `process_priority` (CPU), `gpu_priority` (encoder GPU thread), `copy_queue_priority` (D3D12 overlay queue). Defaults, data flow, source anchors, and open questions.
- `log.md`
  - Stub pointing to `log/recent.md` (recent activity) and `log/archive-YYYY-Www*.md` (weekly archives).

## Page Maintenance
- Each page should keep `Last cross-checked`, `Primary sources`, `Facts`, and `Open questions / stale-risk` current.
- Plan docs and old comments may be useful context, but they are secondary sources. Prefer live code, tests, config, and build scripts.
- Keep `current.md` compact and current. Move detailed history into the topical pages and `log/recent.md` (or the relevant archive) instead of expanding the compact entrypoint indefinitely.
