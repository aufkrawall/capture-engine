# llm-wiki Log

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
