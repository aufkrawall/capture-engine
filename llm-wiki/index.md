# llm-wiki Index

Last cross-checked: 2026-06-30 (updated: WGC uniform playout repeat rescue/attribution)

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
  - Multi system audio (`[Audio.N]`), multi microphone (`[Microphone.N]`), and app audio capture. Config inheritance for codec/quality/layout fields, per-track channel layout resolution, PCM/Opus codec policy, multichannel bitrate scaling, timeline-valid startup, pre-start app-audio bootstrap strictness, late app-source live join, per-track audio cursors, duplicate source routing, sparse started app-source silence padding, encoded-silence handling, AAC/Opus finalization, CFR audio-continuity rules, tiny source-clock-only drift compensation, process-loopback half-packet timestamp bias, process-loopback teardown crash boundary, WASAPI passive telemetry, audio-only render→loopback auto-detect, strict decode/waveform-tail/WGC stop-tail validation, and the A/V sync stimulus matrix for strict system/app tracks plus opportunistic mixed/microphone evidence. Last verified 2026-06-18. Stale-risk: medium.
- `wgc-capture.md`
  - Windows Graphics Capture device/copy path, same-device low-overhead default with dedicated-device fallback, explicit 10-bit high-precision policy, CFR startup sync barrier with reserve-aware render-delay/smoothness handoff and startup reservoir fill budget, WGC copy-pool slot lease/generation ownership, split WGC source/copy VRAM budgeting, surplus copy-pool pressure trim, uniform-playout ownership of source-above-target surplus plus sync-safe repeat rescue, adaptive overcapture, WGC CFR bounded live scheduler/source-selection policy, encoder-limited near-live smoothness mode, sync-delay source/policy hold attribution, source-limited smoothness ceiling triage, overload policy-fault attribution, audio-continuity/exact-stop drain guard, callback locking model, and `[WGC Perf]` / `[WGC CFR CADENCE EVENT]` telemetry. Last verified 2026-06-30. Stale-risk: medium.
- `cfr-capture-sync.md`
  - Shared WGC/inject CFR timeline and audio sync invariants, packet-level and deterministic content-marker diagnostics, sample-exact settled CFR audio cursor logging / zero-drift warnings, track startup/cursor rules, audio-only render-domain auto-detect with explicit sync confidence, video-only delay application for WGC/inject, startup-only WGC smoothness extra delay and reservoir fill budget, WGC retained-slot lifetime invariant, WGC copy-pool pressure trim boundary, WGC uniform-playout versus ingress ownership for source-above-CFR surplus and sync-safe repeat rescue, late app-source live join, sparse app-source silence padding, iterative stop drain, codec-padding finalization, mux/post-mux-probe ownership, process-crash triage, WGC live scheduler rebase with bounded source-selection clamping/fresh-catchup rejection, WGC encoder-limited near-live smoothness mode, WGC source-limited smoothness ceiling triage, inject timer-rebase debt handling, fullscreen/topmost tear-free `dx12_av_sync_test.exe` stimulus captures, planned source-stall classification, real-session stutter/shutdown triage, and the `run_av_sync_matrix.py` fast/full workflow. Last verified 2026-06-30. Stale-risk: medium.
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
