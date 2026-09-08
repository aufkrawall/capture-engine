# llm-wiki Log

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
