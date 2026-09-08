# llm-wiki Log

### 2026-09-08 - The degraded state is a flat post-Present hold, and an ETW suppression bit to test it

Anchoring the flip against GPU execution rather than Present localises the defect. In a 117 fps
segment the generated frame reaches the screen 2324 us after its GPU work starts with 94 us of
spread over 1106 frames, and the two frame types differ correctly: the application frame, finished
about 7 ms before its Present, flips in 753 us, while the generated frame, still finishing, takes
2271 us. That is flip-when-ready. In a 109 fps segment *both* types flip at a uniform ~3050 us after
Present, including the frame that was finished 7 ms earlier - a flat ~2.4 ms hold that completion
cannot explain and that costs the 10 fps. VRAM is ruled out (10.18 vs 10.15 GB, and the slow run in
`new1` used less), as is per-frame GPU work (energy per frame within 1.2%).

`CE_FG_COST_PROBE=0x40000` (`kDisplayTimingEtwOff`) now suppresses CE's screen-change ETW session in
the sensor process, which inherits the same variable. It is the one thing CE runs on the flip path
that a Present-hooking overlay like RTSS - which never reaches the degraded state on this machine -
has no equivalent of. The bit removes every present-to-display measurement with it, so such a run is
judged on output frame rate alone. See `display-change-timing.md`.
Validation: `--verify` passed for 0.1.6513.

### 2026-09-08 - GPU bracket: CE's overlay costs 7 us, and the two states are one VRR lock

First measurement of CE's own GPU time inside the frame-generation runtime's list. Paired 117/109
fps steady segments: CE's overlay commands take **7-8 us** and, for application frames, begin
executing at an unchanged offset after the callback (1554 -> 1614 us) while those frames reach the
screen 2382 us later. Every CE CPU span is equal or lower in the degraded run. The callback path is
excluded; the `fg_cost_probe.h` ~1.8 ms GPU-busy figure cannot be the overlay draw.

The two states are two lock modes against the panel's 6947 us minimum refresh, not two amounts of
work: 117 fps runs presents at 8502 us with flips alternating 6975/10079 us and 28% of gaps against
the floor (`downstream-jitter`, 312-405 permille late), 109 fps runs 9208 us presents with even
9416/9518 us flips and 3.4% near the floor (`healthy`, 66-112 permille). The faster mode is the
jittery one, so the reported rate loss and the reported microstutter are the same bistability seen
from opposite sides. What tips the lock is still open. See `display-change-timing.md`.

### 2026-09-08 - Display-anchored pacing decomposition, steady reference and an opt-in GPU bracket

A five-launch `CE_FG_COST_PROBE=0x4` A/B refutes CE's overlay GPU work as the trigger: with
`cbDraws app=0 gen=0` - and the breadcrumb writes gated on the same condition, so CE appended zero
GPU commands to AMD's lists - two of five runs still latched into the degraded state with
numerically identical signatures. With the profile's own overrides removed the state persists and
shows as throughput instead of jitter (109 vs 117 output fps); the whole difference is the game
blocked longer inside FFX's Present while its own CPU work *drops*, at 123 W against 161-167 W.
That is a stall, and no CE CPU span differs between the two.

Three instruments added, all diagnostic. `Analyze` decomposes each displayed transition against the
callback that produced it (`pacer_wait`, `present_to_display`, `callback_to_display`, per frame
type), which is the discriminator the last several sessions had to reconstruct by hand.
`EpisodeDetector` now returns an `Episode` and saves one signature-blind `steady-reference` capture
per steady segment, because the degraded start can classify as healthy and never trigger a suspect
save. `present_callback_association.{h,cpp}` carries the same decomposition into the live
`[FSRPacingHealth]` line, which also gained a per-window GPU usage/power reading.
`overlay_gpu_timing.{h,cpp}` brackets CE's own commands in the runtime's list with GPU timestamps
behind `CE_FG_GPU_TIMING=1`, to settle whether the missing 2.5 ms sits upstream or downstream of
them. See `display-change-timing.md`. None of this fixes the defect or proves a cause.
Validation: `--verify` passed for 0.1.6511 - native tests, Python self-tests, x64 ASan/UBSan,
zero-warning clang-tidy and the file-size baseline; clang-format notices remain advisory.

### 2026-09-08 - Self-describing pacing saves and equal-timestamp ordering

The latest bad run again had prompt proxy prework and lower Present forwarding. No root-cause
GPU/pacing-policy change is established. Added background-only summary analysis of existing copied
trace events: validated Present pairs, explicit missing/invalid coverage, sample counts, cost
quantiles and matched latest-marker age bounds. These remain CPU spans/completion bounds, never
GPU execution timings or causal diagnoses. Saves preserve same-timestamp producer ordering so
short calls cannot be mispaired by unstable sorting. No extra runtime observations or GPU work.
Regression coverage exercises nesting, thread/epoch identity, malformed and truncated pairs,
marker-generation matching, empty input, equal timestamps and quantiles.
Validation: focused tests and full verification passed for 0.1.6507, including native/Python tests,
x64 ASan/UBSan and zero-warning clang-tidy; formatting notices remain advisory.

### 2026-09-08 - Present scheduling boundaries and race-free heartbeat

Good/bad comparison found identical callback and submission p95 costs, with complete latest GPU
markers in both stable windows, despite much higher display jitter in the bad run. Added paired
game-facing FFX proxy, DXGI detour and forwarding-helper spans to the bounded trace. Proxy input
and forwarded VSync intent plus returned HRESULT are explicit; nesting, unknown results and the
helper's mixed CE/foreign/driver time must not be interpreted as GPU durations.
Repaired plain shared Present heartbeat counters/timestamps: one-attempt monotonic atomic
publication drops contended diagnostics rather than blocking. Deterministic nested/early-return,
disabled-scope and concurrent-heartbeat regression coverage accompanies the change. No pacing
policy or GPU submission changes; the FSR pacing root cause remains open.
Validation: focused tests and full verification passed for 0.1.6506, including native/Python tests,
x64 ASan/UBSan and zero-warning clang-tidy. Fixed test-backend exception safety before resuming.

### 2026-09-08 - Preserve pacing evidence and tighten GPU progress bounds

The marked bad run had prompt callback CPU work and same-list submission, but completion was only
observed on six-slot reuse. Submission traffic consumed about 81% of trace events. Split the existing
event budget into independent core/submission rings, merging only for saves; retain recent queue
detail without crowding out the longer callback/display history. Sample the latest committed mapped
marker at inline acquisition while tracing, without new GPU commands, waits or reuse-policy changes.
Version-2 metadata distinguishes the two history horizons and marker-observation meanings.
See `display-change-timing.md`; neither this evidence nor this diagnostic change proves a pacing cure.
Validation: focused tests and full verification passed for 0.1.6505, including native tests,
Python self-tests, x64 ASan/UBSan and zero-warning clang-tidy; formatting notices were advisory.

### 2026-09-08 - Bounded FSR pacing episode trace

The latest bad pacing survived stable queue ownership and successful FSR creation without access-denied
recovery. Added an in-memory event ring and existing-background-service automatic/manual saving rather
than another steady-state summary. See `display-change-timing.md`, bounded suspect-episode trace.
Audit: callback GPU work is carried on AMD's command list; its upload completion already has marker
and optional fence observations. Reuse those reads; do not introduce timing queries or extra waits.
Host PresentStart/display pairs support same-source cadence comparison, not exact FSR-ID attribution.
The trace does not fix the pacing defect or prove zero measurement interference.

Validation: focused tests and full verification passed for 0.1.6504, including x64 ASan/UBSan,
native tests and zero-warning clang-tidy. The initial Vulkan-layer link failure was corrected by
scoping this DX12-only trace API out of that separate binary; verification resumed successfully.

### 2026-09-07 - Startup access-denied crash and nested recovery correction

The supplied startup dumps establish failed FFX replacement creation (`E_ACCESSDENIED`) followed
by a game-side null read at executable offset 0x2AF04B7. The earlier diagnostic dump captures
INLINE -> Steam -> Deep -> live-entry retry -> INLINE, proving the deep-only recursion flag
did not prevent inline recovery. The same crash offset/family predates queue stabilization.
This does not establish a common cause with random steady-state display jitter.

`swapchain_create_recovery.h` gives the outermost same-HWND create a thread-local recovery owner
across both hooks. Different HWNDs and threads remain independent. Recovery retries once after
CE-owned state release, without the old nested 5/10-attempt sleep loops. Descriptor overrides
are preserved on the inline full-cleanup retry. No foreign reference is forcibly released.
Owner summaries include the HRESULT and nested-call count; FFX context results include duration
and queue/output-slot identities with bounded sampling. Inline ownership retains pre-cleanup
pin-ledger diagnostics, never COM probes of raw pointers.

Open: no retained startup activation reference was reported in this failure, and cleanup did
not free the HWND association. Its remaining owner is unproven; fixing recovery amplification
must not be represented as a proven cure for the initiating create failure or frame pacing.

Validation: focused recovery/source-policy tests and full `--verify` passed for 0.1.6503,
including native tests, Python self-tests, zero-warning clang-tidy and sanitizer regression coverage.

### 2026-09-07 - Separate execution discovery from explicit DX12 queue binding

Pre-FSR Talos logs show two threads repeatedly replacing the global queue with different DIRECT
queues on the same device. ECL activity did not prove ownership, yet the last submitter selected
the retained queue and triggered repeated COM/device publication work. Discovery now retains an
established same-device queue. Explicit wrapper bindings, initial discovery and proven new-device
migration retain their existing authority. Exact swapchain/FSR/Streamline queues remain separate.

`dx12_hook_queue_adoption.cpp` resolves device identity before publication, retains new references
before replacing the old pair, and releases old references outside the queue mutex. A failed
device lookup preserves existing state. Retention/adoption logs expose the reason, identities and count;
after the initial samples, power-of-two sampling bounds noise even under persistent queue churn.
Regression tests cover interleaved auxiliary submissions, initial/new-device discovery, explicit
same-device binding and unavailable identity. Cost probes retain independent adoption/publication gates.

This repairs a concrete ownership defect. It is not yet proof of the cause of Talos's random
downstream jitter or of the historical uncapped test-app slowdown. Hardware validation is pending.
Earlier pacing evidence, including the hidden-overlay comparison, is in
[archive-2026-W37a.md](archive-2026-W37a.md).

Validation: focused policy/probe tests and full `--verify` passed for 0.1.6502 (native tests,
Python self-tests, zero-warning clang-tidy, ASan/UBSan). No Talos runtime cure is claimed yet.
