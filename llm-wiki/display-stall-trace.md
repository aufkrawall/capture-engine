# Raw display trace for stall episodes

Last verified: 2026-11-11 (unit tests and the ETL reader against a local Windows ETL; no hardware stall recorded yet)
Stale-risk: high until a GTA run produces a sealed file - provider volume, the circular size and the seal timing are
unmeasured on hardware.

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
  `display_stall_trace::Policy` (pure) watches the submission-expiry episodes: an episode in which the compositor showed
  at most 2 of the process's frames per second is sealed once at 10 s (keeps the run-up in the circular file) and when
  it ends after >= 1.5 s; at most 6 per session. `DisplayStallTraceSealer` does the exchange and the move off the ETW
  thread; the file lands beside `sensors.log` as `display_stall_<pid>_ep<N>_<ongoing|ended>.etl`.
- Logs: sensors `[DisplayStallTrace] recording ...|sealed ep= pid= reason= episodeMs= shown= file= bytes= eventsLost=
  buffersLost= restart=|seal failed ...|move failed ...|no raw display traces ...`; service
  `[ElevationService] Display flight recorder on (... providers ...)|sealed (...)|off after N seal(s)`.

## Reading a sealed file

`python tools/tracing/etl_stall_digest.py <etl> [--pid <game> --pid <dwm>] [--by-pid] [--from-us N --to-us N]` (Windows'
own ETW consumer API through ctypes, no external program; self-test `tools/tests/test_etl_stall_digest.py`). It prints
lost-event counts and, per event stream, count, first/last and the largest silence in QPC microseconds - the unit of
CE's `qpcUs=` fields. The stage whose silence starts first over the episode is where frames stopped; compare with the
decision table in `log/recent.md` (2026-11-10 GTA entry).

## Open questions

- Not recorded when CE owns its display trace (elevated CE without the service): a leaked file-mode session would keep
  DxgKrnl unfiltered machine-wide, so that path needs the reclaim logic first. Logged once.
- Event volume per second under DLSS-G at 4K, and whether 256 MB keeps the run-up of a 37 s episode, are unmeasured.
- The CE-off baseline (plan step "matched cold start") still has no recorder: CE closed means no service lease.
