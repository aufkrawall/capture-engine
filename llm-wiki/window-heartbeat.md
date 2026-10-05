# Background Window Heartbeat

Last cross-checked: 2026-10-05 (implementation; real game/MPO efficacy unverified).

## Sources and configuration

- `common/config/application_profile.h`: per-process profile setting.
- `common/config/config_load.cpp`: global default and qualified profile resolution.
- `common/platform/window_heartbeat.h`, `.cpp`: selection, cadence, asynchronous delivery and worker ownership.
- `captureengine/app/main_entry.cpp`: controller startup, accepted config reload and shutdown.
- `tests/test_window_heartbeat.cpp`, `tests/test_config_window_heartbeat.cpp`: behavioral/native/config coverage.
- `captureengine/config.ini.template`: shipped reference. No game-specific profiles are added.

```ini
[WindowHeartbeat]
enabled=false

[Profile.My Game]
process=game.exe
WindowHeartbeat.enabled=true
```

The global default applies only to process-backed profiles. A profile's explicit
`WindowHeartbeat.enabled=false` overrides a global true. Only the qualified key is
read inside profiles: a bare `enabled` must not activate this feature accidentally.
Title-only profiles are excluded. Removing the key on reload restores the global default.

The feature is independent of DesktopOverlay, recording and video routing. A canonical
profile with only `process` and `WindowHeartbeat.enabled` has no video/injection route.
For configurations with other injection-capable features, `dll_injection=never` remains
the explicit profile safety lock; enabling this heartbeat itself adds no injection target.

## Selection and lifecycle

- Fixed 250 ms cadence on a separate controller-owned worker, with no catch-up bursts.
- Exact case-insensitive executable basename match, following the process profile override contract.
- Top-level visible, non-minimized, uncloaked windows whose client area covers their monitor
  within the existing fullscreen geometry tolerance. Captioned, child, tool and no-activate
  windows are excluded. The DWM cloak query resolves from System32 at runtime
  and fails closed when unavailable. Fully occluded background windows may still qualify if Windows reports
  them visible; this feature does not attempt to alter occlusion or presentation mode.
- The foreground process and CE's own process are excluded. Unknown foreground state fails closed.
- Enumeration failure sends nothing and preserves outstanding requests. HWND/PID/TID and foreground
  process are checked again before sending, including rejection of same-thread synchronous dispatch.
- `SendMessageCallbackW(WM_NULL, 0, 0)` returns asynchronously for the foreign window thread.
  The worker pumps completion callbacks and permits only one outstanding request per window.
  A stalled recipient cannot accumulate periodic messages or block CE. Delivery errors clear
  the failed request and can retry at the next ordinary cadence; access-denied/UIPI is not bypassed.
- Tokens are never reused during a worker lifetime. HWND reuse or delayed callbacks cannot clear
  a newer request. Outstanding requests remain tracked across profile disable/re-enable.
- Config publication and ticks share a mutex, so disabling stops new submissions once the
  update returns. A previously queued message cannot be recalled. Disabled workers wait on
  their wake event/message queue without a periodic timer. Shutdown wakes and joins the worker.
- No window activation, simulated input, Windows hooks, DLL injection or target memory access.
  Executable identity uses the existing limited-query process interface.

## Diagnostics and limits

`[WindowHeartbeat]` logs configuration, worker failures/shutdown, and metered first-PID/window
plus eligible/sent/pending/failed/error summaries. Pending counts eligible windows skipped
because a request is still outstanding. Repeated summaries use `ce::log_meter::ChangeGate` with a
60-second summary interval; no per-heartbeat success noise or window titles are logged.

WM_NULL is a message-thread wake-up aid, not a foreground/active-state override. It cannot
force a paused game loop, satisfy Unreal's internal progress watchdog, or interrupt a GPU,
fence or unrelated synchronization wait. Successful submission is not proof of game progress.

Primary API references: [WM_NULL](https://learn.microsoft.com/en-us/windows/win32/winmsg/wm-null),
[SendMessageCallbackW](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendmessagecallbackw).

## Validation and stale risk

Deterministic tests cover exact cadence/no bursts, state exclusions, case-insensitive exact
matching, disable/removal, failed delivery/enumeration, backpressure and HWND reuse. A native
hidden message-only window on another thread verifies WM_NULL completion while the receiver remains inactive. A source-boundary
test rejects focus activation, input simulation, hooks, injection and memory-access APIs.
Worker tests exercise disable, joined stop and restart without sleeps or target response waits.

Real game/MPO hang prevention, anti-cheat acceptance and the impact of waking a game's own
work remain unverified. Reproduce the reported failure with the profile toggle off/on and
compare message responsiveness, actual game progress and the game's hang/error report.
