# llm-wiki Log — Archive 2026-W22a

### 2026-05-31 - Confirmed PostSL backend survives post-FSR DLSS swapchain churn (build 0.1.3626 / tests 0.1.3627)

- **Input**: GTA session `installed/captureengine/logs/20260531_232108` crashed while switching from FSR FG to DLSS FG. Small dumps landed on GTA's breakpoint/dialog path; the freeze dump sampled `user32!MessageBoxW` with the visible `ERR_GFX_STATE` dialog, so the actionable failure was CE perturbing the graphics-state handoff rather than a direct CE access violation.
- **Root cause**: The handoff had already proven a valid PostSL overlay route: CE rendered 10 successful overlay submissions on Streamline's runtime-owned queue after FSR, with `hadFSR=1`, `slFG=1`, and PostSL confirmed. A later swapchain-pointer churn inside the first confirmed PostSL warmup frames then took the ordinary swapchain-change cleanup path. That path shut down the descriptor-free backend, cleared overlay init/PostSL state, reset startup activation, armed FG cooldown, and temporarily hid the overlay. In GTA this destroyed the only proven-safe DLSS/PostSL route during the fragile FSR-to-DLSS handoff and was followed by `ERR_GFX_STATE`.
- **Fix**:
  - `hook/common/dx12_overlay_policy.h` now models confirmed PostSL backend warmup protection using the same 30-frame proof threshold as stale-OFF and PostSL fallback guards.
  - `hook/apis/dx12_hook.cpp` now preserves the confirmed PostSL backend across the narrow active FSR-to-DLSS swapchain-change case: Streamline FG is running, PostSL has already rendered, the session had an FSR phase, the swapchain is runtime-owned, and the runtime-owned queue differs from the original game queue. CE clears only stale cached swapchain pointers and logs `DX12: Preserving confirmed PostSL backend during active FSR->DLSS swapchain handoff`.
  - Focused unit coverage locks both the warmup threshold and every required gate for the preservation rule.
- **Validation**:
  - `python build.py --skip-updates` passed (build `0.1.3626`).
  - `python build.py --no-build --run-tests --skip-updates` passed all 836 tests (displayed metadata `0.1.3627`).
  - Local injected `dx12_fg_switch_test.exe` session `installed/captureengine/logs/20260531_233633` exited cleanly with no dumps. The app log reached real DLSS-G (`Mode now DLSS FG`, `slDLSSGGetState ... genFrames=2`) and then returned to FSR, while CE's hook log did not record `Post-SL overlay SUBMIT`; treat that run as app-side mixed-runtime validation plus FSR callback overlay validation, not as a full PostSL overlay-route reproduction of the GTA crash.
- **Logging lesson**: For mixed FG testing, app-side FG success and CE overlay-route success are separate evidence. The test app log proves SDK/runtime state (`Mode now DLSS FG`, generated-frame counts); the hook log proves CE rendering transport (`Post-SL overlay SUBMIT` or `FFX present callback rendered overlay`). Future validation should require both when the target bug is an overlay-route bug.
- **Source anchors**: `hook/apis/dx12_hook.cpp`, `hook/common/dx12_overlay_policy.h`, `tests/test_dxgi_shared.cpp`, `installed/captureengine/logs/20260531_232108/hook_debug.log`, `installed/captureengine/logs/20260531_233633/hook_debug.log`, `installed/testapp/dx12_fg_switch_test.log`.

### 2026-05-31 - FFX callback composition and native-FSR Steam overlay routing (build 0.1.3624 / tests 0.1.3625)

- **Inputs**:
  - GTA `installed/captureengine/logs/20260531_230629_gtafsrfgblackwindowcontent` exited normally but showed a black game image with only CE's overlay visible after FSR FG enabled. The FFX callback bridge logged `originalPresent=0`, `resolvedPresent=0`, and `preserving runtime output without fallback-copy` while rendering overlay onto `outputSwapChainBuffer`.
  - Talos `installed/captureengine/logs/20260531_230835_talosfsrfg` ran with native FSR and CE overlay visible, but Steam overlay appeared only briefly at startup and then disappeared. The log showed Streamline loaded, Streamline FG not running, native FSR active, and repeated `using DXGI bypass without direct Steam hook invoke` lines.
- **Root causes**:
  - Historical conclusion, superseded 2026-06-02: this entry assumed that if CE replaces a null FFX present callback, CE becomes responsible for the official callback finalization step. GTA `20260602_161000_gtafreezestartwithfsrfg` later proved that enabled null-callback configures should preserve AMD's internal no-callback composition instead of synthesizing CE's own callback. Still-current part: active native-FSR callback frames must not mirror overlay work back into `currentBackBuffer`, and app/default-composed output should still be preserved when a real callback exists.
  - The Steam forced-bypass rule treated `Streamline loaded && Streamline FG not running` as bypass-only. That is still the safe no-FG startup default, but native FSR active is an FG-owned presentation path too, so Steam needs the guarded Present invocation opportunity before CE falls back to the disk-bytes bypass trampoline.
- **Fixes**:
  - Superseded 2026-06-02 for null-callback configures: `ShouldComposeFFXPresentSourceToOutput(...)` was introduced for the synthetic no-original callback path, but current code no longer installs that synthetic callback for enabled native FSR with `presentCallback=null`.
  - Still current for real callback bridges: `hook/apis/dx12_hook.cpp` separately logs preservation of app/default-composed runtime output when an original callback exists.
  - `DXGIShared::ShouldInvokeGuardedSteamPresentDuringForcedBypass(...)` now accepts `nativeFSRPresentationActive`; `CallOriginalPresent()` derives it from runtime mode, FSR API-active state, and runtime-owned native-FG presentation state, then gives Steam the guarded path during native FSR before bypass fallback.
- **Validation**:
  - `python build.py --skip-updates` passed (build `0.1.3624`). SafeDelete reported one locked old overlay DLL and scheduled it for deletion on reboot, but the build completed.
  - `python build.py --no-build --run-tests --skip-updates` passed all 834 tests (displayed metadata `0.1.3625`).
- **Manual validation note, superseded for null callbacks**: Re-run GTA with FSR FG; healthy enabled null-callback logs should now show AMD-internal no-callback routing rather than CE's current-to-output synthetic callback composition. Re-run Talos with FSR FG and Steam overlay; healthy logs should show guarded Steam invocation during native FSR instead of the bypass-only message.
- **Source anchors**: `hook/apis/dx12_hook.cpp`, `hook/common/dx12_overlay_policy.h`, `hook/common/dxgi_shared.cpp`, `hook/common/dxgi_shared.h`, `tests/test_dxgi_shared.cpp`, `installed/captureengine/logs/20260531_230629_gtafsrfgblackwindowcontent/hook_debug.log`, `installed/captureengine/logs/20260531_230835_talosfsrfg/hook_debug.log`.

### 2026-05-31 - Native-FSR callback output preservation and no-blank Social handoff refinement (build 0.1.3622 / tests 0.1.3623)

- **Input**: GTA `installed/captureengine/logs/20260531_185019` had no crash dump and exited normally, but still showed two regressions: CE's overlay briefly disappeared when Rockstar/EOS created a Streamline-adjacent no-FG swapchain after the overlay was already stable, and FSR FG showed visible ghosting only with CE injected.
- **Root causes**:
  - The runtime-inactive Streamline handoff was already being preserved, but CE then submitted startup overlay resource priming and deliberately delayed the first overlay draw for `100ms`. That was the remaining visible blink.
  - During active native FSR with `originalPresent=0`, CE fallback-copied `currentBackBuffer` into `outputSwapChainBuffer` on non-generated callback frames. The FFX callback output is already the runtime-selected present surface, so the fallback copy can trample FSR composition/history and produce temporal ghosting.
- **Fixes**:
  - `hook/common/dx12_overlay_policy.h` now makes startup resource priming and the post-prime settle delay conditional on not preserving a live runtime-inactive Streamline handoff.
  - `hook/apis/dx12_hook.cpp` now skips startup priming on that live handoff path and logs `Skipping startup resource priming delay because live overlay is preserved through runtime-inactive Streamline handoff`.
  - **Superseded/refined by build `0.1.3624`, then superseded again for null-callback configures on 2026-06-02**: this fallback-copy rule was too broad for `originalPresent=0`. The still-current rule is to preserve app/default-composed output when a real callback already ran; enabled null-callback configures now preserve AMD's internal no-callback composition instead of installing CE's own callback.
  - Added `DXGISharedTest.LiveStartupOverlayHandoffSkipsResourcePrimingBlank` and extended `DXGISharedTest.FFXPresentCallbackFallbackCopyOnlyRunsWithoutRuntimeCompositionCallback` to lock the native-FSR no-copy rule.
- **Validation**:
  - `python build.py --skip-updates` passed (build `0.1.3622`).
  - `python build.py --no-build --run-tests --skip-updates` passed all 834 tests (displayed metadata `0.1.3623`).
- **Manual validation note**: Re-run GTA startup with Rockstar Social and GTA FSR FG. Healthy startup logs should no longer show a resource-prime delay immediately after a preserved live no-FG handoff. For active native-FSR callback output, use the latest 2026-06-02 rule: enabled null-callback configures keep AMD internal composition; app/default-composed callbacks preserve output.
- **Source anchors**: `hook/apis/dx12_hook.cpp`, `hook/common/dx12_overlay_policy.h`, `tests/test_dxgi_shared.cpp`, `installed/captureengine/logs/20260531_185019/hook_debug.log`.

### 2026-05-31 - Runtime-inactive Streamline handoff no longer blanks settled DX12 overlay (build 0.1.3620 / tests 0.1.3621)

- **Inputs**:
  - GTA `installed/captureengine/logs/20260531_151041_gtacrashondlssfgtofsrfg` froze while switching from DLSS FG to FSR FG. The dump sampled AMD's FSR presenter thread in `amd_fidelityfx_dx12!ffxQuery`; logs showed CE's FFX callback bridge with `originalPresent=0`, generated-frame fallback copy, and active-frame mirroring into `currentBackBuffer`.
  - Talos `installed/captureengine/logs/20260531_150850_talosfg` and `installed/captureengine/logs/20260531_143627` showed protected official FFX startup could remain stable but briefly hide the overlay while waiting for enabled `ffxConfigure`.
  - GTA startup `installed/captureengine/logs/20260531_182455` had no FG yet. After the overlay was already stable, Social/EOS created a Streamline-adjacent no-FG swapchain; CE treated it as a fresh authoritative runtime takeover, shut down the live DescFree backend, re-armed startup compatibility, deferred overlay work, and the overlay disappeared until stale cleanup restored the original queue.
- **Root causes**:
  - Generated FFX callback frames already contain runtime output, so copying `currentBackBuffer` over `outputSwapChainBuffer` can destroy generated output and wedge the runtime. Active native-FSR frames also must not mirror overlay work into `currentBackBuffer`; that buffer is runtime input except during explicit suspension/teardown.
  - Historical visibility hypothesis, superseded 2026-06-02: the protected official FFX startup path seemed too absolute for visibility, and the then-current conclusion was that an already drawable overlay could render safely if the protected create-swapchain call staged the runtime's Direct queue. GTA later disproved that pre-enable overlay-only route.
  - The Social/EOS GTA startup case was not a DLSS-G activation. Once startup compatibility had already settled and no explicit FG signal had appeared, the late Streamline/no-FG runtime-owned handoff should preserve the live single-queue overlay backend instead of blanking and reinitializing it.
- **Fixes**:
  - FFX callback fallback copy skipped generated frames, and overlay mirroring to `currentBackBuffer` was limited to explicit native-FSR suspension/teardown. **Superseded/refined by build `0.1.3624`**: no-original callbacks must compose `currentBackBuffer` into `outputSwapChainBuffer` even for generated frames; the still-current part is that active native-FSR callback frames should show `mirroredCurrent=0` unless explicitly suspended.
  - First enabled `ffxConfigure` now latches FSR API-active state before notifying DX12 so protected official FFX startup can finalize immediately. The old staged-queue overlay-only part of this fix is superseded: current protected startup keeps separate overlay GPU work suppressed until enabled configure/callback proof.
  - Runtime-inactive Streamline/no-FG startup handoffs now preserve the live overlay backend when startup compatibility had already settled, the overlay backend is ready, no explicit `slDLSSGSetOptions(ON)` edge or other FG activity was observed, and the original game queue is available. CE clears stale cached swapchain references, skips startup re-arm/deferral, and routes through the original game queue until a real FG signal appears.
  - Added policy coverage for generated FFX callback copy suppression, FSR suspension-only mirroring, protected FFX overlay-only admission, and settled startup overlay preservation across runtime-inactive Streamline handoffs.
- **Validation**:
  - `python build.py --skip-updates` passed (build `0.1.3620`).
  - `python build.py --no-build --run-tests --skip-updates` passed all 833 tests (displayed metadata `0.1.3621`).
  - Process cleanup after the GTA startup analysis stopped lingering `captureengine.exe` / `GTA5_Enhanced.exe`; a follow-up process check found no matching CaptureEngine, GTA, Talos, switch-app, dx12-test, or unit-test processes.
- **Manual validation still useful**: Re-run GTA startup with Rockstar Social, GTA `DLSS FG -> FSR FG`, and Talos FG switching. Healthy GTA startup logs should show `Keeping settled startup overlay live through runtime-inactive Streamline handoff`, `Fresh authoritative Streamline no-FG handoff preserved live overlay backend`, and no startup-overlay blackout/reinit sequence. Healthy native-FSR switching should show callback overlay renders without generated-frame fallback copy or active-frame current-buffer mirroring.
- **Source anchors**: `hook/apis/dx12_hook.cpp`, `hook/apis/ffx_hook.cpp`, `hook/common/dx12_overlay_policy.h`, `tests/test_dxgi_shared.cpp`, `tests/test_crash_handler.cpp`, `installed/captureengine/logs/20260531_151041_gtacrashondlssfgtofsrfg`, `installed/captureengine/logs/20260531_150850_talosfg`, `installed/captureengine/logs/20260531_182455/hook_debug.log`.

### 2026-05-31 - Protected FFX startup queue staging reduces Talos FG-switch overlay gaps (build 0.1.3614 / tests 0.1.3615)

- **Input**: Talos session `installed/captureengine/logs/20260531_143627` no longer crashed, but the overlay briefly disappeared while switching between DLSS FG, FSR FG, and all-FG-off. The decisive trace was the second official FFX startup handoff: CE correctly protected AMD's startup swapchain from normal side effects for several seconds, then enabled `ffxConfigure` arrived and CE logged `Finalizing protected official FFX startup pass-through ... (no queue captured before configure)`, immediately followed by `FSR FG active but scQueue=null, SKIPPING overlay`.
- **Root cause**: The protected official-FFX create-swapchain path returned before `CaptureSwapchainQueueFromCreateDevice(...)`, which preserved AMD stability but discarded the FSR runtime's Direct queue. When the later enabled configure made rendering safe again, the FFX callback route could eventually initialize from fallbacks, but the normal/runtime-owned routing state had a blind window because `g_SwapchainQueue` was still null.
- **Fix**:
  - Protected official FFX startup now QIs the create-swapchain `pDevice` for a Direct `ID3D12CommandQueue` and stores that pointer only as deferred takeover data. Later builds refined this twice: build `0.1.3620` temporarily allowed overlay-only rendering on that staged Direct queue, but the 2026-06-02 GTA freeze rejected that pre-enable render path. The current rule is to retain the staged queue only for post-proof takeover while Present-hook refresh, queue-ownership mutation, FFX export inspection, capture, hook retry, probes, progress-only graduation, and separate overlay GPU work remain suppressed until enabled `ffxConfigure` / callback proof.
  - Enabled native-FSR configure now applies the staged runtime queue through `DX12_SetSwapchainQueue(..., authoritativeFFXRuntimeQueue=true)` before clearing Streamline/PostSL ownership, so the safe post-configure route should no longer log `scQueue=null` for a staged FFX startup.
  - Added route-aware overlay visibility diagnostics (`normal`, `post-sl`, `ffx-present-callback`) so future logs can distinguish a deliberate protected-startup hold from an unexpected loss of all render routes.
  - Added `DXGISharedTest.ProtectedOfficialFFXStartupStagesOnlyDirectQueuesForDeferredTakeover`.
- **Validation**:
  - `python build.py --skip-updates` passed (build `0.1.3614`).
  - `python build.py --no-build --run-tests --skip-updates` passed all 831 tests (displayed metadata `0.1.3615`).
  - Process cleanup check found no lingering `captureengine.exe`, `dx12_fg_switch_test.exe`, `dx12_test.exe`, `Talos1-Win64-Shipping.exe`, `GTA5_Enhanced.exe`, or `unit_tests.exe`.
- **Manual validation still useful**: Re-run Talos FG switching. A healthy protected-FSR handoff should show `Protected official FFX startup staged runtime queue`, `Finalizing staged official FFX takeover after enabled ffxConfigure`, `FFX swapchain takeover applied staged runtime queue ... applied=1`, and no post-configure `FSR FG active but scQueue=null, SKIPPING overlay`.
- **Source anchors**: `hook/apis/dx12_hook.cpp`, `hook/common/dx12_overlay_policy.h`, `tests/test_dxgi_shared.cpp`, `installed/captureengine/logs/20260531_143627/hook_debug.log`.

### 2026-05-31 - Steam DX12 null-callback recovery slot hardening (build 0.1.3612 / tests 0.1.3613)

- **Inputs**:
  - Strange Brigade DX12 session `installed/captureengine/logs/20260531_141812_strangebrigadedx12crash` crashed with `RIP=0` inside `gameoverlayrenderer64!OverlayHookD3D3`. The dump showed the second Steam E9 path calling through a NULL slot at `steam+0x162200`, after the earlier init path had already patched the legacy `steam+0x1621d8` slot.
  - Talos session `installed/captureengine/logs/20260531_141924_talossteamoverlaydoesnotwork` stayed alive but never showed the Steam overlay. The log showed CE skipping guarded Steam Present every frame because the legacy slot contained `SteamDummyRenderingCallback`, so the 2026-05-30 dummy suppression avoided a crash but also made visible Steam overlay rendering unreachable.
- **Root cause**: Steam's current DX12 overlay has multiple lazy Present-shaped callback slots, and the exact slot can move with Steam builds. A hardcoded `0x1621d8` recovery slot is incomplete. Also, patching a Present-shaped callback to a no-op dummy prevents Steam from presenting/chaining; a bypass trampoline is the correct safe replacement when available.
- **Fix**:
  - `SteamOverlayInitVehHandler` now resolves the exact NULL slot from the faulting Steam call site (`mov rax,[rip+disp]` followed by `call rax`) before falling back to the legacy RVA.
  - The recovered slot is patched to CE's DXGI bypass Present when that trampoline exists; `SteamDummyRenderingCallback` is fallback-only.
  - The non-Streamline Steam steady E9 Present path is now wrapped in the same scoped null-callback VEH guard as the first init and guarded Streamline/Steam path.
  - Added `SelectSteamNullCallbackRecoveryPatchTarget(...)` policy coverage so tests lock "bypass over dummy" as the preferred recovery target.
- **Validation**:
  - Analyzed `external_StrangeBrigade_DX12-20260531-141852-790-9132-8612_5e6f1b57.dmp` with cdb using the project symbol path and confirmed the NULL slot at `steam+0x162200`.
  - `python build.py --skip-updates` passed (build `0.1.3612`).
  - `python build.py --no-build --run-tests --skip-updates` passed all 830 tests (displayed metadata `0.1.3613`).
  - Process cleanup check found no lingering `captureengine.exe`, `dx12_fg_switch_test.exe`, `dx12_test.exe`, `StrangeBrigade_DX12.exe`, `Talos1-Win64-Shipping.exe`, or `unit_tests.exe`.
- **Manual validation still useful**: Re-run Strange Brigade DX12 and Talos with Steam overlay enabled. Healthy logs should show `SteamOverlayInitVehHandler: Patched NULL callback ... -> DXGIBypassPresent` with `dynamicSlot=1` if Steam uses the newer slot, no crash dumps, CE overlay remains visible, and the Steam overlay can appear instead of repeated dummy/bypass skips.
- **Source anchors**: `hook/common/dxgi_shared.cpp`, `hook/common/dxgi_shared.h`, `tests/test_dxgi_shared.cpp`, `installed/captureengine/logs/20260531_141812_strangebrigadedx12crash`, `installed/captureengine/logs/20260531_141924_talossteamoverlaydoesnotwork`.

### 2026-05-30 - Steam dummy callback bypass and FFX configure fallback (build 0.1.3610 / tests 0.1.3611)

- **Inputs**:
  - `installed/captureengine/logs/20260530_234519` showed the switch app's direct `OFF -> FSR FG` path entering protected official FFX startup and then staying quiesced forever. The app log proved FSR itself was active and firing callbacks, but CE never saw direct `ffxConfigure`, so the overlay callback bridge never armed.
  - `installed/captureengine/logs/20260530_234403_taloscrashnofg` showed Talos UE5 DX12 with FG disabled crashing during Streamline/Steam startup. Dumps landed at `RIP=0x1` through `gameoverlayrenderer64` / `sl_common` / `sl_dlss_g` after CE had already patched Steam's null renderer callback slot to `SteamDummyRenderingCallback`.
- **Root causes**:
  - Some official FFX integrations can call `ffxConfigure` through SDK dispatch tables or intra-module paths that bypass both `GetProcAddress` and caller IAT patching. The older progress-only FFX startup fallback was unsafe, so CE needed a narrow real-configure interception fallback instead of graduating protected startup on frame progress.
  - Steam's disabled/uninitialized renderer slot is not a real overlay renderer after CE patches it to the dummy callback. Re-entering Steam's Present hook while that slot still points at CE's dummy can drive the Steam/Streamline hook chain into a partial renderer path and crash.
- **Fixes**:
  - Official AMD DX12 FFX modules still skip standard inline JMP hooks, but `ffxConfigure` now also arms the existing guarded re-arming int3/VEH fallback in addition to IAT/dynamic routing. This catches SDK dispatch-table and intra-module configure calls while still restoring the original byte before forwarding and re-arming after return.
  - Guarded Steam DX12 Present invocation now checks the Steam callback-slot state. Null is allowed only when the one-shot null-callback recovery guard is installed; CE's dummy callback and invalid low-address sentinels use the DXGI bypass trampoline until Steam exposes a real renderer callback. A real Steam renderer callback remains eligible, preserving the visible Steam overlay case.
- **Validation**:
  - `python build.py --skip-updates` passed (build `0.1.3610`).
  - `python build.py --no-build --run-tests --skip-updates` passed all 829 tests (displayed metadata `0.1.3611`).
  - Injected switch-app session `installed/captureengine/logs/20260530_235921` ran `dx12_fg_switch_test.exe 1280 720 20 --duration 28 --fsr-suspend-interval 2 --startup-recreates 1 --bootstrap-native-swaps 1` to clean exit with no dumps and no lingering `captureengine.exe` / `dx12_fg_switch_test.exe`. It logged protected FFX startup finalization after enabled `ffxConfigure`, FFX callback overlay rendering while active and suspended, 97 PostSL overlay submits in DLSS mode, and no device-removal markers.
- **Manual validation still useful**: Re-run Talos no-FG launch with Steam overlay disabled/enabled to confirm the dummy-callback bypass prevents the crash while real Steam overlay rendering remains visible when Steam provides a real callback.
- **Source anchors**: `hook/common/dxgi_shared.h`, `hook/common/dxgi_shared.cpp`, `hook/common/ffx_api_parsing.h`, `hook/apis/ffx_hook.cpp`, `tests/test_dxgi_shared.cpp`, `tests/test_ffx_api_parsing.cpp`, `installed/captureengine/logs/20260530_234403_taloscrashnofg`, `installed/captureengine/logs/20260530_234519`, `installed/captureengine/logs/20260530_235921/hook_debug.log`, `installed/testapp/dx12_fg_switch_test.log`.

### 2026-05-30 - x86 DX12 focus-loss overlay-fence pacing (build 0.1.3608 / tests 0.1.3609)

- **Input**: Follow-up 32-bit DX12 Alt+Tab/focus-loss session `installed/captureengine/logs/20260530_231554` still froze after the previous `DO_NOT_WAIT` removal. The log showed D3D12 focus loss now preserving Present pacing, but unfocused Present calls still arrived around 1 ms until `DX12: GPU device removed (0x887A0006)`. The freeze dump sampled the render thread in `capture_hook_x86!DX12_WaitForOverlayCompletion` after device removal, with a hot `GetModuleHandleA`/loader-log path from startup-overlay module probing.
- **Root cause refinement**: Removing D3D12 `DO_NOT_WAIT` avoided the most obvious unbounded Present loop, but wrapped x86 D3D12 still needed an overlay-side pacing point when the process was no longer foreground. In the ordinary single-queue, no-FG case CE was returning from `DX12_WaitForOverlayCompletion()` without waiting, so the app could keep submitting CE overlay command lists faster than the unfocused presentation path drained. The fix should preserve the visible overlay, not reintroduce overlay hiding/reinitialization.
- **Fixes**:
  - `ShouldWaitForOverlayCompletion(...)` now accepts `processHasForeground` and returns true for single-queue D3D12 focus-loss sessions, while still skipping NVIDIA Smooth Motion.
  - `DX12_WaitForOverlayCompletion()` now computes foreground ownership before startup-overlay module probing, skips that module lookup while the process is not foreground, and logs wait mode as `focus-loss`, `single-queue`, startup overlay module name, or `dedicated-queue`.
  - Wrapped D3D12 `Present` and `Present1` now explicitly flush CE's deferred overlay fence signal immediately after the real Present returns, and log device-lost HRESULTs returned by wrapped D3D12 Present paths.
- **Validation**:
  - `python build.py --skip-updates` passed (build `0.1.3608`).
  - `python build.py --no-build --run-tests --skip-updates` passed all 828 tests (displayed metadata `0.1.3609`).
  - Focused regression coverage added `DXGISharedTest.DX12OverlayWaitPolicyPacesSingleQueueFocusLoss` and `DXGISharedTest.D3D12PresentPathsFlushDeferredOverlaySignalAfterPresent`.
  - Process cleanup check found no lingering `captureengine.exe`, `dx12_test.exe`, or `unit_tests.exe`.
- **Superseded manual target**: This deferred-signal validation target was not sufficient in later `20260601_*` and `20260602_*` runs. Current x86 DX12 focus-loss validation should use the v7 background plus foreground-reacquire backbuffer-hold breadcrumbs from the 2026-06-02 entry above.
- **Source anchors**: `hook/common/dx12_overlay_policy.h`, `hook/apis/dx12_hook.cpp`, `hook/wrappers/dxgi_swapchain_wrap.cpp`, `tests/test_dxgi_shared.cpp`, `installed/captureengine/logs/20260530_231554/hook_debug.log`, `installed/captureengine/logs/20260530_231554/dx12_test.exe_FREEZE_2026-05-30_23-16-39_893.dmp`.

### 2026-05-30 - DX10 overlay route and x86 DX12 focus-loss pacing fix (build 0.1.3606 / tests 0.1.3607)

- **Inputs**:
  - DX10 no-overlay session `installed/captureengine/logs/20260530_225502_dx10testappnooverlay/hook_debug.log` showed D3D10 hooks installed, `Wrapper: D3D10CreateDevice called`, wrapped D3D10 device creation, and DXGI `DetourPresent` traffic, but no DX10 overlay initialization/rendering lines.
  - x86 DX12 freeze session `installed/captureengine/logs/20260530_225544_32bitdx12testappfreezeonalttab` showed focus loss at `Present#256`, then repeated `SyncInterval 1->0 + DO_NOT_WAIT`, then `DX12: GPU device removed (0x887A0006)`. The small freeze dump put the render thread in `win32u!NtGdiDdDDICreateAllocation -> D3D12Core!CCommandQueue<0>::ExecuteCommandLists -> capture_hook_x86!DetourExecuteCommandLists -> dx12_test!Render`. The larger dump sampled the render thread in CE's percentile FPS calculation during `ProcessFrame`, making that path worth hardening even though the Present-pacing loop was the primary failure.
- **Root causes**:
  - `DXGIShared::DetourPresent` and `DetourPresent1` classified the swapchain as D3D10, but only routed D3D11 swapchains into `HandleDX11ProcessFrame()`. D3D10 therefore published FG/off state but never reached `DrawDX10Overlay()`.
  - The DX12 wrapper applied the unfocused flip-model workaround to D3D12 too, forcing `SyncInterval=0` and `DXGI_PRESENT_DO_NOT_WAIT`. In the 32-bit DX12 test app this removed DXGI pacing while the app kept submitting command lists, creating a high-rate unfocused ECL/Present loop and eventual device hang/removal.
- **Fixes**:
  - Added `DXGIShared::ShouldRunSharedD3D10Or11ProcessFrame()` and now route D3D10 plus D3D11 through the shared DX10/DX11 frame path from both `Present` and `Present1`. DX10 perf CSV labels now report `DX10`.
  - `DrawDX10Overlay()` now logs device/RTV failures, explicitly creates and binds a swapchain backbuffer RTV around overlay rendering, preserves/restores prior OM state, and reuses `g_mainRenderTargetView10` so resize cleanup releases CE's backbuffer reference before `ResizeBuffers`.
  - Added `DXGIShared::ShouldApplyUnfocusedFlipModelDoNotWait()`: D3D12 focus loss preserves game Present pacing and logs `preserving Present pacing (D3D12 focus-loss safety)`, while compatible non-DX12 unfocused flip-model paths retain the old throttle guard.
  - `PerformanceMetrics::Get1PercentLowFPS()` / `Get01PercentLowFPS()` now use a fixed-size snapshot plus `nth_element` instead of allocating a vector and fully sorting on the overlay hot path.
- **Validation**:
  - `python build.py --skip-updates` passed (build `0.1.3606`).
  - `python build.py --no-build --run-tests --skip-updates` passed all 826 tests (displayed metadata `0.1.3607`).
  - Focused regression coverage added `DXGISharedTest.D3D10UsesSharedD3D10D3D11ProcessFramePath`, `DXGISharedTest.D3D12FocusLossPreservesPresentPacing`, and `PerformanceMetricsTest.LowPercentilesUseWorstFrameTimesWithoutHeapSortDependency`.
- **Manual validation still useful**:
  - Re-run injected `dx10_test.exe` and confirm `DX10: OverlayAdapter initialized` / visible overlay.
  - Re-run x86 `dx12_test.exe` with Alt+Tab/focus loss and confirm the log uses `preserving Present pacing (D3D12 focus-loss safety)` without device removal/freezing.
- **Source anchors**: `hook/common/dxgi_shared.h`, `hook/common/dxgi_shared.cpp`, `hook/apis/dx11_hook.cpp`, `hook/wrappers/dxgi_swapchain_wrap.cpp`, `hook/common/performance_metrics.cpp`, `tests/test_dxgi_shared.cpp`, `tests/test_performance_metrics.cpp`, `installed/captureengine/logs/20260530_225502_dx10testappnooverlay/hook_debug.log`, `installed/captureengine/logs/20260530_225544_32bitdx12testappfreezeonalttab/hook_debug.log`, `installed/captureengine/logs/20260530_225544_32bitdx12testappfreezeonalttab/dx12_test.exe_FREEZE_2026-05-30_22-56-29_187.dmp`, `installed/captureengine/logs/20260530_225544_32bitdx12testappfreezeonalttab/dx12_test.DMP`.

### 2026-05-26 - Native-FSR early callback routing and DX12 overlay no-blanking checkpoint (build 0.1.3600 / tests 0.1.3601)

- **Inputs**:
  - GTA freeze folder `installed/captureengine/logs/20260525_195848_gtafreeze` contained a freeze dump where AMD FSR presenter/interpolation threads were waiting in `amd_fidelityfx_dx12!ffxQuery` after CE had used the old protected-official-FFX sustained-progress fallback to resume normal side effects without direct enabled `ffxConfigure` or FFX present-callback proof.
  - The first injected switch-app FSR phase could miss CE's callback bridge because the process-wide `GetProcAddress` hook was active before FFX dynamic export names were registered, allowing the startup FSR preload to cache AMD's original `ffxConfigure`.
  - User-visible overlay disappearance during focus/swapchain/startup-overlay transitions remained undesirable, especially when RTSS stayed visible under similar conditions.
- **Fixes**:
  - Protected official AMD FFX startup no longer graduates from quiesced pass-through based on sustained ProcessFrame/ECL progress. Sustained progress is now only a diagnostic (`... remains quiesced`); CE requires direct enabled `ffxConfigure` or FFX callback evidence before resuming normal native-FSR overlay handling.
  - Official AMD FFX modules now use IAT/dynamic routing for `ffxCreateContext`, `ffxConfigure`, and `ffxDestroyContext` when available, with no standard inline JMP export patching on those official modules. `FFXHook::RegisterDynamicHooks()` runs before `IATHook::InitializeGetProcAddressHook()` so early preloads cannot cache unwrapped FFX entrypoints. **Superseded refinement, 2026-05-30**: official DX12 `ffxConfigure` also arms a guarded re-arming int3/VEH fallback for SDK dispatch-table or intra-module calls that bypass IAT/dynamic routing.
  - DX12 transition cooldowns no longer hard-suspend an already drawable overlay. `ShouldHeavySuspendDX12OverlayForSwapchainState(...)` only blanks for zero-sized/iconic swapchains, and startup-overlay compatibility helpers allow a backend-ready overlay to keep submitting through Social/EOS/Steam-like startup windows.
  - Regression coverage now checks early FFX hook routing expectations, no progress-only protected-FFX finalization, initialized-overlay startup visibility, and transition-cooldown no-blanking.
- **Validation**:
  - `python build.py --skip-updates` passed (build `0.1.3600`).
  - `python build.py --no-build --run-tests --skip-updates` passed all 823 tests (displayed metadata `0.1.3601`).
  - Injected session `installed/captureengine/logs/20260526_020450` ran `dx12_fg_switch_test.exe 1280 720 20 --duration 45 --fsr-suspend-interval 2 --startup-recreates 1 --bootstrap-native-swaps 1` to exit code 0 with no dumps, no device-removal markers, and no lingering `captureengine.exe` / `dx12_fg_switch_test.exe`.
  - That session logged first-preload `GetProcAddress: Intercepted FFX API` entries, direct FFX confirmation on the first FSR enable, FFX present-callback overlay rendering during active and suspended native-FSR windows, DLSS PostSL overlay submits, no protected-FFX sustained-progress finalization, and no `ProcessFrame - suspending overlay` diagnostics.
- **Source anchors**: `hook/apis/ffx_hook.h`, `hook/apis/ffx_hook.cpp`, `hook/main.cpp`, `hook/common/ffx_api_parsing.h`, `hook/common/dx12_overlay_policy.h`, `hook/common/overlay_compat.h`, `hook/apis/dx12_hook.cpp`, `tests/test_ffx_api_parsing.cpp`, `tests/test_dxgi_shared.cpp`, `tests/test_fps_limiter.cpp`, `tests/test_crash_handler.cpp`, `installed/captureengine/logs/20260525_195848_gtafreeze/hook_debug.log`, `installed/captureengine/logs/20260526_020450/hook_debug.log`.

### 2026-05-25 - DX12 FG injected routing fix for Streamline proxy handoff (build 0.1.3592 / tests 0.1.3593)

- **Inputs**:
  - Injected `dx12_fg_switch_test.exe` crash session `installed/captureengine/logs/20260525_181920` and follow-up dumps narrowed the failure to the first post-FSR DLSS-G handoff. Crash stacks landed in raw `dxgi!CDXGISwapChain::Present` / the NVIDIA driver path rather than a healthy Streamline proxy path.
  - Later logs showed the app dynamically requested `CreateDXGIFactory1` from `sl.interposer.dll`, but CE's dynamic hook returned `Wrapped_CreateDXGIFactory1`. That hid Streamline's factory proxy from the app, so Streamline never owned the DLSS-G swapchain interposer.
- **Fixes**:
  - `hook/wrappers/iat_hook.*` now leaves Streamline proxy `CreateDXGIFactory*` exports unmodified while preserving feature-hook coverage for Streamline APIs such as `slDLSSGSetOptions` and `slDLSSGGetState`.
  - DXGI factory compatibility routing now splits app-thread FG handoffs from direct FG-runtime callers: app-thread handoffs use the live export so upstream interposers remain visible; direct Streamline/FFX runtime callers use the unhooked disk export to avoid CE wrapper recursion.
  - Post-FSR Streamline runtime handoffs release CE Present vtable ownership before the runtime-owned swapchain create and defer immediate Present-hook refresh for that handoff.
  - Queue capture now skips CE command-queue vtable patching and factory-wrapper command-queue promotion for queues whose vtables belong to Streamline or official FFX runtime modules.
  - `dx12_fg_switch_test.exe` now fills the same required Streamline common constants as the dedicated DLSS test app (`cameraPinholeOffset`, `depthInverted`, `cameraMotionIncluded`, `motionVectors3D`), removing `eDLSSGStatusFailCommonConstantsInvalid` during the switch-app DLSS phase.
- **Validation**:
  - `python build.py --skip-updates` passed (build `0.1.3592`).
  - Injected session `installed/captureengine/logs/20260525_194259` ran `dx12_fg_switch_test.exe 1280 720 20 --duration 35 --fsr-suspend-interval 2 --startup-recreates 1 --bootstrap-native-swaps 1` to exit code 0 with no lingering `captureengine.exe` or `dx12_fg_switch_test.exe`, no dumps, and no device-removal markers.
  - That session logged Streamline proxy `CreateDXGIFactory1` pass-through, 155 PostSL overlay submits, 44 FFX present-callback overlay renders, `runtime=DLSS_FG active=1`, `runtime=FSR_FG active=1`, suspended-FSR callback rendering, and CE Present-vtable release before post-FSR Streamline handoffs. The app log reported `slDLSSGGetState ret=0 (eOk) status=0 genFrames=1`.
  - `python build.py --no-build --run-tests --skip-updates` passed all 821 tests (displayed metadata `0.1.3593`).
- **Source anchors**: `hook/wrappers/iat_hook.h`, `hook/wrappers/iat_hook.cpp`, `hook/wrappers/wrapper_hooks.cpp`, `hook/wrappers/dxgi_factory_wrap.cpp`, `hook/common/dxgi_shared.h`, `hook/common/dxgi_shared.cpp`, `hook/common/dx12_overlay_policy.h`, `hook/apis/dx12_hook.cpp`, `testapp/dx12_fg_switch_streamline.inl`, `tests/test_dxgi_shared.cpp`, `tests/test_iat_hook.cpp`, `installed/captureengine/logs/20260525_194259/hook_debug.log`, `installed/testapp/dx12_fg_switch_test.log`.

### 2026-05-25 - DX12 FG switch-app startup/pacing checkpoint before injected crash follow-up (build 0.1.3578 / tests 0.1.3579)

- **Inputs**:
  - The switch app had a visible startup mini-stall even without CaptureEngine injection, and injected crash work continued with a newer crash folder `installed/captureengine/logs/20260525_181920`.
  - Earlier injected switch-app session `installed/captureengine/logs/20260525_175344` showed hidden helper swapchain bypass working, then a driver AV after the first normal overlay submit on a Streamline/no-FG runtime-owned startup swapchain; this is pending follow-up, not solved by this checkpoint.
- **Fixes/checkpoint state**:
  - Heavy startup stress defaults are now opt-in: startup native recreates, bootstrap native swapchain stress, disabled FSR-context startup probes, and initial FG runtime preloads no longer run during a normal clean startup.
  - The switch app now supports CLI/config controls for eager startup preloads and async runtime preload (`--startup-preload-fg`, `--preload-streamline`, `--preload-fsr`, `--async-runtime-preload`, and matching disabling switches).
  - FSR and Streamline runtime loads are serialized and can be warmed on background threads after the window is already visible; cleanup joins outstanding preload threads. Test-app logging is thread-safe.
  - DLSS FG's unsynced present path now uses tearing support when available (`DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING` plus `DXGI_PRESENT_ALLOW_TEARING`) to avoid `DXGI_ERROR_INVALID_CALL` on `syncInterval=0`.
  - Frame pacing diagnostics now log `[FG-DIAG] Frame pacing spike` and a summary so startup/runtime-switch stalls are visible in the app log.
- **Validation**:
  - `python build.py --skip-updates` passed (build `0.1.3578`).
  - Standalone `installed/testapp/dx12_fg_switch_test.exe 1280 720 20 --duration 18` exited with code 0, left no lingering process, and produced no present/device-removal errors. It still logged transition-time spikes around runtime swaps, with max delta about 1489 ms.
  - `python build.py --no-build --run-tests --skip-updates` passed all 814 tests (displayed metadata `0.1.3579`).
- **Next**:
  - Analyze dumps/logs in `installed/captureengine/logs/20260525_181920`, harden injected overlay routing for FG switching/suspension crashes, re-run injected stress, then commit/update wiki again.
- **Source anchors**: `testapp/dx12_fg_switch_test.cpp`, `testapp/dx12_fg_switch_config.inl`, `testapp/dx12_fg_switch_swapchain.inl`, `testapp/dx12_fg_switch_fsr.inl`, `testapp/dx12_fg_switch_streamline.inl`, `testapp/testapp_common.h`, `installed/testapp/dx12_fg_switch_test.log`.
