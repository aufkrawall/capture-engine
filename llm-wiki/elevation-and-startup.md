# Elevation service and Windows startup

Last source verification: 2026-10-01. Runtime verification remains in progress.

## Summary and source anchors

Three independent, default-off tray settings control a protected elevation service,
administrator requests on the next launch, and Windows autostart.

- captureengine/startup_control.*: early bootstrap, authenticated UAC context
  transfer, asynchronous tray setup and preference transaction.
- captureengine/pawnio_workers.*: tracked driver setup UI/helpers and joined shutdown.
- captureengine/startup_preferences.*, common/startup_policy.h: original-user
  SID ownership, atomic preferences and account-based registration policy.
- captureengine/startup_autostart.cpp: native Task Scheduler COM and user Run key.
- captureengine/elevation_setup.cpp, elevation_runtime.cpp: protected staging,
  pinned hashes, service ACL, rollback and actual process-exit/removal.
- common/elevation_protocol.h, elevation_windows.h, elevation_lifetime.h:
  bounded protocol, overlapped cancellation/draining and terminal client lifetime.
- elevationservice/*: minimal x64 LocalSystem service, authenticated pipe,
  isolated CLR/LHM bridge and privileged ETW ownership.
- captureengine/elevation_client.*, sensor_broker.*, sensor_plugin.*:
  preferred broker backend with existing validation/freshness and local fallback.
- captureengine/display_timing_service*, sensor_service.cpp: ordinary ETW
  consumption, desktop monitor queries, correlation and shared-ring publication.
- tools/build/build_elevation_service.py: separate target without media/FFmpeg linkage.

## Installer role

`captureengine.exe --ce-installer-setup --owner-sid=... --owner-admin=0|1 --prefs=0-7 --service=install|remove|keep
--autostart=apply|keep` is run by the installer (see `installer.md`). It requires an elevated process whose parent is
also a live, older, elevated process, validates every argument (`common/installer_setup_policy.h`), then reuses
`Apply()` so service, startup registration and the preference record behave exactly like the tray toggles.

## Invariants

The manifest remains asInvoker. Only ordinary controller launches honor the full
elevation preference; workers and utilities bypass it. Bootstrap runs before logs,
tray creation and child startup. Canceling UAC cancels that launch without clearing
the preference. Context metadata precedes the preserved raw argument tail, including
the complete --launch command. The executable directory remains the working directory.

An inherited original-user SID carries ownership through alternate-account UAC
credentials. Controller bootstrap ignores unauthenticated inherited ownership.
A random same-image/PID-verified local pipe transfers the SID and account type;
changing the SID requires an elevated sender. Preferences use one REG_BINARY
record under HKEY_USERS/<initiating SID>/Software/CaptureEngine.

Autostart uses Run when elevation is off, an interactive highest-privilege logon
task for an administrator account when on, and Run followed by UAC credentials
for a standard account. Task ownership and logon trigger use the initiating SID,
with no password, no time limit and battery operation allowed. Reconciliation removes
the other registration and restores the preceding registration on failure. Changing
full elevation while autostart is enabled reconciles immediately; current recording
and process elevation are unchanged.

Setup stages an immutable nonce runtime under the native ProgramFiles known folder,
CaptureEngine/ElevationService/runtime-<32 hex digits>. Protected owner/DACLs
prevent ordinary modification. The service binary, four pinned LHM assemblies,
license notices and available service PDBs are copied. Setup/replacement/removal
require UAC. The installing SID can query/start the service. PawnIO driver setup
remains independent.

The local-only byte pipe validates magic/version/opcode/length/sequence before use.
Hello authenticates actual peer SID/session, matching executable and a self/direct
parent controller whose creation time predates the peer. Its process handle bounds
blocked IO and subscriptions. Requests contain selectors and fixed operations,
never commands, paths or arbitrary ETW providers. Hello/status report capabilities;
samples preserve acquisition time and reuse the existing parser.

LocalSystem creates, enables, flushes and stops the ETW session. The installing SID
gets only read rights (WMIGUID_QUERY | WMIGUID_NOTIFICATION | TRACELOG_ACCESS_REALTIME, kConsumerTraceRights) on the fixed CE session GUID; OpenTrace succeeds without them but ProcessTrace returns ACCESS_DENIED (observed 2026-10-01 with realtime-only rights; the widened mask is unverified on hardware); teardown removes
that permission. Desktop queries remain interactive. A service trace the worker cannot consume is not retried for that service PID; the local
backend takes over until the service restarts. Broker failure uses the local
backend; unelevated fallback truthfully reports unreadable CPU rails and
permission-dependent display timing.

Transitions are serialized on the sensor worker. Its local bridge exits before the
broker starts a sensor subscription. PawnIO changes renew that subscription.
Disconnect clears telemetry and stale samples remain unavailable. Reconnection uses
bounded attempts in the existing polling cadence, without synchronization sleeps.

After the last client releases its resources, the service refuses new leases and
stops. Clients monitor its process handle, so an orphaned ETW session cannot mask a service crash.
Replacement/removal disable starts and wait for SCM STOPPED plus actual exit.
Owned child termination is followed by an exit wait. Resident hooks are outside
this feature: injected games must exit before their loaded files are removable.

## Diagnostics and verification

Controller diagnostics cover setup/cancellation, backend changes and bounded
protocol/ETW errors without SID/credential/user-path output. Service PDBs remain
available. Incomplete rollback and cleanup are logged as errors.

tests/test_elevation_protocol.cpp covers all eight policies, argument round trips,
cancellation/persistence rollback, overlapped-read cancellation, actual pipe identity,
sample preservation and concurrent final lease release. The new parser fuzz harness
and committed elevation_protocol corpus are registered. Build self-tests pin runtime
hashes and cache discovery.

Run python tools/run_elevation_integration.py after building for a UAC-approved
temporary install. It refuses an existing service, verifies an ordinary sensor
worker, requires real CPU temperature and display-ring publication from a D3D11
fixture, tests forced controller loss and process exits, removes the service, then
renames/deletes the portable fixture. Missing hardware prerequisites fail explicitly.
Failed runs retain the disposable fixture.

## Open verification

Interactive runtime testing is in progress. Standard-account alternate credentials,
UAC cancellation, real login startup, service toggles during recording and resident-game
file removal need runtime checks. Static/unit/fuzz passes do not establish them.

Service lifecycle notifications follow [NotifyServiceStatusChangeW](https://learn.microsoft.com/en-us/windows/win32/api/winsvc/nf-winsvc-notifyservicestatuschangew); deletion waits for actual registration removal as documented by [DeleteService](https://learn.microsoft.com/en-us/windows/win32/api/winsvc/nf-winsvc-deleteservice).
