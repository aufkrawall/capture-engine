# Raw display trace for stall episodes

Last verified: 2026-11-11 (first live seal: GTA 0.1.7503 `20261111_021555`, 1.42 s silence, 256 MB file covering
72 s, no events or buffers lost; findings below)
Stale-risk: medium - one hardware seal so far; the 10 s ongoing seal and a CE-passive comparison are unrecorded.

Why: GTA V Enhanced 0.1.7500 (`20261110_222057`) presented ~131/s for 37 s after a save load while nothing reached the
screen (`[SCANOUT STALL] ep=1 ... durationUs=38066160 presents=4924`, sensors `composed ready=3563 published=0`). CE's
real-time display session decodes only the events its timing uses, so it could not say whether DXGI, DxgKrnl
submission, the compositor or the display driver stopped. The user asked for the evidence without running external
tools.

## What runs

- `common/platform/display_flight_recorder.h`: one circular file-mode ETW session (`CE_DisplayFlight_Elevation`,
  256 MB, 256 KB buffers, QPC clock, independent-session mode) with DXGI (`kRuntimeKeyword`, verbose), DxgKrnl
  (Base | Performance, verbose, unfiltered), Dwm-Core (`{9e9bba3c-...}`, all keywords, information), Win32k composition
  tokens only (0xc9/0x12d id filter - other Win32k keywords fire per window message), NVIDIA display schedule,
  Intel-PresentMon frame type, Kernel-Process. Seal = stop (ETW writes the file), move, restart.
- Elevation service (`elevationservice/service_flight_trace.cpp`): ops `StartDisplayFlightTrace` /
  `SealDisplayFlightTrace` (protocol values 10/11, `kDisplayFlightTraceCapability = 32`, no payloads, outcome in the
  answer - a reply error would drop the connection and the client's display lease). Start needs the connection's
  display lease; the recorder stops with the last flight lease or when the display providers go off. Work file and
  sealed files live in `<installation>\DisplayTraces`, created under the admin-only installation folder and refused if
  it is a reparse point; a sealed file gets read+delete for the installing user's SID. Limits: one seal per 2 s, 32 per
  recording; uncollected files older than 10 min are removed at the next start.
- CE sensor (`display_timing_service.cpp::StartStallTrace`): only with `log_level=trace` and the service trace.
  `display_stall_trace::Policy` (pure) makes the hook's `[SCANOUT STALL]` judgement from the sensor's own streams: the
  tracked process's DXGI PresentStart events (`OnPresent`, service_events.cpp) and every screen change published for it
  (`OnScreenChange` in `PublishPending`: own flips, and composed frames while the compositor is followed). A *silence*
  runs from the first present after the last change to the next change. It counts only while the process owns the
  foreground window (checked each 250 ms prune) and keeps presenting (a gap > 500 ms restarts it), with >= 8 presents.
  Sealed when the screen changes again after >= 1 s, and once at 10 s while it lasts (keeps the run-up in the circular
  file); at most 6 per session.
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
- Logs: sensors `[DisplayStallTrace] recording ...|sealed ep= pid= reason= silenceMs= silenceStartQpcUs= presents= file=
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
- The captures recur every ~1.0-1.6 s for the whole 72 s file (51 of them, slower under load). Each is followed within
  3 ms by a csrss present of model 5 (`D3DKMT_PM_SCREENCAPTUREFENCE`, the fence of a GDI screen read; its counter
  stood at ~6843, one per second since logon) and a composition present by explorer's thread named `Taskbar` (51/51,
  never at random times). Requester, identified outside the trace: TrafficMonitor 1.8.6, a taskbar speed meter that
  embeds a window in the taskbar and, with `auto_set_background_color` + `disable_d2d` + a transparent color (all on
  in the user's config), calls `::GetPixel(GetDC(NULL), ...)` beside the taskbar in its 1000 ms `MAIN_TIMER`
  (`CTrafficMonitorDlg::OnTimer`, upstream source) to match the taskbar color; it imports GetPixel/BitBlt and started
  at logon. The trace itself cannot name the GDI caller. CE does no screen reads while not recording. So the black
  time ended at the first GDI screen read after DWM switched the window to the new surface - which matches the
  1.37-1.70 s episodes; without such a reader nothing visible forces the realization (the 38 s episode of 0.1.7500 had
  no ETL; TrafficMonitor skips the read while it judges the foreground window fullscreen).
- Stage verdict: the frames stop in DWM, which does not bind a flip-model chain's new composition surface while the
  window's previous surface sits in independent flip on an overlay plane with kept content. DXGI, DxgKrnl submission,
  GPU completion and the token hand-off all ran. CE's share is unproven: no ETL with CE passive
  (`[Diagnostics] overlay_observer_only=1`) or off exists.
- Reading method: `etl_stall_digest.py` ranks the stall correctly (the flip streams' gaps) but the verdict needed
  payloads - Win32k `TokenStateChanged`/`TokenCompositionSurfaceObject` (surface LUID, present count, NewState),
  Dwm-Core `CompSurfInfo_*`, `SCHEDULE_SURFACEUPDATE` (first 8 bytes = surface LUID) and DxgKrnl
  `IndependentFlipTransition`, decoded with TDH property info (scratch decoder, not in the tool yet).

## Open questions

- Not recorded when CE owns its display trace (elevated CE without the service): a leaked file-mode session would keep
  DxgKrnl unfiltered machine-wide, so that path needs the reclaim logic first. Logged once.
- Event volume per second under DLSS-G at 4K, and whether 256 MB keeps the run-up of a 37 s episode, are unmeasured.
- The CE-off baseline (plan step "matched cold start") still has no recorder: CE closed means no service lease. A
  CE-passive run (`overlay_observer_only=1`, recorder still on) is the nearest comparison.
- Direct proof that TrafficMonitor is the reader (a sealed run with it closed, or with its background auto-color off,
  should show no `ETWGUID_RTCAPTURE` and a longer black screen).
