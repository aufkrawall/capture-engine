<!--
SPDX-License-Identifier: MIT
Copyright (c) 2026 aufkrawall
-->

# DX12 queue dispatch ownership

Last cross-checked: 2026-10-07 (eleven native registry cases, four queue and three foreign Signal cases,
device bootstrap/reset coverage; six dispatch and two device mutations; closing gate in log/recent.md).

## Sources and contract

- [dx12_queue_dispatch.h](../hook/d3d12/dx12_queue_dispatch.h) exposes capture, resolution,
  binding facts, copied snapshots, generation and reset. Its private implementation is
  [dx12_queue_dispatch.cpp](../hook/d3d12/dx12_queue_dispatch.cpp).
- [execute_dispatch_registry.h](../hook/d3d12/execute_dispatch_registry.h) owns the production
  installation transaction; native tests invoke this same implementation with controlled patch callbacks.
- [vtable_hook.cpp](../hook/hooking/vtable_hook.cpp) publishes the predecessor before patching.
  GetOriginal recovers only the saved entry for the exact slot, detour and live allocation base.
- Install/forward/reset consumers use this boundary. Signal slot 14 uses a separate private registry
  instantiation of the same installation transaction; its map, lock and first-global aliases are removed.
  ResolveSignal retains the receiver's own live slot or exact saved predecessor and returns no target
  when receiver identity is missing. The trace detour returns E_FAIL and logs the unresolved boundary.

An unknown queue resolves its own live ECL slot, never the first original captured from another queue.
Live fallbacks are not cached: another owner can change the slot without changing this registry.
Saved bindings use a complete thread-local identity/target pair checked against instance and generation;
the previous separately published shared cache atomics are gone. This cache is not a code-lifetime lease.

Cold installation serializes with a recursive mutex, reserves the map node before patching and
invalidates cache evidence before and after publication. A borrowed stack output permits same-thread
reentry to observe the predecessor as soon as the patch primitive writes it. Failed patching rolls
back the binding. A reentrant reset or replacement cannot resurrect retired evidence or use an erased
iterator. Other threads block on the cold operation; steady cache hits do not take this mutex.

Reset retires registry evidence; it does not detach physical hooks. ResolveInterception owns lookup
priority: the private saved/cache pair, exact retained patch evidence, then an untracked live slot.
A foreign live entry can still forward to CE; it cannot replace CE's retained predecessor after reset.
Established bindings do not call cold recovery/live readers. Missing evidence produces a metered
diagnostic. A null queue identity is rejected without invoking a native method with that receiver.

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
  cross-queue fallback, stale reset cache, resurrection after retirement and Signal's missing-receiver
  fallback, reversed recovery priority and discarded follower evidence. All six fail the expected
  assertions; the script restores exact source bytes and verifies
  the restored implementation.
- [test_flow_queue_signal.cpp](../tests/flow/test_flow_queue_signal.cpp) installs real Signal hooks on
  two distinct COM implementations and invokes their physical slots. It checks exact originals,
  duplicate install, S_OK/S_FALSE preservation, fence-value forwarding, reset recovery, native rendering
  afterward, object balance and coverage/debug-layer errors. A pre-fix missing receiver returned a saved
  non-null predecessor; the assertion stopped before the unsafe call. Test-only exports call the same
  production installer/resolver/detour; they do not model the forwarding decisions.
- RTSS was actually loaded in the controlled process. Its generic three startup transport passes
  bypassed overlay admission: one physical output had zero ledger coverage. Present/Present1 now retain
  startup transport choice while proceeding through normal rendering/accounting. Both queue cases and
  all 21 flow scenarios pass, with the first native output covered and no debug-layer errors.

## Pending and limits

D1/D2 are partial. This slice does not own callback admission/drain, physical hook detachment,
provider module/code lifetime or native candidate image lifetime. Generation
invalidation alone does not prove safe unload. Controlled foreign interposer installation/removal,
broader wrapper/native chains and device loss still need deterministic scenarios. Cold FG before
the first native output remains a separate open accounting finding in the NGX lifecycle page.
WARP and incidental RTSS presence do not establish all real games, SDKs, overlay orders or performance.

## Foreign Signal chains and retirement evidence

[signal_interposer.h](../tests/flow/signal_interposer.h) supplies independent foreign code installed
through real physical slot replacement; it forwards through the entry it actually captured. It does
not select CE targets or model CE decisions. [test_flow_signal_interposer.cpp](../tests/flow/test_flow_signal_interposer.cpp)
covers both installation orders, preservation when CE removal encounters a foreign follower, reset
recovery and a barrier-controlled admitted callback across removal. Callback release/join is scoped,
including fatal-assertion exits; no sleeps or timing assumptions. COM/output/debug-layer checks remain.

The pre-fix follower/reset case raised 0xC00000FD. Digest-first investigation and an x64 dump with
Microsoft symbols and matching CE PDBs showed repeated SignalInterposer::Detour / CE Signal frames.
Registry reset left the physical foreign -> CE chain intact, but the resolver selected the foreign
live entry before consulting retained exact-slot evidence. Shared resolution priority fixes the
root cause for all three consumers; the regression asserts target identity before unsafe invocation.

Remove returning Success can mean a foreign follower was preserved with CE still in its chain.
Physical restoration also does not prove callback drain: the admitted-call case finishes afterward.
CE's own image is process-pinned by main_dllmain.cpp; UnloadThread is a pass-through shutdown, not
image unloading. Provider images/allocated thunks and their data require separate scoped retirement.
module_pin.h explicitly reserves permanent pins for named system runtimes and permits real SDK unload.
These executable-backed fixtures do not establish unload safety or replace the pending Present tests.

## Device trace ownership and reproduced crash

Before the fix, enabling CE_DX12_TRACE=1 before loading the flow hook crashed during bootstrap, before queue
Signal assertions. Digest-first investigation and an x64 CDB dump with Microsoft symbols and matching
CE PDBs show D3D12Core!CDevice::TranslateNodeMask -> CreateCommandQueue1 -> CreateCommandQueue ->
CE DetourTraceCreateCommandQueue -> HookSwapchainVTableViaTempSwapchain. The faulting read addresses
baadf00d3f800000. Device trace installation captured D3D12Core's first global original, then intercepted
the debug-layer device without replacing it; the native method received an incompatible device object.
CreateDescriptorHeap and CreateCommittedResource used the same first-global pattern.
Dumps/symbol copies remain ignored local evidence, never commits.

Fixed 2026-10-07: [dx12_device_trace.h](../hook/d3d12/dx12_device_trace.h) exposes only device capture,
three typed creation commands and reset. The [private implementation](../hook/d3d12/dx12_device_trace.cpp)
owns each method's exact-vtable predecessor through the shared installation transaction. All tracing
detours are private to that module; globals/aliases/prototypes were removed. Callers retain the receiver
and invoke commands; they cannot choose an original or reconstruct fallback policy. Reset invalidates
cached evidence; physical slots recover only their allocation-checked saved predecessor.

[test_flow_device_trace.cpp](../tests/flow/test_flow_device_trace.cpp) reproduced the pre-fix access
violation before bootstrap completed. It now verifies native/debug-device creation, actual queue/heap/
resource descriptors, all three methods after reset, physical-output coverage and debug-layer errors.
The Signal flow case now enables the production trace switch and uses the normal queue installer.
Logs show distinct native and SDK-layer vtables with separate original targets for all three methods.
[check_device_trace_mutations.py](../tools/refactor/check_device_trace_mutations.py) detects the
historical first-device borrowing as the expected access violation and missing reset recovery as an
assertion failure; compiler/loader errors cannot satisfy either check. Exact source restoration passes.
This boundary does not own provider code/module lifetime or callback drain; D2/D10 retirement and
foreign-interposer work remain required.
