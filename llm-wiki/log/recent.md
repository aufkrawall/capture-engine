# llm-wiki Log

### 2026-11-13 - GTA FSR menu judder after DLSS->FSR: CE's held DLSS-G unload never ended (root cause, fixed)

- User A/B (session `20261113_175053`, 0.1.7524): direct FSR start -> menu fine; FSR -> DLSS -> FSR -> menu judder /
  ghosting again. Good menu: exactly 144 presents/s (mean 6946 us); bad menu ~158-160/s (mean 6263-6324 us). Both
  `DXGI final present VSync: route=runtime-output in(sync=1 flags=0x0) out(sync=1 flags=0x0)`, DXGI forward ~0.2 ms in
  both, same overlay route (`suspend-backbuffer`), clean `[OverlayGraph]` counters in both.
- Difference: the good FSR chain was created through `sl.interposer.dll` with BufferCount=3 (GTA had unloaded DLSS-G
  first: `slSetFeatureLoaded(DLSS-G,0)` forwarded, no live chain); the bad one by `sl.dlss_g.dll` with BufferCount=6.
  At the switch CE held the unload (`[SL UNLOAD ORDER] deferred` 17:53:35.108). The proxy's release took the native
  chain to 0 refs at .534 (callers `dxgi.dll+0x14C03E/0x14C0B6` = ReShade's proxy, stack via `sl.interposer+0x2A68F`)
  with NO `slHookSwapChainDestroyed`, so `proxy-released` never matched and the lazy `chain-gone` check needs another
  `slSetFeatureLoaded`, which GTA never makes. DLSS-G stayed loaded all session and its plugin made FidelityFX's chain.
- Same signature in every DLSS->FSR session since the hold (gtajudder, gtafsroverlaystutteryghosty,
  gtaoverlaylatefsrgraphstuttery, gtafsr, 20261112_*): `deferred`, never `proxy-released`; once `chain-gone` at a later
  load call. Supersedes the "tearing / driver ignores sync=1" reading of the gtafsr entry below: the unlocked cadence
  belongs to sl.dlss_g-created chains (also 163/s with DLSS-G off on the DLSS chain itself), not to FSR.
- Fix: `UnloadOrderPolicy::OnNativeReleaseReturned` forwards (`reason=chain-gone`) at the release that leaves the held
  chain with no reference, except while that chain's destroy hook runs (`OnDestroyEntered`, Streamline's hook loop).
  Forward context is the same as the existing proxy-released path (after the native Release returned, inside the
  caller's Release). Not hardware-verified: whether real Streamline accepts `slSetFeatureLoaded` re-entrantly there.
- Tests: `StreamlineUnloadOrderPolicy.AHeldChainDestroyedWithoutTheDispatchForwardsAtItsLastRelease`,
  `.TheLastReleaseInsideTheDestroyHookWaitsForTheHookToReturn`; flow
  `FlowDLSSUnloadOrder.AChainDestroyedWithoutTheDestroyHookStillReleasesTheHeldUnload` (fake interposer
  `flowInterposerSetDestroyHookDispatch(false)`: plugin lets go without the hook CE observes) - fails without the fix
  ("DLSS-G stayed loaded", no forward), passes with it; 44 DLSS/switch/menu scenarios pass.
- **VALIDATED** `20261113_181813` (0.1.7525, user: "seems fixed"): 4 DLSS->FSR switches, each `deferred` -> `forwarded
  reason=chain-gone residualRefs=0` after 208-433 ms (caller `dxgi.dll+0x14C0B6`, no deadlock in real Streamline), FFX
  chain by `sl.interposer.dll` BufferCount=3, FSR windows <= 144 presents/s, 12 swapchain handoffs `missing=0`.
- Open (pre-existing, DLSS side): with DLSS-G set OFF by the menu on sl.dlss_g's own chain (18:20:07.6-16) the game ran
  161-163 presents/s with `out(sync=1 flags=0x0)`, every present flipped (+1486/+1488, 0 discarded), no flip schedules
  applied. Same in 20261113_175053 and gtafsr. Likely the same overlay judder in DLSS menus; not investigated yet.
- Diagnostic flaw (mine, e8cb83a9): `[OverlayGraph]` per-route residual/maxStep compare a route's draw with its own
  last draw, seconds old after the other route drew (`out=1072/50/4/3.47/3595.96`); a handoff artifact, not a jump.

### 2026-11-13 - GTA FSR-suspended menu "overlay at lower fps" (session gtafsr, 0.1.7520): presentation, not the graph

- Timeline: DLSS-G on 22:35:35, off 22:36:01; FFX takeover 22:36:08; FSR configure on 12.53 / off 14.99 / on 17.59 /
  off 20.24 (menu, `suspend-backbuffer` route) until exit 22:36:31.
- Overlay side clean: one suspend-route draw per game frame on the owner queue; `[OverlayGraph]` 14.7-24.7 s
  `app=1114/2/0/0.03` (2 holds, 0 jumps, mean residual 0.03 slot); substitute UI texture cleared once on entry, no
  `UI-composite overlay drawn` (old CE-queue path never ran).
- Presentation in the menu: prework count and perf CSV agree on ~157 game frames/s (CSV qpc deltas mean 6.38 ms,
  sd 0.6-0.8 ms) on a 144 Hz panel under `vsync_mode=fifo`; game input `sync=0 flags=0x200`, CE forwards 1/0 at the
  FFX proxy. sensors.log 22:36:17.9-27.9: runtimePresents +1504, flipped +1503, discarded +0, published +1404
  (display series refresh-bounded, sd 0). Every present flipped at > refresh = tearing presents; no driver flip
  metering then (`nvFlipSchedule applied` frozen at 1032 after DLSS-G off).
- Same unlocked cadence already after DLSS-G off without FFX (22:36:02-07, ~162/s). The only earlier 144 lock
  (22:34:39-59) was during focus loss: `overlay_wait` 5.2 ms of CE's own fence wait, i.e. GPU back-pressure under
  composition, not a foreground baseline.
- Open: the final-flip parameters were logged once per thread (first present: `sync=1 flags=0x0` during the protected
  FSR startup), so "interval lost above DetourPresent" vs "driver ignores sync=1" is undecided. Added
  `DXGI final present VSync: route=... in(...) out(...) ... rate=` (hook/present/final_present_vsync_report.h) on
  change + 10 s heartbeat. Next run: out(sync=1) at rate > refresh names the driver (check NVCP V-Sync for GTA);
  out(sync=0, 0x200) names the layer that dropped it. No overlay or pacing behaviour changed.

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

### 2026-11-12 - Unit-test temp directories: never recreate a deleted name

- Gate 0.1.7517: `CrashSymbolStoreTest` x2 failed with ACCESS_DENIED in `create_directories(<pid dir>/installed)`.
  libc++'s `std::filesystem::remove_all` deletes directories with the legacy delete-on-close disposition, so while
  any other handle is open (Defender scanning the files just written, a just-exited child) the name stays
  delete-pending and creating it again fails with ERROR_ACCESS_DENIED (plain `RemoveDirectoryW` uses POSIX
  semantics here and does not). Proven in `ScopedTestDirectoryTest.AHandleHeldOnADeletedDirectoryNeverBlocksTheNextOne`.
- Rule: fixtures take a fresh `ce::test_support::ScopedTestDirectory` (`tests/scoped_test_directory.h`) per test,
  never one fixed per-process name deleted in TearDown and recreated in SetUp. Converted: crash symbol store,
  target launcher, runtime override paths. The helper skips a delete-pending leftover of an earlier same-pid run.

### 2026-11-12 - Menu (FSR suspended) overlay graph judder: draws are not displayed frames

- GTA `20261112_030715` (0.1.7516, `source=display-change`): in the menu AMD suspends FSR FG, display timing reports
  144.0 FPS (32 us stddev) while CE drew ~157 overlays/s (1568 and 1624 per 10 s). The graph cursor stepped one slot
  per draw, so displayed frames moved 1 or 2 slots unevenly. The saved pacing trace does not span the menu, so this
  is the reproduced hypothesis, not a hardware-proven cause; stale-image reuse was not observed or excluded on HW.
- Fix: one cursor per metrics stream (`PerformanceMetrics::PrepareGraphHistory`, `performance_metrics_graph.inl`)
  instead of one per `OverlayAdapter`, so renderer handoffs keep the position. Under display timing each draw maps to
  the first refresh slot after it (grid from the last published display time, period = refresh period, or the
  sample period when displays are slower); draws sharing a slot share a position, distinct runtime outputs
  (`NextGraphOutput()` at the FFX callback, no-callback topmost, PostSL-FG and runtime-owned draw sites) reserve
  successive slots, a repeated output id does not advance. Presentation timing keeps one slot per draw. Prediction
  is capped at `kMaxDryStreak` slots; measured sample values are untouched.
- Tests (`GraphScrollCursorTest`): 160 draws on 144 Hz with batched publication (0.75-1.25 slots per displayed
  frame, drift < 2 slots over 480 displays; failed 0.38-1.87 before), uneven draw phases, 2x/3x/4x bursts with
  duplicates, stall hold/re-arm, FSR active/suspended handoffs sharing one history, stall recovery.
- Open risk: under VRR or a display timestamp whose phase sits near the draw phase, prediction jitter could map a
  pair of draws to slots 2,0 instead of 1,1. `[OverlayGraph] draws= outputs= samples= samePosition= resets=` (10 s)
  shows draws vs published samples and same-position draws; samePosition ~ draws - samples is expected in the
  menu, a large excess with draws == samples indicates that jitter. Hardware validation pending.
- Not done: the FSR-suspend renderer still rebuilds when switching the UI-clear (fmt 28) and backbuffer (fmt 87)
  roles each suspension (`dx12_ffx_suspend_overlay.cpp` keys one state per proxy) - transition cost only.

### 2026-11-12 - Disabled Streamline replacement lost its overlay after the first Present

- GTA `20261112_030715` (0.1.7516): a prewarmed replacement drew once, then inactive-DLSS routing rejected
  its unproven queue. The one-shot marker now promotes lifetime- and buffer-bound normal-route admission on a
  successful submit; active FG cancels it and keeps its own PostSL proof. Retained startup callbacks no longer
  count as physical outputs. The real-hook regression covers two replacements, including observed same-address reuse, stale tokens,
  resize and output pixels; it failed before and passes after. Details: [FG guardrails](../frame-generation/guardrails.md#disabled-streamline-replacement-after-fsr-2026-11-12).
- Gate regression (0.1.7517): promotion cleared `streamlineStartupHandoffPending`, the only handoff evidence of
  the safe post-FSR bootstrap predicate, so a GetState-only DLSS-G enable on the admitted chain was suppressed for
  good (published 0/x1). A submitted admission now counts as that evidence; scenario fails before, passes after.
- Hardware retest pending. The saved pacing trace ends before the suspended-FSR intervals; animation timing
  and output freshness during those intervals remain separate, unproven questions.

### 2026-11-12 - GTA FSR FG overlay at the rendered rate: Streamline's DLSS-G proxy presents AMD's swapchain

- `20261112_013802` (0.1.7514, ReShade): crash gone, overlay/graph juddering after DLSS FG -> FSR FG. Every
  owner-queue batch lay inside AMD's Present: `sl.dlss_g`'s copy (relayed by ReShade's queue proxy), ReShade's flush
  and DXGI's own work in the native Present (disassembled with `cdb`). `sl.dlss_g` created the native swapchain and
  issued its Present, so it is a nested presenter; AMD composes into its proxy buffers on another queue.
- Fix: the anchor also accepts the nested presenter's own owner-queue submission (originator module = native-create
  caller, not the runtime); layers below it never anchor. New flow scenarios (fake `sl.dlss_g` nested proxy, with and
  without ReShade, plus the DLSS -> FSR switch) fail with `granted=0` before and pass after. Details:
  [FG guardrails](../frame-generation/guardrails.md#nested-presenter-under-fsr-fg-2026-11-12). Hardware run pending.

### 2026-11-12 - GTA overlay ~640 ms after ReShade's at startup: EOS's warm-up held the game's own swapchain

- `gtadlssfgtofsrfgcrash` start (no FG, EOS loaded, no startup window): first Present 01.152, CE's first draw 01.789 -
  493 ms startup warm-up, 100 ms post-resume settle, staged backend/RTV/sync init. Nothing was at risk: the swapchain
  was the game's, drawn on its own create queue (`fgOwned=0`).
- Fix: the warm-up applies only while a runtime owns the live swapchain (the startup handoffs behind the old
  `ERR_GFX_STATE` cases); a visible startup window still pauses. New flow scenario: 90 uncovered Presents before,
  none after the first. Outside Streamline the first Present of a ReShade chain stays unprocessed (zero counted ECLs);
  GTA (Streamline no-FG) processed it. Rule: `dx12-overlay-third-party-coexistence.md`.
### 2026-11-12 - GTA DLSS FG -> FSR FG crash: CE drew the back buffer from a queue that did not own it

- `gtadlssfgtofsrfgcrash` (0.1.7513, ReShade 6.8): `6a56e025`'s every-queue relay recovery anchored the final-batch
  route on AMD's batch on a second DIRECT queue; the first draw onto the real back buffer removed the device in the same
  Present (`ERR_GFX_STATE`). The freeze dump's thread-local trace proved the queues and sites.
- Fix: anchors only on the presented swapchain's own queue (`fg_presenter::OutputQueue`), re-checked before each
  append. New flow scenarios fail before (id 907, `0x887A0005`) and pass after; all 24 `FlowFSR` scenarios pass.
  GTA + ReShade keeps the UI baseline at the rendered rate; the new per-batch `submitter=` diagnostic is the next
  evidence. Details: [FG guardrails](../frame-generation/guardrails.md#startup-resources-and-relayed-fsr-composition-2026-11-11).
### 2026-11-11 - GTA late overlay and remaining FSR graph cadence: source changes, no build/tests requested

- `gtaoverlaylatefsrgraphstuttery` (0.1.7511) shows a 100 ms resource-prime pause before the first cold overlay.
  Removed the separate prime/settle path; the selected renderer prepares resources with its draw.
- The deeper FSR walk now finds AMD's Present site, but no final-batch anchor (4 batches, 3 relayed). Removed the
  create-queue-only relay search and direct-before-relayed selection; added bounded site/queue diagnostics and
  separate-composition-queue, mixed-batch and frozen-virtual-clock startup regressions. No builds/tests executed,
  per the user; both changes still require validation. The logs do not establish the cause of visible ghosting.
- Evidence, pending nested-Streamline question and retest criteria:
  [FG guardrails](../frame-generation/guardrails.md#startup-resources-and-relayed-fsr-composition-2026-11-11).
