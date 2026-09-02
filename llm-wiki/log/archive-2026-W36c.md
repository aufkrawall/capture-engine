# llm-wiki Log Archive - 2026-09-01

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
