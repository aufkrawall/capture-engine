# 05 - Frontend and role host

## Target shape of `captureengine.exe`

```
WinMain
 ├─ role dispatch (unchanged roles, unchanged order):
 │    TryRunSetup → loopback worker → sensor bridge → PawnIO setup → dump helper →
 │    service integration/fixture → ParseProcessMode(...)
 │    if mode != Controller: RolePrelude(mode) → InjectProcessMain / MediaProcessMain /
 │                           LoggerProcessMain / SensorProcessMain
 └─ controller mode → FrontendMain(hInstance)      // no common/** initialization here
```

`RolePrelude` is today's prelude (package paths, config load, session dir from `--session-dir`,
logging, crash handler, manifests, power throttling, crash dir) **minus** the controller-only parts.
Controller-only parts move into the runtime (`DiagnosticsSession`, `SessionClaim`) or the frontend.

From M7, `FrontendMain` lives in `frontend/` and includes only `include/cengine/**`. Until M7, the
runtime is a static part of the exe, but the same include rule already applies from M4.

## `FrontendMain` responsibilities (complete list)

| Responsibility | Implementation via API | Source today |
| --- | --- | --- |
| Parse CLI: `--launch`, `--auto-record=D,T`, `--restart-from-pid=`, `--list-monitors`, `--license`, `--config=` | frontend parser (keep accepted syntax byte-compatible; Steam `%command%` form) | `WinMain` |
| `--list-monitors` | `ce_enumerate_monitors` → stdout (M9; until then the transitional allowance) | `WriteMonitorListToStandardOutput` |
| `--license` | `ShellExecute(<package>\LICENSE.txt)` | `WinMain` |
| Restart: wait for prior PID (5 s, then terminate) | frontend, before `ce_runtime_create` | `WinMain` |
| Cursor priming, power throttling, console ctrl handler | frontend (its own process) | `WinMain`/`ControllerMain` |
| Create runtime with `GLOBAL_HOTKEYS | DESKTOP_OVERLAY | CRASH_HANDLER | WATCH_SETTINGS` | `ce_runtime_create` | `ControllerMain` |
| "Already running" message box | on `CE_E_BUSY` | `WinMain` mutex loop |
| Tray icon, menu, shutdown animation | tray; state from events | `tray.*` |
| Tray: Quit | `ce_runtime_request_shutdown` | `onQuit` |
| Tray: Open config | `ShellExecute` on `<package>\config.ini` | `onOpenConfig` |
| Tray: PawnIO install/uninstall/status | `ce_setup_request` / `ce_setup_get_status` (M9) | `onInstallPawnIo` ... |
| Tray: startup toggles, service status | `ce_setup_*` (M9) | `ce::startup::*` |
| Tray recording icon | `CE_EVENT_RECORDING_STATE` | `main_g_Tray->SetRecordingState` from recording effects |
| PawnIO install offer after startup | after READY, if `ce_setup_get_status` says absent and settings enable sensors | `OfferInstallationAsync` |
| `--launch <cmd>` | after READY: `ce_target_launch` | `main_g_DeferredLaunchPath` |
| `--auto-record` | frontend timer: after READY+delay `ce_recording_toggle(VIDEO)`; after duration toggle again, then `request_shutdown`; stop on an integrity failure | `main_g_AutoRecord*` |
| Final shutdown budget / forced termination policy | `ce_runtime_shutdown(budget)`; on timeout, log and exit (helpers kill themselves via the shutdown event as today) **(verify)** today's forced path | `ShutdownHostChildren` |

Not in the frontend: hotkeys, desktop overlay, screenshot bracketing, settings reload effects,
helper service. All runtime-owned.

## Frontend loop

```cpp
cengine::Runtime rt;  // created with features above
HANDLE events = rt.eventHandle();
for (;;) {
    const DWORD r = MsgWaitForMultipleObjectsEx(1, &events, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    if (r == WAIT_OBJECT_0)
        while (auto ev = rt.nextEvent(0ms)) HandleEvent(*ev);   // tray, auto-record, launch, exit
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    if (stopped) break;
}
```

Timers for auto-record use `SetTimer` on the tray window or a waitable timer in the same wait set. No
sleeps.

## Migration order of `ControllerMain` (each row is its own small commit unless noted)

1. Introduce `runtime/core/Runtime` owning recording scope, children scope, heartbeat, Vulkan
   residency and settings. `ControllerMain` creates it and still runs today's loop by calling
   `runtime.ServiceOnce()` (a temporary internal step). Behavior is identical.
2. Move startup (`CompleteControllerStartup`) into `Runtime` start. The frontend no longer posts
   `main_kMsgCompleteControllerStartup`.
3. Move hotkeys (registration, reload, dispatch, hook) into `runtime/desktop/GlobalHotkeys`.
4. Move the pseudo overlay into `runtime/desktop/DesktopOverlay` (including profile overrides; the
   parser debt fix is a separate later commit).
5. Move screenshot orchestration into `runtime/screenshot`.
6. Move the engine loop onto the engine thread. `ControllerMain` becomes the frontend loop above and
   uses the C++ `Runtime` directly (still in-exe).
7. (M4) Implement the C ABI over `Runtime`; switch the frontend to `cengine.hpp`; delete v1
   (`include/libcaptureengine.h`, `libcaptureengine*.{h,cpp}`) and `main_internal.h` globals; turn on
   the boundary rule `frontend/** → include/cengine/**` with the transitional allowance below.

Transitional allowance (listed in `tools/module_boundaries.json`, removed in M9): until the setup
API exists, the frontend may call `ce::startup::*` and `ce::pawnio::*` for tray menu items. This works
only while the runtime is still in the exe (one copy of statics). It **must** be gone before M7
creates the DLL. M7 refuses to start while any allowance remains.

## Role host

### Until M10: `captureengine.exe` is both frontend and role host

- `HostChildrenSession` already spawns `paths.Executable()` with role arguments **(verified)**.
  With `cengine.dll`, `PackageLayout::RoleHost()` provides that path.
- Third parties ship `captureengine.exe` as the role host in their package. Started without role
  arguments it runs the CaptureEngine app (a tray app). That's acceptable for v2.0 but untidy, which
  is why M10 exists.

### M10 (optional): split into `cengine_host.exe`

Rename hazards found by grep (verified 2026-10-07). Each must be handled explicitly:
- `common/capture/screen_grab_privacy.cpp:141` compares the process name to `captureengine.exe`
  (excludes CE's own windows from screen grabs). It must recognize the frontend, the role host and
  any client process that owns runtime windows (desktop overlay). Better: a generic rule (windows
  owned by the runtime process PID plus helper PIDs) instead of names.
- `common/platform/runtime_package_paths.cpp:123` default helper name.
- Packaging and installer lists (`tools/build/build_packaging.py`, `installer/`, payload),
  uninstaller.
- Autostart and elevation setup point at the frontend exe (should stay `captureengine.exe`).
- Log names and `tools/log_digest.py` process recognition (`process_ipc.cpp:435` maps controller
  logs to `captureengine.log`).
- Injection exclusion lists (the injector must never inject into the role host). **(verify)** where
  self-exclusion is implemented.
- Crash dump naming / WER named dump store (`crash_handler.cpp:440`).

## Third-party deployment model

- **App-local package (recommended for v2.0):** the client ships the full runtime package next to
  its executable (or in a subfolder passed as `package_dir`). Settings are in that package's
  `config.ini`. Limits: one runtime per Windows session, and the Vulkan layer registration and
  elevation service are per user/machine (08 Q4).
- **Installed CaptureEngine package (later):** the client finds an installed CE package (registry
  value written by the installer) and loads its `cengine.dll`. That adds a discovery API and
  version negotiation, so it waits until someone asks for it.
