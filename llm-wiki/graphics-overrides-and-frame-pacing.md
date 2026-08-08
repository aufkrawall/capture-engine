# Graphics Overrides And Frame Pacing

Last cross-checked: 2026-08-08

Primary sources:
- `common/config.{h,cpp}`
- `common/mip_mapping_policy.h`
- `common/strict_float_parse.h`
- `common/shared_defs.h`
- `hook/common/{hook_common,dxgi_shared,fps_limiter,fps_limiter_policy,sampler_override_utils,dlss_indicator_spoof}.*`
- `hook/common/{ngx_module_policy.h,ngx_fg_preset_override.*,reflex_limiter.h}`
- `hook/wrappers/{iat_hook.h,iat_hook_init.cpp}`
- `hook/apis/{dx9_hook,dx9_sampler_state,legacy_d3d_sampler_state,dx11_hook,dx12_hook,dx12_sampler_hooks,nvngx_hook,opengl_hook,opengl_sampler_override,opengl_texture_storage_override}.cpp`
- `hook/vulkan_layer/vulkan_layer.{h,cpp}`
- `hook/vulkan_layer/vulkan_sampler_policy.h`
- `tests/{test_config,test_mip_mapping_policy,test_sampler_override_utils,test_dx12_sampler_policy,test_fps_limiter,test_dlss_indicator_spoof,test_ngx_module_policy,test_ngx_fg_preset_override}.cpp`

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
- The parameter machinery downstream (vtable hooks on `SetUI`/`SetI` plus `InjectPreset` at parameter creation) was
  already complete; it simply never ran. `nvngx_debug.log` showing only `Config forced SR Preset ... (via Install)` and
  no `SetUI`/`CreateFeature` lines is the signature of interception never engaging.

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
