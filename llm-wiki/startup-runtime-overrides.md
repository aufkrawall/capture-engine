# Static runtime imports and creator interception

Last verified: 2026-10-10 (creator safety hardening: unlisted children take the caller's creation path,
missed launcher handshakes resume instead of stopping; actual Steam/game retest pending).

## Sources

- `common/platform/startup_imports.{h,cpp}`: bounded mapped-PE inspection and reversible import-name transaction.
- `common/platform/startup_launch_control.{h,cpp}`: creator policy, eligibility, named events and serialization.
- `captureengine/injection/injection_startup_launch.cpp`: creator discovery, attachment, deferred readiness service.
- `hook/runtime/main_startup_launcher.cpp`: native creation/resume boundary, profile selection, generation checks.
- `hook/runtime/main_dllmain.cpp`, `main_hookthread.cpp`, `main_injection.cpp`: isolated creator entry and game bootstrap.
- `tests/test_startup_imports.cpp`, `test_startup_launch_control.cpp`, `tests/flow/test_flow_startup_imports.cpp`.
- `tests/flow/fakes/startup/`: a genuinely statically linked game probe and an intermediate launcher.
- `tests/fuzz/fuzz_startup_imports.cpp`, `tests/fuzz/corpus/startup_imports/`.

## Root cause and contract

Session `20261010_112108`, build 0.1.7077, had the configured core and every observed SL plugin
loaded from the override directory, while the interposer stayed in the executable directory.
The executable's PE imports name `sl.interposer.dll`. That image maps during Windows process
initialization, before the ordinary remote-LoadLibrary attachment. A later LdrLoadDll hook cannot
replace it. Early APC loading before application entry also does not provide static-import replacement.

For a whitelisted, same-generation runtime with `streamline_upgrade=false`, rewrite the child's
existing import to the absolute configured interposer before its loader runs. Keep the original lookup
and address tables. Windows maps and binds one physical interposer; do not implement API takeover,
unload a resident import, edit Windows' loader graph, alter game files or use a generation bridge here.
`streamline_upgrade=true` keeps the existing opt-in 1.x/2.x bridge contract. `ngx_ota=on` keeps its
existing driver-runtime contract. Unknown/mismatched generations and architectures are refused.

## Creator role and handoffs

- Before normal target discovery, existing desktop shells and storefront clients acquire an explicit
  creation-only role. Generic launcher names are covered too. Explicit capture/overlay targets retain
  their normal role. Only same-user/session, non-AppContainer processes whose mitigations permit the
  DLL and patching qualify; critical service/UI processes are not blanket injection targets.
- Every child is classified from its image name in the caller's creation parameters BEFORE the kernel
  commits the process (`ce::startup_launch::ClassifyChild`, names from
  `RTL_USER_PROCESS_PARAMETERS::ImagePathName`; the block lives in the creating process). Targets get
  the transaction below, launch hosts the creator handoff, and everything else the caller's creation
  path byte-identically: no gate hold during creation, no forced suspension, no post-creation work.
  An unresolved name falls back to the post-creation classification. Non-whitelist software (anti-cheat
  titles, overlays, arbitrary tools) is therefore never observable as touched.
- The explicit role returns from DllMain before crash-handler, loader, graphics, capture or overlay
  setup. Its worker installs published native `NtCreateUserProcess` and `NtResumeThread` predecessors.
  Creation structures/attributes remain opaque and unchanged. Readiness requires both installed hooks.
- Create the child suspended, select its own INI profile from the current host's installation, validate
  its interposer generation/machine, and complete the import transaction before releasing the creator
  gate. The caller's requested suspended state and native outputs/Win32 errors retain their semantics.
- Regular host injection and the in-process child injection worker wait for the creator gate before
  starting a remote loader thread. That is synchronization with the transaction, not an injection delay.
- A newly created launcher publishes role/control objects before the native creation gate releases.
  At its first resume, signal `ResumeRequested` after Win32/CSRSS setup and wait for its creation hook's
  `Ready` event. The inject host defers attachment until that request is signaled. An intermediate
  launcher therefore cannot outrun process discovery and start an unprotected game.
- The first-resume wait also observes host shutdown/death and is bounded (the timeout is test-adjustable).
  A missed handshake RESUMES the launcher unintercepted with a `[StartupImport] New launcher ... resumed
  unintercepted` diagnostic: the handshake protects the launcher's game child (which then falls back to
  ordinary discovery), never the launcher itself, so a hiccup cannot stop software that was never a
  target. The only remaining terminations are the rollback contract below and a failed resume of a child
  this code itself suspended. A creation gate that cannot be acquired no longer fails the caller's
  process start: the child is created unintercepted with the caller's original flags and a diagnostic.
  In-process patch transactions resuming their own peers bypass the pending-launcher mutex.
- Creator clients do not count as active captures. Host shutdown clears their active event and retains
  callable code for foreign chains; replacement hosts can adopt ready resident creators. Worker retries
  are driven by explicit activation events. ABI-incompatible/older resident DLLs need process restart.

## Import transaction

Find the fresh executable through public virtual-memory queries, inspect PE32/PE32+ headers and bounded
import descriptors/names, and allocate a read-only replacement name within DWORD RVA reach of its base.
Invalidate bound imports and change the selected descriptors' name/timestamp fields while preserving
their lookup/address RVAs. No remote thread or APC runs during this operation. A bound IAT without a
separate lookup table is unsupported. Configured directory/file paths retain the existing path contract;
ANSI import names require lossless ACP text or an existing 8.3 path.

On any partial write/protection failure, restore original descriptors, bound-directory data and page
protections, then release unused remote memory. A failed rollback retains the referenced name and
requires terminating the child before any corrupted imports execute. Do not fabricate a failed native
creation result after the kernel has already committed opaque success records/handle ownership.

## Verification and diagnostics

Focused loop:
`python build.py --incremental --tests-only --flow-tests --run-tests --gtest-filter=StartupImports.*:StartupLaunchControl.*:FlowStartupImports.* --skip-updates --concise`.

The baseline probe maps exactly one game-directory interposer. Manual substitution and ordinary launch
map exactly one override image. Flows cover requested suspension, dormant host, explicit legacy upgrade,
OTA on, unlisted child, child profile, configured DLL file, partial-write rollback, failed rollback and a
new intermediate launcher. The broker fixture waits on creation/resume events and injects the actual flow
DLL into that launcher; it contains no sleeps. Every child has bounded exit/termination and handle cleanup.
Native tests check both PE formats, malformed/truncated imports, creator synchronization and source order.
The registered fuzz harness uses synthetic PE32/PE64/malformed seeds, never vendor binaries.

Stable prefixes: `[StartupImport] Creation-only launcher ready`, `Creation-only hook`, `Child ... status`,
`New launcher ... first resume`, incompatible generation/machine/path refusals and startup/rollback failures.
Each is associated with a distinct bootstrap/child event; no frame-path logging is added.

The fixtures establish physical import resolution and launch handoffs. They do not establish actual
Steam-overlay/game compatibility. `FlowStartupImports.MissedLauncherHandshakeResumesInsteadOfStoppingIt`
(regression for the stopped-launcher defect: fails with the old kill path) and the unlisted-child policy
record (`CEFlow_GetLastCreationPolicy`) pin the creator safety contract. After installing the fresh build, verify normal Steam launches with
overlay on/off and a direct desktop launch: creator ready before game creation, redirect count nonzero,
one interposer physically mapped from the configured directory, then coherent core/plugin paths and
working capture/overlay/FG transitions. Starting CE after the game imports its interposer cannot replace
that already-resident image.

## Parent-free pre-loader alternatives (research, 2026-10-10)

Goal: run the import rewrite inside the target without touching creator processes at all. Empirical
results from a probe pair (static `sl.interposer.dll` import + logging shim), all runs self-cleaning:

- HKCU `Image File Execution Options` is ignored by the loader (a `Debugger` value under HKCU did not
  affect the target). Only HKLM IFEO is real, so every IFEO-based mechanism needs elevation at
  registration time (the elevation service is the natural owner).
- HKLM IFEO `VerifierDlls` + `GlobalFlag=0x100` is **disqualified**. A plain DLL does not initialize
  (ntdll's AVRF expects a verifier provider with verified exports: "cannot find an entry point for
  provider", "provider ... passed an invalid descriptor", the documented `VerifierStopMessage` export;
  it also refuses DLLs "from arbitrary location"), and the target fails with `0xC0000142`. Decisively,
  a **missing** verifier DLL fails the target's startup the same way: a stale registration would brick
  every registered game after an uninstall or move. `GlobalFlag` alone is harmless.
- Remaining candidate: an apphelp custom SDB using the system `InjectDll` shim (shimeng's
  `SE_InstallBeforeInit` runs before the import walk; missing shim DLLs are expected to be non-fatal,
  which still needs verification). A Rosetta sample exists on stock Windows:
  `C:\Windows\AppPatch\CustomSDB\{22221111-1111-1111-1111-111111111111}.sdb` ("Patch SDB Test App").
  Decoded so far: 8-byte prologue `03 00 00 00 00 00 00 00`, magic `sdbf`, then tagged values; the tag
  is a little-endian WORD with the data type in bits 12-15 (3=WORD, 4=DWORD, 5=QWORD, 6=STRINGREF
  (offset into the string-table list), 7=LIST (4-byte byte length), 8=STRING (4-byte byte length +
  UTF-16 incl. NUL), 9=BINARY). Top level: LIST 0x7802 (database head: TAG_LIBRARY list 0x7803 with a
  constant preamble `02 38 07 70 03 38 01 60 16 40 01 00 00 00` to clone verbatim, plus a
  reversed-prefix string index), LIST 0x7001 (database attributes incl. the DB GUID, and EXE entries),
  LIST 0x7801 (string table). Registration side: `HKLM\...\AppCompatFlags\Custom\<exe>` names the SDB
  GUID, the file lives in `C:\Windows\AppPatch\CustomSDB\{GUID}.sdb` (sdbinst.exe owns both).
- The shim DLL itself is mechanism-independent: in its DllMain it resolves the host config, applies the
  same same-generation/machine policy as the creator transaction, and runs the local import rewrite via
  `ce::startup_imports::Redirect(GetCurrentProcess(), ...)`, which works unchanged in-process. A
  self-check reports whether it ran before or after import binding (the make-or-break ordering).
