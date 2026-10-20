# Code Structure

Last cross-checked: 2026-10-20 (libcengine phase 1, branch `claude/libcengine-refactor-orchestrate-e6c27c`)

Primary sources:
- `AGENTS.md` ("Deep modules", file-size rule)
- `tools/module_boundaries.json`, `tools/check_module_boundaries.py` (dependency rules and exact exceptions)
- `runtime/core/runtime_internal.h` (`RuntimeParts`: the composition seam), `runtime/core/runtime_create.cpp`
- `include/cengine/cengine.h`, `runtime/api/cengine.def`, `tools/check_cengine_exports.py`, `tests/cengine_abi_layout.h`
- `llm-wiki/library/` (02 architecture, 04 modules, 07 testing, 08 decisions DR-15/16/21/22/27/29)

Related pages: [codestyle.md](codestyle.md) (formatting, tooling, splitting),
[transaction-and-retirement-invariants.md](transaction-and-retirement-invariants.md),
[refactor-contracts.md](refactor-contracts.md), [regression-testing-and-logging.md](regression-testing-and-logging.md).

## Deep modules

A module's cost to the rest of the code is its interface; its benefit is what the interface hides
(John Ousterhout, *A Philosophy of Software Design*; Matt Pocock applies the same idea to codebases that
humans and agents change together). Prefer few modules, each with a **small interface hiding a large
policy, its state and its lifetime**.

Examples in the tree:

| Module | Interface | Hides |
| --- | --- | --- |
| `runtime/settings/settings_store` | `Current`, `Check`, `Commit`, `Poll`, `Revision` | INI round-tripping, validation, coherent replacement, self-write detection, conflicts |
| `runtime/core/event_queue` | `Reserve`, `PushState`, `PushReserved`, `Next`, `Release` | bounded memory, coalescing, never-dropped completions, drop notices |
| `runtime/children/host_children` | `Ensure`, `Send`, `RetireMedia`, `Shutdown` | spawning, authenticated endpoints, retirement of finalizing media, never terminating (DR-17) |
| `runtime/recording/recording_session` | `Toggle`, `Start`, `Stop`, `Observe` | request/live/failure state machine; the only toggle policy |
| `runtime/platform/setup_actions` | `Start`, `Collect`, `Status`, `Shutdown` | two worker families, one action at a time, exactly-once results |

Rules:
- **Design the interface first**, from what callers need, not from how it works inside.
- **One owner per policy.** A decision (toggle semantics, service enablement, the driver offer) lives in
  one module; callers ask, they do not re-derive it. Duplicated decision logic is a defect.
- **Pull complexity down.** The module handles its edge cases (already stopped, retry, partial failure);
  callers stay simple.
- **Define errors out of existence** where the semantics allow: idempotent shutdown, no-op for "already
  in that state", exactly-once completion instead of "maybe twice".
- **No pass-through layers**: a method that only forwards to another module with the same signature, a
  "manager" that owns nothing, getters that hand out internal containers.
- **Avoid information leakage**: two modules that both know a file format, a wire layout or an ordering
  rule must share one owner of that knowledge.
- **Avoid temporal decomposition**: split by knowledge (what is hidden), not by the order things happen.

Red flags in review: the same switch in two places, a struct passed through three layers unchanged, a
caller that has to call methods in a specific order to stay correct, comments that explain an
interface by describing its implementation.

## Dependencies and composition

- Dependency rules are data (`tools/module_boundaries.json`), checked by `tools/check_module_boundaries.py`
  in lint: `frontend/**` includes only `include/cengine/**`; `runtime/<subsystem>/` includes itself and
  `common/**`; `runtime/core/` composes (it may include the public headers of runtime subsystems);
  `hook/**`, `mediaengine/**` and the role host never include `runtime/**`; `common/**` includes only
  `common/**`.
- **Subsystems never include each other (DR-21).** A component takes `common/**` types or declares a
  small interface that `runtime/core` adapts (`InjectLink`, `DesktopPresence`, `HotkeySystem`,
  `SetupBackend`, `HostChildren`).
- **Exceptions are exact pairs** (source file, target header, reason, milestone). The list only shrinks;
  adding one is a reviewed decision, never a way to make the checker pass. The checker refuses new pairs
  that are not in the committed file.
- **One composition root.** `runtime/core` (`RuntimeFactory`, `runtime_create.cpp`) wires production parts;
  tests assemble the same engine from fakes through `RuntimeParts`. Nothing else constructs subsystems.
- **Process-wide effects have one owner** (`DiagnosticsSession`, `ce_runtime_create`) and are opt-in for
  library clients (`CE_FEATURE_*`). No other code changes process state (timers, priorities, COM, DLL
  search paths, crash handlers).
- **Code shared by two binaries** is compiled into both and never shares state across them (DR-15).

## Testability seams

- Operating-system effects sit behind small interfaces with a production implementation in a
  `*_system.cpp` unit (`ChildProcessEffects`, `SetupBackend`, `HotkeySystem`, `Clock`, `MonitorListing`);
  tests inject fakes.
- Pure policy is a free function or a small value type tested directly (`service_policy.h`,
  `sensor_offer_policy.h`, `ChildProcessLifecycle::DrainUntilExited`).
- Tests go through interfaces. Waits end on events with generous upper bounds, never on sleeps.
  Source-text tests (reading `.cpp` files for strings) exist for legacy code; do not add new ones for code
  that has an interface, and replace them when touching that code.

## Lifetime, ownership, concurrency

- Every resource has one owner (RAII); a module that creates a thread joins it before it dies.
- Mutable engine state belongs to one thread; state shared across threads sits in one struct behind one
  mutex (`EngineShared`), with documented publication order (publish state, then signal, then announce:
  DR-27, DR-29).
- No new process globals. `thread_local` only for trivially destructible values. The role-host globals
  (`media_main_g_*`) are legacy; phase 2 (`library/09`) removes them.
- Transactions and retirement: [transaction-and-retirement-invariants.md](transaction-and-retirement-invariants.md).

## Files and units

- File size: AGENTS.md (working target 750 lines, ceiling 800, lint-enforced). Split by logical unit
  (`engine.cpp`, `engine_lifecycle.cpp`, `engine_setup.cpp`), never by packing lines.
- Module-private headers end in `_internal.h`; lint reports their includer counts (the depth metric).
- A new runtime unit is registered in the tests-only build lists (`tools/build/build_compile_db.py`); the
  product build picks up `runtime/**` automatically.

## Public C API rules (libcengine)

The shipped API is `include/cengine/cengine.h` (C11/C++), with the header-only C++ wrapper
`cengine.hpp`. Design rationale: `library/03-public-api.md`. Third-party documentation: `sdk/README.md`.

1. **C ABI only.** No STL, COM, FFmpeg or SDK types; no C++ exceptions across the boundary (every entry
   point runs under `RunGuarded`); no callbacks into the client (events are pulled).
2. **Versioning.** `CE_API_VERSION` is major.minor. A runtime serves clients of its major and an equal or
   older minor. Structs carry `struct_size`; once shipped, fields are append-only with a minor bump, and
   the runtime reads only the extent the client declared (`ce_runtime_desc_init`).
3. **ABI is tested.** Every public struct has offset/size/alignment checks in
   `tests/cengine_abi_layout.h` (compiled as C11 and C++). The export list is the header:
   `runtime/api/cengine.def` must match it in order (Python test), the DLL's export table must match it
   (build check), and the build loads the DLL and compares `ce_api_version`.
4. **Handles are opaque and validated** (handle registry): a stale or foreign handle returns
   `CE_E_INVALID_ARGUMENT`, never crashes.
5. **Threads.** Every function may be called from any thread and never waits for engine work. Commands
   are queued and return a request id; exactly one `CE_EVENT_COMMAND_COMPLETED` follows every admitted
   request (DR-27). Admission outside READY is refused without a request id.
6. **Memory and strings.** Strings are UTF-8 in and out, length-bounded and validated. Variable-size
   output uses caller buffers with a `needed` size. Event memory is the runtime's; pointers in an event
   stay valid until `ce_runtime_release_event`.
7. **Errors.** `ce_status_t` values are stable; new codes are appended; `ce_status_string` names each.
   Human-readable detail goes in the event message and the runtime log, never only to the debugger.
8. **Process effects are opt-in** (`CE_FEATURE_*`); the library never terminates a process on its own
   (DR-17) and exposes no global state.
9. **Deprecation.** A shipped function stays for the whole major version: mark it in the header and the
   SDK readme, remove it only with the next major, and say so in the CHANGELOG.
10. **Every API change ships with:** a doc comment in the header, ABI layout checks, C API tests through
    the factory seam (`runtime/api/capi_testing.h`), the `.def` line, the `cengine.hpp` wrapper, the SDK
    readme (and a sample when it is a new task), and a CHANGELOG entry.

## Checklist for a new module or a structural change

- What does the interface hide? If the answer is "little", merge it into its caller or its callee.
- Who owns each policy, each piece of state and each resource? Exactly one place each.
- Which dependency rule applies? Does the change need a boundary exception (it should not)?
- How is it tested through its interface, and which fake replaces the OS?
- Which diagnostics (rate-limited, stable prefixes) show its transitions and failures?
- Does a wiki page or the repo map need the new owner recorded?
