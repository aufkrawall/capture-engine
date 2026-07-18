# Pseudo-Overlay

Last cross-checked: 2026-07-18 (instant recording-start feedback + dedicated UI thread)

Primary sources:
- `captureengine/pseudo_overlay.h`
- `captureengine/pseudo_overlay.cpp`
- `common/pseudo_overlay_focus_grace.h`
- `common/pseudo_overlay_focus_grace.cpp`
- `common/pseudo_overlay_visibility.h` (`ShouldPseudoOverlayBeVisible` pure policy)
- `common/recording_indicator_policy.h`
- `common/shared_defs.h` (`RecordingStartIntent` runtime flags)
- `common/config.h` (`PseudoOverlayConfig`)
- `common/config.cpp` (`process_list` parsing, `foreground_acquire_grace_ms`)
- `tests/test_pseudo_overlay_thread.cpp`
- `tests/test_pseudo_overlay_visibility.cpp`

## Overview

Controller-side overlay for WGC capture (no injection required). It uses two layered top-level popup windows (`WS_EX_LAYERED | WS_EX_TOPMOST`) running entirely in `captureengine.exe`. A dedicated pseudo-overlay message thread owns window classes, windows, GDI resources, the timer, shared-memory mappings, and every render/update call. Controller calls only publish synchronized desired state and post refresh messages.

- `hOv_` (`CE_PseudoOv` class): indicator circle (amber=start pending, red=recording)
- `hWarn_` (`CE_PseudoWarn` class): status/notification text (`STARTING RECORDING...`, `STARTING AUDIO...`, `NOT RECORDING`, encoder overload, or screenshot saved)

Both windows have `WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE` to prevent focus stealing.

## Modes

| Mode | Value | Behavior |
|------|-------|----------|
| `InformationIndicator` | 0 | Amber pending or red live indicator circle only |
| `WarningAndIndicator` | 1 | Indicator plus amber startup text or `NOT RECORDING` warning |
| `WarningOnly` | 2 | Amber startup text or `NOT RECORDING`; windowless during established recording except sanctioned notifications |

## Timer

- A thread timer fires every **500ms** and drives shared-state polling, focus/anchor policy, warning blinking, notifications, and rendering.
- Controller intent, config, screenshot, and overload changes post `kMsgRefresh` to the UI thread, so hotkey feedback does not wait for the next timer tick.
- `Init` waits at most five seconds for a readiness event after the thread creates its message queue and UI resources. `Shutdown` posts `kMsgShutdown`, joins the thread, and leaves all window/GDI/shared-memory teardown on the owning thread.

## Visibility Policy & Mode-2 "Inactive While Recording" Contract

Whether the overlay should have ANY visible window this update is decided by the pure helper
`ce::pseudo_overlay::ShouldPseudoOverlayBeVisible(...)` in `common/pseudo_overlay_visibility.h`
(unit-tested in `tests/test_pseudo_overlay_visibility.cpp`). The live `ShouldOverlayBeVisible`
wrapper in `pseudo_overlay.cpp` just fills the inputs and delegates.

Visibility terms:
- **Indicator** — pending or live recording state and `mode != 2`.
- **Startup text** — pending video/audio state and `mode != 0`.
- **NOT-RECORDING warning** — `warnVisible` only while the resolved state is idle.
- **Encoder-overload warning** — `showEncoderOverloadWarn && now < overloadWarnUntil`.
- **Screenshot notification** — `now < screenshotNotifyUntil`.
- **Ghost keepalive** — `alwaysRender` (explicit opt-in).

**Mode-2 contract:** with `mode=2` and a recording active, the overlay is inactive (no windows,
no `UpdateLayeredWindow`/`SetWindowPos`, no `EnsureOverlayWindows`) **except** the two sanctioned
notifications: encoder-overload and screenshot-saved. Those may spawn a short-lived topmost
window mid-recording (accepted as singular slight-stutter events). The steady-state timer does NOT
call `UpdateOverlay` in this state; only lightweight CPU polling runs (no window/plane work).

## Recording-Start State Contract

- The controller publishes `RecordingStartIntent::Video` or `AudioOnly` before media/limiter readiness waits and posts an immediate refresh. The shared intent stays active through encoder calibration and hidden-frame warm-up; it has no timeout and does not start the timer.
- The UI thread resolves shared `isRecording`, `audioOnly`, and start intent through `recording_indicator::SelectState`. Live state wins over a stale pending bit.
- Pending video renders amber `STARTING RECORDING...`; pending audio renders amber `STARTING AUDIO...`. A pending transition clears the blinking `NOT RECORDING` state and aborts foreground-acquire grace so feedback can render immediately.
- Pending startup text has priority over screenshot, overload, and `NOT RECORDING` text. Encoder-overload polling remains gated on established recording.
- Media clears the intent only when it publishes live recording, stop/cancel occurs, or startup fails. The controller also clears its direct-thread fallback on every readiness/command/child-exit/shutdown terminal path.
- Inject-overlay handoff suppression, `enabled`, mode, process list, anchoring, foreground behavior, and `alwaysRender` keepalive still apply.

## Message-Pump Health

The two topmost layered windows are now owned and pumped by a dedicated UI thread. Multi-second controller waits such as:
- record-start: `EnsureLimiterProcessReady(10000)` (`main.cpp` ~679, only if `captureSyncEnabled`)
- config-reload: `SyncLoggerAndSensorProcesses` (5s+5s) + `SyncLimiterProcess` (10s) (`main.cpp` ~1341)
- process teardown/sync: `ShutdownIpcChildProcess(…,5000)`, `WaitForSingleObject(processHandle,…)`

can still delay tray work and global-hotkey handling on the controller thread, but cannot starve pseudo-overlay windows after the hotkey has been handled. `[Controller] Main-thread blocked ...` now describes tray/hotkey impact. `[PseudoOverlay] UI-thread stall ...` reports a gap on the dedicated thread itself; it is no longer evidence of controller starvation. The disabled-window lifecycle test verifies cross-thread refresh and joined shutdown without timing sleeps.

## Foreground-Acquire Grace Period (Alt+Tab-in settle)

When the whitelisted PID (re)acquires foreground focus, the visible pseudo-overlay is
suppressed for `foreground_acquire_grace_ms` (default **2000ms**) before the first
`ShowWindow` / `SetWindowPos` / `UpdateLayeredWindow` call. This avoids racing Windows
MPO / DXGI fullscreen buffer rebinds on Alt+Tab-in, which on some drivers can freeze
the game window (a likely Windows MPO bug, not a CE code bug).

**What is delayed during grace:**
- `EnsureOverlayWindows()` window creation
- `ShowWindow(SW_SHOWNA)` indicator + warning
- `SetWindowPos` move/resize of the topmost popups
- `UpdateLayeredWindow` GDI composition

**What still runs during grace:**
- Sticky anchor refresh (`ResolveAnchorInfo()`) so the first post-grace frame is in-position
- Warning blink phase advance (driven by `OnTimerTick` before `UpdateOverlay`) so the first
  post-grace frame is in-phase with the 2s-on / 1s-off cycle
- All internal state transitions (warnActive_, lastOv_, etc.)

**Abort signals (commit immediately even during grace):**
- Recording indicator state change, including the initial pending transition — the
  `recordingStateChanged` flag in the helper short-circuits to `suppressVisibleOverlay=false`
- `foreground_acquire_grace_ms` config change — `UpdateConfig` resets the in-flight grace
  so the new value is honored on the next acquire

**Logging breadcrumbs** (in `pseudo_overlay.cpp`):
- `Foreground grace started pid=… grace=…ms (was: hadTarget=…, prevPid=…)`
- `Foreground grace elapsed pid=… waited=…ms`
- `Foreground grace aborted: focus_lost (was pid=…)`
- `Foreground grace reset: grace_ms changed N -> M`
- `Foreground grace skipped: grace_ms=0 (pid=…)`

**Config:**
```ini
[DesktopOverlay]
foreground_acquire_grace_ms=2000   ; 0 disables, 10s max
```

The pure policy helper `ce::pseudo_overlay::ComputeFocusGraceDecision(...)` in
`common/pseudo_overlay_focus_grace.h` is fully unit-testable without Windows APIs.
See `tests/test_pseudo_overlay_focus_grace.cpp` for the focused regression coverage.

**Asymmetry:** focus-out is NOT debounced — when the user Alt+Tabs AWAY from the
whitelisted game, the windows destroy immediately (current behavior). Only the
focus-IN path is debounced, which is the direction that races MPO / fullscreen rebinds.

## NOT RECORDING Warning Logic

Every timer tick in `OnTimerTick()`:

1. If mode 1 or 2: call `IsForegroundTarget()`
2. If foreground process matches `process_list` AND the recording state is idle → activate blinking warning
3. Blink pattern: 2s visible / 1s hidden (3000ms cycle)
4. If match is lost or recording becomes pending/live → deactivate warning
5. Foreground-grace tracking is updated first; the warning blink phase is advanced
   during grace so the first visible frame after grace is in-phase.

## Foreground Process Detection (`IsForegroundTarget`)

`pseudo_overlay.cpp:293-369`:
1. Returns false immediately if `processList` empty
2. Gets foreground window via `GetForegroundWindow()`
3. Gets PID via `GetWindowThreadProcessId()`
4. Caches result per PID for 2 seconds (function-level statics)
5. Opens process with `PROCESS_QUERY_LIMITED_INFORMATION`
6. Gets exe name via `QueryFullProcessImageNameA`
7. Normalizes (lowercases, trims quotes/whitespace)
8. Compares against pipe-delimited `config_.processList` items

`GetForegroundTargetPid()` is a lighter-weight variant that returns the raw foreground
PID without consulting the whitelisted process list — used to feed the grace policy.

## process_list Config Parsing

`common/config.cpp:978-1003` — supports two formats:

**Pipe-delimited (single line):**
```
process_list=Game1.exe|Game2.exe
```
Handled directly: `NormalizePseudoOverlayProcessList(rest)`.

**Multi-line parenthesized block:**
```
process_list=(
Game1.exe
Game2.exe
)
```
First-pass parser enters multi-line mode when `rest == "("` after Trim.

## Known Pitfalls

- **Trim charset**: `Trim()` default chars = `" \t\r\n\"()"`. For the process_list opener check, use `Trim(rest, " \t\r\n\"")` — omitting `()` — so `"("` is preserved as the comparison value.
- **`;` comments inside multi-line blocks are skipped** at `config.cpp:938-939` before reaching the pseudo-process-list handler. Entries starting with `;` are silently ignored.
- **Empty multi-line block**: If all entries are `;`-commented, `pseudoProcessList` stays empty, `pseudoProcessListSet` is true, and the `GetStr` fallback is blocked. Process list remains empty — no warning possible.
- **Grace transition tick renders**: On a mid-session PID change, the very first tick after the transition renders normally (so the overlay repositions onto the new monitor), and the *next* tick starts suppressing. This is the only intentional "violation" of the soft wait — a true 2-second freeze on PID change would make monitor-switch feel laggy. The focus-IN-from-another-app case (no prior whitelisted focus) is the one that always suppresses on the transition tick.
- **Grace never blocks startup feedback**: the resolved pending/live state change is passed to the helper as an abort signal, so the hotkey's pending indicator is committed immediately.

## Debug Logging

High-frequency pseudo-overlay diagnostic logging uses `LogDebug` (only visible at debug/trace log levels):
- `IsForegroundTarget`: processList value, foreground PID, OpenProcess/QueryFullProcessImageNameA errors, normalized exe name vs list items, match result, cache hit/miss
- `OnTimerTick`: warning activation/deactivation with reason
- `UpdateOverlay`: mode/isRecording/warnVisible/ghost/shouldHaveVisible, suppression reason, warning window position/size

`LogInfo` breadcrumbs for grace transitions (visible at info level):
- grace started, grace elapsed (with waited ms), grace aborted (focus_lost),
  grace reset (config change), grace skipped (grace_ms=0).
- resolved recording-indicator transitions and dedicated-thread startup/shutdown.

## Source Anchors

| Component | File | Lines |
|-----------|------|-------|
| Config struct | `common/config.h` | 274-289 |
| `foreground_acquire_grace_ms` config | `common/config.cpp` | ~1389 |
| Grace policy header | `common/pseudo_overlay_focus_grace.h` | full |
| Grace policy implementation | `common/pseudo_overlay_focus_grace.cpp` | full |
| Visibility policy (pure) | `common/pseudo_overlay_visibility.h` | full |
| Visibility tests | `tests/test_pseudo_overlay_visibility.cpp` | full |
| Recording-state policy | `common/recording_indicator_policy.h` | full |
| Instant intent publication | `captureengine/main.cpp` | `PublishRecordingStartIntent`, recording hotkey paths |
| Thread lifecycle / refresh | `captureengine/pseudo_overlay.cpp` | `Init`, `ThreadMain`, `PostRefresh`, `Shutdown` |
| UI-thread stall diagnostic | `captureengine/pseudo_overlay.cpp` | `OnTimerTick` (`lastTimerTickMs_`, `kPumpStallWarnMs`) |
| Controller block timer | `captureengine/main.cpp` | `MainThreadBlockTimer` + record-start/config-reload wraps |
| Pseudo-overlay header | `captureengine/pseudo_overlay.h` | full |
| Pseudo-overlay implementation | `captureengine/pseudo_overlay.cpp` | full |
| Grace state update + decision | `captureengine/pseudo_overlay.cpp` | `UpdateForegroundGraceState` / `EvaluateForegroundGrace` |
| Grace gate inside `UpdateOverlay` | `captureengine/pseudo_overlay.cpp` | after the inject suppression check, before `EnsureOverlayWindows` |
| Tests (config) | `tests/test_config.cpp` | 221-289 |
| Tests (grace policy) | `tests/test_pseudo_overlay_focus_grace.cpp` | full |
| Tests (thread lifecycle) | `tests/test_pseudo_overlay_thread.cpp` | full |

## Open Questions / Stale-risk

- Stale-risk: medium until the dedicated-thread/pending presentation receives fresh game/runtime validation. Pure policy, lifecycle smoke, focused native tests, product compilation, and full automated tests are authoritative for the covered boundaries.
- `SetTimer(NULL, ... TimerProc)` is created and destroyed on the dedicated thread and dispatched by that thread's `GetMessage` loop.
- 2s default is a heuristic. If the MPO/buffer rebind is observed to take longer on
  specific games/drivers, the user can raise `foreground_acquire_grace_ms` up to 10s.
  No upper bound beyond 10s to avoid hiding the indicator for a full minute if someone
  misconfigures it.
- Grace only applies to the pseudo-overlay; the inject (DX12 hook) overlay already
  has its own well-tested focus-loss/focus-acquire handling in
  `hook/common/dx12_overlay_policy.h` and the D3D12 wrapper. The two layers are
  intentionally independent.
