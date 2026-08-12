# Third-Party DLL Loading (ReShade / OptiScaler / Special K)

Last cross-checked: 2026-08-13

Primary sources:
- `common/config.h` (`ThirdPartyConfig`, `AppConfig::thirdParty`)
- `common/config_load_third_party.cpp`
- `common/config_load.cpp` (loader call order)
- `hook/common/third_party_load_policy.h`
- `hook/main_thirdparty_load.cpp`
- `hook/main_hookthread.cpp` (call site)
- `hook/main_loadlibrary.cpp` (`LoadRuntimeDllViaOriginal`)
- `hook/main_overlay_detect.cpp` (shared proxy-name matcher)
- `hook/common/overlay_compat_detail/module_table.h`
- `tests/test_config_third_party.cpp`
- `tests/test_third_party_load_policy.cpp`
- `tests/test_inject_capture_source_part2.cpp`

## Scope
This page describes how CaptureEngine's injected hook loads user-supplied
ReShade, OptiScaler, and Special K DLLs so they work without being copied into
game folders. It is complementary to
`dx12-overlay-third-party-coexistence.md`, which covers hook-chain ownership
once those tools are loaded.

## Facts
- `[ThirdParty] reshade_dll_path`, `optiscaler_dll_path`, and
  `specialk_dll_path` configure the loads. Empty disables a tool. A value whose
  final path component contains a `.` is a file loaded verbatim; anything else
  is a folder and the per-bitness default name is appended: `ReShade64.dll` /
  `ReShade32.dll`, `SpecialK64.dll` / `SpecialK32.dll`, `OptiScaler.dll`
  (single name for both architectures). Per-profile overrides use
  `ThirdParty.<key>` inside `[Profile.*]`, handled by the existing
  `ConfigReader::GetStr` override fallback like `DLSS.<key>`.
- The new fields live in `ThirdPartyConfig` on `AppConfig`, deliberately NOT in
  `GraphicsConfig`/`SharedGraphicsConfig`: the hook reads them directly from
  `config.ini` (same file the host writes), so no shared-memory layout change,
  no `SHARED_MEMORY_VERSION` bump, and no IPC transport exists for them.
- The hook loads the configured tools in the fixed order Special K -> ReShade ->
  OptiScaler (the `Tool` enum declaration order in
  `third_party_load_policy.h`). Special K wants to be present before other
  hookers; OptiScaler is a ReShade-based runtime that layers its own hooks on
  top. Do not reorder without a documented reason.
- `PreloadConfiguredThirdPartyDlls()` runs in `HookThread` immediately after
  the local `config.ini` parse, before CE's wrapper DLL load,
  `PreloadConfiguredGraphicsRuntimeDlls()`, and per-API hook installation.
  `InstallGlobalVTableHooks()` still runs first; CE's below-the-foreign-chain
  Present coexistence logic handles the resulting layering.
- Each tool is loaded through `LoadRuntimeDllViaOriginal` (the original
  `LoadLibraryW`, bypassing CE's redirect hook) and then
  `NotifyHookModuleLoaded`, so the overlay-compatibility tracked-module
  registry observes it and the Present routing sees it as a foreign overlay.
- Duplicate-load protection: a tool is skipped when a canonical base name is
  already mapped (`SpecialK64/32.dll`, `SpecialK.dll`, `ReShade64/32.dll`,
  `ReShade.dll`, `OptiScaler.dll`, `OptiScaler.asi`), or when a renamed copy
  occupies a graphics proxy name (`dxgi.dll`, `d3d*.dll`, ...) and matches the
  tool's export/version markers (`ReShadeVersion`/`ReShadeRegisterAddon`,
  `SK_GetDLL`/`SK_Inject_GetRecord`, OptiScaler version resource). The proxy
  name list is shared with `main_overlay_detect.cpp` via
  `ce::overlay_compat::IsThirdPartyGraphicsProxyCandidateName`, so the
  preloader and identity detection cannot disagree about which tools are
  present.
- Every outcome is logged exactly once per tool with `HookLog` /
  `HookLogImportant`: loaded (module handle), already loaded (kept),
  not found at resolved path, invalid UTF-8 path, or failed load with
  `GetLastError()` (plus an `ERROR_BAD_EXE_FORMAT` wrong-architecture hint).
  A failure never aborts CE's own hook initialization.
- Loading happens once per injection (static atomic latch). Changing the paths
  requires relaunching the game, matching the existing DLSS/Streamline path
  override behavior.

## Tool-side prerequisites (not CE's concern)
- Each tool still reads its own per-game configuration next to the game
  executable: `ReShade.ini`, `OptiScaler.ini`, Special K's profile.
- Keep OptiScaler's `LoadReshade`/`LoadSpecialK` INI switches off when CE
  manages all three loads, otherwise a tool can be loaded twice.
- ReShade hooks Vulkan through its layer mechanism, not through
  `LoadLibrary`, so ReShade-in-Vulkan titles still need ReShade's Vulkan layer
  registered by the user. Special K hooks `vulkan-1.dll` exports itself.
- Prefer plain loader DLL names in the configured paths. A copy renamed to
  `dxgi.dll`/`d3d*.dll` acts as a proxy and cannot intercept a game that
  already resolved the system module by name.
- Late injection into an already-running game is best effort: the tools may
  miss early device-creation hooks, the same limitation CE's own overlay has
  for pre-existing swapchains.

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
  (`hook/common/dx12_factory_slot_policy.h`). A factory object whose vtable is
  not the vtable the saved slot was captured from is refused; the
  real-swapchain retry paths then install the Present hooks.

## Open Questions / Stale-Risk
- Stale-risk: low-medium. The load pipeline mirrors the validated
  DLSS/Streamline preload, but live validation against real ReShade /
  OptiScaler / Special K builds (x64 and x86, combined and single-tool) still
  needs to be performed on a game/test-app target.
- If field testing finds games that create D3D devices before WMI injection
  lands, the documented follow-up is an opt-in "inject at start event without
  settle logic" fast path in `captureengine/injection_manager.cpp`; the
  hook-side loader needs no rework for it.
