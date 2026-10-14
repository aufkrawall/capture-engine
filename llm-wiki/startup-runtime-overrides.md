# Static runtime imports and creator interception

Last verified: 2026-10-14 (loader-init APC route, import ownership/publication gates, OTA policy through the selected takeover provider, UTF-8 paths and `FlowStreamlineTakeover.*`; creator role dormant; live game retest pending).

## Sources

- `hook/streamline/streamline_interposer_takeover.{h,cpp}`: two-phase in-process takeover of a statically imported foreign `sl.interposer.dll` (`ArmEarlyFromDllMain`, `EnsureCustomInterposerReady`, `GetModuleHandle*`/`GetProcAddress` redirection, superseded-module exclusion).
- `hook/streamline/streamline_takeover_decision.h`, `tests/test_streamline_takeover_decision.cpp`: entry-only ownership selection, concurrent-load arbitration and worker-only resident cleanup.
- `hook/streamline/streamline_takeover_publication.h`, `tests/test_streamline_takeover_publication.cpp`: nonblocking query, concurrent preparation and one coherent publication/binding contract.
- `hook/streamline/streamline_takeover_core_scan.h`, `tests/test_streamline_takeover_core_scan.cpp`: complete mapped-path inventory and semantic cached-core detection before arming.
- `hook/streamline/streamline_takeover_imports.{h,cpp}`, `streamline_takeover_import_coverage.h`, `hook/runtime/pe_file_exports.h`, `IATHook::detail::ForEachImportOf` (`hook/hooking/iat_import_table.h`), `tests/test_streamline_takeover_imports.cpp`, `tests/fuzz/fuzz_pe_file_exports.cpp`: import coverage gate and direct rebinding of unthunked imports.
- `hook/streamline/streamline_takeover_module_queries.cpp`: `GetModuleHandle*` redirection (split out of the takeover unit).
- `tests/flow/test_flow_streamline_takeover.cpp` with `tests/flow/fakes/startup/takeover_game_*.cpp` and the `takeover_resident/` fake interposer variant: end-to-end arming, activation, direct rebinding and coverage decline.
- `hook/runtime/runtime_override_paths.h` (`ce::runtime_override_path`), `hook/overrides/published_graphics_config.{h,cpp}` (`CurrentRuntimeOverridePaths`), `tests/test_runtime_override_paths.cpp`: UTF-8 override paths and immutable snapshots.
- `hook/runtime/graphics_runtime_module_policy.h`: `IsStreamlineInterposerModuleQuery`, `CanTakeOverForeignStreamlineInterposer`.
- `hook/runtime/dll_utils.h`, `tests/test_dll_version_read_policy.cpp`: `DllVersionReadPolicy::DirectPeOnly` rejects missing/unreadable resources without loader fallback, while ordinary file queries preserve it.
- `hook/hooking/iat_hook.{h,cpp}`: `RebindIATBaseline`, `RebindIATBaselineAllModules`, `IsOwnedStreamlineInterposerModule` owner recognition.
- `hook/streamline/streamline_ota_preferences.{h,cpp}`: shared 2.x preference-copy forwarding, retained takeover gates and `RetargetOriginalSlInit` for dynamic OTA routes.
- `common/platform/startup_imports.{h,cpp}`: bounded mapped-PE inspection and reversible import-name transaction.
- `common/platform/startup_launch_control.{h,cpp}`: creator policy, eligibility, named events and serialization.
- `captureengine/injection/injection_startup_launch.cpp`: creator discovery, attachment, deferred readiness service.
- `hook/runtime/main_startup_launcher.cpp`: native creation/resume boundary, profile selection, generation checks.
- `hook/runtime/main_dllmain.cpp`, `main_hookthread.cpp`, `main_injection.cpp`, `main_redirect.cpp`: early `DllMain` IAT gating, entry-gated takeover load and worker rollback, and isolated creator entry.
- `tests/test_graphics_runtime_module_policy.cpp`, `tests/test_iat_patch_serialization.cpp`, `tests/test_startup_imports.cpp`, `test_startup_launch_control.cpp`, `tests/flow/test_flow_startup_imports.cpp`.
- `tests/flow/fakes/startup/`: a genuinely statically linked game probe and an intermediate launcher.
- `tests/fuzz/fuzz_startup_imports.cpp`, `tests/fuzz/corpus/startup_imports/`.

## In-process two-phase `sl.interposer.dll` takeover (parent-free)

When creator-process hooks are dormant (so only whitelisted target processes are ever injected), an executable that statically imports `sl.interposer.dll` (e.g. `witcher3.exe` DX12 in session `20261010_225115`) already has its game-directory `sl.interposer.dll` mapped by `ntdll!LdrpInitializeProcess` before CE's `DllMain` runs, and its main thread calls `slInit` ~36 ms after `DllMain` returns (`22:51:36.111` vs `DllMain` exit at `22:51:36.075` and `HookThread` preload at `22:51:36.275`). Because `sl.interposer.dll`'s own `DllMain` only initializes CRT state and loads `sl.common.dll` + plugins lazily inside `slInit`, CE takes over the static import in-process in two phases:

1. **Phase 1 (`ArmEarlyFromDllMain` in `DllMain`, under the loader lock)**: runs at the very beginning of `InstallKernel32LoaderHooks("DllMain")` right after `ce::published_config::ResolveEarlyRuntimeOverridePaths()` and BEFORE the 10-pass `PatchIATAllModules` kernel32 scan (which otherwise stalls `DllMain` ~160 ms before `slInit` IAT slots are patched). Verifies `CanTakeOverForeignStreamlineInterposer` (configured `streamline_dll_path\sl.interposer.dll` exists, resident `sl.interposer.dll` is foreign, no canonical or NGX-cache hashed Streamline core is mapped, and both interposers share the same known Streamline major version `1.x` or `2.x`; cross-generation `1.x -> 2.x` remains owned by `streamline_bridge`). Reads the resident interposer's version resource from memory (`ModuleFileMajorVersion`) and the on-disk override's PE `.rsrc` `RT_VERSION` resource directly via `DllReadPeVersionResourceW` (`CreateFileW` + `ReadFile`, avoiding `GetFileVersionInfoW` / `LoadLibraryExW(LOAD_LIBRARY_AS_DATAFILE)` which stalls ~220 ms under the loader lock through Steam's `gameoverlayrenderer64.dll` hook), and reuses the single `EnumProcessModules` vector for both `ScanLoadedStreamlineCore` and `CollectInterposerImportingModules`. Snapshots the foreign interposer's exports as fallbacks and patches all `sl.interposer.dll`-importing modules' IAT slots to `InterposerTakeoverThunk<I>` (50 Streamline, DXGI, D3D11/D3D12 and Vulkan exports). `InstallKernel32LoaderHooks` also installs `HookedGetModuleHandleA/W/ExA/ExW` across non-Streamline modules.
2. **Phase 2 (`EnsureCustomInterposerReady` outside `DllMain`, entry-thunk gated)**: `HookThread` and `PlaceStreamlinePluginSet` call `EnsureCustomInterposerReady()` only to ensure takeover is armed when `DllMain` did not run it; they do **not** eagerly load `<streamline_dll_path>\sl.interposer.dll` before an entry thunk fires. Why: in `20261011_113848` (`witcher3.exe` p15696/p5516), when `resident!slInit` started before `TryArmTakeover` ran, `resident!slInit` spent ~250 ms inside `nvapi64.dll` and `WinVerifyTrust` before mapping `sl.common.dll`. Eagerly loading a second `sl.interposer.dll` on `HookThread` during that pre-`sl.common.dll` window bound `StreamlineHook` and all 18 `witcher3.exe` IAT slots to an uninitialized duplicate interposer while `resident` finished `slInit`, causing every subsequent `slIsFeatureSupported` / `slSetD3DDevice` / `slGetFeatureFunction` call to fail with `sl::Result::eErrorNotInitialized` (`21`). Instead, `LoadAndPublish` runs only on entry-thunk activation (`CanActivateTakeoverForExport(index, corePresence)`): `index == 0` (`slInit`) or `index >= 27` (pre-core DXGI/D3D11/D3D12/Vulkan proxy exports) when `ScanLoadedStreamlineCore() == CorePresence::Absent`. If a post-`slInit` export (`index == 1..26`, e.g. `slGetFeatureFunction`, `slIsFeatureSupported`, `slSetD3DDevice`) is called first or a Streamline core (`sl.common.dll` or `sl_common_*`) maps before an entry thunk claims custom ownership (`NoteTakeoverInterposerMapped` via `OverlayDllNotificationCallback` and `HookedLdrLoadDll`), `AbandonArmedTakeover` selects resident routing immediately and queues a `TakeoverRollbackRequest`. Only `ServicePendingRollback` on the hook worker restores resident `sl.interposer.dll` IAT bindings via `IATHook::RebindIATBaseline` (keeping an installed 2.x OTA `slInit` gate as a resident fallback), retargets `RetargetOriginalSlInit`, clears `g_SupersededInterposerModule`, and invokes `StreamlineHook::OnModuleLoaded` on the resident interposer so it is hooked in place.
3. **Module identity & hook routing**: `PatchLoadLibraryIatForLateLoadedModule` also calls `PatchModuleHandleIatForModule` on `sl.*` modules (which `PatchIATAllModulesFiltered` skips) so `sl.interposer.dll`'s internal `GetModuleHandleW(L"sl.interposer.dll")` + `GetModuleFileNameW` plugin-directory lookup resolves to `<streamline_dll_path>\sl.interposer.dll`. `DetourGetProcAddress` redirects queries on the superseded `HMODULE` via `RedirectInterposerModuleForGetProcAddress`, CE's internal callers resolve the active interposer via `ActiveInterposerOr`, and `StreamlineHook::InstallHooksForModule` plus `ce::ngx_drs::ForEachLoadedDlssDrsConsumer` skip `IsSupersededInterposerModule` so the dormant game-directory image never claims CE's single forward pointers.

### Loader-safe publication, sub-5 ms `DllMain` arming, and entry-thunk rollback (2026-10-11)

- Early override version reads explicitly select `DllVersionReadPolicy::DirectPeOnly`. A failed raw PE read
  returns an unknown version and declines takeover; it never reaches `GetFileVersionInfoSizeW` or
  `GetFileVersionInfoW`. Previously the direct reader's empty result still invoked those APIs under the
  loader lock. The native regression counts fallback calls for a versionless/missing file (zero in strict
  mode, one for the ordinary query) and verifies an actual system PE version resource still reads directly.
- Module queries only read `RuntimePublication::Query`; they never load or wait for takeover.
  Before publication, named `GetProcAddress` lookups on the superseded interposer return the
  supported export's takeover thunk. Invoking an entry thunk (`slInit` or pre-core proxy export)
  performs the lazy load. A publication racing the HMODULE redirect is rechecked at export lookup
  so cached handles cannot escape.
- `TakeoverDecision` selects custom or resident ownership with one atomic transition before
  loading begins. Only an eligible entry thunk may initially claim custom ownership; worker and
  concurrent post-init calls may join that claim. Once claimed, core notifications or post-init
  calls cannot roll it back while loading/publication/binding is still in progress. A final
  `RuntimePublication` failure releases ownership to the resident. Controlled tests cover both
  ownership outcomes and the blocked-loader window.
- `EnsureCustomInterposerReady` holds no takeover mutex across loading, pinning, export lookup or
  binding. Concurrent callers let Windows serialize the same absolute DLL path, prepare local
  export tables, then one finalizer binds direct imports outside application locks. Peer entry calls join
  that finalizer after leaving the loader; queries remain nonblocking. Only successful binding permits
  the short publication lock (data stores and slInit retargeting). No override entry can initialize
  before direct imports migrate. A final failure and competing success cannot publish contradictory targets.
- `CanActivateTakeoverForExport` and `AbandonArmedTakeover` prevent the split-brain duplicate
  interposer failure observed in `20261011_113848`: if the game's main thread entered the
  resident `sl.interposer.dll!slInit` before `TryArmTakeover` patched IATs, `HookThread` never
  maps a second uninitialized `sl.interposer.dll` while `resident!slInit` is inside `WinVerifyTrust`,
  and as soon as `resident!slInit` maps `sl.common.dll` (or the game calls a post-`slInit` `sl*`
  export), pending takeover selects resident routing immediately. Worker cleanup restores
  imports and hooks that interposer in place; loader notifications never acquire the
  Streamline install mutex or execute import restoration. The hook worker services requests
  at bootstrap and each event/monitor pass; `StreamlineHook::Init` also drains them before
  taking its init mutex, including the isolated flow harness. Cached thunks keep forwarding
  to their resident fallbacks while cleanup is pending, and the superseded-module check
  immediately allows resident detection. The duplicate-load exception in
  `ShouldAllowTakeoverInterposerLoad` also consults ownership: resident selection vetoes
  custom loads even while the old armed flag or load counter remains set before cleanup.
  `StreamlineTakeoverDecision` covers one-shot deferred
  cleanup while the install mutex is held and custom-core notifications retaining ownership.
- `ScanLoadedStreamlineCore` enumerates all mapped paths before arming. The redirect policy's
  semantic classifier recognizes canonical `sl.common.dll` and NGX
  `NVIDIA/NGX/models/sl_common_0/.../<hash>.dll`. A basename query missed initialized cached cores.
  Enumeration failure or unresolvable/truncated paths refuses takeover (`coreScan=unresolved`);
  a known core also refuses it. `StreamlineTakeoverCoreScan` covers these boundaries, entry-thunk
  gating, and resident-wins rollback.

### Import coverage and direct rebinding (2026-10-14)

- `sl.interposer.dll` 2.11 (installed testapp copy) exports 262 names: the sl* API, D3D11/D3D12/DXGI proxies and
  about 200 Vulkan functions. The thunk table gates 50 (the sl* API, the DXGI/D3D12 proxies, nine Vulkan entry
  points, and since 2026-10-14 `D3D11CreateDevice`/`D3D11CreateDeviceAndSwapChain`). Before, every other import
  stayed bound to the dormant game-directory copy after activation, so a D3D11 or Vulkan title linked against
  `sl.interposer.lib` ran half on each interposer.
- Arming now measures every importing module (`MeasureImportCoverage`, `IATHook::detail::ForEachImportOf`) against
  the thunk table and the override's export table, read from disk by `ce::pe_file_exports::ReadFileExportNames`
  (plain `CreateFileW`/`ReadFile`, DllMain-safe; no `LoadLibraryEx AS_DATAFILE`, no `SEC_IMAGE` section). It
  declines (`DeclinedFinal`, log `takeover declined (...): <verdict> - ... lacks N (first: X)`) on malformed import
  tables, ordinal imports (ordinals differ between builds), descriptors without a lookup table (only resolved
  addresses), an unreadable override export table, or any imported name the override lacks (an older same-major
  build). A same major version is therefore necessary but no longer sufficient.
- `CheckImportBindings` checks exact resident predecessors before arming and exact takeover gates at
  activation. A foreign wrapper can retain the resident export despite complete override export coverage;
  preserve that wrapper and decline before loading/initializing the override. Ownership changes after arming
  get the same refusal and worker rollback. CE wrappers without a proven transferable predecessor also decline.
- `ngx_ota=off` must not stack `Hooked_slInit` over an armed takeover's IAT gate. The exact-gate ownership
  check added in `c0f63371` rejected that CE-owned stack before the override loaded. `InstallSlInitRouteIfConfigured`
  now retains the static gate and registers only the dynamic route while takeover is armed. The gate forwards
  a 2.x `slInit` through `ForwardSlInit2x` with its already-selected target; the ordinary OTA wrapper shares the
  same preference-copy policy. The major version captured at arming leaves the 1.x signature untouched.
  Worker rollback retains an installed 2.x OTA `slInit` gate as a cached resident fallback; later initialization
  still clears OTA preferences, and resident selection prevents it from loading the override again.
  No general exemption for CE or foreign wrappers was added to the ownership checks.
- `FlowStreamlineTakeover.OtaControlPreservesTheStaticTakeoverGate` failed before this correction: the
  configured copy never answered the direct identity import. Static and dynamic variants now verify exact
  provider/core paths, zero resident `slInit` calls, selected-provider call counts, cleared OTA bits with other
  flags retained, unchanged caller memory, repeated calls/fresh lookups and unknown-layout passthrough. The
  foreign-owner scenarios also run with OTA disabled, before and after arming. The after-arming scenario
  services worker rollback and calls resident `slInit` again; restoring its raw import deliberately fails
  the preference-bit assertion (72 instead of zero).
- Session `20261014_183445` (0.1.7143) armed 18 imports in one module at 18:35:00.813; its OTA route patched
  `slInit` again at 18:35:01.000. The core mapped at 18:35:01.206 and takeover rolled back at 18:35:02.148.
  There is no intercepted `slInit`, forwarded thunk or activation-ownership rejection in that game log, so it
  does not alone establish the reproduced audit failure as that run's cause. A resident initialization already
  in flight or a bypass remains possible. Worker rollback now logs `slInitGateEntered`, `otaRouteInstalled`
  and `otaSlInitSeen`; OTA installation logs `takeover gate retained`. A live retest is still required.
- `BindUnthunkedImports` prepares all direct slots and uses `RebindIATBaselinesTransaction` under the existing
  IAT patch lock: validate every predecessor before writing; CAS replacements; rollback owned writes on a
  protection/conflict failure. `iat_baseline_transaction.h` holds the tested transaction policy. Thunked imports
  retain their gates; resolved targets publish only after binding succeeds. Failure publishes no custom targets
  and selects resident fallback. Duplicate names in separate IAT slots migrate together.
- Late modules retain the existing `RebindUnthunkedImports` pass after activation. Direct exports preserve their
  original ABI, including Vulkan float arguments and exports with more than twelve arguments.
- Regressions: `IATHookTransactionTest` proves foreign-slot preservation and rollback after a rejected write;
  `StreamlineTakeoverPublication` holds binding while a peer joins and verifies queries/targets stay unpublished;
  `FlowStreamlineTakeover` runs the real hook against foreign owners before and after arming.
- Thunks forward twelve integer/pointer arguments (`kTakeoverThunkArgumentCount`, widest:
  `D3D11CreateDeviceAndSwapChain`); trailing unused slots read the caller's frame and are ignored by the callee.
  The positional sl*/proxy partition of `kTakeoverExportNames` is a `static_assert`.
- Once the override owns the runtime, a thunk whose export the override lacks (a dynamic lookup the coverage gate
  cannot see) fails with `E_FAIL` and a rate-limited log instead of reaching the uninitialized resident copy.
- Residual race, deliberately not "fixed" by policy: a proxy entry export (index >= 27) may still claim the override
  while the resident `slInit`, entered before the imports were gated, is inside `WinVerifyTrust` (core not yet
  mapped). Restricting the claim to `slInit` would instead split games that create DXGI/D3D objects before `slInit`.
  It is now diagnosable: a core mapped from the dormant interposer's own folder after a claim logs
  `SPLIT RUNTIME ... claimed by <export>`.
- Resident-selected thunks no longer rescan the module list per call (`TakeoverDecision::IsPending`), and rollback
  requests wake the hook worker (`g_hCheckHooksEvent`).

### Arming before the game's first `slInit`: the loader-init APC route (2026-10-14)

- Outcome of every retained `witcher3.exe` run Oct 13-14 tracked injection timing, not CE build:
  ACTIVE for p7728 (0.1.7134) and p26376 (0.1.test), rollback `Streamline core mapped before takeover entry`
  for p20004 (0.1.7134), p28948/p164 (0.1.test), p38904 (0.1.7143), p35192 (0.1.7145, `slInitGateEntered=0`),
  declined `coreAlreadyLoaded=1` for p14304/p24024 (slInit finished before DllMain). No audit commit caused it.
- Mechanism: a remote thread created during loader initialization is held until initialization ends and is
  then released together with the initial thread, which runs straight into the image entry point. In p26376 the
  game's `slInit` hit the gate in the same millisecond DllMain started (won by ~0 ms); p7728's DllMain waited
  608 ms for init and won; p35192 lost (core mapped 318 ms after the slot was patched, gate never entered).
- Fix (`common/platform/loader_init_apc.{h,cpp}`, called first in `InjectionManager::InjectImpl` for native x64,
  non-creator injections): suspend the target's initial thread (earliest creation time), read `PEB.Ldr` (+0x18)
  and `CrossProcessFlags` (+0x50, bit 1 `ProcessInitializing`), and queue `LoadLibraryW` as an APC only when
  `Ldr == 0` (not started) or the bit is set (initializing). ntdll's `LdrpInitialize` delivers it via
  `NtTestAlert` after process init and before the entry point, so DllMain arms the takeover before any game
  code. Measured on this machine (Windows 26300): static-import DllMain sees `0x3`, the init-end APC sees `0x1`
  and runs before `main`. Any other phase keeps the remote thread (no stale APC). Only the whitelisted target
  is touched; no parent/launcher hooking.
- Remaining limits: a target first seen after initialization (poll latency up to 250 ms) still races or loses;
  WoW64 targets keep the remote thread. The injector logs one line per injection:
  `[Inject] Load route for <exe> (PID): loader phase=<phase> initialThread=<tid> processAge=<ms> -> <route>`.
- Regression: `LoaderInitApc.*` holds real `cmd.exe /c exit 7` children created suspended and stopped at a
  loader debug event, queues `ExitProcess(42/43)` and asserts that exit code (the APC ran before the entry
  point); `InjectLifecycleSourceTest.TargetWakeupsExistBeforeRemoteOrApcLoadCanStart` pins the route order.
  Live Witcher 3 confirmation pending.

### UTF-8 override paths (2026-10-14)

Config values are active-code-page text (`common/config/config_ini_reader.h`), while the loader hooks, the DLL
notification and every path comparison are UTF-8. A non-ASCII override folder passed the ANSI existence check,
failed the `CP_UTF8` decode used for the load, and made CE's own `sl.common` look foreign to
`NoteRuntimeModuleLoadedForOverridePolicy`, which switched the whole sl.* redirect off. The four override paths
are now converted once into immutable UTF-8 snapshots (`CurrentRuntimeOverridePaths`: early from DllMain, local
after the config load and each inherited-renderer sync; superseded snapshots are never freed because loader-lock
readers hold them without a lock), and file-system access goes through `ce::runtime_override_path` wide helpers.
The ANSI LoadLibrary hooks convert their request to UTF-8 and load redirects through the wide originals. Module
paths are read without a `MAX_PATH` cap (`ModulePath`), so a deep install folder no longer makes the core scan
`unresolved`. A first local publication that differs from the early one logs `this process's profile differs
from the injector's early-published override paths` (the host publishes one slot, for its active target).

## Historical creator role and contract

**Dormant since 2026-10-10** (`ce::startup_launch::kCreationOnlyRoleDormant`, commit `76413000`): nothing below
runs in production; `[Injection] startup_creator_hosts` has no effect. The machinery and its flow tests stay
in-tree until removed or replaced (see the parent-free alternatives at the end of this page).

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

## Creator role and handoffs (dormant)

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
- Creator processes are touched only while substitution is actually possible: some config section must
  set a non-empty `streamline_dll_path` (`AppConfig::graphics.streamlineDllPathConfigured`, computed in
  the whitelist text pass), and `[Injection] startup_creator_hosts` scopes the host set (`auto` = shells
  + storefronts + generic launcher names, `storefronts` = never explorer/cmd/powershell/pwsh, `off` =
  no creator role at all). Without substitution, no parent process ever sees CaptureEngine code.
- The role detaches again: on host shutdown, or when `UpdateConfig` sees that no substitution is
  configured any more, the injector signals the per-creator `Detach` event and the creator's own worker
  removes both entry patches (`InlineHook::Remove`, quiesced) and confirms on `Detached`. When no peer
  thread executes inside the module, the worker then releases the injector's remote-LoadLibrary
  reference and its own (the role is never pinned) via `FreeLibraryAndExitThread`, so the module leaves
  the creator process completely. A removal that cannot quiesce keeps the installed pass-through hooks
  and the mapping (the old behaviour), and a worker that stays alive can reinstall on a later `Role`.
  Replacement hosts therefore normally re-attach freshly instead of adopting; adoption remains the
  fallback for creators whose hooks could not be removed. Worker retries are driven by explicit
  activation events. ABI-incompatible/older resident DLLs need process restart (they lack `Detach` and
  simply retain, as before).

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
`New launcher ... first resume` / `... resumed unintercepted`, `Creator ... detach
completed/requested/unavailable`, `[StartupImport] Detaching ... creation-only creator(s)`, incompatible
generation/machine/path refusals and startup/rollback failures.
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

2026-10-12: pending dynamic `slInit` takeover composes with OTA interception; mapped identity
and ready publication have separate contracts. NGX logging restores only owned environment values
and excludes launcher/blacklisted roles. Its policy remains independent of CE `log_level`. See
[transaction and retirement invariants](transaction-and-retirement-invariants.md) for sources/tests.
