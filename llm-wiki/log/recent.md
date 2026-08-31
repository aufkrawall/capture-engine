# llm-wiki Log

### 2026-08-31 - Optional hardware sensors stay outside the game and release package

Added `[HardwareSensors]` polling for CPU/GPU temperature, package power, and GPU fan RPM through
`plugins/LibreHardwareMonitor`. `sensor_plugin.cpp` owns a persistent Windows PowerShell bridge in a kill-on-close job;
only the dedicated sensor service sees its bounded tab protocol, while hooks receive validated values through the
versioned shared-metrics publication. GPU `auto` follows the highest GPU Core load without equal-load flapping, exact
identifiers can pin any sensor, stale/invalid values disappear, and the existing CPU/GPU rows remain the only UI.
The CPU/GPU-only LibreHardwareMonitor 0.9.6 runtime was reduced by an exact subset matrix to four matching user-supplied
files: `LibreHardwareMonitorLib.dll`, `System.Memory.dll`, `System.Numerics.Vectors.dll`, and
`System.Runtime.CompilerServices.Unsafe.dll`. Build finalization installs only the MIT bridge/setup text and preserves
local DLLs; archive staging has a separate two-file plugin allowlist, and `LibreHardwareMonitor_NOTICE.txt` records the
MPL-2.0/source/notices boundary. A live four-file 0.9.6 smoke published NVIDIA temperature and shut down cleanly; focused
native tests, packaging isolation, warning-profile syntax checks, and 60-second ASan/libFuzzer runs for config, sensor
protocol, and IPC passed. See `configuration.md`, `overlay-rendering.md`, and `fuzzing.md`.

### 2026-08-31 - Face camera stays off the capture clock and on the encoder GPU

Added opt-in `[FaceCamera]` ingest and composition for inject and WGC/DXGI video. A private below-normal-priority
Media Foundation worker publishes only its latest immutable camera sample; the encoder never waits or queues camera
frames. Compatible SourceReader samples stay GPU-resident, with one BGRA upload per camera frame as the driver-safe
fallback. One D3D11 draw supplies output-relative position, fill/stretch crop, mirroring, opacity, analytic
rectangle/rounded/circle masks and borders, plus SDR/scRGB/HDR10 paper-white mapping before the existing single color
conversion. Dynamic CFR repeats retain an uncomposited accepted source and redraw camera/cursor state; inject stages
that cache before submission but commits it only after encoder acceptance. See `face-camera-overlay.md`.

### 2026-08-31 - RTMP/RTMPS live streaming reuses CFR/audio timing and fails as a unit

Added opt-in stream-only `[Streaming]` output for YouTube, Twitch, and custom
RTMP services. The config policy preserves the selected NVENC/AMF/QSV/MF
backend while selecting H.264 CBR, a two-second GOP, low-latency backend
settings, BT.709 8-bit 4:2:0, and one mixed 48 kHz stereo AAC track. The FLV
mux path is shared by injected and WGC/DXGI output and retains the normal CFR,
audio-reservoir, timestamp, and packet-timeline contracts. Live protocol calls
have a five-second interrupt deadline and terminal-failure abort; the packet queue
has an approximately two-second bitrate-derived budget. A stall/overflow fails
the session and requests orderly stop instead of applying wall-clock-breaking
backpressure or dropping predictive packets. Destinations/keys are redacted.
FFmpeg's raw logger is silenced only in live mode because RTMP diagnostics can
include the publish playpath; operation/error-code diagnostics remain active.
Bundled FFmpeg still disables protocols by default and now allows only
file/RTMP/RTMPS/TCP/TLS using Windows Schannel; the RTMPS wrapper now forwards
TCP_NODELAY to its underlying socket. See `live-streaming.md`.

### 2026-08-30 - Export body hooks retry while armed, cheaply

Audit follow-up to the route-agnostic first factory observation: the three `CreateDXGIFactory*` export body hooks
were installed only on the disarmed→armed transition, so a first arm that preceded the load of `dxgi.dll` (or saw
an unresolvable target) left the observation permanently incomplete. `RegisterDynamicFactoryHooks` now calls
`EnsureFactoryExportBodyHooksInstalled` on **every** armed hook-monitor call, still before `g_armed` publishes.
Cost discipline (`hook/wrappers/vulkan_dxgi_fifo_present.cpp`): a complete installation is one atomic conjunction
(`AllFactoryExportBodyHooksInstalled`), an absent module is one `GetModuleHandleA` plus a single one-shot
diagnostic, and a failed attempt is keyed on the loaded dxgi HMODULE (`s_attemptedDxgiModule` CAS) — an unchanged
module image never re-runs the image-mapping on-disk RVA resolve, and a new/first-loaded module re-arms exactly
one attempt. Source guards: `VulkanRendererPolicySourceTest.ExportBodyHooksRetryWhileArmedWithoutPollWork`.
Also inspected the export body install mode: `InlineHook::InstallPublished` already composes with foreign entry
patches (prepend/chain through the exact foreign entry, refusal of non-chainable patches) — no deep-hook change
needed.

### 2026-08-30 - Final-DXGI FIFO re-armed as a per-instance scoped backstop

The retirement from earlier today did not survive a second look. The evidence that killed the override was
unattributed: global `final Present1 #N` counters plus `vkQueuePresentKHR` counts could show the WSI emitting
per-present DXGI parameters, but could not attribute the above-refresh tearing rate (~190/s) to correct variable
refresh. Meanwhile the Remix CPU pacer that CE's metering withholding steers to is group-aware but is not VSync -
it spreads generated batches across the rendered frame interval and never consults the display. With
`vsync_mode=fifo` in Portal RTX 4x MFG the output still ran past refresh with tearing, so the native vblank
contract is back, scoped instead of global.

- `ShouldArmFinalDxgiPresent` arms only when the resident CE Vulkan layer is loaded and `vsync_mode` is `fifo` or
  `adaptive`. `RegisterDynamicFactoryHooks` now stores the decision with an unconditional `g_armed.exchange` (the
  old short-circuit never stored `false`), so disarm is the atomic gate; installed hooks are never live-unpatched.
- New `hook/common/vulkan_dxgi_fifo_registry.h`: bounded 64-slot lock-free open-addressed table of raw swapchain
  pointers, filled by the four creation detours on every successful targeted creation, no COM refs, refresh on
  address recreation, fail-closed when full. DetourPresent/Present1 rewrite only `ShouldForceFifoNow() &&
  registered`; foreign swapchains pass through with a bounded pass-through log.
- Rewrite contract: `DXGI_PRESENT_TEST` (0x1) and no-force pass byte-identical; otherwise `SyncInterval=1`, clear
  `ALLOW_TEARING` (0x200) and `DO_NOT_WAIT` (0x8) so the present may block, preserve `DO_NOT_SEQUENCE` (0x2) and
  all other flags; already-correct calls are no-ops. Diagnostics are per-slot cadence with the swapchain pointer.
- The 20260828 crash (vtable mutation) and the overlay flicker (compute barrier/fence/ring) remain fixed and
  untouched. Tests: `VulkanRendererPolicyTest.FinalDxgiPresentIsNeverArmed` replaced by the new arm/gate/rewrite
  contract plus registry unit tests and source guards in `test_vulkan_renderer_policy.cpp`.
- Follow-up (surface lifetime, tests now in `test_vulkan_dxgi_fifo_scoping.cpp`): the layer records the owning
  `VkInstance` with every tracked surface, and `vkDestroyInstance` sweeps the surfaces the instance still owns,
  retiring their HWNDs exactly once per surface (refcount-safe when surfaces share a window). Explicit
  `vkDestroySurfaceKHR` retirement is unchanged, retirement never runs under the layer's state lock, and the
  `RequestsVblankPacedPresentation` duplicate in `vulkan_dxgi_fifo_policy.h` now calls the canonical helper from
  `vulkan_present_metering_policy.h` instead of drifting next to it.

### 2026-08-30 - CE was forcing fixed vsync onto a variable-refresh display, and the flicker was a barrier

Third round on the Portal RTX judder, and this time the diagnostics shipped last round answered it. Session
`20260830_185703`.

**The measurement killed my own hypothesis first.** `presentation-relevant device extensions requested: <none>` -
no `VK_EXT_swapchain_maintenance1`, no `present_id`/`present_wait`, no `low_latency2`. The swapchain chain is a lone
`VkSurfaceFullScreenExclusiveInfoEXT`, and there is no `vkQueuePresentKHR pNext chain:` line at all because the
present chain is empty. Nothing selects a present mode behind CE's back. Two rounds, two dead hypotheses, both
retired by evidence rather than argument.

**What the same log shows is the answer.** On one FIFO swapchain, seven seconds apart, the same thread:

    final Present1 #1024 force=1 SyncInterval=1->1 Flags=0x0->0x0
    final Present1 #2048 force=1 SyncInterval=0->1 Flags=0x200->0x0

`SyncInterval=0` plus `DXGI_PRESENT_ALLOW_TEARING` is not a driver that forgot to synchronize. **It is how variable
refresh is driven on Windows**: a flip the display is meant to stretch has to be presented that way, so the panel
refreshes on the flip instead of the flip waiting for the panel. The WSI was picking per present. CE was overwriting
every one of them with fixed vertical-blank pacing - which is precisely why the same title is smooth when the
*driver* forces the mode (`vsync_mode` is then not `fifo`, so this path never armed) and judders when CE does. Under
frame generation the rendered frame period is not constant, and a fixed grid turns that variation into stutter while
every present-side frame time stays flat.

`ShouldArmFinalDxgiPresent` now always returns false. No factory hooks, no `Present`/`Present1` body hooks, no
parameter rewriting. `VK_PRESENT_MODE_FIFO_KHR` on the swapchain is the whole contract, and the same WSI honours it -
the application's own FIFO swapchain in that session was presented the same adaptive way. The rate escape the path
was built for was hardware present metering, declined at device level since 0.1.6324.

**The flicker was a real synchronization bug on the route this game takes.** Portal RTX presents from a compute-only
queue (`Present topology - present queue family=2 ... graphics=0`), so CE uses the compute-composite route. That
route waits on the game's present semaphores at `COMPUTE_SHADER` and then acquired the swapchain image with a barrier
whose `srcStageMask` was `TOP_OF_PIPE`. **A semaphore wait only orders the stages named in its `dstStageMask`**, so
the `PRESENT_SRC_KHR -> GENERAL` transition was free to retile the image while the game's own composite into it was
still running. An overlay that appears on some presented frames and not others is exactly what that looks like. The
direct graphics route was already correct - its render-pass dependency names `COLOR_ATTACHMENT_OUTPUT`, which is what
its submit waits at.

Two more things fell out of the same file:

- **A slot fence was disarmed before a submit that could fail.** `vkResetFences` ran before the offscreen graphics
  submit, so a failure left that fence unsignalled for the lifetime of the swapchain and the ring lost a slot
  permanently and silently. The reset now sits immediately before the submit that carries it, a failed composite
  submit re-arms the fence with a fence-only submit, and both failures are logged.
- **The compute route refused to extend the ring**, which is why the log reads `could not be extended past 10 slots
  ... growths=0` past its 1024th occurrence while `fence_wait_us` reaches 65 ms. An appended slot owns no offscreen
  target - and needs none: it fails that route's own bounds check before either queue sees work and falls through to
  the direct path for that present. The ring now grows on both routes, bounded by
  `CustomOverlay::VulkanBackend::kFramePoolSize`, which is a correctness bound and not a depth: that pool's index is
  free-running, so more submissions in flight than it has entries and the CPU overwrites geometry the GPU is still
  reading. The two constants are asserted against each other.

Worth keeping:

- **A diagnostic that prints what you were handed beats three rounds of argument about it.** The extension list and
  the two `pNext` chains cost about forty lines and closed two hypotheses in one session.
- **`force=1 SyncInterval=0->1` was in the log for three sessions and I read it as the driver misbehaving.** It was
  CE's own value on the right of the arrow. An override's log line shows what CE did, not what was wrong.
- Retiring a mechanism is a fix. The DXGI path answered a symptom whose cause was removed a layer up, and kept
  charging rent.

**Not validated on hardware.** Watch: the presented rate staying at or below the refresh rate now that nothing forces
the sync interval (the perf CSV, since the `final Present1` diagnostic goes away with the path), `fence_wait_us`
collapsing, `overlay submission ring extended to N slots` converging, and the flicker.

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
