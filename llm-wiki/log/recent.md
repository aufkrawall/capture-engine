# llm-wiki Log

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


### 2026-09-14 - NVIDIA Smooth Motion is a present interposer, and CE was overlaying its private chain

Strange Brigade DX12 with driver Smooth Motion crashed on the first frame, session `20260914_102700` (0.1.6550).
`NvPresent64.dll` does not wrap frame generation inside the application's swapchain the way DLSS-G or FFX do: it
interposes DXGI itself. The application gets a proxy `IDXGISwapChain` whose whole vtable lives in NvPresent64
(`vtableSlots(addRef=NvPresent64.dll release=NvPresent64.dll present=NvPresent64.dll resizeBuffers=NvPresent64.dll)`),
and NvPresent64 keeps a PRIVATE real DXGI swapchain, created on its own command queue, for the interpolated output.

CE's Present view was the deep `dxgi!Present` body hook it takes below a foreign overlay chain (Steam was loaded).
That body is only ever reached by the interposer's private chain, so:

- CE adopted NvPresent64's swapchain as the game's (`ProcessFrame FIRST CALL (swapchain=00000000224700D0)` while the
  game held `0000000036316DD0`), built RTVs from its back buffers, and submitted the overlay command list on the
  GAME's queue `0000000008CC1B00` - a queue with no relationship to that swapchain.
- The first `ExecuteCommandLists` removed the device: `Reinit SUBMIT #1 ... devRemoved=0x887A002B` =
  `DXGI_ERROR_ACCESS_DENIED`, then `DXGI: Device removed (hr=0x887A0005)` out of Present. **No TDR in the Windows
  System log** - a runtime access-denied removal, not a GPU hang. CE then swallowed the application's command lists
  (its torn-down-driver guard) and the render thread dereferenced null 1.1 s later at
  `StrangeBrigade_DX12+0x8cdebdc` (`movzx ecx, word ptr [rbp+0Eh]`, `rbp = 0`).

Fixed in 0.1.6555 (`acc543d2`): a present interposer is now its own module class, its private output chain is
registered at create time and passed through untouched by both Present entries, "a deep body hook covers everything"
is conditional on the app-facing swapchain's Present actually being the dxgi body CE hooked, and the swapchain
wrapper stops delegating a Present the detour cannot see. Overlay confirmed working on hardware, session
`20260914_105853`.

### 2026-09-14 - Smooth Motion status was reading the chain CE should never have been on

Same session. The FG status went to `runtime=Off` under Smooth Motion once CE moved to the application's chain, and
that is not a regression to undo. The 2026-07-29 detection (`57a374b8`, `8240ddb5`, `bcf664d8`) infers Smooth Motion
from command-list work populations and paired Present gaps in `RecordFrame`, and its own validation runs say what it
was measuring: `20260729_183342` logged "56 total frames, 31 high-work frames, 130 output FPS, 72 base FPS" and
`20260729_184536` logged Present callbacks alternating "about 0.6/13.8 ms". Those are NvPresent64's 2x output
callbacks. **The application's own present stream is 1x by construction**, so neither heuristic can ever fire from
it, and both only ever worked while CE was processing the driver's private chain - the same misidentification that
removed the device.

Replaced in 0.1.6556 with structural evidence: CE counts the interposer's private-chain presents and the
application's presents and takes the ratio (`hook/common/present_interposer_cadence.h`). Session `20260914_105853`
measures it exactly - `DetourPresent: ENTRY #1/#2` at `10:59:08.951/.952` for one `Present ENTRY #0`, and so on for
every frame. 1:1 forwarding reads as Smooth Motion loaded and NOT engaged rather than as a 1x generator. The base
and output FPS the overlay shows come from the same two measured streams, because the frame history now only holds
the application's presents. Hardware confirmation of the visible label still pending.


### 2026-09-13 - Validated: forced vsync under DLSS MFG, on the vertical blank

Session `20260913_201259` (0.1.6548) closes the four-round Portal RTX investigation below. `registered live-WSI
swapchain #1`, then `final Present1 #1 ... force=1 SyncInterval=0->1 Flags=0x200->0x0`. Steady state: **144.0
presents/s on a 144 Hz panel** (`publishedInterval meanUs=6946`), `Pacing health stddev=320-365us
displayJagUs=297-360`, 1% low 99-123 fps, `nvFlipSchedule avgDelayUs=6007` (the generator still holds its frames).

| state | presents/s | screen-time stddev | 1% low | tearing |
| --- | --- | --- | --- | --- |
| forced FIFO + VK_EXT_present_timing | 141 | 6900 us | 57 fps | no |
| present mode left alone, no ceiling | 152 | 154 us | - | yes |
| + vertical blank on the WSI's DXGI flip | **144.0** | **~360 us** | **~110 fps** | **no** |

The stddev above the uncapped 154 us is the vertical blank itself: intervals are now one refresh, occasionally two
(`p99 8300 us`). What CE does for a metered frame generator is now exactly one thing - `SyncInterval=1` with
`ALLOW_TEARING` cleared on the flip NVIDIA's WSI issues. It changes nothing above the WSI, adds no timer, writes
no driver profile.

### 2026-09-13 - The Vulkan layer never exported the query its DXGI backstop is authorized by

Session `20260913_200614` on 0.1.6547: fps still above the refresh rate, still tearing. The path armed correctly -
`Vulkan DXGI FIFO: armed system method-body interception`, all four system creation body hooks active - and then:

    Vulkan DXGI FIFO: swapchain ... from CreateSwapChainForHwnd targets window 0000000000460984
    which is not a live Vulkan surface (occurrence #1); not registered, presents stay untouched

0x460984 is the window the layer itself resolved from the surface and is drawing the overlay on
(`InitializeOverlay ENTRY(... window=0000000000460984 ...)`), so `RegisterSurface` and therefore
`PublishLiveSurfaceHwnd` had both run.

**Cause**: `CEVulkanLayerIsLiveVulkanSurfaceHwnd` was listed in `hook/vulkan_layer/layer.def`, and **no link
command has ever read that file**. `tools/build/build_vulkan_layer.py` passes no `.def`; the layer exports through
`__declspec(dllexport)` on the declaration alone, which `layer_main.cpp`'s Vulkan entry points have and the bridge
queries did not. `llvm-readobj --coff-exports` on the shipped DLL listed exactly three names. The hook DLL's
`GetProcAddress` returned null and the call site fails closed on purpose, so the authorization always said "no"
and the registry stayed empty. The final-present rewrite has never run in this configuration.

**Why nothing caught it**: two source-policy tests asserted the name was present *in `layer.def`*, and passed
throughout. A source list is not an export table.

**Fix (0.1.6548)**: `__declspec(dllexport)` on both bridge queries; `layer.def` deleted rather than left looking
authoritative; the two tests re-pointed at the attribute on the declaration; and
`tools/verify_vulkan_layer_exports.py` added to the build after PE verification, reading the export table of the
DLL that actually ships (`Verified Vulkan layer exports consumed by the hook DLL`). It reproduces the bug against
the pre-fix artifact, and `tools/tests/test_vulkan_layer_exports.py` covers its parsing, including the x86
`--kill-at` decoration.

**Note for the other direction**: `CEVulkanLayerDeviceEnabledPresentMetering`, added earlier the same day for the
present-mode stand-down gate, was equally unexported. Its consumer - the upstream Streamline present-mode override
- also fails *open*, so that gate would silently have kept forcing FIFO in a Streamline DLSS-G Vulkan title. The
layer-side gate reads `DeviceDispatch` directly and was unaffected, which is why Portal RTX still behaved.

### 2026-09-13 - The vertical blank moved to the WSI's own DXGI flip

Third round, continuing the two entries below. Session `20260913_194420` on 0.1.6545 shows the present-mode
stand-down worked: `nvFlipSchedule avgDelayUs=6066` (the generator holds its frames again), `Pacing health
stddev=154us displayJagUs=135` against 6900 us / 12248 us before. It also shows what was left: `fps=152.1` on a
144 Hz panel, `publishedInterval p50=6600us`, and the user reports tearing. The vertical-blank request has two
promises and only one of them was being kept.

**Rejected first, and worth recording.** The driver's frame-generation-aware low-latency interval
(`minimumIntervalUs` via `NvAPI_Vulkan_SetSleepMode`/`vkSetLatencySleepModeNV`, which under DLSS-G bounds
*displayed* frames) holds the output under the refresh rate without any CE-side wait, and was implemented, tested
and then discarded. It is the driver's clock rather than CE's, but it is still a clock: it caps a rate and never
phase-locks a frame to a blank. Proper vsync has no synthetic timer in it.

**Change (0.1.6547)**: re-arm the final-DXGI vertical-blank contract in `hook/wrappers/vulkan_dxgi_fifo_present.cpp`
- `SyncInterval=1` with `DXGI_PRESENT_ALLOW_TEARING` cleared on the flip NVIDIA's WSI issues. That is a vertical
blank, not a rate, and on a G-SYNC panel it is the DXGI spelling of "G-SYNC + V-Sync On". The machinery was intact;
only `ShouldArmFinalDxgiPresent` had been made to return false.

**Why its earlier retirement no longer applies**: it was retired on `20260830_175147`/`20260830_182939`, where
forcing the interval turned a generated group into fast-then-freeze judder. Both sessions *also* forced the Vulkan
present mode to FIFO, which is what unpaced the group in the first place (6842 us -> 141 us of announced flip
lead). The earlier conclusion was measured on an already-unpaced burst. CE no longer touches the present mode for
a metered device, so the group reaches DXGI correctly spread and the blanks align it instead of bunching it.

**Scope**: arming is unchanged (resident layer + `fifo`/`adaptive`), but `ShouldRewriteFinalPresent` now also
requires `VK_NV_present_metering` on the device, read through the layer export added for the Streamline gate. An
ordinary Vulkan title keeps its vertical blank from the forced FIFO present mode and its final presents stay
byte-identical, so the WSI keeps the per-present choice it makes for variable refresh.

**Verdict to look for on hardware**: `Pacing health` stddev staying near 150 us with `fps` at or just under the
panel's refresh, and no tearing. If the stddev climbs back toward 7 ms, the flip quantization is fighting the
generator's schedule after all and the ceiling has nowhere left to live above the driver.

### 2026-09-13 - The driver named it: forced FIFO removes a metered generator's flip scheduling

Follow-up to the entry below, on the 0.1.6542 build that removed CE's own present schedule. Session
`20260913_193655` shows the swapchain created clean (`flags=0x0`, `CE schedules none of these presents`) and the
steady-state frame-time stddev **unchanged at 6.88-6.92 ms**. So the VK_EXT_present_timing injection was not the
cause - the remaining CE action on that swapchain was the Immediate->FIFO present-mode override.

**The measurement that settles it is in `sensors.log`, not the layer log.** `[DisplayTiming]` reports
`nvFlipSchedule(... applied=N avgDelayUs=M ...)`, where M is the lead with which NVIDIA announces the screen time
it scheduled an image for. Session `20260913_184745`, same game, same 3x MFG, one minute apart:

| window | vsync_mode | applied | avgDelayUs | published screen intervals |
| --- | --- | --- | --- | --- |
| 18:48:12 | `default` | 124 | **6842** | p50 7400 us |
| 18:49:25 | `fifo` | 1296 | **141** | p1 1900, p50 **2100**, p99 18500 us |

6.8 ms of announced lead is a generated frame being held for its slot in the rendered interval. 141 us is no hold,
and the published intervals show the consequence directly: the batch flips back to back and the screen then waits.
**Forcing a vertical-blank-paced present mode onto a metered swapchain does not add a vertical-blank wait; it
takes the generator's flip scheduling away.** Remix's own UI says "When Frame Generation is active, V-Sync is
automatically disabled", and NVIDIA excludes Vulkan from DLSS-G V-Sync support.

**Change (0.1.6545)**: `ShouldSkipPresentModeOverride` gates both override sites - the layer's
`vkCreateSwapchainKHR` and the upstream `sl.interposer` hook, which reads the same fact through a new layer export
`CEVulkanLayerDeviceEnabledPresentMetering` (`hook/common/vulkan_layer_metering_bridge.h`). The gate is the
application's own `VkDeviceCreateInfo` extension list, not an observed metered present: the generator creates its
swapchain and presents through it immediately, so there is nothing to observe first and a present mode can only be
chosen at creation. `off` and `mailbox` are untouched.

**Cost, stated**: in a title whose device enables `VK_NV_present_metering`, `vsync_mode=fifo` now does nothing at
all, including while FG is switched off - the extension is device-lifetime. Distinguishing the two would mean
choosing a present mode from state that changes after creation, which is a race.

**Open**: nothing bounds a metered generator that outruns its display. Neither the Vulkan present mode nor a
per-present schedule can supply that without destroying the generator's placement, so the ceiling belongs on the
*rendered* rate. The only non-CE-timer mechanism left is the driver's FG-aware low-latency interval
(`minimumIntervalUs`, which under DLSS-G limits displayed frames) - and `vulkan-forced-fifo.md`'s own rule
currently forbids calling a refresh-derived cap a VSync fallback. That rule is now the open decision.

### 2026-09-13 - Forced FIFO bunched the DLSS-MFG batch on screen: VK_EXT_present_timing scheduling removed

**Report**: Portal RTX (RTX Remix, DLSS multi-frame generation) stutters with `[Graphics] vsync_mode=fifo`;
driver-forced V-Sync in the same game is smooth. Session `20260913_190555`.

**What the presents say**: nothing. `perf_metrics_1508.csv` is three `vkQueuePresentKHR` calls ~0.3 ms apart every
21.128 ms (stddev 0.317 ms, 47.3 fps base x 3 = 142 presents/s on a 144 Hz VRR panel). That burst shape *is* the
normal metered batch, and the rendered period is a metronome. CE's own cost is negligible (overlay 105 us/present,
`fence_wait_us` p50 2 us, `fps_limit_wait_us` ~0).

**What the screen says**: the same CSV's `source_*` columns come from the display-change series
(`screenTime=1 screenTimeShare=1000permille`), and they carry a frame-time stddev of **6.90 ms** with a **57 fps
1% low** at a 142 fps mean. Solving mean 7.03 / stddev 6.90 for a three-interval group gives ~2.2/2.2/16.8 ms: the
batch lands bunched and the screen then holds. That is the stutter.

**The A/B**: session `20260913_184745` crossed a live `vsync_mode` change inside one running game, so nothing else
moved. `perf_metrics_11520.csv` (18:47-18:48, `default`, present timing never armed): stddev **0.43 ms**, 1% low
~110 fps. `perf_metrics_14696.csv` (18:49, `fifo`, armed): stddev **6.91 ms**, 1% low ~14 fps. Identical present
structure (`burstShare` 0.67 in both). Every non-batched Vulkan session that day sits at 0.13-0.91 ms stddev with
or without forced FIFO, so forced FIFO alone is not the trigger - the metered batch is.

**Second defect, same code**: `RefreshTimingProperties` re-read `VkSwapchainTimingPropertiesEXT::refreshDuration`
every 256 presents and used the live value as the per-image floor. On a VRR swapchain that field is the cycle the
panel is running *now*: `20260913_190555` logged 6944400 -> **10140800** -> **10359900** -> 6944400 ns, so CE asked
the driver to hold generated frames to 98.6 and 96.5 fps on a 144 Hz panel for 3.6 s of a 12 s window.

**Change**: the whole `VK_EXT_present_timing` mechanism is deleted - `vulkan_present_timing.{h,cpp}`,
`vulkan_present_timing_policy.h`, the device/instance capability additions, `VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT`
and the per-present `VkPresentTimingsInfoEXT`. `vsync_mode=fifo` keeps its present-mode override; CE now requests no
present schedule of its own anywhere. Removing the swapchain flag also gives every title NVIDIA's native present path
back (see [[ce-took-nvidia-native-vulkan-present]] and the probe in `vulkan-forced-fifo.md`).

**New diagnostic**: `[Overlay] Pacing health:` every 10 s in the layer/hook log - active series, fps, 1%/0.1% low,
stddev, display and presentation jaggedness, screen-time share, FG multiplier. This whole regression moved only the
*shape* of the screen series and was invisible in the log until it was re-derived from the CSV by hand.

**Open**: two CE actions arrive together on a metered swapchain, the Immediate->FIFO override and the timing request,
and only the timing request is withdrawn. The next Portal RTX `fifo` + MFG run decides: `Pacing health` stddev back
under ~1 ms means the batch is spread again; still ~7 ms means the present-mode override is the remaining cause.
Also still open and now unowned: nothing bounds a metered generator that outruns its display (session
`20260829_022419`, 172 presents/s on 143 Hz under 4x MFG). That ceiling belongs on the *rendered* rate.

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

**Found alongside, fixed separately** (see the D3DKMT entry below): `hook/wrappers/d3dkmt_hook.cpp` typed
`D3DKMT_HANDLE` as `UINT64`, so every field it read was at the wrong offset.

### 2026-09-13 - D3DKMT hook: the mirrored structures were off by eight bytes

`hook/wrappers/d3dkmt_hook.cpp` declared `D3DKMT_HANDLE` as `UINT64`; `d3dukmdt.h` defines it as `UINT32`. Every
member after the first handle was therefore shifted:
- `D3DKMT_QUERYADAPTERINFO::Type` read the low dword of `pPrivateDriverData` and `PrivateDriverDataSize` read past
  the structure. DOOM Eternal session `20260913_180809` shows it directly: `QueryAdapterInfo - Type=3086977864,
  Size=52413`, where 52413 is 0xccbd - the low half of the adapter LUID the Vulkan layer logged for the same GPU in
  the same session.
- `D3DKMT_QUERYVIDEOMEMORYINFO` was off by eight from `MemorySegmentGroup` onwards. `hProcess` is a `HANDLE`
  (8 bytes) and `hAdapter` a `UINT32`, not two 64-bit handles. The read-only path only mislogged, but the VRAM
  override branch writes `Budget`/`CurrentUsage`/`CurrentReservation`/`AvailableForReservation` back into the
  caller's structure and would have written the budget over `CurrentUsage` while reading `Budget`'s low dword as
  the segment group. Only `InitializeConfig` leaving the override off by default kept that latent.
- `D3DKMT_ADAPTERINFO` named its last two fields `VidPnSourceId`/`NodeCount`; the real ones are
  `NumOfSources`/`bPrecisePresentRegionsPreferred`, and `AdapterLuid` is a `LUID` (4-byte aligned, so it packs at
  offset 4 behind the handle), not a 64-bit handle.

**Fix**: the layouts moved to `hook/wrappers/d3dkmt_abi.h` with every offset and size pinned by `static_assert`,
documented as a mirror of `d3dkmthk.h`/`d3dukmdt.h` (those headers are not in the MSYS2 toolchain, so mirroring is
required - same arrangement as `vulkan_present_metering_policy.h` for `VK_NV_present_metering`). The hook now logs
real values (`hProcess` resolved to a PID, the adapter LUID, `NumOfSources`, `PhysicalAdapterIndex`), the dead
no-op switch in `Hook_D3DKMTQueryAdapterInfo` is gone, and `Hook_D3DKMTEnumAdapters` clamps its loop to
`MAX_ENUM_ADAPTERS` instead of trusting the reported count. `tests/test_d3dkmt_abi.cpp` asserts the offsets and
checks that a write to `Type` is what a 32-bit read at offset 4 returns.



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
