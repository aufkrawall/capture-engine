# D3D11 Context Dispatch (CE-owned function tables)

Last cross-checked: 2026-10-17 (d3d11.dll 10.0.26100.9711 x64 with Microsoft symbols, WARP flow tests)

Primary sources:
- `hook/d3d11/d3d11_context_dispatch.{h,cpp}` (Adopt, tables, leases, stats)
- `hook/d3d11/d3d11_dispatch_forwarders.h` (forwarder encodings, both architectures)
- `hook/d3d11/dx11_hook_install.cpp` (`InstallContextVTableHooks11`: adopt first, slot hooks otherwise)
- `hook/d3d11/dx11_hook_types.h` (`ResolveContextOriginal11`), `dx11_hook_helpers.cpp` (identity QI hook)
- `hook/hooking/inline_hook_trampoline.cpp` (`AllocateWritableCodePage`, `SealCodePage`: W^X + CFG)
- Tests: `tests/flow/test_flow_d3d11_raw_context_hooks.cpp` (WARP), `tests/test_d3d11_dispatch_forwarders.cpp`

## Summary

d3d11.dll keeps each context's function table **inside the context object** and rewrites it while it
runs. A hook written into that table (what CE's raw context hooks did until 2026-10-17) lasts only
until the runtime's next rewrite of the slot. CE now leaves the runtime's table alone and points the
context's vtable pointer at a CE table: hooked slots hold CE's detours, every other slot a forwarder
that jumps through whatever the runtime's table holds at that moment. The runtime keeps toggling its
own table; CE reads it at every call and never writes it.

Static (image-backed) tables, such as translator frontends or the D3D11 device's own vtable, are
not rewritten by anyone and keep CE's slot hooks.

## d3d11.dll evidence (decisive, logs are not kept)

Static, `cdb -z C:\Windows\System32\d3d11.dll -y srv*` plus `llvm-objdump` over the whole image:
- `CContext::CContext` sets the interface's vtable pointer to `this+0xE0`, right after itself
  (`[ctx] == ctx+8`); the only other self-pointing vptr store is `CDevice::CDevice` (its D3D10
  interface). Nothing reassigns the vptr later.
- `CContext::g_DefaultID3D11DeviceContextVtbl`: static default tables, rows of 0x4B8 bytes (151
  entries) indexed by a variant byte of the context (`[ctx+0xE48]`, e.g. `<1>` single-threaded,
  `<0>` with `CondObjectLock`).
- Writers of the embedded table: `SetAmortizedAcquireResourceOperations11` (Draw, DrawIndexed,
  DrawInstanced, DrawIndexedInstanced, DrawAuto, both indirect draws, Dispatch, DispatchIndirect ->
  `*_Amortized<1|0>`), `UnsetAmortizedAcquireResourceOperations11` / `UnsetAmortizedRenderOperations11`
  / `UnsetAllAmortized11` (copy the static defaults back), `SetAllAmortized11` (about 100 slots: every
  stage's SetShader/SetShaderResources/SetSamplers/SetConstantBuffers, IA/OM/RS setters, ClearState,
  ExecuteCommandList, FinishCommandList, the getters incl. PSGetSamplers, ...).
- `CContext::SetMultithreadProtected` calls `SetAllAmortized11` (and `CDevice::SetAllAmortized10`).
- The `*_Amortized` functions do the pending work through `CDevCtxInterface` /
  `AcquireDevCtxIfaceNoSync` and then call the **static default table's** slot directly, never the
  embedded table: so a call that entered through CE's table is not re-entered.

Dynamic (WARP flow probe, hooked slots before the fix; `rt+` = d3d11.dll RVA):
| After | Slots no longer CE's |
| --- | --- |
| install, first draw | none |
| `Flush` | 12, 13, 20, 21, 38, 39, 40 (and 41) -> `*_Amortized<1>` (13 = `rt+153610`) |
| next draw | same slots -> static defaults (13 = `Draw_<1>` `rt+153540`) |
| `SetMultithreadProtected(TRUE/FALSE)` | additionally 8, 9, 10, 58, 75, 110, 114 (variant `<0>` tables while protected) |
| `SwapDeviceContextState`, `ExecuteCommandList` | `SetAllAmortized11` / acquire set again |
| deferred context | nothing toggles (its draws stayed CE's) |
Slots 0 (QueryInterface), 111 (Flush) and 131 (SwapDeviceContextState) never toggled. The device's
vtable is `MEM_IMAGE` (static); the context's table is `MEM_PRIVATE`. `QueryInterface` for
`ID3D11DeviceContext1` returns the same pointer.

Consequence before the fix: from the first flush (every frame) CE's raw draw hooks were gone, so
forced AF on raw contexts was decided only at sampler binds; with multithread protection or a state
swap the bind, read-back and reset hooks were gone too. A CE re-patch over an `*_Amortized` entry
would have been worse: the captured "original" would skip or repeat the runtime's pending work.

## Design and invariants

- `Adopt(context, detours)`: declines static tables (`VirtualQuery` type `MEM_IMAGE`), declines a
  table that already holds CE's slot detours (forwarding to them would recurse), takes a table,
  fills it, attaches a `TableLease` as the context's private data
  (`{7F0C2A1E-3D11-4CE0-9A57-D15A7C4B0E11}`), then publishes with a CAS on the vptr. No CE lock is
  held across a call into the runtime (a multithread-protected device may hold its lock while
  another thread waits for CE's); concurrent adopts of one context are serialized by an
  "adopting" list.
- Forwarders are per table index, not per context: `mov rax,[abs64 &table.runtime]; jmp [rax+slot*8]`
  (x86: `mov eax,[abs32]; jmp [eax+slot*4]`), 16 bytes each, every one a registered CFG target. They
  never read the object's vptr, so a tool that copies CE's table into its own and repoints the
  context cannot make them loop (flow test `ForeignCopyOfTheTableStillDispatches`).
- Code: chunks of 16 tables (64 KB, 4096 forwarders) built on first need, sealed RX with
  `PAGE_TARGETS_NO_UPDATE` and only the forwarder entries registered valid; never freed.
- Tables: 128 in `.bss` (`g_tables`, 256 entries each: ID3D11DeviceContext4 has 149 methods). The
  runtime's table sits at `entries[-1]`, so `RuntimeTable(ctx)` is a range check and one load.
- Lifetime: the runtime releases the lease when it destroys the context; the table then goes to a
  FIFO and keeps its contents until reused (a dying context's late calls still find valid code).
- Every CE hook that keys by vtable maps an adopted context to the runtime's table
  (`HookTable`): `ResolveContextOriginal11` and the identity QueryInterface hook, which stays in the
  runtime's slot 0. A context whose vptr someone replaced after adoption is still found through the
  owner list (`RuntimeTableOfAdopted`, locked, miss path only).
- Every call through an adopted context costs one forwarder (load + indirect jump) on top of the
  runtime; hooked slots cost what the detours did before.

## Diagnostics

- `[D3D11 DISPATCH] adopted ctx=... runtimeTable=... table=N detours=15 live=... source=...`
  (first 16, then every 256th), `retired ...` likewise, `forwarder chunk=...` once per 16 tables.
- `[D3D11 DISPATCH] ctx=... kept, slot hooks instead: <reason>` (important; first 8 per class):
  `static image memory`, `holds CE slot hooks`, `no dispatch table`, `private data refused`,
  `vtable pointer changed meanwhile`.
- `DX11: Runtime AF hook ensure from Present ... dispatch=ce-table|slot-hooks`.
- Flow: `CEFlow_GetD3D11DispatchStats` (adopted, retired, staticTables, unavailable, live).

## Open questions / stale-risk

- x86 d3d11.dll (SysWOW64) was not disassembled; the x86 forwarder bytes are checked against
  llvm-mc only (no x86 test runner). Same design expected (same source).
- Calls through function pointers a game captured before CE adopted its context (late injection)
  bypass CE entirely; nothing detects that. Forced AF defers sampler binds on adopted contexts
  (`RawAFAccess::DefersSamplerBinds`, dx11-forced-af.md), so such a draw would miss a pending bind;
  slot-hooked contexts stay eager.
- Deferred contexts created with `CreateDeferredContext1/2/3` are not hooked at all (only
  `ID3D11Device::CreateDeferredContext`, slot 27).
- The D3D10 device (`CDevice` embedded table, `SetAllAmortized10`) may rewrite CE's D3D10
  `CreateSamplerState` slot hook the same way; not checked.
- Hardware validation pending: a late-injected native D3D11 game with forced AF (log shows
  `[D3D11 DISPATCH] adopted`).
