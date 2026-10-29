# Background Presentation-Stall Recovery (`WindowHeartbeat`)

Last cross-checked: 2026-10-29 (unit tests; detection and trace steering seen on hardware with Fortnite/EAC, 0.1.7377;
no real stall yet, so the nudge ladder is unverified on hardware).
Stale-risk: medium - the failure was never reproduced on demand; which ladder step helps is unknown until it recurs.

## Problem and why the old heartbeat could not help

With multi-plane overlays, a background window (browser, or a borderless game limited to e.g. 30 fps in the
background) can stop updating until its window is resized. The user suspects crash reporters such as Unreal's then
take the stalled game for a hang and terminate it. Reported weeks before 2026-10-29; no repro on demand.

The first version (2026-10-05, `31c3a956`) sent an asynchronous `WM_NULL` every 250 ms. That only wakes a message
loop. A window that a resize unsticks is still pumping messages (a resize needs the window's thread), so the stuck
part is presentation/composition: the resize changes the window's geometry, which makes DWM re-decide how the window
is presented (overlay plane, independent flip, composition) and makes the game recreate its buffers. Unreal's hang
detection measures game/render thread and Present progress, which no window message satisfies. Never hardware-tested.

## Current design (2026-10-29)

- Same config keys (`[WindowHeartbeat] enabled`, profile `WindowHeartbeat.enabled`), opt-in, process-backed
  profiles only, independent of capture/DesktopOverlay; see [configuration.md](configuration.md).
- **Signal**: `common/platform/present_progress.{h,cpp}` `Tracker` reduces Microsoft-Windows-DXGI `Present_Start`
  (0x2a) / `PresentMultiplaneOverlay_Start` (0x37) and their stops (0x2b / 0x38) per tracked process and thread.
  A thread whose newest event is a start older than the threshold is stalled. A paused or throttled game has no open
  call, so it is never stalled. No stall is judged until some process's stop arrived (`returnsDelivered`): an older
  elevation service enables starts only. Lost trace buffers clear open calls. Threads per process capped at 32.
- **Threshold**: `GpuTimeoutUs()` = `HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers\TdrDelay` (Windows'
  documented 2 s default when unset). A healthy flip-model Present blocks at most queued frames x sync interval.
- **Trace**: `captureengine/display_timing/present_progress_feed.{h,cpp}` `TraceFeed` takes a display lease on the
  elevation service's shared trace (read-only consumer, the `TraceConsumer` lifetime shared with process discovery)
  and routes only the DXGI events. `common/platform/display_timing_etw.h` enables the stop ids in the one shared
  provider set; display timing ignores them. No controller-owned fallback session (needs admin, leaks when the
  controller is killed). Without the service: `Present trace unavailable ... stall recovery is inactive`.
  `FeedControl` holds the lease only while a profiled process has a background window; a refused start is retried
  when that set changes or after a running trace ended, never per tick. Wired by the composition root
  (`runtime/core/runtime_create.cpp` -> `RuntimeParts::makePresentFeed`, approved boundary exception).
- **Selection** (`window_heartbeat_recovery.cpp`): visible, non-minimized, uncloaked, top-level frame windows
  (not child/tool/no-activate; captioned windows now qualify) with a non-empty rect; exact case-insensitive
  executable match; per process the largest such window. CE's own process excluded. Unknown foreground fails closed.
  Process names are cached per process id and pruned to enumerated processes.
- **Ladder**, one attempt per stalled call (pid, thread, start QPC), only while the process is not foreground:
  1. Move +1 px (`SetWindowPos` with `SWP_ASYNCWINDOWPOS | SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER`,
     physical pixels on a per-monitor-v2 worker thread). No size change, so no `ResizeBuffers`.
  2. Once the window shows the moved rect, restore the original; once restored, observe one tick.
  3. Still open: 1 px narrower and back (the manual resize the user reported), observe one tick.
  4. Still open: `Present still blocked ... no further attempt for this stall`; the attempt waits for the call.
  A `WM_NULL` probe (one outstanding per window) is sent with each nudge. A window that keeps its geometry is judged
  only after its thread answered a probe sent after the nudge and a tick passed; a thread that pumps nothing never
  escalates. Activation by the user restores a pending 1 px change and ends the attempt; a geometry change by anyone
  else ends it without restore. "Resumed" = the stalled call's thread returned and the process's return count moved.
- No activation, input, hooks, injection, process handle beyond limited query, or game memory access
  (`WorkerCannotActivateWindowsSimulateInputOrAccessGameMemory` source check).

## Diagnostics

`[WindowHeartbeat]` lines: `Present stall pid=... presentThread=... openMs=... thresholdMs=... lastIntervalMs=...
mpo=0|1 window=... rect=...`, `move|resize nudge queued`, `... left the geometry unchanged although the window
thread answered`, `Present resumed pid=... after the move|resize nudge (sinceStallMs= probeReplyMs=)`,
`Present still blocked ...`, `Recovery ended ...` (closed/hidden, foreground, foreign geometry change), trace
start/release/refusal. A `ChangeGate` summary (60 s) reports windows/background/recovering/trace state and
starts/returns/losses. `mpo=1` means the stalled call was `PresentMultiplaneOverlay`. `probeReplyMs=-1` means the
window thread never answered, i.e. the message loop was blocked too (the first design's assumption).

## Validation and open questions

Unit coverage: `PresentProgressTest` (stall/return, pause, no-returns guard, loss, longest thread, cap, reset),
`PresentProgressFeedTest` (event routing), `WindowHeartbeatTest` (cadence, healthy/paused never touched, move
recovery, escalation and give-up, ignored nudge waits for the probe, foreground restore, exclusions, largest window,
refused placement, foreign change, name cache, stale probe, trace steering, native async placement on another
thread without activation, native `WM_NULL`).

Hardware, session `20261029_181926` (0.1.7377, profile `forti` on `FortniteClient-Win64-Shipping.exe`, EAC, not
injected): `configured enabled=1 ... stallThresholdMs=2000 trace=service` (TdrDelay unset). Four background periods;
each took the service lease on the tick the game lost the foreground (`Consuming the service-owned display trace`),
released it on refocus (`Present trace released: no profiled process has a background window`). `returnsDelivered=1`
within 250 ms of the first lease, so the installed service enables the stop ids. Every summary had
`presents == returns`, `losses=0`, `recovering=0`; one 119.8 s period counted 3595 presents (30.0 fps, the game's
background limit). No stall, no nudge. Teardown residue matched earlier sessions without the trace.

Unverified: whether the real stall shows an open Present call (a game blocking before Present on a frame-latency
waitable object, Vulkan or D3D9 presents emit no DXGI events and are not covered), and which step helps. When it
recurs with the profile on, copy the `[WindowHeartbeat]` lines here before the session folder is deleted. Not done:
D3D9/Vulkan coverage (would need kernel queue-packet tracking), a controller-owned trace fallback.
