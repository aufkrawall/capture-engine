# llm-wiki Log

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
