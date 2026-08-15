# Graphics Overrides And Frame Pacing

Last cross-checked: 2026-08-15

Primary sources:
- `common/config.{h,cpp}`
- `common/mip_mapping_policy.h`
- `common/strict_float_parse.h`
- `common/shared_defs.h`
- `hook/common/{hook_common,dxgi_shared,fps_limiter,fps_limiter_policy,sampler_override_utils,dlss_indicator_spoof}.*`
- `hook/common/{ngx_module_policy.h,ngx_feature_lifecycle.h,ngx_fg_preset_override.*,reflex_limiter.h,ue5_rr_override_policy.h,ue5_cvar_override_policy.h}`
- `hook/main_ue5*.cpp`
- `hook/wrappers/{iat_hook.h,iat_hook_init.cpp}`
- `hook/apis/{dx9_hook,dx9_sampler_state,legacy_d3d_sampler_state,dx11_hook,dx12_hook,dx12_sampler_hooks,nvngx_hook,nvngx_hook_lifecycle,opengl_hook,opengl_sampler_override,opengl_texture_storage_override,streamline_hook_api}.cpp`
- `hook/vulkan_layer/vulkan_layer.{h,cpp}`
- `hook/vulkan_layer/vulkan_sampler_policy.h`
- `tests/{test_config,test_mip_mapping_policy,test_sampler_override_utils,test_dx12_sampler_policy,test_fps_limiter,test_dlss_indicator_spoof,test_ngx_feature_lifecycle,test_ngx_module_policy,test_ngx_fg_preset_override,test_rr_force_source,test_ue5_rr_override_policy,test_ue5_cvar_override_policy}.cpp`

## Configuration contract

- `sampler_override_mode=safe|aggressive` defaults to `safe`. Safe mode protects comparison/reduction, fixed-LOD, and
  point-min/mag sampler families, with API-specific material-address restrictions (DX12/Vulkan remain wrap/mirror;
  D3D10/11 can accept clamp/mirror-once when shader/resource evidence is available). Aggressive mode expands ordinary
  sampler coverage but still preserves comparison/reduction, invalid, fixed-LOD, border, Vulkan non-normalized, and
  other structurally special samplers.
- `mip_mapping=default|nearest|bilinear|trilinear` is case-normalized and invalid values fail back to `default` with a
  bounded configuration diagnostic. On a mipmapped ordinary sampler, nearest means point MIN/MAG plus nearest-mip,
  bilinear means linear MIN/MAG plus nearest-mip, and trilinear means linear MIN/MAG plus linear-mip. The override
  never enables mipmapping for an application state or object that has no usable mip range.
- `cpu_prerender_limit` has integer semantics only: `-1`, `0`, or `1-6`. Fractional, non-finite, trailing-junk, and
  out-of-range inputs normalize to `-1`.
- `backbuffer_count=N` retains physical count changes where safe. A flip-model reduction that would violate the game's
  allocation remains physical-count preserving and uses waitable-swapchain maximum latency `N-1` as the equivalent
  present depth.
- DLSS preset input is exactly one trimmed `A-Z` character or `default`. Sharpening is exactly `default`, `off`, or a
  finite full-string value in `0.0-1.0`.
- `dlss_fg_preset=default|A-Z` overrides the DLSS **frame generation** render preset. It parses exactly like the SR/RR
  presets, but it is not delivered like them - see "DLSS Frame Generation render preset" below. The resolved value
  travels in `SharedGraphicsConfig::dlssFGPreset` (the slot previously retained as padding, so the layout and ABI
  signature are unchanged and a pre-feature host publishes a zero that reads as "no override").
- `dlss_debug_overlay=default|on|off` controls NVIDIA's on-screen DLSS indicator. The NGX runtimes decide by reading
  `HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore\ShowDlssIndicator` (`0x400` = shown); the value is absent on a stock
  driver install, so `on` must synthesize it. CE answers the probe in-process and never writes the registry - see
  "DLSS on-screen indicator" below.
- `[UE5] force_ray_reconstruction=off|on` is the canonical x64 policy. `on` persistently selects the existing NVIDIA
  plugin's `r.NGX.DLSS.DenoiserMode=1` render path in process memory; legacy `[DLSS]` / `[Graphics]` inputs remain
  accepted, and `UE5.force_ray_reconstruction` works in a process-backed profile.
- `ray_reconstruction_optimal_settings=on` adds the exact 29-CVar DenoiserMode/Lumen/VSM/MegaLights bundle listed in
  `captureengine/config.ini.template`; it implies force RR. `disable_post_processing_effects=on` applies dedicated
  built-in sharpen, film-grain/grain-quantization, vignette show-flag, motion-blur, and scene-fringe overrides without
  touching `r.Tonemapper.Quality`. `tonemapper_sharpen=default|0..10` overrides the bundle's sharpen=0 only.
- `internal_fps_limit=default|off|1..1000` overrides UE5's own engine frame rate limiter (`t.MaxFPS`, a float CVar
  read on the game thread). `default` leaves the engine alone, `off`/`0` disables the engine limiter, and a positive
  value (fractional values such as 59.94 are accepted) caps the engine frame rate. This is deliberately independent
  from CaptureEngine's own fps limiter: the engine limiter paces `FEngineLoop::Tick`, CE's limiter paces presents, and
  both can be active at once.
- `internal_anisotropic_filtering=default|off|1x|2x|4x|8x|16x` overrides UE5's internal anisotropic filtering with a
  single shared level applied to both `r.MaxAnisotropy` and `r.VT.MaxAnisotropy` (both int32 render-thread CVars).
  `off`/`1x` disables anisotropic filtering. This is separate from the general `[Graphics] anisotropic_filtering`
  sampler override, which forces API sampler states rather than engine CVars.
- Shared memory contains the host's fully resolved per-process profile. The hook-local config is used only before IPC
  exists; sentinel-only selective merging is forbidden because it prevents a profile from resetting a global value.

## Sampler invariants

- DX9 forces MIN/MAG anisotropy independently of `mip_mapping`; MAXANISOTROPY alone is reconciled by setting MIN/MAG
  on the same eligible sampler. Safe mode requires a bound, filter-capable texture with more than one visible mip and
  material addressing. Mutable state is reconciled only on SetTexture/SetSampler/config events; bootstrap getters are
  one-shot, including pure-device failure, and there is no draw hook. Create/EndStateBlock install per-vtable Apply
  interception; a successful Apply refreshes physical sampler state while retaining the tracked logical application
  state, then immediately reapplies the configured override.
- D3D10/11 wrapper-to-real `CreateSamplerState` forwarding is explicitly marked so the raw vtable hook cannot apply
  offset/base bias twice. D3D10 is creation-time-only and transactionally retries the original descriptor; D3D11 uses
  its shader/resource-aware dirty-slot replacement policy.
- DX12 has one mutation boundary for static samplers: `ID3D12Device::CreateRootSignature`. Serializer detours observe
  dynamic resolution coverage but pass descriptors through, preventing offset/base bias from being applied once at
  serialization and again at root creation. Coverage includes sampler v1/v2, root signatures 1.0/1.1/1.2, raw
  `D3D12CreateDevice`, and `D3D12GetInterface`/`ID3D12DeviceFactory::CreateDevice`.
- Vulkan uses only device-enabled anisotropy, clamps to physical-device limits, recognizes sampler-reduction pNext
  structures directly, and retries the original descriptor transactionally if an override is rejected. Mip-filter
  eligibility is independent from the stricter safe-AF material heuristic, so ordinary point and clamp-to-edge
  samplers still receive the selected mip technique. All modes preserve clamp-to-border, unnormalized-coordinate,
  comparison, special-reduction, nonstandard-filter, and no-mip-range samplers. The decision occurs only at
  `vkCreateSampler`; there is no draw/dispatch cost.
- D3D6-8 use event-driven texture-stage-state reconciliation. Actual returned devices install per-vtable callbacks;
  DX6/7 refresh at EndScene and DX8 at Present. D3D7/8 ApplyStateBlock interception refreshes physical state and
  immediately reapplies the policy. D3D7 MAG anisotropy is value 5, and its sampler vtable slots are 36/37; D3D5 and
  older have no anisotropic filter value to force generically. Pure DirectDraw 2D has no mip sampler state; the
  DirectDraw-hosted mip override is the D3D6/7 path.
- OpenGL intercepts bound texture parameters, sampler objects, core/EXT DSA, mip allocation/storage/copy, and mip
  generation. Version-cached texture/sampler bind hooks reconcile late/default objects without adding draw hooks;
  texture/sampler deletion invalidates caches across contexts so reused GL names cannot inherit a stale decision.
  Integer, vector, and float parameter entry points share the same filter mapping, including
  `GL_NEAREST_MIPMAP_NEAREST`. It verifies actual mip storage and device limits at those mutation boundaries. CPU
  prerender sync rings remain owned per HGLRC.

## Queue-depth and limiter invariants

- D3D10 limit zero uses a native event query; D3D10 limits 1-6 use DXGI maximum frame latency. D3D11 query rings and
  DX12 fence rings are serialized and rebound when device/queue identity changes. Configured waits do not silently
  escape after an 8/16 ms timeout while GPU- or vblank-bound.
- During DX12 frame generation, the CPU prerender fence ring advances only on a proven application-source Present
  and stays pinned to the retained original game queue, with the fence device queried from that exact queue for
  multi-device Streamline topologies. Streamline/FFX output workers, opt-in eager startup draws, and
  unknown-provenance runtime Presents skip only this limiter; their overlay/capture routing remains unchanged.
  Waiting on a runtime-generated Present or rebinding the ring to a runtime wrapper/presenter queue can deadlock
  because that queue may not retire until the same Present returns.
- Vulkan `cpu_prerender_limit=1-6` uses a per-queue seven-fence marker ring; `0` waits the current marker. OpenGL uses
  the same lookback semantics per context. Vulkan drains and resets outstanding markers when the configured depth
  changes so a previously signaled fence is never resubmitted.
- Flip-model latency waitables are requested at creation whenever `backbuffer_count` is active. Wrapped DXGI waits at
  the post-Present/next-frame boundary so simulation/render work cannot begin behind a full vsync queue.
- The timer limiter uses a rational QPC/Bresenham grid, never emits a short catch-up interval after a missed deadline,
  and arms a high-resolution timer before the deadline. Capture-sync late recovery advances by whole rational-grid slots
  until the next deadline has at least half an interval of headroom, preserving source/CFR phase through a hitch;
  general limiting retains now-relative recovery. The fine margin is `clamp(p99 timer wake overshoot + 25us, 50us,
  250us)`; only the final 50us is a tight spin.
- Native Vulkan presents are paced through the grid with `Apply(gateEveryPresent=true)` on EVERY present (both
  `vkQueuePresentKHR` and the async `vkAcquireNextImageKHR` path), not only the first present entering the hook.
  Strange Brigade Vulkan presents several real swapchain images per frame period from concurrent present streams;
  the old first-present-only gating plus the 2ms dedup fast path let those extra images reach the driver unpaced, so
  a 60fps target displayed ~120fps (vsync-capped in intros) with alternating short/long frame times and bad 1% lows.
  Gate-every-present takes the cadence lock blocking (concurrent streams serialize onto the grid: exactly one present
  per target interval, evenly spaced) and bypasses both dedup fast paths. DXVK keeps the legacy first-present gating +
  dedup because its CS thread presents once per frame while the DX9/DXGI hooks already pace the game thread, and
  FG-scaled modes keep legacy behavior so generated frames are not pushed onto the base-frame grid. The strict path
  works identically with FIFO vsync enabled or off: the wait happens before the driver call and the game's present
  mode is left untouched.
- Reflex integration resolves `NvAPI_D3D_SetSleepMode` and `NvAPI_D3D_Sleep` from `nvapi64.dll` and calls the original
  entry points directly. NvAPI code bytes/prologues are deliberately not patched because some DLSS FG integrations
  validate them during Reflex setup; `minimumIntervalUs` is pushed proactively, and pacing hands to the game-owned
  Reflex sleep path once stable.
- Concurrent/re-entrant Present streams cannot advance one cadence: the first caller owns the cadence mutex and other
  callers skip without blocking. VFR disables capture-grid synchronization only, not an independently configured
  general cap.
- Frame-generation scaling depends on the captured source. WGC/DXGI see final presented/generated frames and scale the
  base target; inject capture publishes application-rendered frames and does not divide its capture-sync target.
## DLSS on-screen indicator

- The modules that read `ShowDlssIndicator` are `nvngx_dlss.dll` (super resolution) and `nvngx_dlssg.dll` (frame
  generation) - verified by the UTF-16 string being present in both, and absent from `nvngx.dll`, `sl.dlss.dll`,
  `sl.dlss_g.dll` and `sl.common.dll`. Both statically import `ADVAPI32!RegOpenKeyExW` + `RegQueryValueExW`; the `sl.*`
  plugins use `RegGetValueW` for their own NGXCore lookups.
- Those modules load only when the game creates its DLSS features - roughly ten seconds into a GTA V Enhanced session,
  long after CE's hook thread runs. Any one-shot IAT patch taken at hook-install time therefore never reaches them.
  The answer must be an inline hook on the shared implementation, installed once and independent of module load order,
  import style, and `GetProcAddress` resolution.
- `hook/common/dlss_indicator_spoof.cpp` hooks **`kernelbase!RegQueryValueExW` and `kernelbase!RegGetValueW`**, not the
  advapi32 exports: `advapi32!RegQueryValueExW` is only a 7-byte `48 FF 25` thunk (followed by `int3` padding) that
  jumps into kernelbase, so it is both a poor trampoline target and blind to api-set importers. kernelbase's bodies
  begin with exactly 14 relocation-free prologue bytes. advapi32 remains a documented fallback in the module list.
- A synthesized answer must be complete, because the value genuinely does not exist: `*lpType = REG_DWORD`,
  `*lpcbData = 4`, payload `0x400`/`0`, `ERROR_MORE_DATA` on an undersized buffer, and `ERROR_SUCCESS` with the size
  only on an `lpData == nullptr` probe. `RegGetValueW` additionally honors the caller's `RRF_RT_*` restriction and
  answers `ERROR_UNSUPPORTED_TYPE` when DWORD was excluded. Matching is on the value name alone, case-insensitively.
- `default` installs nothing at all, so an unconfigured process keeps a completely untouched registry path.
- Invariant: CE never writes `ShowDlssIndicator` to HKLM. The setting is process-local by construction, so it cannot
  leak into other applications or survive the session.

## Where NGX must be intercepted

- Only `nvngx.dll` (driver store) and the `_nvngx.dll` System32 stub export `NVSDK_NGX_*`. On this machine
  `_nvngx.dll` does not exist, so `sl.common.dll` reads `HKLM\...\NGXCore\FullPath`, `LoadLibraryW`s the driver-store
  `nvngx.dll`, and resolves every entry point with **GetProcAddress** at DLSS-feature-creation time. `sl.dlss.dll`
  contains neither the NGX symbol names nor an nvngx import - it only holds the `DLSS.Hint.Render.Preset.*` parameter
  strings and reaches NGX through `sl.common.dll`.
- Consequence: neither IAT patching nor CE's `GetProcAddress` dynamic-hook table can reach a Streamline game. Both are
  snapshots of the modules loaded when they ran (`PatchIATAllModules`), and `sl.common.dll` loads about ten seconds
  into a session, after the last pass. `hook/common/ngx_module_policy.h` + `InstallNGXExportInlineHooks()` therefore
  **inline-hook the export bodies in nvngx.dll itself**, driven from `NotifyHookModuleLoaded` so the patch lands inside
  the caller's `LoadLibrary`, before the first `GetProcAddress`. This is resolution-method and load-order independent.
- Invariant: when an export is inline-hooked, the captured trampoline **overwrites** any `nvngx_hook_o*` pointer an
  earlier IAT pass stored. Leaving a raw export address there would make the "original" call re-enter the detour.
- Invariant: a Streamline plugin is never accepted as the NGX provider. `NVNGXHook::Install` previously accepted
  `sl.dlss.dll`, which latched `m_Installed` on a module that exports nothing and stopped every later retry.
- Invariant: the export-name **GetProcAddress** interception applies to the core provider only
  (`ce::ngx::ShouldInterceptNgxExportLookup` + `RegisterDynamicHookFiltered`). The feature snippets
  (`nvngx_dlss.dll`, `nvngx_dlssg.dll`, `nvngx_dlssd.dll`) export the same `NVSDK_NGX_*` names and the core resolves
  them out of the snippet to dispatch into the feature. Answering that internal lookup made the detour forward through
  the single per-symbol `nvngx_hook_o*` - which the inline hooks had pointed at the core's own trampoline - so the core
  body re-entered itself until the stack overflowed (`0xC00000FD` in `nvapi64_impl.dll` during
  `NVSDK_NGX_D3D12_GetFeatureRequirements`). It also hid the snippet's real entry point behind that shared pointer.
  The system-module caller bypass masks this for a driver-store `nvngx.dll`; a title shipping its own `_nvngx.dll`
  hits it immediately.
- The parameter machinery downstream (vtable hooks on `SetUI`/`SetI` plus `InjectPreset` at parameter creation) was
  already complete; it simply never ran. `nvngx_debug.log` showing only `Config forced SR Preset ... (via Install)` and
  no `SetUI`/`CreateFeature` lines is the signature of interception never engaging.

## DLSS/Streamline runtime DLL override loading (`dlss_sr_dll_path` etc.)

- The per-profile override paths (`dlss_sr_dll_path`, `dlss_fg_dll_path`, `dlss_rr_dll_path`, `streamline_dll_path`)
  redirect loads of the NGX snippets and the Streamline stack to the configured folder (e.g. NVIDIA Profile
  Inspector's `sl` runtime) via `GetRedirectedPath()` in `hook/main_redirect.cpp` + the loader hooks in
  `hook/main_loadlibrary.cpp`. This is how a newer `nvngx_dlss.dll` (preset letters) or `sl.dlss_g.dll` reaches a game
  that ships an older runtime.
- Coverage limit (root cause of "override works in Talos but not RoboCop"): the redirect only fires when the load
  goes through CE's hooked LoadLibrary* imports. The IAT pass (`InitializeKernel32Hooks` -> `PatchIATAllModules`) is a
  one-time snapshot of the modules loaded at hook-install time, so **Streamline-internal loads bypass it**: sl.common
  loads the plugin DLLs (sl.dlss/sl.dlss_g/sl.dlss_d) and the NGX core through its own IAT, which was never patched
  because sl.common itself loads seconds later. `LdrLoadDll` is exported-hookable but Steam's overlay owns that export
  on this machine and CE's chain-hook refuses overlay modules (recursion), so direct-ntdll loads bypass it too. Net
  effect: only `sl.interposer.dll` (loaded by the game exe through a patched import) got redirected; everything
  Streamline loads internally came from the game's own folder. Talos ships a Streamline 2.x stack close enough to the
  override that presets/RR still worked; RoboCop ships an older stack, so SR preset M / RR never took effect there.
  The debug HUD is independent (registry spoof at `kernelbase!RegQueryValueExW`), which is why it works everywhere.
- Fix (build 0.1.5896): `PreloadConfiguredGraphicsRuntimeDlls()` in `hook/main_redirect.cpp` loads the configured
  override stack at hook-thread start (sl.interposer, sl.common, sl.dlss, sl.dlss_g, sl.dlss_d, nvngx_dlss,
  nvngx_dlssg, nvngx_dlssd), through the original loader entry, in dependency order. Once a base name is registered,
  every later name-based load - including Streamline's internal loads - resolves to the override copy. The preload
  skips a name that is already loaded (the game's own copy won in the ordering race; adding a second instance would
  not take effect). `PatchLoadLibraryIatForLateLoadedModule()` additionally patches the kernel32 LoadLibrary* IAT of
  every module that loads after the snapshot (when overrides are configured), so Streamline-internal loads reach the
  redirect even without the preload. Only loader imports are touched - no graphics API wrapper is installed into
  runtime modules.
- Diagnostics: every load of a runtime-family module (sl.*, nvngx_dlss*/nvngx core, nvapi64) now logs its **resolved
  full path** from the LdrRegisterDllNotification callback (`Loader: runtime module loaded: <name> -> <path>`), which
  covers LoadLibrary, LdrLoadDll, and dependent loads. This is the authoritative answer to "which physical DLL did the
  game actually load"; the `Redirecting ... to:` lines only prove the redirect decision. Classification lives in
  `hook/common/graphics_runtime_module_policy.h` (`ce::graphics_runtime::IsRuntimeModuleBaseName`), pinned by
  `tests/test_graphics_runtime_module_policy.cpp`; its sl.* prefix rule deliberately mirrors `GetRedirectedPath`.
- **NGX model repository:** NVIDIA's driver stores Streamline plugins in
  `C:\ProgramData\NVIDIA\NGX\models\sl_<name>_<id>\versions\<v>\files\` with every file literally named
  `1B0_E658703.dll` (observed RoboCop 2026-08-09). Since build 0.1.5897 `GetRedirectedPath` maps such paths back to the real DLL
  (`sl_dlss_g_0` -> `sl.dlss_g.dll`) and redirects them to the configured `streamline_dll_path` when set; the loader
  logging tags those loads as `NGX model repository`. The mapping (`ModelSegmentToDllName`,
  `IsNgxModelRepositoryPath`) is unit-tested.

## Persistent UE5 CVar override policy

- NVIDIA's UE plugin declares `CVarNGXDLSSDenoiserMode` as a static `TAutoConsoleVariable<int32>` and selects SR
  (`0`) versus RR (`1`) with `GetValueOnRenderThread()`. CE therefore changes the exact value source the plugin reads,
  rather than periodically issuing a console command that a map/device-profile reload can undo.
- The generalized scanner resolves every currently requested exact NUL-terminated UTF-16 CVar literal in one read-
  section pass per module, correlates their nearby x64 RIP-relative constructor/store references, and counts all
  surviving candidate references in one executable pass. It scans the main game module first, then remaining initial
  modules, and after that only newly loaded modules. Raw candidates first have to prove the writable
  24-byte `FAutoConsoleObject`/`Target`/`Ref` layout, callable object and target vtables, readable
  `TConsoleVariableData<int32>`, and plausible `{game, render}` shadow values of `0` or `1`; impossible layouts are
  discarded before one combined executable-code pass counts reference evidence for every survivor. Only one strongly
  distinguished live candidate is accepted, and ambiguous or unfamiliar layouts leave memory untouched. The single
  reference pass is an invariant: Talos exposes 35 raw nearby candidates, so a pass per candidate delayed selection
  by 6.8 seconds and changed SR to RR during live rendering.
- Graphics-hook installation always precedes the optional module scan, both on initial startup and periodic service
  passes. This ordering protects DXGI queue capture even if an unfamiliar large image remains expensive to inspect;
  RR discovery itself must also finish early enough to select the denoiser before the first DLSS feature rather than
  hot-switching an active renderer.
- CE atomically redirects only each validated `Ref` pointer to typed process-lifetime `{game,render}` shadow storage.
  The game's
  real console variable and priority/history state remain intact; later Engine.ini, scalability, level, or game code
  writes update the original shadow but cannot change the plugin's direct read. Disabling the setting or shutting the
  hook down compare/exchanges the original pointer back. Owner-module unload retires the stale address and an eventual
  reload is rescanned. Live policy/value changes restore removed CVars and atomically update retained shadows. There
  is no disk write, render-thread hook, repeated module sweep, or vtable call into unknown UE code.
- UE 5.4/5.6 (validated: Talos Reawakened 5.4.4, Industria 2 Demo 5.6.1) registers many Lumen/rendering CVars through
  a different layout: the static `FAutoConsoleVariable`-style object is a `{vtable, Ref}` pair (16 bytes) and the
  `FConsoleVariable` holds its value behind a data pointer at `ref+0x50`, with a local fallback pair at `ref+0x58`
  (verified against `IConsoleVariable::GetValueOnGameThread`, which returns `*[this+0x50]` when the CVar is
  registered and `[this+0x58]` otherwise). The scanner therefore has a second install mode, **data-pointer redirect**:
  it CAS-replaces the `ref+0x50` pointer with CE's shadow (render-thread reads) and mirrors the value into the
  `+0x58` local pair (game-thread reads). Multiple registration sites can exist per CVar with separate value records;
  duplicates are deduplicated by data pointer, a validated pointer-model candidate is preferred when present, and
  otherwise the best-correlated data-pointer candidate is applied as a safe best effort.
- Version-conditional specs: some bundle CVars do not exist in every UE build. `r.MegaLights.DownsampleMode` and
  `r.Tonemapper.GrainQuantization` have no literal in UE 5.4 or 5.6; `r.Lumen.Reflections.DownsampleCheckerboard` and
  `r.MegaLights.NumSamplesPerPixel` exist in 5.6 but not 5.4. The summary log separates these ("literals not found in
  loaded modules") from present-but-unvalidated CVars. `ShowFlag.*` CVars are composed at runtime from a short-name
  table via `FString::Printf(TEXT("ShowFlag.%s"), ...)` in UE 5.4/5.6, so no `ShowFlag.X` literal exists to match;
  disabling vignette via `ShowFlag.Vignette` therefore remains unsupported until a runtime-composed-name discovery is
  implemented (film grain, motion blur, and chromatic aberration are already covered by `r.FilmGrain`,
  `r.MotionBlurQuality`, and `r.SceneColorFringeQuality`).
- Observed coverage: Industria 2 Demo (UE 5.6.1) installs 32/38 requested CVars; Talos Reawakened (UE 5.4.4, profile
  adds the two AF CVars) installs 31/40. The remaining entries are the version-conditional/absent names above plus
  `r.Lumen.ScreenProbeGather.Temporal.MaxFramesAccumulated` in Talos (per-title layout variance; its `ref+0x50` points
  into `.pdata`, so the data-pointer validation correctly refuses it). Talos's main menu does not render 3D, so only
  install coverage, not visual effect, is verifiable there; Industria 2 renders during the test window.
- The same machinery overrides `t.MaxFPS`, `r.MaxAnisotropy`, and `r.VT.MaxAnisotropy` (shared-memory ABI 40 added
  `internalFpsLimit` and `internalAnisotropicFiltering`). `t.MaxFPS` is a `TAutoConsoleVariable<float>` in UE5, so
  the spec uses the float value type and the scan log prints it as a float; the two AF CVars are
  `TAutoConsoleVariable<int32>`. The literal scanner previously skipped first characters other than `r`/`s`
  (`FindRequestedLiterals` in `hook/main_ue5_scan.cpp`); `t` is now admitted for `t.MaxFPS`. The FPS limit and AF
  settings are independent of the RR/post-processing bundles and of each other.
- Post-processing removal uses `r.Tonemapper.Sharpen`, `r.FilmGrain`, `r.Tonemapper.GrainQuantization`,
  `r.MotionBlurQuality`, `r.SceneColorFringeQuality`, and matching `ShowFlag.*` CVars including vignette. It does not
  force `r.Tonemapper.Quality` down and does not disable `ShowFlag.PostProcessMaterial`: custom materials have no
  generic semantic label distinguishing sharpen from gameplay/damage/underwater/accessibility effects.
- The policy deliberately does not spoof `SuperSamplingDenoising.Available`,
  `SuperSamplingDenoising.FeatureInitResult`, or `GetFeatureRequirements(Feature 13)`. It cannot add missing
  albedo/specular/normal/depth/motion-vector inputs, enable a disabled temporal upscaler, retrofit an older NVIDIA UE
  plugin, or make an unsupported GPU/runtime work. It also never blocks Feature 1 ordinary SR fallback: a failed RR
  create/evaluate must produce an ordinary frame rather than a blank output or engine assertion.
- CE adds authoritative lifecycle evidence around D3D11/D3D12/Vulkan NGX Create/Evaluate/Evaluate_C/Release. Feature
  13 is reported active only after a successful evaluation, not merely after handle creation; Feature 1 reports an SR
  fallback, and release republishes the remaining evaluated handles. Streamline's successful
  `slEvaluateFeature(kFeatureDLSS_RR=1001)` / `kFeatureDLSS=0` provides the same evidence when the public Streamline
  layer is visible. All transition/failure logs are bounded. Capability observations are diagnostic only.
- There is no CE hardware-ray-tracing gate. Software-RT UE projects can use the same policy when their NVIDIA plugin
  is RR-capable and their renderer supplies the required denoiser inputs. Conversely, merely shipping Streamline or
  copying `nvngx_dlssd.dll` cannot create engine-side RR integration that is absent.
- Expected proof sequence: `force policy enabled`, `persistent ... DenoiserMode=1 override installed`, Feature 13
  creation, then either NGX `Feature 13 evaluation succeeded` or Streamline `kFeatureDLSS_RR (1001) evaluation
  succeeded`. The install line reports `scanMs` plus validated/raw candidate counts. An ordinary Feature 1/feature 0
  evaluation while forced is an explicit fallback diagnostic; if it precedes the install line, RR discovery was too
  late and caused a live renderer transition. Talos runtime logs have confirmed forced Feature 13 evaluation and
  preset E without an on-disk CVar override; repeated level transitions and software-RT cases remain validation work.

## DLSS Frame Generation render preset

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
  use. CE therefore wraps that one resolution: `hook/common/ngx_fg_preset_override.cpp` returns a detour that forwards
  every call and substitutes only setting `0x10E41DF1`.
- Invariant: nvapi64.dll's code bytes are never patched. The interception is CE's existing filtered
  `nvapi_QueryInterface` GetProcAddress/IAT path (`ReflexLimiter::EnsureNvApiQueryInterfaceInterception`), for the same
  reason the Reflex limiter refuses to patch NvAPI prologues - DLSS FG integrations validate them during Reflex setup.
- Invariant: nothing is written to the machine's driver profiles. The answer is process-local, so other applications
  and later sessions are unaffected.
- `nvngx_dlssg` is classified as a Streamline/FG module, and those callers are deliberately bypassed in
  `DetourGetProcAddress`. `ShouldAllowNgxFrameGenerationPresetDynamicHook` is the single narrow exception: only that
  snippet, only `nvapi_QueryInterface`, and only while a preset is configured. `ShouldReturnWrapperToCaller` still
  refuses to hand Reflex wrappers to FG modules, so the snippet's view of NvAPI changes for the DRS getter alone.
- The substituted `NVDRS_SETTING` must look like an explicitly set current-profile DWORD: `settingLocation = 0`
  (`NVDRS_CURRENT_PROFILE_LOCATION`) and `isCurrentPredefined = 0`, because `util::drsReadKey` rejects anything else.
  Only the fields the snippet reads are written; `version` and `settingName` are left alone, and an unrecognized
  struct version is forwarded untouched. `ngx_fg_preset_override.h` mirrors the NvAPI ABI with `static_assert`s on
  `sizeof` (0x3020), `settingId`/`settingType`/`settingLocation`/`currentValue` offsets, and `NVDRS_SETTING_VER1`
  (0x13020) so a layout mistake fails the build instead of corrupting the caller's stack buffer.
- `dlss_fg_preset=default` arms nothing: no dynamic hook registration, no IAT patch, no bypass exception, and the
  wrapper is never returned. Arming happens from config load, shared-memory connect, and `nvapi64.dll` /
  `nvngx_dlssg.dll` load; the snippet's own `kernel32!GetProcAddress` import is patched at its module-load
  notification because the process-wide `PatchIATAllModules` snapshots predate it.
- Diagnostics: `NGX FG preset: armed ...`, `... GetProcAddress import patch on nvngx_dlssg.dll installed`,
  `... wrapping NvAPI_DRS_GetSetting for ...`, then rate-limited `... answered NvAPI_DRS_GetSetting(0x10E41DF1) with
  preset 'X'`. Without the wrapping line the resolution never reached CE. The snippet's own `INFO: Preset ID: %d` in
  `nvngx` logging is the independent confirmation.

## NVIDIA LOD-spread quality fix (`nv_lod_spread_fix`)

- The NVIDIA GL/VK ICD (`nvoglv64.dll` / `nvoglv32.dll`) still carries the `FERMI_UNOPT_LOD_SPREAD` driver setting.
  It defaults to OFF, which emits a LOD spread of `0` into the texture-filtering state record (header `0xA0030E46`)
  instead of `0x10`, and that is the long-standing negative-LOD-bias filtering quality bug. The literal string
  `FERMI_UNOPT_LOD_SPREAD` is **not** in either DLL; the setting is identified by its ON/OFF enum payloads
  `0x37299934` / `0x56023627`.
- **Hardware finding that chose the mechanism**: the first implementation wrote the ON payload into the setting
  global, assuming that one-reader equivalence made it interchangeable with NOPing the branch. Session
  `20260808_154051` disproved the timing assumption: the Vulkan layer created the instance at `15:42:26.778` and
  device at `15:42:26.825`; the injected hook entered `DllMain` at `15:42:27.119` and wrote the global only at
  `15:42:27.361`. The log reported success, but Filter Tester DXVK retained the stock low-quality/shimmery result.
  The process-local fix must therefore apply the exact proven branch patch before device initialization.
- The writer is `mov [<global>], eax`, guarded by `test al,al / je`, immediately after
  `mov edx, 0x3001ac; call <settings accessor>`. So **`0x003001AC` is this setting's DRS id**, the store is
  conditional on that query succeeding, and the accessor is a single internal `.data` function pointer shared by
  ~1000 setting reads (1038 disp32 slots in 1088). It is *not* an import, so CE's IAT machinery cannot see it.
- The Vulkan layer reads the host-resolved `nvLodSpreadFix` bit from shared memory. It sweeps before
  `vkCreateInstance`, then again immediately after the next layer returns successfully - the ICD is mapped at that
  point, while `vkCreateDevice` has not run. A defensive sweep also occurs at device entry. The hook DLL retains
  OpenGL and already-loaded/reloaded-module coverage, but is not the owner of Vulkan's timing.
- Detection is pattern-based and self-validating, never fixed-offset: the resolved address must actually hold one of
  the two documented payloads, and the fallback site must be a short `jcc` whose two paths load table slots exactly
  four bytes apart with the ON slot on the fall-through side. Anything else is refused and logged. Verified against
  32.0.16.1088 and 32.0.16.2012 (620.12), x64 and x86, resolving to the documented offsets in all four; on an
  already-patched driver is accepted only when `90 90`, the ON load, the skip jump, and the adjacent OFF table slot
  remain structurally provable.
- The injected hook and Vulkan layer each compile their own copy of the patcher. Their scans and protection changes
  are serialized through one per-process named mutex, so nested `VirtualProtect` calls cannot restore the code page
  to another copy's temporary writable protection. The live two-byte NOP replacement uses the narrowest aligned
  atomic word that contains the pair: 32-bit normally and 64-bit when it crosses a 32-bit boundary. The complete
  containing word participates in `InterlockedCompareExchange`, so a concurrent adjacent-byte change loses the CAS
  rather than being overwritten. A pair crossing the aligned 64-bit word remains unsupported and fails closed. The
  instruction cache is flushed before the original protection is restored, and failures report the selected width,
  whether bytes changed, cache-flush/protection state, and final byte verification.
- Strange Brigade Vulkan x64 session `20260808_214315` exposed the former 32-bit-only writer gap: 32.0.16.1088's
  validated branch is at `nvoglv64+0x4E35DB`, so its two bytes cross a 32-bit word but fit at byte 3 of the aligned
  64-bit word. All three Vulkan-boundary attempts correctly found the site and then refused it, making the option a
  no-op. The width-aware writer selects a 64-bit CAS for that exact layout. A structurally proven pre-patched driver
  is accepted before writer-width selection because it needs no live write at all.
- `nv_lod_spread_fix=off` (the default) arms nothing. The machine's driver files are never written, so they keep
  their NVIDIA signature, and other processes are unaffected. Caveat carried in `config.ini.template`: patching a
  graphics driver in memory is the kind of thing anti-cheat systems object to.
- The shared flag occupies one byte of the existing alignment gap between `msaaSamples` and `prerenderLimit`;
  compile-time offset/size assertions prove the IPC layout and ABI signature are unchanged. Host config updates,
  initial publication, the hook, and the Vulkan layer all consume the same resolved per-process value.
- Source anchors: `hook/common/nv_lod_spread_override.{h,cpp}`, the pre-device calls in
  `hook/vulkan_layer/vulkan_layer_hooks.cpp`, inject fallback in `hook/main_{hookthread,overlay_detect}.cpp`, and
  coverage in `tests/test_nv_lod_spread_override.cpp`.
- Diagnostics: `NV LOD spread: forced FERMI_UNOPT_LOD_SPREAD ON in ... (validated branch +0x...: 75 .. -> 90 90 via
  atomic 32-bit/64-bit compare/exchange, check +0x..., setting 0x...)`, or an explicit structural-validation/write
  failure with post-write state. Hardware re-validation of the corrected build remains required; the supplied failing
  runs prove the root causes and ordering, not the final pixels.

## Diagnostics and stale-risk

- Sampler logs are bounded by fingerprint/reason. Queue/fence rebinding and failed waits are high-signal and rate
  limited. The limiter's periodic stats report waited/late/reset frames, whole capture-grid slots skipped while
  preserving phase, and actual wait time. The Vulkan perf CSV populates `fps_limit_wait_us` per present (it was
  previously always 0 on the Vulkan path), and strict-grid serialization of a concurrent present is logged
  rate-limited (`lockWaitUs`, count) when the cadence lock acquisition exceeds 500us.
- Runtime validation remains required across representative native/DXVK D3D9, D3D10/11, D3D12, Vulkan, and OpenGL
  games, plus WGC and inject CFR capture. In particular, validate Kena/Blackwell, multi-swapchain engines, asynchronous
  Vulkan present queues, OpenGL shared-context applications, and Strange Brigade Vulkan at a 60fps general cap
  (must display exactly 60fps with flat frame times, with vsync on and off).
