# llm-wiki Log Archive: 2026-W41d

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
