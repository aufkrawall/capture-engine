# Forced FIFO Presentation Under Vulkan

Last cross-checked: 2026-08-30 (withholding VK_NV_present_metering so the FIFO rate contract survives frame generation)

Summary: what it takes for `[Graphics] vsync_mode=fifo` to actually mean "one presented frame per vertical blank" in a
Vulkan title, and why frame generation is the case that breaks every partial answer. Three boundaries are involved -
Streamline's Vulkan proxy above the layer, the layer's own swapchain creation, and NVIDIA's internal DXGI flip
swapchain below it - plus one capability, `VK_NV_present_metering`, that overrides all three for the lifetime of the
device.

Primary sources:
- `hook/vulkan_layer/{vulkan_layer_swapchain,vulkan_layer_present,vulkan_layer_capabilities}.cpp`
- `hook/vulkan_layer/vulkan_present_metering_policy.h`
- `hook/common/{vulkan_dxgi_fifo_policy.h,remix_frame_generation_policy.h}`
- `hook/wrappers/vulkan_dxgi_fifo_present.cpp`
- `hook/apis/{streamline_hook_api,remix_hook}.cpp`
- `tests/{test_vulkan_present_metering_policy,test_remix_frame_generation_policy}.cpp`

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
- Diagnostics, in the order they should appear: `vulkan_layer.log` logs `withholding VK_NV_present_metering from
  vkEnumerateDeviceExtensionProperties` and, if the application still enabled it, `removing VK_NV_present_metering
  from vkCreateDevice`; `hook_debug.log` logs `set rtx.dlfg.enablePresentMetering=False`; and the confirmation is
  that `Vulkan DXGI FIFO: final Present1 #N` reports `SyncInterval=1->1 Flags=0x0->0x0` under frame generation
  instead of `SyncInterval=0->1 Flags=0x200->0x0`. **Not yet validated on hardware** (as of 2026-08-30): no session
  has run with the capability withheld, and whether the Remix CPU pacer engages is the specific thing to check.
- **Watch `fence_wait_us` in the same session.** In `20260830_175147` CE's own overlay submission-slot ring blocked
  the runtime's present thread for a median 0 but a p90 of 6.8 ms and a p99 of 28 ms - one present in every group of
  four - because the app was running far ahead of the display with the rate contract broken. That should collapse
  once presentation is paced again; if it does not, the ring depth is a second, independent defect.
