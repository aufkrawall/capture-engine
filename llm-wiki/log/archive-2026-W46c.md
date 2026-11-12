# llm-wiki Log Archive

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
