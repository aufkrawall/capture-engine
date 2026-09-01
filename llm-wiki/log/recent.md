# llm-wiki Log

### 2026-09-01 - Nominal DLSS-G and nested Reflex Sleep cannot halve a displayed-rate cap

Gothic Remake session `20260831_223114` retained the corrected 120-fps NVIDIA driver target, but exposed two
independent local pacing defects. Streamline reported DLSS-G ON/default 2x for about 19 seconds while GetState still
reported only one presented frame, no Reflex Sleep occurred, and no FG feature existed; CE nevertheless divided the
120 cap to 60. At real 4x activation, every Streamline `slReflexSleep` synchronously entered CE's NvAPI Sleep hook.
Both wrappers applied the 33.3 ms hybrid group period and both incremented the evidence counter, producing roughly
65 ms rendered groups / 60 displayed fps and prematurely satisfying native-handoff evidence.

DLSS-G limiter scaling now requires its nominal runtime signal plus recent successful game-owned Reflex Sleep (or
equivalent Vulkan native ownership). Pending/suspended production stays on the unscaled real-frame cadence and
automatically re-enters 2x/3x/4x grouping when evidence resumes; diagnostics split `fgSignal` from pacing `fg` and
name the proof. A thread-local logical-Sleep boundary gives only the outer Streamline/NvAPI traversal ownership of
hybrid pacing and evidence, with sparse nested-coalescing diagnostics. The Streamline viewport reducer also publishes
the configured `dlss_fg_factor` immediately, clamped by known capability, so a 4x override no longer appears as 2x
until main-menu feature creation. During native recovery, only a newly advanced successful Sleep with an accepted
driver target suppresses CE's fallback for that exact frame, removing the short double-cadence jitter window without
letting merely recent stale evidence delay fallback. Reflex off/on also rebases any old high Sleep-counter baseline,
so multiple suspension epochs cannot strand recovery. Focused limiter/Reflex and Streamline policy suites cover
pending -> producing -> suspended transitions, progress-qualified recovery, exact nested ownership, wrapper wiring,
and early factor publication. Hardware validation is pending. See `graphics-overrides-and-frame-pacing.md` and
`overlay-fg-status.md`.

### 2026-08-31 - Capture startup cannot remove a concurrent final-output cap under MFG

Session `gothicfglimitlogs` showed both a real brief encoder overload and an avoidable pacing transition. Before
recording, the 120-fps general Reflex cap correctly asked NVIDIA for 120 displayed fps under 4x MFG. Media's delayed
inject handshake then gave capture sync unconditional priority and changed the request to 120 base fps / 480
displayed fps. Source load rose, a cutscene produced a 44.648 ms Present gap, encoder pressure briefly accumulated
67 ms of CFR debt, and the unchanged media timeline recovered it in the following healthy window.

Capture sync and the general limiter now compose in the final-output domain. Explicit DX12/Vulkan final-output inject
routes publish that domain before the media handshake; an ordinary base route is multiplied only for constraint
comparison. The stricter 120-fps general cap therefore remains 120 through effective game-selected or overridden
2x/3x/4x factors, while equal final-output constraints retain the phase-preserving capture grid. Diagnostics report
both constraints and `captureSource=base|final`. A Present gap still resets Reflex Sleep evidence, but three fresh
successful game Sleep calls now restore native ownership without layering CE's local cadence for the full 500 ms
diagnostic grace. CFR PTS, audio, and overload recovery policy are unchanged. Fresh game validation is pending.

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
