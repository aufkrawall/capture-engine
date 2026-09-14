# llm-wiki Log

### 2026-09-14 - Gothic II loading screens, and the DX7 overlay that costs the GPU nothing

Session `20260914_180020` (0.1.6578) proved the flip-path fix: `kind=1`, the composite target is the attached back
buffer, a 512x320 region instead of the 3840x2160 frame, no `PresentEx` fallback, and 600 composites in 5.38 s -
112 per second where the full-surface round trip had managed about 17. The overlay still disappeared on loading
screens, and that was a second bug in the same policy.

**A flip chain's front buffer is still the screen.** `ClassifyBlit` suppressed every blit whose destination
belonged to a flip chain, on the reasoning that Flip publishes those images. That is true of a *back* buffer and
false of the primary: a blit onto the surface being scanned out is visible the moment it completes. Gothic II
draws its loading screens exactly that way - the 3D scene is not running, so nothing flips and the progress
display is blitted straight onto the primary - and CE composited nothing for the whole load. The predicate is now
`destIsBackBuffer` (DDSCAPS_BACKBUFFER without DDSCAPS_PRIMARYSURFACE) for "published later", with
`destOwnsFlipChain` kept only to decide *where*: such a blit's source can be a static image the application blits
again unchanged, so the overlay goes into the visible surface after the blit rather than being stamped into that
source.

A presentation-mix counter set now logs every ten seconds (`DDraw: Presentation mix ... flips= blitPresents=
directScanoutBlits= ignoredBlits= scanoutUnlocks= composites= skippedNoPublishedImage= skippedOutsideOverlay=`).
DirectDraw has no single present entry point, so a route that composites nothing is otherwise indistinguishable
from one that is never called - which is precisely what this looked like in the log.

**The overlay now draws with the game's own Direct3D 7 device.** The D3D9Ex composite, even region-scoped, ends in
`GetRenderTargetData`, which blocks the render thread until the GPU has caught up - a hard CPU/GPU serialization
point on the game's own present path, once per present, plus two video-memory locks. A DX6/DX7 title already owns
a device that draws transformed, alpha-blended, textured triangles, which is exactly what the overlay's draw list
is. `CustomOverlay::D3D7Backend` converts the shared vertices to `D3DTLVERTEX`, uploads the font atlas once as a
managed ARGB8888 texture surface, and issues the same handful of `DrawIndexedPrimitive` calls the DX9 backend
issues - no readback, no second device, no staging surfaces, nothing leaving the GPU. `TryDrawNativeLegacyD3DOverlay`
takes it only when the device's current render target *is* the surface this presentation publishes; anything else
keeps the D3D9Ex composite, which works on any surface. State is saved and restored with a `D3DSBT_ALL` state
block, and the overlay suppresses itself rather than running without one.

Three things had to come with it. The legacy `d3d.h`/`d3dtypes.h` headers redefine enumerators `d3d9.h` also
defines, so they exist in exactly one translation unit and the backend's interface is `void*`; the few device
methods the hook itself calls go by vtable index, pinned by `LegacyD3D7VTableAbiTest` in the only test that sees
the real declaration. CE's own state calls bypass the forced-filtering interception through
`LegacyD3DInternalScope` - that layer caches what it believes the *application* asked for, and the overlay's
sampler states are not the application's. And the D3D9Ex helper device is now created only when the composite
route actually needs it, with the adapter LUID the host's telemetry wants published from a device-less
`Direct3DCreate9Ex` instead.

Hot-path work that came out of the same pass: `TrackLegacyD3D7Device` runs from `SetTextureStageState` - once per
material - and took a mutex on every call, so the unchanged case is now a relaxed atomic compare;
`ActivateDirectDrawSurface` runs from every hooked Flip, Blt, BltFast and Unlock and cost a `QueryInterface`, a
lock and two hash lookups each time, now memoized per thread against an association generation; and the native
path's render-target identity check compares pointers before falling back to COM identity.

Measured, for the record: `cpu_prerender_limit` defaults to -1 and neither branch of `ApplyPrerenderLimitDDraw`
runs at that value, so CE adds no flip-status spin of its own. Built and verified at 0.1.6580. **Hardware run
pending** - no Gothic II session has exercised the native D3D7 backend yet, and the loading screens need a
re-check with the presentation-mix counters in the log.

### 2026-09-14 - DirectDraw overlay flicker: the composite was on the wrong side of the present

Gothic II with the SystemPack (DirectDraw7 + Direct3D7, 4K, session `20260914_173658`) drew the overlay and then
lost it again, frame after frame. The log shows what the route actually did: `DDraw: Overlay writeback to primary
surface completed` on every present, and `DDraw: Overlay helper PresentEx hr=0x08760878` - `S_PRESENT_OCCLUDED` -
on every one of them too.

Both lines are the bug. `HandleCapture` ran **after** the original `Flip`/`Blt` returned and composited into the
surface that was already on screen: read the primary, draw the overlay over it, write the whole thing back. On a
flip chain that write lands in a buffer the display is already scanning out, and the next flip replaces that
buffer with one the overlay never touched. The overlay was therefore present for part of a frame, missing for the
rest, and completely absent whenever the write finished after the following flip - flicker at the frame rate, by
construction. The helper `PresentEx` was a second, redundant presentation route that is occluded for as long as
the application holds the display, so it never contributed anything either.

The cost made it worse. Every composite moved the whole 3840x2160 surface up and back down across the CPU, about
66 MB per present; the session's composite counter advanced 112 times in 6.4 s, roughly 17 presents per second,
and the window in which the frame was on screen without the overlay was about 10-25 ms wide.

`hook/common/ddraw_present_policy.h` now names what each hooked call publishes. `Flip` publishes the flip chain's
back buffer (or the caller's explicit target), a full-surface blit onto a **single-buffered** scanout surface
publishes its source, and an `Unlock` of the primary is already visible. The overlay goes into the image the
present is about to publish, before the call reaches the runtime; when that image cannot be resolved CE composites
nothing rather than falling back to the visible surface, because that fallback *is* the race. Blits onto a flip
chain are no longer presentations at all - `Flip` owns those images - and partial blits are 2D updates that only
restore the overlay when they intersect it, instead of re-compositing the whole overlay dozens of times a frame
and feeding each one to the recorder.

The transfer is now the overlay's own bounding rectangle, aligned to a 64-pixel grid: `OverlayAdapter::
GetLastRenderedBounds` derives it from the geometry the renderer last built, and the staging surfaces are sized to
it instead of to the frame (over 20x less traffic at 4K). The rectangle is known only from geometry that already
exists, so the previous frame's rectangle stages the pixels and the frame that grows past it re-stages the union
and re-submits the same geometry through `OverlayAdapter::ResubmitLastFrame` - without that the overlay would be
clipped for exactly one frame every time it grows. The helper `PresentEx` survives only as the fallback for when
the in-frame composite fails outright.

Two smaller things fell out. The 100 ms "last presented source surface" tick heuristic is gone - `Flip` hands us
its target and `Blt` hands us its source, so there was nothing left to guess. And `NotePresentationComplete`
refuses to count a frame while a composite is in flight: the composite locks and unlocks DirectDraw surfaces
through CE's own hooks, so a nested `Unlock` of the scanout surface reaches the frame accounting and would both
corrupt the frame-time series and let the FPS limiter sleep inside the composite.

`DDrawPresentPolicyTest` covers the classification and the region arithmetic, including the two cases that were
previously wrong by construction: a flip composites into the flip target and never into the visible surface, and a
partial blit is not a present. Built and verified at 0.1.6577. **Hardware run pending** - no Gothic II session has
exercised this yet.

### 2026-09-14 - Gothic II/SystemPack startup: old-linker IATs and unrelocated pristine code

All three Gothic2.exe failures in sessions `20260914_151113` and `20260914_151846` were deterministic
`0xC0000005` reads in `msvcrt!strcmp+0x6c`, during `InitializeKernel32Hooks` immediately after CE patched the
main executable's `LoadLibraryA` import. The crash registers were identical: CE's literal `LoadLibraryA` at
`0x63A7551C` was compared with invalid address `0xE8421F72`.

The dump's live PE metadata resolves the equation. SystemPack's `Shw32.dll` was based at `0x71D40000`; its
Kernel32 import descriptor has `OriginalFirstThunk=0`, and the first `FirstThunk` value was the loader-resolved
`LoadLibraryA` pointer `0x766E1F70`. The old IAT walker treated `FirstThunk` as if it still held
`IMAGE_IMPORT_BY_NAME` RVAs, added the module base and the two-byte hint offset, and manufactured the exact fault
address: `0x71D40000 + 0x766E1F70 + 2 = 0xE8421F72` (32-bit wrap).

`iat_import_table.h` now keeps named INTs and name-less resolved IATs as distinct formats. A name-less entry is
matched only by the exact resolved export address, or by CE's own tracked hook record on a repeated pass; an
unidentified foreign replacement is preserved. All import directory, descriptor, thunk and string accesses are
bounded to `SizeOfImage` and checked for readable memory, with rate-limited malformed-table diagnostics.

`IATHookImportTableTest` reconstructs the exact name-less layout plus normal-name, malformed-RVA, tracked-hook,
foreign-owner and overflow directions. The first hardware re-check (`20260914_154220`, 0.1.6566) proved this fix:
the name-less patch completed and injection advanced through the rest of kernel32/GetProcAddress initialization.
It also exposed a second independent generic bug in the guarded DXGI bootstrap.

Steam owned `dxgi!CreateDXGIFactory1` with a five-byte entry jump, so CE built a clean bypass from the DLL's disk
bytes. The x86 disk prolog contains `A1 C0 86 0D 10` (absolute address `0x100D86C0`, based at the preferred
`0x10000000`), while the loaded image at `0x64210000` correctly contains `A1 C0 86 2E 64` (`0x642E86C0`). The
resume verifier mistook that legitimate relocation difference for more foreign patching, extended from +5 to +13,
then copied and executed the raw absolute operand. The crash was immediate and exact: EIP `0x0BA00008` in CE's
bypass trampoline, reading unmapped `0x100D86C0`. `llvm-readobj` confirms a `HIGHLOW` relocation at operand RVA
`0x38549` in the installed SysWOW64 `dxgi.dll`.

`inline_hook_pristine_image.h` now parses bounded x86/x64 PE file layouts and applies overlapping `HIGHLOW`/`DIR64`
base relocations to pristine bytes for the module's actual load address. A moved image with missing, malformed or
unsupported overlapping relocation data is refused rather than made executable. Both deep and bypass trampolines use
the rebased bytes for live comparison, decoding and copying, and log the applied relocation count. The focused
functional tests reconstruct the exact Gothic/DXGI bytes, partial-span application, x64 `DIR64`, missing-directory
and malformed-block directions; a source-policy assertion holds the guarded bypass path to the rebased reader.
Hardware re-check pending.

### 2026-09-14 - The 183 MB Portal RTX exit dump: one termination, two verdicts

Session `20260914_130052`, `crash_external_fatal_exit_NtTerminateProcess_00000001_4bae8a0c.dmp`, 183 MB, for a
clean quit. `hook_debug.log` holds both decisions one millisecond apart:

```
13:02:26.900 FatalExitDump: Skipping pre-termination dump - ... (source=TerminateProcess code=0x00000001
             caller=00007FF7A88B0D6B module=...\NvRemixBridge.exe+0x10D6B fgRuntimeActiveOrRecent=1)
13:02:26.900 FatalExitDump: Capturing pre-termination dump ... (source=NtTerminateProcess code=0x00000001
             exceptionAddr=00007FF9C0830159 crashLike=0 fgRuntimeActiveOrRecent=1 origin=loaded-module)
```

The logged stack of the second decision names the requester outright: `stack[2]=KERNELBASE.dll+0x100159`,
`stack[3]=NvRemixBridge.exe+0x10D6B`. The 2026-09-02 primary-module rule worked exactly as designed at the
`TerminateProcess` layer and then lost, at the `NtTerminateProcess` layer, the one fact it depends on. A single
termination request is observed by every hook it passes through on one thread, and below the outermost layer the
immediate caller is always a Windows module — so `ResolveTerminationOrigin(callerAddress)` reported the plumbing,
not the requester, and the suppression path (which deliberately does not set `g_PreTerminationDumpAttempted`, so it
cannot burn the one-dump budget) gave the inner layer a second, worse-informed chance.

Fix (0.1.6565): frames, not the immediate caller. `ce::crash_dump_policy::ResolveTerminationOriginFromFrames`
walks innermost-first, skipping CE's own hook frames and the forwarding layers
(`kTerminationPlumbingModuleNames`: ntdll, kernel32, KERNELBASE, ucrtbase, msvcrt, vcruntime140), and takes the
first frame that is neither. `ResolveTerminationOrigin` only pays for that walk when the immediate caller is one of
those layers; a caller it can attribute directly is still answered without a stack walk. Classification stays a
pure range check against bounds cached by `CacheTerminationOriginModuleBounds()` at hook-install time — now the
executable, CE's own image and each plumbing module — because `NtTerminateProcess` is also reached from
`RtlExitUserProcess` with the loader lock already held by the terminating thread.

Deliberately narrow: the plumbing list is module names, not "anything under the Windows directory". NVIDIA's FG
runtimes load from the DriverStore under `C:\Windows`, and an FG runtime killing the process while tearing down is
the entire reason the active-FG fallback exists. Everything unprovable still dumps — an unresolvable frame stops
the walk at `kUnknown`, a stack of nothing but forwarding layers resolves to `kUnknown`, and `kUnknown` never
suppresses. Crash-like exit codes never consult the origin at all.

Both log lines now carry `requester=`/`requesterModule=` next to the immediate caller, so a future false positive
of this shape is one line to diagnose instead of a stack dump to read.

Tests: `CrashDumpPolicyTest.LayeredTerminationRequestIsAttributedToItsRequester` encodes the exact frame layout
from this session; `LayeredTerminationRequestFromALoadedModuleStillDumps`,
`UnattributableTerminationRequestResolvesToUnknown` and `OnlyTerminationForwardersCountAsPlumbing` cover the
fail-open directions. `CrashHandlerSourceTest.LayeredTerminationRequestIsAttributedByWalkingTheStack` holds the
resolver to the walk, and `TerminationOriginIsCachedAtInstallAndResolvedWithoutTheLoader` now covers the
classifier as well as the resolver for the no-loader invariant.

**Hardware run pending**: a Portal RTX quit should log the suppression line with
`requesterModule=...NvRemixBridge.exe` and produce **no** `crash_external_fatal_exit_NtTerminateProcess_*.dmp`.

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
