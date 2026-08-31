# llm-wiki Log Archive 2026-W35d

### 2026-08-30 - The metering fix was measured and was not the mechanism; the discriminator is who asked for the mode

Follow-up run `20260830_182939` on the capability withholding below. It took effect end to end - `withholding
VK_NV_present_metering from vkEnumerateDeviceExtensionProperties (1 of 291 device extensions)` and `set
rtx.dlfg.enablePresentMetering=False` - and changed nothing: `Vulkan DXGI FIFO: final Present1 #1024` and `#2048`
still report `SyncInterval=0->1 Flags=0x200->0x0`, and the base frame period distribution is the same (p25/p50/p75
22.7/26.4/30.3 ms against 21.6/26.3/28.7 ms). Hardware present metering is not what displaces the vertical-blank
wait in Portal RTX. Withholding it is kept, because two pacing authorities on one set of presents is still wrong,
but it explains nothing.

**The session that disproved it also named the real discriminator, and it is not frame generation.** The application
created four swapchains: two it asked for FIFO itself, two CE overrode from Immediate. The application's own FIFO
swapchain is presented with `SyncInterval=1`. The one CE overrode is presented with `SyncInterval=0` plus
`DXGI_PRESENT_ALLOW_TEARING`. Same process, same WSI, same driver, 3.4 seconds apart. **The WSI honours FIFO - the
overridden swapchain's effective mode is simply not FIFO**, which means a present-mode selection is happening
somewhere CE does not look. `VK_EXT_swapchain_maintenance1` is exactly that: compatible modes listed at creation,
one picked per present, no recreation. DXVK - which Remix is built on - uses it.

That is a hypothesis, and the last one cost a game session, so this round ships the measurement with the fix:
`vulkan_present_chain_policy.h` prints the `pNext` chain CE was handed at `vkCreateSwapchainKHR` (once per swapchain)
and at `vkQueuePresentKHR` (once per distinct shape), `vkCreateDevice` prints which presentation-relevant extensions
the application requested, and *if* the chain carries a per-present selection that disagrees with the created mode,
CE substitutes its own head node forcing the created mode back. The forced value is always the swapchain's own
creation mode: Vulkan requires the selection to name a declared-compatible mode, and that is the one value
guaranteed to be in the set.

**The second finding is CE's own, it is measured rather than inferred, and it is almost certainly part of the
judder.** `perf_metrics_13360.csv`: `fence_wait_us` median **2867 us**, p90 7989 us, p99 14999 us, max 27654 us, on
more than half of all presents - about 14 ms of a 29 ms rendered frame spent blocking the game's present thread
inside CE's overlay submission ring. This is the 2026-08-29 finding recurring at the wider ring: widening it from
`imageCount` to `imageCount + 4` moved the threshold and kept the shape. **Any fixed depth is a second rate limiter
whose period is the slot count**, and beating a 10-slot ring against a 4-present generated group is an uneven
rendered frame period even while every vertical blank receives a new image.

So the depth stopped being predicted. The ring is extended by one slot whenever a present finds none reusable - one
command buffer, one fence, one binary semaphore - and converges on whatever the runtime actually keeps outstanding.
`kMaxSubmissionSlots` is a memory-safety bound, not a tuned depth, and reaching it keeps the old blocking behaviour
so the failure stays bounded and logged.

Building that surfaced a real correctness bug, and the best candidate yet for the reported overlay flicker: **the
fence proved the wrong thing.** It proves CE's submission retired, so its command buffer may be re-recorded. It says
nothing about the binary semaphore that submission signalled - the present waiting on it is a queue operation that
can still be pending, and re-signalling a binary semaphore whose wait has not executed is undefined behaviour. Under
FIFO those two moments are several vertical blanks apart. Without `VK_EXT_swapchain_maintenance1`'s present fence the
sound proof is the swapchain itself: `vkAcquireNextImageKHR` returning image i means every present of image i has
executed its wait. `SwapchainData` now carries one acquire counter per image and a slot is reusable only once the
counter for the image it was used with has moved.

Three things worth keeping:

- **A counter that tracks the expected rate is not evidence a stage is correct** - and neither is a fix that took
  effect. `withholding ... (1 of 291)` proved the code ran, not that it mattered. The measurement that discriminated
  was two swapchains in one session differing only in who asked for the present mode.
- **Widening a ring is not the same as removing it from the critical path.** A fixed depth just moves where the
  aliasing starts.
- The perf CSV had all of this in it before either game session: `fence_wait_us`, the group period, the image
  indices. Read the columns that describe CE's own cost before theorising about the driver's.

**Not validated on hardware.** The confirmation signals are, in order: the `pNext chain:` lines naming what the
runtime actually chains, `fence_wait_us` collapsing to microseconds, `overlay submission ring extended to N slots`
converging, and only then `final Present1 #N` reporting `SyncInterval=1->1 Flags=0x0->0x0`.

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
