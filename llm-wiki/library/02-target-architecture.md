# 02 - Target architecture

## Goals and non-goals

Goals:
- `cengine.dll` contains the **runtime**: what `ControllerMain` does today minus tray, CLI and other
  app UX. Clients drive it through `include/cengine/cengine.h` only.
- The CaptureEngine app is a thin client: tray, CLI, process-level UX.
- Helper processes, injected DLLs, Vulkan layers and the elevation service keep their topology.
- A small, explicit set of internal modules with checked dependency rules.

Non-goals (separate features, not part of this plan): several runtimes running at once, preview or
packet output to clients, a plugin framework, a non-Windows build, x86 runtime clients, and new
capture capabilities.

## Today **(verified 2026-10-07)**

```
captureengine.exe  (one binary, role chosen by command line; x64 only)
 ├─ controller role: WinMain prelude → ControllerMain
 │    tray, hotkeys, pseudo overlay, settings, children, recording, screenshot,
 │    Vulkan residency, window heartbeat, startup/elevation/PawnIO, C facade (v1, attach-only)
 ├─ inject role   (InjectProcessMain: discovery via WMI/poll, injection, config publication)
 ├─ media role    (MediaProcessMain: WGC/inject capture, loads mediaengine.dll)
 ├─ logger role, sensors role, setup roles, dump helper, loopback worker, sensor bridge, PawnIO setup
mediaengine.dll          (x64, loaded by the media role; package-private exports)
capture_hook_{x64,x86}.dll, VK_LAYER_CE_{overlay,gate}[_x86].dll  (injected; read <dir>\config.ini)
captureengine_elevation_service.exe
```

The x86 builds cover only the hook and Vulkan layer binaries (`tools/build/build_project.py`).

## Target

```
Client process (CaptureEngine app OR a third-party program, x64)
 ├─ client code ──(include/cengine/cengine.h, cengine.hpp)──┐
 └─ cengine.dll  (runtime)                                  │
      api/  C ABI shim  ◄───────────────────────────────────┘
      core/ Runtime: engine thread, commands, events, status, lifecycle
      settings/ children/ recording/ inject/ screenshot/ desktop/ platform/ package/
           │ spawns + authenticated private IPC (unchanged protocol)
           ▼
captureengine.exe (role host; M10 may rename it cengine_host.exe)
   inject / media / logger / sensors / setup / helper roles (unchanged)
           │ shared memory + named objects (unchanged, package-private)
           ▼
games: capture_hook_*.dll, Vulkan layers
```

In the app, client code and `cengine.dll` share one process. That process is today's "controller
process". No new process and no extra IPC hop.

### Binaries

| Binary | Contains | Built for | Public? |
| --- | --- | --- | --- |
| `cengine.dll` + `cengine.lib` / `libcengine.dll.a` + `cengine.def` | runtime (`runtime/**`) + needed `common/**` | x64 | **Yes**: the only public ABI |
| `captureengine.exe` | frontend (`frontend/**`) + role host (`captureengine/**` roles) | x64 | No (the app; role CLI is package-private) |
| `mediaengine.dll` | `mediaengine/**` | x64 | No (package-private, version-locked) |
| hooks, layers, elevation service, installer | unchanged | as today | No |

Until M10, the role host stays inside `captureengine.exe`. `RuntimePackagePaths::FromModule` already
resolves `captureengine.exe` next to a DLL module **(verified 2026-10-07,
`common/platform/runtime_package_paths.cpp:123`)**.

### Public versus package-private contracts

| Contract | Classification | Rule |
| --- | --- | --- |
| `cengine.h` C ABI, `cengine.def` exports | Public | Semver on `CE_API_VERSION`; additive minor changes only; layout tests |
| `cengine.hpp` C++ wrapper | Public, header-only | Only calls the C ABI; never exposes STL across the DLL boundary |
| Role host command line (`--mode=`, `--config=`, `--session-dir=` ...) | Package-private | Version-locked to the same build; free to change |
| Process IPC (`ProcessMessage`), inject control channel, shared memory, discovery | Package-private | Same build only; keep the existing ABI/identity checks |
| `mediaengine.dll` exports | Package-private | Same build only; delete legacy exports when no internal caller remains (M7 adds a load-time build identity check first) |
| `config.ini` keys and sections | **Public data contract** | The settings API addresses them by section/key; renames need aliases (as today) |

## Module map (target)

New top-level modules follow the repo-map convention `<module>/<subsystem>/`, with unique basenames
per module.

| Module | Responsibility (what it hides) | Interface size | Thread | Built from today's |
| --- | --- | --- | --- | --- |
| `include/cengine/` | Public C ABI + C++ wrapper | ~35 functions | any | `include/libcaptureengine.h` (replaced) |
| `runtime/api/` | Handle registry, struct_size/version checks, UTF-8 conversion, exception → status, event memory | none beyond the C ABI | caller | `libcaptureengine.cpp` (replaced) |
| `runtime/core/` | Engine thread, command queue, event queue, status board, startup/shutdown order, settings fan-out, session ownership claim | `Runtime`: Create/Submit/Status/Events/RequestShutdown/WaitStopped | engine | `ControllerMain`, `CompleteControllerStartup`, `main_internal.h` globals |
| `runtime/package/` | Package layout discovery + validation (role host, mediaengine, hooks, layers, licenses, identity) | `PackageLayout::Resolve(options) → layout or error` | engine (create) | `RuntimePackagePaths` (extended) |
| `runtime/settings/` | INI document ownership: load, coherent replacement, debounce, validation diagnostics, transactional edits, atomic write, revision | `SettingsStore`: Current/Revision/Poll/Commit/Value/NextPollDelay | engine (+ pure validation on any thread) | `runtime_configuration.*`, `configuration_state.h`, `common/config` loader |
| `runtime/children/` | Helper process spawn/auth/readiness/health/retire/shutdown, active vs retired media finalizers | `ChildSupervisor` instance methods | engine | `host_children.*`, `child_process_lifecycle.h` |
| `runtime/recording/` | Recording request/state machine, live/failure reconciliation, notices | `RecordingController`: Start/Stop/Toggle/Service/State | engine | `recording_session.*`, `controller_recording.*` |
| `runtime/inject/` | Overlay/benchmark toggles, in-game notifications, recording intent, health reads, target launch | `InjectControl`, `TargetLauncher` | engine | `main_recording.cpp` toggles, `libcaptureengine_controller.cpp`, `main_controller.cpp` launcher |
| `runtime/screenshot/` | Screenshot orchestration: desktop overlay hiding, capture, in-game notification, result paths | `Screenshotter::Take() → result` | engine | `TakeControllerScreenshot`, `captureengine/media/screenshot*` (controller side) |
| `runtime/desktop/` | Optional desktop status overlay and optional global hotkeys (RegisterHotKey + low-level hook thread) | `DesktopOverlay`, `GlobalHotkeys` (each: Start/Apply(settings)/Stop) | own threads + engine | `captureengine/pseudo_overlay/**`, `hotkey_input_hook.*`, hotkey registration in `ControllerMain` |
| `runtime/platform/` | Diagnostics session (log dirs, manifests, cleanup, optional crash handler), Vulkan residency, window heartbeat, setup actions (PawnIO, elevation service, startup preferences) | small per component | engine | `WinMain` prelude, `main_vulkan_residency.h`, `window_heartbeat`, `captureengine/elevation`, `captureengine/sensors/pawnio*` |
| `frontend/` | App UX only: tray, CLI flags, single-instance message box, restart wait, console handler, auto-record script, launch-at-startup | `FrontendMain()` | UI thread | rest of `ControllerMain`, `tray.*`, `WinMain` controller branch |
| `captureengine/` | Role host: inject, media, logger, sensors, setup and helper roles | `RoleMain(mode)` | per process | unchanged |

Rules for these modules:
- One public header per module (`<module>/<subsystem>/<name>.h`). Internal splits use the existing
  `<stem>_internal.h` convention and are never included from outside the subsystem.
- No module exports a mutable reference, a lock, a global or a raw child handle.
- The engine thread owns all runtime components. Nothing outside `runtime/core` calls component
  methods from another thread, except documented pure functions such as settings validation.
- Components don't call each other sideways. `core` composes them and passes data (settings
  snapshots, observations) explicitly. Example: `RecordingController` doesn't know about
  `DesktopOverlay`; `core` forwards recording state to both the overlay and the event queue.

## Dependency rules (enforced by `tools/check_module_boundaries.py`, M1)

Allowed include edges (directory prefix → may include):

| From | May include |
| --- | --- |
| `frontend/**` | `include/cengine/**`, `frontend/**`, system/std headers. **Nothing else**, not even `common/**` (avoids a second copy of process-global state; see "Duplicate statics" below) |
| `runtime/api/**` | `include/cengine/**`, `runtime/core/core.h` (public header only), `common/logging/**`, `common/platform/**` |
| `runtime/core/**` | public headers of all `runtime/*` subsystems, `common/**` |
| `runtime/<x>/**` (other subsystems) | own subsystem, `common/**`, approved leaf code listed in the checker config (e.g. `captureengine/media/screenshot.h` until moved) |
| `captureengine/**` (role host) | `captureengine/**`, `common/**`, `mediaengine/engine/mediaengine.h`; never `runtime/**` or `frontend/**` |
| `hook/**` | `hook/**`, `common/**`; never `captureengine/**`, `runtime/**`, `mediaengine/**` |
| `mediaengine/**` | `mediaengine/**`, `common/**` |
| `common/**` | `common/**` only |
| `tests/**` | anything (tests prefer public headers; see 07) |

The checker reads `#include "..."` lines (repo-root-relative per repo-map), resolves same-directory
includes, and fails on edges not in the table. It also reports the includer count per `*_internal.h`
(the depth metric). Exceptions are listed explicitly in `tools/module_boundaries.json`, each with a
reason and a target milestone, and the exception list only shrinks (same ratchet idea as the
clang-tidy baseline).

## Threading model

| Thread | Owner | Work |
| --- | --- | --- |
| Client threads | client | Call `ce_*` functions. They never run engine work; commands are queued |
| Engine thread | `runtime/core` | Command dispatch, message queue (startup messages, `WM_HOTKEY`), helper service, recording reconciliation, settings poll and fan-out, status publication. Today's controller loop body |
| Hotkey hook thread | `runtime/desktop` | Low-level keyboard hook only; posts to the engine thread (exists today, `hotkey_input_hook.cpp`) |
| Desktop overlay UI and status-sync threads | `runtime/desktop` | Exist today (`pseudo_overlay_thread.cpp`, `pseudo_overlay_sync.cpp`) |
| Setup workers | `runtime/platform` | PawnIO / elevation operations (exist today as async workers) |

Rules:
- No callbacks into client code, ever. Results reach the client only as events and status.
- `ce_*` calls are thread-safe. Only `ce_runtime_shutdown` and `ce_runtime_next_event` may block, and
  both are bounded by a timeout.
- The engine thread may block on helper IPC (bounded, as today). That delays command dispatch, not
  client threads. `MainThreadBlockTimer` diagnostics stay.
- The engine thread initializes COM for itself (apartment chosen per current controller-thread
  needs, **(verify)** which apartment screenshot/WIC/WMI-launcher code expects). No COM interface is
  cached across calls (memory note).
- Module lifetime: `ce_runtime_create` pins `cengine.dll` (`GetModuleHandleExW` with
  `GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS`). The engine thread exits through
  `FreeLibraryAndExitThread` after joining every other runtime thread, so a client's `FreeLibrary`
  can never unload running code.

## Ownership limits (honest single instance)

**(verified 2026-10-07)** The session-wide named objects make the runtime a per-Windows-session
singleton:
- `Local\CaptureEngine_Instance_Mutex` (controller single-instance)
- `Local\CE_Disc_70` discovery mapping and `Local\CE_SM_70_*` (hooks in games find *the* inject host)
- per-target `Local\CE_Inject*`/`CE_Vulkan*` objects, and PID-keyed `CE_Shutdown_`, `CE_StatusSync_`,
  `CE_StatusDark_`, `CE_InjectFrame_`

Contract: at most one runtime per Windows session. If the CaptureEngine app (or any other client)
already owns it, `ce_runtime_create` returns `CE_E_BUSY`. Within one process, a second `create` while
one exists returns `CE_E_BUSY`. Destroy followed by a new create is supported and tested.

Machine/user-wide state shared by every package on the machine: the HKCU Vulkan layer
registration (resident, points to one package), the elevation service (Program Files, one version),
and the autostart Run value. If two packages exist on one machine (the app plus a third party's
app-local copy), the last runtime to start re-registers its own layer path. See
[08](08-decisions-and-open-questions.md) Q4.

## Process-wide effects: who owns them

`WinMain` applies these to the controller process today **(verified 2026-10-07)**. A DLL must not
impose them on an unrelated client process silently.

| Effect today | Target owner | Library behavior |
| --- | --- | --- |
| Setup/bootstrap roles (`ce::startup::TryRunSetup`, `Bootstrap`), dump helper, workers | role host (`WinMain`) | never in the client process |
| `PrimeStartupCursor` | frontend | none |
| Config load before logging | runtime (`SettingsStore`) | at create |
| Session dir naming + `CleanupOldSessionDirs` + session manifest | runtime (`DiagnosticsSession`) | under `data_dir` |
| `Log_Init` for the controller log | runtime | yes: logging is process-global, which is fine with one runtime per process |
| `InstallCrashHandler`, symbol store, WER purge | runtime, **opt-in** | only with `CE_FEATURE_CRASH_HANDLER` (the app sets it) |
| Power throttling opt-out (`SetProcessInformation`) | frontend for its own process; role host for children | none in client process |
| Single-instance mutex | runtime (claim) + frontend (message box) | `CE_E_BUSY` |
| `--restart-from-pid` wait | frontend | none |
| `SetConsoleCtrlHandler` | frontend | none |
| Vulkan layer residency | runtime | yes (required for Vulkan capture) |
| Window heartbeat | runtime | yes **(verify)** what it serves; profiles suggest capture targeting |
| Hotkey registration / keyboard hook | runtime, **opt-in** | only with `CE_FEATURE_GLOBAL_HOTKEYS` |
| Pseudo (desktop) overlay | runtime, **opt-in** | only with `CE_FEATURE_DESKTOP_OVERLAY`; settings still gate it |
| Tray icon | frontend | none |

## Duplicate statics: the main DLL hazard

`common/**` is statically linked into each binary. Once `cengine.dll` exists, the app process
holds two copies of every `common` global (logger, config accessors, IPC helpers): one in the exe and
one in the DLL. Code in the exe that calls `LogInfo` writes through the exe's uninitialized logger.
Two rules prevent this:
1. `frontend/**` includes nothing from `common/**` (checker rule). Frontend diagnostics go through
   `ce_runtime_log` into the runtime's log.
2. In controller mode, `WinMain` dispatches to `FrontendMain` before any `common` initialization.
   The role prelude (config, logs, crash handler) runs only for role modes. See 05.

## Configuration flow (unchanged medium, new owner)

**(verified 2026-10-07)** Every process reads the same INI file itself:
- role children get `--config=<path>` (`process_ipc_client.cpp:320`) and load it in `WinMain`
- the injected hook reads `<hook dll dir>\config.ini` (`hook/runtime/main_hookthread.cpp:95`) and
  also receives resolved config published by the inject child (`inject_config_publication.h`)
- `ProcessCommand::ReloadConfig` tells children to re-read it

So the file stays the distribution medium, and the runtime owns it: programmatic edits are
validated, written atomically and then broadcast with `ReloadConfig`. This reuses every existing
reader, with no new IPC payload and no second config source.

Consequence: settings must live where the hook looks, `<package_dir>\config.ini`, unless the hook
learns another path. Today, `--config=<elsewhere>` probably diverges from what the hook reads; see
Q1 in 08.

## Depth metrics (track per milestone, in 07)

- public functions per module header versus lines hidden behind it
- includers of each `*_internal.h` (should only be same-subsystem files)
- writable globals in `runtime/**` and `frontend/**` (target: 0 outside `runtime/core` statics
  documented as process singletons)
- boundary exceptions count (ratchet down)
