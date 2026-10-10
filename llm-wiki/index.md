# llm-wiki Index

Last cross-checked: 2026-10-02 (rewritten as a routing table; per-page detail lives in each page's own header;
the previous paragraph-per-page catalog is in git history before commit "docs: compact agent docs").

`llm-wiki` is derived documentation for agents and maintainers, not the implementation: when wiki and
code/tests/build scripts disagree, the code wins - fix the page and note it in `log/recent.md`.

## How to read

1. Code location: `repo-map.md`. Session logs: `python tools/log_digest.py <session-dir>` first.
2. One or two topic pages from the tables below. **KB** is the page size (2026-10-02): pages over ~60 KB
   cost 15k+ tokens - read their headings first and jump to the section you need.
3. `log/recent.md` for what changed in the last days; archives (`log/archive-*`, `log/README.md`) only for history.

DX12 overlay/injection/FG work needs, in order: `dx12-injection-bootstrap.md`,
`dx12-overlay-third-party-coexistence.md`, `frame-generation/guardrails.md` (search it, do not read it whole),
`overlay-fg-status.md`, `present-interposers.md`; case studies only for precedent.

## Workflow and code

| Page | KB | What it answers |
| --- | ---: | --- |
| `repo-map.md` | 12 | where each subsystem lives, binaries to sources, build units, high-risk areas |
| `refactor-roadmap.md` | 9 | refactor rules (mechanical vs behavioral waves, fingerprint proof), wave status, controller C API and media DLL ABI contracts (2026-10-05), library plan, log-volume evidence |
| `refactor-contracts.md` | 45 | implemented core ownership, glossary, source/test anchors, locality comparison and pending validation (2026-10-05) |
| `architecture-debt-plan.md` | 42 | parallel graphics/media/build debt and hardware validation; D8/D9/D13 superseded by library plan (2026-10-08) |
| `library/README.md` | 4 | canonical library-first plan and M0-M11 milestone status (adopted 2026-10-08) |
| `library/02-target-architecture.md` | 17 | runtime/client topology, deep module boundaries, threading and process effects |
| `library/03-public-api.md` | 26 | versioned C API and C++ wrapper specification; design, not shipped API |
| `library/04-runtime-modules.md` | 24 | runtime module contracts, migrations and interface tests |
| `library/05-frontend-and-role-host.md` | 8 | frontend conversion, role dispatch and migration hazards |
| `library/06-milestones.md` | 15 | ordered commit series, acceptance criteria and gates |
| `library/07-testing-and-verification.md` | 8 | interface/ABI/external-client tests and depth metrics |
| `library/08-decisions-and-open-questions.md` | 10 | library decisions, rejected options and prerequisite evidence questions |
| `library-delivery.md` | 8 | source-backed v1/child/config prerequisites; delivery order superseded by library plan (2026-10-08) |
| `architecture-inventory.md` | 12 | D0 module inventory, bounded ownership traces, repeatable coupling measurements and exact pending evidence (2026-10-06) |
| `frame-generation/ngx-flow-lifecycle.md` | 6 | D1 intercepted NGX fake, resource/status scenarios, production mutations and pending cold-start/SDK retirement evidence (2026-10-06) |
| `dx12-queue-dispatch.md` | 7 | private queue/device/Present-vtable dispatch, publication/reset/forwarding, real-hook regressions and pending provider/callback lifetime work (2026-10-07) |
| `build.py.md` | 89 | build/gate flags, lint/sanitizer/clang-tidy ratchets, toolchain, dependency provenance, stable releases |
| `codestyle.md` | 8 | style and tooling rules, no whole-file formatters, split rules |
| `changelog-guidelines.md` | 8 | CHANGELOG / release-note rules and `tools/manage_changelog.py` |
| `regression-testing-and-logging.md` | 109 | test expectations, hook log line format, log metering conventions (`ChangeGate`, collision/ownership rules verified 2026-10-04), freeze detection |
| `secret-leak-prevention.md` | 11 | mandatory staged/message and exact-commit/metadata secret checks, manual fallback and remediation (2026-10-05) |
| `debug-tools.md` | 24 | log digest, dated recording windows/ingress aliases (2026-10-10), debugger paths, WoW64 dumps, DX12 DIAG, DRED, debug layer |
| `debug-tools-security-audit.md` | 26 | binary/dependency security-audit tooling |
| `known-debt.md` | 9 | accepted debt and falsified audit findings |
| `fuzzing.md` | 6 | libFuzzer harnesses, corpora, `--run-fuzz` |
| `current.md` | 8 | one line per major change up to 2026-09-15 with its topic page (full text archived) |

## Injection, IPC, process model

| Page | KB | What it answers |
| --- | ---: | --- |
| `startup-runtime-overrides.md` | 7 | static Streamline imports, creator-only role, first-resume handoff, physical-path regressions (2026-10-10; actual Steam retest pending) |
| `dx12-injection-bootstrap.md` | 58 | startup/late injection, process discovery, early native loader coverage (2026-10-10), IPC ownership, Vulkan layer membership |
| `process-ipc.md` | 21 | private child channels, shared-memory ABI publication, media stop/finalize, log routing |
| `elevation-and-startup.md` | 8 | elevation service broker, install-folder runtime/migration, UAC ownership, autostart |
| `installer.md` | 9 | setup/uninstaller payload, transactional replace, config.ini handling |
| `configuration.md` | 39 | config sections and aliases, profiles, overlay/sensor selectors, reload, encoding |
| `window-heartbeat.md` | 6 | opt-in 250 ms background window wake-up, profile keys, asynchronous backpressure and unverified game-hang efficacy (2026-10-05) |
| `third-party-dll-loading.md` | 14 | ReShade / OptiScaler / Special K loading by the hook |

## Overlay, frame generation, present path

| Page | KB | What it answers |
| --- | ---: | --- |
| `overlay-rendering.md` | 94 | overlay layout/graph/font, DX12 uploads, PC latency, sensors (LHM), DirectDraw/D3D7 paths, HDR; presentation binding (2026-10-09) |
| `dx12-overlay-third-party-coexistence.md` | 132 | foreign hook chains (Steam, RTSS, ReShade...), deep/bypass interception, FSR topmost ordering |
| `present-interposers.md` | 21 | NVIDIA Smooth Motion topology and where the overlay goes |
| `overlay-fg-status.md` | 24 | visible FG status publication across DX11/DX12/Vulkan |
| `frame-generation/guardrails.md` | 241 | FG switching invariants (DLSS-G/FSR FG/PostSL/FFX); search, do not read whole |
| `frame-generation/case-studies.md` | 202 | FG incident case studies with evidence |
| `frame-generation/dlss-driver-settings.md` | 15 | DLSS-G DRS keys (`dlss_fg_*`), indicator decoding |
| `frame-generation/streamline-generation-bridge.md` | 30 | `streamline_upgrade`: measured Streamline 1.x ABI, 1.x->2.x bridge |
| `frame-generation/streamline-generation-bridge-runs.md` | 39 | per-run evidence for the bridge (Witcher 3) |
| `frame-generation-switching.md` | 1 | stub: FFX create-entry pins, points to guardrails |
| `vulkan-fg-switch-test.md` | 14 | Vulkan DLSS/FSR switch test app architecture |
| `vulkan-forced-fifo.md` | 43 | `vsync_mode=fifo` under Vulkan, present metering |
| `pseudo-overlay.md` | 25 | GDI pseudo overlay for WGC capture |
| `handoff-dx12-32bit-crash.md` | 12 | fixed 32-bit DX12 overlay crash: trigger, fix, invariants |

## Graphics overrides, pacing, timing

| Page | KB | What it answers |
| --- | ---: | --- |
| `graphics-overrides-and-frame-pacing.md` | 69 | cross-API sampler/config semantics, DirectDraw presentation overrides |
| `frame-pacing-and-limiter.md` | 40 | FPS limiter, present depth, front-loaded release |
| `display-change-timing.md` | 60 | ETW screen-change timing, NVIDIA scheduled flips, Intel/AMD correlation |
| `cross-api-forced-af.md` | 14 | D3D9/D3D6-8/D3D10/OpenGL forced AF and state blocks |
| `dx11-forced-af.md` | 8 | D3D11 forced AF |
| `dx12-forced-af.md` | 5 | D3D12 sampler policy |
| `post-processing-sharpen.md` | 24 | CAS/RCAS sharpen + generic display gamma on D3D11/D3D12/Vulkan |
| `ue5-cvar-overrides.md` | 55 | `[UE5]` CVar resolution, RR preset ladder, refusals |
| `graphics-api-reporting.md` | 4 | graphics API detection and reporting |
| `performance-priority.md` | 10 | process/queue priorities, HAGS, MMCSS |

## Capture, audio, encoding, output

| Page | KB | What it answers |
| --- | ---: | --- |
| `cfr-capture-sync.md` | 148 | CFR and A/V invariants for WGC/DXGI/inject, overload pacing, finalize |
| `wgc-capture.md` | 118 | WGC and DXGI duplication backends, capacity attribution, privacy blackout |
| `multi-audio-capture.md` | 87 | system/mic/app capture, epochs, recovery, mixing, target-relative backlog warnings (2026-10-10) |
| `d3d9-capture.md` | 8 | native D3D9 capture and its limits |
| `screenshots.md` | 18 | screenshot requests, HDR/SDR classification, publication |
| `recording-output-paths.md` | 11 | output paths, staging/publication, I/O timeouts |
| `nvenc-encoding.md` | 11 | NVENC policy and FFmpeg patches |
| `hardware-encoding.md` | 9 | AMF, Quick Sync, Media Foundation |
| `live-streaming.md` | 7 | RTMP/RTMPS streaming |
| `face-camera-overlay.md` | 11 | webcam overlay ingest and composition |

## Page maintenance

- Each topic page keeps `Last cross-checked`, primary sources, facts/invariants, and open questions / stale-risk.
- Topic pages hold current understanding; chronology goes to `log/recent.md`. Keep this index one row per page;
  add a row when a page is created and update the KB column when a page grows a lot.
- Code, tests, config and build scripts outrank plan documents and old comments.
