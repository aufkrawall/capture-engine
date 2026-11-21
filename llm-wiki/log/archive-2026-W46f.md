# llm-wiki Log Archive

### 2026-11-12 - FG flow suite 805 s -> 111-143 s: CE's per-pixel WARP work, not WARP or the debug layer

- Measured per scenario (`GetProcessTimes`, flow-host phase timers): WARP floor 0.17 ms/frame, debug layer + DRED
  ~2%; the rest was CE's display-gamma pass (~68%) and overlay/copies, all proportional to pixels. The 150%-DPI hero
  panel filled 68% of the 640x360 frame. WARP spread over 16 logical processors used ~2x the CPU of one.
- Fix: 256x160 frame, overlay pinned to 100% scale + 8 px font (layout unchanged, guarded by
  `ExpectPanelsFitTheDefaultFrame`), runner pins one scenario per physical core. All 272 scenarios pass.
- Traps found: shrinking only the frame reflowed the panel (available width); the pixel probe scans fixed rows and
  reads the ReShade stand-in's magenta corner as graph, so `FlowFSRMenuPixels` keeps its 640x360/150% geometry;
  screenshot and nested-output scenarios had hard-coded 640x360 sizes and points.
- Then (user go): the post-process pass shades a 16x16 corner in flow builds (routing/accounting unchanged), the
  whole target under `--verify` and in capture/menu-pixel scenarios: 111-143 s default, 258 s full-frame.
  Running gamma scenarios only occasionally was rejected: the pass shares the overlay route's verdict, so most
  FG changes can break it, and late detection makes attribution hard. Not done: shorter 300/600-frame phases.

### 2026-11-12 - FSR menu transition: final-output pixel oracle, one real gap fixed

- `FlowFSRMenuPixels` reads every FidelityFX output back after its Present and judges panel, single blend, HUD,
  text freshness and graph motion from pixels (graph_motion_oracle.h). WARP result through DLSS -> FSR -> menu ->
  resume (nested presenter under ReShade) and direct/callback FSR: uniform ~2.6-2.9 px (one slot) per shown output,
  residual ~0.1 px, no double blend. Flow graphs run on presentation timing (no ETW display data), so GTA's
  display-timed menu cadence is NOT reproduced; no cursor change was made from this.
- Expected, by design: the first ~3 frames after an FSR enable show the same graph on both outputs of a frame (UI
  baseline until the final-batch route re-proves itself). The oracle allows that pattern only in that window.
- Real defect found and fixed (1f58e95e): FSR configure-off kept the topmost route for frame N but retired it when
  AMD began composing N+1; the nested presenter presents N's real output later, so it went out without overlay
  (`AMD FG output of frame 427 has NO overlay owner`, coverage uncovered=1). Retire on composing N+2
  (`ShouldRetireKeptTopmostRoute`). Plausibly a one-frame overlay blink at GTA menu entry; not hardware-verified.
- Test artefacts ruled out on the way: a probe inside the runtime image before Present was learned as CE's anchor
  batch (overlay after the copy), and runtime-image frames between ReShade and DXGI made each real output attribute to
  the next frame (double blend at the handover). Both disappeared with the host-side post-Present probe.
- `[OverlayGraph]` now appends `app=draws/holds/jumps/meanResidual/maxStep out=...` (GraphStepAudit): steps vs the
  elapsed draw interval (application draws) or one display interval per new output, in sample slots. Next GTA run:
  read these in the menu window to see which route holds and whether residual is clamp/resync driven.

### 2026-11-12 - FSR menu transition: displayed pixels still unverified

- Session `gtajudder` (0.1.7519) confirms the nested presenter and final-batch route during active FSR, then the
  producer-queue backbuffer route while FSR is suspended. The sustained menu interval has about 154 proxy draws/s
  against 144 published display samples/s. Expired timing associations do not identify discarded screen frames.
- The graph's draw-time cursor removed repeated CPU positions in the mixed window, but neither that nor the flow
  coverage ledger proves smooth, fresh, single-blended displayed pixels. The fake FFX no-callback route copies the
  scene without composing its registered UI. The flow host can now register a placeholder while the retained proxy
  is disabled and restore the full-size UI on enable; this is scenario setup, not a pixel or cadence oracle.
- Root cause and real GTA/Talos visual acceptance remain open. A final-output pixel observer and realistic retained
  UI composition are needed before choosing a rendering or animation-clock correction.
- The WARP nested-presenter fake now offers opt-in, bounded readback of its native back buffer after the nested
  copy and before downstream Present; it reports the buffer index and sample serial, not display feedback. A test
  observes samples across enable/suspend and checks changing scene pixels before enable; freezing the first sampled
  image makes that pixel assertion fail. The probe retains only the latest image, precedes any downstream Present
  work, and does not yet establish overlay freshness or alpha.
- The FFX fake can opt into GPU UI blending with straight or premultiplied alpha, including when FG is disabled;
  frame configurations pin the registered resource until their outputs finish. Two WARP cases check a half-alpha
  HUD pixel on a retained single texture; forcing straight blending for premultiplied input or blending twice fails
  both phases.
  With internal buffering it snapshots the registered texture on the producer queue, pins it per scheduled frame,
  and checks a frame-coded HUD pixel on each of 52 outputs through suspension and resume. Removing the snapshot
  produces wrong-frame pixels and D3D12 cross-queue ordering errors. These are fake-runtime tests, not evidence of
  the real game's displayed overlay pixels.

### 2026-11-12 - Overlay graph judder in GTA's FSR-suspended menu, second cause

- Session 20261112_154352 (0.1.7518, DLSS FG -> FSR FG, menu open): `[OverlayGraph]` samePosition 128-177 per
  ~1400 draws during the suspend route; source was display-change throughout (presentation only for the first
  ~10 s before scanouts existed). The game called AMD's proxy Present ~156/s (prework counter 600 in 3.84 s) while
  the real chain flipped at 144 Hz with FIFO forwarded (sync=1); ~10/s submissions expired unpublished.
- Cause: the cadence cursor floored each draw onto a predicted scanout grid whose period was the published sample
  period, not the refresh; draws above the refresh rate mapped two to one slot (hold) and then skipped one (jump).
- Fix: cadence cursor runs on draw time / sample period (what the game's content uses), correction capped at 5%
  speed, trail relaxes after a stall. Regression `ApplicationDrawsAboveTheRefreshRateScrollOnTheirOwnClock`
  (old cursor: 236 holds). `[OverlayGraph]` gained `jumps=` (steps >= 1.5 slots). Pending GTA rerun.
