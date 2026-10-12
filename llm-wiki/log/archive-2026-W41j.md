# llm-wiki Log Archive

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
