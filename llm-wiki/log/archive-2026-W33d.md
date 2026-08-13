# llm-wiki Log Archive

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
