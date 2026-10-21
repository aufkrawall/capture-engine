# libcengine: library-first refactor plan

Status: **adopted** 2026-10-08 at implementation baseline `7ebebd43`; design written 2026-10-07.
It covers the library goal (D8/D9/D13) of `llm-wiki/architecture-debt-plan.md`, and reorders that
plan so the library is the critical path. The graphics/media debt waves continue as separate tracks.
This directory is the canonical library plan; the old temporary plan copy is removed.

## Goal

1. Third parties can use every CaptureEngine engine feature through one small, stable,
   versioned C API (`cengine.dll`, `include/cengine/cengine.h`).
2. The shipping CaptureEngine app (tray, hotkeys, CLI) is a client of that same API. It uses no
   private headers, no engine globals and no side channel.
3. Internally, the engine is a handful of deep modules (Ousterhout / Matt Pocock): each one has a
   small interface and hides a large policy and lifetime. Dependency rules are checked by a tool,
   not by convention.
4. (Phase 2, adopted 2026-10-20) No CaptureEngine process is required: the engine runs inside the
   client process by default; today's helper processes remain an opt-in isolation mode
   ([09-in-process-engine.md](09-in-process-engine.md)).
5. (Phase 3, planned 2026-10-21) The app returns to helper processes by default; third parties stay in-process,
   never need `captureengine.exe`, and get recording recovery, FFmpeg isolation and native sensors
   ([11-helper-isolation.md](11-helper-isolation.md)).

## Reading order

| File | Contents |
| --- | --- |
| [01-assessment.md](01-assessment.md) | Why the current plan is reshaped; what to keep from it |
| [02-target-architecture.md](02-target-architecture.md) | Binaries, processes, module map, dependency rules, threading, process-wide effects |
| [03-public-api.md](03-public-api.md) | Full C ABI specification, header draft, C++ wrapper, semantics, error model |
| [04-runtime-modules.md](04-runtime-modules.md) | Internal deep modules: interfaces, hidden policy, migration map from today's code |
| [05-frontend-and-role-host.md](05-frontend-and-role-host.md) | Turning `ControllerMain`/`WinMain` into a thin client plus a role host |
| [06-milestones.md](06-milestones.md) | Ordered milestones with commit series, acceptance criteria and gates |
| [07-testing-and-verification.md](07-testing-and-verification.md) | Test strategy through interfaces, ABI tests, headless client, depth metrics |
| [08-decisions-and-open-questions.md](08-decisions-and-open-questions.md) | Decision records (with rejected options) and open questions to verify |
| [09-in-process-engine.md](09-in-process-engine.md) | Phase 2: the engine in the client process by default, helper processes optional (M12-M19) |
| [10-phase2-audit.md](10-phase2-audit.md) | Phase 2 audit: role globals, process-wide effects, teardown, collisions, package paths (M12) |
| [11-helper-isolation.md](11-helper-isolation.md) | Phase 3 plan: app back to helper processes, in-process coverage, recording recovery, FFmpeg isolation, per-app audio, client-image helpers, native sensors (M20-M26) |

## Ground rules for implementing agents

- `CLAUDE.md` wins on gates, commits, changelog, secret scanning and project constraints. This plan
  never authorizes skipping a gate.
- Implement one milestone of [06-milestones.md](06-milestones.md) at a time, as the listed small
  commits. Mechanical moves and policy changes always go in separate commits.
- Facts marked **(verified 2026-10-07)** were read from source at `644219f2`. Facts marked
  **(verify)** must be checked against code before you rely on them. Code wins over this plan.
- Do not reopen a decision in `08-...` without new evidence. If evidence changes a decision, record
  it there (date, evidence, new decision) before writing code.
- Keep the status table below current at the end of every commit series. It is the only status
  record for this plan; don't add narrative status elsewhere in these files.
- Maintain these pages only in `llm-wiki/library/`; index.md routes here. D8/D9/D13 in
  architecture-debt-plan.md are superseded. Keep historical evidence in llm-wiki/log/, not a second plan.

## Status

| Milestone | State | Last commit | Notes |
| --- | --- | --- | --- |
| M0 Adopt plan, move docs into the wiki | complete | `a50d1600` | Canonical plan; old D8/D9/D13 superseded; historical evidence archived; links and changelog validated. |
| M1 Module boundary checker | complete | `4f584251` | 19 exact exceptions; 3645 edges; 23 boundary/gate fixtures (31 with lint tests). Lint and unfiltered close enforce rules; native/Python/ASan/32 FG checks and setup 0.1.7026 pass. Internal reach: DX12 74 (2 outside), media 29 (0 outside); runtime/frontend absent. |
| M2 Public header v2 + ABI tests (unshipped) | complete | `2f1d8acb` | 30 C declarations; 56 field offset/width checks and 8 size/alignment checks compile in C11/C++20. C object linked/executed; wrapper types compile; two ABI defects detected. Clean transaction and resumed verification pass with setup 0.1.7029. No runtime/export or MSVC-consumption claim. |
| M3 Runtime core inside the exe | complete | `cfc929a5` | Engine thread (`runtime/core`, DR-25..27) owns helpers, recording, hotkeys, desktop overlay, screenshots, launcher, settings store fan-out and platform setup. Shutdown never terminates helpers and waits out finalizing media (Q8, `DrainUntilExited`); destroy is safe after either shutdown signal (DR-29). The role host's per-process settings owner moved to `captureengine/app/role_configuration.*`. |
| M4 C ABI implemented; frontend on the API; v1 removed | complete | `cfc929a5` | 30 exported functions (lifecycle, status, commands, events, log, settings, setup, monitors). The frontend (`frontend/`) uses only `cengine.hpp` and parses its own command line; WinMain dispatches controller mode to it before the role prelude. `ce_runtime_create` claims the session (`SessionClaim`, `CE_E_BUSY`) and opens the diagnostics session (`DiagnosticsSession`). `settings_path` restores `--config=` (DR-28). |
| M5 Settings API | complete | `c653bb43` | Engine reads the transactional `SettingsStore`; `CommitSettings` command; `ce_settings_*` in `cengine.h`; WATCH_SETTINGS polls external edits, a refused edit is one SETTINGS_REJECTED. Open: clamped-vs-invalid severity split. |
| M6 Complete events (finalized output, screenshot paths, helpers) | complete | `30ea5af1` | RECORDING_FINALIZED from retired media exits (coded exit + result slot, DR-23), SCREENSHOT with paths, HELPER transitions. |
| M7 `cengine.dll` + WinMain split | complete (pending user validation) | `a3595d60` | `tools/build/build_cengine.py`: `runtime/**` + common + 46 DR-15 role-host units into `cengine.dll` with `runtime/api/cengine.def`; export table checked against `cengine.h`, DLL load/version check, PDB and PE hardening. Engine thread pins its module and exits via `FreeLibraryAndExitThread`. `captureengine.exe` = role host + frontend, delay-loads `cengine.dll` (helpers never load it); the build fails if a frontend object references `Log_*` or runtime internals. Pending: installer + app smoke on hardware, Steam/RTSS coexistence. |
| M8 SDK package + headless client + outside-repo proof | in progress | `cb9e24ac` | `sdk/` ships in the installer (headers, def, `libcengine.dll.a`, `cengine.lib`, CMake package `cengineConfig.cmake` with a header-derived version file, developer guide `sdk/README.md`, sample); `samples/headless` is built in the product build with CMake through `find_package(cengine)` against the staged SDK only. The outside-repo run is `python tools/run_sdk_outside_repo.py` (staged copy, renamed client, other working directory, two rounds with a finalized audio-only recording, kill-on-close job); it starts the real runtime and helpers, so it is user-driven and has not run yet. |
| M9 Setup API, monitors, client logging | complete | `f8a47608` | `ce_setup_request`/`ce_setup_get_status` (driver offer decided by the runtime), `ce_enumerate_monitors`, `ce_runtime_log`; tray and PawnIO prompts on the API; the sensor helper is held during driver removal. Open: `--list-monitors` still uses `common/platform` in WinMain; setup helpers locate files beside the running executable. |
| M10 Optional: role-host split (`cengine_host.exe`) | superseded | | Replaced by phase 2 (09): the isolated mode keeps the role-host executable; package paths resolve from `cengine.dll` (M15). |
| M11 Optional: recording statistics | not started | | Simpler once media runs in-process (M18). |
| M12 Phase 2: audit and design | complete | | Inventory in 10 (media 146 mutable items, mediaengine 52, logging one process sink, ~20 process-wide effects); DR-37..DR-46: one supervisor with process/thread transports, `RoleContext`, effect gating, thread-bound log sinks and media state, two-step mediaengine instance, loopback worker stays a process. |
| M13 Role-host seam + logger in-process | complete (pending user validation) | `e0cab027` | One supervisor, two transports (DR-37): in-memory channel (`c1c47ede`), thread runs with RoleContext/stop event/sink, transport choice at create; API 2.1 `CE_FEATURE_ISOLATED_HELPERS`/`PROCESS_TUNING`, `[Runtime] isolated_helpers` (`8ea366ab`); logger thread-capable (`ec23b5bb`), real logger on the thread transport tested (`test_logger_role_thread_host.cpp`). |
| M14 Per-role logging sinks | complete | `bc9b50db` | Thread-bound `ce::log::Sink` (DR-40, `2bd776fe`); role threads inherit the sink; mediaengine `DLL_Log` follows the calling pipeline's callback and is inherited by its threads (M16b). |
| M15 Inject role in-process | complete (pending user validation) | `5aae5044` | `RunInjectRole` from a RoleContext: package paths, effect gating (no `CoInitializeSecurity`, working-set trim or log level in a client), stop event, host registration, identity-based screen-grab rule; owner binding by thread + PID (DR-46). `InjectRoleThreadTest` drives the real role in thread mode (unit-test namespace, no injection); a restarted role prewarms again. |
| M16 `mediaengine` instances | complete | `bc9b50db` | M16a (`d657dacb`): state in `MediaEngineInstance`; M16b: `MediaEngine_Create`/`Destroy`, 39 handle-first exports, lock-free generation-checked handles, refcounted loader, per-pipeline log routing, worker = role host (DR-42, DR-43). Two pipelines tested in one process. |
| M17 Media role as an instance | complete (pending user validation) | `a69cd01e` | M17a (`2cebbaba`): `MediaSessionState` + thread binding (DR-41); M17b: `RunMediaRole` from a RoleContext, package paths, effect gating, priority restore, scoped DLL loading, teardown without process exit, process and GPU scheduling priority given back by the last session. `MediaRoleThreadTest` drives the real `RunMediaRole` on a thread (missing `mediaengine.dll`, refused start: exit code, handles closed, lines in the role's own sink). The loader's function-pointer table lives in `ce::mediaengine_api` because `unit_tests.exe` links mediaengine statically and same-named globals aliased its exports. A successful Init is not driven (a product `mediaengine.dll` fails the unit-test build identity). Refusals close the handles the role owns. |
| M18 Media in-process, default switch | complete (pending user validation) | `38e6c20b` | Role entries wired in the composition root; `cengine.dll` links the three roles; app uses `CE_FEATURE_PROCESS_TUNING`; output folders relative to the package (`369d1f7c`); tests isolated from a live CE (`d9486a21`). |
| M19 Phase 2 validation and SDK | in main, pending user retest | | User hardware matrix (09) in both modes; `tools/run_sdk_outside_repo.py --helpers both` (`fb50ab3b`). First run 2026-10-21 (0.1.7286): both modes recorded every round, but each audio-only stop was reported `canceled` (fixed, see log) and the script searched the deployment while the template recorded into the user's Videos; it now records into the deployment's `recordings` folder and the sample requires a file. Second run on 0.1.7287 passed in both modes. `--verify-runtime` passed except lint (fixed) and the integration `smoke_dx9` (NVIDIA driver-thread fault after the stop, open; log 2026-10-21). Flow scenarios fail by name while a foreign overlay (RTSS) injects into them. |
| M20 Core switch: app helper processes by default | not started (planned 2026-10-21) | | 11 part 1: `CE_FEATURE_ISOLATED_HELPERS` as the client's default, `[Runtime] isolated_helpers=auto\|on\|off`; SDK clients stay in-process. |
| M21 In-process coverage and diagnostics | not started (planned) | | 11 part 2: thread labels + crash role attribution, `[MediaTeardown]`/`[RuntimeTeardown]` residue, SDK run with video and overlap, integration matrix in both modes. |
| M22 Recording recovery | not started (planned) | | 11 part 3: 1 s clusters, in-flight journal, remux with exact track ends, fragmented MP4, FINALIZED + tray balloon. |
| M23 FFmpeg isolation | not started (planned) | | 11 part 4: third-party deps static, `--build-suffix=-ce`, import-name verification, decoy test. |
| M24 Per-app audio in-process | not started (planned) | | 11 part 5: owner-thread teardown inside the worker, silent reproducer, verdict; thread transport only on success. |
| M25 Client-image helpers | not started (planned) | | 11 part 6: role dispatcher in `cengine.dll`, `ce_role_host_main` + `CE_FEATURE_SELF_ROLE_HOST`; no `captureengine.exe` for third parties. |
| M26 Native sensors | not started (planned) | | 11 part 7: NVML/NVAPI, ADLX, IGCL, PawnIO CPU modules; `[HardwareSensors] backend=auto\|native\|lhm` with LHM fallback. |
