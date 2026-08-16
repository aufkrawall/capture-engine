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
  `IsNgxModelRepositoryPath`) is unit-tested. Note the base name is **not** unique: the same
  `1B0_E658703.dll` name appears under `sl_common_0`, `sl_reflex_0`, `sl_dlss_0`, `sl_dlss_g_0`, `sl_dlss_d_0` and
  `sl_pcl_0`, so any "is this already loaded" reasoning must key on the redirect **target**, never on the
  requested base name.
- **Invariant (build 0.1.6121): a redirect must never introduce a SECOND instance of an already-loaded module base
  name.** Windows keys module identity on the resolved path, so rewriting a load whose target base name is already
  present maps a duplicate image instead of returning the loaded one. The Streamline/NGX runtimes are process-global
  singletons: a duplicate gets its own uninitialised plugin registry and cannot take effect, but CE's own export
  hooks *do* find it and forward the live instance's calls into it. `RedirectWouldDuplicateLoadedModule` in
  `hook/main_redirect.cpp` guards **both** redirect decisions (the NGX-model branch returns early with its own
  path), backed by `ce::graphics_runtime::WouldRedirectDuplicateLoadedModule`. This generalizes the rule the preload
  already followed. The override still wins every load it can actually win — a name that is not loaded yet, and a
  repeat load of the override copy itself.
- **Invariant (build 0.1.6122): the Streamline override is all-or-nothing, anchored on `sl.common`.** `sl.common`
  is the plugin manager's shared core; every other `sl.*` plugin is built against it and resolves its services
  through it, so a process runs exactly **one** Streamline distribution. CE can only place the override while it
  owns that core. The moment an image *providing* `sl.common` is observed from anywhere other than the override
  location, `g_ForeignStreamlineCoreObserved` latches and every `sl.*` redirect **and** the `sl.*` preload set are
  refused, leaving the game/driver's coherent set intact (`ShouldApplyStreamlineOverrideRedirect`,
  `StreamlineOverrideRedirectAllowed`). It is a **latch**, not a last-writer state: a core Streamline probes and
  unloads has still been resolved, and CE's own later preload must not look like a win.
  - "Providing sl.common" must be answered from the **resolved full path** — the driver's NGX cache stores every
    plugin under the same hashed base name (`ResolveStreamlineProvidedDllName` maps both `...\sl.common.dll` and
    `...\models\sl_common_0\...\<hash>.dll`). Fed from the `LdrRegisterDllNotification` callback, plus one
    `ScanLoadedModulesForForeignStreamlineCore()` pass at preload time for modules that predate CE.
  - The gate is scoped to `sl.*`. `nvngx_dlss*`/`nvngx_deepdvc`/`nvlowlatencyvk` are negotiated independently by
    NGX and the Vulkan loader, so those overrides keep working even when the Streamline stack is left alone.
- **Consequence for CE's own loader calls:** the `LdrLoadDll` hook is process-global, so CE's own
  `LoadLibrary`-by-path calls are redirected too. Anything that means "pin this exact mapped image" must resolve by
  address (`GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS`), never by re-loading a path — see
  `PinLoadedStreamlineModule` in `hook/apis/streamline_hook_resolve.cpp` and the Cyberpunk 20260816_045933 entry in
  `log/recent.md`.

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
  <!-- keep the data-pointer install contract next to the layout it depends on -->
  a redirect is only installed when the pointer it replaces has been recorded (`ce::ue5_redirect::MakePlan` /
  `CanInstall` in `hook/common/ue5_redirect_plan.h`). Until 2026-08-15 the data-pointer path left that record
  default-initialised, so restoring on config disable or hook shutdown compare-exchanged **null** into the live
  console object and the engine dereferenced it on its next read; 20 of Talos's 31 overrides used this path.
  The install also mirrors CE's value into the storage the original pointer addressed (`writeThrough=1` in the
  install log) and restores it on undo, because engine code generated for `FAutoConsoleVariableRef` CVars reads that
  global directly instead of going through the console object - the redirect alone would be invisible to it.
  otherwise the best-correlated data-pointer candidate is applied as a safe best effort.
- **Ref redirect mirrors its shadow pair too, and until 2026-08-16 it did not - which silently disabled most of the
  post-processing bundle.** Repointing the wrapper's `Ref` at CE's shadow only reaches readers that go back through
  the wrapper (`TAutoConsoleVariable::GetValueOnRenderThread()`). UE's renderer routinely resolves
  `IConsoleManager::FindTConsoleVariableDataInt(TEXT("r.X"))` once into a static and reads the returned
  `TConsoleVariableData<T>*` - the very pointer CE swapped out of the wrapper - directly from then on. Those readers
  never consult the wrapper again and kept seeing the game's original value. Every Ref-redirect install logged
  `writeThrough=0`, and **verification could not detect it**: for this mode it read CE's own shadow and compared it
  against CE's own configured value, so it always agreed. `verified 38/38` meant nothing here. The install now also
  mirrors into that `{game, render}` pair, recording both slots first
  (`ce::ue5_redirect::MakeReferencePlan` / `CanMirror`), restores values before the pointer on undo, and verification
  reads the pair back as `through=`. Measured impact on the first run with the fix (Industria 2, 20260816_012826):
  **15 of 38 overrides re-asserted on the first verification pass**, i.e. 15 had been inert - among them
  `r.SceneColorFringeQuality` (`through=0x1` vs `expected=0x0`), `r.MotionBlurQuality` (`0x4` vs `0x0`),
  `r.MaxAnisotropy` and `r.VT.MaxAnisotropy` (`0x8` vs `0x10`) and both `r.Shadow.Virtual.ResolutionLodBiasLocal*`.
  This is the answer to the standing "chromatic aberration is still visible in the Industria 2 main menu despite
  `r.SceneColorFringeQuality=0` verified" report: user-confirmed off after the fix.
- Version-conditional specs: some bundle CVars do not exist in every UE build. `r.MegaLights.DownsampleMode` and
  `r.Tonemapper.GrainQuantization` have no literal in UE 5.4 or 5.6; `r.Lumen.Reflections.DownsampleCheckerboard` and
  `r.MegaLights.NumSamplesPerPixel` exist in 5.6 but not 5.4. The summary log separates these ("literals not found in
  loaded modules") from present-but-unvalidated CVars. `ShowFlag.*` CVars are composed at runtime from a short-name
  table via `FString::Printf(TEXT("ShowFlag.%s"), ...)` in UE 5.4/5.6, so no `ShowFlag.X` literal exists to match and
  the literal scan alone cannot disable vignette. The composed names do exist in the game's
  writable memory as a contiguous UTF-16 table (present once a world/graphics state exists), but they are not embedded
  in the CVar objects and no FString references them inside the owning region, so a literal scan cannot reach the CVar
  objects. Film grain, motion blur, and chromatic aberration are already covered by `r.FilmGrain`,
  `r.MotionBlurQuality`, and `r.SceneColorFringeQuality`; vignette is the one post-processing effect that needs the
  registry path below, which does resolve it (both games, since 20260815_210850).
- **Console-registry resolution** (`hook/main_ue5_registry.cpp`, decoders in `hook/common/ue5_console_registry.h`)
  is the answer to both runtime-composed names and per-title layouts the candidate scoring rejects. `FConsoleManager`
  keeps a `TMap<FString, IConsoleObject*>` of every registered variable, so the composed name is present as an
  ordinary heap FString next to its object. CE does not hard-code that map's layout: it takes CVars the module scan
  already installed as **anchors**, walks committed private RW regions for a qword equal to a known object, and only
  accepts an element whose neighbouring FString also decodes to that CVar's name. That proves the key-to-value
  distance, after which the same allocation is re-read for the missing names and the resolved object is **probed for
  its layout** before anything is written (`InstallConsoleObjectOverride` in `hook/main_ue5_layout.cpp`, logged as
  `installed via console registry`).
  Constraints that keep it safe: every read is `ReadProcessMemory` rather than a raw dereference (a live game frees
  heap regions mid-walk, and the kernel-checked copy fails instead of raising); and it never runs before an anchor
  exists, so it can only ever add to what the scan proved. **The walk must not stop at the first confirmed
  element** - the map's storage does not fit in one heap region, and stopping early made the second pass re-read a
  single 64 KB region (Talos 20260815_202743: anchored in 47 ms, then failed to resolve
  `r.Lumen.ScreenProbeGather.Temporal.MaxFramesAccumulated`, which is certainly registered). It records each region
  holding confirmed elements and resolves across all of them; confirmations count distinct anchors (tracked by
  object address, not list index, because the anchor list is rebuilt each pass) so a CVar registered from several
  sites cannot inflate the ratio.
- **Covering the map's own allocation is the completeness bar, not whole-heap exhaustion.** A `TMap` keeps its
  elements in one allocation; VirtualQuery only reports it in pieces wherever protection or state changes, and all
  those pieces share `AllocationBase`. So once every region of that allocation has been read, a name absent from
  them is absent from the registry, however much unrelated heap was never touched. Sweeping the whole heap instead
  is both far more expensive and strictly weaker evidence *about the map*: Talos (20260816_014003) read 3698 MB
  across 32 passes, never reached the end, and correctly reported that it could not prove anything - while its map
  sat in three regions found inside the first 180 MB. `ExpandToMapAllocations` now pulls in the remaining regions of
  the owning allocation in one enumeration, sets `allocationCovered`, and **stops the sweep**, which also removes
  the 32-pass background scan. It fails closed: an `AllocationBase` that turns out to be a large shared pool rather
  than a discrete block exceeds `kMaxAllocationExpansionBytes` (256 MB), claims nothing, and leaves the whole-heap
  sweep as the fallback.
- **The sweep is resumable, and only a finished sweep may support an absence verdict** (`SweepProgress` and its
  predicates in `hook/common/ue5_console_registry.h`). The 400 ms / 768 MB bound is *per pass*, not per sweep:
  Industria 2 (20260815_214219) covered 218 MB in 406 ms, stopped mid-heap with 31 of 34 anchors placed, and the
  old code then froze that partial result - once `g_map.valid` was set the search never ran again, so all 90 retry
  passes re-read the same two regions and the regions the walk never reached stayed unexamined for the session.
  The sweep now carries a cursor, parks on the chunk it did *not* read (so the chunk overlap still guarantees no
  element falls through a pause), and resumes there on the next pass until the heap enumeration is exhausted;
  cumulative bounds are 32 passes / 8 GB, after which it says it gave up. The all-anchors-placed early exit was
  removed: placing every anchor does not prove every region of the map was seen, and completeness is what the
  verdict rests on. Consequently the closing summary distinguishes **"absent from the fully swept registry"** from
  **"not found in the N MB swept before the sweep stopped at X - unproven, not known-absent"**; the old wording
  called both "never registered by the engine". Resolve passes are time-boxed too (100 ms) and rotate their
  starting region, so a budget that expires cannot starve the regions behind it. Locating the map is the expensive
  half - once anchored, retries only re-read the recorded regions (up to 90 attempts at ~1 Hz), which covers
  `ShowFlag.*` appearing during world/graphics init rather than at startup. A TMap growth reallocates the element
  storage, so the representative element is re-checked each pass and a move restarts the whole sweep (cursor,
  confirmed anchors and region set included) rather than producing silent misses. The cursor only moves forward:
  memory committed below it after CE swept there is new memory, and element storage that *moves* is caught by the
  anchor re-check instead.
- **`ShowFlag.*` is `FConsoleVariableBitRef`, not a variable holding a value - and until 2026-08-16 CE drove it as
  one, which made all four show flag overrides inert.** A bit-ref's value is a single bit in two process-wide force
  masks: its first two qwords are `{Force0MaskPtr, Force1MaskPtr}` and its third field is the bit index, so the
  fixed `ref+0x50` model read `*Force0MaskPtr` (an all-clear mask, hence the long-standing `prevValue=0` in every
  title and engine version) and then CAS-replaced the mask pointer with CE's eight-byte shadow. The engine's own
  `GetInt()`/`Set()` then consulted CE's storage instead of the mask, so the write reached nothing the renderer
  reads *and* removed the object's only route to the real mask. The proof is in the install log itself: the
  20260816_161158 Talos session reported the identical `object+0x58` qword `0x00007FF73B3A16F0` - an address inside
  the exe's writable data - for all four ShowFlag objects. A per-variable `{game, render}` shadow pair cannot be
  identical across four different variables; a shared mask pointer is exactly that.
  CE now classifies the object before writing (`hook/common/ue5_console_layout.h`, unit-tested in
  `tests/test_ue5_console_layout.cpp`) across three shapes: **reference pointer** (`FConsoleVariableRef<T>`,
  accepted only when the shadow pair actually mirrors the global the pointer addresses - the check the ShowFlag
  objects fail), **inline pair** (`FConsoleVariable<T>`, only at the proven `+0x50` value offset because zeroed
  padding satisfies it), and **bit reference**. Bit-ref writes set/clear CE's own bit with a compare-exchange on
  the containing word, so the other show flags sharing the byte are untouched; restore puts back only that bit.
  Two independent gates keep the bit path off everything else: it is offered only to `ShowFlag.` names
  (`ce::ue5_cvar::IsShowFlagSpec`), and a candidate is only accepted once a *second* show flag independently
  reports the same mask pair with a different bit index.
- **CE does not write the force bits, and 0.1.6128 is why.** That one build drove them, and it settles the
  standing "are the masks even live in Shipping" question in the worst way: forcing the four configured flags off
  removed **all lighting** from Talos (`20260816_165501`). The reported numbers explain it. The masks came back as
  `force0=0x…16C0` / `force1=0x…16F0`, exactly 0x30 apart - two adjacent 48-byte (384-flag) bit arrays, so the
  mask discovery is right. The bit numbers are self-consistent too: `Vignette`=13 and `Grain`=14 are adjacent, and
  the engine's own show flag name table in the Talos binary (offsets `0x810d0e0`/`0x810d108`/`0x810d120`) lists
  `GlobalIllumination`, `Vignette`, `Grain` in exactly that order - which puts **`GlobalIllumination` at bit 12,
  one below the first bit CE set**. So the discovery is sound and the *mapping* is not: the index a console object
  carries is not the index the renderer reads the mask by, plausibly because `SHOWFLAG_FIXED_IN_SHIPPING` flags are
  compiled out of one side and not the other. Writing a bit therefore means guessing which flag gets turned off.
  Bit references are now classified, confirmed, and **reported only** (`ReportConfirmedBitReferences`), with the
  mask addresses, bit number, byte and mask in the log line. The classification is kept regardless: it is what
  stops the old redirect from replacing the engine's mask pointer.
- **`ProbeShowFlagBitNumbers`** resolves eight extra show flag names (`GlobalIllumination`, `Lighting`,
  `DirectLighting`, `DiffuseIndirect`, `Tonemapper`, `AntiAliasing`, `TemporalAA`, `Bloom`) through the
  already-anchored map purely to log `name -> bit`, once per session, and only when a bit reference was actually
  seen. **Measured (Talos, `20260816_201740`):** Bloom 1, Tonemapper 3, AntiAliasing 4, TemporalAA 5,
  GlobalIllumination 12, Vignette 13, Grain 14, DirectLighting 28, MotionBlur 37, SceneColorFringe 46, Lighting 109
  (`DiffuseIndirect` is not registered). GI at 12 is exactly what the binary's name table predicted, so the
  console-object side is completely self-consistent and the discrepancy is entirely on the renderer's side of the
  mask - what is still missing is how the renderer indexes it, not what the objects report. Practically the whole question only exposes vignette - grain, motion blur and chromatic aberration are
  already carried by `r.FilmGrain`, `r.MotionBlurQuality` and `r.SceneColorFringeQuality`, all verified live.
- An object matching no shape, or matching one shape at two offsets, is left untouched and its first 0x80 bytes are
  dumped (rate-limited to 6 per session). That is deliberate: guessing a layout is what produced the ShowFlag writes,
  and the dump is what lets the next run identify a per-title layout instead of re-deriving it from a refusal.
- Observed coverage: Industria 2 Demo (UE 5.6.1, session 20260815_214219) installs **38/40** requested CVars and
  verifies 38/38; Talos Reawakened (UE 5.4.4) installs 35/40. Note that the four `ShowFlag.*` counted as "installed"
  in every session before 0.1.6128 were the inert bit-ref writes described above. The remaining entries are the
  version-conditional/absent names plus the four show flag force bits. `r.Lumen.ScreenProbeGather.Temporal.MaxFramesAccumulated`
  was in this list until 0.1.6132: the layout dump showed its shadow at `object+0x58` reading `0x41C80000` = **25.0f**,
  i.e. the variable is a *float* while the spec table declared it `Int32`, so the Int32 plausibility check refused it
  every session. Correctly - installing it as an int would have written 10, a denormal float, into a float global and
  disabled temporal accumulation outright. The spec is now typed `Float`; the checks stay fail-closed in the other
  direction because an int-shaped 10 or 25 reinterprets as a denormal and is refused rather than written. A CVar that
  is "found but never validated" in one title and fine in another is a type mismatch until proven otherwise.
  Industria 2 also exercised drift recovery: two Lumen CVars were rewritten by the game ~30 s in and were
  re-asserted automatically.
- A configuration change that newly requests CVars re-opens the registry resolver (`ReopenConsoleRegistry`). The
  resolver otherwise stays closed for the rest of the session once it has finished with the previous request set,
  so enabling e.g. the post-processing bundle mid-session left its runtime-composed names permanently unresolved.
  Only the "stop looking" state is cleared: the located map, sweep coverage, and per-name layout refusals are facts
  about the title that a new request does not change.
- **Stale-risk / unverified:** the show flag force bits have **no end-to-end evidence** comparable to the RR
  bundle's `Feature 13 evaluation succeeded`, and neither did the redirect they replace. Note that
  chromatic aberration turned off only once the *Ref-redirect* path started mirroring, which is evidence that
  `r.SceneColorFringeQuality` (not `ShowFlag.SceneColorFringe`) is what actually gates the effect in UE 5.6. Talos in-game (session 20260815_191332) is the
  end-to-end proof for the RR bundle: `r.NGX.DLSS.DenoiserMode=1` installed as a Ref redirect and NGX then logged
  `Feature 13 evaluation succeeded; Ray Reconstruction is rendering`, with no Feature 1 SR fallback for the rest of
  the session. Ref redirect is therefore proven to reach the engine *through the wrapper* - which, as the mirroring
  defect above showed, says nothing about readers that cached the shadow pair instead. **No data-pointer-mode
  override has an equivalent end-to-end proof yet**, which is what the write-through and the read-back verification
  below exist for.
- **Read-back verification** (`VerifyOverrides` in `hook/main_ue5_install.cpp`, once a second from
  `RefreshOverrides`) closes the gap between "the write succeeded" and "the value is live". It re-reads the redirect
  slot, CE's shadow, and the write-through storage for every installed override. A slot no longer pointing at CE's
  shadow means the game re-registered the variable: the record is retired, the mirrored value handed back, and a
  rescan requested. A value that drifted means a game-side `Set()` reached storage CE owns: the configured value is
  re-asserted rather than reinstalled. Reporting is per-override capped at three and only lifts after 60 consecutive
  clean passes, so a constantly rewritten CVar stays quiet while a rare regression is still logged; the summary line
  is emitted only when the counts change. There is no bit-reference branch here: CE never writes a force bit, so
  there is nothing to verify or restore for one.
- The same machinery overrides `t.MaxFPS`, `r.MaxAnisotropy`, and `r.VT.MaxAnisotropy` (shared-memory ABI 40 added
  `internalFpsLimit` and `internalAnisotropicFiltering`). `t.MaxFPS` is a `TAutoConsoleVariable<float>` in UE5, so
  the spec uses the float value type and the scan log prints it as a float; the two AF CVars are
  `TAutoConsoleVariable<int32>`. The literal scanner previously skipped first characters other than `r`/`s`
  (`FindRequestedLiterals` in `hook/main_ue5_scan.cpp`); `t` is now admitted for `t.MaxFPS`. The FPS limit and AF
  settings are independent of the RR/post-processing bundles and of each other.
- Third-party overlay coexistence at DX12 hook install: CE reads the Present vtable from a temp 2x2 swapchain,
  and creating it can enter an overlay that hooked the creation path first. Steam dispatches through callback
  slots that stay NULL until it has rendered on a real game swapchain, so entering its handler during startup
  recurses until the stack is gone (`0xC00000FD`, 750+ `gameoverlayrenderer64!OverlayHookD3D3` frames; 4 of 21
  sessions on 2026-08-15). `DX12Hook::Init` deferred the eager install for exactly this reason but
  `FindAndWrapPreExistingSwapchains` ran it on the next line anyway, so the guard was a no-op. Both the eager
  and the deferred decision now use the same startup window (`ShouldDeferEarlyDX12TempSwapchainPresentHookInstall`
  / `ShouldPostponeDeferredTempSwapchainPresentHookInstall`), keyed on the game's own D3D12 device, and
  `DX12Hook::ServicePendingPresentHooks` retries from the hook thread's service pass. Invariant, unit-tested:
  the postponement never outlasts the deferral condition, so a swapchain that pre-dates injection cannot be
  left without Present hooks.
- The UE5 override code is six units: `main_ue5.cpp` (policy/lifecycle), `main_ue5_scan.cpp` (literal and candidate
  discovery), `main_ue5_install.cpp` (install/refresh/verify/restore), `main_ue5_layout.cpp` (console-object layout
  probing and the three console-object install modes), `main_ue5_memory.cpp` (process-memory and PE primitives), and
  `main_ue5_registry.cpp` (console-registry resolution), with shared declarations in `main_ue5_internal.h`. The
  original split happened when `main_ue5_scan.cpp` had grown to 869 lines, past the 800-line ceiling, without being
  recorded in `tools/file_size_baseline.json` - lint would have failed on the next run.
- Post-processing removal uses `r.Tonemapper.Sharpen`, `r.FilmGrain`, `r.Tonemapper.GrainQuantization`,
  `r.MotionBlurQuality`, `r.SceneColorFringeQuality`, and matching `ShowFlag.*` CVars including vignette. It does not
  force `r.Tonemapper.Quality` down and does not disable `ShowFlag.PostProcessMaterial`: custom materials have no
  generic semantic label distinguishing sharpen from gameplay/damage/underwater/accessibility effects.
- **Film grain coverage is complete at the name level**; the gap was never a missing CVar. UE 5.1+ gates the whole
  film grain pass on `r.FilmGrain` (verified live in Talos: `1 -> 0`), `ShowFlag.Grain` gates it as a show flag, and
  `r.Tonemapper.GrainQuantization` is the UE4-era equivalent - kept because the layout machinery works on UE4 titles
  too, at the cost of one "literal not found" line on UE5. The remaining `r.FilmGrain.*` CVars
  (`SequenceLength`, `CacheTextureConstants`, `Texture`) shape grain rather than disable it, and per-volume
  `FilmGrainIntensity` is already gated by `r.FilmGrain`. What was actually broken was `ShowFlag.Grain` being an
  inert bit-ref write; adding further names would not have fixed it. Do not add a guessed CVar name here:
  `r.MegaLights.DownsampleMode` and `r.Tonemapper.GrainQuantization` each cost a permanent unresolved entry, and
  a name that exists in no engine version cannot be distinguished from one CE simply failed to reach.
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
