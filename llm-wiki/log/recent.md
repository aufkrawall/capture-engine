# llm-wiki Log

### 2026-11-06 - DXUP standalone DXGI discovery: factory vtable and swapchain interception in Mirror's Edge

- **Session `20261106_131542`, Mirror's Edge x86 with DXUP (D3D9 -> D3D12) and Steam overlay:**
  When DXUP's `dxgi.dll` was present in the game directory along with `d3d9.dll`, the CE in-game overlay did not appear and logged 0 frames. It previously worked when using only DXUP's `d3d9.dll` without `dxgi.dll`.
- **Root cause:**
  When only DXUP's `d3d9.dll` sat in the game folder, Windows loaded system `dxgi.dll` (`C:\Windows\SysWOW64\dxgi.dll`). `DXGIShared::GetSystemDXGIModuleHandle()` resolved system DXGI by its full path under `GetSystemDirectoryA`, allowing `InstallGlobalVTableHooks()` to hook system DXGI's factory vtable, intercept `CreateSwapChainForHwnd`, install deep Present hooks below Steam, and mark the D3D12 device as created.
  However, when DXUP's standalone `dxgi.dll` was also present, Windows loaded it directly and never loaded Windows' system `dxgi.dll`. Because `GetSystemDXGIModuleHandle()` looked exclusively for the system directory image, it returned `nullptr`, logging `"DX12: System DXGI module not loaded, deferring native factory vtable hooks"` on every pass. Consequently, factory hooks were never installed, DXUP's D3D12 swapchain was never intercepted, `WasD3D12DeviceCreated()` stayed false, `ShouldSuppressSpeculativeDX12Bootstrap` deferred `DX12Hook::Init()`, and DX9/DX11 hook checks skipped.
- **Fix in `hook/present/dxgi_shared_hooks.cpp`:**
  `GetSystemDXGIModuleHandle()` now checks whether system DXGI is absent and `ce::api_translator::IsDxupModuleLoaded("dxgi.dll")` is true. If so, it resolves DXUP's loaded `dxgi.dll` (excluding third-party overlay proxy paths), allowing `InstallGlobalVTableHooks()`, `CreateTempSwapChainViaFactorySlot()`, and `IsAddressInsideSystemDXGI()` to recognize DXUP as the native DXGI implementation.
- **Steam overlay & third-party compatibility preserved:**
  `IsAddressInsideSystemDXGI` returning true for DXUP's entry points ensures that `ShouldReserveEntryForLateOverlays` remains active, reserving the public Present entry for Steam's `gameoverlayrenderer.dll` and installing CE's deep body hooks below Steam. Factory vtable slots 10 and 15 remain left to Steam overlay.
- **Verified:**
  Unit tests `Dx12FactorySlotPolicyTest.NativeModuleLookupResolvesDxupWhenSystemDxgiAbsent`, `Dx12FactorySlotPolicyTest.*`, `DXGISharedSourceTest.*`, `*Dxup*` all compile and pass.

### 2026-11-05 - Overlay review: viewport wrapping and retained hardware detail slots

- Hero/List/Meters ignored the viewport limit, clipping a 656 px Hero panel in a 640 px window at 200% scale. Sections now wrap at their original atlas sizes; bars and the graph remain. Detail fields also lost their slots when samples became unavailable: `HardwareHistory` now owns per-renderer presence, with reserved field widths and `--` placeholders, including retained clock rows.
- Regression evidence: real-atlas bounds, overlap and value checks at 100/150/200%, all layouts through dropout/recovery/reset, long unbroken names, blink stability, all four adapter corners and graph-only redraws. CPU previews inspected at 600 px / 200%; no GPU/game run. Current policy and limits in `overlay-rendering.md`.

### 2026-11-05 - Overlay layouts: hero, list, meters, strip; the old row renderer is gone

- **Why:** the user found the overlay crowded and slow to search. FPS sat on row 7 of 10, one bold size carried everything, seven hues signalled nothing, and values had no columns.
- **What:** `overlay_panel` builds a positioned scene per `[Overlay] layout`; hero is the default. The atlas gained real sizes, fixed digit cells, a degree glyph and working font detection, and `font`, `font_weight`, `font_size` now work. Current rules in `overlay-rendering.md`.
- **Verified:** unit tests plus BMPs from the real renderer, atlas and CPU rasterizer at 150% on dark and white backgrounds, in the recording, FG and stress states. Not run in a game or on any GPU backend.
- **Config:** `bg_alpha` now defaults to 0.72 and `frametime_color` to `fps_color`, but a config.ini written from the old template keeps 0.50 and 00FF00.
