# llm-wiki Log Archive 2026-W34j

Covers 2026-08-22 (Streamline bridge lifecycle). Newest-first.

### 2026-08-22 - Device notification bypassed the legacy teardown boundary

Witcher 3 session `20260822_185158` started and rendered its loading screen, then raised a real
`0xc0000005` null read in the title at `123.exe+0x1610976`. The bridge had progressed much farther,
but the module and log timelines proved it still had not performed the requested NGX upgrade:

- Streamline 2.12.128 finished initialization at `18:52:05.590`; legacy shutdown and NGX retirement
  followed at `18:52:05.592`. Both retirement releases succeeded but both images stayed resident.
- The dump contained one SR and one FG image, but they were the game's `nvngx_dlss.dll` 3.1.1 and
  DriverStore `nvngx_dlssg.dll` 310.2.1. SL2 had requested the configured absolute paths, correctly
  reused the already-resident images rather than mapping duplicates, and thereby acquired the extra
  references which made the later safe retirement release insufficient.
- The bypass was `NotifyD3D12Device`: unlike every bridged import, the queue-derived device path
  called `EnsureRuntimeReady` directly while legacy quiescence was still pending.

Legacy quiescence is now an explicit Pending/Running/Complete/Failed state machine. Every V2 bring-up
route, including device notification, must cross it; `EnsureRuntimeReady` independently refuses to
load V2 until it is Complete. The bridge stays inactive throughout import rewriting, then publishes
Pending before Active; an early thunk entrant remains on V1 and cannot tear it down under the rewrite.
Concurrent game callers wait without polling. A same-thread call
re-entered by `slShutdown` stays on V1 so teardown cannot deadlock itself. After retirement CE
preloads and pins the configured SR/FG images, verifies every physical copy belongs to that folder,
and only then publishes Complete. The inventory is emitted before any V2 interposer load. The next
run should therefore show only configured 310.7.128 SR/FG images at that boundary.

That session's first translated DLSS evaluate returned `eErrorMissingConstants` six seconds before
the game-side null read. Causality is not established; first set-constants/evaluate diagnostics now
record input frame, actual V2 token frame, viewport and result so a remaining translation error can
be distinguished from the proven mixed-generation problem.
