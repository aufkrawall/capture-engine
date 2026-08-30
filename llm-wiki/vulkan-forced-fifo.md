# Forced FIFO Presentation Under Vulkan

Last cross-checked: 2026-08-30 (metering withholding measured and ruled out; per-present mode selection and the overlay ring)

Summary: what it takes for `[Graphics] vsync_mode=fifo` to actually mean "one presented frame per vertical blank" in a
Vulkan title, and why frame generation is the case that breaks every partial answer. Three boundaries are involved -
Streamline's Vulkan proxy above the layer, the layer's own swapchain creation, and NVIDIA's internal DXGI flip
swapchain below it - plus one capability, `VK_NV_present_metering`, that overrides all three for the lifetime of the
device.

Primary sources:
- `hook/vulkan_layer/{vulkan_layer_swapchain,vulkan_layer_present,vulkan_layer_capabilities}.cpp`
- `hook/vulkan_layer/{layer_overlay,layer_overlay_render}.cpp`
- `hook/vulkan_layer/{vulkan_present_metering_policy,vulkan_present_chain_policy,overlay_submit_queue_policy}.h`
- `hook/common/{vulkan_dxgi_fifo_policy.h,remix_frame_generation_policy.h}`
- `hook/wrappers/vulkan_dxgi_fifo_present.cpp`
- `hook/apis/{streamline_hook_api,remix_hook}.cpp`
- `tests/{test_vulkan_present_metering_policy,test_vulkan_present_chain_policy,test_overlay_submit_queue_policy,test_remix_frame_generation_policy}.cpp`

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
- NVIDIA's Vulkan WSI terminates in an internal DXGI flip swapchain. With a resident CE Vulkan layer and explicit
  `vsync=fifo`, `hook/wrappers/vulkan_dxgi_fifo_present.cpp` registers only system `CreateDXGIFactory*` lookups,
  leaves each real factory and every creation descriptor unwrapped/unchanged, and reads its four creation-method
  addresses (slots 10/15/16/24) without writing them. Inline hooks on those system method bodies observe the returned
  real WSI swapchain. Present/Present1 are then intercepted with a deep body hook placed past the widest recognized
  foreign entry patch, while slots 8/22 remain untouched. Steam and other overlays retain their outer entry-chain
  ownership; CE runs below them and forces DXGI `SyncInterval=1` while clearing `DXGI_PRESENT_ALLOW_TEARING` before
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
  See "The effective present mode is not the created one" below for where the evidence now points.

## The effective present mode is not the created one

- **The discriminator is not frame generation, it is who asked for the mode.** In `20260830_182939` the application
  created four swapchains: two it asked for FIFO itself (`18:29:52.494`, `18:29:53.664`) and two CE overrode from
  Immediate (`18:29:51.448`, `18:29:57.006`). DXGI swapchain #2, the application's own FIFO one, is presented with
  `SyncInterval=1`. DXGI swapchain #3, the one CE overrode, is presented with `SyncInterval=0` plus
  `DXGI_PRESENT_ALLOW_TEARING`. Same process, same WSI, same driver. The WSI honours FIFO; the overridden
  swapchain's *effective* mode simply is not FIFO.
- Remix recreates the swapchain as Immediate at `18:29:57.006`, 96 ms before `DLSS FG ACTIVATED` - the documented "V-Sync
  is automatically disabled when Frame Generation is active" - and CE overrides that creation back to FIFO.
- **The remaining candidate is a present-mode selection CE never sees.** `VK_EXT_swapchain_maintenance1`
  (core-promoted as `VK_KHR_swapchain_maintenance1`) lets an application list compatible modes in
  `VkSwapchainPresentModesCreateInfoEXT` at creation and then pick one per present with
  `VkSwapchainPresentModeInfoEXT`, with no recreation involved. DXVK - which Remix is built on - uses it for exactly
  that kind of switch. **Unverified:** no session has printed the chains yet.
- `hook/vulkan_layer/vulkan_present_chain_policy.h` therefore does two things at once. It prints the `pNext` chain CE
  was handed at `vkCreateSwapchainKHR` (once per swapchain) and at `vkQueuePresentKHR` (once per distinct shape), and
  `vkCreateDevice` logs which presentation-relevant extensions the application requested. And when the chain does
  carry a per-present selection that disagrees with the swapchain's created mode, CE substitutes its own head node
  forcing the created mode back.
- **The forced value is always the swapchain's own creation mode**, never a mode of CE's choosing: Vulkan requires a
  per-present selection to name one of the modes the swapchain declared compatible, and the creation mode is the one
  value guaranteed to be in that set. The substitution is head-only for the same reason the metering unlink is, and a
  deeper node is reported rather than rewritten.

## The overlay submission ring must not pace the game

- **CE's own overlay resource recycling was blocking the game's present thread on more than half of all presents.**
  `20260830_182939`, `perf_metrics_13360.csv`: `fence_wait_us` median 2867 us, mean 3551 us, p90 7989 us, p99 14999
  us, max 27654 us, and `vulkan_layer.log` passes `All 10 overlay submission slots are in flight ... wait=1024` in
  twelve seconds. Four presents of a generated group carry roughly 14 ms of that inside a 29 ms rendered frame.
- This is the 2026-08-29 finding recurring at the wider ring. Widening it from `imageCount` to `imageCount + 4` moved
  the threshold and kept the shape: any fixed depth is a second rate limiter in the present path whose period is the
  slot count, and beating a 10-slot ring against a 4-present group is an uneven *rendered* frame period even while
  every vertical blank receives a new image. Generated frames carry scene time, so that is judder.
- **Fix: the ring is extended by one slot whenever a present finds none reusable**, which costs one command buffer,
  one fence and one binary semaphore and converges within a few frames on whatever the runtime actually keeps
  outstanding. `kMaxSubmissionSlots` is a memory-safety bound, not a tuned depth; reaching it keeps the old blocking
  behaviour so the failure stays bounded, and says so in the log. The compute-composite route does not grow: each of
  its slots also owns a full-resolution offscreen target.
- **Slot reuse needed a second fact the fence never proved.** The fence proves CE's submission retired, so its
  command buffer may be re-recorded. It says nothing about the *binary semaphore* that submission signalled: the
  present waiting on it is a queue operation that can still be pending long afterwards, and re-signalling a binary
  semaphore whose wait has not executed is undefined behaviour. Under FIFO the gap between the two is several
  vertical blanks, which is a strong candidate for the reported overlay flicker.
- Without `VK_EXT_swapchain_maintenance1`'s present fence the sound proof is the swapchain itself:
  `vkAcquireNextImageKHR` returning image i means the presentation engine has finished with image i, so every present
  of image i - including the one that waited on this slot's semaphore - has executed its wait. `SwapchainData`
  carries one acquire counter per image; a slot records the image and counter value it was used with, and is
  reusable only once that counter has moved. It stays exact when a generated-frame runtime presents one image
  several times without re-acquiring: those presents all complete before the image comes back.
- Diagnostics: `overlay submission ring extended to N slots` (first eight growths, then powers of two) and, only if
  extension is impossible, `overlay submission ring could not be extended past N slots`. The confirmation signal is
  `fence_wait_us` collapsing to microseconds.
