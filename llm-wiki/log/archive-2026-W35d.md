# llm-wiki Log Archive 2026-W35d

### 2026-08-30 - Suppressing the request could never restore the wait; the capability had to go

Portal RTX with `vsync_mode=fifo` looked "very stuttery despite the frame time graph being flat", and forcing the same
mode in the NVIDIA driver instead looked smooth. Session `20260830_175147`, 4x MFG, `perf_metrics_7260.csv`.

The previous revision of this page's own open question answered itself. `vulkan_layer.log` reports
`frame-generation present metering numFramesPerBatch=4 ... - suppressed` from the moment the frame-generation
swapchain is created, and `hook_debug.log` still reports `Vulkan DXGI FIFO: final Present1 #1024 force=1
SyncInterval=0->1 Flags=0x200->0x0` seven seconds later. **NVIDIA's Windows WSI decides how it will present a
swapchain when the device is created**, so unlinking `VkSetPresentConfigNV` from every `VkPresentInfoKHR` removes the
pacer and changes nothing about the vertical-blank wait.

What was left is the reported symptom, and the numbers name it exactly. With no pacer, CE's forced DXGI sync interval
quantizes an unpaced burst onto consecutive vertical blanks, so a group of four generated frames is drawn across
4/refresh whatever the rendered frame it belongs to cost. Measured group period in that session: **21 ms to 48 ms,
mean 27.8 ms**, against a display that consumed every group in the same 27.8 ms. Motion runs fast for a group and then
freezes; present-side frame times stay flat because the presents really are regular. The graph was not lying.

The fix withholds the capability rather than the request: `VK_NV_present_metering` is removed from
`vkEnumerateDeviceExtensionProperties`, `presentMetering` is reported `VK_FALSE` from `vkGetPhysicalDeviceFeatures2`,
and the name is dropped from `VkDeviceCreateInfo` (transactionally - a driver that rejects the create info because the
application also chained the feature structure gets the extension back rather than a game that will not start). Remix's
own `rtx.dlfg.enablePresentMetering` is set to `False` through the `SetConfigVariable` route CE already owns, because
a runtime that still thinks it has hardware metering never engages the CPU pacer it uses instead. CE adds no timer and
no cap of its own.

Three things worth keeping:

- **A burst of N present calls inside a millisecond is the normal shape of a metered batch, not a pacing failure.**
  The 2026-08-29 entry read "bursts of four inside ~700 us" as evidence and removed the driver's pacer because of it.
  The runtime issues the whole group at once by design; the group *period* against the display is the measurement that
  discriminates, and it was in the same CSV all along.
- **A capability granted at device creation is not undone by editing the calls that use it.** Reach for the
  enumeration and the create-info list when the driver's behaviour is latched, not for the per-call structure.
- `perf_metrics`' `fence_wait_us` is worth reading on every pacing complaint. In this session CE's own overlay
  submission-slot ring blocked the runtime's present thread on one present in four - p90 6.8 ms, p99 28 ms, max 41 ms -
  because the app was running far ahead of a display it was no longer synchronised to. Symptom or second defect is
  decided by whether it collapses once presentation is paced again.

**Not validated on hardware.** No session has run with the capability withheld. The confirmation is
`Vulkan DXGI FIFO: final Present1 #N` reporting `SyncInterval=1->1 Flags=0x0->0x0` under frame generation, and the
open risk is whether Remix's CPU pacer actually engages.
