# llm-wiki Log Archive 2026-W38c

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
