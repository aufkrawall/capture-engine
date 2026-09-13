# Forced FIFO Presentation Under Vulkan

Last cross-checked: 2026-09-13 (for a metered frame generator CE now changes nothing above the WSI - both the
VK_EXT_present_timing scheduling and the present-mode override are retired, because NVIDIA's announced flip lead
collapses from 6842 us to 141 us when FIFO is forced - and states the vertical blank on the WSI's own final DXGI
flip instead; earlier 2026-08-30 state otherwise unchanged)

Summary: what it takes for `[Graphics] vsync_mode=fifo` to actually mean "one presented frame per vertical blank" in a
Vulkan title, and why frame generation is the case that breaks every partial answer. Three boundaries are involved -
Streamline's Vulkan proxy above the layer, the layer's own swapchain creation, and NVIDIA's internal DXGI flip
swapchain below it - plus one capability, `VK_NV_present_metering`, that overrides all three for the lifetime of the
device.

Primary sources:
- `hook/vulkan_layer/{vulkan_layer_swapchain,vulkan_layer_present,vulkan_layer_capabilities}.cpp`
- `hook/vulkan_layer/{layer_overlay,layer_overlay_render,layer_overlay_compute}.cpp`
- `hook/vulkan_layer/{vulkan_present_metering_policy,vulkan_present_chain_policy,overlay_submit_queue_policy}.h`
- `hook/common/overlay_adapter_render_frame.cpp` (the `[Overlay] Pacing health:` line, 10 s cadence)
- `hook/common/{vulkan_dxgi_fifo_policy.h,vulkan_dxgi_fifo_registry.h,vulkan_wsi_surface_table.h,remix_frame_generation_policy.h,custom_overlay_vk.h}`
- `hook/wrappers/vulkan_dxgi_fifo_present.cpp`, `hook/wrappers/wrapper_hooks.{h,cpp}`
- `hook/apis/{streamline_hook_api,remix_hook}.cpp`
- `tools/build/build_vulkan_layer.py`, `tools/tests/test_vulkan_layer_exports.py`
- `tests/{test_vulkan_present_metering_policy,test_vulkan_present_chain_policy,test_overlay_submit_queue_policy,test_remix_frame_generation_policy,test_vulkan_dxgi_fifo_scoping,test_vulkan_renderer_policy}.cpp`

Related: [graphics-overrides-and-frame-pacing.md](graphics-overrides-and-frame-pacing.md) for the rest of the
`[Graphics]` contract, [display-change-timing.md](display-change-timing.md) for measuring what actually reached the
screen.

## Vulkan FIFO through Streamline

- A Vulkan implicit layer can be too late to communicate a present-mode override to Streamline. NVIDIA's
  [`sl.interposer` Vulkan proxy](https://github.com/NVIDIA-RTX/Streamline/blob/main/source/core/sl.interposer/vulkan/wrapper.cpp)
  invokes its `eVulkan_CreateSwapchainKHR` **before hooks** with the application's original
  `VkSwapchainCreateInfoKHR`, then calls the downstream Vulkan dispatch. Portal RTX sessions `20260828_014434` and
  `20260828_022342` proved this split: Streamline received Immediate while CE's downstream layer changed only the
  driver-bound copy to FIFO. A software rate cap cannot repair that contract and is not VSync.
- For `vsync=fifo`, CE inline-hooks only `sl.interposer.dll!vkCreateSwapchainKHR` and substitutes FIFO before the
  proxy invokes DLSS-G. FIFO is the only mode handled at this upstream boundary because Vulkan guarantees it; the
  layer retains its normal validation/enforcement for the complete present-mode configuration. Hook lifetime is
  tracked with the other Streamline core slots so unload/reload cannot retain a stale trampoline.
- Runtime proof is two-sided and swapchain-scoped: `hook_debug.log` reports `before Streamline DLSS-G hooks`, while
  `vulkan_layer.log` reports `driver returned ... presentMode=2`. NVIDIA officially excludes Vulkan from DLSS-G
  VSync support, so those messages prove Vulkan mode propagation but not generated-output synchronization. Portal RTX
  session `20260828_162056` had both messages and still produced about 158.8 FPS in three-output bursts.
- **Re-armed as a scoped backstop (2026-08-30), see "the per-instance backstop" below.** The description that
  follows is what the path does while armed; `ShouldArmFinalDxgiPresent` arms only when the resident CE Vulkan layer
  is loaded and `vsync_mode` is `fifo` or `adaptive`, and the rewrite additionally applies only to swapchain
  instances the creation detours registered. The earlier crash came from vtable mutation, not from the override
  itself, and that regression stays fixed.
- NVIDIA's Vulkan WSI terminates in an internal DXGI flip swapchain. With a resident CE Vulkan layer and explicit
  `vsync=fifo`, `hook/wrappers/vulkan_dxgi_fifo_present.cpp` registered only system `CreateDXGIFactory*` lookups,
  leaves each real factory and every creation descriptor unwrapped/unchanged, and reads its four creation-method
  addresses (slots 10/15/16/24) without writing them. Inline hooks on those system method bodies observe the returned
  real WSI swapchain and registers the instance pointer with the observed-swapchain registry. Present/Present1 are
  then intercepted with a deep body hook placed past the widest recognized
  foreign entry patch, while slots 8/22 remain untouched. Steam and other overlays retain their outer entry-chain
  ownership; CE runs below them and forces DXGI `SyncInterval=1` while clearing `DXGI_PRESENT_ALLOW_TEARING` and
  `DXGI_PRESENT_DO_NOT_WAIT` before
  the system body.
- Never replace factory/swapchain vtable slots for this path. Portal session `20260828_212805` proved that even a
  first Present1 whose arguments were already `1/0` crashes in Steam's DLSS-G worker path after the shared system
  vtable is changed: the initiating x64 dump is a null indirect call in `gameoverlayrenderer64`, with CE Present1 and
  NVIDIA WSI below it. COM identity/Release tracking does not repair an ownership violation. Factory and swapchain
  vtables, QueryInterface/Release, object identity, and creation descriptors must remain byte-for-byte outside CE.
- This is a narrowly exempted synchronization-argument path, not a revival of CE's ordinary DXGI policy under Vulkan.
  It performs no synthetic factory/swapchain probing, descriptor changes, frame-latency configuration, waiting,
  overlay, capture, or limiter work. The restriction preserves the ICD presenter-thread destruction invariant below.
  Its Present hot path reads only atomics. No driver profile/DRS write, timer, Reflex cap, refresh-derived cap, or
  Acquire-side group pacing is a VSync fallback.

## Vulkan FIFO vs. frame-generation present metering

- **The reason an accepted FIFO swapchain still ran past the refresh rate: `VK_NV_present_metering` is a second,
  competing pacing authority for the same presents, and it wins.** The application chains `VkSetPresentConfigNV`
  (`sType` 1000613000) onto `VkPresentInfoKHR` and asks the driver to spread `numFramesPerBatch` images evenly across
  one *rendered* frame interval, which is how multi-frame generation places its generated frames. That interval is
  derived from the base frame rate and knows nothing about the display, so on a metered present the driver stops
  applying the swapchain's vertical-blank wait and the output rate becomes base x `numFramesPerBatch`.
- Portal RTX session `20260829_022419` is the end-to-end proof, and it is the explanation for the earlier
  `20260828_162056` result above. CE overrode Immediate to FIFO and the driver accepted it (`Overriding present mode
  0 -> 2 (fifo)`, `vkCreateSwapchainKHR driver returned: 0 (presentMode=2 ...)`), yet `perf_metrics_29472.csv` shows
  presents climbing to 172/s on a 143 Hz display. The DXGI interception saw the switch from below - with frame
  generation off the driver presented the FIFO swapchain with `SyncInterval=1`, and the moment the DLFG swapchain went
  live it presented with `SyncInterval=0` plus `DXGI_PRESENT_ALLOW_TEARING`. Counting confirms nothing is dropped in
  between: the interval between the DXGI `final Present1 #1024` and `#2048` cadence lines contains exactly 1024
  `vkQueuePresentKHR` calls, so app present, driver present, and display path are 1:1.
- RTX Remix documents both halves of this itself, in its own binary strings: `rtx.dlfg.enablePresentMetering` is
  "Use hardware present metering for DLSS 4.0 frame generation instead of CPU pacing", and its V-Sync option carries
  "When Frame Generation is active, V-Sync is automatically disabled". Hardware metering therefore has a supported
  fallback - the runtime's own `DxvkDLFGPresenter` CPU pacer - and Remix deliberately presents with Immediate under
  frame generation, which is why CE sees the mode flip to 0 on exactly the swapchain generation DLFG creates.
- **A burst of N `vkQueuePresentKHR` calls inside a millisecond is the normal shape of a metered batch, not evidence
  of bad pacing.** The runtime issues the whole group at once and the driver places the flips. Read the *group
  period* against the display instead; that is what the earlier "bursts of four inside ~700 us" observation actually
  measured.

### Withholding the capability, not the request

- **Suppressing the per-present request cannot bring the vertical-blank wait back.** Session `20260830_175147`
  (Portal RTX, 4x MFG, `vsync_mode=fifo`) has both halves at once: `vulkan_layer.log` reports
  `frame-generation present metering numFramesPerBatch=4 (swapchain presentMode=2 images=6 chainNodes=1 chainHead=1)
  - suppressed` from the moment the frame-generation swapchain is created, and `hook_debug.log` still reports
  `Vulkan DXGI FIFO: final Present1 #1024 force=1 SyncInterval=0->1 Flags=0x200->0x0` seven seconds later. NVIDIA's
  Windows WSI decides how it will present a swapchain **when the device is created**, so a device that enabled
  `VK_NV_present_metering` keeps presenting with tearing whatever any individual `VkPresentInfoKHR` says. This is the
  case the previous revision of this page listed as its own open question.
- **What is left is worse than what it replaced, and it is the "stuttery with a flat frame time graph" report.**
  With the request removed there is no pacer at all, and CE's forced DXGI sync interval quantizes the unpaced burst
  onto consecutive vertical blanks. A group of N generated frames is then drawn across N refresh intervals no matter
  how long the rendered frame it belongs to took: in that session the group period ranged from 21 ms to 48 ms
  (mean 27.8 ms, `perf_metrics_7260.csv`) while the display consumed every group in 4/refresh. Motion runs fast and
  then freezes once per group; present-side frame times stay flat, because the presents really are regular.
- **Fix: withhold the capability instead (`hook/vulkan_layer/vulkan_layer_capabilities.cpp`).** When `vsync_mode` is
  `fifo` or `adaptive` the layer removes `VK_NV_present_metering` from `vkEnumerateDeviceExtensionProperties`,
  reports `presentMetering = VK_FALSE` from `vkGetPhysicalDeviceFeatures2`/`2KHR`, and drops the name from
  `VkDeviceCreateInfo::ppEnabledExtensionNames`. A device created without it presents its FIFO swapchain the way it
  did before frame generation went live, and the runtime falls back to the CPU pacer it already ships. CE adds no
  pacing, no timer and no synthetic cap of its own.
- The enumeration filter keeps the two-call idiom intact: the count the application is told is the *filtered* count,
  and a buffer sized from it must come back `VK_SUCCESS`. Filtering the driver's answer in place cannot do both, so
  the layer always fetches the complete list and copies from it (`CopyWithoutPresentMetering`).
- **`vkCreateDevice` removal is transactional.** An application that queried before CE was configured may also chain
  `VkPhysicalDevicePresentMeteringFeaturesNV`, which is application-owned and `const`, so CE cannot neutralize it. If
  the driver then rejects the create info, the layer hands the extension back and retries with the application's own
  list rather than turning a pacing preference into a game that will not start.
- **The runtime's own option is the other half** (`hook/common/remix_frame_generation_policy.h`,
  `hook/apis/remix_hook.cpp`). A runtime whose option still says "use hardware metering" asks for pacing it can no
  longer get and never engages its CPU pacer, so under `fifo`/`adaptive` CE sets `rtx.dlfg.enablePresentMetering` to
  `False` through the same official `SetConfigVariable` route it already uses for `rtx.dlfg.maxInterpolatedFrames`,
  and rewrites the value if the runtime's menu sets it again. An unknown key is inert, so a spelling that a future
  Remix drops costs nothing.
- The per-present suppression stays as the answer for a runtime that chains `VkSetPresentConfigNV` without enabling
  the extension, and for a `vsync_mode` switched to `fifo` after the device was already created. Its conditions are
  unchanged: `vsync_mode` is `fifo` or `adaptive`; the presented swapchain is one the layer tracks and was created
  with FIFO or FIFO_RELAXED; `swapchainCount == 1`; `numFramesPerBatch >= 2`.
- **The chain node can only be unlinked when it is the head.** A `pNext` chain is application-owned and `const`, so
  removing a deeper node would mean writing the predecessor's `pNext` in the game's memory. CE points its own
  `VkPresentInfoKHR` copy one node further instead, and reports the deeper case
  (`NOT suppressed: the request is not the chain head`) rather than silently doing nothing.
- Both structs are mirrored locally in the policy header rather than taken from `<vulkan/vulkan.h>`: the Linux MSYS2
  headers in this tree still predate `VK_NV_present_metering` while the Windows ones define it, and `static_assert`s
  pin the mirrors to the real declarations wherever the header has them.
- **Measured, and it was not the mechanism.** Session `20260830_182939` ran with the capability withheld end to end -
  `vulkan_layer.log` reports `withholding VK_NV_present_metering from vkEnumerateDeviceExtensionProperties (1 of 291
  device extensions)` and `hook_debug.log` reports `set rtx.dlfg.enablePresentMetering=False` - and the DXGI side did
  not move: `final Present1 #1024` and `#2048` still report `SyncInterval=0->1 Flags=0x200->0x0`. The base frame
  period distribution is unchanged too (p25 22.7 ms, p50 26.4 ms, p75 30.3 ms against 21.6/26.3/28.7 before). So
  hardware present metering is not what displaces the vertical-blank wait here.
- The withholding is kept because two pacing authorities on one set of presents is still wrong, and because it costs
  nothing when the runtime does not use the extension - but it is no longer offered as the explanation for anything.
  See "the DXGI override was the bug" below for where the evidence now points.

### VK_EXT_present_timing is retired: it bunched the metered batch it was meant to bound

- **The ceiling attempt.** From 2026-08-30 the layer created a forced-FIFO swapchain on a metering-capable device
  with `VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT` and chained a `VkPresentTimingsInfoEXT` onto every present,
  carrying `VK_PRESENT_TIMING_INFO_PRESENT_AT_RELATIVE_TIME_BIT_EXT` with `targetTime` set to the swapchain's
  reported `refreshDuration`. The intent was a display ceiling that left the generator's own spacing alone.
- **The A/B that killed it.** Portal RTX session `20260913_184745` crossed a live `vsync_mode` change inside one
  running game, so nothing else moved:

  | window | `vsync_mode` | CE present timing | screen-time stddev | 1% low |
  | --- | --- | --- | --- | --- |
  | `perf_metrics_11520.csv` (18:47-18:48) | `default` | not armed | **0.43 ms** | ~110 fps |
  | `perf_metrics_14696.csv` (18:49) | `fifo` | armed | **6.91 ms** | ~14 fps |

  `20260913_190555`'s `perf_metrics_1508.csv` reproduces the broken half (stddev 6.90 ms, 1% low 57 fps).
  `vkQueuePresentKHR` is byte-identical in both: three calls ~0.3 ms apart every 21.1 ms, the normal shape of a
  metered 3x batch, and the *rendered* period is a metronome either way (21.128 ms, stddev 0.317 ms, 47.3 fps
  base). Only what reaches the screen changes. Solving mean 7.03 ms with stddev 6.90 ms for a three-interval group
  gives ~2.2/2.2/16.8 ms - the batch lands bunched and the screen then holds, which is the reported stutter.
  Every non-batched Vulkan session on 2026-09-13 sits at 0.13-0.91 ms stddev whether `fifo` was forced or not, so
  forced FIFO on its own is not the trigger; the metered batch is.
- **A second, independent defect in the same code.** `RefreshTimingProperties` re-read
  `VkSwapchainTimingPropertiesEXT::refreshDuration` every 256 presents and fed the live value straight in as the
  per-image floor. On a variable-refresh swapchain (`refreshInterval == UINT64_MAX`) that field reports the cycle
  the panel is running *now*, not its shortest: `20260913_190555` logged 6944400 ns, then **10140800**, then
  **10359900**, then 6944400 again, so CE asked the driver to hold generated frames to 98.6 and 96.5 fps on a
  144 Hz panel for 3.6 s of a 12 s window. A ceiling must come from the display's *minimum* refresh duration.
- **Fix (2026-09-13): the whole mechanism is removed** - `vulkan_present_timing.{h,cpp}`,
  `vulkan_present_timing_policy.h`, the device/instance extension additions, the swapchain flag and the per-present
  chain node. `vsync_mode=fifo` keeps its present-mode override; CE requests no present schedule of its own.
  `tests/test_vulkan_present_metering_policy.cpp` guards the retirement by reading the layer sources.
- **What the flag cost while it was armed** (`build/vk-wsi-probe`, 2026-09-13, still true and worth keeping):
  enabling `VK_EXT_present_timing` + `VK_KHR_present_id2` + `VK_KHR_calibrated_timestamps` on the device, feature
  node included, changes nothing - the ICD still builds its native presenter (`wgl*` + `nvppex.dll` +
  `dispbroker.dll`, no D3D at all). Adding **only** the swapchain flag makes the same run resolve
  `CreateDXGIFactory2`, `D3D12CreateDevice`, `DwmGetCompositionTimingInfo` and `DCompositionCreateDevice3` and map
  `nvwgf2umx.dll`/`nvldumdx.dll`/`dcomp.dll`. NVIDIA's native presenter cannot serve a present-timing swapchain,
  and the layered path is what puts the window back under DWM composition - directly visible as the Windows volume
  OSD drawing over a fullscreen title. Removing the request restores the native path for every title.
- **Follow-up (same day): removing CE's schedule was not enough, and the driver named the remaining cause.**
  Session `20260913_193655` on 0.1.6542 shows the swapchain created clean (`flags=0x0`, `CE schedules none of
  these presents`) and the steady-state frame-time stddev unchanged at 6.88-6.92 ms. `sensors.log`'s
  `[DisplayTiming]` line carries the measurement that settles it - `nvFlipSchedule(... applied=N avgDelayUs=M ...)`,
  where M is the lead with which NVIDIA announces the screen time it scheduled an image for:

  | 20260913_184745 window | vsync_mode | applied | avgDelayUs | published screen intervals |
  | --- | --- | --- | --- | --- |
  | 18:48:12 | `default` | 124 | **6842** | p50 7400 us |
  | 18:49:25 | `fifo` | 1296 | **141** | p1 1900, p50 **2100**, p99 18500 us |

  6.8 ms of announced lead is a generated frame being *held* for its slot in the rendered interval. 141 us is no
  hold at all, and the published intervals show the result: the batch flips back to back and the screen then
  waits. **Forcing a vertical-blank-paced present mode onto a metered swapchain does not add a vertical-blank
  wait - it takes the generator's flip scheduling away.** RTX Remix says the same in its own UI ("When Frame
  Generation is active, V-Sync is automatically disabled") and NVIDIA excludes Vulkan from DLSS-G V-Sync support.
- **Fix (0.1.6545): the present-mode override stands down on a metering-capable device.**
  `ShouldSkipPresentModeOverride` in `vulkan_present_metering_policy.h` is consulted at both override sites - the
  layer's own `vkCreateSwapchainKHR` and the upstream `sl.interposer` hook, which reaches the same answer through
  a new layer export (`CEVulkanLayerDeviceEnabledPresentMetering`, resolved via
  `hook/common/vulkan_layer_metering_bridge.h`). The gate is the application's own `VkDeviceCreateInfo` extension
  list rather than an observed metered present, because the generator creates its swapchain and presents through
  it immediately - there is no present to observe first, and a present mode can only be chosen at creation.
  `off` and `mailbox` are untouched: they are not a vertical-blank contract and carry none of this measurement.
- **The cost, stated rather than hidden.** In a title whose device enables `VK_NV_present_metering`,
  `vsync_mode=fifo` now does nothing at all - including while frame generation is switched off, because the
  extension is enabled for the life of the device. Making that distinction would mean deciding a present mode
  from a state that changes after creation, which is a race, not a fix.
- **The ceiling is the final DXGI present, re-armed (0.1.6547).** With the override stood down, session
  `20260913_194420` measured the placement fixed (announced flip lead 6066 us, frame-time stddev **154 us**
  against 6900 us, `displayJagUs=135`) and the output settling at **152.1 fps on a 144 Hz panel** - above the
  refresh rate, with tearing. `SyncInterval=1` with `DXGI_PRESENT_ALLOW_TEARING` cleared on the WSI's own flip is
  the vertical blank that supplies the missing half, and it is a vertical blank rather than a clock: no timer, no
  rate, no driver-profile write, no swapchain parameter changed. On a G-SYNC panel it is the DXGI spelling of
  "G-SYNC + V-Sync On" - the panel still varies its refresh below its maximum and the flip waits only at the
  ceiling.
- **Why the earlier retirement of that path does not apply any more.** It was retired because forcing the interval
  was measured turning a generated group into fast-then-freeze judder (`20260830_175147`, `20260830_182939`).
  Both of those sessions *also* forced the Vulkan present mode to FIFO, and that is what unpaced the group - the
  announced flip lead collapses from 6842 us to 141 us on a metered swapchain forced to FIFO. The earlier
  conclusion was measured on an already-unpaced burst. CE no longer touches the present mode there, so the group
  now arrives at DXGI correctly spread, and quantizing a correctly-spread group onto vertical blanks is
  synchronization rather than judder.
- **Scope**: `ShouldArmFinalDxgiPresent` arms on the resident layer plus a `fifo`/`adaptive` profile as before, but
  `ShouldRewriteFinalPresent` now additionally requires `VK_NV_present_metering` on the device - read through the
  same layer export the upstream Streamline gate uses. An ordinary Vulkan title still gets its vertical blank from
  the FIFO present mode CE did force, and its final presents stay byte-identical, so the WSI keeps the per-present
  choice it makes for variable refresh.
- **Rejected on the way (2026-09-13): a refresh-derived rate cap.** The driver's frame-generation-aware
  low-latency interval (`minimumIntervalUs`, which under DLSS-G bounds displayed frames) would hold the output
  under the refresh rate without a CE timer, and was built and discarded. It is a clock, not a vertical blank: it
  caps a rate and never phase-locks a frame to a blank. Proper vsync has no synthetic timer in it, so this page's
  existing rule - no driver profile write, timer, Reflex cap or refresh-derived cap is a VSync fallback - stands
  unchanged.

## The effective present mode is the created one; the DXGI override was the bug

- **The per-present-mode-selection hypothesis was measured and is dead.** Session `20260830_185703` prints what CE is
  actually handed: `presentation-relevant device extensions requested: <none>` (no
  `VK_EXT_swapchain_maintenance1`, no `VK_KHR_present_id`/`present_wait`, no `VK_NV_low_latency2`),
  `vkCreateSwapchainKHR pNext chain: sType=1000255000 (nodes=1)` - a lone `VkSurfaceFullScreenExclusiveInfoEXT` - and
  no `vkQueuePresentKHR pNext chain:` line at all, because the present chain is empty. Nothing selects a present mode
  behind CE's back.
- **What the same session does show is the WSI choosing the DXGI parameters per present on one FIFO swapchain**:
  `final Present1 #1024 force=1 SyncInterval=1->1 Flags=0x0->0x0`, then `#2048 force=1 SyncInterval=0->1
  Flags=0x200->0x0`. Same swapchain, same thread, seven seconds apart. `SyncInterval=0` plus
  `DXGI_PRESENT_ALLOW_TEARING` is not a driver that forgot to synchronize - **it is how variable refresh is driven on
  Windows.** A flip the display is meant to stretch must be presented that way; the panel then refreshes on the flip
  instead of the flip waiting for the panel.
- **So CE's final-DXGI interception was replacing variable refresh with a fixed vertical-blank grid**, and under
  frame generation - where the rendered frame period is not constant - a fixed grid turns that variation into visible
  stutter while every present-side frame time stays flat. It also explains the user's A/B exactly: forcing the mode in
  the *driver* leaves `vsync_mode` at something other than `fifo`, so this path never armed, and the title is smooth.
- **That conclusion rested on evidence that was too weak to keep the override dead.** The `20260830_185703` capture
  attributed presents only through global counters (`final Present1 #N` cadence lines and
  `vkQueuePresentKHR` counts); it proved the WSI can emit per-present DXGI parameters, but not that the
  above-refresh rate with tearing was correct variable refresh rather than an unpaced generated group. The Remix
  CPU pacer that CE's withholding steers to is group-aware but **is not VSync**: it spreads generated batches
  evenly across the rendered frame interval, which says nothing about the display, so the presented output can
  still run past the refresh rate with tearing (~190/s measured). The native vblank contract is therefore restored
  as a scoped backstop - see "the per-instance backstop" below.

## The per-instance backstop: final present scoped to registered swapchains

- **Arming** (`hook/common/vulkan_dxgi_fifo_policy.h`): `ShouldArmFinalDxgiPresent` is true only when the resident
  CE Vulkan layer is loaded and `vsync_mode` is `fifo` or `adaptive` (the canonical `RequestsVblankPacedPresentation`
  from `vulkan_present_metering_policy.h`, included and called directly so the two policies cannot drift). `off`,
  `mailbox` and `default` never arm. `RegisterDynamicFactoryHooks` installs the three system `CreateDXGIFactory*`
  export body hooks first (see "route-agnostic first factory observation" below) and then publishes the decision
  with an unconditional `g_armed.store`, so a
  config change to a non-vblank mode disarms by storing `false`, arming never publishes before the export hooks
  are live, and the installed system body hooks are never
  unpatched at runtime - the atomic gate is the disarm, and the `HookIsShuttingDown()` lifecycle gate is unchanged.
  Armed hook-monitor calls retry an incomplete installation via `EnsureFactoryExportBodyHooksInstalled`: one
  atomic conjunction when complete, one `GetModuleHandleA` while `dxgi.dll` is absent, and exactly one resolve/
  install attempt per distinct loaded dxgi HMODULE (a failed attempt against an unchanged module image is
  deterministic, so it is never re-run per poll).
- **The per-instance registry** (`hook/common/vulkan_dxgi_fifo_registry.h`): a bounded (64-slot), lock-free,
  open-addressed table of raw swapchain instance pointers, filled by the four system creation-method detours on
  every successful targeted creation. No COM reference is taken; a re-created instance at a recycled address
  refreshes its slot naturally. When the table is full, registration fails closed: that instance's presents pass
  through untouched instead of falling back to an unscoped rewrite. Membership plus `ShouldForceFifoNow()` is the
  complete rewrite gate (`ShouldRewriteFinalPresent`), so an armed backstop never restates a pacing contract on a
  swapchain it did not watch being created.
- **Authorization lives in the resident layer, not in heuristics** (`hook/common/vulkan_wsi_surface_table.h`,
  `hook/vulkan_layer/layer_wsi_surface_bridge.cpp`). The layer publishes every live Win32 surface HWND at
  `vkCreateWin32SurfaceKHR` and retires it at `vkDestroySurfaceKHR`; the hook DLL resolves the layer's
  `CEVulkanLayerIsLiveVulkanSurfaceHwnd` export and registers only swapchains whose target window backs a live
  surface. The per-HWND refcount keeps a window shared by several surfaces live until its last surface retires.
  `vkDestroyInstance` closes the same lifetime implicitly: the layer records the owning `VkInstance` with every
  surface, and `VulkanLayerState::UnregisterInstance` sweeps the surfaces the instance still owns through
  `SelectWindowsToRetireOnInstanceDestroy`, retiring one window per surface after its lock is released. Without
  that sweep, an application that never calls `vkDestroySurfaceKHR` would leave its HWNDs published as live
  Vulkan targets (and its recycled values authorized) until process exit. Retirement never runs while the layer's
  state lock is held: the bridge's spin section never nests under a layer lock.
- **The rewrite contract**: no force, or a `DXGI_PRESENT_TEST` (0x1) query, leaves both arguments byte-identical.
  Otherwise `SyncInterval=1`, `DXGI_PRESENT_ALLOW_TEARING` (0x200) is cleared, and `DXGI_PRESENT_DO_NOT_WAIT` (0x8)
  is cleared so the forced FIFO present may block on the vblank; `DXGI_PRESENT_DO_NOT_SEQUENCE` (0x2 - not 0x8) and
  all unrelated flags are preserved. An already-correct interval=1/flags call is a byte-identical no-op.
- **Diagnostics stay per-identity**: registration lines, per-slot present cadence (`#N swapchain=%p slot=%zu`,
  `Present` vs `Present1`, incoming/final interval and flags) and a bounded global cadence for foreign
  pass-throughs. The Present hot path reads only atomics - no mutex, timer, wait, max-frame-latency call, driver
  API, or game-specific heuristic was added.
- The prior crash (COM vtable mutation, session `20260828_212805`) and the overlay flicker (compute-composite
  barrier stage mask, fence re-arm order, submission-ring depth) were separate bugs that are already fixed and are
  not touched by the backstop: vtables stay unwritten and the overlay/ring subsystems are outside this path.

## The overlay submission ring must not pace the game

- **CE's own overlay resource recycling blocks the game's present thread.** `20260830_182939` measured a
  `fence_wait_us` median of 2867 us on more than half of all presents; `20260830_185703`, with the reuse gate below in
  place, measures a median of 30 us but a p99 of 34 ms and a max of 65 ms - roughly 14 ms of a 29 ms rendered frame,
  moved from a constant tax to a spiky one.
- Widening the ring from `imageCount` to `imageCount + 4` on 2026-08-29 moved the threshold and kept the shape. **Any
  fixed depth is a second rate limiter whose period is the slot count**, and beating a 10-slot ring against a
  4-present generated group is an uneven *rendered* frame period even while every vertical blank receives a new image.
  Generated frames carry scene time, so that is judder.
- **The ring is extended by one slot whenever a present finds none reusable** - one command buffer, one fence, one
  binary semaphore - and converges on what the runtime actually keeps outstanding. It is bounded by
  `CustomOverlay::VulkanBackend::kFramePoolSize`, which is a correctness bound rather than a tuned depth: that pool's
  index is free-running, so more submissions in flight than it has entries and the CPU overwrites geometry the GPU is
  still reading. `layer_overlay_render.cpp` asserts the two constants against each other.
- **The compute-composite route grows too.** It owns a full-resolution offscreen target per slot, and an appended slot
  gets none - and needs none: it fails that route's own resource bounds check before either queue sees work and falls
  through to the direct graphics path for that present. Refusing to grow there is what left `20260830_185703` logging
  `could not be extended past 10 slots ... growths=0` past its 1024th occurrence, because Portal RTX presents from a
  compute-only queue (`Present topology - present queue family=2 ... graphics=0`).
- **Slot reuse needs two facts and the fence only proves one.** The fence proves CE's submission retired, so its
  command buffer may be re-recorded. It says nothing about the binary semaphore that submission signalled: the present
  waiting on it is a queue operation that can still be pending, and re-signalling a binary semaphore whose wait has
  not executed is undefined behaviour. Without `VK_EXT_swapchain_maintenance1`'s present fence the sound proof is the
  swapchain itself - `vkAcquireNextImageKHR` returning image i means every present of image i executed its wait - so
  `SwapchainData` carries one acquire counter per image and a slot is reusable only once its image's counter moved.

## Compute-composite synchronization

- **A semaphore wait only orders the stages named in its `dstStageMask`.** The compute composite waits on the game's
  own present semaphores at `VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT`, and its acquire barrier - the
  `PRESENT_SRC_KHR -> GENERAL` transition - used `VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT` as its `srcStageMask`. TOP_OF_PIPE
  is not in the wait's mask, so that transition was free to retile the image while the game's own composite into it
  was still running. **The visible form of that race is an overlay that appears on some presented frames and not
  others**, which is the reported flicker. The barrier now names COMPUTE_SHADER on both sides. The release barrier
  (`GENERAL -> PRESENT_SRC_KHR`, dst BOTTOM_OF_PIPE) was already the correct present idiom, and the direct graphics
  route was already correct too - its render pass dependency names COLOR_ATTACHMENT_OUTPUT, which is exactly what its
  submit waits at.
- **A slot fence is disarmed only for the submit that carries it.** `vkResetFences` used to run before the offscreen
  graphics submit, so a failed submit left that fence unsignalled for the lifetime of the swapchain and the ring lost
  a slot permanently and silently. The reset now sits immediately before the composite submit, and a failed composite
  submit re-arms the fence with a fence-only submit rather than stranding the slot. Both failures are logged.

