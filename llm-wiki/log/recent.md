# llm-wiki Log

### 2026-10-08 - Generic display gamma correction

- `Graphics.display_gamma`/`gamma_source` implemented in the CAS/RCAS post-process stage (D3D11, native D3D12,
  Vulkan, FSR FG callback output). Details: `post-processing-sharpen.md` "Generic display gamma".
- Closing gate 20261008_172*_build_7034 passes (unit, Python self-tests, 32 FG flow scenarios with `srgb`,
  setup 0.1.7034). Fixed two source-scan tests broken by the change (Vulkan registry key now includes the
  queue; resolved-queue scan window 3000 -> 4000 chars). Hardware run pending.

### 2026-10-08 - Child stop outcomes no longer depend on recording state

- common/ipc owns the local accepted/rejected/acknowledgement-unknown type and stop-command adapter.
  RecordingSession retains a compatibility alias internally; host children no longer import its model.
  No wire/shared-memory or public C layout changes. Existing transport/session/child/IPC tests and
  three syntax-checked product TUs pass; the M3 move mapping now has 8 forbidden edges (was 11).
- The first closing gate's privacy inventory still listed the unstaged deleted header. Staging its
  intended deletion and new IPC headers fixes the inventory without changing privacy checks.
  Resumed closing gate 20261008_140222_build_7032 passes native/Python/privacy/all 32 FG and packages
  the 38,815,644-byte setup PE. No new tests/logging: this preserves classified outcomes and existing
  coverage/diagnostics; real helper finalization/A/V and full runtime composition remain later work.

### 2026-10-08 - Keyboard hook no longer imports controller internals

- Hook startup accepts an immutable host thread/private-message route. Publication precedes thread
  creation; reset follows join. Callback matching and failed-delivery key pass-through stay unchanged.
  Invalid/system-message routes and active retargeting are rejected with startup diagnostics.
- Three native queue interface cases verify message identity/payload, host-selected routing and invalid
  targets without installing a global input hook. Existing matching/ownership/wiring/input tests and
  three product TUs pass. M3's move-time forbidden edges shrink 12 -> 11; DR-19 orders detachment first.
- Closing gate 20261008_133330_build_7031 passes native/Python/all 32 FG checks and packages the
  38,817,040-byte setup PE. Nonthrowing constructor fixes the new static-initialization warning;
  focused 20261008_133841_build_7031 and lint 20261008_133850_build_7031 pass (706 warnings/1014 TUs).
  Three existing-file formatting advisories remain. Real keyboard-hook/app smoke is pending hardware
  validation; no new live-hook regression installs hooks on the user's desktop. Runtime composition,
  remaining controller dependencies, module moves and public API implementation remain M3-M4 work.

### 2026-10-08 - v2 C/C++ ABI baseline verified

- C11/C++20 assert 56 public field offsets/widths and 8 type sizes/alignments; the C object is linked
  and executed, with __cplusplus rejected. C uses ambient pack 1 and restores it; C++ uses default
  packing and verifies move-only wrapper types. Copied-header field drift and removed guards fail.
- DR-18 fixes initializer extent/version: the old pointer-only draft could not safely initialize an
  older client's descriptor after a minor extension. All 30 prototypes match the canonical contract.
  C test flags retain target/debug/CFG/privacy/sanitizers; public/runtime/frontend analyzer scope is covered.
- Interrupted 7028 produced no terminal manifest; stopped processes were confirmed, resume refused.
  Strict-clean 20261008_123915_build_7029 compiled products and passed native/Python/ASan/32 FG;
  assertion macro warnings were fixed, then resumed verify 20261008_125944_build_7029 passed/package:
  38,814,944-byte setup PE; 706 accepted warnings across 1013 TUs. Final new-header formatting and
  20261008_130659_build_7029 focused ABI tests pass. No runtime functions, exports or DLL are added;
  live API/ownership tests, external SDK/MSVC use and hardware/A/V evidence remain later milestones.

### 2026-10-08 - Unshipped v2 API and client ownership wrapper

- Full draft covers lifecycle, commands/events/status, settings and setup/utilities; fixed-width C
  fields and saved/restored 8-byte packing define x64 layouts. C++ wrapper retains event/edit owners,
  preserves errors, bounds waits and rejects embedded NULs. No runtime functions or exports are added.
- Declared Clang compiles the draft as C11 and the wrapper as C++20 with warnings as errors.
  Closing gate 20261008_093153_build_7027 passes native/Python/all 32 FG checks and packages the
  38,815,302-byte setup PE. Permanent layout/C build tests follow; live wrapper/runtime tests are M4.

### 2026-10-08 - Module boundaries enforced by verification

- Lint fails architecture regressions even in advisory mode; unfiltered Python gates run policy/gate
  fixtures, the live tree scan and depth report. Manifest details preserve complete includer counts.
- Clean transaction 20261008_085706_build_7026 exposed fixtures inheriting the enclosing Git approval
  set. The checker now requires the exact repository root; a focused regression protects isolation.
  Resumed verify 20261008_090850_build_7026 passes native/Python/ASan/all 32 FG and lint ratchets;
  setup is a fresh 38,815,186-byte PE. Accepted warnings remain 706 across 1011 TUs.
- Actual temporary hook include of app/main_internal.h fails; restored source scan passes. Twenty-three
  boundary/gate cases (31 with existing lint cases) pass. Q13 documents 12 M3 move-time couplings;
  no new waiver is added. No runtime/frontend/library binary or hardware/A/V delivery is claimed.

### 2026-10-08 - Library module dependency checker

- The source scanner checks quoted/angle and relative includes, ignores comments/raw fixture strings,
  and records 19 exact legacy exceptions with reasons/milestones. New edges and additions to the
  committed exception set fail; pruning removes only obsolete entries. Sixteen behavioral fixtures pass.
- Initial graph: 3645 edges; DX12 internal header 74 includers (2 outside), media 29 (0 outside).
  Depth estimates are labeled; writable-global analysis stays unclaimed once runtime/frontend exist.
- Closing gate 20261008_084914_build_7025 passes native/Python/all 32 FG checks and produces the
  38,814,900-byte setup PE. Direct Python style/types and 16 focused fixtures pass; build wiring follows.

### 2026-10-08 - Library-first plan adopted

- The committed plan moved to library/; index routes to the canonical architecture/API/milestones.
  D8/D9/D13 are superseded; the unpublished v1 facade is replaced and hotkeys/desktop overlay are
  optional runtime features. Previous execution evidence is preserved in archive-2026-W41c.md.
- Documentation-only adoption: relative links, canonical-copy checks and changelog validation pass;
  no product behavior changes or new hardware/A/V claim. Milestone status lives only in library/README.md.

### 2026-10-08 - Runtime package paths and helper configuration handoff

- WinMain now consumes explicit configuration arguments; workers previously ignored the INI path
  already sent by SpawnChildProcess. Package defaults are executable/module anchored, and native child
  ownership copies an explicit helper executable. ACP paths no longer go through a UTF-8 decoder.
- Path/option tests cover spaces/Unicode, duplicate/empty arguments, relative resolution and delegated
  game arguments. Native renamed-helper probe loads the selected INI from another directory with bounded
  process exit/cleanup; focused IPC/configuration/owner checks pass. All five parser fuzz targets pass
  a bounded 10-second-per-target run; runtime options execute 579,654 units.
- Strict-clean 20261007_205639_build_7024 compiled all products and passed native/Python/ASan/32 FG
  checks. After fixing its unnecessary-copy lint finding, resumed verify 20261008_081425_build_7024
  passes and packages the 38,814,820-byte setup PE. Baseline stays 706 across 1011 TUs; four formatting
  advisories remain. Full library runtime/client delivery and hardware/A/V validation stay open.
- Main's server rule requires linear history. Replaced unpublished 01760a89 with 644219f2; exact tree
  and unfinished path files preserved, backup ref retained, four outgoing commits scanned, dry run passes.

### 2026-10-07 - Recording recovery branch integrated with owned configuration/helpers

- Integrated 21956184/087452a3 into main; only changelog/journal conflicts required resolution, preserving
  both histories. Closed-loop budget/audio/loader/configuration tests pass; test rounding uses llround.
- Strict-clean 20261007_194219_build_7023 passes products/native/Python/ASan/all 32 FG checks; after test
  rounding corrections, resumed verify 20261007_200539_build_7023 passes and packages the integrated setup:
  38,787,412-byte PE; 706 accepted warnings, 1009 TUs. Twenty-eight formatting advisories remain.
- Direct built-DLL export check: 48-byte/8-aligned snapshot, null/wrong-size rejection, valid=0 and zero
  counters without an engine. Hardware overloaded-output and full codec/multitrack A/V runs remain pending.

### 2026-10-07 - Runtime configuration replaces mutable host settings

- Native settings scope and production transaction own snapshot, startup identity, coherent reload and
  deadlines. Removed main_g_Config; frontend consumes old/new snapshots and retains hotkey/service effects.
- Startup edits before the first poll previously became falsely applied. Eight transaction cases and
  two native/headless INI cases pass with existing policy/sensor/hotkey guards (38 cases); eight product
  TUs pass syntax checks. Controlled native time advances without sleeps or timestamp-resolution assumptions.
- Six production mutants fail their expected assertions; exact source restored and tests pass. Private
  getter scope and one-owner conflicts are protected. Public runtime API and initial transactional
  validation remain pending; executable defaults/loading behavior and parser code are preserved.
- Strict-clean 20261007_190950_build_7022 passes products/native/Python/ASan/all 32 FG scenarios.
  Explicit test optional guards close resumed verify 20261007_192804_build_7022 and setup packaging:
  38,775,318-byte PE; 706 accepted warnings, 1007 TUs. Eight formatting advisories remain.

### 2026-10-07 - Slow output target froze CFR video for 15 minutes; mux byte budget

- `logs/20261007_120811` r0005 (0.1.7018, AV1 4K120 VBR 125/200 Mbps, DXGI dup, SMB share): CPU 100%
  15:59:40-16:12:30, writer 7-9 MB/s vs encoder 16-23 MB/s, 512 MB queue full in ~40 s, WriteFrame
  blocked the encoder. Debt peaked 150.4 s, `FreshMiss` ~99%, recovery ~0.2 s/s, 55.6 s frozen stop
  tail, ~128.7 s audio per source lost to 30 s ring retain-trims; overlay "recovering" correct.
- Time pacer could not help (`fresh=23.07ms repeat=22.33ms`, 5% floor alternating with blocking).
  Fix: `cfr_mux_byte_budget.h` + `MediaEngine_GetMuxFlowSnapshotV1` cap the fresh-slot share at
  measured writer capacity (see `cfr-capture-sync.md` "Mux writer byte budget"). Closed-loop test
  replays the r0005 rates: unpaced blocks, paced never does (~38% fresh). Branch-local gate 0.1.7022 was recorded as passed
  (native, Python, 32 FG flows, setup 38,765,676 bytes). Hardware run pending.
- Follow-up commit: CFR ring-overflow retain-trims are owed as silence at their timeline position
  instead of shifting later audio earlier (`retainSilence=` in `[STOP AUDIO DETAIL]`). Slow all-hold
  repayment of large debt at 4K (repeat encode ~5-9 ms) remains inherent; prevention is the lever.

### 2026-10-07 - Runtime helper ownership begins library extraction

- One host child scope owns active/retired processes and authenticated IPC clients. Removed four
  writable process handles and two client globals from app consumers; headless scope construction
  needs no ControllerMain/tray. Full engine bootstrap, DLL packaging and client conversion remain open.
- Old media finalizers were forgotten on restart and excluded from shutdown. Retirement now retains
  their handles while a fresh recording starts; shutdown collects all active/retired identities.
- Generation checks cancel message-pump reentry and obsolete successful spawns; recursive readiness
  is refused. Shutdown waits only on remaining live peers; failed termination does not count as exit.
- Ten lifecycle regressions, three native empty/headless-scope cases and the focused controller/
  recording/IPC set pass. Five deliberate ownership/cancellation mutations fail, then restoration passes.
- Native test compilation/coverage wiring changed; strict-clean 20261007_171441_build_7021 passes product,
  native/Python/ASan and all 32 FG checks. Its final lint stopped at compiler-vptr analyzer false positives.
  Comment-only annotations then close resumed verify 20261007_180216_build_7021, including setup packaging.
  Installer 0.1.7021: 38,760,346-byte PE; 706 accepted warnings (712 previously), 1005 TUs in full lint.
  Resume had silently overridden clean mode; fixed precedence with a Python regression before retry.
  Blocking latency-band/pointer-conversion lint findings were corrected without baseline increases.
  Full-scope lint also caught shutdown allocation/exception paths; retry retains active ownership.
  Hook/probe casts and explicit patch rollback were corrected. Scoped annotations document intentional
  fatal lock initialization and GoogleTest reporting-allocation failures in isolated test destructors.
  Formatting retains 16 advisory files; hardware/A/V and complete library delivery remain unverified.
  No helper recording/hardware claim follows from these controlled lifecycle and native-empty tests.
- Initial capabilities/embedding effects/path inventory: library-delivery.md. MainThread cancellation
  is atomic; auxiliary reconfiguration retains its stop event until old services exit. Setup/sensor
  recovery, full config/runtime ownership and old/new media observation attribution remain pending.

### 2026-10-07 - Present vtable ownership preserves foreign links and caller provenance

- Reproduced a skipped predecessor: a foreign layer installed before CE received zero of three
  presents. Private typed bindings and scoped forwarding preserve Present/Present1 next links;
  inline reentry cannot restart the same link. Original receiver/arguments/HRESULT remain exact.
- Original caller provenance must cross the adapter: hiding the SDK caller caused uncovered DLSS/
  NGX startup outputs. The scope now retains it only for its own receiver/method/view. Legacy shared
  original publication remains for SDK observation; broader D3 aliases are not closed by this slice.
- Self-target checks recognize both CE views. Temporary handoffs restore the exact displaced entry,
  preserving foreign followers. Source guards still protect ownership/publication and Steam ordering.
- Two additional real-hook cases keep inline hooks active and cover both physical vtable orders,
  nested probes, handoff/removal and subsequent native output. Six native scope/identity cases and
  six deliberate mutations pass; restored production passes 539 focused native cases/all 32 FG flows.
- Closing 20261007_141239_build_7020 passes native/Python/all 32 FG flows and x64/x86 products/package.
  Setup: 38,737,206-byte PE. No new compiler warnings; no sanitizer/fuzz/hardware checks in this slice.
- Query-forwarder flip-queue pacing and source-including duplicate test symbols are documented debt.
  CE callback drain, actual nested outputs, remaining inline/DLL orders and provider unload stay open.

### 2026-10-07 - Reusable library and first-client delivery join the active refactor

- User requested integrating library extraction with ownership work. D13 now requires independent
  runtime/configuration/helper ownership, versioned C ABI, a packaged standalone client and the
  shipping application consuming the same API. No runtime/library implementation is claimed.
- Preserve current controller-bound v1 attach/detach semantics and existing process/DLL topology.
  D0/D8/D5-D6/D9 feed library milestones; no wait for every graphics audit and no one-shot rewrite.
- Updated canonical/temp plan and historical roadmap/contracts; rich events, preview/packets,
  arbitrary parallel instances and plugins remain separate feature work.

### 2026-10-07 - Present coexistence exposes and fixes status-probe frame accounting

- Three real-hook cases exercise a foreign layer above the underlying DXGI Present chain, repair,
  nested status probes, admitted probe completion after removal, and native/wrapper Present/Present1.
- The baseline counted 16 outputs for 10 actual presents, and an admitted probe added a false output.
  DXGI_PRESENT_TEST now forwards before drawing, pacing, metrics, accounting and FG observation;
  original arguments/HRESULT remain intact. Change-gated diagnostics avoid idle-loop noise.
- All four deliberate native/wrapper guard mutations fail the query regression; exact restoration passes.
  A shared completion-aware barrier releases/joins both Present and Signal fixture calls on every exit.
- Closing 20261007_104720_build_7018 passes native/Python/all 30 FG cases and x64/x86 products/package.
  Installer: 38,731,270-byte PE. Sanitizers/runtime/fuzz were not rerun for this bounded slice.
- D1 remains partial: opposite Present installation order, actual nested outputs, CE removal during
  active Present callbacks and external provider unload are still required. Goal pause requested after commit.

### 2026-10-07 - Retained interception evidence prevents foreign-follower recursion after reset

- Three real Signal interposer cases cover installation above/below CE, preserved foreign followers,
  reset and an admitted callback across removal. Barriers and scoped release/join avoid test sleeps.
- The follower/reset case reproduced 0xC00000FD; an x64 dump with Microsoft and matching CE symbols
  showed repeating foreign/CE Signal frames. Reset recovery selected the foreign live slot too early.
- Shared ResolveInterception prioritizes private saved bindings, retained exact-slot evidence and
  untracked live slots. ECL/Signal/device tracing use it; established bindings call no cold readers.
- Six dispatch mutations and both device mutations fail expected checks; exact source restoration passes.
  The follower regression asserts the target before invocation, catching deliberate bad chains safely.
- Closing 20261007_100840_build_7017 passes x64/x86 products, native/Python and all 27 FG cases.
  Installer: 38,729,166-byte PE. No touched-source compiler warnings; sanitizers/runtime/fuzz not rerun.
- Remove success/physical restoration do not prove callback drain or provider code retirement.
  SDK unload, allocated thunks, Present interposers and full D2/D10 remain open; CE itself stays pinned.
