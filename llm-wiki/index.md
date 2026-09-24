# llm-wiki Index

Last cross-checked: 2026-09-24 (fourth audit: D3D7/D3D8 state blocks in `cross-api-forced-af.md`, explicit audio device + loss latch in `multi-audio-capture.md`, coherent config reload in `configuration.md`, Vulkan fence recovery in `vulkan-forced-fifo.md`, HDR-switch assessment in `recording-output-paths.md`); 2026-09-24 (audit deferrals: D3D9 state blocks + sampler re-arm in `cross-api-forced-af.md`, per-swapchain Vulkan sharpen in `post-processing-sharpen.md`, UTF-8/DBCS config reader and ANSI path inventory in `configuration.md`); 2026-09-24 (third risk audit: source-change handling in `recording-output-paths.md`, endpoint following in `multi-audio-capture.md`, config encoding/reload in `configuration.md`, GL state ownership in `overlay-rendering.md`); 2026-09-23 (reliability audit entries cross-checked against code and regression coverage; crash capture, output publication, IPC, injection, and Vulkan layer membership updates; the no-host Vulkan target list now lives in the registry, see `dx12-injection-bootstrap.md`).

Primary sources:
- `AGENTS.md`
- `CHANGELOG.md`
- `build.py`
- `tools/manage_changelog.py`
- `tools/config/.clang-format`
- `tools/config/.flake8`
- `tools/config/pyrightconfig.json`
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
- If you are about to understand or change code in an unfamiliar area, first
  orient with `repo-map.md` (the code map: semantic units per subsystem, build
  pipeline units, test matrix, important paths) before reading the topic page.
- Read `current.md` next for a compact current-state summary and routing.
- Read `log/recent.md` after that when you need the recent historical genesis for a changing area. For older entries, consult the relevant `log/archive-YYYY-Www*.md` file.
- For build, commit, and tooling questions, read `build.py.md`, `codestyle.md`, and `changelog-guidelines.md`.
- Streamline API generation (1.x vs 2.x) is an ABI precondition for hooking `slSetTag`/`slEvaluateFeature` and for `streamline_dll_path`; see `frame-generation/guardrails.md`.
- DLSS frame generation's driver-settings keys (render preset, NVIDIA Profile Inspector's forced mode / fixed and dynamic multi-frame counts / target frame rate, and the driver VSync key `vsync_mode` has to travel) are in `frame-generation/dlss-driver-settings.md`. It also decodes the on-screen indicator.
- `streamline_upgrade` runs a 2.x runtime inside a 1.x game (DLSS-G on SL1 titles). `frame-generation/streamline-generation-bridge.md` carries the **measured 1.x ABI** - function signatures, feature values, and the `Constants` / `Resource` / `DLSSConstants` / `DLSSSettings` / `DLSSGConstants` layouts - which exists in no public source and cannot be re-derived from documentation. Read it before touching anything 1.x-shaped.
- For DX12 overlay, injection, or FG work, read `dx12-injection-bootstrap.md`, `dx12-overlay-third-party-coexistence.md`, `present-interposers.md`, `frame-generation/guardrails.md`, `frame-generation/case-studies.md`, `overlay-fg-status.md`, and `regression-testing-and-logging.md`.

## Content Catalog
- `changelog-guidelines.md`
  - ADHD-friendly, scannable structure, bold lead-in anchors, and issue transparency standards for `CHANGELOG.md` and GitHub release tag notes. Continuous pre-commit update mandate, allowed categories, and release automation via `tools/manage_changelog.py`. Last verified 2026-09-20.
- `configuration.md`
  - Resource-backed first-run template, task-oriented canonical sections and legacy aliases, optional bounded `[HardwareSensors]` selectors (nine metrics, all defaulting to auto), injected-overlay display-change/presentation frame-time and PC-latency selection, stable fail-closed monitor selection, best-effort fullscreen-focus WGC/DXGI blackout, unlimited named application profiles with separate video-source and DLL-injection policies, window/audio/DesktopOverlay/override routing, the single target-resolved shared-memory config publication that the runtime overlay-visibility toggle rides on, the two-path hotkey delivery (RegisterHotKey plus a low-level keyboard hook for applications that suppress hotkeys with RIDEV_NOHOTKEYS; the hook thread never logs, runs time-critical, re-arms after a late answer and is removed before a crash dump), validation rules, and maintenance invariants. Last verified 2026-09-22.
- `overlay-rendering.md`
  - Shared overlay layout/graph/font invariants, DX12 allocator-coupled mapped glyph-upload ownership, real Streamline PCL plus native marker-enhanced and fallback PC latency built on the sensor-associated present/display pairing and a measured present-return / low-latency-sleep-return frame-begin anchor, optional out-of-process LibreHardwareMonitor CPU/GPU telemetry with deterministic anti-flap sensor selection, zero-means-unreadable rails, dedicated clock rows and the load/sensor color split, plus its packaging/license boundary, actual display-change timing with generated-output/VRR cadence and presentation fallback, effective-monitor inject-overlay DPI scaling, Vulkan layer-created queue loader-data ownership, DXGI/Vulkan presentation-color contracts including secondary DX12 renderer synchronization, HDR10 gamut/transfer and per-monitor paper-white rules, runtime-owned FG UI transitions, exact-ABI telemetry including split-renderer direct-child LUID provenance, legacy cache/state isolation, the no-synthetic-scene native Direct3D 7 sidecar and primitive-cached CPU DirectDraw fallback (no D3D9Ex/readback/GPU wait), per-surface backdrop/write/damage state, presentation-before-publication policy, direct RGB16/RGB24 capture conversion, PQ-alpha performance boundaries, the CE-owned Direct3D 7 texture-binding references that make the sidecar's state-block restore legal, what CaptureEngine actually costs a frame-generation game (GPU starvation measured by board power at a fixed clock, the app-side phase/GPU timers and the `CE_FG_COST_PROBE` per-behaviour kill switch that attribute it), callback-owned FSR ECL transparency, the bootstrap's released DirectDraw footprint, the row-staged CPU composite that keeps both video-memory streams sequential, and runtime-validation stale-risk, plus the application-source Present stream that a proxy-swapchain frame generator (FidelityFX) hides from CE's present hook and the measured-versus-modelled generator hold that depends on it. Last verified 2026-09-16.
- `handoff-dx12-32bit-crash.md`
  - Fixed-state pickup note for the 32-bit DX12 inject-overlay crash/freeze: proven font-resource trigger, unconfirmed NVIDIA/WoW64 attribution, solid glyph-span text fix, runtime validation boundary, invariants, and source anchors. Last verified 2026-07-16.
- `current.md`
  - Compact current-state summary, current logging model, and token-efficient routing into the longer wiki pages. Last verified 2026-09-15.
- `codestyle.md`
  - Style/tooling rules, the no-whole-file-in-place-formatter constraint for existing sources, bounded C++/Python fragment rules, Python tooling config, and common tree conventions. Last verified 2026-09-20.
- `build.py.md`
  - Supported build/gate flags including failed-manifest verification-mode restoration on resume, clean binary packages plus FFmpeg/libiconv corresponding source, content-addressed lint/sanitizer reuse, Windows/Linux toolchain policy, dependency provenance, exact-commit strict-clean stable releases with pinned Actions and scoped tokens, public artifact attestations, privacy-sanitized assets and immediate self-hosted log deletion, operational notes, and MinGW pitfalls. Also what each ratchet actually measures: the clang-tidy baseline now covers compiler diagnostics (`-extra-arg=-w` is gone), the line-length baseline covers what the 800-line ceiling cannot see, and `--verify` is the static gate while `--verify-runtime` is the one that launches anything. Last verified 2026-09-15.
- `screenshots.md`
  - Shared ABI 38 screenshot requests, DirectDraw/Direct3D 7 presentation-boundary capture with independent overlay inclusion, presentation-contract-aware SDR-R10/scRGB/HDR10 classification, native-HDR versus forced-SDR versus default combined HDR-plus-SDR output policy with concurrent encodes and one shared name seed, split-device WGC readback ownership, bounded low-latency 10-bit 4:4:4 AVIF, strict payload validation, placeholder-free atomic publication, explicit result notifications, the asynchronous D3D12 readback that keeps every wait off the present thread, actual-Present-aware PostSL ownership through DLSS suspension, and exact swapchain-resource/queue device validation. Last verified 2026-09-16.
- `process-ipc.md`
  - Restricted private child channels, accept-before-finalize disposable media stops with media-owned completion notification, recording-health publication, internal GUI launch-feedback suppression, Explorer tray recovery, exact shared-memory ABI 38 publication/isolation, session log routing, shared-ring integrity, malformed/incompatible-message rejection, tolerated late `ReloadConfig`/`Ping` replies, and the media reload/config race; the limiter child process is retired. Last verified 2026-09-23.
- `repo-map.md`
  - **Code map**: top-level layout, semantic-unit inventory per subsystem (hook/captureengine/mediaengine/common/testapp/tools), the Python build pipeline units, and important paths. Re-point stale monolithic-file anchors here after splits. Last verified 2026-09-16.
- `d3d9-capture.md`
  - Native classic-D3D9 device preservation, opportunistic shared-ring probing, D3D9Ex incompatibilities, synchronization/reset lifetime, diagnostics, GPU-based WGC fallback, and Vulkan ownership for D3D9 translation/split RTX Remix bridges. Last verified 2026-08-26.
- `dx12-injection-bootstrap.md`
  - Startup and late injection, callback-before-monitor ordering, event-driven WMI process starts with a native `NtQuerySystemInformation` fallback for unelevated runs (no WMI polling query), DllMain-first loader/CreateProcess hooks, duplicate-worker coalescing, cooperative resident deject/reactivation, generation-safe IPC ownership, capture-method/injection independence, cached target-profile publication, UTF-16 remote DLL paths, bounds-checked PE import walking including old-linker name-less IATs, loaded-base relocation of pristine deep/bypass bytes, WARP-only/thread-local temporary DX12 discovery, grouped CFG-safe inline-hook publication/patching, recognized Agility/factory evidence, early continuous FFX/Streamline observation, process-tree-scoped renderer/runtime-override claims, and negotiation-time Vulkan layer membership. Open issue: the no-host target list is written beside the executable but read beside the staged layer DLL, so pre-host Vulkan admission currently fails. Last verified 2026-09-23.
- `dx12-overlay-third-party-coexistence.md`
  - Cross-API foreign hook-chain ownership for ReShade, OptiScaler, Special K, RTSS/Detours and established overlays; loaded-base relocation of pristine x86/x64 bypass code, below-entry-chain deep interception, real DX12 identity preservation for hidden/fully-deep foreign-overlay chains, pre-Phase3 exact-queue app-callback and generic final-ECL-batch FSR topmost ordering with a shared marker renderer plus callback prewarm, explicit baseline-retirement ownership, and single displayed-output FPS/history sampling, plus thread-quiesced patching, proxy exclusions, resident pass-through, and DX12/FG provenance. Last verified 2026-09-14.
- `third-party-dll-loading.md`
  - User-configured ReShade / OptiScaler / Special K loads by the injected hook (`[ThirdParty]` paths, file-or-folder per-bitness resolution), fixed load order, duplicate/proxy suppression, early HookThread placement, and per-tool diagnostics. Last verified 2026-08-13.
- `dx11-forced-af.md`
  - Current Blackwell-safe D3D11 forced anisotropic filtering policy: immutable shader/SRV/sampler object caches, dirty-slot-only reconciliation, zero-lock clean draws, broader material coverage, state-boundary handling, diagnostics, and runtime-validation stale-risk. Last verified 2026-07-16.
- `cross-api-forced-af.md`
  - D3D10 creation-time, D3D9/D3D6-8 event/state-block-driven, and OpenGL
    parameter/storage/cached-bind sampler architecture, shared mip-filter policy, safety rules,
    zero-draw-overhead invariants, SDK-backed legacy ABI coverage, resolved legacy-device AF/mip/cap diagnostics,
    and runtime stale-risk.
    Last verified 2026-09-15.
- `dx12-forced-af.md`
  - Conservative-by-default creation-time DX12 sampler policy, no device mutation hooks when every override is default, complete Agility SDK/factory-device interception, dynamic/static and precompiled root-signature 1.0-1.2 coverage, per-vtable chaining, diagnostics, and Kena/Blackwell validation requirements. Last verified 2026-09-09.
- `post-processing-sharpen.md`
  - AMD FidelityFX CAS/RCAS applied to the presented frame on D3D11, D3D12 and Vulkan:
    the before-capture/before-overlay ordering rule that keeps CE's own overlay unfiltered
    and the recording in agreement with the screen, uniform filtering of generated frames
    under FG and why filtering pre-FG was rejected, the refusal reasons, the linear-values
    question behind `sharpen_color_space`, per-backend submission mechanics, the Vulkan
    `TRANSFER_SRC` negotiation, the rule that a `sharpen=off` teardown must use the
    unlocked body (re-entering the pass mutex froze the RHI thread), why "the queue that
    rendered the frame" is two different D3D12 queues and what a switch between them has
    to be ordered with, what a submitted command list does NOT keep alive, the
    submission-outcome rule that keeps the Vulkan ring from draining on failed submits,
    and the vendored MIT FidelityFX headers. No hardware run yet. Last verified 2026-09-20.
- `graphics-overrides-and-frame-pacing.md`
  - Cross-API sampler/config semantics including normalized mip filtering, native DirectDraw
    Flip/full-surface-Blt overrides and the fixed-refresh-versus-DXGI-VRR boundary, dual NGX
    MFG-factor contracts, Vulkan compute-present overlay compositing and live same-swapchain
    present-family relearning, upstream Streamline FIFO propagation before DLSS-G observes
    swapchain creation, the DLSS on-screen indicator, authoritative NGX/Streamline RR lifecycle
    evidence, NGX foreign-resolver/proxy coexistence, the DRS-sourced DLSS FG render preset, and
    the process-local NVIDIA LOD-spread quality fix (`nv_lod_spread_fix`). Also covers the
    DLSS/Streamline runtime DLL override loader and its no-duplicate-instance/process-tree ownership
    gates, the Vulkan swapchain image-count floor (`backbuffer_count` may raise `minImageCount`, never
    lower it), the two-sided swapchain-lifetime rule, diagnostics, and the runtime validation matrix.
    Queue depth and the FPS limiter moved to `frame-pacing-and-limiter.md`; `[UE5]` console-variable
    overrides to `ue5-cvar-overrides.md`; display-change timing to `display-change-timing.md`; and
    forced Vulkan FIFO to `vulkan-forced-fifo.md`. Last verified 2026-09-15.
- `frame-pacing-and-limiter.md`
  - Producer-queue CPU/present depth enforcement, including typed DirectDraw Flip/Blt completion
    ownership, and the whole FPS limiter: the exact rational QPC/Bresenham cadence grid, the
    `ce::fps_limiter_policy::PresentSite` call-site contract, front-loaded cadence placement with a
    reservation learned from real overruns and a capture-sync opt-out, deterministic FG output-group
    admission and rational group cadence, composed capture/general constraints, explicit
    base-versus-final-output inject semantics, phase-preserving capture-sync recovery, the display
    vertical-blank ceiling as a third simultaneous constraint, and post-gap native Reflex handoff.
    Last verified 2026-09-15; front-loaded placement, its overrun controller, and the vertical-blank
    ceiling need a hardware re-check.
- `vulkan-forced-fifo.md`
  - What `vsync_mode=fifo` has to reach in a Vulkan title: Streamline's Vulkan proxy above the layer, the layer's own present-mode override, and the `VK_NV_present_metering` capability that overrides both for the life of the device. What CE does for a metered generator is exactly two things: state the vertical blank on the WSI's own final DXGI flip (`SyncInterval=1`, `ALLOW_TEARING` cleared - re-armed 2026-09-13 once the present-mode override, not the interception, was identified as what unpaced the batch), and bound the *rendered* rate at `refresh / multiplier` so the batch still fits the panel. Two mechanisms stay retired and why: the present-mode override (NVIDIA's announced flip lead collapses from 6842 us to 141 us) and `VK_EXT_present_timing` relative scheduling (it bunched the metered batch it was meant to bound - 0.43 ms vs 6.91 ms screen-time stddev across a live `vsync_mode` change in one Portal RTX session - and its swapchain flag cost the game NVIDIA's native present path). Also the overlay submission ring that must never pace the game and the compute-composite acquire barrier that has to name the stage its submit waits at. Last verified on hardware 2026-09-14: 2x/3x/4x all pace at 143.6-144.2/s with 269-1062 us screen-time stddev once the rendered-rate ceiling is stated (before it, 41 x 4 = 164 fps on a 144 Hz panel made the generator stop scheduling flips entirely).
- `display-change-timing.md`
  - Event-based `MsBetweenDisplayChange`, NVIDIA scheduled-flip and Intel/AMD frame-type correlation, raw kernel completion timestamps without inferred refresh-grid rewriting, blank cadence as diagnostics only, concurrent shared-ring publication and metric-history safety, exact FSR-tagged pacing windows, PresentStart-to-screen attribution, the controlled Talos 116/107.5-fps launch pair, the phase-dependent injection/graphics-startup collision, WARP-only bootstrap and grouped-hook correction, plus the healthy different-scene validation boundary. Last verified 2026-09-09.
- `ue5-cvar-overrides.md`
  - The whole `[UE5]` section: how CE resolves a UE console variable's real value storage (literal scan, candidate scoring, the three console-object layouts, console-registry resolution), what it refuses to write, and every shipped override - the graduated RR-quality presets, independent RR force policy, typed custom-CVar final precedence, post-processing/sharpen, `t.MaxFPS`, `r.MaxAnisotropy`/`r.VT.MaxAnisotropy`, `r.MipMapLODBias`, display gamma, depth of field, forced DLSS Super Resolution (including the third-party-upscaler levers and screen-percentage quality modes), and the HDR output/luminance/gamut parameters; also the UE 5.6+ full-resolution short-range-AO restore, floor (minimum) semantics for the screen-probe history, the corrected MegaLights sample tier, and the open DLSS-RR `ResponsivityMask` thread - plus the open `ShowFlag.*` force-bit investigation (measured bit maps for UE 5.4.4/5.6.1, what is proven, ordered next steps). Last verified 2026-09-10.
- `frame-generation-switching.md`
  - Stub pointing to `frame-generation/guardrails.md` (invariants, including independent no-callback cleanup on GetState-first post-FSR ownership repair, exactly-one-path PostSL bootstrap submission with shared lifecycle state, single-submit all-transport exact PostSL suspension keep-alive, FSR-off game-swapchain-recovery keep-alive against the late outer SL-FG-OFF teardown, FSR->DLSS prewarmed-handoff keep-live with one-shot exact-proof consumption, warm-resume restoration of the OFF-released DLSS-G proxy queue, eager present-time overlay draw across the DLSS toggle-ON startup bypass window, allocator-coupled PostSL upload ownership plus the upload-slot fence-lifetime pin against ABA reuse, function-scope Streamline feature-query module pins, provisional FFX-startup retirement at every authoritative exit - FFX FG swapchain-context destroy, authoritative Streamline swapchain handoff, Streamline-FG-ON edge, and successful explicit DLSS enable - first-confirmed Reflex suspend, and Streamline startup transport rules) and `frame-generation/case-studies.md` (chronological deep-dive). Last verified 2026-09-13.
- `vulkan-fg-switch-test.md`
  - Vulkan DLSS/FSR switch-app architecture, dual FidelityFX SDK constraint, immutable WSI ownership, cross-owner bridge/pre-retirement rules, backend-safe injected FFX hooking, DirectFlip/present-queue and Reflex-pacing boundaries, rendering inputs, diagnostics, and standalone/injected runtime validation. Last verified 2026-07-16.
- `overlay-fg-status.md`
  - Current visible FG status publication rules across DX11, DX12, and Vulkan, including early capability-clamped publication of a configured MFG factor, NVIDIA Smooth Motion, and authoritative first-confirmed Reflex-driven Streamline suspend edges. Last verified 2026-09-14.
- `present-interposers.md`
  - NVIDIA Smooth Motion and anything else that replaces DXGI for the application: the proxy-swapchain/private-output-chain topology, why a `dxgi!Present` body hook is not a view of the application's chain, where CE's overlay goes, how the generation factor is measured from the two present streams, and why forced vsync cannot reach the display. Stale-risk medium (module-name keyed). Last verified 2026-09-14.
- `debug-tools.md`
  - Available Windows debug tools and architecture-correct paths, how to read a WoW64 dump and why CE's x64 helper has to supply the 32-bit stacks itself, plus always-on DX12 present/ProcessFrame stage diagnostics, including wrapper-initialization overlap. Last verified 2026-09-16.
- `pseudo-overlay.md`
  - Controller-side pseudo-overlay for WGC capture: dedicated UI-thread ownership, the capture-dark handshake that takes the recording-start status off the composited screen before screen-grab capture starts, recording-health/recovery warnings, truthful finalizing/saved/degraded/failed feedback, application-profile setting selection/pinning, instant amber recording-start state, modes, inject handoff, foreground detection/grace, compatibility process_list parsing, diagnostics, and source anchors. Last verified 2026-08-05.
- `regression-testing-and-logging.md`
  - Regression coverage expectations, API-owner-authoritative freeze detection (including live Vulkan-call evidence and worker-pool target logging), latched recording-capacity and legacy overload attribution, full-duration inter-track content-offset detection, epoch-bootstrap liveness diagnostics, runtime backend-transition and recording-scoped/source-relative analyzer attribution, stop-force-drain/live-drift separation, target-relative app-latency classification, first-chance fault recording and termination-context dumps, external-helper exception streams, Steam/FFX exception recovery, the nested incremental/clean/verify decision rule with content-proven analyzer/sanitizer reuse, concise verification diagnostics, deterministic capture runners including zero-drift, overload/long-soak, contention, and x86 compatibility gates, and the central log-privacy funnels (`ce::privacy` account masking + output-path collapse). Last verified 2026-09-23.
- `multi-audio-capture.md`
  - Multi system/microphone/app capture, including recording-sticky bootstrap settlement across ordered capture epochs, truthful bootstrap/rejoin diagnostics, timeline-recovery-safe route-local compensation, encoder-latency-independent inject CFR source-clock targets, rate-limited worker scheduling evidence, process-loopback protocol 4 transport/lifecycle integrity, polling-first activation, render-session recovery, private disposable worker isolation, the adaptive CFR ingestion reservoir, and exact finalization. Last verified 2026-08-01. Stale-risk: medium (fresh device/runtime validation remains necessary).
- `wgc-capture.md`
  - WGC and DXGI Desktop Duplication backends, including recording-level capacity attribution, exact-QPC hardware-pointer history for pointer-only acquisitions, the best-effort fullscreen-focus privacy blackout with warmup-anchored focus verification, post-prewarm barrier refresh and wall-anchored startup, an authoritative copy-pool reserve, grid-aware debt retention, measured sustainable/degraded overload pacing, grid-matched historical-frame recovery with held-repeat fallback on the immutable CFR output grid, timeline-recovery-safe audio, loss-aware copy-pool and actual-backend phase-lock diagnostics, stable single-monitor selection/fail-closed retargeting, HDR/SDR conversion and metadata/chroma, Windows-SDR-white cursor composition, delayed source-pointer ownership, max-rate CFR production, transactional handoff, and the FP16/R10 reservoir. Last verified 2026-08-01. Stale-risk: medium (fresh DXGI runtime cursor-motion validation remains necessary).
- `cfr-capture-sync.md`
  - Shared WGC/DXGI/inject CFR and A/V invariants, including the DOOM Eternal 4K120 Vulkan compute-present validation and no-allocation common wait chain, final-output DLSS multi-frame-generation capture with scheduled-display correlation, recording-epoch clock reset, expiring source-boundary authority, bounded virtual lead, phase-normalized display cadence, texture-lease-safe inject retention, PostSL-ordered suspended base capture, final/base path-transition continuity, composed capture/general limiter constraints, and post-gap Reflex recovery, latched observational capacity health with no settings/timeline mutation, backend-neutral measured overload pacing with repeat-cost probes and inject debt recovery, overload-aware camera/cursor repeats, recording-sticky audio bootstrap across capture epochs, full-duration content correlation, post-prewarm wall-anchored WGC/DXGI startup, authoritative pool reserve, grid-aware debt retention, the adaptive audio ingestion reservoir against consumer-overrun starvation, immutable-grid deep-debt holds plus grid-matched historical-frame recovery, timeline-recovery-safe audio, transition-aware diagnostics, atomic warm-up cancellation, phase-preserving nearest source selection, contiguous screen-grab packet PTS, packet-only validation, bounded source reservoirs, and exact decoded stop/tail accounting. Last verified 2026-09-13. Stale-risk: medium (fresh DLSS 2x/3x/4x, suspend/resume, and sustained-overload driver validation remains necessary).
- `live-streaming.md`
  - Stream-only YouTube/Twitch/custom RTMP/RTMPS output, backend-preserving H.264/AAC compatibility policy, reuse of the CFR/audio timeline, interruptible five-second network I/O, two-second fail-fast encoded queue, endpoint/key redaction, minimal Schannel FFmpeg protocol closure, and runtime-validation boundaries. Last verified 2026-08-31. Stale-risk: medium (fresh authenticated provider/encoder matrix remains necessary).
- `face-camera-overlay.md`
  - Optional USB/webcam overlay: nonblocking latest-frame-only Media Foundation ingest, GPU-resident/CPU-upload transport, one-draw analytic rectangle/rounded/circle composition, SDR/scRGB/HDR10 mapping, output-space placement, dynamic CFR repeat recomposition shared by recording/streaming, and hysteretic overload fallback to fresh-frame-current fully composited repeats. Last verified 2026-09-01. Stale-risk: medium (fresh webcam/driver/GPU overload validation remains necessary).
- `nvenc-encoding.md`
  - NVENC hardware-frame ownership, safe CFR repeat encoding, user-authoritative settings with recording-level capacity/degradation reporting, HDR context/frame/container/header signaling, startup capacity-warning suppression, lookahead/multipass/split-AQ/B-reference policy, native HEVC/AV1 split-frame multi-engine policy, AV1 S12M safety, and bundled FFmpeg patch invariants. Last verified 2026-08-01. Stale-risk: medium (fresh driver/GPU matrix, including a multi-NVENC target, remains necessary).
- `hardware-encoding.md`
  - AMD AMF, modern Intel oneVPL/Quick Sync, and Media Foundation hardware-frame ownership, native rate-control/quality/lookahead options, direct D3D11-to-QSV mapping, backend-neutral HDR context/frame/container/header metadata, diagnostics, and runtime-validation boundaries. Last verified 2026-07-21. Stale-risk: medium (fresh AMD and Intel runtime matrices remain necessary).
- `performance-priority.md`
  - `[Performance]` priorities, `[Overlay]` queue priority, adapter-aware HAGS auto resolution, D3D11 priority persistence/readback, capture/encoder MMCSS QoS, and D3D12 queue constraints. Last verified 2026-07-18. Stale-risk: medium.
- `recording-output-paths.md`
  - Canonical `[Output]` settings, unpublished content-gated video staging/publication, preservation of recordings with committed packets after trailer/close failure, collision-safe audio reservation, placeholder-free screenshot publication, identity-owned cleanup, executable-relative/default paths, and elevated mapped-drive handling. Last verified 2026-09-23.
- `fuzzing.md`
  - libFuzzer harnesses for the config, IPC, and optional hardware-sensor protocol parsers, the fail-closed `build.py --run-fuzz` stage, curated byte-exact seed corpora, MinGW libFuzzer/ASan toolchain wiring, and the always-run regression floor that covers the same boundaries without a fuzz run. Last verified 2026-08-31.
- `known-debt.md`
  - Deliberately accepted debt with its reasoning: the resolved oversized-source ratchet and its future split rules, the clang-tidy baseline now at zero warnings with per-translation-unit counting, concurrent test-suite hygiene, duplicated overlay telemetry, the policy-header rationale for module/test mapping, and audit findings that were checked and falsified. Last verified 2026-08-03.
- `debug-tools-security-audit.md`
  - Security-audit tooling available for binary/dependency inspection, beyond the general debug tools.
- `graphics-api-reporting.md`
  - Graphics API detection and reporting behavior, including DirectDraw/legacy-D3D evidence isolation from internal overlay/capture helpers. Last verified 2026-09-15.
- `log.md`
  - Stub pointing to `log/recent.md` (recent activity) and `log/archive-YYYY-Www*.md` (weekly archives).

## Page Maintenance
- Each page should keep `Last cross-checked`, `Primary sources`, `Facts`, and `Open questions / stale-risk` current.
- Plan docs and old comments may be useful context, but they are secondary sources. Prefer live code, tests, config, and build scripts.
- Keep `current.md` compact and current. Move detailed history into the topical pages and `log/recent.md` (or the relevant archive) instead of expanding the compact entrypoint indefinitely.
