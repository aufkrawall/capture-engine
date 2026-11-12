# llm-wiki Log

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

### 2026-11-11 - Present-chain overlay/HDR flow coverage; ordinary queue replacement misread as FSR FG

- Added native/FidelityFX Present-chain stand-ins for ReShade, Steam, Epic, Ubisoft and Social Club, plus
  HDR10/scRGB FG toggles and a stacked HDR10 FSR run. Stand-ins wrap queues/swapchains and draw at Present;
  real effects, wrapped devices, hardware colors and temporal ghosting remain outside this coverage.
- Native cold initialization is measured separately; all later presents and all FG transitions retain strict coverage.
  A fresh ordinary game queue falsely became runtime-owned FSR FG and suppressed every later overlay draw.
  Creation provenance now resets ownership/baselines immediately (`dx12_hook_postsl_route.cpp`).
- Epic's hyphenated launcher names now enter every overlay cache; underscore aliases remain (`40263b53`).
- The harness also retained old swapchains through same-pointer WRL `Attach`; empty returned-reference holders fix it.
  Details and regression anchors: [refactor-roadmap.md](../refactor-roadmap.md), "FG flow harness".

### 2026-11-11 - GTA + ReShade FSR overlay at rendered rate; display trace opt-in; onset wording

- `gtafsroverlaystutteryghosty` (0.1.7505, DLSS FG -> FSR FG with ReShade): the final-batch route never armed
  (`trace unusable ... runtimeBatch=0 relayed=1`); AMD's Present call lay beyond the 20 stack frames searched behind
  Social Club, Epic's overlay, Streamline and ReShade. Deeper search `05276970`, flow scenario with a 40-frame chain
  ([overlay-rendering.md](../overlay-rendering.md)). Hardware retest pending; "ghosting" not explained by the logs.
- Raw display trace now opt-in: `[Diagnostics] display_stall_trace=false` by default (`e68f3a8f`).
- `Surface onset` says "on screen by N ms" (an upper bound, about one refresh late).
- Not done after a closer look: tokens marking composed presents for screen times (needs a present-identity
  correlation and changes the composed state the latency and graph use; gain ~100 ms per switch), and quieting the
  pre-first-present `no screen-change timestamp published yet` warning (would also hide a trace delivering no presents).

### 2026-11-11 - Token fix verified on hardware; composed first frames missed by two readouts

- GTA 0.1.7505 `20261111_162241` (normal mode, DLSS-G save load, clean): `tokens(created=6415 ... flipped=6406
  undecodable=0)`, `Surface onset` 14.7 / 28.6 ms by composition. The sealed reference keeps 49 s of full detail (was
  ~30 s with `DCompCommitBatch`).
- The trace shows the onset is an upper bound: Retired comes with DWM's next frame (29.0 ms), the first frame was on
  the display at 20.4 ms. `[SL STARTUP] firstScreenChange` (178 ms) and the sealer's `silenceMs` (110 ms) wait for the
  first independent flip because composed frames enter the screen-change stream only after presents expire.
- Clean and black loads still match up to the first present; only DWM's bind differs (0.9 ms here, ~1.36 s black).

### 2026-11-11 - Composition tokens were never read; surface onsets; recorder drops DCompCommitBatch

- The sensor's Win32k token counts were zero in every session: Windows 11 build 26300 sends both token events as
  version 1 (fields only appended) and the handlers took version 0 only. Fixed in `0c4e828d`; the tally now tells
  retired, independent-flip and unshown endings apart and logs one `Surface onset` line per new swapchain binding (time
  to first shown frame, presents discarded before), which identifies a cold DLSS-G black load from any session.
- GTA black-screen verdict: no CE cause in evidence (DWM fails to bind before CE draws); no CE workaround made.
- Recorder (`0fd15d28`): DCompCommitBatch left out - ~63 k/s from one DWM thread during GTA's save load, 63% of a file.

### 2026-11-11 - GTA cold DLSS-G black screen: decided at DWM's bind, PostSL trigger refuted

- 0.1.7504 sealed two `switch` references: `20261111_153003` (observer flags) and `20261111_153313` (normal mode, the
  PostSL back-buffer draw ran exactly as in the black `024650`), first frame after 90/78 ms. The draw is not
  sufficient for the black screen.
- All four recordings agree: in clean loads DWM touches the new composition surface 6-27 ms after DXGI binds it
  (`CompSurfInfo_ForceUpdateRealization bindId=0`), in black loads only at ~1.36 s, 180-250 ms before CE draws anything.
- The 4/4 vs 3/3 split followed a Windows session: every black load ran between 01:37 and 02:49 (shutdown 03:22); all
  four loads after the 14:34 start were clean. Next evidence: a black load sealed with the Win32k events.
- `DCompCommitBatch` is 63% of the recorder's events under GTA (~36 k/s), cutting full history to ~30 s.

### 2026-11-11 - Display recorder: Win32k surface-binding events and switch references

- To find why DWM leaves GTA's new DLSS-G chain unbound, the recorder now takes 36 Win32k events by id (swapchain and
  logical-surface binds, binding status, visibility regions, focus/fullscreen, DCompCommitBatch), and the sensor seals
  a `switch` reference when a new swapchain's first frame shows within 1 s (2 per session), so a clean run yields the
  comparison trace. Details: `display-stall-trace.md`.

### 2026-11-11 - GTA cold DLSS-G black screen: the PostSL back-buffer draw is the trigger

- Staged observer runs `20261111_144926` (policy-only) and `20261111_145243` (startup-present-only): clean, first frame
  after 142/130 ms, while the startup policy ran and the overlay was drawn into DLSS-G's UIColorAndAlpha. What remains
  is CE's PostSL draw onto the new chain's output buffers from its first present (4/4 black with it, 3/3 clean
  without). Why DWM then does not bind the surface is not in the traces (`display-stall-trace.md`).
- The 1388 ms screen capture of the first seal began after DWM's realization; captures never ended the black time.
- The session manifest keeps the startup observer flags after a config reload; the hook's lines show the real mode.

### 2026-11-11 - GTA cold DLSS-G black screen: clean with CE passive, screen captures ruled out

- Second seal `20261111_024650` (TrafficMonitor closed, 1.38 s): no screen capture in the whole file, yet DWM forced
  the new surface's realization at 1355 ms and recovered as before. The capture at 1388 ms in the first seal only
  coincided; the realization trigger is unknown.
- `overlay_observer_only=1` (`gtanooverlay`): the same cold DLSS-G load showed its first frame 144 ms after the first
  present (5 presents), no stall, nothing sealed; all four overlay-active loads took 1.16-1.46 s. One run, lower
  DLSS-G base rate. Next: the staged observer modes separate the Streamline startup policy and startup-Present
  routing from the first-frame PostSL draw (`display-stall-trace.md`, "Second seal and the CE-passive run").

### 2026-11-11 - First sealed display trace: GTA's cold DLSS-G black screen stops in DWM

- `20261111_021555` (0.1.7503) sealed `display_stall_7008_ep1_ended.etl` (1.42 s silence, 157 presents, nothing lost).
  The new DLSS-G chain presented 153 frames to DWM, which confirmed and discarded each without binding the chain's
  composition surface; the window's old surface stayed in independent flip on MPO plane 1 (destroyed with
  `KeepExistingContent=1`), no plane flipped at all. Another process's ~1 s screen capture forced the realization at
  1388 ms; DWM then left the old surface's independent flip and composed the new frame. Evidence and method:
  `display-stall-trace.md` ("First live seal"). CE's share is unproven until a passive or CE-off comparison.
- The ~1 s screen reader is most likely TrafficMonitor's per-second GDI taskbar color probe. That it ended the black
  screen was refuted by the second seal (entry above).

### 2026-11-11 - GTA 0.1.7501: ownership gate held, cold DLSS-G start still black for 1.7 s

- Session `20261111_013758` (DLSS FG on at load, then FSR <-> DLSS <-> off many times, 8 Streamline handoffs): no
  crash, no device removal, no `[PostSL OWNERSHIP]` refusal. `[FG OWNERSHIP] slDLSSGGetState(ON) starts no DLSS FG`
  fired on FidelityFX chains in protected disabled startup (`held by=ffx-swapchain lifetime=4/6/10/13`), first at
  01:40:41.192, 853 ms after the takeover, 32 calls - the 0.1.7500 crash precursor (that one published DLSS FG 1.1 s
  after the takeover), now refused.
- Black screen: only the first, cold DLSS-G handoff. FG was enabled through SetOptions 114 ms after the create, before
  the chain's first present (255 ms); first screen change after 1.69 s and 156 S_OK presents, recovered via composed
  frames (`[SCANOUT STALL] ep=2 ... firstChange=composed`). The 7 later handoffs, where FG came on 1.5-2 s after the
  first screen change, showed the screen after 0.20-0.25 s (11-16 presents). The user hears coil whine during the
  black phase: frames are rendered and presented, scanout never gets them. Same signature as the 37 s in
  `20261110_222057`; still unattributed (no CE-off comparison, no ETL).
- The recorder ran but sealed nothing: the expiry monitor started 1.5 s into the silence and counted 531 ms.
- `20261111_015744` (0.1.7502): the same cold start, black for 1.37 s (121 presents, ended by the game's own flip, the
  game again toggled Reflex off and DLSS-G off/on in the first frames). The expiry monitor never started, so even the
  corrected expiry trigger sealed nothing. The trigger now measures the silence directly from presents and published
  screen changes, as the hook's `[SCANOUT STALL]` does, with a 1 s threshold (`display-stall-trace.md`).

### 2026-11-11 - Raw display trace sealed on no-display episodes (pending hardware)

- For the GTA save-load stall (37 s of presents, nothing on screen) the user asked for evidence without external tools.
  The elevation service now keeps a circular raw ETL of DXGI/DxgKrnl/DWM/Win32k-token/NVIDIA events with trace logging
  on; the sensor seals it into the session folder when an expiry episode shows <= 2 composed frames/s (once at 10 s,
  at the end if >= 1.5 s). Read with `tools/tracing/etl_stall_digest.py`. Details and open questions:
  `display-stall-trace.md`. Unit tests (policy, sealer, protocol, path validation) pass; no live seal yet.

### 2026-11-11 - GTA late DLSS -> FSR device removal: stale GetState took FidelityFX's chain

- Session `20261110_222057` (0.1.7500): the unload-order fix worked (forwarded at proxy release, FFX create ok,
  same COM address). At 22:24:34.526 a `slDLSSGGetState(mode=on)` on the title's thread (its stale options) passed the
  lapsed 250 ms suppression and `safeBootstrap`, published DLSS FG, retired the protected FFX startup and armed PostSL;
  the first PostSL submit (`falling back to scQueue` = the DLSS-G-era queue) on FFX's chain removed the device
  (`0x887A002B`) right after logging `PostSL CONFIRMED`. Dumps: `ERR_GFX_STATE` dialog, then GTA's `int 3`.
- Fix: presenter ownership in `ce::fg_presenter` gates every DLSS-G signal, the protected-FFX retirement and PostSL
  admission; device-removed submits never confirm (see `frame-generation/guardrails.md`, "Presenter ownership").
  WARP repro failed before and passes after; all 236 flow scenarios and the unit suite pass. Hardware retest pending.
