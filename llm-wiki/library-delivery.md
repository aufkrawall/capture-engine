# Engine library delivery: current contracts and next boundaries

Last source check: 2026-10-08. Required delivery is D13 in
[architecture-debt-plan.md](architecture-debt-plan.md); this page records evidence and design inputs.
There is no independently built engine library/runtime yet.

## Existing capability and ownership inventory

| Capability | Current entry and authority | Library/client boundary needed |
| --- | --- | --- |
| Video/audio-only/streaming recording | `libcaptureengine_controller.cpp`, `controller_recording.cpp`, `recording_session.cpp`; media process owns actual live/finalized output | Commands/outcomes stay engine-owned; frontend consumes intent/live/finalizing/error without child bookkeeping |
| Overlay and benchmark toggles | `main_recording.cpp` and C facade send authenticated inject commands; inject owns overlay settings publication | One engine operation for each command; UI/hotkeys only request it |
| Screenshot | C facade wraps `TakeScreenshot`; hides/restores desktop overlay and publishes inject notification | Engine owns capture and notification result; frontend presents outcomes |
| Injection/target discovery and launch | `main_controller.cpp`, inject process; CLI `--launch` is deferred until startup readiness | Target/launch intent belongs in engine API; preserve launcher and compatibility behavior |
| Configuration | `runtime_configuration.{h,cpp}` and `configuration_state.h` own settings, startup identity and coherent reload; mutable `main_g_Config` removed | Complete runtime/bootstrap and programmatic settings contracts; frontend adapts published changes |
| Child start/health/retirement | `host_children.{h,cpp}` and `child_process_lifecycle.h` now own all four roles and active/retired identities | Reuse this non-UI boundary from owned runtime; public API exposes operations/observations, never handles/clients |
| Tray/hotkeys/desktop presentation | `ControllerMain` in `main_entry.cpp`, startup portion in `main_recording.cpp`, `tray.cpp`, `hotkey_input_hook.cpp` | Frontend owns tray/input registration and adapts engine outcomes; preserve desktop overlay features |
| Sensors/logger/display timing | Existing worker roles plus service-policy/config helpers | Runtime supervises helpers; sensor readiness remains nonfatal and capability-specific |
| Startup/elevation/PawnIO installation | `captureengine/elevation`, `captureengine/sensors`; tray exposes installation/startup actions | Separate explicit platform/setup actions from engine lifecycle; no automatic elevation or client-global mutations |
| Poll/state/stats | Controller-bound C facade; recording intent is available, full recording stats are unsupported | Specify owned lifecycle and truthful status before publishing new ABI; no invented zero telemetry |

Anchors: [public C header](../include/libcaptureengine.h),
[controller bootstrap/loop](../captureengine/app/main_entry.cpp),
[controller commands](../captureengine/app/libcaptureengine_controller.cpp),
[recording owner](../captureengine/app/recording_session.h),
[child owner](../captureengine/app/host_children.h).
This is an initial capability map, not a completed inventory of every config key, worker or UI action.

## Current C facade is attachment, not runtime creation

`ce_engine_create` requires `BindControllerBackend` on the initialized controller's thread and
supports one handle. `ce_engine_destroy` releases that handle while the controller/recording continue.
Custom config values and recording statistics return unsupported. Polling pumps the controller's
thread messages and allows supported hotkey dispatch, but rejects nested pumps/destruction.
Keep these v1 contracts honest. A versioned owned-runtime API must define its own creation/shutdown,
threading, descriptor/string ownership, command acceptance, live/finalized state and failure behavior.
The C ABI must not expose STL, mutable AppConfig, IPC mappings or renderer/SDK resources.

## Child ownership prerequisite implemented in this slice

`HostChildrenSession` acquires one host-thread owner without constructing a tray or calling
ControllerMain. It creates the authenticated endpoint clients and owns helper handles privately.
Parent adapters use readiness, command, retirement and validated inject-control operations; six
writable handle/client globals are removed. The actual native module is linked into unit tests.
This proves endpoint-owner construction only; it does not initialize the complete engine headlessly.

`ChildProcessLifecycle` is the production admission/ownership transaction tested with controlled OS
effects. A broken channel cannot replace a still-running peer. Message-pump reentry invalidates
readiness by generation; recursive startup is refused. A late successful spawn after shutdown stays
owned as retired instead of resurrecting the active slot. Media retirement permits an immediate new
recording while preserving the old finalizer's handle. Collection closes only observed exited peers.
Shutdown includes retired children and waits only on remaining live identities, avoiding a spin on
an already-signaled child while another is draining. Failed termination does not count as exit.
Shutdown reserves retirement storage before changing admission. An exception during a shutdown effect
retains active ownership and permits retry; destruction retains the native owner if cleanup throws.

Auxiliary service reconfiguration keeps its shutdown event signaled until old logger/sensor processes
have exited, then starts the requested services. It no longer drops live handles and resets the event
after an unverified timeout. Health recovery is metered; recording reconciliation precedes recovery.
The console's cross-thread stop flag is now atomic. Current shutdown still uses the established
10-second graceful budget and confirmed forced termination as the executable's fallback; a future
library shutdown API must explicitly report failure and retain ownership rather than unload live code.

Tests: [lifecycle](../tests/test_child_process_lifecycle.cpp),
[native empty/headless scope](../tests/test_host_children.cpp), existing recording/API/IPC tests.
Mutation evidence and final gate results belong in log/recent.md. Controlled lifecycle effects do not
prove real helper finalization or the full hardware/A/V matrix.

## Owned configuration prerequisite

RuntimeConfigurationSession loads the resolved executable INI path without ControllerMain/tray.
ConfigurationState owns the published AppConfig, observed startup identity and reload timing. Seeding
from startup avoids losing an edit before the first poll. Replacement requires stable identity, a
readable complete document, no failed reads during parsing and unchanged identity afterward; candidate
failure preserves the snapshot and retries. Reentrant polling cannot publish twice. Unsigned tick
arithmetic preserves wrap behavior; frontend code consumes old/new settings without owning the gate.
The private application view is const and scope-bound; process log destination remains a named host
operation. Existing INI/default startup behavior is preserved, including missing-file template creation.
This is not a versioned programmatic settings API or fully transactional startup validation. Package
paths now have a private owner; public runtime descriptors and process-wide read accounting remain open.

Anchors: [settings transaction](../captureengine/app/configuration_state.h),
[native owner](../captureengine/app/runtime_configuration.cpp),
[production/native tests](../tests/test_runtime_configuration.cpp),
[package paths](../common/platform/runtime_package_paths.h),
[path/helper handoff tests](../tests/test_runtime_package_paths.cpp).

## Remaining extraction order

1. Finish startup/configuration/child replacement traces, including auxiliary shared-event and setup
   interactions. Define the complete supported capability matrix and one-active-owner conflicts.
2. Move runtime bootstrap/service/shutdown out of `ControllerMain`: recording scope, child scope,
   window heartbeat, Vulkan residency and configuration must have engine-owned lifetimes. Tray,
   hotkey registration and UI callbacks become client effects. Preserve reentry and cancellation.
3. Resolve process-wide effects before embedding: WinMain currently installs crash handling, log
   directories/cleanup, process power policy, console handling and startup/elevation behavior. A DLL
   must not silently impose those policies on an unrelated client process.
4. RuntimePackagePaths now owns executable-anchored defaults/module helper selection and explicit INI
   overrides; the child owner copies an explicit worker executable. A renamed helper outside the client
   directory loads the requested INI in the native probe. Complete resource/loader/module lifetime,
   public descriptors and outside-repo library deployment still require D13 work.
5. Specify and build the independent runtime/C ABI with explicit module/source ownership; package
   x64/x86 public headers/import artifacts/helpers. Then migrate frontend capability slices onto it
   and prove standalone public-header-only consumption, failure cleanup and shutdown/recreate.

Open risks: full engine scope still uses main_internal.h and frontend bootstrap; setup can race sensor
recovery; IPC active/retired media observation attribution needs D5 traces; process-global discovery
and injected settings require explicit ownership conflict handling. No parallel-instance, provider
unload, cross-compiler ABI, real-game or end-to-end recording claim follows from this slice.
