# llm-wiki Log

### 2026-11-06 - Late dxgi.dll module-load retry for factory vtable and swapchain hooks (Mirror's Edge on DXUP)

- **Session `20261106_131542` and `20261106_135827`, Mirror's Edge x86 with DXUP and Steam overlay:**
  When DXUP's `dxgi.dll` was present in the game directory alongside `d3d9.dll`, the CE in-game overlay did not appear and logged 0 frames.
- **Root cause:**
  DXUP's `d3d9.dll` explicitly calls `GetSystemDirectoryA` and `LoadLibraryA("C:\Windows\system32\dxgi.dll")` to create its DXGI factory and swapchain on the system runtime. When `dxgi.dll` is not in the game folder, Steam overlay loads system DXGI at startup, so `InstallGlobalVTableHooks()` finds it immediately on its initial HookThread pass. But when DXUP's `dxgi.dll` is also in the game folder, Steam loads that copy at startup, and system DXGI is only loaded later when `d3d9.dll` executes its first `Direct3DCreate9` call.
  Previously, `InstallGlobalVTableHooks()` deferred when system DXGI was not loaded yet at startup, but was never retried: `CheckAndInstallHooks()` did not invoke it, `NotifyHookModuleLoaded()` had no `dxgi.dll` handler, and `DX12Hook::Init()` was deferred by `ShouldSuppressSpeculativeDX12Bootstrap` until D3D12 device evidence was seen (which required the very swapchain creation hooks that were deferred).
- **Fix in `main_overlay_detect.cpp` and `main_install.cpp`:**
  1. `NotifyHookModuleLoaded()` triggers `InstallGlobalVTableHooks()` on completed `dxgi.dll` loads. The 2026-11-06 review added shared installer admission before its mutex: nested returns still inside DllMain defer to the hook thread (`RtlIsThreadWithinLoaderCallout`); ordinary completed loads remain synchronous. Vulkan ownership or a resident CE Vulkan layer also veto bootstrap before renderer publication. Tests inject these contexts and cover recovery; live DXUP/Steam retest is pending.
  2. `CheckAndInstallHooks()` retries `InstallGlobalVTableHooks()` when `!s_vulkanActive`, ensuring fast recovery if module notifications were missed.
  3. `GetSystemDXGIModuleHandle()` continues to resolve system DXGI strictly by full path under `GetSystemDirectoryA`, avoiding premature binding to unused proxy images in the game folder.
- **Verified:**
  Unit tests `Dx12FactorySlotPolicyTest.ColdDxgiLoadTriggersFactoryVTableHookInstall`, `Dx12FactorySlotPolicyTest.*`, `DXGISharedSourceTest.*` all compile and pass.

### 2026-11-05 - Overlay review: viewport wrapping and retained hardware detail slots

- Hero/List/Meters ignored the viewport limit, clipping a 656 px Hero panel in a 640 px window at 200% scale. Sections now wrap at their original atlas sizes; bars and the graph remain. Detail fields also lost their slots when samples became unavailable: `HardwareHistory` now owns per-renderer presence, with reserved field widths and `--` placeholders, including retained clock rows.
- Regression evidence: real-atlas bounds, overlap and value checks at 100/150/200%, all layouts through dropout/recovery/reset, long unbroken names, blink stability, all four adapter corners and graph-only redraws. CPU previews inspected at 600 px / 200%; no GPU/game run. Current policy and limits in `overlay-rendering.md`.

### 2026-11-05 - Overlay layouts: hero, list, meters, strip; the old row renderer is gone

- **Why:** the user found the overlay crowded and slow to search. FPS sat on row 7 of 10, one bold size carried everything, seven hues signalled nothing, and values had no columns.
- **What:** `overlay_panel` builds a positioned scene per `[Overlay] layout`; hero is the default. The atlas gained real sizes, fixed digit cells, a degree glyph and working font detection, and `font`, `font_weight`, `font_size` now work. Current rules in `overlay-rendering.md`.
- **Verified:** unit tests plus BMPs from the real renderer, atlas and CPU rasterizer at 150% on dark and white backgrounds, in the recording, FG and stress states. Not run in a game or on any GPU backend.
- **Config:** `bg_alpha` now defaults to 0.72 and `frametime_color` to `fps_color`, but a config.ini written from the old template keeps 0.50 and 00FF00.
