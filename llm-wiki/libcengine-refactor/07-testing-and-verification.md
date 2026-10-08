# 07 - Testing and verification

## Principle: test at the deepest interface that exists

A deep module's tests call its interface and observe its outputs. Its internals can be rewritten
without touching the tests. Test layers, from most to least preferred for runtime work:

| Layer | What it tests | Where |
| --- | --- | --- |
| Public API tests | `ce_*` contract: lifecycle, admission, exactly-once completion, events, settings, errors, stale handles, busy | `tests/test_cengine_api_*.cpp` (in `unit_tests.exe`, using a fake-children runtime factory) |
| Module interface tests | `Runtime`, `SettingsStore`, `ChildSupervisor`, `RecordingController`, `TargetLauncher`, `Screenshotter` through their headers | `tests/test_engine_*.cpp` |
| Pure policy tests | existing `RecordingSession`, `ConfigurationState`, `ChildProcessLifecycle`, parsers | existing files, keep |
| Integration (real processes) | headless client with the real role host and real helpers; outside-repo deployment | M8 integration test (bounded, process cleanup verified) |
| Hardware/user validation | real games, FG switching, A/V | user-driven, listed as pending per milestone |

Seam/source-reading tests stay where no behavioral interface exists (hook hot paths, WARP FG flow
harness, ABI/layout, hook/security invariants, packaging policy). Don't add new source-reading
tests for runtime behavior.

## Test doubles

- **Fake children:** `ChildSupervisor` gets a test constructor taking an effects interface. The
  existing `ChildProcessLifecycle` already uses controlled OS effects, so reuse that seam. The
  `Runtime` test factory (`ce::engine::testing::CreateRuntime(options, FakeEffects&)`) lives in a
  test-only header that production code doesn't include, and is linked only into `unit_tests.exe`.
- **Fake clock:** inject it the way `RuntimeConfigurationSession(path, nowMs)` does. Deadlines,
  debounce and auto timers are driven by advancing the fake clock. **No sleeps** (CLAUDE.md).
- **Isolated named objects:** session claim and discovery names get a test prefix through an
  internal parameter (as `InjectControlChannel` already has `discoveryName`). Tests never touch the
  real `Local\CaptureEngine_Instance_Mutex`, so a running CaptureEngine on the dev machine doesn't
  break tests.
- **Settings files:** temp directories per test; never the repo's `config.ini`.

## Required test cases (beyond those listed per module in 04)

API lifecycle:
- create with a NULL desc / wrong `struct_size` / major version mismatch / client minor too new
- create twice in one process → `CE_E_BUSY`; create while another (fake) owner holds the claim →
  `CE_E_BUSY`
- create → READY → `request_shutdown` → STOPPED → destroy → create again (recreate)
- destroy while READY → `CE_E_INVALID_STATE`; stale handle after destroy → `CE_E_INVALID_ARGUMENT`
- shutdown timeout keeps the runtime valid; a retry succeeds once the fake finalizer exits
- package missing role host → `CE_E_PACKAGE` with message naming the file

Commands/events:
- every accepted request yields exactly one COMMAND_COMPLETED (property-style test over random
  command sequences against a fake runtime: count completions per id)
- commands in STARTING → `CE_E_INVALID_STATE`
- overflow: produce more than the capacity in state events without releasing → coalescing, DROPPED
  count, COMMAND_COMPLETED never lost
- `ce_runtime_event_handle` signaled iff events pending (check with `WaitForSingleObject(h, 0)`)
- `next_event` with `UINT32_MAX` → `CE_E_INVALID_ARGUMENT`

Recording mapping (fake media/inject effects):
- start → STARTING → LIVE → stop → STOPPING → IDLE, then FINALIZED with path
- immediate restart while the first finalizes: two ids, events correctly attributed
- failure during STARTING → IDLE with `failure`, in-game failure notification requested
- toggle semantics identical to `RecordingSession` (reuse its truth table)

Settings: see 04 §settings. Plus API-level: diagnostics strings remain valid until discard; commit
consumes the edit only on success.

Threading:
- concurrent submits from N client threads while the engine services (no lost completions, no
  deadlock; run under the existing sanitizer gate in `--verify`)
- `ce_runtime_get_status` from a client thread during a settings commit returns a coherent snapshot
  (all fields from one publication: test with a status revision counter)

ABI (M2+):
- the C translation unit compiles as C11
- `static_assert` offsets/sizes of every public struct (x64)
- M7: exported symbol set == `cengine.def` == the functions in `cengine.h` (script compares all three)
- M7: frontend objects reference no `common` logging symbols (duplicate-statics guard)

Module unload (M7): load `cengine.dll` dynamically, create/shutdown/destroy, `FreeLibrary`; repeat
three times; assert no crash and no thread left (enumerate threads of the test process whose start
address lies in the DLL range).

## Mutation proofs (targeted, not blanket)

Use a deliberate mutation only where a regression would be silent and costly:
- exactly-once completion (drop or duplicate one completion → test fails)
- shutdown ownership retention on timeout (release early → test fails)
- settings conflict detection (skip the identity check → test fails)
- retired-finalizer attribution (attribute FINALIZED to the newest id → test fails)
- module pin (remove the pin → unload test fails)

Record each mutation and its failing test in the milestone's log entry, not in source.

## Integration fixture rules (M6/M8)

- Start processes in dependency order and wait for readiness signals (RUNTIME_STATE READY), never a
  fixed delay.
- Every started process gets an explicit stop condition and a bounded total budget; on failure the
  fixture shuts down the runtime, then terminates only processes it started, then verifies none
  linger (CLAUDE.md lifecycle rules).
- No game needed: use audio-only, or WGC capture of a fixture window the test creates. Inject capture
  stays in hardware validation (and the existing WARP flow harness for hook behavior).

## Depth metrics (record at each milestone end)

Script `tools/analysis/module_depth.py` (added in M1, extended later), reported in the build summary:

| Metric | Definition | Direction |
| --- | --- | --- |
| Public surface | exported `ce_*` functions; public methods per module header in `runtime/*` | small and stable |
| Hidden size | lines in the module's `.cpp`/internal headers | may grow, that is depth |
| Depth ratio | hidden lines / public functions | up |
| Internal header reach | includers of each `*_internal.h` outside its subsystem | 0 |
| Writable globals | namespace-scope non-const variables in `runtime/**`, `frontend/**` | 0 (documented singletons excepted) |
| Boundary exceptions | entries in `tools/module_boundaries.json` | down only |
| Frontend size | lines in `frontend/**` | down after M4 |

Report the numbers. Don't turn them into a "maintainability score" (the old plan rightly rejects
that).

## Verification statements

At the end of each milestone, state what was verified (tests that fail without the change, artifacts
inspected) and what was not (hardware, A/V, real games, foreign overlays). Real-game, FG and A/V
evidence stays in the user's validation matrix from the old plan, "User hardware/application
validation", which is still valid.
