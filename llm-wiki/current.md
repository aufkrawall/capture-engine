# Current State

Last cross-checked: 2026-10-02 (condensed). One line per major change up to 2026-09-15, newest first, with the
topic page that holds the current rule. The full text of each item is in
`log/archive-current-digest-2026-09-15.md`; anything newer is in `log/recent.md`. Topic pages and code win
over these lines.

## Items

- DirectDraw/D3D7 overlay no longer blocks on the GPU or invents scene boundaries (2026-09-15) -> `overlay-rendering.md`
- Injection startup no longer perturbs hardware D3D12/FSR initialization (2026-09-09) -> `dx12-injection-bootstrap.md`, `display-change-timing.md`
- DLSS final-output capture separates display cadence from common correlation phase (2026-09-01) -> `cfr-capture-sync.md`
- Inject overload recovery uses measured cheap repeats without confusing FG-off source loss (2026-09-01) -> `cfr-capture-sync.md`, `face-camera-overlay.md`
- Extreme encoder loss stays sync-safe and is reported truthfully (2026-08-01) -> `cfr-capture-sync.md`, `wgc-capture.md`, `nvenc-encoding.md`
- DXGI hardware-pointer motion follows its own timestamp stream (2026-07-30) -> `wgc-capture.md`
- Startup reservoir made attainable without weakening overload sync (updated 2026-07-26) -> `wgc-capture.md`, `cfr-capture-sync.md`
- Encoder-overload CFR content sync and transition-aware diagnostics (updated 2026-07-26) -> `cfr-capture-sync.md`, `wgc-capture.md`, `multi-audio-capture.md`
- Screen-grab startup and overload recovery hardened from measured failures (updated 2026-07-27) -> `wgc-capture.md`, `cfr-capture-sync.md`
- Short DXGI UHD120 capture and cursor-transition diagnosis (2026-07-22) -> `wgc-capture.md`, `cfr-capture-sync.md`
- Inject HDR/SDR and overlay presentation contract (corrected offline 2026-07-19) -> `overlay-rendering.md`, `screenshots.md`
- Correct SDR content inside the Windows HDR desktop (corrected and live-validated 2026-07-19) -> `wgc-capture.md`
- Complete HDR recording signaling and Rec.2100 chroma phase (2026-07-21) -> `wgc-capture.md`, `hardware-encoding.md`
- Native-HDR and forced-SDR screenshots (2026-07-19) -> `screenshots.md`
- Native NVENC split-frame multi-engine policy (updated 2026-07-21) -> `nvenc-encoding.md`
- AMF and modern oneVPL/Quick Sync parity plus a stricter Media Foundation fallback (2026-07-21) -> `hardware-encoding.md`
- Single-source configuration and unified application routing (2026-07-19) -> `configuration.md`
- Graceful disposable-media stop and warm-up cancellation (updated 2026-07-19) -> `process-ipc.md`, `recording-output-paths.md`, `cfr-capture-sync.md`
- Internal launch-feedback suppression and Explorer tray recovery (2026-07-18) -> `process-ipc.md`
- Late-join app-audio transport integrity (2026-07-18) -> `multi-audio-capture.md`
- Safe faster cross-host build verification (updated 2026-07-19) -> `build.py.md`
- Capture-sync/CFR phase stability (2026-07-17) -> `cfr-capture-sync.md`, `graphics-overrides-and-frame-pacing.md`
- Exact shared-memory ABI and recording evidence boundary (updated 2026-08-01) -> `process-ipc.md`, `wgc-capture.md`, `overlay-rendering.md`
- Source-relative capture triage (2026-07-19) -> `wgc-capture.md`, `cfr-capture-sync.md`, `regression-testing-and-logging.md`
- AV1 NVENC S12M metadata safety (2026-07-17) -> `nvenc-encoding.md`
- Convergent inject CFR overload recovery (2026-07-17) -> `cfr-capture-sync.md`, `multi-audio-capture.md`
- Targeted inject-overlay polish and legacy hot-path optimization (2026-07-16, DirectDraw superseded 2026-09-15) -> `overlay-rendering.md`
- Vulkan DLSS/FSR FG switch app (implemented, standalone/injected NVIDIA-validated 2026-07-16) -> `vulkan-fg-switch-test.md`, `build.py.md`
- Non-elevating Vulkan layer registration repair (2026-07-17) -> `build.py.md`, `process-ipc.md`
- Vulkan recording producer-depth invariant (2026-07-17)
- All-direction first-output FG coverage (pixel-validated 2026-07-16) -> `frame-generation/guardrails.md`
- Cross-API sampler filtering (2026-07-18) -> `cross-api-forced-af.md`, `dx11-forced-af.md`, `dx12-forced-af.md`
- FSR->DLSS exact proxy-buffer takeover (corrected and pixel-validated 2026-07-15) -> `frame-generation/guardrails.md`
- Exact first-Present ownership across DLSS activation/deactivation, including pointer ABA (corrected and strict-log-validated 2026-07-16) -> `frame-generation/guardrails.md`
- Repeated pure-DLSS first-Present ownership (corrected and strict-log-validated 2026-07-16) -> `frame-generation/guardrails.md`
- CFR audio live-edge protection (2026-07-15) -> `cfr-capture-sync.md`, `multi-audio-capture.md`
- Wrapped-Present DLSS-G suspension and warm-resume continuity (corrected 2026-07-15) -> `frame-generation/guardrails.md`
- FSR->DLSS exact UI coverage and native-return queue ownership (corrected 2026-07-15) -> `frame-generation/guardrails.md`
- GTA repeated-menu DLSS-G pink-tint/one-Present-gap fix (corrected 2026-07-18) -> `frame-generation/guardrails.md`, `overlay-fg-status.md`
- Audit-boundary hardening (updated 2026-08-01) -> `screenshots.md`, `recording-output-paths.md`, `process-ipc.md`
- Classic D3D9 stays classic (2026-07-12) -> `d3d9-capture.md`
- GTA DLSS-FG suspend after FSR history (2026-07-12) -> `frame-generation/guardrails.md`
- GTA FSR-FG overlay owner-queue fix (2026-07-12) -> `frame-generation/guardrails.md`
- GTA FSR-FG missed-create owner-queue recovery (corrected 2026-07-14) -> `frame-generation/guardrails.md`
- GTA FSR-FG re-enable 0.x-FPS fix (2026-07-14) -> `frame-generation/guardrails.md`
- GTA post-FSR normal-return stale-PostSL queue fix (2026-07-14) -> `frame-generation/guardrails.md`
- GTA post-FSR existing-proxy rotation crash fix (2026-07-14) -> `frame-generation/guardrails.md`
- Video source determines normal DLL use (2026-07-19) -> `configuration.md`, `dx12-injection-bootstrap.md`
- First DLSS-G output overlay coverage (2026-07-12) -> `frame-generation/guardrails.md`
- dx12_fg_switch_test super resolution (2026-06-11) -> `frame-generation/guardrails.md`
- Streamline Reflex/DLSS-G ownership invariant (test app 2026-06-10; GTA clarified 2026-07-15) -> `frame-generation/guardrails.md`
- x86 DX12 overlay crash/freeze is fixed as of build `0.1.3822`. -> `handoff-dx12-32bit-crash.md`
- DX12 focus/fallback policy
- DX12 dynamic glyph uploads are allocator-owned (2026-09-13) -> `overlay-rendering.md`, `frame-generation/guardrails.md`
- DX12 third-party/FG coexistence remains topology-driven. -> `dx12-overlay-third-party-coexistence.md`, `frame-generation/guardrails.md`, `frame-generation/case-studies.md`
- Immutable CFR grid / bounded exact stop -> `recording-output-paths.md`, `wgc-capture.md`, `cfr-capture-sync.md`
- WGC/DXGI transactional frame zero (2026-07-14) -> `wgc-capture.md`, `cfr-capture-sync.md`
- WGC active-delay smoothness and grid-debt recovery (corrected 2026-07-26) -> `wgc-capture.md`, `cfr-capture-sync.md`
- WGC variable-input CFR producer contract (2026-07-14) -> `wgc-capture.md`
- WGC target selection / item diagnostics (2026-07-18)
- Monitor-scope target selection (2026-07-21) -> `configuration.md`, `wgc-capture.md`
- DXGI Desktop Duplication backend (2026-07-02) -> `wgc-capture.md`
- DXGI keyed-mutex repeat/reclaim lifecycle (2026-07-11) -> `wgc-capture.md`
- A/V sync harness is now the strict oracle for CFR capture changes (2026-06-14) -> `cfr-capture-sync.md`, `multi-audio-capture.md`, `regression-testing-and-logging.md`
- HAGS-on capture contention (2026-07-12) -> `performance-priority.md`, `wgc-capture.md`, `cfr-capture-sync.md`
- D3D11 forced AF / wrapper path -> `dx11-forced-af.md`
- DX12 forced AF -> `dx12-forced-af.md`
- Audio/CFR sync -> `multi-audio-capture.md`, `wgc-capture.md`, `cfr-capture-sync.md`
- Process-loopback selection and activation-session proof (2026-07-14) -> `multi-audio-capture.md`

## Maintenance

- Add a line here only for a change that alters how a subsystem must be understood; details go to the topic page.
