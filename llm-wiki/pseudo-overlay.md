# Pseudo-Overlay

Last cross-checked: 2026-05-16 (build 0.1.3213)

Primary sources:
- `captureengine/pseudo_overlay.h`
- `captureengine/pseudo_overlay.cpp`
- `common/config.h` (`PseudoOverlayConfig`)
- `common/config.cpp` (`process_list` parsing)

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

- Single `WM_TIMER` fires every **500ms** (`kTimerInterval`, `pseudo_overlay.h:128`)
- `TimerProc` calls `OnTimerTick()` which drives all state checks and `UpdateOverlay()`

## NOT RECORDING Warning Logic

Every timer tick in `OnTimerTick()` (`pseudo_overlay.cpp:524-552`):

1. If mode 1 or 2: call `IsForegroundTarget()`
2. If foreground process matches `process_list` AND not recording → activate blinking warning
3. Blink pattern: 2s visible / 1s hidden (3000ms cycle)
4. If match lost or recording starts → deactivate warning

## Foreground Process Detection (`IsForegroundTarget`)

`pseudo_overlay.cpp:293-347`:
1. Returns false immediately if `processList` empty
2. Gets foreground window via `GetForegroundWindow()`
3. Gets PID via `GetWindowThreadProcessId()`
4. Caches result per PID for 2 seconds (function-level statics)
5. Opens process with `PROCESS_QUERY_LIMITED_INFORMATION`
6. Gets exe name via `QueryFullProcessImageNameA`
7. Normalizes (lowercases, trims quotes/whitespace)
8. Compares against pipe-delimited `config_.processList` items

## process_list Config Parsing

`common/config.cpp:976-1001` — supports two formats:

**Pipe-delimited (single line):**
```
process_list=Game1.exe|Game2.exe
```
Handled directly at line 985: `NormalizePseudoOverlayProcessList(rest)`.

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

## Debug Logging

All pseudo-overlay diagnostic logging uses `LogDebug` (only visible at debug/trace log levels):
- `IsForegroundTarget`: processList value, foreground PID, OpenProcess/QueryFullProcessImageNameA errors, normalized exe name vs list items, match result, cache hit/miss
- `OnTimerTick`: warning activation/deactivation with reason
- `UpdateOverlay`: mode/isRecording/warnVisible/ghost/shouldHaveVisible, suppression reason, warning window position/size

## Source Anchors

| Component | File | Lines |
|-----------|------|-------|
| Config struct | `common/config.h` | 274-284 |
| Config parsing | `common/config.cpp` | 976-1001, 1380-1397 |
| Timer interval | `captureengine/pseudo_overlay.h` | 128 |
| Implementation | `captureengine/pseudo_overlay.cpp` | 1-1257 |
| Tests | `tests/test_config.cpp` | 221-285 |

## Open Questions / Stale-risk

- Stale-risk: low. Core logic is stable. Debug logging added in build 0.1.3213.
- `SetTimer(NULL, ... TimerProc)` — timer fires correctly via `PeekMessage`/`DispatchMessage`. Verified by timer message counts in diagnostic logs.
