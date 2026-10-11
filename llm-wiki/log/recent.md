# llm-wiki Log

### 2026-10-11 - Attested stable release v0.1.7102 and persistent runner sync fix

- Pre-release `--no-build --lint` caught `tests/test_config.cpp` at 817 lines (above the 800-line ceiling);
  rebalanced WGC/capture tests into `tests/test_config_part3.cpp` (727 lines) and `[Graphics]` sharpen tests
  into `tests/test_config_gamma.cpp` (164 lines), bringing `tests/test_config.cpp` to 656 lines (`05fb4ec6`).
- First `release-stable.yml` dispatch (`38140961568`) failed in `Sync repository (persistent workspace, never cleans)`
  because `external/` in the runner workspace is junctioned to the dev `external/` tree and commit `f3d86683`
  modified tracked `external/fidelityfx/README.md`, making it appear locally modified to the runner's git index.
  Updated `.github/workflows/release-stable.yml` and `tools/tests/test_workflow_security.py` to use
  `git switch --force -C main $env:GITHUB_SHA`, which reconciles tracked files without touching ignored trees or
  junctions (`7dfc5e11`).
- Dispatched `release-stable.yml` run `38141479599` (`version=0.1.7102`, `attest=always` on `7dfc5e11`). Built
  all 8 FFmpeg source dependencies and custom FFmpeg from source, passed `--verify --verify-clean --portable-archives`
  (full native suite, 17 Python tool self-test groups, 52 FG flow scenarios, ASan/UBSan sanitizer regression pass,
  1,052-TU clang-tidy ratchet, PE hardening and PDB privacy scrub), attested all 6 release assets via Sigstore
  (`gh attestation verify` confirmed), published `v0.1.7102`, verified `release-log-cleanup.yml` (`38145153028`)
  purged the self-hosted run log, and promoted `CHANGELOG.md`.

### 2026-10-11 - Keep Streamline takeover ownership coherent and defer resident hook installation

- Review of `e66054e5` exposed two additional startup windows without changing its entry-only
  activation contract. An entry blocked in runtime loading could lose takeover to a concurrent
  post-init call, then publish the custom runtime after resident imports/hooks were restored.
  `TakeoverDecision` now atomically selects ownership before the loader; peers join the selected
  override and only a final publication failure selects resident fallback after a custom claim.
- A resident-core notification previously performed import restoration and installed Streamline
  hooks under the loader lock. That takes the module-install mutex also held by workers across
  loader-dependent lookups. Notifications now select resident routing and queue cleanup only;
  bootstrap/monitor service and `StreamlineHook::Init` drain it outside the loader callback.
  Superseded-module and active-interposer queries recognize resident ownership before cleanup,
  so cached thunks already forward to the resident and detection can inspect that runtime.
  Late-load permission uses the same ownership decision; a pending resident cleanup cannot
  grant the duplicate-interposer exception merely because its old armed flag is still set.
- Controlled production-function reproductions: old callback could not return while the install
  mutex was held; fixed callback returned with zero hook installs, followed by exactly one worker
  install and a resident call with zero custom loads. The concurrent custom-load case reaches the
  override on both calls with no resident hooks or rollback. Focused native policy/publication/
  decision tests and changed-product syntax checks pass. Live commercial-game retest remains pending.
- Validation caveat: one unfiltered 0.1.7097 run failed the GTA-style FSR switching flow
  (2 uncovered presents, 2,226 output-owner violations out of 2,400 outputs), despite same-batch
  draw-success entries. The scenario configures no Streamline override and never arms interposer
  takeover; Streamline initialization/detection succeeded. After rebuilding the harness, its
  isolated run and three concurrent copies passed. Cause and baseline status remain unproven;
  no FSR routing change was made to hide it. The failed trace was preserved locally for follow-up.
- Final clean 0.1.7098 closing gate passed the full native suite, Python tool self-tests and all
  52 FG flow scenarios. Both hook architectures were rebuilt and verified; PE import closure and
  shipped-artifact privacy checks passed. The fresh setup package has version `0.1.7098` and
  39,413,456 bytes. Optional integration/sanitizer/fuzz runs and live-game validation were not run.

### 2026-10-11 - Fix Streamline interposer takeover race and split-brain duplicate in The Witcher 3

- Session `20261011_113848` (`witcher3.exe` p15696 and p5516, build 0.1.7093) broke Streamline detection:
  every `slGetFeatureFunction`, `slIsFeatureSupported` and `slSetD3DDevice` call returned `result=21`
  (`sl::Result::eErrorNotInitialized`).
- Root cause:
  1. `witcher3.exe`'s main thread (`WinMain`) reads its static `__imp_slInit` IAT slot without touching
     `LdrpLoaderLock`, so it can enter the game-directory `sl.interposer.dll!slInit` while CE's `DllMain`
     is still running. Inside `DllMain`, `InstallKernel32LoaderHooks("DllMain")` spent ~159 ms running
     10 `PatchIATAllModules` passes across ~90 modules, and `TryArmTakeover("DllMain")` then called
     `DllFileMajorVersion(overridePath)` -> `GetFileVersionInfoW` -> `LoadLibraryExW(LOAD_LIBRARY_AS_DATAFILE)`,
     stalling another ~220 ms through Steam's `gameoverlayrenderer64.dll` hook (plus ~21 ms from
     `3a7f92ef`'s `ScanLoadedStreamlineCore`), totaling ~380 ms before `slInit` was patched.
  2. When `resident!slInit` started during that 380 ms window, it spent ~250 ms inside `nvapi64.dll` and
     `WinVerifyTrust` before mapping `sl.common.dll`. `TryArmTakeover("DllMain")` ran in that pre-`sl.common.dll`
     window (seeing `CorePresence::Absent`) and armed takeover *after* `resident!slInit` had already been
     entered. `HookThread` then eagerly called `EnsureCustomInterposerReady()` 15 ms later (still before
     `sl.common.dll` mapped), loading a second `sl.interposer.dll` (`00007FFF2BCC0000`) and rebinding all
     18 `witcher3.exe` IAT slots and `StreamlineHook` to the uninitialized duplicate while `resident`
     (`00007FFF3D190000`) completed `slInit`.
- Fix:
  - `hook/runtime/dll_utils.h`: added `DllReadPeVersionResourceW` to parse the PE `.rsrc` `RT_VERSION` (`16`)
    resource directly via `CreateFileW` + `ReadFile` without calling `GetFileVersionInfoW` /
    `LoadLibraryExW(LOAD_LIBRARY_AS_DATAFILE)`.
  - `hook/runtime/main_injection.cpp` & `hook/streamline/streamline_interposer_takeover.cpp`: moved
    `ResolveEarlyRuntimeOverridePaths()` + `ArmEarlyFromDllMain()` to the very start of
    `InstallKernel32LoaderHooks("DllMain")` before the 10-pass kernel32 IAT scan, and reused the single
    `EnumProcessModules` vector for both `ScanLoadedStreamlineCore` and `CollectInterposerImportingModules`,
    eliminating ~380 ms of `DllMain` delay before `slInit` IAT gating.
  - `hook/streamline/streamline_takeover_core_scan.h`, `hook/streamline/streamline_interposer_takeover.cpp`,
    `hook/runtime/main_overlay_detect.cpp`, `hook/runtime/main_redirect.cpp`: gated `LoadAndPublish` behind
    entry-thunk activation (`CanActivateTakeoverForExport`: only `slInit` index `0` or pre-core proxy
    exports `>= 27` when `CorePresence::Absent`) so `HookThread` never eagerly maps a duplicate interposer,
    and added `AbandonArmedTakeover` rollback (restoring IAT slots to the resident interposer and invoking
    `StreamlineHook::OnModuleLoaded` on it) when a Streamline core maps or a post-`slInit` export (`1..26`)
    fires while takeover is still armed and inactive.

### 2026-10-11 - Fix three recent-commit regressions in GPU lifetime and Streamline startup

- Borrowed D3D12 lists retain a separate slot for every post-process recording until actual ECL
  completion or explicit abandonment. An append previously cleared the earlier slot; resizing the
  second target freed source/SRV objects still referenced by the first commands. WARP debug-layer
  failure was reproduced before the fix; multi-target readback and explicit reset/discard tests pass.
- Streamline module queries only read prepared publication state; early named dynamic exports retain
  their takeover thunk. Runtime loading/preparation no longer holds the takeover mutex, and only local
  data/pointer stores are serialized at publication. Controlled tests cover blocked loading, concurrent
  preparation, single binding and both success/failure races; production source guards fail on old code.
- Arming scans every module path with the existing semantic core classifier, recognizing hashed
  `NVIDIA/NGX/models/sl_common_0` images. Existing canonical cores and incomplete scans also refuse
  replacement. Core-scan regression cases and the pre-arm source guard pass.
- Full product build resumed successfully as 0.1.7093 after preserving GetProcAddress's native lookup
  and dormant pass-through contract. Explicit no-build verification passed the full native suite and
  Python tool self-tests; a separate cached flow run passed all 52 scenarios. Installer: 39,377,824
  bytes. Product/PE/privacy checks and task-owned secret scans passed; live game/hardware retest remains
  pending. Ordinary resume needs explicit test flags (already documented in `build.py.md`).
- Current invariants and source/test anchors: `post-processing-sharpen.md`,
  `startup-runtime-overrides.md`. No game-specific compatibility rule or GPU wait/copy was added.

### 2026-10-10 - In-process two-phase `sl.interposer.dll` takeover for static-import games

- Witcher 3 DX12 session `20261010_225115` (with parent/creator hooks dormant so only whitelisted target
  processes are injected) mapped `H:\SteamLibrary\steamapps\common\The Witcher 3\bin\x64_dx12\sl.interposer.dll`
  via the executable's static import table during kernel process initialization, and the main thread
  (`T6154`) called `slInit` 36 ms after `DllMain` returned (`22:51:36.111` vs `22:51:36.075`), 164 ms
  ahead of `HookThread`'s `PreloadConfiguredGraphicsRuntimeDlls` (`22:51:36.275`).
- Added `hook/streamline/streamline_interposer_takeover.{h,cpp}`, `CanTakeOverForeignStreamlineInterposer`
  and `IsStreamlineInterposerModuleQuery` in `hook/runtime/graphics_runtime_module_policy.h`,
  `IATHook::RebindIATBaseline` in `hook/hooking/iat_hook.{h,cpp}`, and `RetargetOriginalSlInit` in
  `hook/streamline/streamline_ota_preferences.{h,cpp}`:
  - Phase 1 (`ArmEarlyFromDllMain` in `DllMain`, right after `ResolveEarlyRuntimeOverridePaths` and
    before `InstallSlInitRouteIfConfigured`): gates all `sl.interposer.dll` IAT slots to
    `InterposerTakeoverThunk<I>` while the loader lock is held, provided no `sl.common.dll` core is
    loaded yet and the resident and override `sl.interposer.dll` share the same known Streamline major
    generation (`1.x` or `2.x`).
  - Phase 2 (`EnsureCustomInterposerReady` early on `HookThread` right after `InstallLowLevelLoaderHook`,
    or lazily on first thunk/module query): loads and pins `<streamline_dll_path>\sl.interposer.dll`,
    permanently rebinds all `sl.interposer.dll` IAT slots to the custom exports via `RebindIATBaseline`
    (retargeting any stacked `Hooked_slInit` route), patches the custom interposer's own
    `LoadLibrary*`/`GetModuleHandle*`/`CreateProcess*` IATs, and redirects `GetModuleHandle*` /
    `GetProcAddress` queries for `sl.interposer.dll` while excluding the superseded game-directory module
    from `StreamlineHook::InstallHooksForModule` and `ce::ngx_drs::ForEachLoadedDlssDrsConsumer`.

### 2026-10-10 - Crash dumps capture the fault neighborhood

- Witcher 3 session 20261010_160937 died of its own compiled-in int3 trap (witcher3.exe + 0x1F10AC3,
  byte-identical to the on-disk binary, PE timestamp matching; the game's crashreporter ran). CE's
  pre-termination dump proved all of that but could not say what the game was reporting: the reporting
  thunk's LEA targets (.rdata strings/records) and the call-site code on the stack were not captured,
  while 85% of the 182 MB was module .data segments that answered nothing.
- `FaultNeighborhoodCollector` (captureengine/diagnostics) now feeds dbghelp's memory callback the
  faulting thread's stack, code windows around every code pointer on it, register windows, and the
  data its captured code references (relative CALL targets and rip-relative LEA/MOV targets; heuristic
  scan in `common/crash/fault_neighborhood_policy.h`, same mechanism and budgeting as the WoW64
  stacks, chained behind one callback, x64 only). CaptureEngine's own modules keep their writable
  sections in the dump.
- Crash-like pre-termination exits now take the fatal-assert dump scope (handles, stack-referenced
  memory, memory map, no module data segments); suspicious but clean exits keep the rich scope.
- `tools/dump_triage.py` automates the triage that session needed by hand: fault module/RVA and trap
  bytes, call-chain code pointers, harvested references with captured strings or a MISSING marker.
  `--self-test` runs in the Python tool self-tests; run against the real session dump it reproduces
  the manual findings and names the missing report payload.
- Also fixed the clang-tidy ratchet findings from the startup-import work (exception escape at
  std::thread entries and main, one narrowing conversion). The elevation service client worker now
  confines failures to its client instead of terminating the service process; the ratchet folded the
  lowered counts in. Lint green.
