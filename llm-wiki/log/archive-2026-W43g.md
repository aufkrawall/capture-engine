# llm-wiki Log Archive

### 2026-10-23 - XeFG support (native and OptiScaler), phases 0-5 in code; hardware pending

- Plan from session `20261023_183119` (Talos + OptiScaler `FGOutput=XeFG`: overlay on 72 of 144 frames, `DLSS_FG 2x`
  published for XeFG). Commits `4d389d04`..HEAD: FG runtimes by base name, present originator log, coverage
  inheritance only on the composing runtime's presents, `ce::fg_presenter`, `RuntimeMode::kXeFG`/status type 4,
  the DLSS-G front, XeFG/XeLL export detours, the runtime-output route on XeFG's queue, the `SetPresentId` limiter
  site, XeLL anchors, capture coverage. Topic page: `frame-generation/xefg.md`.
- Two defects the new flow scenarios caught: a runtime-presented chain seeded `dx12_hook_g_OriginalGameQueue`
  (after destroy every native present `zero-ecl-skip`, 899 uncovered), and the scene-transition gap measured across
  the FSR FG -> XeFG swapchain handoff (`gap=4170ms`, 30 blank outputs). Both fixed at the root.
- Third: XeFG's first output after a DLSS-G signal turned out to be a front lost its overlay to the Streamline OFF
  teardown (and XeFG's chain may reuse a released chain's address): registration lifetimes are swapchain identities
  now (`frame-generation/xefg.md` invariant 9). `dx12_hook_process_session_stage2*` split (811 lines).
- Not done: Intel-GPU UI-texture path (contingent, no evidence), exclusive fullscreen, Steam on XeFG's presenter
  thread, an OptiScaler `dxgi.dll` fake serving the DLSS-G API (the provider rule is covered by policy tests).
