# Graphics API reporting

Last verified: 2026-10-18 (swapchain API classifier, D3D11 evidence callers, D3D12 translator labels)

## Summary

The inject overlay reports the API evidenced by the active application device or context, rather than the newest
runtime interface available on the machine or an API DLL that happens to be loaded. The common formatting and
device-scope registry live in `hook/runtime/graphics_api_identity.h`; accepted visible-label changes flow through
`OverlayAdapter::SetGraphicsAPI` and dirty layout state only when the label actually changes.

## Labels and evidence

- DirectDraw reports `DirectDraw`, `DirectDraw2`, `DirectDraw3`, `DirectDraw4`, or `DirectDraw7` from the interface
  that created the active presentation surface. Successful application `IDirect3D3::CreateDevice` and
  `IDirect3D7::CreateDevice` evidence overrides that transport label with `DX6` or `DX7`.
- D3D8 reports `DX8` from its active device.
- D3D9 records classic versus Ex identity at successful `CreateDevice` / `CreateDeviceEx`. Registered capture and
  overlay helper devices are excluded. A one-time interface query is only a fallback for a device first discovered
  after creation. DXVK suffixes are preserved.
- D3D10 records `DX10` versus `DX10.1` at the matching creation export, including the combined device/swapchain
  entrypoints. A separately created device carries its identity into later factory swapchain presentation.
- Every D3D11 device starts at `DX11`. Successful application queries for Device1/2/3/4/5 or Context1/2/3/4
  monotonically promote only that device through `DX11.1` to `DX11.4`. "Application" is decided by the caller's
  module (`ApiEvidenceCaller`, return address through the loader-free module cache): CE itself, the module that
  implements the device (Windows' or DXVK's `d3d11.dll`, found through its original QueryInterface), modules under
  the Windows directory, known third-party overlays and unresolved thunks are ignored and logged
  (`[GraphicsAPI] D3D11.N use ignored ... caller=<module> (<class>)`). Until 2026-10-18 only scoped CE probes were
  excluded: the prerender fence path (`ID3D11Device5`, `ID3D11DeviceContext4`) made any D3D11 game with
  `cpu_prerender_limit` 1-6 "DX11.4" (`FlowD3D11Prerender.CesOwnInterfaceQueriesDoNotRaiseTheD3D11Version`), and
  any foreign caller could promote. The active swapchain chooses which device identity is visible.
- D3D10 versus D3D11 for a swapchain (overlay, capture, screenshot and prerender paths, not only the label) is
  `SelectPrimarySwapChainAPIType` via `DXGIShared::ClassifySwapChainAPI` (`hook/present/swapchain_api_classifier.h`,
  shared by `DetectAPIType` and `DetectSwapChainAPITypeForDX11Hook`). A device answering both `ID3D10Device` and
  `ID3D11Device` is D3D10 only with a D3D10 runtime module loaded (`d3d10.dll`, `d3d10_1.dll`, `d3d10core.dll`) and
  a feature level below 11_0. Native D3D10-on-D3D11 and DXVK's D3D10 meet both; a DXVK D3D11 device answers the
  D3D10 interfaces on every device (DXVK `D3D11DXGIDevice::QueryInterface`, swapchain `GetDevice` forwards there,
  checked upstream 2026-10-18) and meets neither. From 3a41978e (2026-05-02) to 2026-10-18 both answering always
  meant D3D10, so DXVK D3D11 games ran CE's D3D10 paths as "DX10 (DXVK)" whenever CE's DXGI hooks owned them. The
  Vulkan layer's label (`SelectTranslatedGraphicsAPIName`) uses the same runtime-module check without a feature
  level, so a DXVK D3D11 game that also loads a D3D10 runtime still shows "DX10 (DXVK)" there.
- D3D12 labels come from `Dx12OverlayApiLabel()` (`hook/d3d12/dx12_api_label.h`) on every D3D12 overlay path:
  `DX12 (VKD3D-Proton)` when `d3d12.dll` is vkd3d-proton (version resource, the Vulkan layer's check), `DX12 (VKD3D)`
  for Wine's vkd3d, else `DX12`. DXUP games come first: DXUP (Oblivionevil fork, local source
  `build/diagnostics/dxup-source`, 0.8.1) translates DirectDraw+D3D6/7, D3D8, D3D9 and D3D11 to one native D3D12
  swapchain that owns the overlay (`api-translation-overrides.md`); its legacy frontends render through DXUP's own
  `d3d11.dll`/`dxgi.dll` (so those load for every DXUP game, `d3d8`/`ddraw` link the D3D9 core statically), and it
  has no D3D10 frontend (`src/d3d10` is a placeholder). Its DLLs carry `ProductName "DXUP"` (since DXUP 58752bd,
  2026-10-04), cached per image in `hook/runtime/api_translator_modules.h`. `SelectDxupFrontend` prefers a frontend
  with CE evidence (`WasGameD3D9DeviceCreated`, `WasDirectDrawInterfaceObserved`), else the oldest loaded one;
  `DxupFrontendLabel` adds CE's detail (D3D6/7 over DirectDraw, D3D9Ex, `DX11Hook_HighestApplicationMinor`); both
  live in `DxupOverlayApiLabel` (`hook/runtime/dxup_api_label.h`). The D3D12 label and CE's D3D11 overlay on DXUP's
  `d3d11`/`dxgi` output both use it: that D3D11 swapchain also carries the D3D9/D3D8/DirectDraw games (x86 Mirror's
  Edge read `DX11.1 (DXUP)`, session 20261018_143747). Where CE's own D3D8/D3D9/DirectDraw overlay draws on a DXUP
  device, that path appends ` (DXUP)`. A DXUP DLL querying its own d3d11 for a newer interface (`d3d9.dll` asked for
  `ID3D11DeviceContext1`) is `kDeviceRuntime` evidence (`IsDxupModuleHandle`), not the game's. Until 2026-10-18
  every DXUP game showed `DX12`. Unverified on hardware; a game that loads a DXUP frontend DLL it does not
  render with, and shows no CE evidence for it, would be labeled by that DLL.
- OpenGL reparses `GL_VERSION` whenever the active `HGLRC` changes. OpenGL 3.2+ also uses
  `GL_CONTEXT_PROFILE_MASK`, producing labels such as `OpenGL 2.1`, `OpenGL 3.3 Compat`, and
  `OpenGL 4.6 Core`; parse failure reports `OpenGL`.

## Source anchors

- Formatting, parsing, monotonic upgrade, scoped registry: `hook/runtime/graphics_api_identity.h`
- Transition-only overlay update and diagnostics: `hook/overlay/overlay_adapter.cpp`
- DirectDraw 1/2/3/4/7 plus D3D6/7 evidence: `hook/ddraw/ddraw_hook.cpp`
- D3D9 classic/Ex and helper exclusion: `hook/d3d9/dx9_hook.cpp`
- D3D10/10.1 and D3D11 revision evidence: `hook/d3d11/dx11_hook.cpp`, `hook/wrappers/wrapper_hooks.cpp`,
  `hook/wrappers/d3d11_device_wrap.cpp`, `hook/wrappers/d3d11_devicecontext_wrap.cpp`
- OpenGL version/profile reporting: `hook/opengl/opengl_hook.cpp`

## Invariants and failure modes

- Feature levels are not API revisions and never change the D3D11.x revision; they only separate a D3D10 device
  from a DXVK D3D11 device that both answer the D3D10 and D3D11 interfaces.
- DirectDraw bootstrap devices, D3D9 capture helpers, D3D11 wrapper promotions, and capture synchronization probes
  cannot promote the application label.
- D3D11 revision evidence is monotonic per real device, but switching to another swapchain/device selects that
  device's independent revision.
- DirectDraw's CPU raster and auxiliary native D3D7 overlay paths are implementation details; neither may promote
  the application's label beyond the DirectDraw/legacy-D3D evidence observed on its actual surface and device.
- Repeated per-frame submissions of an unchanged label are no-ops and must not invalidate cached overlay layout.

## Tests

`tests/test_graphics_api_identity.cpp` covers all labels, precedence, DXVK suffixes, scoped-device isolation,
monotonic D3D11 promotion, OpenGL parsing/profiles/context isolation, and unchanged-label behavior.
`tests/test_graphics_api_reporting_source.cpp` protects creation-hook wiring and internal-evidence exclusions.
`tests/test_graphics_api_classification.cpp` covers the D3D10/D3D11 decision (native, DXVK), evidence callers and
D3D12 labels; flow tests `FlowD3D10Prerender` (classifier on a WARP D3D10.1 swapchain) and
`FlowD3D11Prerender.CesOwnInterfaceQueriesDoNotRaiseTheD3D11Version` run them on real devices.

Open question / stale-risk: DirectDraw capture still relies on Windows exposing a Surface7 upgrade for old surface
interfaces. The API identity remains exact even when that capture-backend upgrade is unavailable, but capture and
overlay rendering then log a rate-limited upgrade failure.
