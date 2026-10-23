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
| `library/03-public-api.md` | 26 | versioned C API and C++ wrapper specification; `include/cengine/cengine.h` (shipped in `sdk/`) is authoritative |
| `library/04-runtime-modules.md` | 24 | runtime module contracts, migrations and interface tests |
| `library/05-frontend-and-role-host.md` | 8 | frontend conversion, role dispatch and migration hazards |
| `library/06-milestones.md` | 15 | ordered commit series, acceptance criteria and gates |
| `library/07-testing-and-verification.md` | 8 | interface/ABI/external-client tests and depth metrics |
| `library/08-decisions-and-open-questions.md` | 10 | library decisions, rejected options and prerequisite evidence questions |
| `library/09-in-process-engine.md` | 20 | phase 2: engine in the client process by default, helper processes optional (M12-M19), effort and branching |
| `library/10-phase2-audit.md` | 10 | phase 2 audit (M12): role globals, process-wide effects, teardown, two-session collisions, package paths, loopback worker |
| `library/11-helper-isolation.md` | 36 | phase 3 (M20-M26, implemented in code 2026-10-21, hardware runs pending): app back to helper processes, in-process coverage, recording recovery, FFmpeg isolation, per-app audio, client-image helpers (DR-51), native sensors |
| `library-delivery.md` | 8 | source-backed v1/child/config prerequisites; delivery order superseded by library plan (2026-10-08) |
| `architecture-inventory.md` | 12 | D0 module inventory, bounded ownership traces, repeatable coupling measurements and exact pending evidence (2026-10-06) |
| `frame-generation/ngx-flow-lifecycle.md` | 6 | D1 intercepted NGX fake, resource/status scenarios, production mutations and pending cold-start/SDK retirement evidence (2026-10-06) |
| `transaction-and-retirement-invariants.md` | 9 | Present pacing, CPU/GPU retirement, audio/time boundaries, override environment ownership and bounded PE/crash invariants (2026-10-12); live hardware scenarios pending |
| `dx12-queue-dispatch.md` | 23 | private queue/device/Present-vtable dispatch, native translator discovery, frontend Release lifetime, nested caches and refused-slot retry (2026-10-16), real-hook regressions and pending provider/callback lifetime work |
| `build.py.md` | 89 | build/gate flags, lint/sanitizer/clang-tidy ratchets, toolchain, dependency provenance, stable releases |
| `binary-size.md` | 5 | what makes each shipped image big, how to measure it, enforced image size invariants, open size decisions (2026-10-21) |
| `codestyle.md` | 8 | style and tooling rules, no whole-file formatters, split rules |
| `code-structure.md` | 20 | deep modules, dependency and composition rules, test seams, ownership, public C-API rules (libcengine) |
| `changelog-guidelines.md` | 8 | CHANGELOG / release-note rules and `tools/manage_changelog.py` |
| `regression-testing-and-logging.md` | 111 | test expectations, complete log records, shared ring/restart protocol (2026-10-12), log metering ownership (2026-10-04), freeze detection |
| `secret-leak-prevention.md` | 11 | mandatory staged/message and exact-commit/metadata secret checks, manual fallback and remediation (2026-10-05) |
| `debug-tools.md` | 24 | log digest, dated recording windows/ingress aliases (2026-10-10), debugger paths, WoW64 dumps, DX12 DIAG, DRED, debug layer |
| `debug-tools-security-audit.md` | 26 | binary/dependency security-audit tooling |
| `known-debt.md` | 9 | accepted debt and falsified audit findings |
| `fuzzing.md` | 6 | libFuzzer harnesses, corpora, `--run-fuzz` |
| `current.md` | 8 | one line per major change up to 2026-09-15 with its topic page (full text archived) |

## Injection, IPC, process model

| Page | KB | What it answers |
| --- | ---: | --- |
| `startup-runtime-overrides.md` | 31 | static Streamline imports, two-phase takeover, import ownership/publication gates, OTA policy, UTF-8 paths, rollback diagnostics and acknowledged loader-init APC loads (2026-10-14; live retest pending) |
| `dx12-injection-bootstrap.md` | 59 | startup/late injection, provider/ETW handoff, native image notification ownership and loader-return coalescing (2026-10-16), IPC ownership, Vulkan layer membership |
| `process-ipc.md` | 24 | private child channels, child doorbell for command latency (2026-10-18), shared-memory ABI publication, terminal-output worker shutdown (2026-10-12), media stop/finalize, log routing |
| `elevation-and-startup.md` | 8 | elevation service broker, delayed-sample retention (2026-10-13), install-folder runtime/migration, UAC ownership, autostart |
| `installer.md` | 9 | setup/uninstaller payload, transactional replace, config.ini handling |
| `configuration.md` | 39 | config sections and aliases, profiles, overlay/sensor selectors, reload, encoding |
| `window-heartbeat.md` | 6 | opt-in 250 ms background window wake-up, profile keys, asynchronous backpressure and unverified game-hang efficacy (2026-10-05) |
| `third-party-dll-loading.md` | 18 | ReShade / OptiScaler / Special K and custom DLL loading (`custom_dll_paths`, 2026-10-12) by the hook; the OptiScaler `slGetPluginFunction` hook loop and the leave-foreign-entry rule (2026-10-23) |

## Overlay, frame generation, present path

| Page | KB | What it answers |
| --- | ---: | --- |
| `overlay-rendering.md` | 94 | overlay layout/graph/font, DX12 uploads, PC latency, sensors (LHM), delayed-sample retention (2026-10-13), DirectDraw/D3D7 paths, HDR; presentation binding (2026-10-09); uncounted Presents capturable without a generator (2026-10-19) |
| `dx12-overlay-third-party-coexistence.md` | 131 | foreign hook chains, late-follower admission and late injection routing (2026-10-12), deep/bypass interception incl. x86 (2026-10-17), FSR topmost ordering |
| `dxgi-late-overlay-injection.md` | 8 | native entry reservation (x86 since 2026-10-17), late RTSS/DXUP evidence, output admission and restore/re-patch regression (2026-10-12; initial HOTS/RTSS retest reports fixed) |
| `present-interposers.md` | 21 | NVIDIA Smooth Motion topology and where the overlay goes |
| `overlay-fg-status.md` | 24 | visible FG status publication across DX11/DX12/Vulkan |
| `frame-generation/guardrails.md` | 241 | FG switching invariants (DLSS-G/FSR FG/PostSL/FFX), frame-classification queue starvation (2026-10-19); search, do not read whole |
| `frame-generation/case-studies.md` | 202 | FG incident case studies with evidence |
| `frame-generation/dlss-driver-settings.md` | 15 | DLSS-G DRS keys (`dlss_fg_*`), indicator decoding |
| `frame-generation/streamline-generation-bridge.md` | 30 | `streamline_upgrade`: measured Streamline 1.x ABI, 1.x->2.x bridge |
| `frame-generation/streamline-generation-bridge-runs.md` | 39 | per-run evidence for the bridge (Witcher 3) |
| `frame-generation/xefg.md` | 19 | Intel XeFG (native, OptiScaler `FGOutput=XeFG`), OptiScaler `FGOutput=FSRFG` with two FidelityFX images and default FFX input detours: presenter attribution, runtime-output route on XeFG's queue, `XeFG Nx` status, `SetPresentId` limiter site, XeLL, capture, DLSS-G front, FSR resize lifetime, no-UI-resource overlay on the final-batch route (2026-10-24, flow verified, hardware pending) |
| `frame-generation-switching.md` | 1 | stub: FFX create-entry pins, points to guardrails |
| `vulkan-fg-switch-test.md` | 14 | Vulkan DLSS/FSR switch test app architecture |
| `vulkan-forced-fifo.md` | 43 | `vsync_mode=fifo` under Vulkan, present metering |
| `pseudo-overlay.md` | 25 | GDI pseudo overlay for WGC capture |
| `handoff-dx12-32bit-crash.md` | 12 | fixed 32-bit DX12 overlay crash: trigger, fix, invariants |

## Graphics overrides, pacing, timing

| Page | KB | What it answers |
| --- | ---: | --- |
| `graphics-overrides-and-frame-pacing.md` | 72 | cross-API sampler/config semantics, DirectDraw presentation overrides, Vulkan image count and present-wait queue-depth pacing, DLSS/Streamline DLL override loading and effectiveness log (2026-10-19) |
| `nv-lod-spread-fix.md` | 8 | `nv_lod_spread_fix`: GL/Vulkan ICD LOD-spread branch patch, site validation, one-byte write, RTX Remix bridge (split out 2026-10-19; hardware-validated 2026-09-02) |
| `frame-pacing-and-limiter.md` | 56 | FPS limiter, present depth (D3D11 prerender fence stream; D3D10/11 pre-wait flush, wake stats), pre-limiter capture present-release stamps and DLSS-G idle pacing incl. post-FSR output ratio, post-FSR bypass and pre-FSR Streamline outputs as final-output boundaries, the per-game-frame Reflex warmup (2026-10-19), front-loaded release, limiter modes (auto default, fg_fallback retired) and the FSR FG game-thread proxy site (2026-10-18, hardware pending) |
| `display-change-timing.md` | 64 | ETW screen-change timing, independent-session delivery, NVIDIA/Intel/AMD correlation; HOTS latency recovery verified 2026-10-13 |
| `cross-api-forced-af.md` | 14 | D3D9/D3D6-8/D3D10/OpenGL forced AF and state blocks |
| `dx11-forced-af.md` | 29 | D3D11 forced AF; replacement registry, policy rebasing, cost model (thread memos, x86 counters, wrapper deferral; round 2 measured 2026-10-18), static-table deferral rejected, raw draw-hook loss fixed (2026-10-17) |
| `d3d11-context-dispatch.md` | 8 | d3d11.dll rewrites its contexts' embedded function tables (amortized variants, evidence); CE-owned dispatch tables, forwarders, CFG (2026-10-17) |
| `dx12-forced-af.md` | 5 | D3D12 sampler policy |
| `api-translation-overrides.md` | 10 | cross-API override ownership, memoized raw D3D11 AF and its cost lines (raw + wrapper), HOTS/DXUP thread split and audits (2026-10-17) |
| `post-processing-sharpen.md` | 24 | CAS/RCAS sharpen + generic display gamma on D3D11/D3D12/Vulkan |
| `ue5-cvar-overrides.md` | 56 | `[UE5]` CVar resolution, RR preset ladder, refusals; image-load discovery and snapshot handoff (2026-10-16) |
| `graphics-api-reporting.md` | 10 | graphics API detection and reporting; D3D10 vs DXVK D3D11 classifier, swapchain verdict kept on the chain (2026-10-19), D3D11.x evidence callers, D3D12 translator labels, DXUP frontend over Windows' D3D11 (2026-10-22) |
| `performance-priority.md` | 10 | process/queue priorities, HAGS, MMCSS |

## Capture, audio, encoding, output

| Page | KB | What it answers |
| --- | ---: | --- |
| `cfr-capture-sync.md` | 174 | CFR/A/V invariants, native FSR capture and inline post-process retirement (2026-10-14), inject lease covers the encoder GPU read (2026-10-15; polled after AV1 repeats 2026-10-19), capture binding stamp vs reused swapchain address (2026-10-15), DLSS final-output clock causality + in-flight copy fence tail + pair-avoiding horizon playout + pre-limiter inject frames stamped with their present release + inject frame zero by content target with encoder prewarm (2026-10-19), overload pacing, finalize |
| `wgc-capture.md` | 118 | WGC/DXGI duplication, captured-target A/V queue compensation (2026-10-14), capacity attribution, privacy blackout |
| `multi-audio-capture.md` | 87 | system/mic/app capture, epochs, recovery, mixing, protected-target backlog warnings (2026-10-12) |
| `d3d9-capture.md` | 16 | native D3D9 capture and its limits; classic Ex-gate unlock and decoded device lock (2026-10-15; hardware run pending) |
| `screenshots.md` | 18 | requests, HDR/SDR classification, publication; opaque PNGs whatever the source alpha (2026-10-18); FSR failed-signal lifetime, alpha and post-processing WARP regressions (2026-10-13; live hardware pending) |
| `recording-output-paths.md` | 11 | output paths, staging/publication, I/O timeouts |
| `nvenc-encoding.md` | 13 | NVENC policy and FFmpeg patches, pipeline depth vs GPU-queue latency, encoder device bridge (2026-10-16, bridge VALIDATED) |
| `wgc-reservoir-vram-design.md` | 19 | retained NV12/P010 reservoir, resize output contract and camera history lifetime (2026-10-17); hardware runs pending |
| `encoder-auto-selection.md` | 6 | `encoder=auto` default: GPU-aware AV1>HEVC>H.264 choice by real open probes, cache, real-open retry, binding kept across first-frame re-Init (2026-10-19; RTX 5070 probe only) |
| `hardware-encoding.md` | 9 | AMF, Quick Sync, Media Foundation (device ownership, MF input lifetime, QSV depth; 2026-10-16) |
| `live-streaming.md` | 7 | RTMP/RTMPS streaming |
| `face-camera-overlay.md` | 13 | webcam overlay ingest and composition; frame chosen at the slot's content time, inject delay included (2026-10-19) |

## Page maintenance

- Each topic page keeps `Last cross-checked`, primary sources, facts/invariants, and open questions / stale-risk.
- Topic pages hold current understanding; chronology goes to `log/recent.md`. Keep this index one row per page;
  add a row when a page is created and update the KB column when a page grows a lot.
- Code, tests, config and build scripts outrank plan documents and old comments.
