# Installer and uninstaller

Last verified: 2026-10-02 (source, unit tests, native `--files-only` and off-screen UI tests). Interactive
runs that touch the machine - UAC prompt, service/startup registration, PawnIO, Installed Apps, shortcuts, launch as
the user - are **unverified** and need a manual run. Stale-risk: medium.

## Summary and source anchors

`captureengine-setup.exe` is a native Win32/GDI wizard (no frameworks, no third-party code, system DLLs only).
`captureengine_uninstall.exe` is a removal-only build of the same sources that ships inside the payload and is
registered in Installed Apps. A prior agent attempt on branch `codex/native-installer` was rejected and shares
nothing with this one.

- `installer/payload_format.h` (pure): footer/index layout, CRC-32, bounds checks, safe relative paths.
- `installer/install_policy.h` (pure): CLI, option defaults, directory rules, config.ini plan, manifest, stale files,
  user-data and temp-name predicates, shortcut ownership.
- `installer/payload.cpp`: footer/index read, streamed block decode (LZMS via `cabinet.dll`, resolved at run time).
- `installer/engine_install.cpp`, `engine_uninstall.cpp`, `files.cpp`: the file transaction, registration, removal.
- `installer/filesystem_guard.h`: directory leases, resolved paths and filesystem-identity comparison.
- `installer/process_control.cpp`: close a running Capture Engine, stop the elevation service.
- `installer/registration.cpp`, `shortcuts.cpp`, `integration.cpp`: Installed Apps record, shortcuts, ACLs, the program's own roles.
- `installer/ui_*.cpp`, `wizard.h`: window, pages, theme, custom button/checkbox control.
- `installer/main.cpp`: CLI, self-elevation, silent mode, uninstaller hop to a temp copy.
- `common/setup/installer_setup_policy.h` + `captureengine/elevation/startup_control.cpp` (`TryRunInstallerSetup`): the elevated role.
- `tools/build/build_installer.py`, `tools/installer_payload.py`: compile, payload packer, assembly and verification.
- Tests: `tests/test_installer_policy.cpp`, `tests/test_installer_files.cpp`,
  `tools/tests/test_installer_{payload,build,native,ui}.py`. The native unit fixtures use the real file and shell-link units
  in scratch folders, with no registry/service registration.

## File format

`[setup stub PE][data][index][64-byte footer]`. Files are 1 MiB blocks; method Store keeps raw bytes, method Lzms
prefixes each block with its stored length (a block whose stored length equals its content length is raw).
Integrity (CRC-32 per file, footer and index) only; there is no publisher signature. The C++ parser and
`tools/installer_payload.py` must agree on every offset; `PayloadLayoutTest` pins the Python side and
`InstallerPayloadTest` the C++ side. The packer verifies every file with an independent reader before publishing.

## Behaviour and invariants

- **Elevation.** Both binaries are `asInvoker` and relaunch themselves with `runas` for install/uninstall, so
  `--verify-payload`, `--extract=`, `--preview=` and `--files-only` need no prompt. The copy that only asks for
  elevation must not open the log (it would keep the elevated copy from writing its own).
- **Closing.** Processes whose image lies under the folder get `WM_CLOSE` on their windows (the tray window class
  `CaptureEngineTray` quits and stops its children); after `--close-timeout` seconds (default 30) the rest are terminated.
  Games are never touched: a loaded hook DLL is renamed aside, the new file takes its name, the old one is deleted
  at restart (`MOVEFILE_DELAY_UNTIL_REBOOT`).
- **Transaction.** Extract every file to `<name>.cenew`, then rename existing to `<name>.cebak` and the new one into
  place; any failure restores the backups and removes what the run created. `config.ini` is outside the unit.
- **config.ini.** Missing: created. Existing: never read for merging, never written; defaults go to `config.ini.new`.
  Uninstall keeps `config.ini` unless "delete my settings and logs" is ticked; recordings are never deleted.
- **Manifest** (`captureengine_install.manifest`) lists exactly the files written. Updates delete only files the old
  manifest listed and the new payload no longer ships; uninstall deletes only manifest files. User data paths are
  never removable through it. Cleanup and rollback resolve the root once and hold read-access directory handles
  without delete sharing while operating on internal folders; a junction or unavailable folder is skipped.
  Internal-folder and leaf opens/deletes/renames use the retained parent object (`NtCreateFile` / `NtSetInformationFile`), preventing
  alias or reparse changes from redirecting the operation while still permitting rollback and loaded-image renames.
  Recursive logs removal inspects the root without following it, so a `logs` junction is removed as a link.
- **Relocation.** Previous-install cleanup compares volume/file IDs on retained directory handles, including aliases
  through junctions or drive mappings. Matching IDs keep the newly installed files; unknown identity leaves the old
  folder with a warning. Distinct-folder deletion uses the resolved old path. Disabled shortcuts are owned by either
  the current or previous folder; foreign shortcuts stay untouched.
- **Uninstall handoff.** The installed launcher waits on a temp copy and propagates its exit code for silent and GUI
  removal. The child validates its live, older parent and retains handles to same-image waiting launchers,
  including the initiating UAC process forwarded explicitly (AppInfo ancestry is not assumed);
  shutdown excludes only those waiting PIDs. Their mapped installed image can be renamed aside and deleted at
  restart. The parent closes its log before the child starts, forwards files-only mode and close timeout, then removes
  the temp copy after the child exits. Both setup and uninstaller explicitly forward `--uninstall`.
- **Permissions.** Program files keep the folder's inherited ACL. `logs`, `captures`, `screenshots`, `benchmarks`
  and `config.ini` get a Users Modify ACE, because the app writes there as a standard user and only reads
  `config.ini`. The hard-link symbol store lives under `logs`, with a copy fallback.
- **Service and startup.** Setup runs `captureengine.exe --ce-installer-setup ...` (elevated parent required, live and
  older than the child), which reuses `Apply()` from `startup_control.cpp` for the exact tray semantics, rollback
  and preference record. The owner is the desktop shell's user, not setup's token. PawnIO uses the existing
  `--install-pawnio` role (hash + Authenticode verified there). Failures in these steps are warnings, never a rollback.
- **Installed Apps** record carries `CaptureEngineSetupRecord=1`; entries without it (another installer's, e.g. the
  rejected attempt's) are not trusted as our install folder.
- **Defaults.** Fresh: all shortcuts, service, autostart, PawnIO on; administrator mode off. Updates keep each earlier
  choice; defaults follow the folder the user picks until an option is touched.

## Testing without touching the machine

`--files-only --dir=<scratch>` runs the file transaction, config.ini rules, manifest and ACLs with no registry,
service, shortcut or driver work and no elevation; without `/S` it opens the real wizard off-screen. Native tests
drive fresh install, update, locked file rollback, a DLL loaded in a running process, junction refusal, process
closing/termination, uninstall and ACLs, plus external-file preservation and real temporary-copy completion/failure;
UI tests click through the pages with real mouse messages.
`--preview=<dir>` writes a BMP of every page. Never run setup without `--files-only` on a development machine
unless the point is to test registration: it registers a service, startup entry and Installed Apps record.

## Diagnostics

`%TEMP%\CaptureEngine-Setup.log` / `CaptureEngine-Uninstall.log` (overwritten per run, also to `OutputDebugString`).
Exit codes: 0 ok, 1 failed, 2 cancelled, 3 bad arguments.

## Open questions

- Hardware run: UAC flow with a standard user and alternate admin credentials; shortcuts for the shell user; service
  and Run-key toggles after install; PawnIO silent install; launch-as-user; the initiating UAC launcher of the uninstall hop.
  Files-only temporary-copy completion and failure are covered automatically.
- The service is a machine-wide name: uninstalling one copy removes a service another copy registered.
- Unsigned: SmartScreen/SAC will warn until the file is signed (needs a certificate-aware footer position).
