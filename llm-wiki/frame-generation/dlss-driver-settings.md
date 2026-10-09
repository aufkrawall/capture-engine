# DLSS Frame Generation Driver Settings

Last cross-checked: 2026-10-09 (loader-notification patch for consumers no CE load hook saw; hardware run pending). Dynamic MFG validated on hardware 2026-09-19; `vsync_mode` answered over the same channel.

Primary sources:
- `hook/ngx/ngx_drs_override{,_policy}.{h,cpp}`
- `common/ipc/shared_defs_detail/dlss_frame_generation_policy.h`
- `hook/pacing/reflex_limiter.*, hook/runtime/hook_common.*, hook/streamline/streamline_runtime_policy.*`
- `hook/wrappers/{iat_hook.h,iat_hook_init.cpp}`
- `hook/streamline/streamline_hook_dlssg.cpp`
- `hook/runtime/main_overlay_detect.cpp`
- `tests/{test_ngx_drs_override,test_config_override_dlss,test_streamline_runtime_policy_part2}.cpp`

Measured against NVIDIA Profile Inspector's `nspector/Native/NVAPI/NvApiDriverSettings.h` plus disassembly of
`nvngx_dlssg.dll` 310.9.1, `sl.dlss_g.dll` / `sl.common.dll` 2.14.1 and the driver's `nvapi64_impl.dll`.

## The five keys

- **One unit, five keys.** `hook/ngx/ngx_drs_override_policy.h` (pure policy, unit-tested) and
  `hook/ngx/ngx_drs_override.{h,cpp}` (state, detour, arming) answer five DLSS driver-settings reads
  process-locally: the FG render preset plus the four keys NVIDIA Profile Inspector exposes as
  "DLSS-FG - Forced Mode" (`0x10308298`), "DLSS-MFG - Fixed Frame Generation Count" (`0x104D6667`),
  "DLSS-MFG - Dynamic Frame Generation Count" (`0x10562D0F`) and "DLSS-MFG - Target Dynamic Frame Rate"
  (`0x10CF4125`). Ids and value ranges match NVIDIA's own `NvApiDriverSettings.h` (`NGX_DLSSG_*_ID`,
  `EValues_NGX_DLSSG_*`).
- **Two different readers, one NvAPI entry point.** `nvngx_dlssg.dll` reads the render preset itself. The four
  multi-frame keys are read by **`sl.dlss_g.dll`** (`readDRSKeys` -> `readSingleDRSKey`), which does not call NvAPI:
  it goes through the DRS context **`sl.common.dll`** owns, and that module resolves `0x73BF8338` through
  `nvapi_QueryInterface` and calls `NvAPI_DRS_GetSetting(session, profile, settingId, setting)` with
  `NVDRS_SETTING_VER1`, reading `currentValue.u32Value`. It tries the application profile first and falls back to the
  base profile, so one key can produce two calls and both reach CE's answer. Measured on Streamline 2.14 from the
  NGX model store.
- **The caller filter cannot be a file name alone.** An OTA-downloaded Streamline plugin is mapped from the NGX model
  store under a content-addressed name such as `160_E658703.dll`. Every Streamline plugin, `sl.common` included,
  exports `slGetPluginFunction` and nothing else but `DllMain`, so that export is what identifies it;
  `IsDlssDrsConsumerModuleLoaded` accepts either the known names or that export.
- **Value encodings, cross-checked against sl.dlss_g's own acceptance checks.** Forced mode is
  1 off / 2 fixed(on) / 3 auto / 4 dynamic, mapping onto `sl::DLSSGMode` 0..3; anything else is logged as
  "Ignoring invalid DLSSG mode %d from DRS". Both cadence keys carry **generated frames**, so 1..5 means 2x..6x, and
  0 means untouched. The target rate is `0x01000000` for "max refresh" (sl turns it into `0.0f`, its auto), else a
  plain frame rate in 1..0x00FFFFFF; above that and non-zero it is logged as "Ignoring invalid dynamic target frame
  rate". CE normalizes to those ranges, so it can never emit a value the runtime would reject.
- **`dlss_fg_mode=dynamic` and `dlss_fg_factor` are mutually exclusive.** `ResolveEffectiveDLSSFGFactor`
  (`common/ipc/shared_defs_detail/dlss_frame_generation_policy.h`) returns 0 for the factor under dynamic mode, and every
  consumer of the configured factor goes through it: the NGX parameter writes, the Streamline options override, the
  published overlay multiplier, and the Remix scheduler. Without it the runtime would be told "vary the cadence" by
  the driver and "it is exactly N" on every evaluation, and the outcome would depend on call ordering.
- **`vsync_mode` reaches DLSS-G only through the driver key, so CE answers that one too.** The indicator's `V`
  is the NGX parameter `DLSSG.VSyncOn`, which `sl.dlss_g`'s `vsyncState.cpp::shouldEnableVSync` resolves in this
  order: an internal "unsupported" flag wins, then the **driver** VSYNCMODE key (`0x08416747` FORCEOFF /
  `0x47814940` FORCEON), and only if neither force value is set does the **application's** SyncInterval decide.
  CE's own `vsync_mode` rewrite happens in `DXGIShared::ProcessPresentVSyncOverride`, on the real dxgi Present -
  *below* Streamline's swapchain proxy, per the multi-overlay hook-below rule - so the runtime never observes it
  as an application request. Session `20260919_223111` had `vsync=fifo` live and the indicator still read
  `144 Hz F`; forcing VSync in a driver profile instead reads `144 Hz VF`. CE therefore answers `0x00A879CF`
  alongside the DLSS keys: `off` -> FORCEOFF, `fifo`/`adaptive` -> FORCEON, `mailbox`/`default` -> not claimed.
  This is **not cosmetic** - `checkGsyncAndVsync` and the Silk/RSYNC path consult the same value - and it is
  scoped by the existing caller filter, so only DLSS frame generation consumers ever see CE's answer; the game,
  the driver and every other component keep the real one.
- **The runtime notices the half-answer and does not care.** `shouldEnableVSync`'s caller compares
  `bCplVsyncOn` from NvAPI against the DRS value and logs `RSYNC: Inconsistency detected!` when they disagree -
  which they will, because CE answers the key without touching the control-panel state. Verified by disassembly
  that the mismatch branch falls through to the same place as the consistent one: it is logged, never acted on.
  That is what makes answering the key alone sufficient, with no second NvAPI interception.
- **Reading the on-screen indicator.** `nvngx_dlssg` 310.9.1 builds it from
  `- %ux` (fixed) or `- %ux/%ux` (dynamic: current/max, only when mode is dynamic and max > current > 1), an
  optional mode token `Auto` / `Dyn` / `Dyn DRV`, and `- %.0fHz %s`. Every one of those is a named NGX parameter
  `sl.dlss_g` writes, so the decode is exact rather than inferred: the number is **`DLSSG.TargetFrameRate`**, not
  the panel's refresh rate (`DLSSG.RefreshRate` exists separately and is not printed); the mode token is
  `DLSSG.StreamlineMode` 2/3 with `Dyn DRV` selected by **`DLSSG.Dynamic.ReflexDriven`** being non-zero (Reflex-
  driven, *not* "driver-requested"); the trailing token is `V` when **`DLSSG.VSyncOn`** is set plus `F` when
  **`DLSSG.FlipMetering`** is 2 (hardware flip metering) or `C` when it is not. So `144 Hz F` reads "targeting
  144 fps, hardware flip metering, VSync not reported" - and `144 Hz` is itself confirmation that
  `dlss_fg_target_fps=max_refresh` resolved.
- **Whether dynamic MFG was actually accepted is observable.** `slDLSSGState::bIsDynamicMFGSupported` is the
  runtime's own verdict; sl.dlss_g otherwise logs its refusal ("Dynamic MFG is not supported on this system,
  ignoring request from DRS") only into NGX's log. `Hooked_slDLSSGGetState` reports each transition of that flag once
  while `dlss_fg_mode=dynamic` is configured.
  **But only when the game's struct carries it.** `slDLSSGState` is allocated by the application, so a field above
  its `structVersion` is memory the game never reserved; `bIsDynamicMFGSupported` arrived in version 4 and Talos
  Principle 2 publishes version 3. Session `20260919_223111` read that byte as 0 and reported "NOT supported" while
  DLSS-G was demonstrably alternating 2x/3x/4x. `ResolveDLSSGStateOptionalBool` now reports -1 (unknown) below the
  field's minimum version, and every DLSS-G state log carries `stateVer=`.
- **The realized cadence is the reliable evidence, and it is CE's own present accounting.** Under dynamic MFG
  `DXGIShared::DetourPresent` sees the multiplier move while `output_fps` stays pinned: session `20260919_223111`
  recorded `published_multiplier` 2/3/4 with `base_fps` 46.52/34.89/27.85/20.89 against a constant 139.57 output on
  a 144 Hz panel. That is `dlss_fg_target_fps=max_refresh` doing its job. The Streamline options keep reporting the
  game's own request (`optionsMode=on generated=3`), because the runtime varies the cadence below that API.
  `FGCompatibility::SetDLSSFGMultiplier` is metered for the same reason - unmetered it was 26% of that session's log.
- The FG preset is **not** an NGX parameter. `nvngx_dlssg.dll` exposes no `*.Hint.Render.Preset.*` name at all; the
  create-time parameters it parses are `DLSSG.UserInterfaceRecompositionEnabled`, `MenuDetectionEnabled`,
  `AsyncCreateEnabled`, the linearized-depth trio and `IndicatorLevel`. The preset comes from the driver settings
  (DRS), read in `DLSSGDRSKeys::ReadValuesFromDRSImpl` - the same channel the NVIDIA app and profile editors write.
  So the SR/RR approach (rewrite the value the game hands NGX) cannot work here; there is nothing to rewrite.
- Verified by disassembly of `nvngx_dlssg.dll` 310.6 and 310.7: the snippet iterates a table of **eight** DRS setting
  ids (`0x10E41DF6`, `0x104596A1..A3`, `0x104D6667`, `0x104C9A99`, `0x10E41DF1`, `0x10308298`), reading each with
  `NvAPI_DRS_GetSetting`. `0x10E41DF1` is the render preset: the value is logged as `INFO: Preset ID: %d`, `1` selects
  preset A ("Preset A selected, disabling UIR") and `2` selects preset B ("Preset B selected, enabling UIR"). A value
  of `0x10E41DF6` bit 2 means "Not parsing presets due to private flag overrides". So in 310.7 the preset letters are
  exactly a UI-recomposition switch, but the selection itself is a plain 1-based index, so CE accepts A-Z.
- Version floor: 310.4 (shipped with GTA V Enhanced) and the 310.2.1 driver-store copy contain neither the preset
  strings nor `0x10E41DF1`. The bundled testapp runtime is 310.6 and does support it. On an older runtime the override
  is simply inert.
- `NvAPI_DRS_GetSetting` is function id **0x73BF8338**, resolved by the snippet through `nvapi_QueryInterface`
  (nvapi64.dll exports only `nvapi_QueryInterface` and `nvapi_Direct_GetMethod`) and cached for the process on first
  use. CE therefore wraps that one resolution: `hook/ngx/ngx_drs_override.cpp` returns a detour that forwards
  every call and substitutes only the configured keys. A key with nothing configured passes through even while
  another one is armed, which matters because both readers pull several keys from the same loop.
- Invariant: nvapi64.dll's code bytes are never patched. The interception is CE's existing filtered
  `nvapi_QueryInterface` GetProcAddress/IAT path (`ReflexLimiter::EnsureNvApiQueryInterfaceInterception`), for the same
  reason the Reflex limiter refuses to patch NvAPI prologues - DLSS FG integrations validate them during Reflex setup.
- Invariant: nothing is written to the machine's driver profiles. The answer is process-local, so other applications
  and later sessions are unaffected.
- `nvngx_dlssg` and the `sl.*` modules are classified as Streamline/FG modules, and those callers are deliberately
  bypassed in `DetourGetProcAddress`. `ShouldAllowNgxFrameGenerationPresetDynamicHook` is the single narrow
  exception: only a DLSS driver-settings consumer, only `nvapi_QueryInterface`, and only while something is
  configured. `ShouldReturnWrapperToCaller` still refuses to hand Reflex wrappers to FG modules, so their view of
  NvAPI changes for the DRS getter alone.
- The substituted `NVDRS_SETTING` must look like an explicitly set current-profile DWORD: `settingLocation = 0`
  (`NVDRS_CURRENT_PROFILE_LOCATION`) and `isCurrentPredefined = 0`, because `util::drsReadKey` rejects anything else.
  Only the fields the snippet reads are written; `version` and `settingName` are left alone, and an unrecognized
  struct version is forwarded untouched. `ngx_drs_override_policy.h` mirrors the NvAPI ABI with `static_assert`s on
  `sizeof` (0x3020), `settingId`/`settingType`/`settingLocation`/`currentValue` offsets, and `NVDRS_SETTING_VER1`
  (0x13020) so a layout mistake fails the build instead of corrupting the caller's stack buffer.
- All five keys at their default arm nothing: no dynamic hook registration, no IAT patch, no bypass exception, and
  the wrapper is never returned. Arming happens from config load, shared-memory connect, and `nvapi64.dll` / any
  DRS-consumer module load; each consumer's own `kernel32!GetProcAddress` import is patched at its module-load
  notification because the process-wide `PatchIATAllModules` snapshots predate it.
- Diagnostics: `NGX DRS: configured ...`, `NGX DRS: armed ...`, `NGX DRS: GetProcAddress import patch on <module>
  installed`, `NGX DRS: wrapping NvAPI_DRS_GetSetting for ...`, then rate-limited `NGX DRS: answered
  NvAPI_DRS_GetSetting(0x..., <key name>) with <value>`. Without the wrapping line the resolution never reached CE.
  The readers' own NGX logging (`INFO: Preset ID: %d`, `Read DRS key %d = 0x%x from app profile`) is the independent
  confirmation.
- **Version floor is per key.** The preset needs a DLSS-G runtime of 310.6+. The multi-frame keys need a Streamline
  DLSS-G plugin that reads them (2.14 does) and, for dynamic mode's full behaviour, NVIDIA documents driver 595.97 or
  newer. On anything older the keys are simply never read and nothing changes - the same outcome Profile Inspector
  produces.

## Consumers mapped before CE arrived (startup sweep)

The `GetProcAddress` import patch on a consumer is only installed from the loader notification, which CE registers
~200 ms after injection, and the process-wide `PatchIATAllModulesFiltered` sweep skips Streamline modules by design.
A title that runs `slInit` first therefore hands CE a `sl.common` nobody ever patched.

Evidence, Witcher 3 Remastered (native Streamline 2.14.1 in the game folder, `dll_injection=always`, session
`20261008_211749`): `sl.common` appears only through the hook thread's module scan ("Installed hooks for sl.common.dll"),
never as `Loader: runtime module loaded` or `IAT: Patched ... in module <its base>`; the sl.* plugins that loaded
later did get both. `NGX DRS: wrapping NvAPI_DRS_GetSetting` fired once, for `nvngx_dlssg.dll` (`streamlinePlugin=0`),
which answered only the preset and forced mode (`dynamic`). The count / target / VSync keys travel through
`sl.common`, were never answered, and the game kept the fixed factor from its options menu. The log also said
"dynamic MFG ... reported SUPPORTED", so a runtime that supports it is no evidence that the request arrived.
The "Streamline override redirect refused" lines in that session are benign here: game and override are both 2.14.1.

Fix: `ce::ngx_drs::ForEachLoadedDlssDrsConsumer` (pins each module, same predicate as the notification path) and
`PatchLoadedDlssDrsConsumers` (`hook/runtime/main_overlay_detect.cpp`). It runs when the overrides are armed from
config / shared memory and again right after `InitializeGetProcAddressHook` in `InstallHookThreadHooks`; the loader
notification arms without sweeping because it holds the loader lock. nvapi64.dll is mapped later than that
(`nvapi64=0` at config time, first load ~1.2 s after injection), so the patched import is in place before
`sl.common` calls `GetProcAddress(nvapi_QueryInterface)`. A consumer that already cached that pointer before CE
arrived cannot be recovered without patching `nvapi64.dll` code, which stays forbidden.

**The import patch alone was not enough (session `20261008_214202`, 0.1.7050).** `sl.common` was patched at 07.159 and
still logged no routed lookup, no wrap, no answers: the `nvapi64=0 nvapi=0` line only prints when an NvAPI module IS
loaded, so NvAPI was up at config time and `sl.common`'s static NvAPI layer had already called
`GetProcAddress(nvapi_QueryInterface)` during `slInit`. That layer (`sl.common` +0x1240 init, per-function cache
table at +0xc3b78, QI pointer at +0xc3b58; `sl.dlss_g` and `sl.interposer` carry their own copies of the stubs) keeps the
driver's pointer in `.data` and never asks again, so every later `NvAPI_DRS_GetSetting` lookup bypassed CE.
`sl.dlss_g!readSingleDRSKey` only calls function pointers of the context `sl.common` owns (`[ctx+0]` global,
`[ctx+0x20]` app profile), so the reads happen in `sl.common`'s code after `sl.dlss_g` loads (~4 s after the sweep).
Fix: `RetargetCachedNvApiPointers` scans each swept consumer's writable, non-executable sections for the driver's
`nvapi_QueryInterface` (and, when armed, `NvAPI_DRS_GetSetting`) address and compare-exchanges in CE's
`ReflexDetour_QueryInterface` (resp. the DRS wrapper), recording each slot; `ShutdownIATHooks` restores them.
The detour is handed out only once the limiter knows the driver export (`QueryInterfaceDetourIfReady`), because it
returns null for every id before that. No offsets are hard-coded. Order inside a visit is IAT patch first, then scan,
so a core that initialises between the two already holds CE's pointer. Hardware run pending.

**A consumer mapped after the sweeps but before CE's LdrLoadDll hook was still missed (session `20261009_110607`,
0.1.7057, Witcher 3 + ReShade).** Same config, same build: 105819 and 105938 answered all six keys, 110158 and 110607
answered two (preset + forced mode, both from `nvngx_dlssg`) and `capabilityMax` was 5 instead of 3. Cause is a startup
race, not ReShade. The hook thread's "Streamline use observed (sl.interposer.dll)" can win against the game thread's
first `sl.common` request; then `PlaceStreamlinePluginSet` preloads CE's own override `sl.common`/`sl.dlss`/`sl.dlss_g`/
`sl.dlss_d` (`Runtime preload: ... loaded`), and the game maps ITS `sl.common` from the game folder ~17 ms later (the
interposer loads by full path, so the preloaded name is not reused). That second image was mapped after the three
startup sweeps and ~60 ms before `Installed LdrLoadDll hook`, so `NotifyHookModuleLoaded` never saw it; only the
loader notification did, and it merely logged `Loader: runtime module loaded`. No sweep re-ran, so the live core kept
the driver's `nvapi_QueryInterface`. In the passing runs the game thread simply won the race, the hook thread logged
"override disabled" immediately and the sweep patched the game's copy.
Fix: the loader notification now calls `PatchDlssDrsConsumerImport` (`via=loader notification`) for any consumer,
on the loading thread before its `LoadLibrary` returns (ordered ahead of any `GetProcAddress` it can make; no module
pinning), and sets a flag; `ServiceDlssDrsConsumerSweep` (hook thread, next to `RefreshThirdPartyOverlayIdentityCache`)
then runs `PatchLoadedDlssDrsConsumers("loader notification")` for the cached-pointer retarget. Source-contract test
`NgxDrsStartupSweepTest.LoaderNotificationPatchesAConsumerNoCeLoadHookSaw` (fails without the change). Hardware run
pending. **Orphan copies, fixed the same day:** in the losing order CE had also preloaded an override `sl.common` that owned
its single `slGetPluginFunction` forward pointer, so the hook thread logged `Refusing to retarget slGetPluginFunction` x10
and re-hooked `sl.common` reloaded x66 in the first 2 s (feature hooks still resolved). `PlaceStreamlinePluginSet` now stands
down for good when a `sl.interposer.dll` is resident from anywhere but the configured override path
(`IsForeignStreamlineInterposer`): that interposer owns the stack, a name-registered copy is never reused by a path-keyed
load, and only the loader redirect can substitute a request. Log: `Runtime preload: sl.* plugin set not placed - sl.interposer.dll
is already resident from <path>`. Not evidenced and left alone: a shipped-but-not-yet-mapped interposer (CE places the
override interposer first, which is the intended mechanism). Hardware run pending.

Diagnostics: `NGX DRS: GetProcAddress import patch on <module> installed ... via=startup sweep (<source>)` is logged
once per module and outcome (the old shared 4-line budget was spent on `nvngx_dlssg.dll` alone), then
`NGX DRS: startup sweep ... visited N`, then `NGX DRS: nvapi_QueryInterface resolved by DLSS driver-settings consumer
<path> - routed through CE`, then `NGX DRS: wrapping NvAPI_DRS_GetSetting for ...sl.common.dll (streamlinePlugin=1`
and the `answered` lines for `0x10562D0F` / `0x10CF4125`. Hardware run pending.

## Validated on hardware

Session `20260919_230915` (Talos Principle 2, sl 2.14.1 + nvngx_dlssg 310.9.1, driver 616.92, 144 Hz):
`mode=dynamic dynamicMax=6x targetRate=max refresh driverVSync=force on`, all five keys answered - VSYNCMODE
`0x47814940` to `sl.common`, then mode 4, target `0x01000000`, dynamic max 5, in exactly `readDRSKeys` order.
The runtime then held ~138 fps by varying the cadence between 2x and 4x (`base_fps` 30-71 against
`output_fps` 123-143), with `displayJagUs` 287-575 and `screenTimeShare=1000permille` - no VRR or pacing
regression from the forced driver VSync. `stateVersion=3` confirmed the DLSSGState gap, so `dynMFG` reports
-1 rather than a false 0.

It never reaches 5x/6x despite `dlss_fg_dynamic_max=6x`, and that is correct: at a 30-70 fps rendered rate
3x-4x already meets the ~144 target, and "up to" means the runtime stops there.

## The activation-health monitor does not apply to auto/dynamic

`ShouldTrackDLSSGActivationHealthSample` refuses `sl::DLSSGMode::eAuto` and `eDynamic`. The monitor exists to
catch the GTA case - the game asked for frame generation and nothing was ever generated - and that question
only has a fault answer for a FIXED request. Under a runtime-chosen cadence, choosing 1x because the rendered
rate already meets the target is a legitimate operating point, so a run of `numFramesActuallyPresented == 1`
proves nothing.

**The application's options are not the whole answer.** `dlss_fg_mode=dynamic` is delivered over the driver
settings and applied *inside* sl.dlss_g, so the `slDLSSGOptions` CE sees keep reporting the game's own fixed
request - Talos asks for `eOn(1)` throughout. A first attempt at this gate looked only at `optionsMode` and
therefore did nothing: session `20260919_231939` still warned four times with `optionsMode=on(1)` while the
cadence varied 2x..4x. The gate also consults CE's own configured mode, via a lock-free
`ce::ngx_drs::GetConfiguredFrameGenerationMode()` because this is read per GetState. It is keyed on the
configured *intent*, not on evidence that the runtime accepted it, because that evidence is exactly what CE
cannot observe here - and having asked for a runtime-chosen cadence is already enough to make "nothing was
generated" unusable as a fault signal.
