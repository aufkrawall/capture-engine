# llm-wiki Log Archive 2026-W38e

### 2026-09-14 - DOOM Eternal black window again: the overlay's present semaphores outlived nothing

Session `20260914_122133`, build 0.1.6561, black window, no crash. Same shape as `20260913_174040`
(`nvlddmkm` 153 at 12:27:18.2572, then `Vulkan Prerender: wait failed result=-4` and
`device loss latched from submission-slot fence probe` on the first present of the replacement swapchain) - but
this time the 0.1.6537 release-before-destroy ordering was in place and had run:
`Releasing overlay state built over swapchain 0000019E8FF100D0 before the driver destroys it (deviceIdleWait=1)`
at .223, `Partial cleanup complete` at .255, the fault at .257. So the image views were not the whole story.

`perf_metrics_25856.csv` holds exactly two frames and pins the discriminator: the overlay composited **once**, into
the startup swapchain at .158, and the game destroyed that swapchain 15 ms after the present. Session
`20260913_193606` (0.1.6542) confirms it from the other side: its first two swapchain destroys carried no composite
at all and did not fault, while the third came after ten composited presents and faulted 1 ms into the teardown.
A destroy that follows composites faults; one that does not, does not.

Root cause: every composited present waits on one of `OverlayState::semaphores` - `RenderOverlay` returns the
slot's binary semaphore and the present hook rewrites `pWaitSemaphores` to it (`chainedWaitSemaphore`). That wait
is executed by the presentation engine, and `vkDeviceWaitIdle` does not prove it has run: it covers queue
operations, not a present already handed to the presentation engine. CE's own submission-ring reuse says exactly
this ("the fence proves the submission retired and says nothing about the present that waits on the semaphore",
which is why slot reuse gates on the acquire generation), but both teardown paths destroyed the whole ring behind
a device-idle wait anyway.

Fix (0.1.6562): `overlay_present_semaphore_lifetime` in `overlay_swapchain_lifetime_policy.h`. The ring's
semaphores are moved into a deferred store tagged with the swapchain they were presented against instead of being
destroyed with the state, by both `CleanupOverlayState` (swapchain destroy) and `CleanupOverlay` (the
`oldSwapchain` retirement path, which also runs while presents can be pending).
`Capture_vkDestroySwapchainKHR` drains the matching batch **after** the driver's `vkDestroySwapchainKHR` returns -
the one point that proves no pending present of that swapchain can still be waiting - and `Capture_vkDestroyDevice`
drains the rest. Note the asymmetry that makes both rules hold at once: image views and framebuffers must die
*before* the driver destroy because the presentable images die with the swapchain; the semaphores must die *after*
it. Nine regression tests cover the policy plus both source orderings. Hardware re-check pending: a cold DOOM
Eternal start has to survive the startup swapchain recreate repeatedly with
`destroyed N overlay present semaphores held past swapchain ...` in the layer log and no `nvlddmkm` 153.

### 2026-09-14 - GPU load was the sum of concurrent engines, so it lived on the 100 clamp

Follow-up question from the same Portal RTX run: with the rendered-rate ceiling in place, 4x holds the base at
**36.0 groups/s against 47.8 at 3x** (measured from the present bursts in `perf_metrics_1620.csv` of
`20260914_120049`) - a quarter of the render work removed - and the overlay's GPU load did not move off ~100%.

`captureengine/host_metrics.cpp` summed **every** non-video `\GPU Engine(*)\Utilization Percentage` instance on the
adapter and clamped the total to 100. Those instances are per (process, adapter, physical engine) and the engines
run **concurrently**, so the sum is not a fraction of elapsed time. Frame generation is the case that makes it
unreadable: the generator's work is on compute, the game's raster on 3D, and the two add up past the clamp whatever
the GPU is really doing. Scene changes still moved the number whenever the sum happened to fall below 100, which is
why it looked responsive.

Fixed: `metrics_policy::ResolveAdapterGpuLoadPercent` sums **within** one engine - where processes do time-share it -
and takes the **maximum across** engines, which is what Task Manager reports and the only aggregation that stays a
fraction of elapsed time. `ParseGpuEngineKey` keys the grouping on the instance name from `phys_` onward, so an
engine's identity does not depend on which process used it and a second physical engine of the same type stays its
own engine. Degenerate readings (no `phys_` token, negative, non-finite, PDH's occasional >100) are handled
explicitly. Covered in `tests/test_host_metrics_policy.cpp`. Hardware run pending.

Worth carrying forward: **a metric that saturates cannot be validated by watching it respond.** This one tracked
scene changes convincingly and was still wrong by construction in exactly the regime that mattered.

### 2026-09-14 - 4x MFG was never the problem: a metered generator that outruns its panel stops metering

Portal RTX session `20260914_114142` (0.1.6559, `vsync_mode=fifo`, 3840x2160 @ 144 Hz VRR). The user reported the
overlay's frame-time graph looking "rendered twice" after an in-game 3x -> 4x step. The graph was faithful; the
screen series really is a comb.

The session walks 3x/4x/2x/3x/4x inside one running game, so every multiplier is measured against the same scene:

| multiplier | output | screen-time stddev | displayJag | 1% low | `nvFlipSchedule` lead |
| --- | --- | --- | --- | --- | --- |
| 3x | 144.1/s | 253 us | 217 us | ~54-115 fps | 6127 us |
| 2x | 132.5/s | 905 us | 541 us | 53.7 fps | - |
| **4x** | **162.5/s** | **6980 us** | **8866 us** | **54.2 fps** | **< 10 us** |

`nvFlipSchedule avgDelayUs` is cumulative in the health line; differencing `avg x applied` across windows gives the
per-window lead, and the 11:42:57-11:43:07 window (pure 4x, 1628 flips) comes out under 10 us against 6127 us for the
pure 3x window. **Zero announced lead is the driver not scheduling its flips at all.** `publishedInterval` for that
window is `p50=2300us p99=18400us` around a 6142 us mean - three images inside one refresh and then a wait, which is
exactly the comb in the screenshot.

The discriminator is not the multiplier, it is the refresh. Base rates fell with the multiplier (66/48/41), so
2x -> 132 and 3x -> 144 fit the panel while 4x asks for 41 x 4 = **164 fps on a 144 Hz panel**. Talos at the same
4x multiplier, whose 135/s fits, reads `avgDelayUs=8952 stddevUs=1074` (`20260913_184745`): 4x meters fine when it
fits. The 3x validation in `20260913_201259` looked perfect only because 48 x 3 landed on 144 exactly.

`hook/vulkan_layer/vulkan_present_metering_policy.h` had already written down the gap in its own words - "nothing
now bounds a metered generator that outruns its display. That ceiling belongs on the *rendered* rate, which is the
unit the metering spreads". This is that consequence arriving, and restating the vertical blank on the final DXGI
flip cannot re-spread a schedule that was never spread.

**Fix (0.1.6560): own the ceiling on the rendered rate.** When CE's present-mode override stands down for a metered
generator, `ResolveVblankCeilingOutputFps` states the display's own maximum refresh (read from the swapchain
window's display mode - `EnumDisplaySettingsW(ENUM_CURRENT_SETTINGS)`, never a configured constant) as an *output*
rate, and the limiter divides it by the LIVE multiplier: `refresh / multiplier` rendered frames per second make the
generator's metered batch exactly one image per vertical blank at every multiplier. It is a new simultaneous
constraint (`LimiterConstraintSource::kDisplayVblankCeiling`), never a replacement - a stricter capture-sync or
general cap still wins, compared in the same final-output domain. It carries no mode of its own, so AUTO hands it to
NVIDIA's own frame-generation-aware `minimumIntervalUs` where Reflex is active and CE adds no wait at all.

Publication is scoped to the metered device: a create on a device that never enabled the extension says nothing
about the generator's bound and must not clear one, because the limiter is per-process while swapchains are not.

**VALIDATED on hardware the same day**, session `20260914_120049` (0.1.6560, same game, same panel, walking
3x/4x/3x/2x/4x/3x):

| multiplier | output | screen-time stddev | displayJag | announced flip lead |
| --- | --- | --- | --- | --- |
| 3x | 143.6-144.1/s | 269-588 us | 215-469 us | 6357-6498 us |
| 2x | 107.9/s | 383 us | 273 us | 4526 us |
| **4x** | **143.6-144.2/s** | **269-1062 us** | **215-847 us** | **6880-7475 us** |

4x now reads like 3x. `publishedInterval meanUs=6942-6981 p50=6900` is one image per vertical blank, and the
announced lead is back from under 10 us to ~7 ms - the generator is scheduling its flips again. The limiter chose
`sync=vblank-ceiling, limiter=reflex, target=144, effective=36|48|72, group=144/4|3|2, driver=144` and armed
`Vulkan Reflex ... native driver pacing handoff`, so NVIDIA's own interval does the pacing and CE adds no wait at
all. The 0 -> 144 -> 0 -> 144 arming sequence in `vulkan_layer.log` is Remix creating a FIFO chain and then
recreating it as IMMEDIATE for DLFG: clearing on the FIFO one is correct, the WSI owns that wait. Residual: the one
10 s window containing a 2x -> 4x switch reads `stddev=3438us`, a transition artifact inside the window; the next
window is clean.

### 2026-09-14 - The chain was never the problem, the queue was: overlay back on Smooth Motion's output flip

0.1.6555 kept the game alive by moving CE's overlay onto the application-facing chain, and that was one step too
far. Two consequences were wrong: the overlay was interpolated along with the game frame, and it drew BELOW Steam
and RTSS instead of above them.

`dx12_overlay_policy/ffx_routing.h` already records the real cause, by the exact HRESULT: `DXGI_ERROR_ACCESS_DENIED`
(0x887A002B) is **cross-queue backbuffer access**, from the late-inject DLSS-G case (`20260811_221202`). CE was not
wrong to draw on NvPresent64's output chain - RTSS draws there too, which is why its overlay is not interpolated.
CE was wrong to submit that overlay on the GAME's queue while drawing into buffers owned by the interposer's queue.
`Execution discovery retained queue=...8CC1B00 instead of auxiliary=...115396C0` is CE rejecting the only correct
queue, and `scQ=0000000000000000` is the association it never recorded.

0.1.6559: the interposer create records its queue, and the overlay is routed onto it - the same rule as
`kUseFSRSwapchainQueue` ("pSwapChain is FSR's swapchain, backbuffers belong to FSR's queue"). The overlay submit
enters that queue's live ECL chain rather than the raw D3D12 entry, for the same reason it already must on an FSR
queue. The application-facing wrapper stays a pure pass-through so the frame is composited exactly once, and still
counts the application's presents for the cadence measurement. Where CE never observed the interposer's create it
has no safe queue, does not draw there at all, and falls back to the application-facing chain.

Result: CE's overlay is topmost (its deep body hook is below the dxgi entry Steam and RTSS patch, so CE draws last)
and not interpolated (the driver has already generated the frame). Hardware run pending.

### 2026-09-14 - Forced FIFO under Smooth Motion: state the vertical blank on the interposer's own flip

I first reported forced vsync as out of reach here. That was wrong, and `vulkan_dxgi_fifo_policy.h` says why in its
own words: the 2026-08-30 sessions that measured "forcing FIFO unpaces a metered generator" **also forced the Vulkan
present MODE**, and that is what collapsed NVIDIA's announced flip lead from 6842 us to 141 us. Stating the interval
on the final flip of an already-correctly-spread group is vertical-blank synchronization; quantizing an unpaced
burst was the judder. That is the Portal RTX fix validated on 2026-09-13 (144.0 presents/s on 144 Hz, stddev
~360 us, no tearing).

Smooth Motion is the same shape. CE touches nothing above NvPresent64, so its generated pair arrives at DXGI already
spread by the driver's own metering - `SyncInterval=0 Flags=512 (DXGI_PRESENT_ALLOW_TEARING)` IS that scheduling.
`vsync_mode=fifo` is therefore applied with the same pure contract, `ApplyFinalDxgiFifoParameters`, on the
interposer's output present only: `SyncInterval=1` with `ALLOW_TEARING`/`RESTART`/`DO_NOT_WAIT` cleared. The
input-side override is withheld there. `off` and `mailbox` are left alone; the interposer's own parameters already
are those. Hardware run pending.
