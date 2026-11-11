# Display change timing: compositor activity and composition tokens

Last verified: 2026-11-11 (code, unit tests and the token payloads of two live GTA traces; the fixed sensor has **no
hardware run yet**). Stale-risk: high until `sensors.log` of a run on 0c4e828d or later shows `tokens(created=...)`
non-zero and `Surface onset:` lines. Parent page: [display-change-timing.md](display-change-timing.md).

## Summary

Composed episodes (`ComposedPresentation`) count the compositor PID's present packets and the flips of them that
completed (`compositorPresents`, `compositorFlips`; kept after the episode so the Stopped line can print them). The
Win32k composition tokens of followed processes are matched between creation (`TokenCompositionSurfaceObject`, 0xc9,
emitted by the tracked process) and their state changes (`TokenStateChanged`, 0x12d, emitted for every windowed present
on the machine) on (surface LUID, present count low 32 bits, bind id) in `display_timing_tokens.h`. Ids, keywords
(`0x8000000400001000` covers both) and the packed layouts come from PresentMon's `Microsoft_Windows_Win32k.h`
(`external/SpecialK-main/.../present_mon/ETW`); the payload is decoded from the user data directly
(`display_timing_etw.h`), and both the CE-owned and the elevation-service-owned session enable them through the one
shared provider set.

## Version 1 and the zero counts (fixed 2026-11-11, `0c4e828d`)

- From 2026-11-10 to 0c4e828d every session logged `tokens(created=0 ... otherProcess=0)` (e.g. `20261111_024650`,
  `20261111_153313`) although the Win32k enable delivered (`input(retrievals=10969 ...)`): the handlers refused any
  event version but 0, and Windows 11 build 26300 emits both token events as **version 1** (manifest and the GTA ETLs).
- Version 1 only appends fields: TokenCompositionSurfaceObject `FlipInterval, DestWidth, DestHeight` after `BindId`,
  TokenStateChanged `EarlyComposition` after `BindId`; both payloads are 56 bytes for a 64-bit process. The existing
  structs are the prefix and decode both (test `VersionOnePayloadsFromWindowsBuild26300Decode` uses the raw bytes of
  `20261111_153313`). `IsDecodableWin32kTokenVersion` accepts up to `kWin32kTokenNewestDecodableVersion` (1); a later
  version is refused, counted (`tokens(... undecodable=N)`) and reported once
  (`[DisplayTiming] Win32k composition-token events of PID ... arrive as version ...`).

## How a token ends (from the GTA traces)

| Path | States | Counted as |
|---|---|---|
| composed and shown | InFrame -> Confirmed -> Retired, then Discarded on release | `retired` (release ignored) |
| independent flip | Completed -> InFrame -> Discarded, `IndependentFlip=1` (with TokenIndependentFlipSkipComplete) | `flipped` |
| not shown | Discarded without either | `discarded` |

`confirmed` counts tokens taken into a compositor frame. Before the fix a retired token's release would have counted as
`otherProcess`, and every independent-flip present as discarded. Presents a newer one replaced before the compositor's
next frame also end as `discarded`; a black screen is `discarded` with nothing retired or flipped - GTA's cold DLSS-G
loads took ~150 presents through Confirmed -> Discarded before DWM bound the new surface
([display-stall-trace.md](display-stall-trace.md)).

## Surface onsets

The first present of a (surface LUID, bind id) pair the process has not used among its last 8 starts an onset; it ends
at that pair's first retired or flipped present, or unshown when the same process starts yet another pair first. One
`sensors.log` line each, at any log level, at most 64 per session (`surfaceOnsets(logged unlogged dropped)` in the
health line):

- `[DisplayTiming] Surface onset: PID <pid> composition surface <luid> bind <id> first shown <ms> ms after its first
  present (by composition|independent flip; presents=<n> discardedBefore=<n>)`
- `... was left before any of its presents was shown (presents=<n> discarded=<n>)`

Expected: a healthy swapchain switch shows in tens of ms with `presents` 1-2 (the clean GTA references: 25 ms by
composition); the black loads would read ~1.4 s with ~150 discarded. A ResizeBuffers rebind is a new pair and logs too.
This is the per-load answer to "did DWM take the new swapchain late", without a raw trace or trace logging.

## Source anchors

- `captureengine/display_timing/display_timing_composed.h` (`NoteCompositorPresent/Flip`, reset by `Begin`, kept after
  `End`), `display_timing_tokens.h` (`CompositionTokenTally`, `SurfaceOnset`), `display_timing_health.h`
  (`DisplayExpiryEpisodeActivity`, `tokens(...)`, `surfaceOnsets(...)`, `LogSurfaceOnset`),
  `display_timing_service_events.cpp` (`HandleCompositionTokenCreated/State`, `LogFinishedSurfaceOnsets`),
  `display_timing_service.cpp` (`PruneSubmissions` reads the episode activity before `End`).
- `common/platform/display_timing_etw.h`: `kWin32kTokenCreated` 0xc9, `kWin32kTokenStateChanged` 0x12d,
  `kWin32kTokenKeyword`, `IsDecodableWin32kTokenVersion`, the packed payload structs and `DecodeWin32kTokenCreated/State`;
  the one shared provider set (`EnableDisplayTimingProviders`) enables them for both session owners.
- Tests: `tests/test_display_timing_tokens.cpp`.
- Consumers: `tools/log_digest.py` does not parse these lines specially; read `sensors.log` onset, expiry and health
  lines. Token counts feed only logs, no timing decision.
- Open: the state events are per-present on the whole machine while tokens are tracked; the cost on a busy desktop is
  unmeasured (a linear search over at most 256 tracked tokens per event).
