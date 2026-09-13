# llm-wiki Log

### 2026-09-13 - Front-loading has to budget the GPU half too, or it buys nothing

Run `20260913_132320` showed the overrun controller doing its job - `overruns=3` total, `headroomUs` decaying
23 -> 2 us, late-frame rate back to 0.30% from 0.63% - and the 1%/0.1% low only recovering 84.9 -> 85.0 and
83.2 -> 84.0. So missed deadlines were not the main jitter source.

The two timelines disagreed, which is the clue. On the PRESENT timeline front-loading was already BETTER than the
back edge (stddev 194 -> 169 us, |frame-to-frame delta| 189 -> 126 us, p99.9 11911 -> 11740 us). On the DISPLAY
timeline it was worse (published 1% low 86.9 -> 85.0, stddev 148 -> 202 us). The overlay publishes percentiles from
`m_display` (screen times) when the effective source is `DisplayChange`, and screen time is the right thing to
measure - so the regression was real and the CSV comparison was the misleading one.

Root cause: the budget covers the CPU half of a frame, but the flip cannot happen until the GPU half finishes.
Strange Brigade DX12 is GPU-bound - ~1.8 ms CPU in front of ~8.5 ms GPU - so a CPU-sized budget released the game
far too late and the GPU ran past the deadline. `presentToDisplay` rose 0.4 -> 6.8 ms, and with the screen time then
set by GPU completion instead of by CE's grid, the game's own frame-to-frame variance landed directly on the display
timeline.

The algebra says the placement is worthless below that threshold. With L = input-to-photon, B = budget, W = whole
CPU+GPU work, F = irreducible flip latency: `B >= W` gives `L = B + F` and grid-pinned screen times; `B < W` gives
`L = W + F` and GPU-driven screen times. Shrinking B below W buys **zero** latency and pays for it in jitter. The
optimum is exactly `B = W`. Numbers for this session: W ~= 8.8 ms (independently consistent with the p2d excess and
with the measured latency delta), so the optimum budget ~= 9.1 ms costs ~0.3 ms against the current 2.1 ms budget
and buys back the whole percentile regression, while still sitting ~2 ms below the back edge.

Also note the published latency estimate over-reports the front-load gain: `anchorToPresent` is
`modelled base interval + measured hold`, so CE's own hold is counted twice. The true back-edge-to-front-load gain
is ~0.8 ms, not the 2.2 ms the overlay showed. The estimator limitation is documented in
`system_latency_frame_begin.h`; it was deliberately NOT touched here, because changing the measurement in the same
change as the behaviour would make the next A/B unreadable.

Fixes:
- `ResolveFrontLoadGpuExcessUs()` / `UpdateFrontLoadGpuHeadroom()`: grow the reservation by the measured excess of
  present-to-display over its own floor, fed from `PerformanceMetrics::ConsumeDisplayTiming` (one call; the overlay's
  own metrics are untouched). `DecayFrontLoadGpuHeadroomUs()` walks it back in by one timer margin per clean
  64-frame window - a bounded probe, not a proportional decay that would periodically put the GPU a large step past
  the deadline just to discover it no longer needs to be there.
- `HasUsableGpuCompletionEvidence()`: no seeded present-to-display floor, no front-loading. The floor may only be
  seeded while the placement is at the back edge, the only state in which the GPU is known to have finished before
  the present. Without the evidence the back edge stays the default and the withholding is logged.
- `SmartWait()` no longer arms the kernel timer for less than a scheduler tick. `EnsureTimerResolution()` puts the
  scheduler on a 1 ms tick and a shorter arm cannot land inside it. Invisible while the limiter's waits were whole
  milliseconds; front-loading made the pre-present wait hundreds of microseconds and the measured overshoot went
  from a 37 us median (212 us worst) on ~9 ms coarse waits to an 88 us median (561 us worst) on ~500 us ones.

Tests: GPU excess/decay/evidence tables, an integration case proving the reservation grows when presents start
waiting on GPU work, one proving no displayed-transition evidence keeps the back edge, and a sub-tick SmartWait
accuracy case. Hardware run pending: expect `gpuHeadroomUs` to climb for a few seconds then settle, `p2dUs` to fall
back towards `p2dFloorUs`, the published 1%/0.1% low back near the back-edge figures, and the latency estimate to
settle between the two previous runs.


### 2026-09-13 - Front-loading's cost was a sliding-window ceiling; capture sync opts out

Hardware run `20260913_130052` confirmed the placement change: `frontLoad=1`, `releases` climbing,
`releaseWaitUs` 8.4-8.9 ms, and the pre-present `scheduledWaitUs` collapsed from ~9300 us to 172-930 us. Published
PC latency fell 26.4 -> 24.2 ms.

Two things the run also settled:

**The gain is 2.2 ms, not 9.3.** `anchorToPresent` fell 20.5 -> 11.8 ms exactly as designed, but `presentToDisplay`
rose 0.4 -> 6.8 ms. Presenting earlier moves the frame's wait out of CE's sleep and into the flip queue; what is
left is a presentation-queue depth question, not a limiter-placement one. The same session's third-party front-edge
limiter published the identical 24.2 ms, so that is the current floor for this configuration.

**The 1%/0.1% low regression was real and had a precise cause.** Published 1% low 86.9 -> 84.9 fps, 0.1% low
86.5 -> 83.2, overlay frame-time stddev 148 -> 225 us, limiter late-frame rate 0.24% -> 0.63%. A budget of
`max(last 64 work samples) + margin` is by construction exceeded by roughly one in 65 later frames - over one missed
deadline per second at 90 fps, landing exactly where a 1% low is measured. The trace separates the two populations
cleanly: budget overruns of 24-455 us, against hitches of 6.9-718 ms.

Note the measurement subtlety: an arbitrary 2100-frame CSV window showed front-loading as slightly BETTER (stddev
171 vs 194 us). The overlay's own rolling `source_1pct_low_x100` / `source_frametime_stddev_us` columns over the
whole run are the like-for-like comparison and show the regression. Compare those, not a hand-picked window.

Fix: `GrowFrontLoadHeadroomUs()` raises the reservation to the worst sub-interval overrun observed while the
placement owned that frame, and `DecayFrontLoadHeadroomUs()` removes an eighth per clean 64-frame window and reaches
zero. Only a frame whose release actually ran can move it. This pays latency the game demonstrates it needs rather
than padding for everyone - and while the presentation queue still holds the frame for 6.8 ms, those microseconds do
not reach the screen at all.

Capture sync now opts out of front-loading entirely (`ShouldFrontLoadCadenceWait(..., usingCaptureSync)`): a missed
deadline there skips whole CFR grid slots via `AdvanceCaptureSyncDeadlineAfterLateFrame()` - a repeated frame in the
recording - rather than costing a fraction of a millisecond of frame time. While a recording is the product the
capture grid outranks input latency. Recording with capture sync therefore keeps the original back-edge latency by
design.

Tests extended in `tests/test_fps_limiter_front_load.cpp`: the headroom grow/decay tables including the
hitch-rejection bound and the decay actually reaching zero, the capture-sync exclusion in the eligibility table, and
an integration case proving capture sync arms no release. Hardware re-check pending: look for `overruns` settling,
`headroomUs` in the hundreds of us, and the published 1%/0.1% low back at the back-edge figures.


### 2026-09-13 - The limiter was holding finished frames: front-loaded cadence release

Follow-up run `20260913_124032` confirmed the dedup fix (`activeDedup=0`, `site=1 strictGrid=1`, `waited=120 late=0
avgFps=90.0`, `resets=0`, steady-state frame-time stddev 194-202 us against 648 us for a third-party front-edge
limiter in the same scene). It also exposed the next problem: the overlay reported ~26.4 ms PC latency against
~24.2 ms for that limiter.

The PC-latency chain decomposed it exactly. `latency = anchorToPresent + presentToDisplay + inputWait`, and the
inputWait term was identical (5.55 ms) in both. CE: `anchorToPresent=20.5ms presentToDisplay=0.4ms appQueue=6`.
Third-party: `anchorToPresent=11.2ms presentToDisplay=7.5ms`. With FG off the estimator records the same present
into both rings, so the 9.3 ms difference in `anchorToPresent` is `runtimePresent (ETW PresentStart) - CE hook
entry` - CE's own pre-present wait, measured. The perf CSV agrees: median frame delta 11.09 ms, median
`fps_limit_wait_us` 9.21-9.34 ms, CE in-hook total 0.10 ms, so the game built each frame in a median 1.78-1.94 ms
(stddev 135-169 us, max 3.1 ms) and then aged in CE's hook for the remaining 9.3 ms.

Root cause: the limiter's deadline decides when a frame is PRESENTED, and CE was also letting it decide when the
frame was BUILT - by placing the entire wait after the game had already finished rendering. The frame was therefore
9.3 ms old by the time it reached the runtime, and 0.4 ms later it was on screen.

Fix: `ApplyPostPresent()` now releases the game `ResolveFrameWorkBudgetUs()` before the next deadline, so the frame
is built last and presented immediately. The budget is the measured high-water of recent frame work plus the
adaptive timer margin (a ceiling, not a percentile: overrunning it makes the present late, and a late present
re-phases the general cadence). Crucially `localTargetTime_` and the pre-present wait are untouched, so this is a
latency control only - a skipped release, an unmeasurable work time, or a budget of a whole interval all degrade to
the original back-edge placement with the cap and the grid phase intact. Gated by `ShouldFrontLoadCadenceWait()` on
a grid-gated site that runs the post-present half, no active FG, and no explicit Reflex cadence owning the slot.

Expected effect on the measured chain: `anchorToPresent` falls to ~11.2 ms (the modelled interval plus a ~0.1 ms
hook gap) and the published estimate to ~17 ms. Note the estimator's own modelling limit either way - it models
input-to-present as one base interval when no marker exists, which over-states a front-edge loop whose real work is
1.8 ms. That limitation is documented in `system_latency_frame_begin.h` and is unchanged here; it affects the
absolute number, not the A/B.

New units `hook/common/fps_limiter_detail/{front_load,cadence_diagnostics}.h` keep `apply.h` under the size ceiling.
Tests: `tests/test_fps_limiter_front_load.cpp` (budget table incl. the not-measurable and does-not-fit cases, the
eligibility truth table, the placement moving under a unique-present site, the cap surviving a skipped release, and
duplicate-prone/FG sites keeping the back edge). Hardware run pending: a good run shows `frontLoad=1` with
`budgetUs` a few hundred us above `workCeilingUs`, `releases` climbing, `resets=0` still, and the PC latency sample
dropping by roughly the old `scheduledWaitUs`.


### 2026-09-13 - SB DX12 fps limiter: the 2 ms duplicate-present window ate 46 genuine frames/s

Session `20260913_122208` (Strange Brigade DX12, `FpsLimiter.general_fps=90`, `general_limiter_mode=basic`, inject
capture): the game presented ~130 fps with alternating short/long frame times while `fps_limiter_trace.log` reported
a flawless cadence - `waited=120 late=0 avgFps=90.0` every window. Both halves were true. Apply() ran 129 times/s
(2160 paced + 1126 `activeDedup` over 25.5 s, matching the 3448 perf-CSV present rows over 26.7 s and the overlay's
own `source_current_fps` of ~128.9). The escapes were the whole gap: ~88 paced/s + ~46 deduped/s = the observed rate.

Root cause: the non-boundary Apply() path classified a call as a duplicate present by wall clock - a 2 ms window
since the last Apply return. Strange Brigade DX12 renders a frame in 1-2 ms, so a genuine next present repeatedly
landed inside that window, returned without taking a grid slot, and reached the swapchain unpaced. The logged
`sinceReturnUs` values cluster at 1.1-1.95 ms, not the tens of microseconds a real Present+PresentEx duplicate
takes. The dedup never updates `lastApplyReturnQpc`, so the pattern is "paced frame, short unpaced frame ~1.5 ms
later, wait to the next slot" - exactly the short/long alternation the user saw.

The window could only misfire there: `IsRecursivePresent()` (`dxgi_shared_g_presentThreadId`/`presentDepth`) and the
`IsInWrapperPresent()` early return already reject every nested, cross-thread and wrapper-owned re-entry before
`ExecutePresentCore` reaches Apply(), so a second Apply() for one presented frame is structurally impossible on the
DXGI path. This is the same defect already fixed for Strange Brigade Vulkan in 2026-08 (`gateEveryPresent`), which
never reached the D3D sites.

Fix: `Apply()`'s second parameter is now `ce::fps_limiter_policy::PresentSite` - a structural call-site contract
instead of a bool. `kDuplicateProne` keeps the legacy window for sites whose second call genuinely is the same frame
(DXVK Present+PresentEx, the D3D9/D3D8/DDraw/OpenGL wrappers). `kFinalOutputBoundary` is the old `gateEveryPresent`,
unchanged, and stays the only contract allowed to own `OutputGroupAdmission`. The new `kUniqueApplicationPresent`
covers the four DXGI top-level boundaries (`DetourPresent`, `DetourPresent1`, `CWrapDXGISwapChain::Present`/
`Present1`): the duplicate window is skipped entirely and every entry takes a cadence-grid slot under the blocking
cadence lock.

Deliberate boundary: while frame generation is producing, a `kUniqueApplicationPresent` site keeps the established
window. That DXGI stream also carries runtime-owned generated presents (FFX presents an interpolated frame from its
own proxy swapchain) that CE cannot classify structurally there yet; gating every entry would spend a base-rate grid
slot on a generated present AND block the runtime's presenter thread inside CE's cadence lock - the documented FFX
freeze class. This is not the rejected `strictGrid = boundary && !FGActive` escape from Portal RTX: a real
final-output boundary stays unconditionally strict. Classifying generated presents on the DXGI path the way the
Vulkan boundary does is the open follow-up.

Diagnostics: the `LOCAL timer cadence active` / `LOCAL timer start` lines now carry `site=` and `strictGrid=`.
A fixed run must show `activeDedup=0` in the DX12 stats lines.

Tests: `tests/test_fps_limiter_present_site.cpp` (the bug as a requirement - an immediate second Apply on a unique
site must take a grid slot; the FG-active site must keep the window; the inactive fast path must not stall; pure
policy table for `ShouldGateEveryApplyOnCadenceGrid`) and a `PresentPacingPolicySourceTest` that pins the contract
at all four DXGI call sites. Hardware run pending.


### 2026-09-13 - DX12 dynamic glyph boxes were an upload/allocator ownership race

A supplied DLSS-G 4x screenshot showed the first `3` of the graph's dynamic `33 ms` ceiling label as a box while
the adjacent identical `3` rendered correctly. The shared ASCII atlas and CPU text construction therefore could not
explain the per-instance failure. Static lifetime reconstruction found that the x64 descriptor-free backend reused
four persistently mapped VB/IB slots independently of PostSL's fence-selected pool of up to 16 command allocators,
while PostSL disabled the upload guard. At generated-output cadence the CPU could wrap the smaller ring and rewrite
vertices still being read by the GPU; digit-count and string changes made those mixed bytes visible.

The descriptor-free, textured, normal, and PostSL paths now share a 16-slot allocator/upload lifetime domain and
force each draw's upload slot to its proven-complete allocator index. PostSL publishes the precise next overlay-fence
value before recording and signals that same value after submitting the list. Missing descriptor-free coupling uses
the guarded ring only with a live/nonzero completion guard; otherwise the draw is refused and rate-limit logged.
Focused `DX12UploadSlotGuardTest` coverage passes. The first complete verification run exposed a known
scheduler-sensitive FPS-limiter test: it required every admitted callback to spend at least 3 ms asleep even though
a callback arriving after its cadence deadline correctly returns immediately and re-bases. The no-FG integration
test now checks deterministic boundary, group-owner, generated-slot, and concurrency-skip counters instead of elapsed
wall time. The final `--verify --skip-updates --concise` gate passed on build `0.1.6527`, including both hook
architectures, the full native and Python suites, clang-tidy/file-size ratchets, and ASan/UBSan. Proprietary-driver
and game confirmation remain pending at this entry.

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
