# 04 - Runtime modules: interfaces, hidden policy, migration map

Every section has the same parts: interface sketch (C++, internal), what it hides, invariants that
must survive (taken from today's code), where the code comes from, and tests through the interface.
Signatures are working names; adjust them to the code, but keep the interface **small**. If a
sketch grows by more than a few functions during implementation, stop and redesign.

Common conventions:
- `RequestId` is `uint64_t`, allocated by `core`.
- `Clock` is injectable (`uint64_t (*)() noexcept` or a small struct) as `RuntimeConfigurationSession`
  already does (`nowMs`). No sleeps in tests.
- Components receive `const AppConfig&` snapshots from `core`. They never call a global accessor.
  `RuntimeConfiguration()` disappears from the runtime (today 7 files use it in `captureengine/app`,
  verified 2026-10-07).

---

## runtime/core: `Runtime`

```cpp
namespace ce::engine {
struct RuntimeOptions {
    PackageLayout package;          // validated by runtime/package
    std::wstring dataDir;
    std::string clientName;
    FeatureSet features;            // hotkeys, desktop overlay, crash handler, watch settings
};

using Command = std::variant<StartRecording, StopRecording, ToggleRecording, ToggleOverlay,
                             ToggleBenchmark, TakeScreenshot, LaunchTarget, CommitSettings,
                             SetupAction, ClientLog>;

class Runtime {
public:
    static Expected<std::unique_ptr<Runtime>, CreateError> Create(RuntimeOptions);
    Admission Submit(Command);                    // thread-safe; {status, RequestId}
    StatusInfo Status() const;                    // thread-safe snapshot copy
    EventQueue& Events();                         // thread-safe; owned memory
    void RequestShutdown();                       // thread-safe, idempotent
    WaitResult WaitStopped(std::chrono::milliseconds);
    ~Runtime();                                   // precondition: stopped; joins threads
};
}
```

Hides: engine thread and loop, command queue, wake event, message queue (hotkeys, deferred
startup), startup order, helper servicing cadence, settings fan-out, status publication, shutdown
order, the session ownership claim, module pinning and the diagnostics session.

### Engine loop (direct port of today's `ControllerMain` loop body, verified 2026-10-07)

```
loop until stopped:
  wait: MsgWaitForMultipleObjectsEx({commandEvent}, NextWakeMs(), QS_ALLINPUT, MWMO_INPUTAVAILABLE)
  drain message queue: startup-complete message, WM_HOTKEY, hook hotkey message (DispatchHotkey → Submit internal)
  drain commands: dispatch each; push COMMAND_COMPLETED unless the command completes later (screenshot, settings, launch)
  hotkeys.ReportDiagnostics()                   // ReportHotkeyInputHookDiagnostics
  recording.Service(includeChildHealth=false)   // CheckRecordingFailureState: failure before health
  children.Service(services, beforeRecovery = recording.Service(true))   // CheckChildProcessHealth
  if settings.Poll() → ApplySettings(old, new)
  ce::startup::Pump() equivalent (setup workers) → setup events
  publish status board; emit state-change events
  loop-health diagnostics (existing [ControllerDiag] summary, unchanged format)
```

`NextWakeMs()` = min(2000, settings.NextPollDelay(), pending command timers). The auto-record timer
moves to the frontend, so `GetControllerLoopWaitMs` loses that term.

### Startup (port of `CompleteControllerStartup`, verified order)

1. `children.Ensure(Inject)`. On failure: state FAILED, shut down helpers, emit RUNTIME_STATE FAILED.
2. Media is not started at startup. Today only `--auto-record` starts it early
   (`ShouldStartMediaProcessAtStartup`). Keep that as a frontend hint via the `StartRecording` command
   flow; no special API.
3. Logger if `ShouldStartLoggerProcess(config)`, sensors if `ShouldStartSensorProcess(config, recording)`.
   Failures are non-fatal and produce HELPER events.
4. Wait for connection (`ConnectToChildProcesses` semantics: present ⇒ ready).
5. If `features.hotkeys`: register hotkeys and start the hook thread (exact port of the registration
   block and `PublishHotkeyBindings` + `StartHotkeyInputHook(engineThreadId)`).
6. If `features.desktopOverlay`: `desktop.Apply(settings)` (port of `SyncPseudoOverlayConfiguration`).
7. Startup perf log line (keep the `[StartupPerf]` format), then state READY.
8. The PawnIO offer (`OfferInstallationAsync`) is **not** done by the runtime. The frontend decides
   after READY, using setup status (M9). Until M9, the frontend keeps calling it through the
   transitional allowance listed in M4.

`PumpStartupMessages` callbacks into `HostChildrenSession` become a pump of the engine thread's own
queue, so spawn waits keep hotkeys and desktop overlay messages serviced.

### Settings fan-out (port of the reload block in `ControllerMain`)

`ApplySettings(old, new)` calls, in today's order:
1. `Log_SetLevel(new.logLevel)`
2. `heartbeat.UpdateProfiles(new.applicationProfiles)`
3. `hotkeys.Apply(old, new)` (re-register changed bindings, republish bindings to the hook thread)
4. `children.ReconfigureServices(...)` (`SyncLoggerAndSensorProcesses`, including sensor restart on a
   hardware-sensor config change) inside `MainThreadBlockTimer`
5. `children.SendToAll(ReloadConfig)`
6. `desktop.Apply(new)`
7. emit `SETTINGS_CHANGED{revision, source}`

This is a fixed, explicit list in `core`. Do **not** add an observer/subscriber framework.

### Shutdown (port of `ControllerMain` tail and `ShutdownChildProcesses`, verified order)

1. `heartbeat.Stop()`
2. `hotkeys.Stop()` (stop hook, unregister all five)
3. `desktop.Stop()` (pseudo overlay shutdown, before helpers)
4. setup workers shutdown (`ce::startup::Shutdown`, `ce::pawnio::ShutdownSetupWorkers`)
5. `recording.Shutdown("runtime shutdown")`
6. `children.Shutdown(deadline)`; on success `ReleaseSessionLatencyChannel()`; on failure keep
   ownership and report `CE_E_TIMEOUT`
7. state STOPPED; the engine thread ends via `FreeLibraryAndExitThread` once `destroy` released it,
   or parks until destroy

The tray shutdown animation is frontend work, driven by RUNTIME_STATE STOPPING.

### Globals removed (from `main_internal.h`, verified 2026-10-07)

| Global | New home |
| --- | --- |
| `main_g_Running` | `Runtime` stop token (engine) + frontend's own loop flag |
| `main_g_ConfigPath` | `SettingsStore::Path()`; the frontend's "open config" uses `ce_settings_*` or the path from a frontend-known package dir |
| `main_g_AutoRecord*` | frontend auto-record script |
| `main_g_DeferredLaunchPath` | frontend → `ce_target_launch` after READY |
| `main_g_Tray` | frontend |
| `main_g_PseudoOverlay` | `DesktopOverlay` member of `Runtime` |
| `main_g_HotkeyOwnership` | `GlobalHotkeys` member |
| `main_g_ControllerStartupTiming` | `Runtime` member |
| `g_Session` (controller_recording.cpp) | `RecordingController` member |
| `g_ApiMutex`, `g_Backend`, `g_Engine` ... (libcaptureengine.cpp) | deleted with v1 |

Tests through the interface (`tests/test_engine_runtime.cpp`): construct `Runtime` through a private
test factory that takes fake components (children, clock). Cases: create→READY→shutdown→STOPPED;
inject spawn failure → FAILED with helpers shut down; command before READY rejected; exactly one
COMMAND_COMPLETED per request; event overflow coalescing and DROPPED count; shutdown during startup;
shutdown with a retired finalizer still running (timeout keeps ownership, a later shutdown
succeeds); recreate after destroy; settings change fan-out order (fake components record calls).

---

## runtime/package: `PackageLayout`

```cpp
struct PackageLayout {
    static Expected<PackageLayout, PackageError> Resolve(HMODULE runtimeModule, const wchar_t* packageDirOverride);
    const std::wstring& Directory() const;
    const std::wstring& RoleHost() const;       // captureengine.exe (M10: cengine_host.exe)
    const std::wstring& Settings() const;       // <dir>\config.ini
    std::string AnsiDirectory() const;          // existing exact/8.3 behavior
    bool EncodingExact() const;
};
```

Hides: module path resolution, the ANSI/8.3 fallback (`RuntimePackagePaths`), required-file
presence (role host, `mediaengine.dll`, `capture_hook_x64.dll`, `capture_hook_x86.dll`, Vulkan layer
DLLs and manifests, license file), and build identity matching (role host `--version`-free check:
read the PE version resource or a generated identity export, **(verify)** which exists).

Source: `common/platform/runtime_package_paths.*` (extend; keep `FromExecutable` for the role host).

Tests: missing role host → `CE_E_PACKAGE` naming the file; identity mismatch; non-ASCII package
directory (exact/8.3/none); package dir differs from the client exe dir and CWD.

---

## runtime/settings: `SettingsStore`

```cpp
class SettingsStore {
public:
    SettingsStore(std::wstring path, Clock clock, bool watchFile);
    std::shared_ptr<const AppConfig> Current() const;            // immutable snapshot
    uint64_t Revision() const;
    std::optional<SettingsChange> Poll();                       // external edits (watchFile)
    uint32_t NextPollDelayMs() const;
    std::optional<std::string> Value(std::string_view section, std::string_view key) const;
    // Pure, any thread: parse candidate text, return diagnostics.
    static Validation Validate(const IniDocument& candidate);
    CommitResult Commit(const SettingsEdits& edits, uint64_t baseRevision);   // engine thread
};
```

Hides: INI text model, coherent-replacement rules (stable identity, complete read, no failed reads,
unchanged identity afterwards), seeding from startup identity, debounce, retry and wrap-safe
timing, template creation for a missing file, atomic write, self-write suppression, profile
resolution and the loader.

Invariants to keep (from `configuration_state.h` / `runtime_configuration.cpp`, verified via the
library-delivery wiki page and code): a failed candidate preserves the snapshot and retries;
reentrant polling cannot publish twice; an edit before the first poll is not lost; missing-file
template creation stays.

Required loader work (in `common/config`, its own commits):
1. **Text input**: `LoadConfig` today takes a path (`config.h:647`). Add
   `LoadConfigFromDocument(const IniDocument&, AppConfig&, ConfigDiagnostics*)` and make the path
   overload a wrapper, so file behavior is byte-identical.
2. **Diagnostics sink**: today the loader logs warnings (`LogApplicationProfileWarning`, `LogWarn`
   in `config_load_*.cpp`). Route each warning through the sink (it still logs when no sink is given,
   so the log stays the same).
3. **Unknown keys**: let the INI reader record which keys were looked up. After a load, every key
   present but never read is "unknown". This needs no hand-written schema and stays correct as
   loaders change. Profile sections are handled by the same lookup tracking.
4. **IniDocument edit**: set/remove a key in place (keeping comments, order, line endings and
   encoding: UTF-8 with or without BOM, as `config_text_encoding` handles today); create a missing
   section at the end.
5. **Fuzz**: `fuzz_config_parser` exists. Add a document-edit harness (`fuzz_settings_edit`) with a
   committed seed corpus (CLAUDE.md fuzz rule: parser change).

Tests: commit success publishes and fans out; validation error keeps the edit open; conflict when the
file changed between begin and commit; self-write is not re-applied by `Poll`; external invalid edit
produces REJECTED and keeps the snapshot; comments and order preserved byte-exactly except edited
lines; BOM and non-BOM files; profile section edit; concurrent `Validate` on client threads while the
engine commits.

---

## runtime/children: `ChildSupervisor`

Today `HostChildrenSession` owns a hidden instance, and free functions (`EnsureHostChild`,
`SendHostChildCommand`, ...) reach it globally (verified, `host_children.h`). Target: the same
`Impl`, exposed as instance methods. The policy doesn't change.

```cpp
class ChildSupervisor {
public:
    ChildSupervisor(const PackageLayout&, const SessionIdentity&, PumpFn pump, AcceptingWorkFn accepting);
    bool Ensure(HostChild, uint32_t timeoutMs = 0);
    bool Ready(HostChild) const;
    bool Send(HostChild, ProcessCommand, const char* payload, ProcessResponse*, uint32_t timeoutMs = 1000);
    void SendToAll(ProcessCommand);
    void RetireMedia();
    CommandOutcome StopRecording(HostChild, uint32_t timeoutMs);
    std::vector<ChildEvent> Service(AuxiliaryServices, FunctionRef<void()> beforeRecovery);
    void ReconfigureServices(AuxiliaryServices);
    bool StopSensorsForSetup();
    bool Shutdown(Deadline);
    InjectControlChannel InjectControl() const;  // bound to the current inject child identity
    std::vector<FinalizerExit> CollectRetiredMediaExits();     // M6
};
```

Hides: spawn arguments (`--mode`, `--config`, `--session-dir`, recording id), authenticated
endpoints, readiness generations, the active/retired split, health recovery metering, the shutdown
event, termination fallback and collection of exited peers.

Invariants (from `ChildProcessLifecycle`, verified via library-delivery.md): a broken channel can't
replace a running peer; reentrant readiness is cancelled by generation; a late spawn after shutdown
stays retired; media retirement allows an immediate new recording; shutdown waits only on live
identities; failed termination doesn't count as exit; an exception during a shutdown effect keeps
ownership.

New for the library: `ChildEvent` (ready/lost/recovered) for HELPER events, and
`CollectRetiredMediaExits()` for finalization reporting (M6).

Migration: mechanical rename of the free functions to methods. Callers get the supervisor
reference from `core`. Existing tests (`test_child_process_lifecycle.cpp`, `test_host_children.cpp`)
move with it unchanged except for construction.

---

## runtime/recording: `RecordingController`

```cpp
class RecordingController {
public:
    RecordingController(ChildSupervisor&, InjectControl&, Clock);
    Admission Start(Mode, const AppConfig&, const char* reason);
    Admission Stop(const char* reason);
    Admission Toggle(Mode, const AppConfig&, const char* reason);
    void Service(bool includeChildHealth);      // reconciliation; emits state changes
    RecordingStateInfo State() const;           // {state, mode, id, failure}
    std::vector<RecordingEvent> TakeEvents();   // drained by core → public events + desktop/tray
    void Shutdown(const char* reason);
};
```

Keeps `RecordingSession` (already a good deep module: `recording_session.h`) unchanged inside. The
`RecordingEffects` implementation (`ControllerEffects` in `controller_recording.cpp`) moves here,
with these changes:
- `main_g_Tray->SetRecordingState` → remove. The tray reacts to RECORDING_STATE events.
- `main_g_PseudoOverlay->SetRecordingStartIntent / ShowRecordingFinalizingNotification` → emitted as
  `RecordingEvent`s; `core` forwards them to `DesktopOverlay`.
- `DisableAutomaticRecording` → becomes a RECORDING_STATE event field (`failure` with an
  integrity class). The frontend's auto-record script stops itself on it.
- `PublishRecordingFailureOverlayNotification` stays (in-game notification via `InjectControl`).

Public state mapping (from `RecordingSnapshot` + notices):

| Internal | Public `CE_RECORDING_*` |
| --- | --- |
| `!requested` | IDLE |
| `requested && !observedLive` | STARTING |
| `requested && observedLive` | LIVE |
| Stop accepted (`StopResult`), until child stop resolves | STOPPING |
| `Failed` notice | IDLE with `failure` set |

**Finalization (M6).** Today the controller only knows "stop accepted, finalization asynchronous".
The media process finalizes and exits. Needed: (a) the output path(s) of each recording and (b) the
finalization result. Smallest option, reusing existing channels: media writes the final status and
path into its recording manifest (it already writes `recording_<id>_<pid>.manifest`,
`WriteRecordingManifest`) **and** sets a distinct process exit code. The supervisor reports the exit
(`CollectRetiredMediaExits`) and `RecordingController` reads the manifest for that recording id/pid
to fill the path. This uses no new IPC, and the manifest is already a durable artifact. **(verify)**
whether the media role's exit code distinguishes success from failure today; if not, define codes
in `process_ipc.h`. Mapping recording identity requires carrying the controller request id into
the media spawn arguments (the diagnostic `g_RecordingId` already is).

Tests: existing `RecordingSession` tests stay. New: state/event mapping table above, including
immediate restart while the previous recording is finalizing (two recording ids, one FINALIZED for
the first), failure during STARTING, stop during STARTING, toggle mode switch.

---

## runtime/inject: `InjectControl` and `TargetLauncher`

```cpp
class InjectControl {
public:
    explicit InjectControl(ChildSupervisor&);
    Outcome ToggleOverlay();                      // ProcessCommand::ToggleOverlay
    Outcome ToggleBenchmark();
    void Notify(OverlayNotificationType, uint64_t expiryTick);
    bool PublishRecordingIntent(RecordingStartIntent);
    RecordingHealthObservation ReadHealth();
    void ClearMediaFailure(uint32_t failure, bool mediaGone);
};

class TargetLauncher {
public:
    explicit TargetLauncher(const AppConfig& snapshot);
    LaunchResult Launch(std::string_view commandLineUtf8);   // engine thread
};
```

Sources: `ToggleOverlay`/`ToggleBenchmark` in `main_recording.cpp` and their duplicate in
`libcaptureengine_controller.cpp` (two copies of one policy today, so delete one);
`PublishHost*`/`ReadHostRecordingHealth`/`ClearHostMediaFailure` free functions;
`LaunchGameSuspended` + `ParseDeferredLaunchCommand` in `main_controller.cpp`.

Debt found in `LaunchGameSuspended` (verified 2026-10-07). Fix it in its own commits with tests, never
inside a move commit:
- a filename heuristic (`_dx`, `_vulkan`, `game`, `test.exe` ...) decides launcher versus game. It
  isn't game-specific, but it is fragile. Replace it with a generic rule, e.g. always launch suspended
  and early-inject, and let the existing injection policy (whitelist/exclusions) decide; **(verify)**
  with the user, since it changes launch behavior.
- `Sleep(100)` before the fallback injection violates the no-sleep rule. Replace it with a readiness
  signal (e.g. wait for the initial thread to run past loader init, or rely on the inject child's
  normal discovery path).
- `ANSI CreateProcessA` with a UTF-8 API input: convert to wide and use `CreateProcessW`.
- `static std::shared_ptr<InjectionManager> s_launcherInjector` hidden global: becomes a launcher member.

Tests: command-line parsing (quoted/unquoted/empty), UTF-8 path with non-ASCII characters, launch
failure → COMMAND_COMPLETED with `CE_E_PROCESS`, inject unavailable → overlay toggle `CE_E_IPC`.

---

## runtime/screenshot: `Screenshotter`

```cpp
class Screenshotter {
public:
    Screenshotter(InjectControl&, DesktopOverlay* /* optional */);
    ScreenshotResult Take(const AppConfig&);    // {status, paths[]}
};
```

Today (verified): `TakeControllerScreenshot` brackets the pseudo overlay (`BeginScreenshotCapture` /
`EndScreenshotCapture` + `ShowScreenshotNotification`), calls `TakeScreenshot(dir, colorSpace)`
(returns `bool`), and publishes an in-game notification. Changes:
- `TakeScreenshot` returns the written path(s). Combined HDR+SDR is the default, so up to two paths
  (memory: screenshot both variants).
- The desktop-overlay bracketing stays internal (the overlay is a runtime component), so clients
  never take part in it.
- It runs on the engine thread as today. Moving it to a worker later is a separate, measured change.

Tests: success with two paths, failure → `CE_E_IO` with no paths, overlay bracketing always ends
even when capture fails (existing scope-guard behavior).

---

## runtime/desktop: `DesktopOverlay`, `GlobalHotkeys`

`DesktopOverlay` wraps `PseudoOverlay` (already self-threaded). Interface:
`Apply(const AppConfig&)` (port of `SyncPseudoOverlayConfiguration` +
`ResolvePseudoOverlayApplicationConfigs` + `ParseProfileDesktopOverlayOverrides`),
`OnRecording(RecordingEvent)`, `BeginCapture()/EndCapture(bool saved)`, `Stop()`.

Debt to fix (separate commit): `ParseProfileDesktopOverlayOverrides` re-reads the INI file by path
(`main_g_ConfigPath`) with its own `TryParseBool`/`TryParseInt` parser, a second parser for profile
keys. Move these keys into the common config loader (profile resolution) so `SettingsStore` is the
only reader. Then validation diagnostics cover them too.

Status-dark protocol (verified, `status_overlay_sync.h`): before capture starts, media waits up to
300 ms for the controller-side overlay to confirm its recording-start status left the screen,
keyed by controller PID. With `CE_FEATURE_DESKTOP_OVERLAY` off, nobody acknowledges, so every start
costs up to 300 ms. Required: tell media that no status consumer exists (spawn flag
`--no-status-consumer` or a field in the authenticated startup handshake), and have it skip the wait.
Test: recording start latency without the feature has no 300 ms ack wait (assert on the media log
event or the handshake field, not on timing).

`GlobalHotkeys`: `Start(const AppConfig&, DWORD engineThreadId)`, `Apply(old, new)`, `Stop()`,
`ReportDiagnostics()`, `Dispatch(int hotkeyId) → Command`. Port of the registration block in
`CompleteControllerStartup`, the reload block in `ControllerMain`, `DispatchHotkey`, and
`hotkey_input_hook.*`. Hotkey presses become internal `Submit` calls plus an informational HOTKEY
event. Invariants: the hook thread never blocks or logs; only combinations this process owns are
served by the hook (`HotkeyOwnership`); and the hook is removed before a crash dump suspends the
process (pre-dump callback). That last one only applies with `CE_FEATURE_CRASH_HANDLER`; otherwise,
register the removal with whatever handler exists **(verify)**.

---

## runtime/platform

| Component | Interface | Source | Notes |
| --- | --- | --- | --- |
| `DiagnosticsSession` | `Open(dataDir, settings, features) → SessionIdentity`, `LogPath()` | `WinMain` prelude (session dir, cleanup, manifests, `Log_Init`, crash handler, symbol store, WER purge, crash dir from settings) | Role host keeps its own prelude for child roles; share the code as functions, not as a global |
| `VulkanResidency` | RAII, as `VulkanLayerResidency` | `main_vulkan_residency.h` | Unchanged behavior: registration intentionally outlives the process |
| `WindowHeartbeat` | `UpdateProfiles`, `Stop` | `common/platform/window_heartbeat` | **(verify)** purpose before deciding feature gating |
| `Setup` | `Request(action) → async result`, `Status()` | `captureengine/elevation/startup_control.*`, `captureengine/sensors/pawnio_*` | Must stop sensors via `ChildSupervisor::StopSensorsForSetup` before PawnIO uninstall (today's tray callback). Split app autostart (client exe path) from the elevation service; **(verify)** `Bootstrap(controller)` semantics (it may relaunch elevated — that stays frontend/role-host policy) |
| `SessionClaim` | RAII mutex `Local\CaptureEngine_Instance_Mutex` | `WinMain` | No retry loop with `Sleep(50)`: claim once and return `CE_E_BUSY`. The frontend's restart flow waits on the prior PID first (it already does) |

Source-backed existing prerequisites: [library-delivery.md](../library-delivery.md).
