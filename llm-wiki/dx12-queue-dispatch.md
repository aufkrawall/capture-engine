<!--
SPDX-License-Identifier: MIT
Copyright (c) 2026 aufkrawall
-->

# DX12 queue dispatch ownership

Last cross-checked: 2026-10-11 (native translator discovery, probe cleanup and single output ownership;
real DXUP video/screenshot/resize checks and 54 flow scenarios; closing evidence in log/recent.md).

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

### DXGI translators and discovery submissions (2026-10-11)

Native Present interception and factory discovery must target the same system DXGI implementation.
`InstallGlobalVTableHooks` in [dx12_hook_hook_install.cpp](../hook/d3d12/dx12_hook_hook_install.cpp)
uses the path-qualified `GetSystemDXGIModuleHandle` before resolving its discovery export. A
same-named proxy factory can have completely different vtables and create its DX12 swapchain
through a backend factory. Hooking that proxy alone loses the owning queue even though CE still
intercepts native Present. The system-module lookup caches successful resolution only: a proxy
can load before native DXGI, and an early absence must remain retryable. Existing foreign-entry
bypass, factory-slot ownership and below-chain interception rules still apply to native discovery.

`DetourExecuteCommandLists` in [dx12_hook_ecl.cpp](../hook/d3d12/dx12_hook_ecl.cpp) forwards submissions
inside `DX12_IsInternalDXGISwapchainProbe` through the queue's exact predecessor before application
device-loss, FG and queue-adoption policy. A D3D11 translator may submit DX12 initialization work
while CE creates its temporary D3D11 probe. Those submits must complete without publishing game
device/queue ownership or cadence. The existing nested, thread-local scope excludes only the
probing thread and now covers the temporary D3D11 objects' releases, where DXUP also submits work.
`DX12 ECL: internal discovery submit` uses `ChangeGate` to name forwarding and
the unchanged ownership contract; factory discovery logs its native module/vtable once per install.

Session `20261011_194431` (Heroes of the Storm with DXUP, CE 0.1.7102) adopted the probe queue
before game creation. No real swapchain create was intercepted. The first normal overlay draw
fell back to that queue and immediately reported `GetDeviceRemovedReason=0x887A002B`, followed
by `Present=0x887A0005`. The manual dump's matching CE PDB resolved the hook worker; the DXUP
`dxup-cs` worker was idle in its condition-variable wait. This is a post-loss stalled snapshot,
not an exception at CE's GPU submit. An isolated host using the installed DXUP DLLs reproduced
the same access-denied removal on the first overlay submit and was stopped at its 20-second bound.
Private DXUP sources, dumps, logs and probe binaries remain local, outside Git.

Regression anchors: `Dx12FactorySlotPolicyTest.FactoryDiscoveryTargetsNativeCreationBelowAnApiTranslator`,
`NativeModuleLookupDoesNotPermanentlyCacheAnEarlyProxyOnlyLoad`, and
`FlowQueueDispatch.TranslatorDiscoverySubmitsWithoutClaimingTheGameQueue` (exact forwarding under
nested scopes, unchanged cold ownership, application adoption after scope exit, then overlay coverage).
The real-DXUP probe also checks device health and per-Present overlay coverage; full live-game
capture/FG/foreign-overlay validation remains a separate check.

### Single native owner below API translation

[translated_swapchain_owner.cpp](../hook/present/translated_swapchain_owner.cpp) records native
swapchain creation inside thread-local frontend create scopes. D3D10/10.1 and D3D11 combined
creation exports, plus DXGI factory wrapper creates with non-DX12 inputs, publish ownership only
after observing one covered native output for the same HWND. Repeated views of the same create
are deduplicated; different outputs, windows, threads, uncovered Presents and native command-queue
inputs do not establish delegation. The mechanism does not identify translators by filename or
assume their frontend API. Older frontends reaching these DXGI factories use the same evidence.

The ownership marker resides in per-object private data, contains the frontend identity, and
retains no native COM reference. CE wrappers are unwrapped only when their private QI returns
the same object, since a foreign proxy can forward that QI to an underlying CE wrapper. This identity
check matters because an API proxy can forward private data to its native swapchain: the output
must keep processing its frames while the frontend only forwards Present/Present1. DX11 frame
processing also respects the marker so residual entry routes cannot reinitialize its own overlay
or capture transport. Native output retains its original owning queue and ordinary overlay,
capture, screenshot and post-processing order. No compatibility readback/copy or wait is added.
Native DX12/FG factory inputs never acquire this frontend marker.

`TranslatedSwapchainOwner` tests evidence admission, nested scope restoration, ambiguity and
cross-thread isolation. `FlowTranslationOwner` exercises production publication and real
Present/Present1 hooks with a proxy that forwards private data to a native WARP swapchain; the
native output must remain covered once per Present. Live DXUP validation and gate evidence are
recorded in `log/recent.md`. Older DXUP frontend binaries are not present in the supplied game
directory, so their live rendering/capture remains unverified.

### Existing dispatch registry regressions

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
These executable-backed fixtures do not establish unload safety or replace the full Present matrix.

## Present coexistence and status-query boundary

[present_interposer.h](../tests/flow/present_interposer.h) replaces the real underlying DXGI Present
slot with independent foreign code and forwards through its captured entry. The original three cases in
[test_flow_present_interposer.cpp](../tests/flow/test_flow_present_interposer.cpp) cover repair while
the foreign layer remains, nested DXGI_PRESENT_TEST probes, admitted probe completion after physical
removal, and native/wrapper Present/Present1 query behavior. Repair logs confirm the actual foreign
native slot was observed. Retained COM references and scoped release/join bound fixture lifetime.

The baseline counted 16 ledger outputs for 10 real presents; a lone admitted test probe added another
false output. [Microsoft's DXGI contract](https://learn.microsoft.com/windows/win32/direct3ddxgi/dxgi-present)
defines these calls as status queries without an output. Native and wrapper entry boundaries now
forward before entry frame processing, metrics, output accounting or FG observation, preserving
the original arguments and HRESULT. Change-gated probe diagnostics avoid idle-loop noise.
[check_present_probe_mutations.py](../tools/refactor/check_present_probe_mutations.py) removes each
of the four guards; every mutant fails the actual query case, then exact restoration passes.

[call_barrier.h](../tests/flow/call_barrier.h) is shared by Signal and Present fixtures. Completion
wakes admission waiters even if the callback was never reached, and release/join remains scoped.
Remaining inline/DLL orders, actual nested physical outputs, CE removal while a Present callback is
active, allocated thunks and provider unload remain open; physical vtable order is covered below.

## Present vtable forwarding ownership

[present_vtable_dispatch.h](../hook/present/present_vtable_dispatch.h) exposes physical installation
and typed forwarding operations. Its implementation stays with claim/repair/detach in
[dxgi_shared_hooks_present_vtable.cpp](../hook/present/dxgi_shared_hooks_present_vtable.cpp).
The private Present/Present1 adapters snapshot their independently published exact predecessors
and original caller addresses. Legacy shared-original publication stays intact for SDK route
observation; later inline publication cannot overwrite the private vtable bindings. Installation
publishes before CAS, restores evidence on a lost claim and keeps the established vtable binding
when a distinct implementation is observed. Repair/detach use those same private predecessors.

The failing baseline delivered zero of three presents to a foreign layer installed before CE's
claim. CallOriginalPresent preferred its existing inline trampoline over the captured vtable next
link. Scoped typed forwarding now preserves that link, its receiver/arguments/HRESULT and Present1
parameters. While the predecessor executes, reentrant inline/body calls use their own transports;
reusing the vtable link there would loop. Scopes restore their prior frame without hot-path locks,
allocation or COM reference churn. They snapshot dispatch identity, not provider-code lifetime.

Two added real-hook cases install foreign Present/Present1 before CE's vtable claim and check
three calls before/after handoff, nested status probes, subsequent native drawing and exact coverage.
The test seam invokes production runtime handoff, preserving inline hooks throughout. This covers
physical vtable order; it does not simulate foreign inline installation before CE DLL loading.
[test_present_vtable_dispatch.cpp](../tests/test_present_vtable_dispatch.cpp) verifies method/receiver
isolation, reentry refusal, exact arguments/results and scope closure. Existing read-only repair/
detach regressions now install through the production owner instead of assigning shared originals.
[check_present_chain_mutations.py](../tools/refactor/check_present_chain_mutations.py) requires actual
assertion failures for dropped predecessors, reused active links and borrowed receiver identity.
All six forwarding/scope/caller/identity mutations fail. After source-protection anchors migrate,
restored production passes 539 focused native cases and all 32 FG flows. Direct idle-scope
receiver and nested-scope tests close a coverage gap exposed by the first identity mutation run.
All DLSS/NGX and Present flows pass with original caller provenance preserved. A new adapter had
hidden SDK caller identity and caused uncovered startup outputs; direct origin tests protect the
scoped receiver/method/view contract. Self-target and temporary-handoff consumers recognize both
CE views and restore the exact entry they displaced. Shared route aliases still require D3 work.

Open findings: native forwarders still call flip-queue pacing for status queries when backbuffer
configuration and CE waitable provenance enable it. Current query cases use default configuration
and do not prove that path harmless; add a focused waitable regression and fix. Full callback drain,
CE removal during active Present, actual nested outputs, other inline/DLL orders and provider unload
remain open. The tests retain executable-backed foreign code, not dynamically unloaded providers.

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
