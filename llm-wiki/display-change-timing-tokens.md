# Display change timing: compositor activity and composition tokens

Last verified: 2026-11-10 (code and unit tests; **no live ETW trace, no hardware run yet**). Stale-risk: high until
`sensors.log` of a GTA run shows `tokens(created=... otherProcess=...)` non-zero. Parent page:
[display-change-timing.md](display-change-timing.md).

## Summary

Composed episodes (`ComposedPresentation`) now count the compositor PID's present packets and the flips of them that completed (`compositorPresents`, `compositorFlips`; kept after the episode so the Stopped line can print them), and the Win32k composition tokens of followed processes are matched between creation (`TokenCompositionSurfaceObject`, 0xc9, emitted by the tracked process) and end state (`TokenStateChanged`, 0x12d, emitted for every windowed present on the machine: confirmed 4, retired 5, discarded 6) on (surface LUID, present count low 32 bits, bind id) in `display_timing_tokens.h`. Ids, keywords (`0x8000000400001000` covers both) and packed layouts come from PresentMon's `Microsoft_Windows_Win32k.h` (`external/SpecialK-main/.../present_mon/ETW`); the payload is decoded from the user data directly (`display_timing_etw.h`), and both the CE-owned and the elevation-service-owned session enable them through the one shared provider set. The health line gains `compositorPresents`/`compositorFlips` and `tokens(created confirmed retired discarded otherProcess)`; the Stopped and StopRefused expiry lines print the episode's counts. Purpose: GTA's 19.5 s black screen (session `20261110_182518`) showed `composed ready=2340 published=0`; zero compositor presents means DWM did not present, presents without attributed frames mean not composed or a failed association, discarded tokens mean DWM dropped the frames. **Not yet seen on a live trace**: if `tokens(created=0 ...)` stays 0 while a windowed present is composed, the events or their pid attribution are not what the header implies. Open: the token events are per-present on the whole machine while the service is leased; check the ETW thread's cost on a busy desktop.

## Source anchors

- `captureengine/display_timing/display_timing_composed.h` (`NoteCompositorPresent/Flip`, reset by `Begin`, kept after `End`),
  `display_timing_tokens.h` (`CompositionTokenTally`), `display_timing_health.h` (`DisplayExpiryEpisodeActivity`,
  `compositorPresents`, `tokens(...)`), `display_timing_service_events.cpp` (`HandleCompositionTokenCreated/State`),
  `display_timing_service.cpp` (`PruneSubmissions` reads the episode activity before `End`).
- `common/platform/display_timing_etw.h`: `kWin32kTokenCreated` 0xc9, `kWin32kTokenStateChanged` 0x12d,
  `kWin32kTokenKeyword`, the packed payload structs and `DecodeWin32kTokenCreated/State`; the one shared provider set
  (`EnableDisplayTimingProviders`) enables them for CE-owned and elevation-service-owned sessions alike.
- Tests: `tests/test_display_timing_tokens.cpp`.
- Consumers: `tools/log_digest.py` does not parse these lines specially; read `sensors.log` expiry and health lines.
