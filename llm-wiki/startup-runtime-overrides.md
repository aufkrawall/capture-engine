# Static runtime imports and creator interception

Last verified: 2026-10-10 (native/flow regressions and parser fuzzing; actual Steam/game retest pending).

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
- The first-resume wait also observes host shutdown/death. An unavailable startup hook has a bounded
  failure path with a stopped child and a specific diagnostic; it never leaves a suspended child running
  indefinitely. In-process patch transactions resuming their own peers bypass the pending-launcher mutex.
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
Steam-overlay/game compatibility. After installing the fresh build, verify normal Steam launches with
overlay on/off and a direct desktop launch: creator ready before game creation, redirect count nonzero,
one interposer physically mapped from the configured directory, then coherent core/plugin paths and
working capture/overlay/FG transitions. Starting CE after the game imports its interposer cannot replace
that already-resident image.
