# llm-wiki Log Archive: 2026-W41c

### 2026-10-08 - Pre-library-plan execution evidence snapshot

Preserved from architecture-debt-plan.md during M0 adoption. This is historical evidence,
not the library milestone status; current status lives in [library/README.md](../library/README.md).

## Execution status (2026-10-08)

- D8/D13 runtime package path slice verified: executable/module defaults and explicit INI paths
  have one owner; helpers accept a copied runtime executable and WinMain honors --config. Active-code-
  page paths no longer pass through UTF-8 decoding. Eleven path/option cases plus an actual renamed
  helper in a different directory protect launch/configuration handoff; delegated game options are
  excluded. A fifth registered fuzz harness has six safe UTF-16 argument seeds. Focused IPC/config/
  owner tests and five product TUs pass. All five fuzz targets pass a bounded 10-second-per-target
  run (runtime options: 579,654 units). Strict-clean 20261007_205639_build_7024 compiled all products
  and passed native/Python/ASan/all 32 FG checks; an unnecessary string copy then blocked lint.
  After fixing it, resumed verify 20261008_081425_build_7024 passes all gates and produces the
  38,814,820-byte setup PE. Accepted warnings stay 706 across 1011 TUs; four formatting advisories
  remain. Full library bootstrap, resource resolution, public runtime descriptors and independent
  client delivery stay open; hardware/game and full codec/multitrack A/V validation are not claimed.
- Requested recording-recovery branch integration verified: 21956184/087452a3 add a versioned
  48-byte mux-flow snapshot, byte-capacity-aware CFR fresh/repeat admission and overflow timeline
  accounting. Source merges cleanly; changelog/journal conflicts preserve both ownership and media work.
  Focused budget/audio/loader/configuration tests pass. Strict-clean 20261007_194219_build_7023
  passes products/native/Python/ASan/all 32 FG checks; test rounding fixes close resumed verification
  20261007_200539_build_7023 and the 38,787,412-byte setup. Direct built-DLL checks prove 48-byte/8-aligned
  snapshot storage, invalid-argument rejection and unavailable counters without an engine. Baseline
  stays 706 across 1009 TUs; 28 formatting advisories remain. The closed-loop
  incident model does not prove arbitrary storage stalls or uninterrupted audio after data already lost;
  real overloaded-output and codec/multitrack A/V validation remain D5/D6 evidence requirements.
- D8/D13 configuration slice verified: one settings scope owns the snapshot, startup file identity,
  debounce/coherent replacement and deadlines; the mutable main_g_Config is removed. Frontend effects
  consume published changes. Eight transaction cases and two actual headless/native INI scope cases
  pass, with existing reload/sensor/hotkey checks (38 focused tests). Six mutations detect missed startup
  edits, unvalidated publication, premature commit, file replacement, recursive reload and ignored timing.
  Product syntax covers eight TUs. Strict-clean 20261007_190950_build_7022 compiled all products and
  passed native/Python/ASan/all 32 FG checks; explicit test optional guards then close resumed verify
  20261007_192804_build_7022 and package (38,775,318-byte PE). Accepted warnings stay 706; full scope is
  1007 TUs. Eight formatting advisories remain. Initial INI/default fallback stays compatible; transactional startup
  validation, complete resource resolution and programmatic settings remain D8/D13 work before a public library API.
- D8/D13 child ownership slice verified: one headless-constructible host child owner replaces six
  writable process/client globals. The production lifecycle retains old media finalizers across
  immediate restart, cancels reentrant readiness by generation and owns shutdown collection.
  Ten lifecycle cases, three native empty-scope cases and focused recording/API/IPC checks pass.
  Capability/bootstrap/path inputs are in [library-delivery.md](library-delivery.md). All five mutations detect the expected defects;
  Strict-clean product compilation and native/Python/ASan/32 FG checks pass at 20261007_171441_build_7021.
  That run stopped at test-vptr analyzer false positives; after comment-only annotations, final resumed
  verification 20261007_180216_build_7021 passes all gates/package. Accepted warnings tighten 712->706.
  Installer 0.1.7021 is 38,760,346 bytes. Full engine initialization and library delivery remain open.
  Next resume: finish D8 bootstrap/configuration and D13 lifecycle/API/path contracts before publishing
  a runtime library. Auxiliary shutdown-event failures/setup versus sensor recovery and D5 old/new
  media observation attribution need focused traces. Independent packaging, external-client proof and
  frontend conversion are still required; full D0-D13 phase exits remain as specified below.
  C++ formatting still reports advisory issues (16 files in the final gate); retain targeted D12 cleanup.
- D1/D2 Present vtable forwarding slice: foreign layers installed before CE's physical claim now
  receive Present and Present1 instead of being skipped by the inline trampoline. One private owner
  retains separate typed predecessors and scopes forwarding to the method/receiver; inline reentry
  cannot reuse the active vtable link. Two real-hook cases keep inline hooks active, exercise nested
  probes and handoff/removal, with exact physical coverage and no debug-layer errors. Six native
  argument/result/reentry/receiver/caller/identity cases and the read-only repair/detach cases pass.
  All six forwarding/scope/caller/identity mutations fail; restored production passes 539 focused
  native cases and all 32 FG flows. Closing 0.1.7020 passes native/Python/all 32 FG flows, x64/x86 products and setup packaging.
  Original SDK caller provenance and legacy shared-original publication are preserved; D3 aliases remain. This tests physical vtable
  installation order, not foreign inline hooks installed before CE DLL loading. Full D1/D2 stay open.
- D1 Present coexistence slice: a controlled foreign layer above the real DXGI Present chain covers
  repair, nested status probes and removal with an admitted callback. Native/wrapper Present and
  Present1 probes exposed false output accounting (16 ledger entries for 10 actual presents); their
  entry boundaries now forward before frame side effects. Three cases and four deliberate guard
  mutations pass. Remaining inline/DLL installation orders, actual nested physical outputs, CE removal with
  active Present callbacks and external provider unload remain pending; D1 is still partial.
  Closing 0.1.7018 passes native/Python/all 30 FG flows, x64/x86 products and the setup package.
- D0 active: every first-party subsystem inventoried; bounded operation traces and repeated coupling
  evidence recorded in [architecture-inventory.md](architecture-inventory.md). Remaining full lifecycle
  audits and child replacement/finalization traces are explicit there; clean/IPC fuzz gates passed.
- D1 partial: four NGX lifecycle/OFF/ON cases and four distinct queue-vtable scenarios pass through
  production hooks; NGX and queue mutations detect five and four defects respectively. Queue coverage
  includes exact ECL/Signal forwarding, duplicate capture and reset recovery; four queue cases pass.
  Contracts: [NGX lifecycle](frame-generation/ngx-flow-lifecycle.md) and
  [queue dispatch](dx12-queue-dispatch.md). Controlled foreign interposer, wrapper/native combinations,
  SDK unload/retirement and cold-FG startup coverage remain required. Apply D9/D12 inside each slice.
- D7 publication slice verified: one private NGX creation/evaluation boundary owns multiplier selection,
  accepted-OFF precedence and compatibility/shared publication. Four focused cases, five production
  mutations and closing 0.1.7008 (native/Python/all 19 FG flows, x64/x86 products/package) pass.
  Full SDK settings concurrency/generation and module/feature/context retirement remain pending.
- D2 partial: private queue dispatch owns installation/predecessor publication, coherent thread-local
  cache pairs and invalidation. Install, resolution, passive probes and reset now use operations/value
  snapshots instead of writable map/cache aliases; the unrelated global ECL fallback is removed.
  Nine native registry cases, four real-hook queue cases and four production mutations pass. Physical
  slot recovery remains allocation-checked in VTableHook; callback/code lifetime, hook detachment,
  native candidate lifetime still require further work. Signal uses the same private transaction and
  exact-slot recovery; its writable map/lock/global-original aliases are removed. D2 exit remains open.
  Final closing 0.1.7015 passes native/Python/all 23 FG flows, x64/x86 products and the setup package.
- D1/D2 follower-retirement slice: real Signal interposers cover CE above/below a foreign provider,
  removal/reset with a retained follower and an admitted callback across removal. The follower case
  reproduced a stack overflow after reset; ResolveInterception now owns saved-binding -> exact retained
  interception -> untracked live-slot priority for ECL, Signal and device tracing. Established bindings
  consult no cold readers. Eleven native registry cases and six dispatch mutations pass; physical
  restoration/Remove success do not prove callback drain or provider-code retirement. Remaining Present
  orders, provider unload, scoped code holds and D2 retirement still need implementation.
  Closing 0.1.7017 passes native/Python/all 27 FG flows, x64/x86 products and the setup package.
- D0/D10 device tracing slice: fixed the first-global creation-target crash under CE_DX12_TRACE=1.
  One private module owns exact-vtable installation, dispatch, reset recovery and tracing detours for
  queue/descriptor-heap/resource creation. Removed global targets, aliases and exposed detour prototypes.
  Native/debug-device creation, typed output checks and reset recovery pass through real hooks; the
  Signal case now runs with the production trace switch and queue installer. Both deliberate device
  mutations fail the expected crash/assertion; restored production passes. Provider lifetime, hook
  detachment and the rest of D10 remain pending; this does not close D2/D10.
  Closing 0.1.7016 passes native/Python/all 24 FG flows, x64/x86 products and the setup package.
- D9/D12 acceptance slice: removed unused factory detours and duplicate wrapper IDs from the touched
  queue installer, and moved Vulkan/ledger source protection to the actual wired factory hooks. The
  pacing fixture now installs the existing ClockSource; its deadline regression deterministically
  rejects missing clock wiring instead of letting host load saturate a supposedly virtual budget.
- Additional defect fixed: third-party startup transport passes skipped overlay admission/accounting.
  Transport selection is preserved while rendering proceeds; the first output is covered with RTSS
  present in the controlled WARP runs. This does not establish the real-game compatibility matrix.
- Baseline no-build reuse refused the stale unit-test link manifest at 0.1.7004; no native test ran.
  D0 clean verification 0.1.7005 passed products, native/15 FG flows, Python, ASan/UBSan and lint ratchets.
  All four parser fuzz targets passed a bounded 10-second-per-target run; hardware/application checks remain pending.
- Temporary directory literally inventoried: only this working copy remains. The earlier 32-file
  cleanup is already complete; do not repeat it or treat future diagnostic files as disposable.

- Additional pending findings from this slice: status-query entry guards skip frame processing,
  but CallOriginalPresent/Present1 still invoke flip-queue pacing when backbuffer override/provenance
  enables it. A waitable-query regression and correction remain required. Native test linking also
  reports duplicate FontAtlas/VulkanBackend definitions from source-including test units; the build
  currently accepts these. Investigate their test seams/link ownership separately from this slice.
