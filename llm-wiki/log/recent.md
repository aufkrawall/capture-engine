# llm-wiki Log

### 2026-08-31 - Cutscene display-phase jump cannot latch inject CFR into repeats

Session `gothicimprovedbutstilldied` contained a real but brief game/encoder stall: at 07:46:31 the game Present gap
reached 66.896 ms and one encoder service reached 180.098 ms, with a 7.05 ms EMA. By the next second encoder EMA was
0.88 ms with no overload, DLSS MFG remained active at 4x, and inject still supplied about 120 final outputs per second.
The subsequent static-looking capture was therefore not sustained NVENC overload or an FG-off interval.

The cutscene moved display-correlation phase from roughly 14-22 ms to 67-81 ms. With a fixed 13-frame live cap,
media trimmed the oldest future candidate immediately before it aged into the CFR target, then repeated while another
candidate was available; the session ended with 803 such holds and 786 cap trims. Since source and target advanced at
the same rate, the old policy would remain latched while that phase persisted.

Live inject retention now derives its cap from the actual newest-to-target timestamp span (bounded to 28 of the 32
ring slots). At the cap it preserves a future/target-adjacent front and trims the newest eligible frame before the
fence tail; stale encoder backlog still trims from the front. Final-output/base-Present transitions reset outgoing
correlation, freeze older path segments, and rebase the new base segment once for monotonic continuity. New telemetry
and `inject_cfr_timestamp_retention_fault` distinguish this failure from ongoing overload. See `cfr-capture-sync.md`.

### 2026-08-31 - DLSS 4x capture clock cannot inherit a stale worker-handoff phase

DX12 inject session `20260831_070957` published thousands of changing final Streamline outputs, but media encoded a
static picture: 2 unique frames and 2,732 repeats across 2,734 CFR ticks, with `targetHold=2732` and no encoder,
mux, ring, or GPU-capacity overload. The producer timestamps were about 19.2 seconds ahead of media's selection
target, so its correct too-new-candidate policy retained the incoming frames and repeated the last eligible frame;
the buffer then emergency-trimmed 2,393 candidates. Recording health was therefore healthy but not evidence that
the source timestamp phase was usable.

The stale phase formed before recording. After Streamline moved final Presents from the application thread to its
worker, source-boundary observations stopped, but the last complete transition-era group remained authoritative
forever. Its 152,770-QPC output interval (about 65.5 Hz) advanced a virtual clock on callbacks arriving near 138 Hz,
so the lead accumulated throughout idle overlay service and crossed into the later capture epoch.

The generic DX12/Vulkan final-output policy now resets its clock on each recording capture epoch, expires source-group
authority after the greater of 250 ms or eight estimated output intervals without another boundary, and bounds a
monotonic virtual timestamp to four intervals ahead of the callback that supplied the pixels. Heartbeats expose
`virtualLeadUs`, `sourceExpiry`, and `leadClamps`; media warns rate-limited when a held inject candidate is still more
than 250 ms in the future. Unit coverage reproduces the stale 65.5-versus-139 Hz topology and preserves a valid 4x
burst. Fresh DLSS 4x hardware recording validation is pending. See `cfr-capture-sync.md`.

### 2026-08-31 - Vulkan overlay under 4x MFG: one composite route, and a live FG factor

Portal RTX session `20260831_054801` reported two separate defects under 4x DLSS multi-frame generation.

The overlay's FG factor froze on the value the game started with. The Vulkan layer is its own DLL, so it mirrors the
hook DLL's DLSS-G state out of shared memory - and that mirror sat inside the FPS limiter's
`if (!asyncPresentDetected)` block, which latches off permanently the first time the game acquires and presents from
different threads (7.5 s into this run). `hook_debug.log` recorded the in-game 4x -> 3x change; `vulkan_layer.log`
never left `multiplier 0 -> 4`. The mirror now runs on every present. See `overlay-fg-status.md`.

The overlay's translucency flickered because two composite routes alternated per present. Compute-route resources are
indexed slot-major per (submission slot, target image), and they were sized once for the ring's initial depth: 6
images, 10 slots, 60 cached composites. 4x MFG extended the ring to 12, and the two appended slots indexed past every
compute-route array, failed that route's bounds check, and fell through to the direct render-pass route - the
compute -> graphics -> compute round trip the compositor exists to remove on a compute-only present queue.
`Compute-present CPU summary` counted 2048 composites per 15.9-17.1 s window against 143 Hz presents in
`perf_metrics_28608.csv`. Ring growth now extends the compute route with it or declines the growth and falls back on
the existing bounded backpressure. Also added: a repeat-present guard (a blend is not idempotent, so one image must
not be composited into twice while its acquire generation is unchanged) and a `vkAcquireNextImage2KHR` hook, since
both that guard and the ring's slot-reuse proof read the acquire generation. See `overlay-rendering.md`.

Hardware validation of both fixes is pending.

### 2026-08-31 - Face-camera teardown cancels the reader before source shutdown

Session `20260831_060838` recorded and GPU-composited the face camera successfully, then stopped logging immediately
before `VideoEncoder::Stop` could emit its first line. The preceding audio-resource destruction placed the stall in
`StopFaceCamera`. That path inverted Media Foundation's cancellation ownership: the finalization thread called
`IMFMediaSource::Shutdown` while the camera worker was blocked in synchronous `ReadSample`, and only afterward called
the Source Reader operation documented to cancel pending reads. Stop now sets the stop intent, flushes the active
reader, joins the released worker, and leaves media-source shutdown exclusively on the worker that created it. Bounded
phase logs distinguish a future flush stall from a join/cleanup stall. A hardware smoke on the same default camera
recorded and GPU-composited for 25.2 seconds; flush returned `S_OK` in 16 ms, the worker joined in 297 ms, the mux
closed, and async finalization completed. See `face-camera-overlay.md`.

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
