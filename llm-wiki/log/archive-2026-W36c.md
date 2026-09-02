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
