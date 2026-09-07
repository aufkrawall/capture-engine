# llm-wiki Log

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
