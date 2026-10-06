<!--
SPDX-License-Identifier: MIT
Copyright (c) 2026 aufkrawall
-->

# DX12 queue dispatch ownership

Last cross-checked: 2026-10-06 (nine native registry cases, two real-hook queue cases,
three deliberate production mutations and the 21-scenario flow suite).

## Sources and contract

- [dx12_queue_dispatch.h](../hook/d3d12/dx12_queue_dispatch.h) exposes capture, resolution,
  binding facts, copied snapshots, generation and reset. Its private implementation is
  [dx12_queue_dispatch.cpp](../hook/d3d12/dx12_queue_dispatch.cpp).
- [execute_dispatch_registry.h](../hook/d3d12/execute_dispatch_registry.h) owns the production
  installation transaction; native tests invoke this same implementation with controlled patch callbacks.
- [vtable_hook.cpp](../hook/hooking/vtable_hook.cpp) publishes the predecessor before patching.
  GetOriginal recovers only the saved entry for the exact slot, detour and live allocation base.
- Install/forward/reset consumers use this boundary. Signal bindings remain separately owned.

An unknown queue resolves its own live ECL slot, never the first original captured from another queue.
Live fallbacks are not cached: another owner can change the slot without changing this registry.
Saved bindings use a complete thread-local identity/target pair checked against instance and generation;
the previous separately published shared cache atomics are gone. This cache is not a code-lifetime lease.

Cold installation serializes with a recursive mutex, reserves the map node before patching and
invalidates cache evidence before and after publication. A borrowed stack output permits same-thread
reentry to observe the predecessor as soon as the patch primitive writes it. Failed patching rolls
back the binding. A reentrant reset or replacement cannot resurrect retired evidence or use an erased
iterator. Other threads block on the cold operation; steady cache hits do not take this mutex.

Reset retires registry evidence; it does not detach physical hooks. A still-intercepted CE slot can
recover its exact predecessor from the patch primitive. Missing exact evidence produces a metered
diagnostic. A null queue identity is rejected without invoking any native method with that receiver.

## Regressions and decisive evidence

- [test_execute_dispatch_registry.cpp](../tests/test_execute_dispatch_registry.cpp) covers publication
  reentry, failed patches, follower/reclaim, nested captures, reset/replacement and concurrent cached
  identity reuse with barriers rather than sleeps.
- [test_flow_queue_dispatch.cpp](../tests/flow/test_flow_queue_dispatch.cpp) uses two real COM queue
  implementations with distinct ECL entries over the same native WARP queue/device. Production install,
  target resolution and transparent forwarding are exercised; assertions precede unsafe forwarding.
  Duplicate capture, alternating targets, reset recovery, balanced COM objects and physical coverage
  are checked. The pre-fix untracked queue resolved the native queue's entry rather than its own entry.
- [check_ecl_dispatch_mutations.py](../tools/refactor/check_ecl_dispatch_mutations.py) reintroduces
  cross-queue fallback, stale reset cache and resurrection after retirement. All three fail the expected
  assertions; the script restores exact source bytes and verifies the restored implementation.
- RTSS was actually loaded in the controlled process. Its generic three startup transport passes
  bypassed overlay admission: one physical output had zero ledger coverage. Present/Present1 now retain
  startup transport choice while proceeding through normal rendering/accounting. Both queue cases and
  all 21 flow scenarios pass, with the first native output covered and no debug-layer errors.

## Pending and limits

D1/D2 are partial. This slice does not own callback admission/drain, physical hook detachment,
provider module/code lifetime, native candidate image lifetime or Signal bindings. Generation
invalidation alone does not prove safe unload. Controlled foreign interposer installation/removal,
broader wrapper/native chains and device loss still need deterministic scenarios. Cold FG before
the first native output remains a separate open accounting finding in the NGX lifecycle page.
WARP and incidental RTSS presence do not establish all real games, SDKs, overlay orders or performance.
