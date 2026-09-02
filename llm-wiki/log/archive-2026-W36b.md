# llm-wiki Log Archive - 2026-08-31

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
