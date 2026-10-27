# Third-Party DLL Loading (ReShade / OptiScaler / Special K / Custom DLLs)

Last cross-checked: 2026-10-27 (GTA on vkd3d-proton: DXVK hides the NVIDIA GPU unless told not to; FG had no flip metering through dxvk-nvapi/vkd3d - CE's SetFlipConfig -> VK_NV_present_metering bridge built, hardware run pending; DX12 PostSL route kept out of Vulkan-owned processes); 2026-10-26 (System32 D3D10/11 adapter guard for Epic's overlay, 0.1.7347; resident-runtime replacement and Vulkan probe ownership; 0.1.7345 GTA run reaches Vulkan, then native capture interop misroutes; dump residue puts the fault in native D3D11 on DXVK's adapter; resolver correction plus fail-closed adapter guard regression-tested, field rerun pending); 2026-10-25 (tool-thread suspension access fix); 2026-10-23 (OptiScaler + Streamline hook cycle, foreign entry policy)

Primary sources:
- `hook/hooking/inline_hook_policy.h` (`ForeignEntryPolicy`, `ClassifyEntryJumpOwner`)
- `hook/streamline/streamline_foreign_entry_policy.h`, `hook/streamline/streamline_inline_hook_batch.cpp`
- `common/config/config.h` (`ThirdPartyConfig`, `AppConfig::thirdParty`, `ParseThirdPartyCustomDllPaths`)
- `common/config/config_load_third_party.cpp`
- `common/config/config_load.cpp` (loader call order)
- `hook/runtime/third_party_load_policy.h`
- `hook/runtime/main_thirdparty_load.cpp`
- `hook/runtime/tool_thread_suspension.{h,cpp}` (`ToolThreadSuspension`; tests `tests/test_tool_thread_suspension.cpp`)
- `hook/runtime/main_hookthread.cpp` (call site)
- `hook/runtime/main_loadlibrary.cpp` (`LoadRuntimeDllViaOriginal`)
- `hook/runtime/main_overlay_detect.cpp` (shared proxy-name matcher)
- `hook/overlay/overlay_compat_detail/module_table.h`
- `hook/runtime/module_replacement_policy.h` (pure rules), `hook/runtime/module_replacement.{h,cpp}` (executor),
  `hook/runtime/module_replacement_exports.cpp` (typed native/replacement entry pairs),
  `hook/hooking/iat_hook_retarget.cpp` (`IATHook::RetargetOriginals`)
- `hook/hooking/iat_hook_init.cpp` (`DetourGetProcAddress`, CE caller bypass)
- `hook/vulkan_layer/vulkan_presentation_ownership.h`, `layer_ipc.cpp`, `vulkan_layer_swapchain.cpp`
- `tests/test_config_third_party.cpp`
- `tests/test_third_party_load_policy.cpp`
- `tests/test_inject_capture_source_part2.cpp`
- `hook/hooking/unhooked_module_export.h`, `tests/test_unhooked_module_export.cpp`
- `hook/ngx/flip_metering_bridge{.h,.cpp,_policy.h}`, `hook/vulkan_layer/vulkan_flip_metering_bridge{.h,.cpp,_policy.h}`,
  `tests/test_flip_metering_bridge.cpp` (D3D12 flip metering on vkd3d-proton)
- `tests/test_module_replacement_policy.cpp`, `tests/flow/test_flow_module_replacement.cpp` with
  `tests/flow/fakes/replacement/` (a proxy `dxgi.dll` and a late importer)

## Scope
This page describes how CaptureEngine's injected hook loads user-supplied
ReShade, OptiScaler, Special K, and arbitrary custom third-party DLLs so they
work without being copied into game folders. It is complementary to
`dx12-overlay-third-party-coexistence.md`, which covers hook-chain ownership
once those tools are loaded.

## Facts
- `[ThirdParty] reshade_dll_path`, `optiscaler_dll_path`, `specialk_dll_path`,
  and `custom_dll_paths` configure the loads. Empty disables a tool/list. For
  the three named tools, a value whose final path component contains a `.` is a
  file loaded verbatim; anything else is a folder and the per-bitness default
  name is appended: `ReShade64.dll` / `ReShade32.dll`, `SpecialK64.dll` /
  `SpecialK32.dll`, `OptiScaler.dll` (single name for both architectures).
  `custom_dll_paths` is a comma-separated list of explicit DLL file paths
  (`ParseThirdPartyCustomDllPaths`): internal spaces in folder/file names are
  preserved, entries may be unquoted or single/double-quoted (including
  retaining quotes at the section-reading boundary for a single comma-containing
  path as well as a list), and duplicate paths within the list are collapsed in order.
  Per-profile overrides use `ThirdParty.<key>` inside `[Profile.*]`;
  `ThirdParty.custom_dll_paths=off` (or `none`) clears a globally configured
  custom DLL list for that profile.
- The new fields live in `ThirdPartyConfig` on `AppConfig`, deliberately NOT in
  `GraphicsConfig`/`SharedGraphicsConfig`: the hook reads them directly from
  `config.ini` (same file the host writes), so no shared-memory layout change,
  no `SHARED_MEMORY_VERSION` bump, and no IPC transport exists for them.
- The hook loads the configured tools in the fixed order Special K -> ReShade
  -> OptiScaler (the `Tool` enum declaration order in
  `third_party_load_policy.h`), followed by `custom_dll_paths` in their listed
  order. Duplicate and file-existence checks precede all waits/suspension. Resident
  known tools seed the thread-range inventory even without a configured load path.
  Before every actual load with previously loaded tools, the
  executor (a) waits for the Windows loader work queue to drain by joining a
  trivial `LoadLibrary` probe thread, and (b) suspends the threads owned by
  previously loaded tools for the duration of the load (threads identified by
  their start address inside a loaded tool module; two enumeration passes, no
  globally stable snapshot requirement, plus a bounded post-suspension probe
  with resume-and-retry when a tool thread was caught inside the loader).
  Tool threads are opened with `THREAD_QUERY_INFORMATION`: the Win32 start-address query reads nullptr
  through a limited handle, and from `cfa42fb4` (2026-08-13) until 2026-10-25 that made the filter match
  nothing, so the suspension never suspended a thread and only the loader-quiescence wait protected
  the loads. The same fix reset the processed count in `ResumeAll` (a retry re-found every thread and
  suspended none) and resumes only suspensions it made (a refused or held thread is not its own).
  A tool thread held suspended by another owner is left alone. The suspension as designed has not
  had a field run yet; the load line says how many it paused: `ThirdParty preload: <tool> loaded from
  ... toolThreadsSuspended=N`.
  Game and driver threads are never suspended — the first all-threads version
  was unreliable in a busy game and coincided with a driver crash
  (`nvwgf2umx`, session `20260813_033707`), while a run where suspension gave
  up entirely (`20260813_033912`) succeeded, showing the deadlock is a race.
  With the tool threads suspended, Special K's enumerator cannot hold its
  thread-hook critical section across a loader call, so OptiScaler's DllMain
  thread creation proceeds. Simple reordering and the quiescence wait alone
  were proven insufficient: sessions `20260813_020236` and `20260813_025615`
  (Special-K-first without suspension), `20260813_021731` and
  `20260813_031321` (Special-K-last, with/without the wait). Do not reorder,
  remove the wait, or drop the suspension without re-checking all sessions.
- `PreloadConfiguredThirdPartyDlls()` runs in `HookThread` immediately after
  the local `config.ini` parse, before CE's wrapper DLL load,
  `PreloadConfiguredGraphicsRuntimeDlls()`, and per-API hook installation.
  `InstallGlobalVTableHooks()` still runs first; CE's below-the-foreign-chain
  Present coexistence logic handles the resulting layering.
- Each tool is loaded through `LoadRuntimeDllViaOriginal` (the original
  `LoadLibraryW`, bypassing CE's redirect hook) and then
  `NotifyHookModuleLoaded`, so the overlay-compatibility tracked-module
  registry observes it and the Present routing sees it as a foreign overlay.
- Duplicate-load protection: a named tool is skipped when a canonical base name
  is already mapped (`SpecialK64/32.dll`, `SpecialK.dll`, `ReShade64/32.dll`,
  `ReShade.dll`, `OptiScaler.dll`, `OptiScaler.asi`), or when a renamed copy
  occupies a graphics proxy name (`dxgi.dll`, `d3d*.dll`, ...) and matches the
  tool's export/version markers (`ReShadeVersion`/`ReShadeRegisterAddon`,
  `SK_GetDLL`/`SK_Inject_GetRecord`, OptiScaler version resource). The proxy
  name list is shared with `main_overlay_detect.cpp` via
  `ce::overlay_compat::IsThirdPartyGraphicsProxyCandidateName`, so the
  preloader and identity detection cannot disagree about which tools are
  present. Custom DLLs in `custom_dll_paths` check full-path module equivalence
  (`AreModulePathsEquivalent` after `GetFullPathNameW` vs `GetModuleFileNameW`,
  plus canonical tool base-name check via `FindKnownToolByBaseName`) rather than
  bare `GetModuleHandleA(baseName)`, so a custom DLL named `dxgi.dll` is not
  falsely skipped because `System32\dxgi.dll` is already mapped.
- Every outcome is logged exactly once per tool with `HookLog` /
  `HookLogImportant`: loaded (module handle), already loaded (kept),
  not found at resolved path, directory instead of file, invalid UTF-8 path, or
  failed load with `GetLastError()` (plus an `ERROR_BAD_EXE_FORMAT`
  wrong-architecture hint). A failure never aborts CE's own hook initialization.
- Loading happens once per injection (static atomic latch). Changing the paths
  requires relaunching the game, matching the existing DLSS/Streamline path
  override behavior.

- **OptiScaler's frame generation output (2026-10-23):** OptiScaler keeps `libxess_fg.dll`, `libxell.dll` and
  `amd_fidelityfx_framegeneration_dx12.dll` in its own folder; CE identifies frame-generation runtimes by base name
  wherever they live (`IsFrameGenerationRuntimeModulePath`), never as part of an overlay. With `FGInput=DLSSG`
  OptiScaler serves the game's DLSS-G API itself, which CE treats as a front, not DLSS FG. XeFG output support,
  evidence and pending validation: `frame-generation/xefg.md`.

## Tool-side prerequisites (not CE's concern)
- Each tool still reads its own per-game configuration next to the game
  executable: `ReShade.ini`, `OptiScaler.ini`, Special K's profile.
- Keep OptiScaler's `LoadReshade`/`LoadSpecialK` INI switches off when CE
  manages all three loads, otherwise a tool can be loaded twice.
- ReShade hooks Vulkan through its layer mechanism, not through
  `LoadLibrary`, so ReShade-in-Vulkan titles still need ReShade's Vulkan layer
  registered by the user. Special K hooks `vulkan-1.dll` exports itself.
- A custom DLL named like a module the game already resolved (a proxy or a
  translation runtime named `dxgi.dll`/`d3d*.dll`) replaces it; see the next section.
- Late injection into an already-running game is best effort: the tools may
  miss early device-creation hooks, the same limitation CE's own overlay has
  for pre-existing swapchains.

## Custom DLL replacing a resident module (2026-10-26)
- **Cause (session `20261026_145243`, 0.1.7342, GTA V Enhanced, `custom_dll_paths` = DXVK `dxgi.dll` +
  vkd3d-proton `d3d12.dll`/`d3d12core.dll`):** the game ran on native D3D12. CE's DllMain (initial-thread APC,
  before the image entry point) already found `ModulePin: pinned name=dxgi.dll ... (C:\WINDOWS\SYSTEM32\dxgi.dll)`
  and the same for `d3d12.dll`. Neither `GTA5_Enhanced.exe` nor its first-level static imports import them
  (`MFPlat.DLL` only delay-loads them); something initialized during process init maps them - likely
  `dstoragecore.dll` (imports both), which the static `DSTORAGE.dll` loads - unverified. The preload then
  logged `ThirdParty preload: dxgi.dll loaded from
  ...\vkd3d\dxgi.dll` etc. - second images. Windows resolves every by-name request (import binding, bare-name
  `LoadLibrary`, `GetModuleHandle`) to the first image of a name, so nothing reached them; vkd3d's
  `d3d12core.dll` even bound its own `dxgi.dll` import to System32's. CE cannot win that race: static imports
  are mapped before any APC runs.
- **Rule:** a custom DLL whose base name a by-name lookup still resolves to another image after the preload
  (`GetModuleHandleW(base) != module`) is adopted as that name's replacement, for *application* code only
  (`ClassifyRequesterPath`): Windows modules (System32/SysWOW64/WinSxS, incl. DriverStore) keep the system image
  (System32's `d3d11.dll` needs System32's `dxgi.dll` internals), CE modules keep their explicit System32 loads
  (the Vulkan layer's D3D11 interop). `RequesterScope`: Windows = none; CE modules and configured custom DLLs =
  bare names only (a proxy reaches the image it wraps by full path; redirecting that would make it call
  itself); other application code = bare names plus the superseded image's own path and handle (Streamline's
  interposer loads `<GetSystemDirectory>\dxgi.dll`/`d3d12.dll` by full path - strings in `sl.interposer.dll`).
- **Already mapped custom images:** an exact configured-path match keeps a process-lifetime module reference and enters the same adoption list as a newly loaded image. This includes dependencies loaded by an earlier custom entry. Merely returning from the already-loaded branch skipped adoption and custom caller classification; `FlowModuleReplacement.AdoptsAConfiguredRuntimeThatWasAlreadyLoaded` failed both factory routing and module identity before this correction. Its diagnostic is `ThirdParty preload: ... adopting the configured existing copy`.
- **Factory export cache:** `hook/hooking/unhooked_module_export.h` owns disk export lookup and a coherent per-module cache. Direct code RVAs bypass in-memory EAT edits; an RVA inside the export directory is a forwarding string and uses Windows loader resolution (API sets and ordinals included). Cache resolution runs outside its lock, and each pinned module retains its own result, so overlapping native/replacement lookups cannot publish a mismatched pair. Missing exports are retried. The factory caches deliberately survive static destruction, because other DLLs can create factories during detach. `UnhookedModuleExportTest.*` covers native direct/forwarded exports, missing inputs, out-of-order resolution, reentry and retry.
- **What adoption does** (`AdoptCustomModules`, once, after the whole list so one replacement's imports of
  another move): `IATHook::RetargetOriginals` moves CE's forwarding originals that point into the superseded
  image (dynamic GetProcAddress hooks such as `oCreateDXGIFactory2`, `oD3D12CreateDeviceRaw`, and the originals
  recorded for patched slots) to the replacement's export of the same name; then an import sweep rebinds every
  application module's slots that still point into the superseded image (`PlanImportRebind` + one
  `RebindIATBaselinesTransaction` per module; slots owned by CE's wrappers or another tool stay, names the
  replacement lacks stay native and are logged). Native runtime components living beside the game are not
  rebound (`IsRuntimeComponentOf`: the Agility SDK's `D3D12Core.dll`/`d3d12SDKLayers.dll` serve System32's
  `d3d12.dll`, which CE's own native bootstrap still uses), unless they are configured custom DLLs themselves.
- **Afterwards:** late modules are rebound from the loader notification. Verified on Windows 11 26300
  (`ModuleReplacementLoaderTest.LoaderNotificationSeesBoundImports`, and the flow fake's import slot read in its
  `DllMain`): the notification arrives with the imports bound and before `DllMain`. A module seen unbound anyway is
  queued and retried when its load returns (`NotifyHookModuleLoaded`) or on the hook thread's service pass.
  `HookedLoadLibrary*` (module-level IAT hooks, which know the caller; `LdrLoadDll` is shared with Windows and CE
  and never substitutes), the `GetModuleHandle*` hooks (now arch-neutral; on x86 installed only once a
  replacement is active) and `DetourGetProcAddress` (`ModuleForProcLookup`) answer per `RequesterScope`.
  `PatchLoadLibraryIatForLateLoadedModule` runs whenever a replacement is active, and adoption patches the
  loader imports of every module mapped since DllMain. CE's wrapper paths that bypass the originals use the
  application's module too: `Wrapped_CreateDXGIFactory*` (`GetUnhookedDXGIExport`/`GetLiveDXGIExport`, re-read
  when that module changes), `PinApplicationModule` in `iat_hook_init.cpp`; identity checks use
  `ApplicationModuleByName` (`Dx12OverlayApiLabel` -> "DX12 (VKD3D-Proton)", `IsDXVKD3D11WrapperLoaded`).
- **Cached export entries (2026-10-26 follow-up):** import rebinding and loader hooks cannot redirect a native
  function address cached before adoption or obtained by walking the export table (reported in the GTA follow-up;
  reproduced by `FlowModuleReplacement` with `ResolveAddressDirect`). Adoption installs SDK-typed entry detours
  for known DXGI/D3D10/10.1/11/12 creation, debug/interface and D3D12 serialization exports present in both images.
  Native entries forward application calls; Windows, CE, superseded images, native Agility components and configured
  proxies keep the native predecessor. The caller lookup reuses `module_address_cache` and its unload generation.
  Native-forwarded export aliases are left alone. D3D8/9/DirectDraw entries are excluded because existing CE hooks
  own them; no cached-entry coverage is claimed for those APIs or arbitrary custom export names.
- **Tail-call invariant:** a configured proxy can tail-call System32, keeping the *game's* return address. Caller
  classification alone then sends it back to the proxy forever (reproduced with the flow fake). Each entry pair
  therefore also wraps the replacement entry and holds a thread-local native-runtime scope while calling either
  predecessor. This protects both a cached native entry and an import already bound directly to the proxy.
  `InstallPublishedBatch` publishes both callable trampolines before patches become live; a missing replacement
  trampoline selects the native predecessor. Adoption remains active when the CE host goes dormant, since rebound
  imports and already-created application objects continue using the replacement.
- **Vulkan adapter probes:** layer IPC connection/instance creation is eligibility, not presentation evidence.
  DXVK's DXGI can enumerate Vulkan adapters while a game still presents native D3D12. `Ownership` starts the
  PID-scoped claim immediately around the driver's actual `vkCreateSwapchainKHR` (its private DXGI WSI must be
  passed through), rolls it back on failure, and retains it for successful live swapchains. Replacement and
  overlapping creations preserve ownership without a gap; destroying the last swapchain releases it. Reconnect
  republishes only existing swapchains/in-flight creation, not an enumeration-only instance. The shared-memory
  layout and inherited runtime-override claim are unchanged. Tests: `VulkanPresentationOwnershipTest.*`.
- **Limits:** objects created on the native runtime before adoption stay native. Delay-load slots are not rebound,
  although calls through covered native entry points are forwarded. CE-internal raw `GetModuleHandleA("d3d12.dll")`
  presence checks still see the first image (CE's own module is not IAT-hooked). Hooks already present at an entry
  are chained through the inline-hook primitive; actual coexistence with every foreign injector is not established
  by the proxy regression.
- **Native capture interop (GTA session `20261026_165613`, 0.1.7345):** the game reached replacement
  `D3D12CreateDevice`, DXGI factory and root-signature exports. The driver's Vulkan swapchain creation succeeded
  at 3840x2160 and CE's Vulkan overlay pipeline initialized successfully. Immediately afterwards the layer's
  explicit System32 `CreateDXGIFactory1` lookup was intercepted and its wrapper selected the *DXVK* export
  (`selectedModule=...\vkd3d\dxgi.dll`), then `CreateD3D11InteropDevice` used native D3D11 with that adapter.
  This is a CE bug: `DetourGetProcAddress` exempted `capture_hook`/`d3d12_wrappers`, but omitted `VK_LAYER_CE`.
  It now reuses `ClassifyRequesterPath` for all CE modules. A rate-limited `GetProcAddress: preserving ... for CE
  module ...` diagnostic records the bypass. The extended flow fake loaded as `VK_LAYER_CE_overlay.dll` failed
  before this correction (wrapper address returned, replacement count incremented), then passed with a native
  export and no replacement call. Lookup helpers must preserve their frame: a tail call attributes the lookup
  to the test executable instead of the fake layer.
- **Crash attribution:** the pre-termination dump records `NtTerminateProcess(0xC0000005)` on thread `45e4`
  (`vkd3d_queue`, the thread that logged the interop lines), after `RtlUserThreadStart` unwound the fault, so no
  frame chain survives. The unwound stack below CE's dump handler still holds the faulting call's residue:
  `dps 00000056ef5fcc00` shows `d3d11!CCreateDeviceCache::CAdapterCache::GetWddmVersion+0x2f` with DXVK's
  `dxgi_7ffb3a2a0000+0x4556` and `d3d11` strings beside it. System32's D3D11 faulted while reading the DXVK
  adapter's private state inside `D3D11CreateDevice`. It is residue, not a stack walk, but it matches the last
  log line (`Wrapped_D3D11CreateDevice: CALLED` from the layer) and the vendor-spoofed adapter (`VendorId=1002`).
- **Fail-closed guard (`hook/runtime/com_object_module.h`):** `CreateD3D11InteropDevice` passes an adapter to
  System32's D3D11 only when its vtable lies in the System32 `dxgi.dll` image the layer loaded by full path (the
  module's path is checked too: the fallback bare-name load can be answered by a replacement). Anything else logs
  `Vulkan Layer: [Warn] Refusing native D3D11 interop: adapter ... is implemented by <path>` (first 4, then every
  64th) and returns no interop device: Vulkan capture fails, the game does not. Tests: `ComObjectModuleTest.*`,
  including a real System32 factory/adapter pair that must pass the guard.
- **Second crash (GTA session `20261026_175321`, 0.1.7347):** interop then got NVIDIA's real adapter (`VendorId=10de`),
  capture initialized and the first frame drew the overlay. About 0.9 s later the `vkd3d_queue` present thread died
  of `0xC0000005` again. The recorded first-chance fault survived in the dump: `crash_first_chance` slot 0 at
  `capture_hook_x64+...496160` (thread `0x7458`, code `c0000005`, address
  `d3d11!CCreateDeviceCache::CAdapterCache::GetWddmVersion+0x2f`, read of `0`). `.cxr` on the CONTEXT at slot+0xB0
  shows `rdx=0x80004002` (E_NOINTERFACE) and `mov rax,[rcx]` with `rcx=0`. D3D11 asked an adapter for its private
  interface and got none. The caller was not CE: the log shows
  `dxgi.dll!CreateDXGIFactory1 entry called from EOSOVH-Win64-Shipping.dll -> replacement` at 17:53:50, and
  `GetProcAddress(System32 d3d11, "D3D11CreateDevice")` returning `EOSOVH_Win64_Shipping+0x3c280` (Epic's overlay
  intercepts that lookup). Epic's Vulkan overlay builds a native D3D11 device on the adapter of the factory it got,
  which is DXVK's under the replacement. DXVK's `dxgi.dll` beside the executable would presumably hand Epic the
  same adapter (not verified).
- **Native adapter guard (`hook/runtime/module_replacement_adapters.cpp`):** while `dxgi.dll` is replaced and a
  System32 D3D10/D3D11 runtime is not, that runtime's `D3D11CreateDevice`, `D3D11CreateDeviceAndSwapChain`,
  `D3D10CreateDevice(AndSwapChain)` and `D3D10CreateDevice1`/`D3D10CreateDeviceAndSwapChain1` code entries
  (resolved from the export table, so lookup interceptors do not hide them) are inline-guarded. An adapter whose
  vtable lies in the replacement image is exchanged for System32's adapter with the same LUID
  (`IDXGIFactory4::EnumAdapterByLuid` on a factory from System32's own entry; CE's call is never forwarded). No
  match fails the call with `DXGI_ERROR_UNSUPPORTED` rather than letting the runtime fault. Installed at adoption,
  after loads return and from the hook thread's service pass (a load notification only sets a flag; the inline
  patch is never written under the loader lock); the guarded image is pinned. A replaced D3D11 keeps no guard: its
  forwarders send application calls to the replacement runtime, which wants the replacement's adapters. Flow
  regression `FlowModuleReplacement.NativeD3D11ReceivesTheSystemAdapterForAReplacementAdapter` uses a fake
  translation adapter (`dxgi_translated_adapter.cpp`, answering only public IIDs) with the code entry called
  directly. D3D11On12 with a vkd3d-proton D3D12 device cannot be translated and is not guarded (open).
- **Dumps now keep that fault:** `ShouldDumpWithRecordedFault` (crash_dump_policy.h) attaches the thread's recorded
  fault to a pre-termination dump when the crash-like exit code equals its exception code. That is the shape of
  `RtlUserThreadStart`'s top-level `__except`, which unwinds before it calls `NtTerminateProcess`. Both GTA dumps had
  only CE's termination hook as context.
- **Logs:** `ModuleReplacement: CE forwarding originals for <name> moved to the replacement (dynamic=N tracked
  slots=N missing=N first=...)`, `ModuleReplacement: <name> now resolves to <path> for application code instead
  of <path>`, `ModuleReplacement: initial import sweep rebound N slot(s) in N module(s) (kept foreign=... missing
  exports=... not yet bound=... failed transactions=...)`, `ModuleReplacement: rebound N import slot(s) of
  late-loaded <module>` (ChangeGate per module), and rate-limited `ModuleReplacement: <LoadLibrary|module
  query|GetProcAddress on the superseded image> for <name> from <caller> -> replacement` (first 16, then every
  1000th). Entry adoption logs installed pairs, native aliases, missing exports and failures; forwarded calls log
  `ModuleReplacement: <module>!<export> entry called from <caller> -> replacement` (ChangeGate keyed by caller
  module, 5 s heartbeat with suppressed counts: a new caller is always logged). The adapter guard logs
  `ModuleReplacement: System32 d3d11.dll creates devices only on System32 adapters while dxgi.dll is replaced
  (guarded=N ...)` per runtime and `... from <caller> received an adapter of the replaced dxgi.dll; passed
  System32's adapter with the same LUID ...` (first 4, then every 64th per entry). `Vulkan ownership:` logs
  swapchain creation/lifetime and release transitions.
- **Status:** 0.1.7347 field-validated the resolver correction (native interop adapter, capture initialized, overlay
  drawn) and exposed the Epic overlay crash above. 0.1.7354 ran GTA on vkd3d-proton for minutes with the overlay
  drawn and DLSS SR + FG 2x active (`20261027_140018`); recording on this stack remains pending a field run.

### NVIDIA features on a DXVK/vkd3d-proton stack (2026-10-27)
- **DLSS hidden by DXVK, not by CE.** `20261027_132910` (0.1.7354; DXVK 3.1.1 `dxgi.dll`, vkd3d-proton
  `d3d12`/`d3d12core`, dxvk-nvapi 0.9.2 `nvapi64.dll`): every DLL loaded and routed, yet GTA offered no DLSS.
  Streamline unloaded `sl.dlss_g`/`sl.deepdvc`/`sl.directsr` right after init, Reflex `slGetFeatureFunction`
  returned 21, and no `NVNGX: Found` line appeared - the NGX core never loaded, so Streamline rejected the adapter.
  DXVK's `dxgi.hideNvidiaGpu` (default `Auto`) reports an AMD vendor ID unless `DXVK_ENABLE_NVAPI=1`; Proton's
  `PROTON_ENABLE_NVAPI=1` sets it, loading dxvk-nvapi alone does not. `dxgi.hideNvidiaGpu = False` for the game
  restored DLSS SR and FG (`20261027_140018`). `vulkan_layer.log`'s `Interop adapter: ... VendorId=10de` is CE's
  own System32 query and says nothing about the adapter the game sees.
- **FG pacing: no flip metering through this stack.** Native D3D12 DLSS-G meters flips with
  `NvAPI_D3D12_SetFlipConfig(queue, vidpnSrcID, NV_FLIP_CONFIG*)` (public NVAPI since R570, interface ID
  `0xf3148c42`, called "right before the first Present() of each batch"). dxvk-nvapi 0.9.2 carries the name only in
  its function-ID table (beside unimplemented `NvAPI_GSync_*`) and `nvapi_d3d12.cpp` has no implementation;
  vkd3d-proton's `d3d12core.dll` has no `VK_NV_present_metering` reference and its device enables no such extension.
  `sensors.log` reads `nvFlipSchedule(... applied=0)` in every vkd3d session, against `applied=2728
  avgDelayUs=3778` for the native D3D12 `fgtests` harness on the same PC. In `20261027_140018` (FG 2x, ~122 fps,
  144 Hz VRR, GPU 99 %) `vkQueuePresentKHR` intervals alternate ~7.5/8.1 ms (presentJag 0.4-0.5 ms) while the
  display-change series reads stddev 1.8 ms, jag 2.7-3.1 ms, p1 5.5 / p99 10.5 ms: the generator's CPU pacing is
  close, the flips follow GPU completion. `20261027_140552` with `vsync_mode=default` shows the same (stddev 2.1 ms,
  jag 3.8 ms), so CE's forced FIFO is not the cause.
- **CE's flip-metering bridge (2026-10-27, hardware run pending).** `sl.dlss_g.dll` (Streamline in GTA) carries
  the interface IDs of `NvAPI_D3D12_SetFlipConfig` (`0xf3148c42`) and `NvAPI_NGX_GetDriverFeatureSupport`
  (`0x6194b19d`, R595+, asks whether `NV_NGX_DRIVER_FEATURE_ID_SET_FLIP_CONFIG_V2` is understood); `sl.common.dll`
  carries `SetAsyncFrameMarker`/`NotifyOutOfBandCommandQueue`. Hook side, `hook/ngx/flip_metering_bridge.{h,cpp}`
  + `flip_metering_bridge_policy.h`: `ReflexDetour_QueryInterface` answers both IDs only when the resident NVAPI
  returns null and the layer reports a metering device (`DecideQueryInterface`). Routing DLSS consumers' query
  through CE no longer needs a DRS override: `IsTranslatedNvApiResident` (nvapi64 outside System32/SysWOW64) arms
  it too (`IsNvApiConsumerRoutingWanted` in `main_overlay_detect.cpp`, iat_hook_init's dynamic-hook gate).
- **Batch identity is the frame id, never a present count.** vkd3d (`swapchain.c` `dxgi_vk_swap_chain_Present`)
  copies the device's open `OUT_OF_BAND_PRESENT_START` frame id into each present request and replays it with
  `vkSetLatencyMarkerNV(OOB_PRESENT_START, presentID)` immediately before that request's `vkQueuePresentKHR`;
  every present of one DLSS-G batch carries the same id. dxvk-nvapi remaps application frame ids
  (`LowLatencyFrameIdGenerator`), so the hook keys on what vkd3d receives: a vtable hook on vkd3d's
  `ID3DLowLatencyDevice::SetLatencyMarker` (IID `f3112584-41f9-348d-a59b-00b7e1d285d6`, slot 6, VkLatencyMarkerNV
  values 10/11). `BatchBinder`: a SetFlipConfig binds to the open START's frame, otherwise to the next START.
  Layer side, `hook/vulkan_layer/vulkan_flip_metering_bridge.{h,cpp}` + `_policy.h`: on an instance whose
  `pEngineName` is `vkd3d`, vkCreateDevice adds `VK_NV_present_metering` + `presentMetering` (withdrawn on a
  refused create); `Capture_vkSetLatencyMarkerNV` records the frame id per swapchain; the present path chains
  `VkSetPresentConfigNV{numFramesPerBatch}` on the first present of a new frame id with a bound size
  (`BatchSchedule`, keyed, so a dropped batch cannot shift later ones). A metered batch's presents keep vkd3d's
  per-present mode (`PresentModeSelectionInput::meteredBatchPresent`) and `CEVulkanLayerDeviceEnabledPresentMetering`
  reports true while metering is engaged, moving `vsync_mode=fifo` to the final DXGI flip as for Portal RTX.
- **Field checks for the next run:** hook log `NVAPI flip metering: NvAPI_D3D12_SetFlipConfig requested by ...
  answered`, `... observing ID3DLowLatencyDevice::SetLatencyMarker`, `SetFlipConfig numFramesPerBatch=2`;
  `vulkan_layer.log` `D3D12 translation device - enabled ...`, `D3D12 flip metering drives sc=...` (skipped/evicted
  near 0) and no nonzero `presentConfigFeedback`; `sensors.log` `nvFlipSchedule applied>0` and the display-change
  jaggedness back near the present series. `declined` in the hook log means sl.dlss_g asked before the device existed;
  SetFlipConfig answers NVAPI_NOT_SUPPORTED (generator keeps its CPU pacing) whenever the marker hook is not in place.
- **DX12 PostSL route in Vulkan-owned processes (fixed 2026-10-27):** with FG on, the DX12 Streamline transition
  armed PostSL startup activation although the Vulkan layer presented, so
  `StreamlineHook::FlushSuppressedSetOptionsOffIfNeeded` retried it on vkd3d's present thread at every present for
  28 s (2448x `Startup window expired with activation pending ...`, `DX12: No startup activation swapchain available
  for PostSL recovery`) while the startup window held the title's SetOptions(OFF). `ResolveStreamlineFGTransitionRoute`
  (`dx12_overlay_policy/streamline_ownership.h`) now routes a Vulkan-owned process like observer-only: PostSL off,
  startup state and window cleared. The direct-callback line is ChangeGated (5 s heartbeat) for every title.
- NGX logs `NvAPI_D3D12_GetCudaIndependentDescriptorObject failed - nvapi status -5` (dxvk-nvapi
  `InvalidArgument`) twice every ~8 s with FG on; DLSS-G keeps running.

## Diagnostics
- `ThirdParty preload: <Tool> loaded from <path> (module=...)` - success.
- `ThirdParty preload: <Tool> already loaded; keeping the existing copy` -
  duplicate suppressed.
- `ThirdParty preload: <Tool> not found at <path> - skipping` - configured
  path or per-bitness file absent.
- `ThirdParty preload: <Tool> FAILED to load from <path> (error=...)` -
  load failure; `error=193` means the DLL architecture does not match the
  game process.

## Known failure modes
- ReShade 6.8 (and other factory-proxying tools) hook `CreateDXGIFactory1` and
  return a proxy factory object. CE's temp-swapchain installer used to pass
  that proxy straight into the raw saved `IDXGIFactory2::CreateSwapChainForHwnd`
  slot function, which interprets its first argument as a genuine
  `CDXGIFactory` and reads the adapter table at `+0xE8`. The proxy's layout is
  unrelated, so dxgi crashed in `EnumAdapterByLuid` (sessions
  `20260813_004853` / `20260813_004923`, Strange Brigade DX12 + ReShade,
  AV inside `dxgi!FindIndex<SAdapterDesc,...>`). Fixed by bypassing the foreign
  entry patch on `CreateDXGIFactory1` for the temp factory and by guarding the
  raw slot call with the saved-slot vtable match
  (`hook/d3d12/dx12_factory_slot_policy.h`). A factory object whose vtable is
  not the vtable the saved slot was captured from is refused; the
  real-swapchain retry paths then install the Present hooks.
- On game close with a ReShade swapchain proxy, CE's swapchain wrapper
  destructor over-released the "final wrapper reference": `Release()` mirrors
  one real `Release()` per external wrapper reference, so the final external
  release already consumed the base reference before the destructor ran. The
  destructor then released the four promoted interface refs (destroying the
  proxy, whose refcount was exactly those four) and released the base once
  more — a use-after-free that surfaced as DEP at `0x10000000000` inside
  `reshade!Release` (`_orig`'s vtable from the freed genuine swapchain).
  Session `20260813_012613`. Fixed with
  `ShouldReleaseRealSwapchainWrapperReferenceDuringWrapperDestructor`: the
  base release is skipped on the releasing path (the Streamline non-retaining
  wrapper keeps returning its borrowed reference).
- Loading ReShade + OptiScaler + Special K in the original Special-K-first
  order deadlocked startup (session `20260813_020236`, manual dump: hook thread
  in `LdrpLoadDllInternal` -> OptiScaler DllMain -> Special K thread-creation
  hook -> critical-section wait; Special K's init thread in
  `FreeLibraryAndExitThread` -> loader work-queue drain). The game process
  exited with code 1 about 30 seconds later without CE's crash path, so CE's
  VEH handler never saw the hang. The fix is the loader-quiescence wait
  described above (Special-K-first order retained); a Special-K-last order
  deadlocks the mirror way (next bullet).
- Special-K-last (the first fix attempt) still deadlocked for Special K +
  OptiScaler (session `20260813_021731`): Special K's DllMain called
  LoadLibrary, re-entered OptiScaler's mutex-guarded loader hook, and that
  mutex was held by OptiScaler's nvapi-init thread while it waited for the
  loader lock CE's hook thread held. The final fix keeps Special K FIRST and
  adds `WaitForLoaderQuiescence` (a LoadLibrary probe thread joined before
  every tool load after the first) so the tools' background loader work
  cannot overlap the next tool's DllMain.
- Special-K-first WITH the quiescence wait still deadlocked for all three
  tools (session `20260813_025615`): Special K's enumerator thread starts new
  `FreeLibraryAndExitThread` loader cycles at any time, so the wait only
  excluded the cycle in flight at that instant. CE's hook thread then held
  the loader lock in OptiScaler's DllMain, whose thread creation waited on
  Special K's critical section while the enumerator held it inside the loader
  drain. The final order therefore loads Special K LAST: ReShade and
  OptiScaler initialize before Special K's thread hook exists, and the
  quiescence wait before Special K drains OptiScaler's startup loader work.
- With all three tools loaded, CE's DX11 temp-device probe crashed inside
  `d3d11!CLayeredObject<CDevice>::CContainedObject::Release` with a garbage
  `this` (a UTF-16 string fragment) while
  `DetectSwapChainAPITypeForDX11Hook` released the device it got from
  `IDXGISwapChain::GetDevice` (session `20260813_024327`). CE's "saved
  original" `D3D11CreateDeviceAndSwapChain` was the entry address, which the
  foreign tools had patched, so the temp device/swapchain were proxy objects;
  releasing through the mixed ReShade/OptiScaler/Steam wrapper chain forwarded
  a corrupted pointer. Fixed by bypassing the entry patch (CE's own or a
  foreign prepend) with `InlineHook::CreateBypassTrampoline` before creating
  the temp D3D11 (and D3D10) device, so the probe operates on genuine d3d11
  objects — the same genuine-object rule as the temp-DXGI-factory fix.
- Talos (DX12) + ReShade-only crashed twice in the queue hook chain, not in
  the loader (sessions `20260813_041416` / `20260813_050515`): CE's
  "first captured original" globals for `ExecuteCommandLists`/`Signal` were
  captured from ReShade's proxy queue vtable, so re-entering them with the
  wrapped real queue threw `std::system_error(EDEADLK)` in ECL and jumped a
  garbage vtable slot in Signal. Fixed with type-safe per-vtable original
  resolution and eager native-original publication; see
  `dx12-overlay-third-party-coexistence.md` ("Third-party proxy queue
  re-entry in the ECL/Signal trace hooks") and
  `tests/test_dx12_ecl_recursion_break_policy.cpp`.
- Talos (Streamline game) + SpecialK-involved combinations crash inside the
  NVIDIA Streamline stack, with **no CE frames in any failing stack** (sessions
  `20260813_051600` / `20260813_055907`, build 0.1.5995). ReShade-only and
  ReShade+OptiScaler ran clean. `SpecialK + ReShade + OptiScaler` failed
  deterministically ~5s after injection with `STATUS_HEAP_CORRUPTION` raised
  in `RtlFreeHeap` from `sl.interposer.dll` during `slInit` called by
  OptiScaler; the freed pointer lives inside `SpecialK64.dll`'s `.data`
  (UTF-16 `XYZ:\123\456\!#$%^@?|` at SK+0xC86FA0), identical with the game's
  own interposer 2.11.1 and the redirected 2.12.0 (WER bucket
  `HEAP_CORRUPTION_ACTIONABLE_BlockNotBusy_DOUBLE_FREE_sl.interposer.dll`).
  **Root cause is a SpecialK bug, not CE:** `SHGetKnownFolderPath_Detour`
  (`src/diagnostics/debug_utils.cpp` ~line 5185, upstream `11f5ccb`)
  returns its `static wchar_t fake_path[MAX_PATH]` as the out-param for
  `sl.interposer` callers querying `FOLDERID_ProgramData` (when SK's VEH saw
  a prior exception on the thread); the interposer frees the static buffer
  via `CoTaskMemFree` and corrupts the heap. Fix is a one-line
  `CoTaskMemAlloc`+copy in SpecialK. `SpecialK`-only additionally failed
  once, ~22s in, with an access violation writing 0x8 in
  `RtlEnterCriticalSection(NULL)` — game code invoked from an sl.interposer
  worker thread during SL plugin loading, followed by a 60-second
  render-thread freeze (re-test after the SK fix). See `log/recent.md`.

- **Talos + OptiScaler as game-dir `dxgi.dll` overflowed the stack in CE's and OptiScaler's
  `slGetPluginFunction` hooks** (session `20261023_173151`, build 0.1.7155, OptiScaler
  `10.0.0-dev (1db5042)`; works without CE). `0xC00000FD` on `GameThread` during the
  interposer's plugin loading: 65 530 frames of OptiScaler's `slotGetPluginFunction<Common,0>`
  (`dxgi!...+0xa1832`, a `call rdi` through `pluginSlots[Common][0].original`) under
  `capture_hook_x64!Hooked_slGetPluginFunction`, whose forward is a tail jump (no CE frame).
  Bytes from the dump: OptiScaler detoured `sl.common!slGetPluginFunction` first (Detours `E9`
  to its relay, `CC` fill; trampoline #1 = `40 53 48 83 EC 20` + `jmp +6`); CE's preload pass
  logged `IsAlreadyHooked: Detected JMP rel32` and prepended, rewriting OptiScaler's displacement
  (CE's trampoline = `jmp` to OptiScaler's relay). OptiScaler keeps a plugin hook slot alive
  only while the first `kPatchBytes = 8` bytes still equal what it wrote; on its next
  `hookPlugin(Common)` pass it took CE's rewrite for a freshly loaded image, released slot 0
  without detaching and attached it again on top of CE (trampoline #2 = relocated copy of CE's
  `E9`). Slot 0's original then led to CE, CE's original to OptiScaler's relay, and that relay
  to the same slot-0 detour. **Fix (2026-10-23):** `InlineHook::ForeignEntryPolicy::kLeaveToOwner`
  leaves a chainable entry jump whose bytes differ from the image on disk untouched
  (`ClassifyEntryJumpOwner`, `inline_hook_policy.h`; unreadable disk bytes count as foreign, a
  module's own pristine `E9`/`FF 25` is still chained). The six Streamline core exports CE also
  reaches through GetProcAddress-time and import hooks use it unless the interposer takeover
  owns the import slots (`ShouldLeaveForeignOwnedStreamlineEntry`); the slot records the export
  without a hook and keeps its single forward pointer (`ResolveStreamlineHookSlotRequest`), and
  `original` stays the export itself, so CE's caller-side hooks forward into the foreign chain.
  **Rejected:** a re-entry breaker in CE (OptiScaler's detour would run twice per lookup and save
  its own `slOnPluginLoad` hook as the original - a second loop) and a deep body hook at +6 (it
  still changes bytes 6-7, inside OptiScaler's fingerprint, and OptiScaler then re-detours over
  its own jump). **Consequences:** CE no longer sees `sl.common!slGetPluginFunction` when another
  tool owns it (the interposer's lookups skip CE's dynamic hooks as a Streamline caller; the CE
  hook only forwards), so `IsExternalOverlayPluginLookupGuardReady()` is false and the guarded
  Steam invoke from Streamline stacks (prepend fallback only) uses the bypass. Feature-function
  hooks, `vkCreateSwapchainKHR`, the 1.x recorders and every non-Streamline family still prepend.
  Log markers: `InlineHook: Left <addr> to the component that patched its entry (... owner=<dll>
  pristineKnown=1)`, `Streamline Hook: Left <export> at <addr> ...`, `Batch completed (...
  leftToForeignOwner=N ...)`. Tests: `tests/test_inline_hook_foreign_entry_policy.cpp` (the
  dump's bytes), `tests/test_streamline_foreign_entry_policy.cpp`. **Validated** in `20261023_183119`
  (0.1.7298): no crash, `slGetFeatureFunction`/`slSetTag`/`slSetTagForFrame`/`slEvaluateFeature`/`slGetPluginFunction`
  left to OptiScaler, the game's lookups intercepted through GetProcAddress.
- **OptiScaler `FGOutput = XeFG` is unsupported (2026-10-23):** with `FGInput = DLSSG` OptiScaler takes the game's
  DLSS-G calls and runs Intel XeFG (`libxess_fg.dll`), which presents every other frame from its own thread. CE reads
  the DLSS-G signal, draws through PostSL on the Streamline-originated presents only and ProcessFrame skips the XeFG
  presents, so the overlay is on half the frames (dim, slow). Native XeFG has no support either. Evidence in
  `log/recent.md` (2026-10-23).

## Open Questions / Stale-Risk
- Stale-risk: low-medium. The load pipeline mirrors the validated
  DLSS/Streamline preload, but live validation against real ReShade /
  OptiScaler / Special K builds (x64 and x86, combined and single-tool) still
  needs to be performed on a game/test-app target.
- FIFO factory capture (2026-10-26): the layer's `GetOrCreateD3D11Device` lookup of `CreateDXGIFactory1` used to pass
  through `Wrapped_CreateDXGIFactory1`, where `vulkan_dxgi_fifo::MaybeInstallFactoryHooks` captures the system
  factory's creation-method bodies. The CE-caller bypass removed that trigger, and GTA 0.1.7347 armed FIFO and then
  captured nothing. `RegisterDynamicFactoryHooks` now also captures from a factory CE creates itself from System32's
  own entry (once per loaded image). Still open, pre-existing: arming follows `s_vulkanActive` on a hook-thread pass
  after swapchain ownership, so an ICD swapchain created inside the first `vkCreateSwapchainKHR` can precede it; GTA
  0.1.7347's was created lazily at the first present, after arming.
- If field testing finds games that create D3D devices before WMI injection
  lands, the documented follow-up is an opt-in "inject at start event without
  settle logic" fast path in `captureengine/injection/injection_manager.cpp`; the
  hook-side loader needs no rework for it.
