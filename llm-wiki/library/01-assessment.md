# 01 - Assessment of the current plan

Subject: `llm-wiki/architecture-debt-plan.md` (`temp/refactor.md`), D0-D13, state at `644219f2`.

## What the current plan gets right (keep all of it)

- Definition of depth: "a deep module owns a meaningful policy and lifetime behind a small contract".
  Moving fields, adding forwarders and splitting files don't count as progress.
- Rejects the shallow patterns: getter/setter wrappers, god managers, empty facades, generic
  plugin/frame/backend hierarchies, a single "FG mode" enum, and opaque handles everywhere.
- Keeps the process/DLL topology, the native DX12 overlay, and the hot paths free of virtual
  dispatch, copies, allocation and lock churn.
- Keeps settings, runtime presence, route ownership and visible status distinct.
- Vertical, independently verified slices. The frontend is the first library client. A separately
  built headless client is the proof.
- The completed owners are real improvements and remain the base: `RecordingSession`,
  `HostChildrenSession`/`ChildProcessLifecycle`, `RuntimeConfigurationSession`/`ConfigurationState`,
  `RuntimePackagePaths`, inject control channel, submission adapters, PostSL owners and queue
  dispatch.

## Problems

### P1. Organized around debt, not around a target design

D0-D13 lists what to fix but never names the end-state modules or their interfaces. Names are
"settled from the source inventory", which is bottom-up. Deep modules are designed outside-in:
decide the few modules and their small interfaces first, then move the code behind them.

The plan's own measurements show the effect. Full source context grew in every tracked
investigation: 1,509 → 1,705 lines, 2,886 → 3,302 and 2,015 → 2,271. New owners were layered on
top of the old code; callers got more local, but the system got more layers.

### P2. The library's critical path is buried behind hook-internal work

The library boundary sits at the controller/runtime level: settings, helper processes, recording
state, commands and events. Hook internals (D1-D4, D7, D10) live behind the injected DLL and IPC,
so a library client never sees them. Even so, most recent execution was D1/D2 hook coverage. The
plan does say that "full D2-D7 migration is not a prerequisite", but no ordering enforces it.

### P3. Preserving the v1 facade costs effort and protects nobody

**(verified 2026-10-07)** `include/libcaptureengine.h` is compiled into `captureengine.exe` and is
not shipped as a library. No source outside `captureengine/app/` includes it. `ce_engine_create`
only attaches to an already running controller thread, and stats and custom config return
`CE_ERROR_UNSUPPORTED`. With no external consumer, keeping its semantics ("preserve those v1
contracts") is pure cost. Replace it.

The same applies to `mediaengine.dll` exports and hook↔host shared memory: they ship together in
one package and are built together. They are **package-private** and version-locked, not public ABI.
The plan's "preserve legacy positional exports" rule only holds while internal callers still use
them.

### P4. The API decisions that make a library "done right" are missing

D13 lists properties the API must have but makes none of the choices: threading model, event
delivery, recording state machine, configuration model, ownership conflict behavior, module unload
safety and process-wide side effects. [03-public-api.md](03-public-api.md) makes them.

### P5. Tests are coupled to internal seams

The plan's main test style is "private static adapters", "production transaction seams" and a
mutation proof per seam. That pins internal structure, which is the opposite of what deep modules
buy you: you should be able to rewrite a module's inside without touching its tests. Seam tests stay
justified where no public interface exists (hook hot paths, WARP flow harness). For runtime,
recording, settings and children, test through the module interface and the public API. See
[07-testing-and-verification.md](07-testing-and-verification.md).

### P6. The plan document is itself a shallow module

At 758 lines it mixes about 115 lines of build-number status, plan, rules and a restatement of
`CLAUDE.md`, and it exists twice (wiki page plus `temp/refactor.md`). Every agent session pays to
read all of it. Split it: target architecture (stable), milestones (short), status table (one
place), evidence in `llm-wiki/log/`.

### P7. No enforcement of boundaries

Include fan-out (`main_internal.h` has 8 includers in `captureengine/app` **(verified 2026-10-07)**;
`dx12_hook_internal.h` 71; `media_main_internal.h` 28) is measured but nothing stops it from growing
back. Deep modules decay without a checked dependency rule. M1 adds one.

## What changes

| Current plan | This plan |
| --- | --- |
| D0 inventory as a phase | Done enough; the library inventory lives in 04/05 here |
| D1-D4, D7, D10 graphics waves | **Graphics track**, independent, driven by bugs and FG requirements; not a library prerequisite |
| D5-D6 media | **Media track**. Only two items gate the library: finalized-output reporting and stats (M6, M11) |
| D8 controller/config | Absorbed into M3-M5 (runtime core, settings API) |
| D9 shrink interfaces | Replaced by the boundary checker (M1) plus interface review per milestone |
| D11 build modules | **Build track**, independent; the library adds only targets and a checker |
| D12 hygiene | Ongoing; also delete the v1 facade and legacy package-private exports once unused |
| D13 library | M2-M11, designed outside-in from the public header |
| "Preserve v1 attach semantics" | Dropped: v1 is replaced (no external consumers) |
| Per-slice mutation proof everywhere | Mutation proofs only for high-risk invariants (lifetime, ordering, A/V); interface tests elsewhere |
