# llm-wiki Log

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

### 2026-08-30 - The flat line exposed a graph that had always animated at the base frame rate

With display-change timing fixed, the frame time graph animated "like a lower frame rate". It was not the new
measurement: the graph had always advanced by sample arrival, and under frame generation sample arrival is not the
same clock as drawn frames. The sawtooth in the line had been masking it; once the line went flat, the stepping was
the only motion left.

Measured from the real draw timestamps of the 4x MFG session (`perf_metrics_8480.csv`, 5792 draws over 45 s):
**64.1% of overlay draws advanced the graph by zero samples**, 12.6% by three, 10.1% by four - mean 1.00, stddev
2.18. Draw gaps p25=652us p50=804us p75=10.1ms p90=27.2ms: the runtime issues the whole group of presents within
about two milliseconds and then idles, while the display consumes them 7.8 ms apart. So the graph updated at the
base rate in three-to-four slot jumps while the screen updated at 128 Hz.

`hook/common/graph_scroll_policy.h` advances the cursor one slot per drawn frame and pulls it gently toward the
sample stream instead of being driven by it. Two things fell out of building it that were not obvious up front:

- **The cursor must slow, never rewind.** A first version let the correction pull backwards, and a stall then
  dragged the plot back eight slots before crawling forward again. A scrolling graph that steps backwards reads as
  a glitch, not as a correction. Only a genuine stream restart re-arms.
- **The trail is not a tuning constant, it is the group size.** To scroll across a burst the cursor needs N-1 slots
  of already-received samples plus one guard; that is one base-frame interval and cannot be avoided, because the
  samples for those frames do not exist when they are drawn. Measuring the dry streak instead of assuming a value
  costs two slots without frame generation and settled at 4.9 under 4x MFG. Measured dry streaks never exceeded 3,
  matching a group of four - a fixed guess would have been wrong in both directions.

The user asked whether the sub-slot approach's one limitation - a partial empty slot at the right edge - could be
worked around so the graph still looks completely intact. It can: `DrawFrameTimeGraph` now takes a guard sample past
each edge and clips the polyline to the panel by interpolating both crossings, so the curve fills the panel exactly
at every offset. The guard is what the trail pays for.

Replaying the real draw timestamps through the production cursor, settled stretch: **99.05% of draws advance within
+/-20% of one slot** (p1 0.86, p50 0.98, p99 1.09, mean 0.9994), against 64% advancing zero before. Across the whole
session it is 92.1%; the difference is the stretch where the FG factor was being switched between 1x and 4x, where a
transient is the correct response to a changed group size.

Method note: the replay harness reads a session's own `perf_metrics` CSV and drives the production header with it,
so the policy was measured against real burst timing rather than a synthetic pattern. That is worth reaching for
whenever a policy's input is something a session already records.

### 2026-08-30 - The display timing was real, the timestamps were not

`frametime_source=display_change` drew the presentation sawtooth under DLSS 4 MFG while RTSS
`G=msBetweenDisplayChange` drew a flat line on the same frames. Sessions `logs/20260830_064454` and
`logs/20260830_071302`, Talos Reawakened, `published_multiplier=4`, 144 Hz VRR.

The counters said the chain was healthy, and they were right about the count: `runtimePresents=3434
submitAssociations=3434 published=2780 suppressed=0 regressed=0`, one sample per displayed frame, no drops, no
regressions. Streamline issues a real DXGI `Present` per generated frame, so under MFG the runtime present count
already equals the displayed frame count. **Only the timestamp values could be wrong, and every one of them was.**
A stage counter that tracks the expected rate is not evidence the stage is correct.

`frameType(received=0)` for the whole session. The wiki claimed DLSS MFG delivers `FlipFrameType` through the
`Intel-PresentMon` provider; it does not and never did, since that provider's frame-type enumeration names exactly
`Intel_XEFG` (50) and `AMD_AFMF` (100) and has no NVIDIA value. Every frame took the completion fallback, which
publishes the `MMIOFlipMultiPlaneOverlay` event timestamp - and on NVIDIA that is when the driver *programmed* the
flip, not when it reaches the screen. Under frame generation the driver programs the paced flips within a fraction
of a millisecond of each other and holds each until its own scheduled time, so the events burst while the frames
scan out evenly.

The driver announces that scheduled time through its own provider, `{AE4F8626-8265-40D1-A70B-11B64240E8E9}` event 1
`FlipRequest`. **The first fix enabled it and got nowhere: `nvFlipSchedule(received=5550 undecodable=5550
applied=0)`.** The provider has no registered manifest anywhere on this machine - nothing under `WINEVT\Publishers`,
no NVIDIA entry among `logman query providers`' 1171, and `TdhGetEventInformation` answers `ERROR_NOT_FOUND` for
every event - so `TdhGetProperty` can never resolve `alloc`/`vidPnSourceId`/`ts`/`token`. PresentMon reads those
names, so its own NV path cannot work here either; porting it faithfully ported a dead code path.

The payload is therefore read positionally, and the field is **located by what it is** rather than at a hardcoded
offset: the announcement is the only slot holding a QPC near the timestamp of the event carrying it. 58 of 64
samples must agree before a slot is locked; before that, a payload with exactly one plausible slot is already
unambiguous and is used, so there is no uncorrected window at startup. Reads are re-validated, 64 consecutive
rejections re-arm discovery, and eight failed windows abandon the correction rather than guess.

Measured end to end against the production header, 2x DLSS-G via `dx12_dlss_fg_test`, 2768 flips, zero events lost:

    A  flip event timestamp (before)   mean 7.223 ms  stddev 7.083 ms  jaggedness 14.166 ms  p1/p50/p99 0.099/0.476/14.620
    D  with the announcement (after)   mean 7.226 ms  stddev 0.079 ms  jaggedness  0.060 ms  p1/p50/p99 6.970/7.225/7.545

A's p1/p99 pair *is* the sawtooth - alternating ~0.1 ms and ~14.6 ms at a steady 138 fps. The announcements
alternate to match: the application frame announced ~0.002 ms ahead, the generated frame ~7.07 ms ahead, both
programmed ~0.17 ms apart. `located=1 offset=16 rejected=0 undecodable=0 applied=2768`.

Three things worth keeping:

- **The project has its own DLSS-G repro.** `installed/testapp/dx12_dlss_fg_test.exe` reproduces frame-generation
  pacing locally, which turned a "please run the game again" loop into a measurement. Reach for the test apps before
  asking for a game session.
- A provider missing from `logman query providers` predicts nothing about `EnableTraceEx2` succeeding - it succeeded
  and delivered 2768 events - but it does predict that TDH cannot decode them. Enablement and decodability are
  separate questions; the earlier `logman create trace` vs `StartTraceW` note only covered the first.
- Porting a reference implementation faithfully is not the same as porting a *working* one. PresentMon's NV path is
  correct and inert on this machine at once.

Also split out of `graphics-overrides-and-frame-pacing.md` into `display-change-timing.md`, and the ETW session
plumbing out of `display_timing_service.cpp` into `display_timing_etw.h` / `display_timing_health.h` to stay under
the 800-line ceiling.

**Still unvalidated:** MFG factors above 2x, and the overlay end to end in a real game. The health line reports
`completion(...)` and `nvFlipSchedule(...,fieldOffset,abandoned)` so one session settles both.

### 2026-08-30 - A dead renderer's claim silenced the next game's DLSS DLL overrides

Talos Reawakened crashed at startup with a null read inside `nvngx_dlssd.dll`, on the first
`NVSDK_NGX_D3D12_CreateFeature` for feature 13 (Ray Reconstruction), reached through CE's own
`Hooked_CreateFeature_D3D12` (session `logs/20260829_220520`, PID 164). Relaunching the game after restarting
CaptureEngine worked with an unchanged config (`logs/20260829_221333`), which is what made it look racy.

It was stale cross-process state, not timing. The same CE session had been capturing Portal RTX minutes earlier,
where `NvRemixBridge.exe` is a direct child Vulkan renderer of `hl2.exe`; its layer published
`inheritedRendererProcessPid = 12072` at 22:05:30. Only the publishing process can clear that field, and a
renderer that is terminated never runs `LayerIPC_Shutdown`, so the dead PID was still there when Talos was
injected 55 seconds later. `ShouldApplyProcessLocalRuntimeOverrides` compared Talos's PID against it, decided a
child renderer owned the process-local overrides, and logged two lines that named no PIDs at all:

    Runtime preload: skipped because an inherited child renderer owns process-local DLSS/Streamline overrides
    DLSS indicator: skipped because an inherited child renderer owns the process-local registry probe

So `dlss_rr_dll_path` never took effect: Talos loaded its own bundled `nvngx_dlssd.dll` 310.6.0 instead of the
configured 310.7.129, while the NGX parameter hooks - which are not behind that gate - still injected the
profile's `dlss_rr_preset=f`. The good session loads the 310.7 override and creates feature 13 successfully with
the identical preset, so the redirect is the whole difference.

Root cause: the record was a single session-wide PID with no notion of which process tree it spoke for. It now
carries both identities - `CaptureState::inheritedRendererClaim`, renderer PID packed with the client PID the
layer proved it against (ABI 49) - and only that client stands down. A claim from another tree, alive or dead,
cannot suppress anything. The claim is one 64-bit value because the gate also runs inside the `LdrLoadDll`
redirect hook under the loader lock: it must stay a single load, and a process enumeration there would be a
deadlock hazard, so the claim answers "whose tree is this?" by itself rather than having the reader derive it.
The sensor loop additionally reaps a claim whose renderer is absent from a *successful* process snapshot
(`QueryDirectParentProcessId` grew a `processFound` out-param so a failed snapshot is never read as a dead
process); that is hygiene for other readers, never the guard. Both skip messages now name the renderer and
client PIDs - the missing detail that made this cost a crash dump to find.

Regression coverage: `VulkanRendererPolicyTest.ClaimFromAnotherProcessTreeNeverSuppressesOverrides` (the exact
PID triple from the session), the updated `ProcessLocalOverridesMoveOnlyAfterRendererPublication`,
`SharedDefsTest.InheritedRendererClaimPacksBothIdentitiesIntoOneValue`, and source-policy guards that the layer
publishes the client half and the host reaps only on a proven-dead process.

Unvalidated: no game run yet on the fixed build. The natural check is Portal RTX followed by Talos in one CE
session - the previous split-renderer case must still hand its overrides to `NvRemixBridge.exe`, and Talos must
log `Runtime preload: nvngx_dlssd.dll loaded` from the override directory.

### 2026-08-30 - Display-change MFG correlation is a bounded policy, not a causal proof

The display-change source now correlates DLSS MFG's independently delivered `FlipFrameType` payload with MPO
presentation data using the exact `(VidPnSourceId, LayerIndex, PresentId)` identity. This is required when multiple
same-layer PresentIds are in flight; source/layer-only matching can overwrite one transition with another. The
version-1 payload's `TimeStamp` QPC is the generated transition. FrameType 50 and 100 therefore remain distinct
display transitions, while a later HSync/VSync/eligible MMIO completion represents the application's transition.
Generated types do not suppress that completion; a non-generated explicit payload does suppress its duplicate
fallback. Anchors: `captureengine/display_timing_service.cpp`, `captureengine/display_timing_correlation.h`, and
`tests/test_display_timing_correlation.cpp`.

The 24 ms reorder watermark is deliberately only a bounded policy. Independent providers can deliver a matching
kernel completion or FrameType event arbitrarily late, so the watermark cannot prove that the event will never arrive.
An actual causal distinction would require a provider-disabled state, an ordered stream watermark/flush acknowledgement,
or a newer same-stream sequence with a documented monotonic no-late guarantee. Without one, exact no-duplicate plus
no-first-frame-loss is mathematically impossible for arbitrary delays. The reducer commits fallback at the watermark;
any later payload is telemetry-only (`late`) and cannot regress or duplicate history. Its duplicate/late/pending
results and the service's stage/FrameType health counters are diagnostic anchors. Tuple uniqueness is scoped to the
service lifetime and tracked source/PID stream; resets or identity changes clear the reducer state.
