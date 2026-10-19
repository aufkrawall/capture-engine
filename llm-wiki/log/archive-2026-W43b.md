# llm-wiki Log Archive

### 2026-10-19 - Talos DLSS FG / FSR FG recordings on 0.1.7270: steady state clean, two FG-transition gaps

- `20261019_182822`: both recordings started cleanly under FG (frame zero 0.7 / 4.7 ms from target, prewarm
  ~250 ms, grid anchored at selection). r0002 (capture sync 120): `Input` 120/s with 0 repeats/skips through
  DLSS FG 2x, FSR FG 2x and FG-off segments. r0001 had no capture sync (general 140 cap, DLSS FG 2x): 138 fps
  source -> 18 skips/s, inherent to 138->120 resampling.
- Open 1: 18:31:29.7-36.9 DLSS-G reported `[DLSSG HEALTH] ON but NOT interpolating` (CE's multiplier
  detector: `real=0`), yet the limiter kept `fg=1/2x fgProof=d3d-sleep effective=60`. The game showed and
  recorded 60 fps (60 repeats/s) for 7 s until the FSR switch.
- Open 2: capture froze longer than the game during FG switches (max repeat streaks vs perf-CSV present
  gaps): DLSS->FSR 155 ticks = the game's own 1.32 s stall (fine), then 118 ticks (~1 s) at FSR FG
  activation while the game presented (gap 100 ms; `Protected official FFX startup pending ... suppressing
  ... capture`, transport 2->115 at 18:31:40.8); FSR->DLSS 224 ticks (1.87 s) vs a 219 ms game gap
  (Streamline handoff at 18:32:21.2, DLSS-G OFF->ON 22.687, transport 115->116 at 22.932).

### 2026-10-19 - Capture-sync recording validated end to end

- `20261019_182429` (0.1.7270, BioShock 120 fps capture sync = CFR 120): fresh frame zero (3 ms), grid
  anchored at selection, 0 skips, 1 repeat at a game hitch (limiter: 2 late frames), phase lock 1/0/0,
  healthy, no audio startup gap. Nothing avoidable left in this scenario.

### 2026-10-19 - Inject CFR grid anchored at frame-zero selection (fixed, hardware pending)

- `20261019_180734` validated frame zero (0.5 ms from target, first encode 2 ms) but tick 1 skipped a frame:
  the inject grid anchored 5.6 ms after selection (end of encode + A/V start reset). Now anchored at the
  selection instant, bounded to commit - I for a late commit. Details: `cfr-capture-sync.md`.

### 2026-10-19 - Inject frame zero by content target, encoder prewarm (fixed, hardware pending)

- Root cause of the stale opening frame (`20261019_173119`): the go-live tick's selector still ran the
  warm-up rule (pop oldest) over a ring backlog filled before media ingested, and the first encode (header
  27 ms + shader compile 52 ms) held frame zero ~95 ms before tick 1 jumped 12 frames. Fix: frame zero
  nearest the content target with bounded wait for a fresh frame, no warm-up flush/trim for inject CFR,
  and an inject encoder prewarm (new `MediaEngine_PrepareInjectVideo`). Details: `cfr-capture-sync.md`.

### 2026-10-19 - Present-release stamps validated; stale inject frame zero found

- `20261019_173119` (0.1.7268): phase lock 1 acquire / 0 rephase / 0 release, 4439 consecutive unique ticks
  after the first second (0 repeats, 0 skips), every claim matched; user saw it smoother. Remaining 3 repeats +
  12 skips are the startup: frame zero 272 ms behind the live target (stale ring backlog kept by warm-up),
  held to PTS 95 ms by a 120 ms first encode. Pre-existing; open item in `cfr-capture-sync.md` Open Questions.

### 2026-10-19 - Capture sync: inject frames stamped before the limiter wait (fixed, hardware pending)

- Session `20261019_164020` (0.1.7266, BioShock Infinite x86 DX11, capture sync 120 = CFR 120): limiter
  Presents exact (phase sd 0.2 ms over 4490 frames), yet 14 repeats + 12 skips in 36 s. Media stamps = hook
  entry + ~50 us, 1.2-6.1 ms before the Present, phase jumping ~4.5 ms with load; `[CFR PhaseLock]` 3 acquires,
  3 rephases, 2 releases, pairs clustered at those transitions. Root cause: D3D10/11/12-base captures run in
  ProcessFrame before `ApplyFpsLimiterStage` and stamp their own QPC.
- Fix: limiter-published present-release forecast taken inside `ScopedPreLimiterCapture` (DXGI sites only;
  D3D9/D3D8/OpenGL/DirectDraw pace after Present, Vulkan captures after the wait). A/V: frames now sit where
  they appeared on screen (before: up to one limiter wait early). Details, evidence and diagnostics:
  `cfr-capture-sync.md`, `frame-pacing-and-limiter.md`. Validate with a capture-sync recording that changes
  load: `[CFR PhaseLock]` should acquire once and stay, Dup/SelDrop only at `phaseSkipped` hitches, limiter
  stats `stamp(... claims=N/0/0)`, media `StampAhead=` nonzero.

### 2026-10-19 - Review of eaf7e902..1890a232: gate blockers and the FPS limiter's passed-Apply mark

- An external review listed seven issues. Fixed: lint (three files over 800 lines; clang-tidy checks absent from
  or above the baseline in the D3D11 AF engine, context dispatch, overlay, AV1 and encoder-probe code), trailing
  whitespace in `dx11_hook_detours.cpp`, and the FPS limiter's `t_FpsLimiterPassedApplyPending` leak
  (`frame-pacing-and-limiter.md`). The `.patch` whitespace hits are unified-diff context lines for blank source
  lines and stay.
- The full lint run showed far more than the review's targeted runs: about 35 sites over the 2026-10-15 baseline,
  mostly tests added since (zero-initialized D3D descriptors, fixed RNG seeds, `push_back` without `reserve`,
  assignments in `if`). The ratchet counts raw lines, so a header finding counts once per including TU: a new TU
  that includes `test_capture_pipeline_policy_shared.h` alone added five `unused-function` hits. Fixed in place,
  baseline untouched.
- `DXGIShared::ClassifySwapChainAPICached` keyed its thread-local verdict by the swapchain address only, so a
  swapchain created at a freed one's address reused the old verdict (the analyzer's stack-escape finding in
  `test_graphics_api_classification.cpp` pointed at it). Fixed the same day: the verdict lives in the chain's
  private data (`graphics-api-reporting.md`).
- Rejected after reading the code: the sticky retained-YUV fallback (`retainedSplitShareFailed_` lives in a
  `WGCCapture` that both stop paths publish as null, and the media process exits after each recording); the CFR
  horizon planner cost (bounded by the 32-candidate cap and the slot window, tens of us worst case once per output
  slot in the media process, not the game); the Vulkan carried-window map (one small entry per window; not
  carrying `degraded` is deliberate re-probing).
- Splits: the NVIDIA LOD-spread section moved to `nv-lod-spread-fix.md`; `cursor_renderer_bitmap.cpp` holds GDI
  bitmap acquisition; `test_perf_logger.cpp` holds the `PerfLoggerTest` cases.

### 2026-10-19 - Review fixes: encoder=auto binding and context leak at first-frame Init; inject slot content time

- `encoder=auto` (e16764e6): the first-frame `Init(savedConfig)` (screen grab every recording; inject while
  `initDone` is false) saw the already concrete name and cleared `autoEncoder.request`, so `ResolveAutoEncoder`
  returned early from then on (no real-open retry, no late GPU decision). `ReinitWithSavedConfig` now keeps the
  binding; `EncoderAutoProbeTest.FirstFrameReinitialization*` failed before the fix (request `""`).
- Face camera (b4b3abf9) and cursor (since dd7bb5db, 2026-07-14) on inject: selected at `scheduled` while
  `SelectInjectFrame` targets `scheduled - avContentDelayQpc`; with an audio capture latency both led the game
  frame by it. One `ComputeSlotContentQpc` now feeds both. No hardware run yet (webcam clap test, inject with
  app-audio latency).
- Context leak, fixed: the first-frame re-Init allocated new `fmtCtx`/`codecCtx` over the unopened previous ones
  (no `Stop()` in between). `Init` now frees them first and refuses `initDone`/`fileOpened`; the deferred writer
  thread is already running there, so it cannot be part of the refusal. `VideoEncoderReinitTest` counts the
  release through FFmpeg buffer callbacks (0 before the fix, 1 after, still 1 after the destructor).
