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
- The hook loads the configured tools in the fixed order Special K -> ReShade
  -> OptiScaler (the `Tool` enum declaration order in
  `third_party_load_policy.h`). Before every tool load after the first, the
  executor (a) waits for the Windows loader work queue to drain by joining a
  trivial `LoadLibrary` probe thread, and (b) suspends the threads owned by
  previously loaded tools for the duration of the load (threads identified by
  their start address inside a loaded tool module; two enumeration passes, no
  globally stable snapshot requirement, plus a bounded post-suspension probe
  with resume-and-retry when a tool thread was caught inside the loader).
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

## Open Questions / Stale-Risk
- Stale-risk: low-medium. The load pipeline mirrors the validated
  DLSS/Streamline preload, but live validation against real ReShade /
  OptiScaler / Special K builds (x64 and x86, combined and single-tool) still
  needs to be performed on a game/test-app target.
- If field testing finds games that create D3D devices before WMI injection
  lands, the documented follow-up is an opt-in "inject at start event without
  settle logic" fast path in `captureengine/injection_manager.cpp`; the
  hook-side loader needs no rework for it.
