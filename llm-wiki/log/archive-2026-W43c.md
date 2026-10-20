# llm-wiki Log Archive

### 2026-10-19 - Pre-FSR Streamline output boundary VALIDATED on 0.1.7277

- `20261019_215024` (Talos; DLSS 2x pre-FSR, FSR 2x, post-FSR DLSS 4x; capture sync 120). Pre-FSR limiter lines
  show output-group admission (`group=140/2`, `120/2`); only 3 `Reflex Sleep paused` in the session, all real
  stalls at menu transitions (`recent=0`, `async=1`). PC latency 2x 41-45 ms, FSR 2x 43 ms, 4x 69-71 ms (30 fps
  base). Display phase 0-10 ms, `phaseReacquire=0`, `pendingNow` <= 9.
- Recording: 7 seconds with repeats in ~110 s; DLSS->FSR = the game's 1291 ms stall (Dup 151); FSR->DLSS 4x =
  the game's 216 ms stall (Dup 32) plus the base-present -> final-output path handover and phase-lock reacquire
  (~0.75 s, Dup 27/19), the same size as in earlier sessions. A/V nominal.


### 2026-10-19 - Pre-FSR Streamline outputs paced as a final-output boundary; async Sleep phase (hardware pending)

- Topology evidence (Talos 204226/212042, GTA V Enhanced `fgtests`): `presentOwner=0x0000` always, SL routing never
  active, every Present at the hook from Streamline. Both DXGI normal routes now pace these as
  `kFinalOutputBoundary` unless the application's own Present or CE's wrapper is paced on that swapchain
  (`ce::streamline_output_site`, `DXGIShared::IsStreamlineFinalOutputForLimiter`). Model: basic 140/4x Talos burst
  measured 46.4 ms frames on the old site vs 28.57 ms at the boundary.
- `212042` residue of the warmup fix: `perFrame=1` episodes with 2-3 `withoutSleep=1` stalls at FG off and post-FSR;
  Talos Sleeps on T616C and presents on T35C0/T46B0. The coverage bound gets +1 off the Sleep thread
  (`ReflexLimiter::GetLastGameSleepThreadId`).
- Hardware check: `DXGI limiter: Streamline outputs on sc=... are a final-output boundary`, limiter lines with
  `site=2 group=140/4` pre-FSR, handoff lines `async=1` and `stalls` at most 1 per pause.

### 2026-10-19 - Per-game-frame Reflex warmup VALIDATED on 0.1.7276; DLSS-off PC-latency outlier open

- `20261019_212042` (Talos; DLSS 4x pre-FSR, FSR, post-FSR DLSS 2x, FG off). Pre-FSR 4x reports `perFrame=4` and
  only real stalls pause the handoff: every transition shows 1-3 `Reflex Sleep paused (... stall #n)` then hybrid
  pacing holds (was continuous flapping for ~1.6 s). PC latency 4x 55-62 ms at 35 fps base, 69.5 ms at 30; 2x
  43-46 ms. Display phase 0-19 ms, `phaseReacquire=0`, `pendingNow` <= 10. Repeat clusters sit on game stalls
  (1297/255/265 ms heartbeat gaps) and menu entries; A/V nominal.
- Open, cosmetic: at each DLSS OFF the overlay publishes one PC-latency sample of ~144 ms for ~2 s
  (21:22:07.697, `samples=1`, `markerToPresent=136091us`, cross-check presentation/display 16.6 ms; also 146 ms at
  20:44:41 in `20261019_204226`): the first marker pairing after the source change spans the game's own switch stall.
- Logger: `log ring overflowed 7 time(s)` at the FSR switch burst (21:21:39), direct-append fallback, no loss.


### 2026-10-19 - Pre-FSR Reflex warmup flap: judged per game frame now (fixed, hardware pending)

- Root cause of the latent item below (`20261019_204226`, 20:42:48.86-49.73): on Talos's pre-FSR route every DLSS
  output is a confirmed standalone Streamline Present paced as `kUniqueApplicationPresent`; 4x outputs come as a
  flip-metered ~2.5 ms burst, the 2 ms duplicate window caught outputs 2-3, output 4 reached the warmup without a new
  Sleep and was judged a stall (~25 ms CE wait on Streamline's presenter thread T35D4, streak re-based) every frame.
- Fix: one Sleep covers up to 2(N - 1) further evaluations, N = evaluations per game frame at the site
  (`reflex_warmup_policy.h`); N = 1 sites keep the per-evaluation rule. Stall/resume logs metered per episode, one
  handoff summary line. Details and the bound: `frame-pacing-and-limiter.md`.
- Hardware check: after FG switches and menu exits on Talos pre-FSR DLSS 4x, expect `Reflex warmup handed pacing to
  the driver (... sameFrame>0, stalls=0..1, perFrame=4, site=1)` and no repeated `REFLEX sleep stalled`; no
  `DetourPresent TOTAL SLOW ~25ms` on Streamline's thread.
- Left open: structural final-output classification of those standalone outputs (needs proof the game's Present never
  reaches the hook on that topology).

### 2026-10-19 - FSR <-> DLSS 2x/3x/4x fixes VALIDATED on 0.1.7275

- `20261019_204226` (Talos; DLSS 4x, FSR, post-FSR DLSS 2x/3x/4x, FG off; capture sync 120). 4x PC latency
  65-75 ms before and after FSR (~2 base frames at 30 fps; was 115-145 ms post-FSR). Post-FSR limiter lines show
  output-group admission (`group=120/2`, `120/3`, `120/4`) and at most 1-2 `Reflex Sleep paused` per FG switch.
- Display timing: `correlationPhaseUs` 0-40 ms all session (was up to 645 ms), `phaseReacquire=0`, `pendingNow`
  ~10 (was ~68), `claimed=` rising only at transitions (8 -> 133). Still +5-10 ms per idle<->generating transition,
  bounded.
- Idle proof: 0.03-0.27 s from the first non-interpolating sample to `dlss-g-idle` (was ~1 s); menu-entry dips
  Dup 28/37 per second (were 78/90). Post-process: 1 uncorrected frame in the session, no false keep-alive run.
- Every remaining repeat cluster sits on a game present stall at an FG switch (heartbeat gaps 251-1311 ms); A/V at
  the nominal 60 ms pull latency.
- Latent, minor: the pre-FSR normal DXGI route also evaluates two DLSS 4x outputs per game frame (site
  `kUniqueApplicationPresent` on Streamline's present thread T35D4); after a stall at 20:42:48 the Reflex warmup
  flapped for ~1.6 s before settling. The warmup still requires a new Sleep per evaluation on that route.


### 2026-10-19 - The three 20261019_195220 defects and the menu dip fixed (hardware pending)

- 4x latency: the post-FSR bypass now applies the limiter as `kFinalOutputBoundary`, so one evaluation per output
  group and the Reflex warmup completes (`frame-pacing-and-limiter.md`). Expect `fgProof=d3d-sleep` without
  `REFLEX sleep stalled`/`timer fallback` after FG switches and 4x PC latency near 2-2.5 base frames.
- Display-timing drift: a claimed nearest sample is `kClaimed` instead of the next sample; reacquire clears the
  ordering (`cfr-capture-sync.md`). Expect `correlationPhaseUs` to stay near its 2x value and `claimed=` > 0.
- "RCAS gap": a ledger misreport; frames the PostSL keep-alive corrected are `already-applied`
  (`post-processing-sharpen.md`).
- Menu dip: health streak 12 samples, sliding 16-frame output window; expect the base-rate dip at menu entry to
  shrink from ~1 s to a few tenths.


### 2026-10-19 - Talos FSR -> DLSS 2x/3x/4x on 0.1.7273: idle menus at 120, 4x latency, three open defects

- `20261019_195220` (capture sync 120; FSR FG, then post-FSR DLSS 2x/3x/4x, FG off, 2x, 4x). Validated: DLSS-G idle
  menus pace at 120 (`fgProof=dlss-g-idle effective=120` at 19:53:53/19:54:19/19:54:23; ratio via
  `frameSource=title-marker` ~0.4 s after health). Steady recording 120 input / 0 dup in every mode; repeat clusters
  sit on the game's own FG-switch stalls (301/262 ms heartbeat gaps). `[CLASSIFICATION]` starved from game start
  (origGame `...2DD0` vs primaryQ `...33E0`), so uncounted-present capture carried the whole pre-FSR phase.
- Open 1 (4x PC latency 115-145 ms, 3x 57 ms; 2x 44 ms): after each >250 ms FG-switch gap the Reflex warmup never
  finished. Trace per 33 ms frame: `REFLEX native warmup fresh=1`, 2 ms later `REFLEX sleep stalled` (with
  `recent=1`) + `timer fallback`; `Local timer stats ... avgFps=30.1 lastWait=29835us` on T3534 (Streamline's present
  thread). Cause: the post-FSR standalone bypass applies the limiter as `kUniqueApplicationPresent`
  (`ResolveDxgiPresentSite`), so no output-group admission and two evaluations per game frame; warmup requires a
  new game Sleep per evaluation (`ResolveReflexPacingDecision`), the second never has one and re-bases the streak.
  `anchorToPresent` 94-124 ms at 4x vs ~1.5 base frames at 2x/3x. Fix candidate: that bypass carries every final
  output, so `kFinalOutputBoundary`.
- Open 2: media's display-timing correlation phase steps at each idle<->generating menu transition on the post-FSR
  DLSS route: 16 ms (2x) -> 51 -> 184 -> 528 ms (3x steady, `pendingNow` ~68) -> 645 -> 514 ms (4x); reset to 25 ms
  after FG off, 489 ms at the next 4x. ~530 ms cannot be scanout delay (association ~64 frames off). Each reacquire
  costs ~12 repeat+skip pairs (`Dup: 12 SelDrop: 12` at 19:53:54/19:54:24/19:55:56). Root cause not found yet.
- Open 3: RCAS sharpen absent ~40 s in FG-off after DLSS OFF post-FSR: `PostProcess: DX12 still not correcting
  reason=left-before-decision` 19:55:04-19:55:34 (4800 frames) while the PostSL explicit-OFF keep-alive drew the
  overlay; `corrected=191 covered=6211 uncorrected=798 runs=1` at 19:55:00.
- Minor: menu entry still shows ~1 s at base rate before idleness is proven (`Input: 40 Dup: 78`, `Input: 30
  Dup: 90`): health streak 30 samples + 32-frame ratio window.


### 2026-10-19 - Post-FSR DLSS-G: idle proof from title markers; timeline boundary left alone (fixed, hardware pending)

- Route (`20261019_182822`): after DLSS-G came back post-FSR, every output was a `Post-FSR confirmed standalone
  normal-route bypass` Present on Streamline's thread T48D0; DXGI Presents on the game thread T67A8 stopped at
  18:32:22.99, so `DX12_ObserveApplicationSourcePresentTiming` never ran. Pre-FSR both threads presented (FG
  samples alternate T67A8/T48D0 in 300-frame blocks).
- Fixed: the DLSS-G idle proof's output half is now a 32-frame outputs-per-frame window, frames from the title's PCL
  PresentStart (exclusive once seen) or the present hook. Details: `frame-pacing-and-limiter.md`.
- Rejected: feeding the title marker into the final-output timeline: asynchronous to the outputs, and every
  completed group replaces the interval (weight 0). Post-FSR `virtualLeadUs` sat at ~32 ms (`leadClamps=63`) vs
  3-5 ms pre-FSR; source groups refine only the interval, not this phase, so that stays open
  (`cfr-capture-sync.md`).

### 2026-10-19 - Frame-classification queue starvation: reported, not followed (no behavior change)

- Follow-up to the open item below. Queue census of `20261019_182822`: the render thread T349C submits on primaryQ
  `...87E0` from 18:29:27 to the end; origGame `...D490` only gets lists from the presenting threads (T67A8, T48D0).
  Before FSR under DLSS FG 2x every Present counted 2-3 (`consInterp=0`); after FSR every one counted 0.
- Rejected: classifying on primaryQ. Its submissions are not in step with the Presents, and
  `ShouldDetectECLPatternFG` latches FSR FG after 10 cumulative zero windows, so a noisy count could falsely latch
  heuristic FSR (cooldowns, overlay blanking). With 56c43347 every remaining zero-count consumer in FG-off phases is
  diagnostic. Post-FSR DLSS FG never reached the ECL path (no `Frame #` samples after 18:32:20).
- Added: `[CLASSIFICATION]` starvation log (120 zero-count Presents while primaryQ carried lists, reminder every
  3000, recovery line) from a primaryQ shadow count that never feeds classification. Details:
  `frame-generation/guardrails.md`.
- Still open: the post-FSR DLSS FG phase bypasses `DX12_ObserveApplicationSourcePresentTiming`, so final-output
  source groups (and the DLSS-G idle proof) get no samples there; `correlationPhaseUs` was +35 ms vs -5 ms before FSR.
  (Idle proof fixed in the entry above; the timeline part stays open.)

### 2026-10-19 - DLSS-G idle pacing and both FG-switch capture freezes (fixed, hardware pending)

- Open 1 above (menu at 60 under a 120 capture sync): DLSS-G now counts as idle only when the health streak and
  >= 30 single-output final-output groups agree; while idle the limiter paces the full target and asks the D3D
  Reflex driver for target x factor. Details: `frame-pacing-and-limiter.md`.
- Open 2 at FSR FG enable (0.96 s) was NOT the protected-FFX startup suppression noted above: the producer's
  `Reset` for FSR's new swapchain was deferred 39.810-40.771 by consumer leases. Media held generation 2's last
  frame (171) pending its source read, polled only after encoder submits, while every tick was an AV1
  show-existing repeat until the 120-repeat ledger bound forced a re-encode (120 ticks = 1 s at 120 fps). Fix:
  poll after show-existing repeats; producer deferrals logged per episode. Details: `cfr-capture-sync.md`.
- Open 2 at FSR->DLSS (1.87 s): perf CSV frames 19262-19438 (21.25-22.65) present at 120 fps with the overlay
  drawn but `capture_us=0`, source index frozen at 4833. FG samples show `cmdLists=0` with `consInterp` rising
  from 18:31:45 through 18:32:20 (also after the FSR API went off at 14.916): the game's 2-3 lists/frame go to
  primaryQ `...87E0` (render thread T349C) while frame classification counts origGame `...D490`. FSR's callback
  verdict covered capture until its swapchain context died (21.166); then nothing did until the PostSL route at
  22.901. Fix: without a possible generator every uncounted Present is an application frame
  (`MayFrameGenerationProduceThisPresent`, `overlay-rendering.md`).
- Still open: the classification queue itself stays on origGame after FSR, so FG pattern detection sees every
  frame as interpolated (API state masks it). Generic follow-up: classify on the queue carrying the game's
  submissions. Hardware check: record Talos DLSS->FSR->DLSS; no repeat streak beyond the game's own present gap;
  look for `resource reset proceeds after`, `capturable as an application frame` and `fgProof=dlss-g-idle`.
