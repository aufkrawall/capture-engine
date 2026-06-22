# Pseudo-Overlay

Last cross-checked: 2026-06-23 (mode-2 record-start visibility leak fix + pump-stall diagnostics)

Primary sources:
- `captureengine/pseudo_overlay.h`
- `captureengine/pseudo_overlay.cpp`
- `common/pseudo_overlay_focus_grace.h`
- `common/pseudo_overlay_focus_grace.cpp`
- `common/pseudo_overlay_visibility.h` (`ShouldPseudoOverlayBeVisible` pure policy)
- `common/config.h` (`PseudoOverlayConfig`)
- `common/config.cpp` (`process_list` parsing, `foreground_acquire_grace_ms`)

## Overview

Controller-side overlay for WGC capture (no injection required). Uses two layered top-level popup windows (`WS_EX_LAYERED | WS_EX_TOPMOST`) running entirely in `captureengine.exe`.

- `hOv_` (`CE_PseudoOv` class): Indicator circle (red=recording, blue=idle)
- `hWarn_` (`CE_PseudoWarn` class): Warning text overlay ("NOT RECORDING", "Encoder overloaded!", "Screenshot saved!")

Both windows have `WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE` to prevent focus stealing.

## Modes

| Mode | Value | Behavior |
|------|-------|----------|
| `InformationIndicator` | 0 | Recording indicator circle only, no warning |
| `WarningAndIndicator` | 1 | Indicator + "NOT RECORDING" warning |
| `WarningOnly` | 2 | "NOT RECORDING" warning only, no indicator |

## Timer

- Single `WM_TIMER` fires every **500ms** (`kTimerInterval`, `pseudo_overlay.h:152`)
- `TimerProc` calls `OnTimerTick()` which drives all state checks and `UpdateOverlay()`

## Visibility Policy & Mode-2 "Inactive While Recording" Contract

Whether the overlay should have ANY visible window this update is decided by the pure helper
`ce::pseudo_overlay::ShouldPseudoOverlayBeVisible(...)` in `common/pseudo_overlay_visibility.h`
(unit-tested in `tests/test_pseudo_overlay_visibility.cpp`). The live `ShouldOverlayBeVisible`
wrapper in `pseudo_overlay.cpp` just fills the inputs and delegates.

Visibility terms:
- **Indicator** — `isRecording && mode != 2`.
- **NOT-RECORDING warning** — `warnVisible && !isRecording` (gated on `!isRecording`; see below).
- **Encoder-overload warning** — `showEncoderOverloadWarn && now < overloadWarnUntil`.
- **Screenshot notification** — `now < screenshotNotifyUntil`.
- **Ghost keepalive** — `alwaysRender` (explicit opt-in).

**Mode-2 contract:** with `mode=2` and a recording active, the overlay is inactive (no windows,
no `UpdateLayeredWindow`/`SetWindowPos`, no `EnsureOverlayWindows`) **except** the two sanctioned
notifications: encoder-overload and screenshot-saved. Those may spawn a short-lived topmost
window mid-recording (accepted as singular slight-stutter events). The steady-state timer does NOT
call `UpdateOverlay` in this state; only lightweight CPU polling runs (no window/plane work).

**Record-start leak fix (2026-06-23):** the NOT-RECORDING warning is mutually exclusive with
recording, but `warnVisible_` used to be cleared only by the next 500 ms timer tick. Starting a
recording while the warning was visible therefore left the layered topmost warning window up (and
re-rendered "NOT RECORDING") for up to ~500 ms into the recording. Fixed two ways: `SetRecordingState(true)`
now clears `warnActive_/warnVisible_` synchronously before `UpdateOverlay()`, and the policy helper
gates the warning term on `!isRecording` (single source of truth; the `showW` term in `UpdateOverlay`
is also gated defensively).

## Message-Pump Health (Finding B — game-freeze risk, diagnostics only so far)

The two overlay windows are `WS_EX_TOPMOST | WS_EX_LAYERED` and are owned by the controller's
**single main thread** (the timer is `SetTimer(NULL, …)` armed in `Init`; the windows are created
from `OnTimerTick` on that thread and pumped by the main `PeekMessage`/`DispatchMessage` loop). That
thread also performs multi-second **non-pumping** `WaitForSingleObject` waits:
- record-start: `EnsureLimiterProcessReady(10000)` (`main.cpp` ~679, only if `captureSyncEnabled`)
- config-reload: `SyncLoggerAndSensorProcesses` (5s+5s) + `SyncLimiterProcess` (10s) (`main.cpp` ~1341)
- process teardown/sync: `ShutdownIpcChildProcess(…,5000)`, `WaitForSingleObject(processHandle,…)`

While blocked, the topmost overlay windows stop pumping. A non-responsive topmost window on the
game's output can wedge a foreground / MPO fullscreen transition and present as a frozen game window
(fits the reported freeze "with and without WGC"; also explains why a mode-2 recording feels safe —
no topmost overlay window is alive then). Hotkeys (`RegisterHotKey(NULL,…)`) are on the same thread,
so they are also unresponsive during these waits.

Diagnostics added (no behavior change yet):
- `[PseudoOverlay] Message-pump stall: Nms between timer ticks` — emitted in `OnTimerTick` when the
  inter-tick gap ≥ `kPumpStallWarnMs` (1500 ms, 3× the 500 ms interval). Proves the pump was starved.
- `[Controller] Main-thread blocked Nms in <label>` — RAII `MainThreadBlockTimer` in `main.cpp`
  around the record-start limiter wait and the config-reload service sync (warns at ≥250 ms).

**Stale-risk / open:** the root-cause fix (dedicated never-blocking overlay UI thread, or pumped
controller waits) is **deferred** pending a real-hardware trace correlating a freeze with a pump
stall — the MPO freeze cannot be reproduced in-repo and the threading refactor must not introduce
races. This is a hypothesis with strong code evidence, not a confirmed-and-fixed bug.

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
- Recording state change (user toggles start/stop during the grace window) — the
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
[pseudo-overlay]
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
2. If foreground process matches `process_list` AND not recording → activate blinking warning
3. Blink pattern: 2s visible / 1s hidden (3000ms cycle)
4. If match lost or recording starts → deactivate warning
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
- **Grace never blocks recording aborts**: The `recordingStateChanged` flag in the helper always commits immediately, so the user never sees a "I pressed the hotkey but no indicator" regression.

## Debug Logging

All pseudo-overlay diagnostic logging uses `LogDebug` (only visible at debug/trace log levels):
- `IsForegroundTarget`: processList value, foreground PID, OpenProcess/QueryFullProcessImageNameA errors, normalized exe name vs list items, match result, cache hit/miss
- `OnTimerTick`: warning activation/deactivation with reason
- `UpdateOverlay`: mode/isRecording/warnVisible/ghost/shouldHaveVisible, suppression reason, warning window position/size

`LogInfo` breadcrumbs for grace transitions (visible at info level):
- grace started, grace elapsed (with waited ms), grace aborted (focus_lost),
  grace reset (config change), grace skipped (grace_ms=0).

## Source Anchors

| Component | File | Lines |
|-----------|------|-------|
| Config struct | `common/config.h` | 274-289 |
| `foreground_acquire_grace_ms` config | `common/config.cpp` | ~1389 |
| Grace policy header | `common/pseudo_overlay_focus_grace.h` | full |
| Grace policy implementation | `common/pseudo_overlay_focus_grace.cpp` | full |
| Visibility policy (pure) | `common/pseudo_overlay_visibility.h` | full |
| Visibility tests | `tests/test_pseudo_overlay_visibility.cpp` | full |
| Record-start warn-clear | `captureengine/pseudo_overlay.cpp` | `SetRecordingState` |
| Pump-stall diagnostic | `captureengine/pseudo_overlay.cpp` | `OnTimerTick` (`lastTimerTickMs_`, `kPumpStallWarnMs`) |
| Controller block timer | `captureengine/main.cpp` | `MainThreadBlockTimer` + record-start/config-reload wraps |
| Pseudo-overlay header | `captureengine/pseudo_overlay.h` | full |
| Pseudo-overlay implementation | `captureengine/pseudo_overlay.cpp` | full |
| Grace state update + decision | `captureengine/pseudo_overlay.cpp` | `UpdateForegroundGraceState` / `EvaluateForegroundGrace` |
| Grace gate inside `UpdateOverlay` | `captureengine/pseudo_overlay.cpp` | after the inject suppression check, before `EnsureOverlayWindows` |
| Tests (config) | `tests/test_config.cpp` | 221-289 |
| Tests (grace policy) | `tests/test_pseudo_overlay_focus_grace.cpp` | full |

## Open Questions / Stale-risk

- Stale-risk: low. Core logic is stable. Grace period added 2026-06-01.
- `SetTimer(NULL, ... TimerProc)` — timer fires correctly via `PeekMessage`/`DispatchMessage`. Verified by timer message counts in diagnostic logs.
- 2s default is a heuristic. If the MPO/buffer rebind is observed to take longer on
  specific games/drivers, the user can raise `foreground_acquire_grace_ms` up to 10s.
  No upper bound beyond 10s to avoid hiding the indicator for a full minute if someone
  misconfigures it.
- Grace only applies to the pseudo-overlay; the inject (DX12 hook) overlay already
  has its own well-tested focus-loss/focus-acquire handling in
  `hook/common/dx12_overlay_policy.h` and the D3D12 wrapper. The two layers are
  intentionally independent.
