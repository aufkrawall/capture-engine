# llm-wiki Log Archive

### 2026-08-13 - FIXED (order finalized): Special K now loads LAST; quiescence wait alone was not enough

- Session `20260813_025615` (all three tools, Special-K-first + quiescence wait): CE's hook thread held the loader
  lock in OptiScaler's DllMain; OptiScaler's thread creation waited on Special K's critical section; Special K's
  enumerator thread held that section while blocked in `FreeLibraryAndExitThread`'s loader drain. The wait cannot
  fix Special-K-first because the enumerator starts NEW loader cycles at any time, so the overlap is a race, not a
  one-shot init transient.
- Final order: ReShade -> OptiScaler -> Special K. OptiScaler's DllMain thread creation runs before Special K's
  thread hook exists, and the existing loader-quiescence wait before Special K drains OptiScaler's startup loader
  work (nvapi init, update check), which is startup-only. Order constant, executor array, tests, and template/README/
  wiki text updated accordingly.

### 2026-08-13 - FIXED: all-three-tools crash in the DX11 temp-device probe (0.1.5985 -> next)

- Session `20260813_024327` (ReShade + OptiScaler + Special K): AV in
  `d3d11!CLayeredObject<CDevice>::CContainedObject::Release` with a garbage `this` (UTF-16 string fragment),
  called from CE's `DetectSwapChainAPITypeForDX11Hook` while releasing the device returned by
  `IDXGISwapChain::GetDevice`. CE's temp D3D11 device/swapchain were third-party proxy objects because the
  "saved original" `D3D11CreateDeviceAndSwapChain` entry had been patched by the tools; releasing through the
  mixed ReShade/OptiScaler/Steam wrapper chain forwarded a corrupted pointer.
- Fix: `hook/apis/dx11_hook.cpp` now bypasses the entry patch on `D3D11CreateDeviceAndSwapChain` (and the D3D10
  temp route's `D3D10CreateDevice`) with `InlineHook::CreateBypassTrampoline` before creating the temp device, so
  the probe operates on genuine d3d11 objects — same rule as the temp-DXGI-factory fix. Source-order test added
  to `tests/test_inject_capture_source_part2.cpp`.

### 2026-08-13 - FIXED (supersedes the order-only fix): Special K + OptiScaler loader deadlocks in BOTH orders

- Session `20260813_021731` (SK + OptiScaler, Special-K-last order from the previous fix): CE's hook thread held the
  loader lock loading Special K; Special K's DllMain called LoadLibrary, which re-entered OptiScaler's mutex-guarded
  loader hook; that mutex was held by OptiScaler's nvapi-init thread while it blocked in `LdrpDrainWorkQueue` on the
  loader lock. Same deadlock shape as `20260813_020236`, mirrored.
- Conclusion: load order alone cannot fix this — both orders create a loader-lock/tool-mutex cycle. The fix keeps
  Special K FIRST and synchronizes the loads on the real Windows synchronization primitive: before every tool load
  after the first, CE joins a trivial `LoadLibrary` probe thread (`WaitForLoaderQuiescence` in
  `hook/main_thirdparty_load.cpp`). The probe blocks in the loader work-queue drain until every in-flight loader
  call finished, so the next tool's DllMain never overlaps the previous tool's init loader work. No fixed sleeps.
- Order constant back to Special K -> ReShade -> OptiScaler; `ShouldWaitForLoaderQuiescenceBeforeToolLoad` added to
  `hook/common/third_party_load_policy.h` with tests in `tests/test_third_party_load_policy.cpp` and a source-order
  pin in `tests/test_inject_capture_source_part2.cpp`. Template/README/wiki order and rationale updated.

### 2026-08-13 - FIXED: ReShade + OptiScaler + Special K startup deadlock (0.1.5983 -> next)

- Session `20260813_020236` (manual 21MB dump): with all three tools configured, the game never fully started.
  CE's hook thread was inside `LdrpLoadDllInternal` loading OptiScaler; OptiScaler's DllMain created a thread and
  hit Special K's CreateRemoteThread hook, which waited on a Special K critical section; Special K's init threads
  were in `FreeLibraryAndExitThread` draining the loader work queue (loader lock held by CE's hook thread), and the
  game's main thread waited on the same Special K critical section. Classic 3-way deadlock.
- Fix: Special K now loads LAST. ReShade and OptiScaler load before Special K's early thread hooks exist (their
  DllMains are then clean, as the working ReShade+OptiScaler combo proves), and Special K's own DllMain is already
  proven safe standalone. This also matches the projects' own supported combination (OptiScaler's `LoadSpecialK`
  option loads Special K after OptiScaler). Order constant updated in `hook/common/third_party_load_policy.h`,
  executor array in `hook/main_thirdparty_load.cpp`, tests in `tests/test_third_party_load_policy.cpp`, and the
  template/README/wiki order text.

### 2026-08-13 - FIXED: game-close UAF when ReShade proxies the swapchain (0.1.5982 -> next)

- Session `20260813_012516` (Strange Brigade DX12 + ReShade 6.8): gameplay/overlay fine, crash on close —
  DEP at `0x10000000000` from `reshade!Release` while `CWrapDXGISwapChain::~CWrapDXGISwapChain` ran.
- Root cause: CE's wrapper `Release()` mirrors one `m_pReal->Release()` per external wrapper ref, so the final
  external release already consumed the wrapper's base reference. The destructor then released the four promoted
  interface refs (the proxy's exact remaining refcount -> ReShade destroyed proxy and genuine swapchain) and
  released the base reference once more: use-after-free on the freed proxy, `_orig` dangling.
- Fix: `ShouldReleaseRealSwapchainWrapperReferenceDuringWrapperDestructor` in
  `hook/common/dx12_overlay_policy/streamline_ownership.h` — skip the base release on the releasing path
  (`wrapperReleasing=true`); the Streamline non-retaining wrapper keeps returning its borrowed reference.
  Guard added in `hook/wrappers/dxgi_swapchain_wrap_lifetime.cpp`. Tests:
  `tests/test_dxgi_shared_part6.cpp` (policy values) + source-order pin in
  `tests/test_inject_capture_source_part2.cpp`.

### 2026-08-13 - FIXED: ReShade factory proxy crashed CE's temp-swapchain install (0.1.5978 -> next)

- Sessions `20260813_004853` / `20260813_004923` (Strange Brigade DX12, ReShade 6.8 loaded via `[ThirdParty]`):
  AV in `dxgi!FindIndex<SAdapterDesc,...>` right after `Temp swapchain creation — passthrough`. ReShade hooks
  `CreateDXGIFactory1` and returns a proxy factory; CE passed that proxy as `this` into the raw saved
  `IDXGIFactory2::CreateSwapChainForHwnd` slot function, so dxgi read the adapter table from the proxy's
  unrelated `+0xE8` layout (garbage: freed heap / ASCII) and crashed.
- Root-cause fix in `hook/apis/dx12_hook_hook_install.cpp`: the temp factory creation bypasses the foreign
  entry patch on `CreateDXGIFactory1`, and the historical raw-slot call is guarded by the saved-slot vtable
  match (`hook/common/dx12_factory_slot_policy.h`, new global
  `dx12_hook_s_savedCreateSwapChainForHwndVtable` captured together with the slot value). Mismatched/proxied
  factories are refused with a one-shot log; the real-swapchain retry paths install the Present hooks instead.
- Tests: `tests/test_dx12_factory_slot_policy.cpp` (policy + source-order pinning). OptiScaler-only runs were
  unaffected because OptiScaler hooks the factory vtable slot instead of returning a proxy object.

### 2026-08-13 - FEATURE: injected hook loads user-supplied ReShade / OptiScaler / Special K DLLs

- New `[ThirdParty]` config section: `reshade_dll_path`, `optiscaler_dll_path`, `specialk_dll_path`. File values load
  verbatim; folder values get the per-bitness default name appended (`ReShade64/32.dll`, `SpecialK64/32.dll`,
