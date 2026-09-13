# llm-wiki Log

### 2026-09-13 - CE was taking NVIDIA's native Vulkan present path away from the game (two causes)

**Report**: with the inject active, DOOM Eternal always presented through DXGI even with the driver's
Vulkan/OpenGL present method set to prefer native - visible as the Windows volume OSD compositing over the game.
Without CE it stayed native. Session `20260913_180809` confirms it from CE's own side: the driver called
`CreateSwapChainForHwnd` *inside* `vkCreateSwapchainKHR` (18:08:25.359, between the layer's entry at .097 and the
driver's return at .374), which CE logged three times as `Vulkan layer owns presentation - exact DXGI
swapchain-create pass-through`. That line is the authoritative "NVIDIA's WSI went layered" signal.

**Reproducer**: `build/vk-wsi-probe` (standalone, ~15 s, not part of any gate). It creates an ordinary Vulkan
FIFO swapchain and traces the ICD's own imports (`nvoglv64.dll` IAT: `GetProcAddress`, `LoadLibrary*`,
`GetModuleHandle*`, plus the GDI pixel-format and `D3DKMTEnumAdapters2` entries). The two paths are trivially
separable from inside the process:
- **native**: the ICD resolves `wglDescribePixelFormat`/`wglCreateLayerContext`/`wglShareLists`/`wglDeleteContext`/
  `wglMakeCurrent`/`wglSwapLayerBuffers`/`wglGetCurrentContext`, then loads `nvppex.dll` (`ppeGetVersion`,
  `ppeGetExportTable`) and `dispbroker.dll`/`winsta.dll`. It never touches D3D.
- **layered**: the same run additionally resolves `dxgi!CreateDXGIFactory2`, `d3d12!D3D12CreateDevice`,
  `dwmapi!DwmGetCompositionTimingInfo` and `dcomp!DCompositionCreateDevice3`, and maps `nvwgf2umx.dll`,
  `nvldumdx.dll`, `d3d12core.dll`, `dcomp.dll`.
Module-presence alone is not a detector: OBS's `graphics-hook64.dll` and RTSS's `rtssvklayer64.dll` map `dxgi.dll`
into every Vulkan process here regardless.

**Cause 1 - the Streamline preload (`streamline_dll_path`)**. `PreloadConfiguredGraphicsRuntimeDlls` mapped
`sl.interposer.dll`, `sl.common.dll`, `sl.dlss*.dll` into *every* injected process whose profile configured any
DLSS/Streamline override path, as a name-registration trick so later name-based loads resolve to CE's copies.
DOOM Eternal never loads Streamline. The probe reduces it to a single fact: **mapping `sl.interposer.dll` alone is
enough** for the ICD to build the layered presenter - it is how Vulkan DLSS-G has to present. The `nvngx_*.dll`
snippets are inert (probe `--preload-ngx`: native). Bisected away from every other suspect first: CE's added
device/instance extensions (`VK_KHR_external_memory_win32`, `external_semaphore_win32`, `timeline_semaphore`,
`get_physical_device_properties2`), the reserved overlay queue, DOOM's `imageUsage=0x1f`,
`VK_EXT_full_screen_exclusive`, a real D3D12 device plus DXGI flip swapchain in the process, CE's `opengl32`/
`gdi32` swap-entry inline hooks, and CE's `GetProcAddress` router (traced: it never intercepts the ICD's own
lookups) - all stayed native.
- **Fix**: `ce::graphics_runtime::ShouldPlaceStreamlinePluginSet` + `PlaceStreamlinePluginSet` in
  `hook/main_redirect.cpp`. The sl.* set is placed only once the process shows Streamline use - the core is
  already mapped, `sl.interposer.dll` ships beside the process image, or a sl.* load/request has been observed
  (`NoteStreamlineUseObserved`, latched from `GetRedirectedPath` and `NoteRuntimeModuleLoadedForOverridePolicy`).
  The deferred half runs from the hook thread's 100 ms monitor loop (`PlaceConfiguredStreamlinePluginSetIfObserved`),
  off the loader-lock path. The NGX snippets keep their eager placement. The loader redirect is unchanged and still
  serves the first real request, so a Streamline game gets the same copies as before.

**Cause 2 - `vsync_mode=fifo|adaptive`**. The layer asked for `VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT` on every
forced-FIFO swapchain (`abcafbeb`). The probe separates the pieces: the device extensions (`VK_EXT_present_timing`,
`VK_KHR_present_id2`, `VK_KHR_calibrated_timestamps`) and the feature node are **inert**; the swapchain flag alone
flips the path, because NVIDIA's native presenter cannot serve a present-timing swapchain.
- **Fix**: `ShouldEnableSwapchain` now also requires `meteredPresentationPossible` - the application enabled
  `VK_NV_present_metering` on this device (recorded at `vkCreateDevice`). That is the only case where a FIFO
  swapchain can outrun its display; a plain FIFO swapchain already waits for the vertical blank, so the flag bought
  nothing and cost the native presenter.

**Validated**: `installed/testapp/vulkan_test.exe` with a DOOM-shaped profile (`vsync_mode=fifo`,
`streamline_dll_path`, `dlss_sr_dll_path`) - `nativeTiming=0`, `flags=0x0`, zero DXGI swapchain-create
pass-throughs. The hold-back path is proven separately with the probe process (no Streamline beside it):
`Runtime preload: sl.* plugin set held back ... (mapped=0 shippedWithApplication=0 loadObserved=0)` and no sl.*
module in the process. The test app ships the whole sl.* set next to its exe, so it takes the
`coreShippedWithApplication` branch and still gets the full placement - the intended positive. **DOOM Eternal
itself is still unrun.**

**Open**: `hook/wrappers/d3dkmt_hook.cpp` types `D3DKMT_HANDLE` as `UINT64` (it is `UINT32`), so every
`D3DKMT_QUERYADAPTERINFO` / `D3DKMT_QUERYVIDEOMEMORYINFO` field it reads is at the wrong offset - the garbage
`Type=3086977864, Size=52413` lines in the DOOM log are a pointer's low dword and the adapter LUID. Harmless while
the VRAM override is off (every hook passes the application's struct through untouched), but the override path
writes at those offsets. Not this bug; not fixed here.

### 2026-09-13 - DOOM Eternal black window: overlay views outlived their swapchain

Session `20260913_174040`: the first launch stayed black, the second worked. The layer log shows the startup
swapchain destroyed at 17:41:45.923 and the replacement created at .927, CE tearing its overlay state down at
.942-.951 inside the next `InitializeOverlay`, and the first present on the new swapchain failing with
`Vulkan Prerender: wait failed result=-4` plus `device loss latched from submission-slot fence probe` at 46.134.
The Windows System log pins the cause between the two: `nvlddmkm` event 153 ("Error occurred on GPUID: 700") at
45.9535, i.e. inside CE's own teardown. Capture was not involved - `RetireCaptureSwapchain` only moves state to a
retired list and owns no swapchain-derived objects.

Root cause: the overlay builds a `VkImageView` per presentable image, a `VkFramebuffer` over each, and compute-route
descriptor sets and command buffers bound to them, and CE released all of it at the *next* `vkCreateSwapchainKHR`.
Presentable images die with their swapchain, so between the game's destroy and its next create CE held views over
freed images and then handed those stale views back to the driver. That is a use-after-free, which is exactly why
the second launch of the same build survived the identical sequence.

Fix (0.1.6537): `Capture_vkDestroySwapchainKHR` now calls `ReleaseOverlayForSwapchain` before the driver destroy,
gated by `ce::overlay_swapchain_lifetime::Decide` - release only the state whose recorded `OverlayState::swapchain`
is the one being destroyed, and skip the device-idle wait on a latched device loss. `OverlayState` gained the
`swapchain` field that makes that identification possible. Seven regression tests cover the policy plus the source
ordering (release before `fp_vkDestroySwapchainKHR`, and `InitializeOverlay` recording the owning swapchain).
`--verify` passed. Hardware re-check pending: a cold DOOM Eternal start has to survive the startup swapchain
recreate several times over, and the log should show `Releasing overlay state built over swapchain ...` instead of
`InitializeOverlay - Existing state found`.

### 2026-09-13 - DOOM Vulkan compute-present capture and authoritative freeze evidence

DOOM Eternal recording `20260913_163446` is a healthy 3840x2160/120 inject capture from a source capped near
140 FPS. The compute-present compositor stayed active after the live swapchain moved from graphics family 0 to
compute family 2. Present cadence averaged 7.148 ms with a 9.442 ms maximum gap; capture CPU averaged 40.8 us
(p95 50 us). The 62.875 s output has exactly 7,545 video packets, no encoder skip/duplicate/backpressure, and two
48 kHz audio tracks of exactly 3,018,000 samples each. Strict analysis found no media/audio/visual fault; only the
bounded startup-publication backlog and external-overlay contexts.

The configured 140 FPS general cap plus disabled capture sync intentionally produces timestamp-nearest 140-to-120
decimation: 1,258 candidates were superseded, with zero missing CFR slots/duplicates and a 3.726 ms maximum residual.
For absolute motion uniformity, capture sync multiplier 1 is preferable because it makes source and output cadence
120-to-120; retaining 140 is a valid gameplay-latency/source-choice tradeoff.

The trace exposed two generic hot-path issues. The common one-semaphore Vulkan overlay/capture/present chain allocated
three temporary vectors per captured frame; it now uses inline storage and retains allocation only for uncommon
multi-wait submissions. The swapchain also changed present family without recreation after bounded prerender topology
learning had ended, which could leave `cpu_prerender_limit=1` attached to the startup route. A stable queue now costs
one atomic comparison, while a live family move retires the cached producer decision and safely re-arms bounded
dependency learning. Focused capture/overlay/prerender tests pass.

Freeze session `20260913_154630` confirms a separate false-positive family. Vulkan presents stopped normally at
15:57:01 and resumed on the same game instance at 15:59:33, but a historical D3D12 ECL helper heartbeat kept the old
watchdog armed and it dumped at 15:57:33. The named last-present worker (tid 20308) was merely waiting on an idTech
event; its stack contained no CE, Vulkan, or driver stall. While the Vulkan layer owns final presentation, only a
currently published `vkQueuePresentKHR` is now authoritative: a truly stuck call remains published and targetable,
whereas a returned worker cannot trigger a timeout dump. Worker-pool target-switch logs are rate-limited and status
reports historical versus current evidence explicitly. Focused watchdog policy tests pass.

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
