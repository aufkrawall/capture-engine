# llm-wiki Log — Archive 2026-W18b

### 2026-05-02 - Fix Steam DX11 32-bit game overlay missing — InitializeWrapperHooks never retries D3D11 IAT after DllMain partial init

- **Motivation**: BioShockInfinite.exe (32-bit DX11 Steam game) had no overlay visible. Logs at `installed/captureengine/logs/20260502_171120` on build `0.1.2727` showed `D3D11=0` in IAT init, no DX11 hook initialization, no overlay frames (`perf_metrics_*.csv` header-only).

- **Root cause analysis**:
  1. DllMain called `InitializeWrapperHooks()` when d3d9.dll was loaded but d3d11.dll was not yet loaded. DXGI + D3D12 + D3D9 IAT hooks installed successfully → `g_WrappersActive = true`.
  2. `InitializeWrapperHooks()` at `wrapper_hooks.cpp:643-644` had: `if (g_WrappersActive) return true;` — an early return that prevented any retry.
  3. HookThread's periodic `CheckAndInstallHooks()` → `InitializeWrapperHooks()` hit the early return → never retried `IATHook::InitializeD3D11Hooks()`.
  4. When d3d11.dll loaded later (during game startup), the IAT for `D3D11CreateDevice/AndSwapChain` was never patched → `Wrapped_D3D11CreateDeviceAndSwapChain` never called → `WasD3D11Or10DeviceCreated()` stayed false.
  5. The DX11 hook condition at `main.cpp:1479-1481` (`!g_DX11Hook && d3d11Or10DllPresent && (d3d11Or10DeviceCreated || (!d3d12DeviceCreated && !legacyD3DLoaded))`) never passed: `d3d11Or10DeviceCreated` was false AND `legacyD3DLoaded` was true (d3d9.dll loaded), making the fallback clause false.
  6. The per-category `!s_D3D11Initialized` guard inside `InitializeWrapperHooks` was designed to allow retry but `g_WrappersActive` early return defeated it.

- **Fix**: `wrapper_hooks.cpp:643-644`: Removed the `if (g_WrappersActive) return true;` early return. Now the function proceeds past the guard to retry any uninitialized categories (`!s_*Initialized`). When D3D11.dll is loaded later, `InitializeD3D11Hooks()` will find the module, patch IAT entries, set `s_D3D11Initialized=true`, and subsequent calls will skip it.

- **Verification**: Build `0.1.2728`: `success=1`, 668 tests passed (all existing tests pass, no regressions).

- **Files changed**: `hook/wrappers/wrapper_hooks.cpp`, `llm-wiki/log/recent.md`

- **Stale risk**: Medium. Fresh BioShock Infinite validation is needed. The fix affects any 32-bit DX11 game that loads D3D11.dll after the hook DLL's DllMain (common with Steam overlay initialization sequence).

### 2026-05-02 - Fix DX10 overlay not visible: D3D10-on-D3D11 detection short-circuits at D3D11

- **Motivation**: After the crash fix (build `0.1.2725`), the DX10 test app stopped crashing but the overlay was still invisible (both x64 and x86). Logs at `installed/captureengine/logs/20260501_215216` showed `DetourPresent: IsInWrapperPresent early return`, overlay rendering on every frame (`DX11: [frame 1194] calling RenderOverlay`), but `overlay_us = 0` in `perf_metrics_*.csv` and nothing visible on screen.

- **Root cause analysis**:
  1. `DetectSwapChainAPITypeForDX11Hook()` (dx11_hook.cpp) and `DetectDXGISwapChainAPIType()` (dxgi_shared.cpp) both short-circuited the D3D10 QI when D3D11 succeeded (`if (!hasD3D12Device && !hasD3D11Device) { try D3D10; }`).
  2. On Windows 10+, the D3D10 runtime is implemented on a D3D11 translation layer ("D3D10-on-D3D11"). A `GetDevice(IID_ID3D11Device)` call on a D3D10-on-D3D11 swapchain SUCCEEDS, so the D3D10 QI was never attempted.
  3. `SelectPrimarySwapChainAPIType(false, true, false)` → `D3D11` — the swapchain was classified as D3D11 even though it was created from a D3D10 device.
  4. When the swapchain is classified as D3D11, `DrawOverlay` runs the D3D11 overlay rendering path on what is functionally a D3D10 device. The D3D11 overlay backend may initialize successfully but produces no visible output on D3D10-on-D3D11 hardware feature levels. The rendering commands execute, textures are created, but nothing appears on screen.
  5. The wrapper architecture correctly calls `DX11_ProcessFrameExternal` → `HandleDX11ProcessFrame` → `DrawOverlay` on every frame (confirmed by log), but the D3D11 overlay renderer draws pixels that never make it to the display on a D3D10 device.
  6. `DrawDX10Overlay()` exists specifically for this case but was never reached because the detection returned `D3D11`.

- **Fix** (3 files):
  1. `DetectSwapChainAPITypeForDX11Hook()` in `hook/apis/dx11_hook.cpp`: removed the `if (!hasD3D11Device)` guard on the D3D10 QI. Always try all three QI's independently.
  2. `DetectDXGISwapChainAPIType()` in `hook/common/dxgi_shared.cpp`: same fix — always try all three QI's without short-circuiting.
  3. `SelectPrimarySwapChainAPIType()` in `hook/common/dxgi_shared.h`: reversed the priority — prefer `D3D10` over `D3D11` when both succeed. When both QI's succeed the device is D3D10-on-D3D11 (functionally D3D10). A native D3D11 device only QI's for D3D11. A native D3D10 device only QI's for D3D10 (no D3D11 translation on pre-Win8 systems).

- **Test changes**:
  - Updated `SelectPrimarySwapChainAPITypePrefersHighestDeviceVersion` to expect `D3D10` (was `D3D11`) for `(false, true, true)`.
  - Added `D3D10OnD3D11SwapchainReturnsD3D10` regression test covering all three device combinations.
  - Added `SelectPrimarySwapChainAPIType(false, true, false)` → D3D11 (native D3D11) assertion.

- **Verification**: Build `0.1.2726`: `success=1`, 668 tests passed (was 667 before, 1 new regression test added).

- **Files changed**: `hook/apis/dx11_hook.cpp`, `hook/common/dxgi_shared.cpp`, `hook/common/dxgi_shared.h`, `tests/test_dxgi_shared.cpp`, `llm-wiki/log/recent.md`

- **Stale risk**: Medium. The D3D10 overlay needs fresh end-to-end validation with the fixed detection to confirm `DrawDX10Overlay` is called and the overlay appears on screen. The fix is generic and applies to any D3D10 game on Windows 10+.

### 2026-05-01 - Fix DX10 test app stack-overflow crash from HookExport overwriting shared IAT original with wrapper address

- **Motivation**: `installed/captureengine/logs/20260501_204705` on build `0.1.2723` showed the DX10 test app (both x64 and x86) crashing with `0xC00000FD` (stack overflow) immediately after DX11 hook initialization. The `crash.log` confirmed a VEH-handled stack overflow in `ntdll.dll`.

- **Root cause analysis**:
  1. `wrapper_hooks.cpp` defines `PFN_D3D11CreateDeviceAndSwapChain oD3D11CreateDeviceAndSwapChain = GetProcAddress(d3d11.dll, ...)` (shared extern defined in `dx11_hook.h:38`).
  2. `dx11_hook.cpp` then calls `CustomHook::HookExport("d3d11.dll", "D3D11CreateDeviceAndSwapChain", DetourD3D11CreateDeviceAndSwapChain, (void**)&oD3D11CreateDeviceAndSwapChain)`.
  3. `HookExport` calls `PatchIATAllModules` which scans ALL loaded modules. A module that was already patched by DllMain's IAT hook (with `Wrapped_D3D11CreateDeviceAndSwapChain`) has `currentFunction == Wrapped_D3D11CreateDeviceAndSwapChain`. Since `currentFunction != hookFunction` (``DetourD3D11CreateDeviceAndSwapChain`), the code falls through to line 313 which saves `*outOriginal = Wrapped_D3D11CreateDeviceAndSwapChain`.
  4. Since the modules are scanned in order, the FIRST successfully-patched module may already have the prior hook, so `firstOriginal` becomes `Wrapped_D3D11CreateDeviceAndSwapChain` instead of the real d3d11.dll function address.
  5. Now `oD3D11CreateDeviceAndSwapChain` points to `Wrapped_D3D11CreateDeviceAndSwapChain`. When `DetourD3D11CreateDeviceAndSwapChain` calls `oD3D11CreateDeviceAndSwapChain`, it enters the wrapper, which calls `oD3D11CreateDeviceAndSwapChain` (still pointing to the wrapper) -> infinite recursion -> stack overflow.
  6. The crash log showed `Wrapped_D3D11CreateDeviceAndSwapChain: Calling original at 00007FFE598CDF10` which is in the CE hook DLL range, confirming the wrong original pointer.

- **Fix**:
  1. `dx11_hook.cpp` now saves the real `GetProcAddress` result into a local `static s_oRealD3D11CreateDeviceAndSwapChain` BEFORE `HookExport` overwrites the shared `oD3D11CreateDeviceAndSwapChain`.
  2. `DetourD3D11CreateDeviceAndSwapChain` uses `s_oRealD3D11CreateDeviceAndSwapChain` (falling back to `oD3D11CreateDeviceAndSwapChain`) to always reach the real d3d11.dll code, never the wrapper.
  3. The temp D3D11 swapchain creation path (used for Present vtable hook installation) also uses `s_oRealD3D11CreateDeviceAndSwapChain` for the same reason.

- **Verification**:
  1. Full canonical regression passed: `python build.py --skip-updates --run-tests` ran all 669 unit tests successfully and completed the product build (`build_version=0.1.2725`, `success=1`).
  2. The full build produced both x64 and x86 hook DLLs plus all test apps and the Vulkan layer.

- **Files changed**: `hook/apis/dx11_hook.cpp`, `llm-wiki/log/recent.md`

- **Stale risk**: Medium-high until fresh DX10 test app validation confirms no more `0xC00000FD` from the D3D11 create path. The same IAT-vs-export-hook overlap pattern could theoretically affect `D3D11CreateDevice` (also hooked by both wrapper and DX11 hook), but only `D3D11CreateDeviceAndSwapChain` has a `HookExport` call, so the risk is isolated. Also applies to x86 builds of functionally identical 32-bit DX10/11 games.

### 2026-05-01 - Fix 64-bit DX12 overlay missing when injection wait allows pre-existing swapchain with deferred Present hooks

- **Motivation**: `installed/captureengine/logs/20260501_185121` on build `0.1.2720` showed the overlay working in the 32-bit `dx12_test.exe` (412 DX12 frames in `perf_metrics_18176.csv`) but completely missing from the 64-bit `dx12_test.exe` (0 frames in `perf_metrics_19612.csv`). The 64-bit process showed full DX12 hook initialization but no overlay rendering.

- **Root cause analysis**:
  1. The injector waited 1000ms for D3D12 initialization (`D3D12.dll detected, waiting for init...`). During this window the 64-bit test app created its D3D12 device, factory, and swapchain before any CE hooks were installed.
  2. After injection, `InstallGlobalVTableHooks()` successfully installed factory vtable + inline CreateSwapChainForHwnd hooks, but the game's swapchain was already created — these hooks never fire for a pre-existing swapchain.
  3. `ShouldDeferEarlyDX12TempSwapchainPresentHookInstall(false, true)` returned `true` because `nvspcap64.dll` (NVIDIA Capture SDK) was loaded and `WasD3D12DeviceCreated()` always returns `false` in non-wrapper builds (no `D3D12CreateDevice` wrapper hook).
  4. `FindAndWrapPreExistingSwapchains()` was a **no-op** — it only logged a misleading `Pre-existing swapchain support enabled via inline Present hooks` message without actually installing anything.
  5. Present hooks were permanently missing → overlay never renders.
  6. The 32-bit process did not have `nvspcap64.dll` loaded (64-bit only DLL), so `ShouldDeferEarlyDX12TempSwapchainPresentHookInstall` returned `false`, Present hooks were installed eagerly via temp swapchain, and the overlay worked.

- **Fix**:
  1. `FindAndWrapPreExistingSwapchains()` in `hook/apis/dx12_hook.cpp` now detects when Present hooks are missing (`HasPresentInlineHooks()` / `HasPresentDetourHooks()`) and retries installation via a postponed temp swapchain.
  2. The `g_CreatingTempSwapchain` guard prevents re-entrant side effects; calling `oCreateSwapChainForHwndGlobal` bypasses CE's own hooks. By this point the overlay's startup hook chain is settled, so the recursion risk that motivated the original deferral is minimal.
  3. If the postponed temp swapchain succeeds, log `DX12: Present hooks installed via postponed temp swapchain`; if it also fails, log `Postponed temp swapchain also failed — pre-existing swapchains will not have overlay...`.
  4. Fixed the misleading `DX12Hook: Initialized (factory + Present hooks installed)` log message — now accurately reports `(factory hooks installed; Present hooks deferred to FindAndWrapPreExistingSwapchains)` when hooks are missing.
  5. Added focused regression test `FindAndWrapPreExistingSwapchainsCanDetectMissingPresentHooks` in `tests/test_dxgi_shared.cpp`.

- **Verification**:
  1. Full canonical regression passed: `python build.py --skip-updates --run-tests` ran all 668 unit tests successfully and completed the product build (`build_version=0.1.2723`, `success=1`).
  2. The full build produced both x64 and x86 hook DLLs plus all test apps and the Vulkan layer.

- **Files changed**: `hook/apis/dx12_hook.cpp`, `tests/test_dxgi_shared.cpp`, `llm-wiki/log/recent.md`

- **Stale risk**: High until fresh 64-bit dx12_test.exe validation confirms the hook_debug.log shows `DX12: Present hooks installed via postponed temp swapchain` and the perf_metrics CSV has frame data. The fix is generic: any 64-bit DX12 game with fast startup that has nvspcap64.dll loaded (NVIDIA Capture SDK) could hit the same timing window.

### 2026-04-30 - Arm manual Reflex QueryInterface hook before NvAPI loads

- **Motivation**: Talos `installed/captureengine/logs/20260430_191957` on build `0.1.2692` still reported about 60 ms NVIDIA overlay latency with CE's manual Reflex limiter at 60 fps. The run proved the previous low-latency OFF -> ON re-arm executed, but it still never reached the game-owned NvAPI Sleep wrapper handoff.

- **Root cause analysis**:
  1. `fps_limiter_trace.log` showed a correct 60 fps cap through `Apply: REFLEX post-present cadence ...`, so this was not an FPS-cap failure.
  2. `hook_debug.log` showed `ReflexLimiter: Re-armed manual FPS limit...` and a successful capped `Pushed FPS limit...`, but no `ReflexLimiter: Returning Sleep wrapper...` or `Game called Reflex Sleep via NvAPI`.
  3. The filtered `nvapi_QueryInterface` hook was registered only when `g_ReflexLimiter.Init()` ran after NvAPI loaded at `19:20:19.288`, more than two seconds after the hook thread started. That left a startup window where Talos could cache original NvAPI Reflex pointers before CE armed the manual low-latency handoff.

- **Fix**:
  1. `hook/main.cpp` now arms the filtered QueryInterface/GetProcAddress hook immediately after local `config.ini` load when manual Reflex mode is configured, and again after shared-memory sync if shared memory is the first source that proves manual Reflex is wanted.
  2. The existing caller filter remains the safety boundary: game callers can receive CE's SetSleepMode/Sleep wrappers only while manual Reflex is configured or active, while Streamline/FFX runtimes, third-party overlays, system modules, and CE modules still receive original driver pointers.
  3. Manual Reflex config detection is now centralized in `ce::fps_limiter_policy::IsManualReflexLimiterConfigured(...)` and covered by a focused regression test, so the early-arm condition matches the runtime/shared-memory condition used by the Reflex limiter itself.
  4. Diagnostics now include `ReflexLimiter: Early filtered nvapi_QueryInterface hook armed from config.ini...` (or shared memory), making fresh Talos traces prove whether the QueryInterface handoff was armed before NvAPI readiness.

- **Verification**:
  1. Focused limiter coverage passed: `python build.py --run-tests --tests-only --skip-updates --gtest-filter=ReflexFpsLimiterPolicyTest.*:FpsLimiterTest.ManualReflexFirstPushRearmsLowLatencyModeBeforeLimit:FpsLimiterTest.NonManualReflexPushDoesNotForceLowLatencyReset`.
  2. Full canonical regression passed: `python build.py --skip-updates --run-tests` ran all 666 unit tests and completed the product build (`build_version=0.1.2694`, `success=1`).
  3. Direct touched-file formatter check passed for the edited C++/test files, and the required direct compile `python build.py --skip-updates` completed successfully afterward (`build_version=0.1.2695`).

- **Files changed**: `hook/main.cpp`, `hook/common/fps_limiter_policy.h`, `hook/common/reflex_limiter_query_hook.inl`, `tests/test_fps_limiter.cpp`, `llm-wiki/current.md`, `llm-wiki/regression-testing-and-logging.md`, `llm-wiki/log/recent.md`

- **Stale risk**: High until fresh Talos validation confirms the new early-arm log appears before NvAPI readiness and the run transitions to `Returning Sleep wrapper...` / `Game called Reflex Sleep via NvAPI` with latency near the game's own limiter. GTA validation should still confirm Reflex / DLSS FG switching because the manual limiter remains disabled there and sensitive runtime callers still receive original driver pointers.

### 2026-04-30 - Re-arm manual Reflex sleep mode on first capped push

- **Motivation**: Talos `installed/captureengine/logs/20260430_181933_talos` on build `0.1.2690` still started around the same high-latency 60 ms behavior, but the user's manual sequence of minimizing the game, switching the limiter to `basic`, then switching back to `reflex` dropped reported latency to about 45 ms.

- **Root cause analysis**:
  1. The trace showed no game-owned NvAPI Sleep wrapper calls, so the filtered QueryInterface handoff was not the active low-latency path in this run.
  2. The meaningful runtime difference was the config churn: switching to basic called `ClearFpsLimit()` and sent low-latency OFF / interval 0, then switching back to Reflex sent low-latency ON / interval 16666.
  3. The initial manual Reflex activation only pushed the ON state. If the driver/game Reflex state was already low-latency active or stale, it did not get the same OFF -> ON transition that the user's manual toggle created.

- **Fix**:
  1. Manual explicit Reflex now marks the next push for re-arm whenever the manual target activates, target interval changes, or the native pacing device changes.
  2. The next `PushFpsLimit()` in manual mode first sends a forced low-latency OFF / interval 0 reset, then immediately sends the configured low-latency ON / capped interval. This is scoped to CE's manual limiter; non-manual/game-owned Reflex pushes keep the single driver call so GTA's no-manual-limiter Reflex/DLSS FG comparison path stays untouched.
  3. Diagnostics now log `ReflexLimiter: Re-armed manual FPS limit with low-latency reset before push ...`, so fresh Talos traces should prove the automatic path is doing the same state transition the manual basic -> reflex toggle did.

- **Verification**:
  1. Focused limiter coverage passed: `python build.py --run-tests --tests-only --skip-updates --gtest-filter=FpsLimiterTest.ManualReflexFirstPushRearmsLowLatencyModeBeforeLimit:FpsLimiterTest.NonManualReflexPushDoesNotForceLowLatencyReset:ReflexFpsLimiterPolicyTest.*`.
  2. Full canonical regression passed: `python build.py --skip-updates --run-tests` ran all 665 unit tests and completed the product build (`build_version=0.1.2692`, `success=1`).
  3. Direct touched-file `clang-format --dry-run -Werror` passed for `hook/common/reflex_limiter.h` and `tests/test_fps_limiter.cpp`.

- **Files changed**: `hook/common/reflex_limiter.h`, `tests/test_fps_limiter.cpp`, `llm-wiki/current.md`, `llm-wiki/regression-testing-and-logging.md`, `llm-wiki/log/recent.md`

- **Stale risk**: High until fresh Talos validation confirms the automatic re-arm gets the low-latency result immediately at startup without the manual config toggle, and GTA validation confirms Reflex / DLSS FG switching remains stable with CE's manual limiter disabled.

### 2026-04-30 - Replace explicit Reflex queue clamp with filtered NvAPI game-sleep handoff

- **Motivation**: Talos `installed/captureengine/logs/20260430_021241` still reported about 60 ms NVIDIA overlay latency with CE's manual Reflex limiter at 60 fps, while the game's own limiter and RTSS-style Reflex limiting can reach roughly 40 ms or lower. The user rejected manually overriding swapchain latency as the wrong fix; a correct Reflex limiter should pace at the game's Reflex sleep point instead of clamping queues from the outside.

- **Root cause analysis**:
  1. CE could cap Talos at 60 fps with post-Present local cadence, but that still aged frames relative to the game's own Reflex limiter.
  2. Talos did not expose Streamline `slReflexSleep` traffic in this path, so CE never observed the stable game-sleep streak required for native handoff.
  3. The prior queue/prerender clamp could reduce latency symptoms but was not root-cause Reflex behavior. The missing seam was game-owned NvAPI `NvAPI_D3D_Sleep` obtained through `nvapi_QueryInterface`.
  4. GTA/DLSS FG remains sensitive to pointer substitution: Streamline/FFX runtime callers and overlays must still receive original driver pointers so Reflex and DLSS FG switching do not regress.

- **Fix**:
  1. Removed the explicit Reflex `SetMaximumFrameLatency(1)` / implicit DX11/DX12 prerender-limit path and the associated `WantsExplicitReflexFrameQueueClamp()` policy.
  2. Added a filtered `nvapi_QueryInterface` IAT/GetProcAddress hook for manual explicit Reflex mode. It registers only when manual Reflex is configured or active, returns CE wrappers for `NvAPI_D3D_SetSleepMode` and `NvAPI_D3D_Sleep` only to game callers, and passes original driver pointers to Streamline/FFX runtimes, third-party overlays, system modules, and CE modules.
  3. Reused the existing Reflex wrapper path so game-owned NvAPI Sleep can run `ApplyHybridPacingBeforeNativeSleep()` at the game's Reflex sleep point, forward to the original driver function, and mark fresh game sleep for the native handoff state machine.
  4. Kept CE post-Present cadence as the fallback while waiting for stable game sleep, including manual Reflex with DLSS FG active, without reopening GTA's no-manual-limiter Reflex/DLSS FG switching path.

- **Verification**:
  1. `python build.py --skip-updates --run-tests` passed all 663 unit tests and completed the product build after the final source changes.
  2. Full-tree C++ `clang-format --dry-run -Werror` over `common`, `hook`, `captureengine`, `mediaengine`, `testapp`, and `tests` was clean.
  3. `python build.py --skip-updates --lint` now reports C++ Style OK and clang-tidy with 0 warnings, but cannot complete Python lint/type checks because `pyright`, `flake8`, and `black` are missing and pip cannot resolve PyPI from this environment.
  4. The exact `python build.py` command was run as requested. The sandboxed run failed immediately with an MSYS2 bash signal-pipe permission error; the elevated run reached lint and then failed only on the same missing Python tooling / PyPI DNS problem. A later exact rerun was rejected by the platform's elevated-execution limit, so the closest complete verification remains the successful `--skip-updates --run-tests` build plus the lint evidence above.

- **Files changed**: `build.py`, `common/crash_handler.cpp`, `common/crash_handler.h`, `hook/apis/dx11_hook.cpp`, `hook/apis/dx12_hook.cpp`, `hook/apis/dx12_hook.h`, `hook/apis/ffx_hook.cpp`, `hook/apis/ffx_hook.h`, `hook/apis/streamline_hook.cpp`, `hook/common/dx12_overlay_policy.h`, `hook/common/dxgi_shared.cpp`, `hook/common/fps_limiter.h`, `hook/common/fps_limiter_policy.h`, `hook/common/hook_common.h`, `hook/common/overlay_compat.h`, `hook/common/overlay_metrics_publisher.cpp`, `hook/common/reflex_limiter.h`, `hook/common/reflex_limiter_query_hook.inl`, `hook/common/streamline_runtime_policy.h`, `hook/wrappers/iat_hook.cpp`, `tests/test_crash_dump_policy.cpp`, `tests/test_dxgi_shared.cpp`, `tests/test_ffx_api_parsing.cpp`, `tests/test_fps_limiter.cpp`, `tests/test_streamline_runtime_policy.cpp`, `tests/test_stubs.cpp`, `llm-wiki/current.md`, `llm-wiki/regression-testing-and-logging.md`, `llm-wiki/log/recent.md`

- **Stale risk**: High until fresh Talos validation confirms the manual Reflex limiter holds 60 fps with latency near the game's own limiter, including when DLSS FG is active, and fresh GTA validation confirms Reflex / DLSS FG on/off switching still stays stable with CE's manual limiter disabled.

### 2026-04-30 - Clamp explicit Reflex limiter frame queue for remaining Talos latency gap

- **Superseded**: The next entry records why this interim queue-clamp approach was removed and replaced with filtered game-owned NvAPI Sleep handoff. Keep this entry only as historical context for the rejected attempt.

- **Motivation**: Talos `installed/captureengine/logs/20260430_014850` on build `0.1.2670` showed that the post-Present explicit Reflex limiter now capped correctly at 60 fps and reduced NVIDIA overlay latency from the earlier ~70+ ms range to about 60 ms. The game's own limiter was still much better at roughly 40 ms or less, so the remaining issue was not FPS cap failure but queued-frame latency.

- **Root cause analysis**:
  1. `fps_limiter_trace.log` showed stable `Apply: REFLEX post-present stats ... avgFps=60.0` with local waits around 12-13 ms, proving CE was pacing at the right output rate.
  2. The trace did not show CE limiting the DXGI flip queue or the D3D11/DX12 prerender queue when manual Reflex owned cadence. A correct cap can still report elevated NVIDIA overlay latency if the game/render queue keeps one extra frame in flight.
  3. GTA comparison sessions deliberately did not enable CE's manual Reflex limiter. The safe boundary is therefore explicit CE-owned Reflex local cadence, not "Reflex available" or "DLSS FG active/off".

- **Interim fix, now removed**:
  1. `FpsLimiter` temporarily published `WantsExplicitReflexFrameQueueClamp()` while explicit/manual Reflex used CE-owned local cadence. The trace/hook diagnostics included `queue=1` / `queueClamp=1` so Talos runs could prove the queue clamp was active.
  2. `DXGIShared::ApplyPresentFrameLatencyOverrides()` temporarily applied `IDXGISwapChain2::SetMaximumFrameLatency(1)` for that explicit Reflex ownership path, restored the previous latency when the manual clamp ended, and honored explicit user `frameLatency` / `cpu_prerender_limit` overrides first.
  3. DX11/DX12 prerender limiting temporarily treated explicit Reflex queue ownership as an implicit prerender limit of `1` when the user had not configured a prerender override.
  4. The next entry removed this queue-clamp approach and replaced it with filtered game-owned NvAPI Sleep handoff.

- **Verification**:
  1. Direct touched-file formatter check passed: bundled `clang-format --dry-run -Werror` over the edited C++/test files.
  2. Focused coverage passed: `python build.py --run-tests --tests-only --skip-updates --gtest-filter=ReflexFpsLimiterPolicyTest.*:FpsLimiterTest.*` ran 26/26 tests successfully.
  3. Full canonical regression passed: `python build.py --skip-updates --run-tests` ran all 662 unit tests successfully and completed the product build.

- **Files changed**: `hook/common/fps_limiter.h`, `hook/common/fps_limiter_policy.h`, `hook/common/dxgi_shared.h`, `hook/common/dxgi_shared.cpp`, `hook/apis/dx11_hook.cpp`, `hook/apis/dx12_hook.cpp`, `hook/wrappers/dxgi_swapchain_wrap.cpp`, `tests/test_fps_limiter.cpp`, `llm-wiki/current.md`, `llm-wiki/regression-testing-and-logging.md`, `llm-wiki/log/recent.md`

- **Stale risk**: Medium-high until fresh Talos runtime validation confirms the same 60 fps cap with NVIDIA overlay latency near the game's own limiter, and fresh GTA validation confirms Reflex / DLSS FG on/off switching stays crash-free when CE's manual limiter remains disabled.

### 2026-04-30 - Move explicit Reflex DXGI/DX12 cadence after Present for low latency

- **Motivation**: Talos `installed/captureengine/logs/20260429_225939_talos_latencyhigh` on build `0.1.2663` showed the explicit Reflex limiter now capped at the configured 60 fps, but the NVIDIA overlay still reported latency above 70 ms. `fps_limiter_trace.log` showed stable 60 fps stats, but the local cadence wait was usually ~11-12 ms before `Present`, while CE-owned `NvAPI_D3D_Sleep` only took ~0.5 ms.

- **Root cause analysis**:
  1. The previous fix correctly stopped trusting near-immediate driver sleep and restored the cap, but it placed the local cadence wait in the existing pre-Present `FpsLimiter::Apply()` call.
  2. Pre-Present waiting happens after the game has already sampled input, simulated, and rendered the frame, so the wait adds directly to displayed-frame age. That explains the high NVIDIA overlay latency despite correct 60 fps pacing.
  3. Talos still has no game-owned `slReflexSleep` traffic in this path, so the fix cannot simply hand pacing to game-owned Reflex without reopening the GTA/DLSS FG switching risks.

- **Fix**:
  1. `FpsLimiter::Apply(bool allowPostPresentReflexCadence)` can now arm explicit CE-owned Reflex cadence during the pre-Present phase without doing the wait yet.
  2. `FpsLimiter::ApplyPostPresent()` performs the local cadence wait and CE-owned `NvAPI_D3D_Sleep` after `Present` returns. This blocks the game before it starts the next simulation/render frame, preserving the 60 fps cap while avoiding the stale-frame latency added by pre-Present sleeping.
  3. DXGI/DX12 Present and Present1 paths in `hook/common/dxgi_shared.cpp` and `hook/wrappers/dxgi_swapchain_wrap.cpp` opt into the two-phase path and call `ApplyPostPresent()` only after a successful Present. Non-DXGI call sites keep the previous single-phase behavior unless they explicitly opt in later.
  4. `hook/common/fps_limiter_policy.h` now exposes the post-present phase decision for focused tests. Game-owned Reflex handoff still requires fresh stable game sleep and no recent present gap, so GTA Reflex / DLSS FG switching remains guarded.
  5. Diagnostics now distinguish `Apply: REFLEX post-present armed`, `Apply: REFLEX post-present cadence ...`, and `Apply: REFLEX post-present stats ...` from the older pre-Present local cadence path.

- **Verification**:
  1. Focused coverage passed: `python build.py --run-tests --tests-only --skip-updates --gtest-filter=FpsLimiterTest.*:ReflexFpsLimiterPolicyTest.*:ProcessIPCTest.*` ran 27/27 tests successfully.
  2. Full canonical verification passed: `python build.py --skip-updates --run-tests` ran all 661 unit tests successfully and completed the product build.
  3. Canonical compile passed after the final code changes: `python build.py --skip-updates` produced build `0.1.2670` successfully.
  4. Lint/LSP status was rechecked: touched C++ files pass direct `clang-format --dry-run -Werror`, and `python build.py --lint --skip-updates` reports clang-tidy with 0 warnings, but the repo-wide lint command still cannot complete because it reports 5 clang-format batches outside the direct touched-file check and host Python packages `pyright`, `flake8`, and `black` are missing while pip DNS lookup fails. The installed npm/Node pyright path also crashes before analysis with the local `ncrypto::CSPRNG(nullptr, 0)` Node assertion. No Python type errors were reported by an actual analyzer in this run because the analyzer could not start.

- **Files changed**: `hook/common/fps_limiter.h`, `hook/common/fps_limiter_policy.h`, `hook/common/dxgi_shared.cpp`, `hook/wrappers/dxgi_swapchain_wrap.cpp`, `tests/test_fps_limiter.cpp`, `llm-wiki/index.md`, `llm-wiki/current.md`, `llm-wiki/regression-testing-and-logging.md`, `llm-wiki/log/recent.md`

- **Stale risk**: Medium-high until fresh Talos runtime validation confirms `REFLEX post-present cadence` holds 60 fps with NVIDIA overlay latency well below 70 ms, and fresh GTA validation confirms Reflex / DLSS FG on/off switching still avoids accidental game-owned handoff and stays crash-free.
