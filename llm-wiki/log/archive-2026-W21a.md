# llm-wiki Log — Archive 2026-W21a

### 2026-05-24 - Talos FSR launch crash, minimal-first dumps, and DX12 FG switch-app teardown stress (build 0.1.3529 / tests 0.1.3530)

- **Inputs**:
  - Talos FSR launch crash `installed/captureengine/logs/20260524_182433`: hook logging stopped at swapchain wrapper final teardown after external refs reached zero, and automatic dump creation left only a 0-byte `.dmp.inprogress` artifact.
  - Earlier Talos/GTA FG sessions showed the overlay can render through the FFX callback path and report FSR FG correctly, so the remaining failure was likely a lifecycle/teardown edge rather than "FSR overlay impossible".
- **Root cause refinement**:
  - The dump pipeline attempted broader/richer dumps before securing a minimal artifact; in fragile crash states that could leave only an empty `.dmp.inprogress`.
  - `CWrapDXGISwapChain` destruction was still performing optional DXGI side-channel cleanup (`UnregisterDestructionCallback`, wrapper private-data clear) during final wrapper release. That is not required for correctness once the wrapper is already dying, and it is risky around Streamline/FFX/third-party-overlay swapchain teardown.
- **Fixes**:
  - `common/crash_handler.cpp` now writes minimal-first dumps (`minimal-primary`, `minimal-no-exception`) before compatibility/rich variants, and logs `CrashHandler: using minimal-first crash dump attempts`.
  - `hook/wrappers/dxgi_swapchain_wrap.cpp` now logs staged destructor progress and skips optional destruction-callback unregister/private-data clear when `m_Releasing` indicates final wrapper release. COM refs are still released normally.
  - `hook/common/dx12_overlay_policy.h` exposes regression-testable helpers for the final-release teardown policy.
  - `dx12_fg_switch_test.exe` gained config/CLI stress knobs for auto-exit, startup native swapchain recreates, repeated FSR suspend/resume, and shutdown isolated native-wrapper probes.
- **Validation**:
  - `python build.py --skip-updates` passed (build `0.1.3529`).
  - Injected switch-app session `installed/captureengine/logs/20260524_185734` ran `dx12_fg_switch_test.exe 1280 720 20 --duration 45 --bootstrap-native-swaps 3 --startup-recreates 4 --fsr-suspend-interval 2` to exit code 0. It produced FSR callback overlay render diagnostics, DLSS PostSL overlay submit diagnostics, four startup recreate stress cycles, three shutdown native wrapper probes, repeated FSR suspend/resume, staged wrapper destructor logs, and no crash/error markers.
  - `python build.py --no-build --run-tests --skip-updates` passed all 802 tests (displayed metadata `0.1.3530`).
  - Process cleanup check found no lingering `captureengine.exe`, `dx12_fg_switch_test.exe`, `dx12_test.exe`, Talos, or unit-test processes.
- **Source anchors**: `common/crash_handler.cpp`, `hook/common/dx12_overlay_policy.h`, `hook/wrappers/dxgi_swapchain_wrap.cpp`, `testapp/dx12_fg_switch_test.cpp`, `testapp/dx12_fg_switch_config.inl`, `testapp/dx12_fg_switch_swapchain.inl`, `tests/test_crash_handler.cpp`, `tests/test_dxgi_shared.cpp`, `installed/captureengine/logs/20260524_185734/hook_debug.log`.

### 2026-05-22 - GTA/Talos DX12 FG overlay crash fixes and hardened switch-app stress validation (build 0.1.3490 / tests 0.1.3491)

- **Inputs**:
  - GTA FSR crash/freeze `installed/captureengine/logs/20260522_021818`: the CE callback bridge rendered overlay first, but a later separate DX12 overlay submit still ran while FSR was active and the device reported `0x887A002B`.
  - Talos DLSS no-overlay `installed/captureengine/logs/20260522_121217`: PostSL submitted once, then remained in warmup suppression with `postSLCallback=1`, `postSLActive=1`, `skip=1`, and `stableFrames=1`.
  - Talos FSR crash `installed/captureengine/logs/20260522_121344`: crash log showed stack overflow inside `capture_hook_x64.dll`; hook logs showed FFX configure had captured CE's own present callback as the original callback.
- **Fixes**:
  - Native/authoritative FSR now suppresses separate injected DX12 overlay GPU work as soon as direct FSR evidence, FSR runtime mode, or runtime-owned native-FG present ownership exists; it no longer waits for the older runtime-owned swapchain latch. The FSR route stays callback-only while FSR owns presentation.
  - The FFX present-callback bridge is idempotent. Already-bridged configures retain or create the bridge without storing CE's own callback as the original, missing user context is normalized to the bridge key, and the DX12 callback has a recursion guard.
  - Retained PostSL startup activation can be serviced from a confirmed-but-stalled warmup path through an explicit warmup-service flag, preserving the normal two-argument shared service as pre-confirmation only.
  - Explicit `slDLSSGSetOptions(OFF)` after confirmed PostSL rendering is authoritative when no startup/unconfirmed/settling/stabilizing window is active, which fixes stale DLSS FG reporting in the switch app after `DLSS FG -> all FG off`.
  - `dx12_fg_switch_test.exe` now refreshes FSR FG configure descriptors every active FSR frame, mimicking harder real-world engines that resend FFX descriptors during play/loading and stress-testing the bridge idempotency path.
  - FFX bridge install/retain diagnostics are now throttled after the first repeated entries so per-frame configure games remain diagnosable without flooding logs.
- **Validation**:
  - `python build.py --skip-updates` passed (build `0.1.3490`).
  - `python build.py --no-build --run-tests --skip-updates` passed all 792 tests (displayed metadata `0.1.3491`).
  - Injected switch-app session `installed/captureengine/logs/20260522_125259` exited with code 0 and no lingering `captureengine.exe` or `dx12_fg_switch_test.exe` processes. It produced no `crash.log`, no `.dmp`, no device-removal markers, no separate `DX12: FG overlay SUBMIT` during FSR, sampled FSR callback overlay render diagnostics, sampled DLSS PostSL overlay submit diagnostics, one authoritative DLSS OFF breadcrumb, and one `FG state transition ON->OFF via SetOptions`.
- **Source anchors**: `hook/common/dx12_overlay_policy.h`, `hook/apis/dx12_hook.cpp`, `hook/apis/dx12_hook.h`, `hook/apis/ffx_hook.cpp`, `hook/apis/streamline_hook.cpp`, `hook/common/streamline_runtime_policy.h`, `testapp/dx12_fg_switch_test.cpp`, `testapp/dx12_fg_switch_fsr.inl`, `tests/test_dxgi_shared.cpp`, `tests/test_streamline_runtime_policy.cpp`, `installed/captureengine/logs/20260522_125259/hook_debug.log`.

### 2026-05-22 - Mixed DX12 FSR/DLSS FG switch test app and protected FFX startup quiesce (build 0.1.3482 / tests 0.1.3483)

- **New test app**: `dx12_fg_switch_test.exe` starts with all FG off, automatically switches OFF -> FSR -> DLSS -> FSR at 3-second intervals, and supports live keys `1` off, `2` DLSS, and `3` FSR without restarting. It uses the real Streamline/DLSS-G and FidelityFX/FSR FG runtimes and logs `[FG-DIAG]` transition, frame, cleanup, and official SDK result breadcrumbs.
- **Standalone test-app fixes**: Leaving active FSR FG at runtime now destroys the FSR context and recreates a native swapchain before continuing. A disabled parked FSR swapchain can stop advancing the frame-latency waitable, while recreating a native swapchain during process-exit cleanup can freeze shutdown; cleanup now only disables/destroys active runtimes and leaves native recreation to runtime OFF transitions.
- **Injected crash root cause**: During DLSS -> FSR switching, protected official FFX startup swapchain creation deliberately deferred heavy CE side effects until enabled `ffxConfigure`. That was correct for the prior GTA fast-fail family, but it left a stale DLSS PostSL callback alive long enough to submit overlay work after AMD/FFX startup takeover began, causing D3D12 device removal (`0x887A002B`) in the mixed switch test.
- **Fix**: Protected official FFX startup now immediately quiesces live Streamline/PostSL state, while still deferring Present refresh, queue-ownership mutation, and FFX export inspection until enabled `ffxConfigure` or the then-accepted sustained-progress fallback. **Superseded refinement**: GTA `20260525_195848_gtafreeze` later rejected progress-only graduation; current code requires direct enabled FFX proof/callback evidence for takeover side effects, with only staged-queue overlay-only rendering allowed while protected startup is pending. The regression policy helper is `ShouldQuiesceStreamlinePostSLDuringProtectedOfficialFFXStartup(...)`, and the expected runtime breadcrumb is `Protected official FFX startup immediately quiesced Streamline/PostSL`.
- **Rejected approach**: Do not allow normal overlay rendering on a disabled/inactive FSR runtime-owned swapchain. Local session `installed/captureengine/logs/20260522_014211` reproduced `0x887A002B` on the first normal overlay submit; FSR-owned presentation remains callback-only until ownership is truly gone.
- **Validation**:
  - `python build.py --skip-updates` passed (build `0.1.3482`).
  - `python build.py --no-build --run-tests --skip-updates` passed all 790 tests (displayed metadata `0.1.3483`).
  - Standalone `dx12_fg_switch_test.exe` completed the automatic sequence and manual all-direction switching with exit 0, no app failures, and no lingering process.
  - Injected session `installed/captureengine/logs/20260522_021308` completed with no dumps, no device-removal markers, 26 FFX callback overlay render diagnostics, 95 PostSL overlay submit diagnostics, 8 PostSL confirmations, and 1 immediate protected-FFX quiesce breadcrumb.
- **Source anchors**: `testapp/dx12_fg_switch_test.cpp`, `testapp/dx12_fg_switch_*.inl`, `testapp/dx12_fsr_fg_test.cpp`, `build.py`, `hook/apis/dx12_hook.cpp`, `hook/apis/ffx_hook.cpp`, `hook/common/dx12_overlay_policy.h`, `tests/test_dxgi_shared.cpp`, `tests/test_crash_handler.cpp`, `installed/captureengine/logs/20260522_021308/hook_debug.log`.

### 2026-05-21 - DX12 FG test-app injected overlay crash and FSR callback bridge fix (build 0.1.3454 / tests 0.1.3455)

- **Symptom**: With CaptureEngine injected, both local DX12 FG test apps crashed during startup. After the crash fix, DLSS rendered overlay through PostSL, but FSR installed the FFX present-callback bridge without actually drawing overlay there; the normal path logged `FSR FG active but scQueue=null, SKIPPING overlay`.
- **Root causes**:
  1. `TryInstallFatalTerminationDumpHooks()` installed inline hooks on both KERNELBASE and kernel32 forwarded fatal-exit API pairs. A shared original pointer could point at the second trampoline; forwarded calls then recursed through the first hook and stack-overflowed when benign debug exceptions such as `OutputDebugString` raised `0x40010006`.
  2. `ShouldBridgeOverlayViaFFXPresentCallback()` required `HookHasRuntimeOwnedNativeFGPresentPath()`. The official FFX callback bridge can be installed and active from direct FFX/FSR evidence before that older native-FG-present-path latch is set, especially in the local `dx12_fsr_fg_test.exe` configuration.
- **Fix**:
  - For forwarded KERNELBASE/kernel32 pairs (`RaiseException`, `RaiseFailFastException`, `TerminateProcess`, `ExitProcess`), hook KERNELBASE first and use kernel32 only as fallback. Ntdll/ucrt termination hooks remain unchanged.
  - Added a shared policy helper so the FFX callback bridge trusts runtime-owned native-FG path, authoritative FSR API-active state, direct FFX confirmation, or FSR runtime mode.
  - FFX present-callback backend init now falls back to the known swapchain/command/original game queues when the callback descriptor has no queue, and logs skip reasons for missing official evidence, resources, IPC, shared memory, or hidden overlay.
- **Validation**:
  - `python build.py --skip-updates` passed (build `0.1.3454`).
  - Injected session `installed/captureengine/logs/20260521_233919` ran `dx12_dlss_fg_test.exe` and `dx12_fsr_fg_test.exe` to clean exit, with no crash dumps, no device-removal markers, and no lingering CaptureEngine/test-app processes afterward.
  - DLSS path: 130 `Post-SL overlay SUBMIT` diagnostics.
  - FSR path: `FFX Hook: Installed DX12 overlay present-callback bridge`, one callback-backend init, and 27 `DX12: FFX present callback rendered overlay on runtime-owned FSR path` diagnostics.
  - `python build.py --no-build --run-tests --skip-updates` passed all 784 tests (displayed metadata `0.1.3455`).
- **Source anchors**: `hook/main.cpp`, `hook/apis/dx12_hook.cpp`, `hook/common/dx12_overlay_policy.h`, `tests/test_dxgi_shared.cpp`, `installed/captureengine/logs/20260521_233919/hook_debug.log`.

### 2026-05-21 — dx12_fsr_fg_test window stall fix

- **Symptom**: Standalone `dx12_fsr_fg_test.exe` showed brief motion then froze; sometimes blank after FSR FG enable.
- **Root causes**:
  1. FFX FG swapchain + `SetMaximumFrameLatency(1)` with 3 app buffers — `Present()` blocked after the first frames (frame-latency deadlock).
  2. `TestPresentCallback` logged only; did not copy `currentBackBuffer` → `outputSwapChainBuffer` when FG owned presentation.
  3. Fence slots assumed fixed `FRAME_COUNT` indices; FFX swapchain could report out-of-range back-buffer indices.
- **Fix** (`testapp/dx12_fsr_fg_test.cpp`, `testapp/dx12_fg_resources.h`):
  - Query swapchain `BufferCount`; set max frame latency to `min(buffers, 3)`; wait on `GetFrameLatencyWaitableObject()` before each render.
  - Implement `CopyFfxPresentSourceToOutput` in present callback (mirrors CE `CopyFFXPresentSourceToOutput`).
  - Size RTV/fence arrays up to 4 buffers; clamp/log bad back-buffer indices; slow-fence diagnostic logging.
  - Preload `amd_ags_x64.dll` / `amd_acs_x64.dll` before FFX runtime; default 1920×1080 windowed for faster bring-up.
  - Frame heartbeat logging every 60 frames.
- **Validation**: 5s standalone run reached `frame heartbeat` at 60/120/180 with FSR FG enabled and present/frame-gen callbacks firing; no stall.
- **Source anchors**: `testapp/dx12_fsr_fg_test.cpp`, `testapp/dx12_fg_resources.h`.

### 2026-05-21 — FSR FG and DLSS FG DX12 test apps + auto DLL download (build 0.1.3426)

- **New test apps**: Two new DX12 test apps in `testapp/` for automated FG testing with the capture engine:
  - `dx12_fsr_fg_test.cpp` — loads `amd_fidelityfx_framegeneration_dx12.dll` at runtime (SDK v2.2.0), calls `ffxCreateContext`/`ffxConfigure`/`ffxDestroyContext`. Realistic config: 3 back buffers, incremental frameID, hudlessColor from backbuffer. FG starts disabled, enables after ~2s.
  - `dx12_dlss_fg_test.cpp` — loads `sl.interposer.dll` at runtime (Streamline v2.11.1), calls `slInit()` + `slSetD3DDevice` + `slGetFeatureFunction`/`slDLSSGSetOptions`. Realistic config: 3 back buffers, `numBackBuffers=3`, `slDLSSGGetState()` polling after enable. FG starts disabled (mode=0), enables after ~2s (mode=1).
- **Build**: Added to `build.py compile_testapps()` — x64 + x86 variants for both. Uses same linker flags as `dx12_test`.
- **Auto DLL download**: New `setup_fg_sdk_dlls()` function in `build.py` downloads FidelityFX-Samples-v2.2.0-prebuilt.zip and streamline-sdk-v2.11.1.zip to `build/fg_sdk_cache/`, extracts only the needed runtime DLLs to `installed/testapp/`. Respects `--skip-updates`. Automatically runs before `compile_testapps()`.
- **Extracted DLLs**: `amd_fidelityfx_framegeneration_dx12.dll`, `amd_fidelityfx_loader_dx12.dll` (FSR FG); `sl.interposer.dll`, `sl.common.dll`, `sl.dlss_g.dll` (DLSS FG).
- **Fixes (build 0.1.3427)**:
  - DLSS FG test: removed `slInit(nullptr)` which caused hang on startup. Streamline interposer auto-initializes on DLL load; explicit `slInit` with null desc is unsafe.
  - FSR FG test: reordered DLL search to prefer loader DLL first (proper SDK v2.2.0 entry point), added verbose logging of effect type and return codes for easier debugging.
  - **FG activity caveat**: Both test apps make correct FFX/Streamline API calls that the capture engine's hooks will detect as FG active. However, `ffxCreateContext` with minimal `{type,pNext=nullptr}` (no DX12 backend descriptor) means the AMD runtime won't actually interpolate frames — the capture engine's *detection* works, but real FG rendering needs a proper backend descriptor in the pNext chain.
- **Source anchors**: `testapp/dx12_fsr_fg_test.cpp`, `testapp/dx12_dlss_fg_test.cpp`, `build.py` (~line 1308 setup_fg_sdk_dlls, ~line 3707 compile_tasks, ~line 5100 call site).
- **Known gaps**: No integration into `testapp/run_tests.py` yet. No targeted unit tests yet.

### 2026-05-21 — VEH single-shot hook for ffxConfigure on official AMD runtime (build 0.1.3414 / tests 0.1.3415) — SUPERSEDED historical predecessor

Superseded note, verified 2026-05-30: current official AMD modules use IAT/dynamic FFX routing and skip standard inline JMP export hooks, but official DX12 `ffxConfigure` may also arm a guarded re-arming int3/VEH fallback for SDK dispatch-table or intra-module calls. This old entry remains historical context for why normal overlay ECL submission, broad progress fallbacks, permanent inline detours, and single-shot-only breakpoint hooks were rejected.

- **Input**: Session `20260521_144908` (build 0.1.3410) — the overlay completion fence fix still crashed because the AMD FSR FG runtime rejects ANY unexpected `ExecuteCommandLists` on the shared device. The `realECL()` call at `dx12_hook.cpp:14276` itself triggers `devRemoved=0x887A002B` (pre-ECL check passes, post-ECL check fails).
- **Root cause**: CE's attempt to submit overlay GPU work on the game queue triggers device removal by the AMD FSR runtime. The completion fence cannot help because the device dies during ECL submission, before any fence signal can fire. The FFX present callback bridge (`DX12_RenderOverlayViaFFXPresentCallback`) would provide a safe rendering point (called by FSR after its GPU work completes), but CE could not install it — inline hooks on `ffxConfigure` cause fast-fail (0xC0000409) because the official AMD runtime uses Control Flow Guard (CFG) or anti-tampering that rejects standard inline detour patches.
- **Fix — VEH single-shot hook**:
  - Instead of a permanent inline JMP hook on `ffxConfigure`, patch the first byte with `0xCC` (int3) and catch it in a VEH handler. This bypasses CFG validation because int3 is a breakpoint, not an indirect call through an unapproved target. The handler restores the original byte, calls `Hooked_ffxConfigure()` (which installs the present callback bridge via `DX12_SetFFXPresentCallbackBridge`), and returns to the caller via stack-popping (reads return address from RSP, sets RIP to it, skips the real ffxConfigure body since `Hooked_ffxConfigure` already called `g_Original_ffxConfigure`).
  - Added `FfxConfigureSingleShotVEH` handler, `InstallFfxConfigureSingleShotHook()`, and cleanup in `ffx_hook.cpp:Shutdown()`. Installed from the protected-module init path (where `!allowInlineHooks && !allowIATHooks` for official AMD modules).
  - x86/x64 compatibility: context register names (`Rcx`/`Ecx`, `Rdx`/`Edx`, `Rax`/`Eax`, `Rip`/`Eip`) use `#ifdef _WIN64`.
  - Removed unused `prevCount` variable that produced a compiler warning.
- **Key insight**: The existing `Hooked_ffxConfigure` at `ffx_hook.cpp:311` already correctly modifies the configure descriptor to add CE's `DX12_RenderOverlayViaFFXPresentCallback` as the present callback. The only problem was that this hook never fired for the official AMD runtime because CE skipped inline hooks. The VEH single-shot approach solves this: it intercepts the FIRST intra-module call to ffxConfigure (which GetProcAddress hooks can't see), installs the callback bridge, and removes itself.
- **Source anchors**: `hook/apis/ffx_hook.cpp` — VEH handler (~line 593), install function (~line 610), call from init path (~line 575), shutdown cleanup (~line 810). `hook/common/ffx_api_parsing.h:202-206` — `ShouldInlineHookFFXExportsForModule` returns false for official AMD modules.
- **Regression tests**: All 783 tests pass (build `0.1.3414`). `python build.py --skip-updates` passed. `python build.py --no-build --run-tests --skip-updates` passed 783/783 tests (build `0.1.3415`).
- **Historical validation note**: this entry's original expected VEH breadcrumb has been superseded. Current official AMD evidence should instead include IAT/dynamic FFX interception such as `GetProcAddress: Intercepted FFX API ffxConfigure`, followed by `FFX Hook: Installed DX12 overlay present-callback bridge for context=...`.

### 2026-05-21 — Overlay completion fence for FG overlay safety (build 0.1.3410 / tests 0.1.3411)

- **Input**: GTA V Enhanced DX12 crash dump `session_20260521_142658` from build 0.1.3409 (the 2-second max overlay suspension enforcement).
- **Dump analysis**:
  - `ERR_GFX_STATE (0x887A002B)` — D3D device removal, not a simple crash
  - Timeline: `14:28:34.132` (2s timeout fires) → `14:28:34.139` (InitImGui font upload on game queue) → `14:28:34.825` (alloc Reset OK) → `14:28:34.834` (SUBMIT #1: `devRemoved=0x887A002B`)
  - The FFX runtime reads the swapchain backbuffer while CE's overlay GPU work (font texture upload, draw commands) is still in-flight on the same queue → inconsistent backbuffer state → device removal
- **Root cause (corrected)**: The 2-second timeout enforcement forced overlay GPU work (command lists, barriers, font upload, draw commands) onto the game's D3D12 queue via raw ECL at line 14228-14235, but **no fence signal or GPU completion wait** was performed before returning from ProcessFrame. The FFX runtime then reads the backbuffer for its own frame interpolation processing, finds it in a transitional state (barriers still pending, overlay draws not yet complete), and removes the D3D device with ERR_GFX_STATE. The issue is NOT about cross-queue submission — it is about **GPU work completion**: overlay GPU work must be fully completed before the FG runtime reads the backbuffer.
- **Why inline hooking Signal is not viable**: SL (Streamline) hooks the Signal vtable slot on D3D12 command queues for frame sync. Calling the hooked Signal would let SL see CE's fence value, potentially causing frame pacing issues. Using `g_State.fence` (CE's own shared fence) would also be visible to SL via the hooked Signal.
- **Fix — Overlay GPU work completion fence**:
  - Added `ID3D12Fence* g_OverlayCompletionFence` (separate from `g_State.fence`) and `g_RealD3D12Signal` (raw D3D12 Signal function pointer from vtable[14]) in `hook/apis/dx12_hook.cpp`.
  - Probed `g_RealD3D12Signal` from a clean COMPUTE D3D12 command queue vtable[14] alongside the existing ECL probe (line 3970 area). COMPUTE queues are not hooked by SL, so vtable[14] returns the real driver Signal.
  - Created `g_OverlayCompletionFence` in `InitOverlaySync()` alongside `g_State.fence` (same device, signaled value 1, initially 0).
  - Replaced the old "skip fence signal entirely" block for FG with: after `realECL(eclQueue, 1, lists)`, signal `g_OverlayCompletionFence` to value 1 via raw Signal pointer, then CPU-wait (`SetEventOnCompletion(1, event)` + `WaitForSingleObject(event, INFINITE)`) to ensure all overlay GPU work completes before returning from ProcessFrame.
  - Released the completion fence in `DX12Hook::Shutdown()`.
- **Why safe**:
  - The raw Signal pointer bypasses SL/FSR vtable hooks, so SL never sees CE's completion fence value.
  - A separate fence (`g_OverlayCompletionFence`) avoids any interference with SL's view of `g_State.fence`.
  - The CPU wait is on a fence that was just signaled — the wait completes immediately if the GPU has already finished (typical) or blocks briefly until the GPU catches up.
  - No cross-queue interaction: the fence is on the same queue used for overlay ECL submission.
- **Coverage**: This fix addresses the fundamental safety issue that the 2-second timeout did not. Whether the overlay submits on the game queue (2s timeout) or with normal FG suppression logic, if it does submit, its GPU work must complete before Present / before the FG runtime reads the backbuffer.
- **Regression tests**: All 783 tests pass (build `0.1.3410`). `python build.py --skip-updates` passed (build `0.1.3410`). `python build.py --no-build --run-tests --skip-updates` passed 783/783 tests (build `0.1.3411`).
- **Manual validation still needed**: Re-run GTA V Enhanced with FSR FG active. Expected: overlay renders within ~2 seconds, no ERR_GFX_STATE, no device removal. Test all FG transitions.
- **Source anchors**: `hook/apis/dx12_hook.cpp` — `g_OverlayCompletionFence` and `g_RealD3D12Signal` declarations (near line 111), ECL probe extension for Signal (line ~3970), `InitOverlaySync()` creation, FG fence-skip replacement (line ~14357-14410), shutdown release.

### 2026-05-21 - 2-second max overlay suspension enforcement for all FG transitions (build 0.1.3409 / tests 0.1.3409) — **REJECTED (causes ERR_GFX_STATE)**

- **Input**: GTA V Enhanced DX12 logs at `installed/captureengine/logs/20260521_141135`; the previous fixes (save-game-reload progress-resolved clear, 30-second escape hatch, shutdown freeze cleanup) were insufficient. The progress-resolved path graduates the **initial** GTA boot (not a save-game reload), and the FFX present callback bridge **never fires** because CE's GetProcAddress-only hook cannot intercept the official AMD runtime's `ffxConfigure`. The overlay stays permanently blocked despite `sameQueue=1` (swapchain queue == original game queue) and `stableProof=1`. The 30-second escape hatch was too slow — the freeze happens within 55 seconds, the hatch needs 31.5s.
- **Root cause (as understood at the time)**: The progress-resolved assumption was designed for exactly this case (no visible ffxConfigure despite sustained frame progress), but the stall-fallback gate unconditionally blocked the overlay because `directFFXApiConfirmation=false`. No existing mechanism could recover visibility within any reasonable time bound.
- **Fix attempted — Universal 2-second max overlay suspension enforcement**:
  - Added file-static `g_OverlaySuppressedSinceMs` in `dx12_hook.cpp` tracking when the overlay was first suppressed.
  - In `ShouldSkipSeparateOverlayGpuWorkForCurrentSwapchain()`: if the overlay has been continuously suppressed for >= 2 seconds, force-return `false` (allow overlay rendering) regardless of FG state.
  - The timer is reset to 0 when the normal (non-override) path returns false.
  - In `dx12_overlay_policy.h` `ShouldAllowNormalOverlayFallbackForStalledFFXPresentCallback()`: removed the 30-second escape hatch.
  - Added shutdown reset of `g_OverlaySuppressedSinceMs`.
- **Why rejected**: CRASH — GTA V Enhanced with FSR FG active produced ERR_GFX_STATE (D3D device removal) ~700ms after the 2-second timeout fired. The crash log at `session_20260521_142658` shows: `14:28:34.132` (timeout triggers InitImGui/font upload) → `14:28:34.834` (`devRemoved=0x887A002B`). The root cause is NOT about cross-queue submission: overlay GPU work was submitted on the game queue but **without any fence signal or GPU completion wait**, so the FFX runtime read an inconsistent backbuffer. Replaced by the overlay completion fence approach (build 0.1.3410).
- **Source anchors**: The code changes from this approach were removed when transitioning to the completion fence approach.

### 2026-05-21 - GTA FSR FG save-game-reload overlay recovery and shutdown cleanup (build 0.1.3407 / tests 0.1.3407) — SUPERSEDED by direct FFX proof requirement

Superseded note, verified 2026-05-26: the progress-resolved fallback and long-timeout escape hatch described in this historical entry are no longer accepted recovery mechanisms. Current native-FSR recovery requires direct enabled `ffxConfigure` / FFX present-callback evidence or a genuinely non-runtime-owned drawable swapchain.

- **Input**: GTA V Enhanced DX12 logs and freeze dump at `installed/captureengine/logs/20260521_134802`; after loading a save game with FSR FG enabled, the overlay never reappeared and the game froze on close. The progress-resolved assumption from the initial boot (build 0.1.3377) survived the FFX context lifecycle reset.
- **Root cause** (overlay lost): When GTA loads a save game, it destroys and recreates all FFX FG contexts. The `directFFXApiConfirmed` flag was correctly cleared by `SetFSRFGActive(false)` on context destruction and `SetFSRFGActive(true)` on recreation. However, the progress-resolved assumption (`g_OfficialFFXRuntimeOwnedPresentPathAssumedAfterProgress`) — latched during the initial GTA boot — was **never cleared** by context destruction. After recreation, the new `ffxConfigure(enabled=true)` was not seen by CE's GetProcAddress-only hook, so `MarkDirectFFXApiConfirmation()` never re-established `directFFXApiConfirmed`. The stall-fallback gate `ShouldAllowNormalOverlayFallbackForStalledFFXPresentCallback()` checked `progressResolvedOfficialFFXPresentPath && !directFFXApiConfirmation && !ffxPresentCallbackEverFired` and returned false, permanently blocking normal overlay rendering.
- **Root cause** (freeze on close): The render thread was stuck in `WaitForSingleObjectEx` on a DX12 synchronization primitive. CE's overlay was in a deferred/suspended state, and CE's state references (present callback bridges, progress-resolved assumption) were only cleaned up during `CleanupOverlay`/`CleanupRTVs` — too late to prevent contributing to the stall.
- **Fix 1 — Clear progress-resolved assumption on all-FG-contexts-destroyed**:
  - Added `DX12_ClearOfficialFFXRuntimeOwnedPresentPathAssumption(const char* reason)` to `dx12_hook.h` / `dx12_hook.cpp` (non-static wrapper around the existing static `ClearOfficialFFXRuntimeOwnedPresentPathAssumption`).
  - In `ffx_hook.cpp` `Hooked_ffxDestroyContext()`, when `newCount == 0` (all FG contexts gone), call `DX12_ClearOfficialFFXRuntimeOwnedPresentPathAssumption("FFX FG context destroy")` before `g_FGCompat.SetFSRFGActive(false)`.
  - After this fix: context destruction clears the progress-resolved latch → stall fallback gate is no longer blocked → `ShouldSkipSeparateOverlayGpuWorkForCurrentSwapchain()` returns false → overlay rendering proceeds normally.
- **Fix 2 — Long-timeout escape hatch**:
  - In `dx12_overlay_policy.h` `ShouldAllowNormalOverlayFallbackForStalledFFXPresentCallback()`: added `stallDurationMs` parameter. If `progressResolvedOfficialFFXPresentPath && !directFFXApiConfirmation && !ffxPresentCallbackEverFired` AND the stall has lasted >= 30 seconds AND `progressResolvedStableOverlayProof` is true (same queue, device healthy), allow the fallback. This is a defense-in-depth safety net for edge cases Fix 1 doesn't cover.
  - In `dx12_hook.cpp`: added `g_FFXPresentCallbackFirstStallEverDetectedMs` tracking with `UpdateFFXPresentCallbackFirstStallDetection()` and `GetFFXPresentCallbackStallDurationMs()`. The first-stall timestamp is set once via `compare_exchange_strong` and cleared on shutdown.
- **Fix 3 — Shutdown cleanup**:
  - In `DX12Hook::Shutdown()`: added force-clean of the FFX present callback overlay adapter (`ResetFFXPresentCallbackOverlayBackend`), clearing of all FFX present callback bridges under mutex, clearing of the progress-resolved assumption, and resetting of the first-stall detection timestamp — all before the existing `CleanupResources`/`CleanupOverlay`/`CleanupRTVs` sequence.
  - Added detailed shutdown-phase log of the complete CE state (runtime mode, overlay init, sync init, FG ownership, native FG path, progress-resolved, callback bridge count).
- **Fix 4 — Improved debug logging**:
  - `fg_detection.cpp`: `SetFSRFGActive()` now logs when `directFFXApiConfirmed` transitions (cleared on context recreation when transitioning from inactive-to-active; cleared on FSR OFF; `MarkDirectFFXApiConfirmation` logs when skipped because FSR not active).
  - `ffx_hook.cpp`: At the `MarkDirectFFXApiConfirmation()` call site, logs when confirmation is established from an enabled `ffxConfigure`.
  - `dx12_hook.cpp`: The two main overlay deferral log lines (init deferral at ProcessFrame overlay-init path, and staged sync-init deferral) now include a full decision matrix: `apiFSR`, `directFFX`, `progressResolved`, `nativeFGPath`, `ffxStalled`, `ffxStallAllows`, `runtimeOwns`, `callbackEver`, `callbackLast`, `sameQueue`, `stableProof`, `cooldown`, `overlayInit`, `syncInit`.
- **Regression tests**: All 783 existing tests pass. No new test units added (the fix is in hook integration code that is not easily unit-testable; the existing `DXGISharedTest.FFXPresentCallbackStallAllowsNormalOverlayRendering` and `DXGISharedTest.ProgressResolvedOfficialFFXCallbackStallRequiresCallbackBridgeBeforeNormalOverlayFallback` implicitly cover the affected path).
- **Verification**: `python build.py --skip-updates` passed (build `0.1.3406`). Focused `python build.py --no-build --run-tests --skip-updates --gtest-filter="FGSessionStateTest.*:FFXApiParsingTest.*:FFXHookValidationTest.*:DX12FGTransitionSequencesFixture.*:DX12FGTraceReplayFixture.*"` passed all. Full `python build.py --no-build --run-tests --skip-updates` passed 783/783 tests (build `0.1.3407`).
- **Manual validation still needed**: Re-run GTA V Enhanced with FSR FG active, load a save game. Expected: overlay reappears within ~3-5 seconds after FSR FG reactivates (logs should show `Cleared progress-resolved official FFX runtime-owned Present path assumption (FFX FG context destroy)` or the stall no longer blocking fallback). Verify no freeze on close. Also test DLSS FG and FG mode switching in Talos Principle for regressions.

### 2026-05-20 - GTA official FFX progress fallback overlay recovery (build 0.1.3377 / tests 0.1.3379) — REJECTED/SUPERSEDED

Rejected note, verified 2026-05-26 and refined 2026-05-31: GTA `20260525_195848_gtafreeze` proved this progress-only fallback can wedge AMD FSR threads in `amd_fidelityfx_dx12!ffxQuery`. Current implementation must keep takeover/capture/hook/probe side effects quiesced until direct FFX proof or callback evidence appears; only staged Direct-queue overlay-only rendering is allowed while protected startup is pending.

- **Input**: GTA V Enhanced DX12 logs and freeze dumps at `installed/captureengine/logs/20260520_153423`; loading with FSR FG no longer crashed, but the CE overlay never reappeared after FSR FG activated, and the game later froze on close. The automatic freeze dump was useful and showed a freeze-style wait rather than a fresh CE crash exception.
- **Root cause**: The protected official AMD FFX startup pass-through from build `0.1.3362` was too absolute. It correctly avoided the early `0xC0000409` fail-fast by deferring Present refresh, queue mutation, FFX export inspection, and Streamline/PostSL teardown. In this GTA path, however, the enabled `ffxConfigure` never became visible through CE's GetProcAddress/IAT hook path, so `g_ProtectedOfficialFFXStartupSwapchainPending` stayed true for thousands of ProcessFrame/ECL calls. That kept overlay/capture/FFX retry side effects quiesced indefinitely and prevented the existing FFX present-callback-stall fallback from restoring normal overlay rendering.
- **Fix**:
  - Added protected-official-FFX startup progress counters and `ShouldFinalizeProtectedOfficialFFXStartupAfterSustainedFrameProgress(...)`. The initial fragile startup is still protected, but sustained real ProcessFrame/ECL progress now graduates the path when direct configure proof never arrives.
  - The progress fallback finalizes the authoritative FFX takeover, sets FSR active/support state, clears deferred Streamline/PostSL ownership, arms a `progress-resolved official FFX runtime-owned Present path assumption`, and clears native-FSR startup arming without pretending direct FFX API confirmation happened.
  - `HookHasRuntimeOwnedNativeFGPresentPath()` and `DX12_IsRuntimeOwnedSwapchainActiveForFrameGeneration()` now respect that progress-resolved assumption, and the FFX present-callback stall detector can fall back based on the assumption timestamp even when no runtime-owned queue was captured.
  - The assumption is cleared on real native-FSR OFF/configure, swapchain return to `origGame`, Streamline comeback/off cleanup, runtime-mode FSR cleanup, stale FSR cleanup, and explicit startup-arming clear paths.
  - `DetectSLPresentHook` trampoline-byte diagnostics are now throttled so native-FSR-owned sessions no longer spam `hook_debug.log` with an unbounded trampoline byte dump on every suppressed SL-routing check.
- **Regression tests**: Added `DXGISharedTest.ProtectedOfficialFFXStartupCanResolveAfterSustainedProgressWithoutDirectConfigure`; extended `CrashHandlerBinaryTest.HookDllContainsLazyExecRegressionStrings` for the progress fallback and progress-resolved Present-path assumption diagnostics.
- **Verification**: `python build.py --skip-updates` passed (build `0.1.3377`). Focused `python build.py --no-build --run-tests --skip-updates --gtest-filter="DXGISharedTest.*FSR*:DXGISharedTest.*FFX*:DXGISharedTest.*Bypass*:DXGISharedTest.*Streamline*:DXGISharedTest.*PostSL*:CrashHandlerBinaryTest.*:CrashDumpPolicyTest.*:StreamlineRuntimePolicyTest.*:FFXApiParsingTest.*:FFXHookValidationTest.*"` passed 246/246 tests (displayed metadata `0.1.3378`). Full `python build.py --no-build --run-tests --skip-updates` passed 780/780 tests (displayed metadata `0.1.3379`).
- **Historical only**: Do not use this entry's sustained-progress finalization as an expected healthy path. Current validation should require enabled `ffxConfigure` / FFX callback evidence for takeover side effects, or staged Direct-queue overlay-only rendering while protected startup remains pending.

### 2026-05-20 - GTA official FFX startup pass-through and inline fatal-exit dump hook (build 0.1.3362 / tests 0.1.3364)

- **Input**: GTA V Enhanced DX12 logs at `installed/captureengine/logs/20260520_012459`; startup with FSR FG activated still exited back to desktop with no CE session `.dmp`.
- **Root cause**: The trace exited with `0xC0000409` immediately after `amd_fidelityfx_dx12.dll` created a runtime-owned startup swapchain behind EOS and before any `ffxConfigure` line. CE was still doing too much on the create-swapchain return path: refreshing Present hooks, capturing/claiming queue ownership, inspecting official AMD FFX exports, and staging Streamline/PostSL teardown before the AMD runtime had accepted the enabled FSR configure. The existing fatal-exit dump hooks were import/dynamic-hook based and did not fire for this direct fast-fail/termination path.
- **Fix**:
  - Added `ShouldProtectOfficialFFXStartupSwapchainCreateFromCESideEffects(...)` and wired the DX12 create-swapchain hooks so official AMD FFX startup swapchain creation is a protected pass-through until direct enabled FFX API confirmation arrives.
  - The protected path latches native-FSR startup arming and FSR history, counts as runtime-owned for the first disabled startup-arming `ffxConfigure`, suppresses separate injected overlay GPU work while the runtime-owned native-FG path is pending, and returns before Present-hook refresh, queue-ownership mutation, FFX export inspection, or Streamline/PostSL teardown.
  - Enabled `ffxConfigure` now finalizes the protected official startup path and applies the authoritative FFX takeover side effects only after the AMD runtime has produced direct enabled-configure proof.
  - The fatal-exit dump installer now also inline-hooks loaded fail-fast/termination exports (`RaiseFailFastException`, `TerminateProcess`, `ExitProcess`, `RtlExitUserProcess`, `NtTerminateProcess`) so direct calls that bypass IAT patching still get one supplemental dump attempt.
- **Regression tests**: Added protected-startup pass-through coverage (current successor: `DXGISharedTest.ProtectedOfficialFFXStartupQuiescesCESideEffectsUntilDirectConfigure`), expanded native-FSR overlay-suppression coverage for a protected native-FG present path before queue capture, and extended `CrashHandlerBinaryTest.HookDllContainsLazyExecRegressionStrings` for the protected FFX and inline fatal-exit diagnostics.
- **Verification**: `python build.py --skip-updates` passed (build `0.1.3362`). Focused `python build.py --no-build --run-tests --skip-updates --gtest-filter="DXGISharedTest.*FSR*:DXGISharedTest.*FFX*:DXGISharedTest.*Bypass*:DXGISharedTest.*Streamline*:DXGISharedTest.*PostSL*:CrashHandlerBinaryTest.*:CrashDumpPolicyTest.*:StreamlineRuntimePolicyTest.*:FFXApiParsingTest.*:FFXHookValidationTest.*"` passed 243/243 tests (displayed metadata `0.1.3363`). Full `python build.py --no-build --run-tests --skip-updates` passed 776/776 tests (displayed metadata `0.1.3364`).
- **Manual validation still needed**: Re-run GTA V Enhanced with FSR FG active at startup and through `off <-> FSR`, `off <-> DLSS`, and `FSR <-> DLSS` switching. Expected logs should show `Protected official FFX startup swapchain pass-through` before the first enabled configure, no immediate `0xC0000409` exit, and if another crash-like direct exit happens, `FatalExitDump: Installed inline pre-termination hook` plus one pre-termination supplemental dump attempt.

### 2026-05-19 - GTA native-FSR re-enable startup-arming and fatal-exit dump fix (build 0.1.3347 / tests 0.1.3349)

- **Input**: GTA V Enhanced DX12 logs at `installed/captureengine/logs/20260519_191243`; startup with FSR FG on worked, switching all FG off worked, Reflex on worked, but switching FSR FG on again ended the process with exit `0xC0000409` and no session `.dmp`.
- **Root cause**: The previous startup-arming fix preserved FSR state for the first disabled `ffxConfigure(frameGenerationEnabled=0)` after a fresh runtime-owned FFX takeover, but it also installed CE's present-callback bridge on that disabled setup packet. The failing trace ended immediately after `enabled=0 startupArming=1 originalPresent=null resolvedPresent=null`, so GTA/FFX appears to fail-fast if CE mutates that disabled startup packet with a synthetic callback before the real enabled configure is accepted.
- **Fix**:
  - `ShouldInstallFFXPresentCallbackBridgeForConfigure(...)` is again enabled-configure-only. Disabled teardown packets and disabled startup-arming packets are forwarded unmodified.
  - `Hooked_ffxConfigure` still treats a qualified disabled startup-arming packet as setup, preserving authoritative FSR state until a direct enabled FFX configure arrives. New diagnostics distinguish this path with `Native FSR disabled startup-arming configure forwarded without CE present-callback bridge`.
  - Added a narrow pre-termination dump guard for current-process `RaiseFailFastException`, `TerminateProcess`, and `ExitProcess` crash-like exit codes. It captures at most one supplemental CE dump through the existing temp/rename writer, ignores normal exits and CE's own external-dump-storm termination code, then forwards the original API.
- **Regression tests**: Updated `DXGISharedTest.FFXPresentCallbackBridgeInstallsOnlyForEnabledFrameGenerationConfigure`; added `CrashDumpPolicyTest.PreTerminationDumpCapturesOnlyCurrentProcessCrashLikeExitCodesOnce`; extended the hook-DLL binary regression strings for the disabled-startup-arming and fatal-exit diagnostics.
- **Verification**: `python build.py --skip-updates` passed (build `0.1.3347`). Focused `python build.py --no-build --run-tests --skip-updates --gtest-filter="DXGISharedTest.*FSR*:DXGISharedTest.*NativeFSR*:DXGISharedTest.*Bypass*:DXGISharedTest.*PostSL*:DX12FGTraceReplayFixture.*:CrashHandlerTest.*:CrashHandlerBinaryTest.*:CrashDumpPolicyTest.*:StreamlineRuntimePolicyTest.*"` passed 198/198 tests (displayed metadata `0.1.3348`). Full `python build.py --no-build --run-tests --skip-updates` passed 771/771 tests (displayed metadata `0.1.3349`).
- **Manual validation still needed**: Re-run GTA V Enhanced through `FSR on -> off -> Reflex on -> FSR on` and pairwise FG transitions. Expected logs should show the disabled startup-arming packet forwarded without bridge mutation, then an enabled FFX configure installing the callback bridge, with no `0xC0000409` process exit. If another fail-fast path happens, expected `FatalExitDump` diagnostics and a supplemental `crash_external_fatal_exit_*.dmp` should appear in the session folder.
