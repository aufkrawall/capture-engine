# Raw display trace for stall episodes

Last verified: 2026-11-11 (two live seals: GTA 0.1.7503 `20261111_021555` and `20261111_024650`, 1.42/1.38 s
silence, no events or buffers lost; two `switch` references on 0.1.7504, `20261111_153003` and `20261111_153313`, and
one on 0.1.7505 without `DCompCommitBatch`, `20261111_162241`; findings below)
Stale-risk: medium - the 10 s ongoing seal is unrecorded; no black load has been recorded with the Win32k events, and
whether CE contributes to DWM's missed bind is open.

Why: GTA V Enhanced 0.1.7500 (`20261110_222057`) presented ~131/s for 37 s after a save load while nothing reached the
screen (`[SCANOUT STALL] ep=1 ... durationUs=38066160 presents=4924`, sensors `composed ready=3563 published=0`). CE's
real-time display session decodes only the events its timing uses, so it could not say whether DXGI, DxgKrnl
submission, the compositor or the display driver stopped. The user asked for the evidence without running external
tools.

## What runs

- `common/platform/display_flight_recorder.h`: one circular file-mode ETW session (`CE_DisplayFlight_Elevation`,
  256 MB, 256 KB buffers, QPC clock, independent-session mode) with DXGI (`kRuntimeKeyword`, verbose), DxgKrnl
  (Base | Performance, verbose, unfiltered), Dwm-Core (`{9e9bba3c-...}`, all keywords, information), Win32k by event id
  (`kWindowManagerEvents`, 35 ids: composition tokens, SwapChainBind/UnBind, SwapChainBindingOpen/Release/Status, DWM
  sprite and logical-surface binds, visibility regions, focus/fullscreen, DCompDeferBatch, layout - other Win32k
  events fire per window message), NVIDIA display schedule, Intel-PresentMon frame type, Kernel-Process. Seal = stop
  (ETW writes the file), move, restart. The Win32k filter was checked live (2026-11-11, unelevated Performance Log Users
  real-time probe): accepted, ~184 events/s on an idle desktop, nearly all tokens. Left out since 2026-11-11:
  `DCompCommitBatch` (id 151, `kDwmBatchCommitEvent`, a static_assert keeps it out). In both reference seals one DWM
  thread committed on its own channel at 60/s, then ~1 k/s from 22 s and ~63 k/s from 19 s before the switch until it
  (GTA's save load, observer and normal mode alike): 1.09 M of 1.74 M events, cutting full-detail history to ~30 s
  (35-72 s without it), with only a channel and batch id per event.
- Elevation service (`elevationservice/service_flight_trace.cpp`): ops `StartDisplayFlightTrace` /
  `SealDisplayFlightTrace` (protocol values 10/11, `kDisplayFlightTraceCapability = 32`, no payloads, outcome in the
  answer - a reply error would drop the connection and the client's display lease). Start needs the connection's
  display lease; the recorder stops with the last flight lease or when the display providers go off. Work file and
  sealed files live in `<installation>\DisplayTraces`, created under the admin-only installation folder and refused if
  it is a reparse point; a sealed file gets read+delete for the installing user's SID. Limits: one seal per 2 s, 32 per
  recording; uncollected files older than 10 min are removed at the next start.
- CE sensor (`display_timing_service.cpp::StartStallTrace`): only with `[Diagnostics] display_stall_trace=true` (default
  false since 2026-11-11, read when the sensor starts; files are up to 256 MB), `log_level=trace` and the service trace.
  `display_stall_trace::Policy` (pure) makes the hook's `[SCANOUT STALL]` judgement from the sensor's own streams: the
  tracked process's DXGI PresentStart events (`OnPresent`, service_events.cpp) and every screen change published for it
  (`OnScreenChange` in `PublishPending`: own flips, and composed frames while the compositor is followed). A *silence*
  runs from the first present after the last change to the next change. It counts only while the process owns the
  foreground window (checked each 250 ms prune) and keeps presenting (a gap > 500 ms restarts it), with >= 8 presents.
  Sealed when the screen changes again after >= 1 s, and once at 10 s while it lasts (keeps the run-up in the circular
  file); at most 6 per session.
- Switch references (`reason=switch`, own limit of 2 per session): a present on a swapchain pointer the process has not
  used among its last 4 (DXGI PresentStart's `pIDXGISwapChain`), after the process already had frames on screen, whose
  first screen change comes within 1 s, seals the same recording as a known-good comparison. `silenceMs` and
  `silenceStartQpcUs` are then the new chain's first present to first change. A pointer the runtime reuses for a new
  chain is not seen as a switch.
- Evidence for the threshold: the published screen-change gaps of the three GTA sessions are 38.06 s, 1.70 s and 1.38 s
  for the cold DLSS-G loads (all black on screen) and <= 0.66 s for every other FG switch.
- Rejected (2026-11-11): watching the submission-expiry episodes. The monitor starts only once a present is older than
  the completion bound (~1.02 s) and prunes every 250 ms; it counted the 1.70 s silence (`20261111_013758`) as 531 ms
  and never started for the 1.38 s one (`20261111_015744`: 116 presents expired in prunes that already saw the
  recovery flips). Measuring from the oldest expired present fixed the first case only.
- Known blind spot: composed frames are published only while the expiry monitor follows the compositor, so a game that
  moves from independent flip to composition can look silent for ~1-1.3 s; in the GTA sessions such transitions never
  reached 1 s, and a false seal costs one file (capped).
- `DisplayStallTraceSealer` does the exchange and the move off the ETW thread; the file lands beside `sensors.log` as
  `display_stall_<pid>_ep<N>_<ongoing|ended>.etl`.
- Logs: sensors `[DisplayStallTrace] recording ...|sealed ep= pid= reason=ongoing|ended|switch silenceMs= silenceStartQpcUs= presents= file=
  bytes= eventsLost= buffersLost= restart=|seal failed ...|move failed ...|no raw display traces ...`; service
  `[ElevationService] Display flight recorder on (... providers ...)|sealed (...)|off after N seal(s)`.

## Reading a sealed file

`python tools/tracing/etl_stall_digest.py <etl> [--pid <game> --pid <dwm>] [--by-pid] [--from-us N --to-us N]` (Windows'
own ETW consumer API through ctypes, no external program; self-test `tools/tests/test_etl_stall_digest.py`). It prints
lost-event counts and, per event stream, count, first/last and the largest silence in QPC microseconds - the unit of
CE's `qpcUs=` fields. The stage whose silence starts first over the episode is where frames stopped; compare with the
decision table in `log/recent.md` (2026-11-10 GTA entry).

## First live seal: GTA cold DLSS-G load (2026-11-11)

`20261111_021555`, 0.1.7503: `sealed ep=1 pid=7008 reason=ended silenceMs=1416 silenceStartQpcUs=6899741153
presents=157`, hook `[SCANOUT STALL] ep=1 ... durationUs=1682680 presents=159 firstChange=composed`. Times below are
relative to the new chain's first present (QPC 6899741153 us).

- -245 ms: old 3-buffer chain destroyed (`DXGI UnBindBackBuffersFromDwm KeepExistingContent=1`); it was in independent
  flip on MPO plane 1 (composition surface 169407265). -209 ms: the 6-buffer DLSS-G chain created and bound to DWM
  (surface 170042330); -185 ms ResizeBuffers (unbind/bind again, `KeepExistingContent=1`).
- 0..1388 ms: the game presented 153 times (DXGI Present, DxgKrnl PresentHistory, Win32k `TokenCompositionSurfaceObject`
  for 170042330, ~110/s). DWM took every token through InFrame -> Confirmed -> Discarded (Win32k `TokenStateChanged`
  NewState 3/4/6) and never Retired one; it scheduled no surface update for 170042330 (`SCHEDULE_SURFACEUPDATE` named
  only other surfaces), rendered nothing (no `RENDEREVENT`, no dirty region) and issued no flip on any plane
  (`MMIOFlip*` silent machine-wide from -250 ms to +1418 ms). The screen kept the old chain's last frame.
- 112 ms: a DWM screen capture (`ETWGUID_RTCAPTURE`, `CompSurfInfo_ForceUpdateRealization`) still realized the *old*
  surface 169407265.
- 1388.5 ms: the next capture's forced realization bound the new surface (`CompSurfInfo_Binding surfaceLuid=170042330
  BufferCount=6`); DWM then ended the old surface's independent flip (`IndependentFlipTransition Entering=0
  surface=169407265 RequestedByDwm=1`), composed present 153 (`Windowed_Dx_Flip_Consumed`, DWM flip at 1391.8 ms),
  and moved the new surface to independent flip at 1403.9 ms; the game's own flips follow from 1514 ms.
- The captures recur every ~1.0-1.6 s for the whole 72 s file (51; each followed by a csrss model-5 present,
  `D3DKMT_PM_SCREENCAPTUREFENCE`, and explorer's `Taskbar` thread): GDI screen reads, most likely TrafficMonitor 1.8.6's
  per-second taskbar color probe. Refuted: that a capture ended the black time - the 1388 ms one began 0.46 ms after
  the realization (second section).
- Stage verdict: the frames stop in DWM, which does not bind a flip-model chain's new composition surface while the
  window's previous surface sits in independent flip on an overlay plane with kept content. DXGI, DxgKrnl submission,
  GPU completion and the token hand-off all ran.

## Second seal and the CE-passive run (2026-11-11)

- `20261111_024650` (0.1.7503, TrafficMonitor closed): `sealed ep=1 pid=24212 reason=ended silenceMs=1384
  silenceStartQpcUs=8802354064 presents=152`; no `ETWGUID_RTCAPTURE` anywhere in the 35 s file. Same DWM state: old
  surface 188368582 flipped independently on plane 1 until its chain was destroyed at -274 ms (`KeepExistingContent=1`)
  and stayed in independent flip, new surface
  189951950 bound by DXGI at -240/-211 ms, every new token InFrame -> Confirmed -> Discarded. DWM ran one frame per
  vblank (~48 Hz panel floor once the old chain stopped flipping, none from 58 to 243 ms) and scheduled surface updates
  only for another process's 1534x898 surface. At 1355.0 ms it forced realization of the new surface and bound it
  (`CompSurfInfo_ForceUpdateRealization` -> `CompSurfInfo_Binding BufferCount=6`) with no capture and no CE event at
  that time, ended the old surface's independent flip at 1356.8 ms and flipped the new one from 1371.7 ms. The trigger
  of that realization is unknown; from first native present to first screen change the active runs took 1435, 1160,
  1458 and 1420 ms (`013758`, `015744`, `021555`, `024650`).
- `gtanooverlay` (session `20261111_031447`, 0.1.7503, `[Diagnostics] overlay_observer_only=1`, recorder on): the same
  cold DLSS-G load showed its first frame 144 ms after the first native present (`[SL STARTUP] handoff=1
  firstScreenChange sinceHandoffUs=465327 presentsBefore=5`); no `[SCANOUT STALL]`, nothing sealed. One run only, and
  its DLSS-G base rate was lower (CE estimate 10 fps against 61 in `024650`).
- What observer-only removes on that handoff: the PostSL draw on the new chain's first native present (prewarmed
  handoff backend, `present->rt` barrier, submit on the DLSS-G present queue), the Streamline UI bootstrap overlay
  recorded into UIColorAndAlpha, the Streamline startup policy (GetState reactivation suppression, held SetOptions)
  and startup-Present routing. Handoffs 2-8 of `20261111_013758` (overlay drawn on the first present of a no-FG chain,
  DLSS-G on ~2 s later) did not black out, so they cannot separate drawing from DLSS-G on from frame one.
- Bisection with the staged observer modes (`common/config/config.h::IsOverlayObserverPolicyOnly`):
  `20261111_144926` (policy-only) and `20261111_145243` (startup-present-only; both modes per the hook's own lines -
  the manifest still showed the pre-reload flags) were clean: first frame 142/130 ms after the first native present,
  6 presents, DLSS-G base rate 66 fps as in the black runs. Both ran the startup policy (OFF churn suppressed) and drew
  the overlay into DLSS-G's UIColorAndAlpha ~500 times. Left as the trigger: the PostSL callback - CE's draw straight
  onto the new DLSS-G chain's output back buffers from its first native present (RTVs on all 6 buffers, submit on the
  DLSS-G present queue); the Present transport is `normalChain` in every mode. Cold DLSS-G starts with it: 4/4 black;
  without it: 3/3 clean. Superseded: `20261111_153313` ran this draw and was clean, and the split followed a Windows
  restart (next section).
- Not explained yet: why DWM reacts. DWM normally binds a re-bound surface 2-7 ms after DXGI's
  `BindBackBuffersToDwm` (another process did so five times in `024650`); the game's DLSS-G surface, bound at -240 and
  -211 ms while the old surface sat in independent flip, was realized only at 1355 ms. In both seals that realization
  happened inside DWM's channel-batch processing (between `CurrentFrameId` and `CHANNELBATCHES_PROCESSED`), and in
  `021555` 0.46 ms *before* the screen capture began - no capture ever ended the black time. The recorder filters
  Win32k to composition tokens, so the window-manager update that forced it is not in the file.
- Duration: 38.05 and 38.12 s in 0.1.7500 (`20261110_221616`, `20261110_222057`), 1.16-1.46 s from 0.1.7501 on. Between
  them: `ca274de0` (presenter ownership) and `4f16603b` (this recorder). Whether either shortened it is unproven; the
  38 s loads also ran in a different Windows session (uptime 7.7 h).
- Reading method: `etl_stall_digest.py` ranks the stall correctly (the flip streams' gaps) but the verdict needed
  payloads - Win32k `TokenStateChanged`/`TokenCompositionSurfaceObject` (surface LUID, present count, NewState),
  Dwm-Core `CompSurfInfo_*`, `SCHEDULE_SURFACEUPDATE` (first 8 bytes = surface LUID) and DxgKrnl
  `IndependentFlipTransition`, decoded with TDH property info (scratch decoder, not in the tool yet).

## Reference seals: the black screen is decided at bind time (2026-11-11)

- `20261111_153003` (0.1.7504, all three observer flags) and `20261111_153313` (0.1.7504, normal mode) each sealed a
  `switch` reference (`silenceMs=90` / `78`, 5 presents; `[SL STARTUP] firstScreenChange` 400/350 ms after the handoff).
  `153313` ran the full active path with the same lines as the black `024650`: prewarmed PostSL backend, exact PostSL
  back-buffer draw submitted before the first Present. That draw is not sufficient for the black screen.
- The DXGI side is identical in clean and black loads: old chain `UnBind KeepExistingContent=1` with its surface in
  independent flip on plane 1, `CreateDirectFlipResource`, `SwapChainBind` + `BindBackBuffersToDwm` + `DWMRedirection`,
  ResizeBuffers unbind/rebind 25-30 ms later, first token `BindId=2`. DWM is not:

| Load | Mode | DWM's first `CompSurfInfo_*` on the new surface | First token after InFrame | First change |
|---|---|---|---|---|
| `153003` | observer | -254 ms, 6 ms after the bind | bound at 14 ms | 90 ms |
| `153313` | normal | -184 ms, 27 ms after the first bind | `SCHEDULE_SURFACEUPDATE`, bound 0.4 ms later | 78 ms |
| `021555` | normal | 1388 ms | Confirmed -> Discarded, no surface update | 1416 ms |
| `024650` | normal | 1355 ms | Confirmed -> Discarded, no surface update | 1384 ms |
| `162241` | normal | -244 ms, 1.6 ms after `CreateDirectFlipResource` | bound 0.9 ms after the first present | 110 ms |

- "First change" is the sealer's `silenceMs`. It sees the process's own flips and composed episodes started by expired
  presents, not the composed frames right after a switch: in `162241` (0.1.7505) DWM bound the new surface at 0.9 ms,
  ended the old surface's independent flip at 1.4 ms (`IndependentFlipTransition Entering=0`), and its frame with the
  first present reached the display with the MMIO flip at 20.4 ms; the token retired at 29.0 ms (`Surface onset ...
  28.6 ms`), while `silenceMs=110` and `[SL STARTUP] firstScreenChange` (178 ms after the first present) waited for
  the first independent flip. The black rows are unaffected (nothing reached the screen until DWM bound the surface).

- The early event is `CompSurfInfo_ForceUpdateRealization surfaceLuid=<new> bindId=0 applied=0` (twice) inside DWM's
  channel-batch processing: DWM's visual tree already references the new composition surface, so its first token is
  scheduled and bound at once. In a black load DWM has no record of the surface until ~1.36 s and discards every token.
  The divergence lies 180-250 ms before CE's first draw on the chain, and CE's lines on the chain-creating thread (SL
  lifecycle, old chain release, CreateSwapChainForHwnd, hook refresh, queue adoption) are identical in both.
- Confound: all four black loads ran in one Windows session (uptime 1.3-2.45 h, ended by a shutdown at 03:22); its
  only clean load was observer-only (`gtanooverlay`, 2.9 h). Since the next start (14:34) all four loads were clean,
  normal mode included. Whether CE contributes at all rests on that single observer-only contrast.
- The Win32k events show nothing else on the clean path: `SwapChainBind` (hWnd, binding handle), `DCompDeferBatch`
  (frame-deadline deferrals, reasons 1/3, under one frame) and one `CompositionTargetGlitch`; SwapChainBindingOpen/
  Status and the sprite/logical-surface binds never fired. A black load with this recorder is what is missing.
- Reading method (scratch, not in the tool): DWM's `CompSurfInfo_*` and `SCHEDULE_SURFACEUPDATE` by surface LUID
  against the game's `TokenCompositionSurfaceObject` LUIDs and the DXGI bind events, relative to `silenceStartQpcUs`.
- Verdict (2026-11-11): no CE cause in evidence. What CE does is identical in clean and black loads up to the bind, and
  the failure precedes its first draw; a CE change would only nudge DWM (a workaround with side effects), so none was
  made. Since `0c4e828d` the sensor logs `[DisplayTiming] Surface onset: ...` for every new swapchain binding at any
  log level (on screen by N ms, presents discarded before; [display-change-timing-tokens.md](display-change-timing-tokens.md)),
  so black loads and CE modes can be compared from ordinary sessions; the raw seal adds the DWM/Win32k detail.

## Open questions

- Not recorded when CE owns its display trace (elevated CE without the service): a leaked file-mode session would keep
  DxgKrnl unfiltered machine-wide, so that path needs the reclaim logic first. Logged once.
- History without `DCompCommitBatch` (`20261111_162241`, GTA 4K save load with DLSS-G): 256 MiB, 1.14 M events, span
  72 s, full DxgKrnl detail for the last 49 s (was ~30 s), nothing lost. Whether it keeps the run-up of a 37 s episode
  is open (the 10 s ongoing seal keeps the first 10 s).
- What DWM's batch-commit storm during GTA's save load is (one DWM thread, its own channel, ~63 k/s); it ran in clean
  loads of both modes, so it is not the black-screen discriminator.
- The CE-off baseline (plan step "matched cold start") still has no recorder: CE closed means no service lease. The
  CE-passive run is clean; a passive run seals nothing, so its DWM sequence is unrecorded.
- Why DWM, in some Windows sessions, does not take a new flip-model surface into the window's visual at bind time,
  what makes it do so at ~1.36 s (38 s in an earlier session), and whether CE contributes. Needs a black load sealed
  with the Win32k events (none since the 14:34 start), and `Surface onset` lines from several loads per Windows
  session in both normal and observer mode.
