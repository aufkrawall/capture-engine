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

## Open questions (verify before the milestone that needs them)

| # | Question | Why it matters | How to verify | Needed by |
| --- | --- | --- | --- | --- |
| Q1 | With `--config=<other path>`, does the injected hook use the same settings? It reads `<hook dir>\config.ini` (`main_hookthread.cpp:95`); the inject child also publishes resolved config (`inject_config_publication.h`) | Decides whether the settings path can ever be configurable; may be an existing bug | Read the hook's local-vs-published precedence (`g_LocalConfigLoaded`, `main_redirect.cpp:208`); a test with divergent files | M5 |
| Q2 | Which COM apartment does controller-thread code need (screenshot/WIC, `InjectionManager` WMI, elevation client)? | The engine thread must initialize it correctly | grep `CoInitialize*` in `captureengine/**`, `common/**` | M3 commit 7 |
| Q3 | Does the media role's exit code distinguish successful finalization? Does the manifest know the output path(s)? How does the controller request id reach media? | RECORDING_FINALIZED design | Read `media_main_start_shutdown.cpp`, `recording_manifest.h`, `WriteRecordingManifest`, spawn arguments in `process_ipc_client.cpp` | M6 |
| Q4 | Several packages on one machine: HKCU Vulkan layer registration (resident, one path), elevation service (Program Files, one version), autostart Run value | A third-party app-local package could re-point the app's layer or meet a mismatched service | Read `vulkan_layer_registration.cpp` (staging dir `CaptureEngine\vulkan_layers`), elevation setup version checks; decide on last-writer-wins plus version guard, or a shared installed runtime | M8 |
| Q5 | What does `window_heartbeat` serve? | Whether it is engine (always on) or feature-gated | Read `common/platform/window_heartbeat*` and its wiki notes | M3 |
| Q6 | `ce::startup::Bootstrap(controller)` and `Setting::Elevation`: does the app relaunch itself elevated? How are autostart and the service coupled? | Split between frontend policy and the setup API | Read `captureengine/elevation/startup_*.cpp` | M9 (earlier if M7 needs the allowance removed) |
| Q7 | Replace the launcher filename heuristic and `Sleep(100)` in `LaunchGameSuspended` | Project rules (no sleeps; generic behavior); changes user-visible launch behavior | **User decision**; then tests | M3 or later, own commits |
| Q8 | Where does today's forced termination at shutdown live (graceful 10 s budget, then forced)? | DR-17 moves the policy to the frontend | Read `host_children.cpp` shutdown path and `ChildProcessLifecycle` | M3 |
| Q9 | Package build identity: does any binary expose a readable build identity (PE version resource, export)? The mediaengine loader has no visible identity check (grep found none) | `CE_E_PACKAGE` mismatch detection, DR-12 | Read `mediaengine_loader.cpp`, `.rc` files, `build_identity.h` | M7 |
| Q10 | **Self-exclusion:** how do the injector and screen-grab privacy recognize CE's own processes and windows? `screen_grab_privacy.cpp:141` matches `captureengine.exe` by name | A third-party client process hosts the runtime (and maybe the desktop overlay): CE could inject into it, or record its overlay windows | Read the injection exclusion policy (`injection_policy.h`, `injection_path_policy.h`) and privacy rules; replace names with PID-based identity published by the runtime | M7 (blocker for third parties) |
| Q11 | `log_level=none` today creates no `logs/` tree at all | The library must keep that guarantee for `data_dir` | Test: create with log_level none → no directory | M3 |
| Q12 | Redistribution obligations for third parties (LGPL FFmpeg corresponding source, licenses) | The SDK package must carry them | `build_corresponding_source.py`, license staging | M8 |
| Q13 | M3's initial mechanical moves turn 12 currently allowed includes into forbidden role/runtime edges; `controller_recording.cpp` and `hotkey_input_hook.cpp` still include `main_internal.h`, and `child_recording_stop.h` remains in the app | Moving files alone cannot satisfy the checked target boundaries; resolve the migration order before moving these units, without broad waivers | Simulate M3's source/target path mapping over the checked include graph; detach controller coupling through interfaces and recheck before committing moves | M3 commit 1 |

## Evidence log for this plan

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
