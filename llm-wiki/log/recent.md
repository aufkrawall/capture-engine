# llm-wiki Log

### 2026-09-02 - nv_lod_spread_fix silently refused a driver-relocated branch

Filter Tester DXVK x86 session `20260902_065516` reported Vulkan negative-LOD-bias shimmer that D3D10/11/12 did not
show, with `nv_lod_spread_fix=on`. CE had not regressed: 32.0.16.1656 moved the 32-bit ICD's validated branch to
`nvoglv32+0x576C27`, and because module bases are 64KB-aligned that site is 7 modulo 8 - the single alignment where a
two-byte NOP pair spans two aligned 64-bit words. The writer correctly fails closed there rather than tearing an
instruction, so hook and layer both found the site and patched nothing. The same driver's x64 branch is at
`nvoglv64+0x4C413B`, so 64-bit titles kept working and the symptom looked API-specific. The fix removes the alignment
dependency instead of widening the CAS (32-bit processes have no `cmpxchg16b`): the branch is now neutralized by
zeroing its `rel8` displacement, one byte, which cannot tear at any address, and `jcc +0` falls through to the ON path
exactly as `90 90` did. Both forms are recognized as already-patched, so on-disk-patched drivers and older CE builds
still resolve. Coverage: every alignment in a page, the real 32.0.16.1656 x86 instruction sequence, the Strange
Brigade x64 site, and both already-neutralized encodings.

### 2026-09-02 - Async DLSS-G invalidated timestamp-only PC-latency pairing

Talos session `20260902_011013` initially reported DLSS-G `presented=2`, but later health records repeatedly showed
`presented=1` with a frozen fence while the nominal publication remained DLSS FG 2x. The later interval therefore was
not a valid FG-on comparison. More importantly, the sensor was publishing a screen timestamp without the runtime
`PresentStart` that produced it; once generated and application presents were interleaved, the marker matcher could
select a newer marker submitted before an older frame reached the display and undercount latency. ABI 54 now carries
the reducer's associated runtime `PresentStart` alongside each display sample, and marker matching is bounded by that
association. Marker cadence is authoritative when a native report supplies it; nominal FG cadence is only a fallback.
CE still consumes the game's real Streamline/Reflex markers and does not inject synthetic markers. Overlay PC-latency
sample logging is rate-limited to one record per ten seconds so future comparisons include value, sample count, and the
overlay's nominal FG/base/output-FPS state.

### 2026-09-02 - D3D Streamline latency consumes the game's PCL markers

Talos session `20260902_003841` loaded both `sl.pcl.dll` and `sl.reflex.dll`, enabled Reflex, and accumulated hundreds
of successful game-owned `slReflexSleep` calls, but the overlay reported only `Latency est.`. The display timeline was
healthy; CE's native marker query returned no samples. The root cause was a provider mismatch: CE queried
`NvAPI_D3D_GetLatency`, whose report is populated by `NvAPI_D3D_SetLatencyMarker`, while Streamline games submit the
separate cross-IHV markers through `slPCLSetMarker`.

The Streamline hook now resolves/intercepts the game's existing `slPCLSetMarker` function and records only
SimulationStart/PresentStart in a lock-free fixed ring. The D3D latency provider prefers fresh complete PCL pairs,
then retains native NVAPI and presentation/display fallback behavior. CE deliberately does not inject synthetic
markers: a game with no valid PCL pairs remains `Latency est.` instead of receiving a plausible but false
`PC Latency~`. Reports expire after two seconds, plugin unload invalidates the feature hook, failed forwards are
rate-limited, and focused pairing/order/reuse/freshness/provider/source-policy tests cover the path; see
`overlay-rendering.md`.

### 2026-09-01 - Vulkan/DLSS publications cannot escape their renderer process tree

Session `20260901_174634` proved two stale-state failures in one CaptureEngine lifetime. Portal RTX's child renderer
left the global DLSS FG state active, so Filter Tester immediately displayed DLSS FG 3x without a DLSS runtime. The
same session-global Vulkan bit made Talos DX12 treat a transitive `vulkan-1.dll` load as authoritative layer
ownership, bypass every D3D/DXGI hook, and render no CE overlay. Restarting CaptureEngine only hid both defects by
recreating the mapping.

Shared ABI 53 replaces those unowned values with renderer/client process-tree claims. Vulkan ownership, recent
present/thread evidence, and overlay-active evidence are PID-tagged; DLSS FG publisher, active state, and multiplier
are one coherent 64-bit publication. Direct renderers claim themselves, inherited Vulkan children claim their exact
profiled parent, unrelated processes reject both halves, and owner-only teardown cannot clear a newer target. The
sensor loop reaps provably dead Vulkan claims only as hygiene. Transition-only diagnostics identify replaced,
rejected, and foreign claims, and the periodic DX9 bootstrap-skip message is exponentially rate-limited.

Portal also returned `VK_ERROR_DEVICE_LOST` from the overlay submission-slot fence probe during early close. CE now
latches that result per device, performs no later overlay GPU work, and skips its cleanup idle wait. The x86 parent
dump and x64 full renderer dump place CE's hook/layer lifecycle threads in normal waits; the blocking application
thread is inside DXVK Remix, so the supplied evidence does not attribute the freeze to CE. Focused process-tree,
publication, teardown-race, device-loss source-policy, and existing renderer-policy tests pass, as does the full
x64/x86 product build; see `dx12-injection-bootstrap.md`, `overlay-fg-status.md`, and `overlay-rendering.md`.

### 2026-09-01 - Resume restores a failed verification gate

Portal RTX verification exposed that plain `--resume` retained the failed build identity but selected build-only
stages, so a prior `--verify` could end with tests, lint, and sanitizers unrun. Resume now reads the validated failed
manifest's authoritative `mode=verify` (with prior `--verify` as backward compatibility), restores verification
before gate selection, and carries that mode through another failed resumed run. Focused Python tests cover verify,
legacy-argument, and ordinary-build manifests; see `build.py.md`.

### 2026-09-01 - Layer-created Vulkan queues need loader dispatch data

Portal RTX session `20260901_071629` crashed twice during CE's initial Vulkan font upload. Both x64 dumps end in
`SteamOverlayVulkanLayer64!vkQueueSubmit`; CE's reserved queue still began with `ICD_LOADER_MAGIC` (`0x01CDC0DE`)
instead of the parent device's loader dispatch pointer. CE had widened queue family 0, called the next layer's
`vkGetDeviceQueue` directly for index 1, and registered the result without the loader trampoline that normally
initializes a newly returned dispatchable object. Steam therefore found no dispatch record and jumped through a
null function slot. The later `hl2.exe` dumps are secondary: RTX Remix exited the host after its bridge died.

`Capture_vkCreateDevice` now preserves the chain's `VK_LOADER_DATA_CALLBACK` before advancing its link. A reserved
queue is published only after `pfnSetDeviceLoaderData` succeeds and its dispatch key matches the parent device. The
loader-documented parent-pointer copy remains the compatibility path when an old loader supplies no callback;
callback rejection, missing keys, and false success disable the reserved queue and retain the synchronized borrowed
game-queue fallback. Ready/failure logs expose the exact loader-data outcome. Focused tests cover every outcome and
pin initialization before `RegisterQueue`; see `overlay-rendering.md`.

### 2026-09-01 - Display-correlation phase cannot consume the inject texture pool

Gothic Remake session `20260901_040907` was not encoder overload. Across the 187.3-second recording, NVENC emitted
all 22,476 CFR packets, its normal service stayed roughly 0.2-1.8 ms, overload/debt remained zero, DLSS MFG stayed
4x, and the final-output hook continued near 120 callbacks/s. At about 33 seconds the unique capture rate nevertheless
fell from roughly 120/s toward 40/s and never recovered; 11,186 of 22,476 video slots repeated.

Game hitches moved NVIDIA's measured virtual-to-display correlation phase from about 15 ms to 360 ms. That phase can
contain both scheduling latency and slow virtual/display clock drift. Media added it wholesale to CFR source timestamps
and adaptively retained as many as 28 metadata entries, unaware that the
producer owns only sixteen reusable shared textures. `CpuLease` contention began at the exact cadence collapse and
reached 5,706. The intentional retained buffer also reached the old producer-throttle threshold, forming a closed
loop: fresh pixels were suppressed while fresh arrivals were the only way the selector could close its future gap.

Display matches are now normalized by their learned common transport phase, so they contribute only bounded
screen-cadence residuals around the already-smooth virtual final-output clock. Four sustained 20 ms mismatches retire
and reacquire the phase with a 500 ms window while virtual timestamps remain the nonblocking fallback. Adaptive
startup headroom and adaptive retention are normally bounded to fourteen of sixteen texture leases, leaving two
producer/handoff slots while preserving any larger explicit A/V-delay minimum, and `throttleCapture`
uses only real ingress-queue or fence pressure rather than retained history. New logs separate `correlationPhaseUs`,
`timestampCorrectionUs`, phase reacquisition, and producer throttle transitions. CFR PTS, A/V endpoints, configured
audio-latency video delay, encoder policy, and Present-thread nonblocking behavior are unchanged. See
`cfr-capture-sync.md`.

### 2026-09-01 - Inject overload recovery measures cheap repeats and sheds dynamic-overlay repeat work

Gothic Remake session `20260901_031705` was not a recovered one-shot overload followed by a poisoned capture
cadence. It was a sustained 4K120 AV1 NVENC capacity failure from 03:19:27 until stop: preset p5 and the enabled
1280x720 camera left fresh/repeated service around 7-14 ms, output unique cadence fell to roughly 41-110 fps, and
immutable CFR debt grew monotonically to 499 ticks / 4.16 seconds. Every one of 2,432 repeated slots also reran the
camera/cursor composition and conversion. Packet coverage stayed exact at 12,719/12,719 and the audio endpoint was
within 1 us, so this was visual capacity degradation with intact CFR/A/V structure, not FG suspension, mux pressure,
or a timeline-recovery failure.

The backend-neutral overload pacer now learns fresh and cached-repeat service separately. It issues only one bounded
repeat-cost probe per four eligible slots while repeat service is unknown, retains inject candidates rather than
discarding them on a paced hold, and distributes fresh frames with fractional credit once repeats prove materially
cheaper. During an armed inject recovery episode, measured repeats below 90% of the output interval may repay one
extra immutable slot despite fresh-path encoder pressure; mux pressure always blocks this catch-up. A near-target
source-health floor disarms pacing when a cutscene or FG transition genuinely reduces source cadence.

Dynamic camera/cursor repeats also gain an encoder-local degradation mode. Two consecutive fresh submissions above
95% of the frame interval make held slots reuse the last fully composited converted frame; eight fresh submissions
below 75% restore per-repeat overlay recomposition. Thus only repeated slots can briefly hold camera/cursor pixels,
while every fresh game frame still updates them. Logs and final summaries expose probes, measured fresh/repeat
service, pacing episodes, frozen-overlay repeats, and recovery transitions. CFR PTS, source timestamps, audio samples,
and final duration contracts are unchanged. Focused policy/source tests pass; hardware validation is pending.

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
