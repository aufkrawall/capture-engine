# 08 - Decisions and open questions

Format: decision, rejected options, rationale. Change a decision only with new evidence, and record
the date and the evidence here.

## Decisions

**DR-1 The runtime runs in-process in `cengine.dll`; helpers stay separate processes.**
Rejected: (a) the library as a remote control of a running CaptureEngine app or service. That makes
the tray app the engine, third parties depend on it, and the app couldn't be a client of its own
API. (b) One DLL containing everything. Injection, media isolation, the WoW64 hook split and the
privilege boundaries need processes.
Rationale: today's controller process becomes "whatever process loaded `cengine.dll`". That's the
smallest topology change, with no extra hop.

**DR-2 Replace the v1 facade; no compatibility shim.** v1 has no external consumers (verified
2026-10-07) and attach-only semantics. Rejected: a v1 shim over v2, which would be ABI to maintain
with no user.

**DR-3 Events are pulled; there are no callbacks.** Rejected: callback registration, which brings
reentry, client-thread affinity, delivery to destroyed clients and lock-order hazards. The pull model
plus a Win32 event handle fits both GUI loops and headless clients.

**DR-4 Commands are asynchronous, with exactly one completion event per accepted request.** Rejected:
synchronous commands (helper IPC can block up to seconds, e.g. the 5 s stop acknowledgement and the
10 s media readiness) and fire-and-forget (the client can't tell success).

**DR-5 Settings are addressed as INI section/key text.** Rejected: typed C structs per domain (a
huge, churning ABI that duplicates the loader), and a JSON/document API (a second parser and a
second source of truth). The INI keys are already the user-facing contract, and the production
loader is the single validator.

**DR-6 The settings file is the distribution medium, `<package_dir>\config.ini`.** Every helper and
the hook already read it, and `ReloadConfig` already exists. Rejected: pushing settings over IPC
(every reader would need a second path, and the hook loads config before any IPC exists).

**DR-7 One runtime per Windows session, reported as `CE_E_BUSY`.** Named objects are session-global
(verified). Rejected: per-instance name namespacing, which is a large ABI change across hook/host
for a feature nobody asked for (old plan: "arbitrary parallel instances" is a separate feature).

**DR-8 Global hotkeys and the desktop overlay are optional runtime features, not frontend code.**
Rejected: frontend ownership. That would make the frontend include `common/**` (duplicate statics),
and the status-dark capture protocol and screenshot bracketing would need client callbacks. As
runtime features, third parties get the RIDEV_NOHOTKEYS-safe hotkey logic and the desktop status
overlay for free.

**DR-9 `frontend/**` includes only `include/cengine/**`.** That enforces dogfooding and prevents
the duplicate-statics hazard. Frontend logging goes through `ce_runtime_log`.

**DR-10 The role host stays `captureengine.exe` until optional M10.** `RuntimePackagePaths` already
assumes it. The rename has many hazards (05).

**DR-11 The v2 runtime is x64 only.** Media, WGC and `mediaengine.dll` are x64 only; x86 is
hook/layer only (verified in `build_project.py`).

**DR-12 Package-private contracts are not public ABI.** `mediaengine.dll` exports, process IPC,
shared memory and role CLI ship and change together. Drop "preserve legacy positional exports" once
internal callers have migrated, behind a load-time build identity check.

**DR-13 Build the runtime in-exe first (M3-M6), then make it a DLL (M7).** This separates
behavioral extraction from binary/packaging risk. The C ABI and boundary rule apply from M4, so M7 is
mostly a link change.

**DR-14 Public structs use fixed-width integers instead of enum types.** Enum size is
compiler-dependent across C/C++ toolchains. Constants are `#define`s.

**DR-15 Code shared by the runtime and the role host (e.g. `InjectionManager`, screenshot capture,
`common/**`) is compiled into both.** That's acceptable because the controller process and helper
processes never share in-memory state. Revisit if a component needs cross-binary state in one
process (it must not).

**DR-16 `core` fans out settings and events through an explicit, fixed call list.** Rejected:
an observer/subscriber framework, which is indirection without a second producer.

**DR-17 The library never force-terminates helpers on its own.** A shutdown timeout is reported and
ownership is kept. Termination policy belongs to the frontend (the app may still do what it does
today).

**DR-18 Descriptor initialization takes the caller's extent and compiled API version.**
Evidence (2026-10-08, M2): the draft promised additive minor descriptor fields, but its initializer
took only a pointer; an older client's allocation size/version cannot be recovered from uninitialized
storage. The unshipped initializer now takes `struct_size` and `api_version`, never writes beyond
that extent, and validates the requested version. Rejected: assuming native sizeof, inferring a
version from buffer size, or requiring an uninitialized first field to carry the size. Other output
structs already require a seeded struct_size; monitor arrays carry item_size.

**DR-19 Detach existing controller dependencies before M3's mechanical moves.**
Evidence (2026-10-08): Q13's 12 new edges are existing couplings, including a keyboard hook that
imports main_internal.h solely for its delivery message. Extract such inputs/effects through small
component contracts in separate behavioral commits, then perform the fingerprint-proven moves.
Rejected: adding broad migration waivers or mixing policy changes into the mechanical move commit.

**DR-20 The recording owner reports presentation effects synchronously.** Evidence (2026-10-19,
`685f3a8f`): the pseudo overlay must learn the start intent before `EnsureMedia` spawns media, because
media's status-dark handshake runs during that spawn. A fixed single-implementer `RecordingObserver`
(start intent, requested changed, finalizing, integrity failure) is called at the exact old points.
Rejected: 04's `TakeEvents()` drain (reorders the intent after the spawn). Not an observer framework
(DR-16): one interface, one implementer.

**DR-21 Runtime subsystems never include each other.** Evidence (2026-10-19, `bfa0ddf9`): the move
exposed `controller_recording.cpp` -> `runtime/children` and `runtime/settings`. `core` composes;
a component takes `common/**` types or declares a small interface that a `core` adapter implements
(the children-backed `RecordingEffects` adapter belongs in `runtime/core`). 04 sketches such as
`RecordingController(ChildSupervisor&)` or `InjectControl(ChildSupervisor&)` are amended accordingly.
Role-host code shared with the runtime (DR-15: `InjectionManager`, `TakeScreenshot`, setup/PawnIO) is
included only through exact exceptions with milestone M7.

**DR-22 Transitional exact exceptions for controller files not yet moved.** Resolves Q13 (2026-10-19):
the move turned the controller's own includes (`captureengine/app/main_internal.h`, `main_recording.cpp`
-> `runtime/**`) into 7 forbidden edges. They are listed as exact pairs (milestone M3/M4) and disappear as
`ControllerMain` becomes the frontend. Rejected: wildcard waivers, or moving `WinMain`-adjacent files in
the mechanical commit.

**DR-23 Media finalization is reported by a coded exit code plus an inherited result slot.** Evidence
(Q3, 2026-10-19, `5fa910a1`..`f3bcd249`): media's exit code was always 0, the manifest knew no path, and
neither manifests nor logs exist with `log_level=none`. Exit codes keep 0 (nothing finalized) and 1
(init failure); a finalization is `0x4345` in the high half plus flags and status (saved/failed/
canceled). The per-child anonymous mapping (seqlock) carries up to 8 UTF-8 paths (never truncated;
omitted with a flag). A silent child resolves to "lost". The runtime must stamp the request id at
retirement and read exit code and slot before closing the handle. Rejected: the manifest (log-level
dependent), the health channel (the final path only exists after atomic publication), a result file
(location and cleanup).

**DR-24 The event queue lives in `runtime/core` and owns event memory.** `runtime/core` may include
`include/cengine/` (the wire format). `sequence` is assigned at delivery; the drop notice sits outside
the budget; reservations are taken at admission (2026-10-19, `a8bc877d`). During the in-exe phase
`CE_STATIC_LIBRARY` gives the C ABI plain linkage (`1c58426b`).

**DR-25 The engine owns its thread from M3; the app is its first client.** Evidence (2026-10-20,
`3d109fda`, `701b6618`): `runtime/core/Runtime` runs the controller loop on an STA engine thread (Q2);
`ControllerMain` keeps only tray, console handler, `--launch` and `--auto-record` and talks to the engine
through `Submit`/`Status`/`Events`. The v1 facade was deleted in M3 instead of M4 because it bound to the
controller thread, which no longer runs the engine (DR-2: no consumer). Rejected: keeping a transitional
`ServiceOnce()` on the UI thread (it would have needed a second hotkey owner and a re-entrant pump).

**DR-26 Hotkey presses during a spawn wait are deferred.** The old `PumpStartupMessages` dropped thread
messages (WM_HOTKEY, hook deliveries) while helpers spawned; executing them inside the wait would re-enter a
recording command. The engine's pump collects them and runs them after the current step.

**DR-27 Admission and completion rules of the engine.** Commands are admitted only in READY and never after
`RequestShutdown`; every admitted command gets exactly one COMMAND_COMPLETED, also when it is still queued at
shutdown (`CE_E_INVALID_STATE`, not executed). A command's RECORDING_STATE change is published before its
completion, so a client that sees the completion already sees the new state. Mode is the last start intent
(`pendingIntent` clears once live). RECORDING_STATE `failure`: 1 generic, 2 integrity (automation stops).

**DR-28 The client may name the settings file (`ce_runtime_desc_t.settings_path`).** Evidence (2026-10-20,
`a856c28c`): the frontend move left the runtime always reading `<package_dir>\config.ini`, which silently broke
the app's `--config=`. 03's "deferred until the hook can follow it" was the wrong trade: dropping an existing CLI
behavior is worse than documenting the open hook question (Q1). The path is UTF-8 and absolute, resolved like the
role host resolves `--config=`; the settings store and every helper use it. v2 was unshipped, so the 2.0
descriptor grew to 48 bytes instead of taking a minor bump.

**DR-29 Destroy is safe after either shutdown signal.** Evidence (2026-10-20, `ddcbab6a`, `17b7d7ea`,
`c173c3c6`): one fix set the stopped handle before STOPPED (a client destroying on the STOPPED event found the
engine unfinished), the next reversed it (a client destroying when `ce_runtime_shutdown` returned found the state
not yet STOPPED). The engine now publishes the STOPPED status, then sets the handle, then pushes the event.

**DR-30 The runtime owns the process session; the app owns nothing of it.** `ce_runtime_create` (production
assembly) claims the Windows session (`SessionClaim`, the old mutex name, no retry loop) and opens the
`DiagnosticsSession` (session directory, cleanup, manifest, runtime log, crash dump directories; the crash
handler and the WER purge only with `CE_FEATURE_CRASH_HANDLER`, which pins the module). WinMain dispatches
controller mode to `FrontendMain` before the role prelude; the role host keeps its own per-process settings owner
(`captureengine/app/role_configuration.*`, state machine in `common/config/configuration_state.h`). A package path
without an exact ANSI form is refused (`CE_E_PACKAGE`) instead of running with defaults; the app explains it.

**DR-31 The runtime decides the sensor-driver offer.** The frontend never re-parses settings (INI aliases live in
`common/config`): `ce_setup_status_t.sensor_driver_offer` comes from the runtime's applied
`HardwareSensorsConfig` with the predicate of `common/setup/sensor_offer_policy.h`; the frontend only prompts.

**DR-32 `cengine.dll` is linked from an explicit closure; the app delay-loads it.** `CENGINE_ROLE_HOST_SOURCES`
(`tools/build/build_cengine.py`) lists the 46 role-host units the runtime calls (DR-15), found by link closure; a
missing unit fails the link. `captureengine.exe` contains no `runtime/**` unit, links `libcengine.dll.a` and
delay-loads the DLL, so helper processes never map the runtime. The build fails when a frontend object references
`Log_*`, `ce::engine::`, `ce::runtime::` or `ce::capi::` (llvm-nm), and when the DLL's export table, load or
API version disagrees with `cengine.h`.

**DR-33 The engine runs in the client process by default; helper processes are an opt-in.** User
decision 2026-10-20: third parties must integrate CaptureEngine natively, without a CaptureEngine
process. `CE_FEATURE_ISOLATED_HELPERS` keeps today's helper processes for crash isolation; the app uses
the default, with `[Runtime] isolated_helpers=on` to opt in. Rejected: in-process only (loses crash
isolation for clients that want it); helpers only (phase 1's state; not a native library); a dedicated
`cengine_host.exe` (M10) as the answer (still a CaptureEngine process). Plan: 09.

**DR-34 Overlapping recordings stay possible in-process.** Recording N finalizes while N+1 starts; in one
process that needs instance state in the media role and in `mediaengine.dll` (handle-based API).
Rejected: serializing recordings in-process (a new recording would wait for the previous file).

**DR-35 Sensors stay out of process.** The LibreHardwareMonitor bridge hosts the .NET runtime and talks
to the PawnIO driver; loading that into a client process is intrusive. It remains a helper in both
modes (or off).

**DR-36 One role code base for both modes.** Role logic is written against a host interface (commands,
events, shared state, log sink); the process host is the role prelude around it, the thread host a
runtime component. The command protocol stays the same; the thread host uses an in-memory transport.

**DR-37 One supervisor, two transports.** Evidence (M12 audit, [10](10-phase2-audit.md)): the supervisor
is already split into `HostChildren` (runtime-facing, faked by the engine tests) and `ChildProcessEffects`
(OS seam under `ChildProcessLifecycle`). Phase 2 keeps both: `NativeHostChildren` chooses a transport per
role (process or thread); both tokens are waitable HANDLEs, so the lifecycle, retirement and
`MediaFinalizations` stay one code path. The role side of the command channel is `ce::role::RoleChannel`
(`ProcessIPCServer` or an in-memory peer), the host side `ce::role::HelperClient` (`ProcessIPCClient` or
the in-memory peer). `ProcessMessage` stays the unit and both ends validate it; depth one, as the pipe.
Rejected: a new RoleHost hierarchy over `HostChildren` (a pass-through layer), `TerminateThread`.

**DR-38 Roles run from a `RoleContext`, never from their process.** `int Run<Role>Role(ce::role::RoleContext&,
const AppConfig&)` (`captureengine/roles/role_entries.h`, an approved leaf for `runtime/children`). The
process host builds the context from argv in `<Role>ProcessMain`; the thread host builds it in the
runtime. The context carries the channel, stop event, controller PID, package directory, role-host path,
config path, recording id, media result slot and latency channel, the log sink, and `ownsProcess`. Roles
never read argv, `GetModuleFileName(NULL)` or the exe directory.

**DR-39 A thread-hosted role changes no process state it does not own.** Process host only: console
handlers, working-set trims, role crash handlers, `Log_Init`/`Log_Shutdown`/`Log_SetLevel`,
`CoInitializeSecurity`, `SetDefaultDllDirectories` (the thread host loads mediaengine and FFmpeg with
explicit `LOAD_LIBRARY_SEARCH_*` flags). Opt-in `CE_FEATURE_PROCESS_TUNING` (the app sets it): the settings'
process priority class and process GPU scheduling priority. Kept and documented: `timeBeginPeriod(1)` while
recording, per-thread MMCSS/priorities/COM, MF refcount, CE's FFmpeg log callback, one identity registration
by the runtime. Rejected: applying today's media defaults (`process_priority=high`) to any client.

**DR-40 Thread-bound log sinks.** `ce::log::Sink` (a file with today's name) bound to a thread by
`SinkScope`; unbound threads write the process sink (the runtime log). The level stays process-wide and
runtime-owned. Threads a role starts inherit the sink at creation; mediaengine routes `DLL_Log` through a
per-instance callback with a thread-bound current instance. Rejected: a sink parameter on ~1000 call sites.

**DR-41 Media state lives in `MediaSessionState`, reached through a thread binding.** The 65 globals, the
semantic statics, the source-loss latch and the recording id move into one state object per media session.
Every media thread entry (role loop, encoder, WGC, inject capture, DXGI duplication, WinRT callbacks)
installs a `MediaSessionBinding`; an unbound access fails fast. The rename is mechanical; explicit
references follow where the code is already a member. Rejected: threading a reference through ~1500 uses
in free functions in one step; serializing recordings (DR-34).

**DR-42 mediaengine becomes an instance in two steps.** M16a moves the state into the `MediaEngine`
instance behind unchanged exports; M16b adds `MediaEngine_Create`/`Destroy` and a handle on 40 exports.
`SetLogCallback`, `GetBuildIdentity`, `RunProcessLoopbackWorker` and `MeasureRenderEndpointLatency` stay
process-wide. One D3D11 device per instance: the same cost as two media processes today.

**DR-43 The per-app audio worker stays a process in both modes.** Evidence: [10](10-phase2-audit.md)
"Per-app audio worker" (AudioSes `CLoopbackMixer` teardown crash; commits `5fdc11df`, `4e1f30ed`,
`9ff8b41d`). The worker is the package's role host, never the client image. Consequence: an in-process
client that records application audio still starts `captureengine.exe` workers.

**DR-44 A stalled role thread is retained and reported, never killed.** Same policy as a live hung helper
process today: a missed state-changing reply breaks its channel (HELPER lost), the identity is retained,
shutdown waits for it and the client's shutdown timeout is the only bound. A crash in a role thread crashes
the client; `CE_FEATURE_ISOLATED_HELPERS` is the isolation answer.

**DR-45 Transport per role, decided at create.** A role runs as a thread unless the runtime is isolated
(`CE_FEATURE_ISOLATED_HELPERS`, or `[Runtime] isolated_helpers=on` read by the runtime at create) or the role
is not thread-capable yet (logger from M13, inject from M15, media from M18; sensors never, DR-35).
Rejected: the frontend reading the setting (DR-31).

**DR-46 Inject control binds an owner handle and PID.** `InjectControlChannel` checks the discovery PID
against the owner it was bound to: the inject process, or in-process the inject role thread's handle and
the client PID. No shared-memory ABI change; fixed names stay valid under one runtime per session (DR-7).

**DR-47 The default switch is one step.** Evidence (2026-10-20): wiring each role as it became thread-capable
(logger in M13, inject in M15) would have moved the app's validated helper processes in-process piecemeal, before
media could follow. The role entries are wired together in M18 (`38e6c20b`); until then every role ran as a process.
Rejected: per-role default switches; a transitional opt-in setting (one more mode to validate).

**DR-48 Test builds name session-wide objects in their own namespace.** Evidence (2026-10-20): desktop-overlay unit
tests read a running CaptureEngine's discovery and recording state; logger tests had to skip while one ran.
`common/ipc/shared_defs_detail/session_namespace.h` spells every session-wide name `Local\CE_` (product),
`Local\CE_UT_` (unit tests) or `Local\CE_FG_` (FG flow tests); product spellings are pinned by tests. FG flow
scenarios report a foreign overlay or second CE hook loading into their game by name (RTSS's `RTSSHooks64.dll`
caused the four flow failures of the 2026-10-20 gate), and every scenario runs on a private desktop. The per-user
registry state the product writes follows the same rule: the Vulkan layer target list, the Khronos implicit-layer
registration and plain autostart entries live below `Software\CaptureEngine_UT` in unit tests (before, tests wrote a
fake layer into the real loader key and a real logon entry). Rejected: skipping tests while CaptureEngine runs.

## Open questions (verify before the milestone that needs them)

| # | Question | Why it matters | How to verify | Needed by |
| --- | --- | --- | --- | --- |
| Q1 | With `--config=<other path>`, does the injected hook use the same settings? It reads `<hook dir>\config.ini` (`main_hookthread.cpp:95`); the inject child also publishes resolved config (`inject_config_publication.h`) | Decides whether the settings path can ever be configurable; may be an existing bug | Read the hook's local-vs-published precedence (`g_LocalConfigLoaded`, `main_redirect.cpp:208`); a test with divergent files | M5 |
| Q2 | Which COM apartment does controller-thread code need (screenshot/WIC, `InjectionManager` WMI, elevation client)? | The engine thread must initialize it correctly | grep `CoInitialize*` in `captureengine/**`, `common/**` **Answered 2026-10-19: the controller thread has no apartment today; every COM user scopes its own. Engine thread: STA + OLE1DDE disabled, own queue, owns RegisterHotKey and the hook target, pumps continuously; tray stays on the UI thread.** | M3 commit 7 |
| Q3 | Does the media role's exit code distinguish successful finalization? Does the manifest know the output path(s)? How does the controller request id reach media? | RECORDING_FINALIZED design | Read `media_main_start_shutdown.cpp`, `recording_manifest.h`, `WriteRecordingManifest`, spawn arguments in `process_ipc_client.cpp` **Answered 2026-10-19: no (always 0); manifest had no path; `--recording-id` is the diagnostic serial, not the request. See DR-23.** | M6 |
| Q4 | Several packages on one machine: HKCU Vulkan layer registration (resident, one path), elevation service (Program Files, one version), autostart Run value | A third-party app-local package could re-point the app's layer or meet a mismatched service | Read `vulkan_layer_registration.cpp` (staging dir `CaptureEngine\vulkan_layers`), elevation setup version checks; decide on last-writer-wins plus version guard, or a shared installed runtime | M8 |
| Q5 | What does `window_heartbeat` serve? | Whether it is engine (always on) or feature-gated | Read `common/platform/window_heartbeat*` and its wiki notes **Answered 2026-10-19: opt-in per profile (default off), experimental, only `ControllerMain` used it; optional lazy runtime component.** | M3 |
| Q6 | `ce::startup::Bootstrap(controller)` and `Setting::Elevation`: does the app relaunch itself elevated? How are autostart and the service coupled? | Split between frontend policy and the setup API | Read `captureengine/elevation/startup_*.cpp` **Answered 2026-10-19 (`14ab682d`): Bootstrap relaunches elevated (runas, authenticated context pipe); one preference record {service, elevated, autostart}. Setup actions are now separately requestable; remaining M9 gaps: helper/staging assume the exe dir, PawnIO has dialogs and no result, launch-elevated stays app policy.** | M9 (earlier if M7 needs the allowance removed) |
| Q7 | Replace the launcher filename heuristic and `Sleep(100)` in `LaunchGameSuspended` | Project rules (no sleeps; generic behavior); changes user-visible launch behavior | **User decision**; then tests **2026-10-20: `Sleep(100)` removed (a remote LoadLibrary thread serializes behind loader initialization; `84e8d613`); the heuristic is isolated in `LooksLikeGameExecutable` (`113db273`) and stays a user decision; note it treats `EpicGamesLauncher.exe` as a game (bare `game` marker).** | M3 or later, own commits |
| Q8 | Where does today's forced termination at shutdown live (graceful 10 s budget, then forced)? | DR-17 moves the policy to the frontend | Read `host_children.cpp` shutdown path and `ChildProcessLifecycle` **Answered 2026-10-19: `NativeHostChildren::Shutdown` (1 s ack, 10 s shared drain, then TerminateProcess). Retired finalizing media get only 10 s against media's 60 s encoder join: app exit can truncate a recording (fix scheduled in M3). Fixed 2026-10-20: `ChildProcessLifecycle::DrainUntilExited` waits for every helper without a deadline and never terminates (progress line every 10 s); the client's `ce_runtime_shutdown` timeout is the only bound.** | M3 |
| Q9 | Package build identity: does any binary expose a readable build identity (PE version resource, export)? The mediaengine loader has no visible identity check (grep found none) | `CE_E_PACKAGE` mismatch detection, DR-12 | Read `mediaengine_loader.cpp`, `.rc` files, `build_identity.h` | M7 |
| Q10 | **Self-exclusion:** how do the injector and screen-grab privacy recognize CE's own processes and windows? `screen_grab_privacy.cpp:141` matches `captureengine.exe` by name | A third-party client process hosts the runtime (and maybe the desktop overlay): CE could inject into it, or record its overlay windows | Read the injection exclusion policy (`injection_policy.h`, `injection_path_policy.h`) and privacy rules; replace names with PID-based identity published by the runtime | M7 (blocker for third parties) |
| Q11 | `log_level=none` today creates no `logs/` tree at all | The library must keep that guarantee for `data_dir` | Test: create with log_level none → no directory **Answered 2026-10-19: true for every role at startup. Residual creators: inject child crash handler, WER adoption, hot reload to a logging level, the hook DLL and the Vulkan layer.** | M3 |
| Q12 | Redistribution obligations for third parties (LGPL FFmpeg corresponding source, licenses) | The SDK package must carry them | `build_corresponding_source.py`, license staging | M8 |
| Q13 | M3's initial mechanical moves now turn 8 currently allowed includes into forbidden role/runtime edges; `controller_recording.cpp` still includes `main_internal.h`; the stop-command adapter/result now live in common IPC | Moving files alone cannot satisfy the checked target boundaries; resolve the migration order before moving these units, without broad waivers | Simulate M3's source/target path mapping over the checked include graph; detach controller coupling through interfaces and recheck before committing moves **Resolved 2026-10-19 by DR-19/DR-22 (7 exact transitional exceptions; 2 runtime-sideways edges removed with the core composition).** | M3 commit 1 |

## Evidence log for this plan

- 2026-10-20, M12 (phase 2 audit at `07b7853d`): six parallel read-only audits (media globals, media
  effects/teardown, mediaengine, inject and hooks, logging/logger/prelude, transport/supervisor/loopback).
  Inventory and evidence in [10](10-phase2-audit.md); DR-37..DR-46 above.

- 2026-10-20, rounds 2-6 (`c08a7f45`..`cb9e24ac`, 104 commits on the orchestrator branch): M3, M4, M5, M6, M9
  complete; M7 complete pending hardware validation; M8 packaged without the outside-repo run. DR-28..32 above.
  Closing gate at `a3595d60` with 12 build jobs: native suite, Python self-tests, 150 FG flow scenarios, setup
  0.1.7288. Boundary exceptions 21 (the frontend has none).

- 2026-10-19, round 1 of the parallel execution (`c08a7f45`..`16711090`): M3 commits 0-1, M4 foundations
  (event queue, status/version/descriptor entry points, handle registry), M5 groundwork in progress, M6
  media side, M7 build-identity/self-exclusion groundwork in progress, M9 setup-action split and monitor
  enumeration, D12 boundary hygiene (exceptions 20 -> 16 before the M3 transitional ones). Q2, Q3, Q5,
  Q6, Q8, Q11, Q13 answered above.

- 2026-10-08, after `f76aa68e`: the M3 mechanical path mapping over the 3645-edge graph produces
  12 new forbidden edges, including controller recording/hotkey accesses to main_internal.h.
  Q13 records this staging constraint; the boundary rules are not relaxed to hide the coupling.

- 2026-10-07, at `644219f2`: initial facts from `include/libcaptureengine.h`,
  `captureengine/app/{libcaptureengine*.cpp,main_entry.cpp,main_controller.cpp,main_recording.cpp,
  main_internal.h,controller_recording.cpp,host_children.h,runtime_configuration.h,recording_session.h,
  hotkey_input_hook.h,status_overlay_sync.h}`, `common/platform/runtime_package_paths.h`,
  `common/ipc/{process_ipc.h,inject_control_channel.h,shared_defs_detail/abi_constants_and_config.h}`,
  `hook/runtime/main_hookthread.cpp`, `mediaengine/engine/mediaengine.h`, `llm-wiki/repo-map.md`,
  `llm-wiki/library-delivery.md`. The working tree had uncommitted runtime-package-path work from
  another session. These docs don't depend on it beyond `--config=` propagation.
