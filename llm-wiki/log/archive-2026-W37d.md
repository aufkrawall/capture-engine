# llm-wiki Log Archive 2026-W37d

### 2026-09-12 - DLSS-suspended PostSL capture and screenshot ordering

Static reconstruction found one shared transition seam behind ignored inject screenshots and overlay pixels in
`capture_include_overlay=false` recordings. A real PostSL callback can remain the exact output/overlay owner after a
game suspends DLSS-G for a cutscene, but final-output capture previously required the active FG signal. The callback
could therefore draw first while capture either disappeared or later used the nominally overlay-free ProcessFrame
location. Screenshot routing had the complementary error: global PostSL active/confirmed latches made ProcessFrame
yield even when the current callback returned on scene cooldown or render-lock contention, leaving the request
Pending with no producer.

Real presented-output callbacks now choose a final-generated or suspended-base capture domain from the live DLSS-G
signal. Both domains retain same-queue before/after-overlay ordering; the suspended domain publishes ordinary base
metadata and shares the base cadence gate. A Present-scoped capture claim prevents duplicate ProcessFrame capture,
and base gating occurs only after Phase1 route selection. Screenshot ownership during suspension requires the actual
callback or a successful PostSL draw in this Present, while the post-ProcessFrame include path rechecks request state.
The D3D12 screenshot producer also derives its device from the exact backbuffer, retains and validates the submission
queue's COM device identity, passes a live swapchain rather than a released `IDXGISwapChain3`, completes post-claim
readback setup failures explicitly, and emits rate-limited stage diagnostics. Focused DXGI, final-output, and
screenshot policy/regression suites pass; proprietary-driver/game validation remains pending.

### 2026-09-12 - Screen grab privacy: Virtual desktop switching and Task View privacy blackout improvements

Investigated delayed/unreliable video blackening under `black_when_no_fullscreen_focus=true` with `dxgi_dup` monitor capture when switching Windows virtual desktops (`Win+Tab` hotkey or desktop navigation). Root causes identified:
1. When switching virtual desktops, Windows cloaks windows on inactive desktops using DWM cloaking (`DWM_CLOAKED_SHELL` 0x02 via `DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, ...)`), but `IsWindowVisible()` remains `TRUE` and `IsIconic()` remains `FALSE`. Because Windows focus handover to the new desktop lags by 100-500 ms, `TryClassifyWindowFullscreenLike()` previously continued treating the invisible/cloaked game window on Desktop 1 as an active fullscreen foreground window while `dxgi_dup` was already duplicating the physical display showing Desktop 2.
2. `Win+Tab` opens Task View (`XamlExplorerHostIslandWindow` / `Windows.UI.Core.CoreWindow`, owned by `explorer.exe` or `xamlexplorerhost.exe`), which spans the entire monitor and was previously classified as `fullscreenLike = true`.
3. Inactive desktop backgrounds (`WorkerW` / `Progman`) and taskbars could be classified as fullscreen-like if focused on Virtual Desktop 2.
4. In monitor-scope capture (`dxgi_dup`), once a game establishes verified fullscreen focus on the captured monitor, switching to another virtual desktop that contains another fullscreen window (e.g., browser or document viewer) previously had no process continuity check while the target game remained alive.

Fixes implemented:
- Added `IsWindowCloaked()`, `IsWindowOnCurrentVirtualDesktop()`, and `IsIgnoredShellWindow()` to `common/screen_grab_privacy.*`. `TryClassifyWindowFullscreenLike()` now immediately rejects cloaked windows, windows not on the current virtual desktop, and shell/system UI classes/processes (`explorer.exe`, `xamlexplorerhost.exe`, `shellexperiencehost.exe`, `startmenuexperiencehost.exe`, `searchhost.exe`, `textinputhost.exe`, `WorkerW`, `Progman`, etc.).
- Added target continuity tracking (`capturedMonitorWindow_`, `capturedMonitorPid_`) to `ScreenGrabPrivacyRuntime`. In monitor-scope capture, once verified fullscreen focus is established, only windows from that same application/process can satisfy fullscreen focus while that target window remains alive. If the target closes, the lock releases cleanly.
- Added regression tests in `tests/test_screen_grab_privacy.cpp` for cloaked, virtual desktop, and shell class rejection.
- Validation: Complete `--verify --skip-updates --concise` gate passed with zero regressions (content-validated build, all unit tests, Python self-tests, clang-tidy ratchet clean, ASan/UBSan clean).
