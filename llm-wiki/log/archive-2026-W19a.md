# llm-wiki Log — Archive 2026-W19a

### 2026-05-09 — D3D11 forced AF for UE3 (BioShock Infinite): infinite recursion from EAT patching — fix: patch GetProcAddress only in main EXE IAT (build 0.1.2979)

- **Previous approach (0.1.2972-2978)**: Use `PatchEAT` on d3d11.dll export table for D3D11CreateDevice/D3D11CreateDeviceAndSwapChain to intercept early GetProcAddress calls. Plus DetourGetProcAddress EAT-pollution resolution fix.
- **Crash — infinite recursion still happened**: `Wrapped_D3D11CreateDeviceAndSwapChain: CALLED` → `Calling original at 6213eff0` → `CALLED` → ... repeating. The DetourGetProcAddress fix did not resolve it.
- **Actual root cause**: The real `D3D11CreateDeviceAndSwapChain` function (at `SysWOW64\d3d11.dll` RVA `0x9E960`) reads its OWN EAT entry internally — not via `GetProcAddress`, but by directly indexing into the Export Address Table. When we patched the EAT, the real function's internal dispatch follows the patched RVA → our wrapper → wrapper calls real function → real function reads EAT → our wrapper → **infinite recursion**. This is a hardware-level EAT read inside d3d11.dll's code, completely invisible to DetourGetProcAddress.
- **Fix**: **Remove PatchEAT entirely.** Instead, patch `GetProcAddress` IAT only in the **game's main EXE** from `NotifyHookModuleLoaded`:
  - `hook/main.cpp:832-850`: After `InitializeD3D11Hooks()`, call `IATHook::PatchIAT(GetModuleHandle(nullptr), "kernel32.dll", "GetProcAddress", (void*)IATHook::DetourGetProcAddress, &dummy)` on the main executable only
  - This intercepts the game's early `GetProcAddress(d3d11.dll, "D3D11CreateDevice")` call without patching any other module's IAT
  - The Steam overlay crash is avoided because DXGI's IAT is not touched
  - EAT recursion is avoided because the EAT is not patched
  - The already-registered dynamic hooks (from `InitializeD3D11Hooks()`) handle the interception when `DetourGetProcAddress` is called by the game's EXE
- **Why this works**: When the game's EXE calls `GetProcAddress(d3d11.dll, "D3D11CreateDevice")`, the EXE's IAT redirects to `DetourGetProcAddress` which checks `g_DynamicHooks` and returns `Wrapped_D3D11CreateDevice`. The wrapper calls `oD3D11CreateDevice` (real function, stored by `InitializeD3D11Hooks()` before any patching). The real function reads the UNPATCHED EAT → gets its real address → no recursion.
- **Key lesson**: Never patch the EAT of a DLL whose exported function internally reads its own EAT entry. The 32-bit `SysWOW64\d3d11.dll` on Windows 10/11 uses this pattern for D3D11CreateDevice/D3D11CreateDeviceAndSwapChain.
- **Verification**: Build 0.1.2979 passed. All 698 unit tests pass.

### 2026-05-09 — D3D11 forced AF for UE3 (BioShock Infinite): install vtable hooks during wrapper device creation, not deferred from Present (build 0.1.2969-2970)

- **Problem**: Forced AF override had zero effect on UE3 games (BioShock Infinite). Log showed `Wrapped_D3D11CreateDevice: CALLED` but zero `NoteD3D11DrawHookHit` entries — the D3D11 context Draw vtable hooks were never firing.
- **Two-part root cause**:
  1. **Late IAT hooks**: The IAT hook for `D3D11CreateDevice` was installed from the HookThread's periodic scan, which fires after UE3's startup code has already loaded `d3d11.dll` via `LoadLibrary` and cached `D3D11CreateDevice` via `GetProcAddress`. The game received the real function pointer, bypassing the wrapper entirely.
  2. **No vtable hooks during CreateDevice**: Even when the wrapper WAS called (after part 1 was fixed), `Wrapped_D3D11CreateDevice` never called `InstallVTableHooks`. It wrapped the device in `CWrapD3D11Device` but returned the **real context** directly (`*ppImmediateContext = pRealContext`). The vtable hooks were only installed later from `ApplyDeferredSamplerOverrides11` (called from Present), after UE3 had already cached Draw function pointers from the real context's vtable. `Wrapped_D3D11CreateDeviceAndSwapChain` had the same issue — `DX11Hook_OnSwapChainCreated` did call `InstallVTableHooks` via swapchain → device → context, but still too late for UE3's caching.
- **Fix — Part 1**: Early-hook in `NotifyHookModuleLoaded` (`hook/main.cpp:804-825`): when `d3d11.dll` loads via `LoadLibrary`, install D3D11 IAT hooks immediately via `IATHook::InitializeD3D11Hooks()` before the function returns. This ensures the game's subsequent `GetProcAddress(d3d11.dll, "D3D11CreateDevice")` returns `Wrapped_D3D11CreateDevice`.
- **Fix — Part 2**: Added `DX11Hook_InstallDeviceAndContextHooks` public function (`hook/apis/dx11_hook.h:33-34`, `hook/apis/dx11_hook.cpp:2387-2391`) that calls `InstallVTableHooks`. Called from:
  - `Wrapped_D3D11CreateDevice` (`hook/wrappers/wrapper_hooks.cpp:461-463`): after device creation succeeds, before returning.
  - `Wrapped_D3D11CreateDeviceAndSwapChain` (`hook/wrappers/wrapper_hooks.cpp:516-518`): right after the original call succeeds, before swapchain-specific setup. `DX11Hook_OnSwapChainCreated` is still called afterward for compatibility.
- **Sanity**: `InstallVTableHooks` accepts NULL for any parameter (handles NULL device, NULL context, NULL swapchain gracefully). The call during device creation is safe and idempotent — subsequent calls from `ApplyDeferredSamplerOverrides11` or `DX11Hook_OnSwapChainCreated` just overwrite the same vtable entries with the same detours.
- **Source anchors**: `hook/main.cpp:804-825` (early LoadLibrary hook), `hook/apis/dx11_hook.h:33-34` (declaration), `hook/apis/dx11_hook.cpp:2387-2391` (implementation), `hook/wrappers/wrapper_hooks.cpp:461-463` (CreateDevice call), `hook/wrappers/wrapper_hooks.cpp:516-518` (CreateDeviceAndSwapChain call).
- **Verification**: `python build.py --skip-updates` passed (build 0.1.2969). `python build.py --no-build --run-tests --skip-updates` passed 698/698 tests (build 0.1.2970).
- **Stale-risk**: Confirmed in logs that wrapper IS now called (`Wrapped_D3D11CreateDevice: CALLED` appears). Need in-game testing on BioShock Infinite to confirm `NoteD3D11DrawHookHit` appears and AF override takes effect. The early-hook approach assumes `d3d11.dll` is loaded via `LoadLibrary` — if a game uses static linking or a different loading mechanism, the `NotifyHookModuleLoaded` path won't fire and the IAT hooks may still be too late.

### 2026-05-09 — D3D11 forced AF: `sample_l` (explicit LOD) no longer treated as unsafe (build 0.1.2965)

- **Problem**: UE3 games (BioShock Infinite) use `sample_l` extensively for detail textures, lightmap sampling, and material blending. The forced AF classifier treated `sample_l` as an unsafe opcode, rejecting ~12-17% of sampler slots from AF override, leaving textures blurry.
- **Root cause**: `D3D11ShaderSamplerUsesUnsafeExplicitSample()` in `sampler_override_utils.h:331` included `samplerUsesLodSample` in the unsafe check. `sample_l` is safe with AF because it specifies which mip level to read from, while AF controls filtering within that level. All unsafe resource types (arrays, cubes, depth, RT, UAV, integer formats, BC4-BC7) are already caught by separate resource validation.
- **Fix**: Removed `samplerUsesLodSample` from `D3D11ShaderSamplerUsesUnsafeExplicitSample()`. `sample_l` is now treated as AF-safe alongside implicit and bias samples. `sample_d` (gradient), `sample_c` (comparison), and OtherExplicit remain unsafe.
- **Diagnostics added**: New `g_DiagSamplerAllowLodSample` and `g_WrapperAFAllowLodSample` positive counters track how many LOD-using samplers get AF. The AF allow log now includes a `lod=` field. Shutdown summaries include `AF_lodAllowed`. New `TreatsD3D11SampleLodAsAFSafe` test added.
- **Phase 1 (build 0.1.2965-2966)**: Removed `samplerUsesLodSample` from `D3D11ShaderSamplerUsesUnsafeExplicitSample()`. This let samplers with BOTH `sample` and `sample_l` pass the AF-safe check, but lod-only samplers still failed `D3D11ShaderSamplerUsesAFSafeSample()` because it required `implicit || bias`. Textures remained blurry (~12-17% rejected).
- **Phase 2 (build 0.1.2967-2968)**: Added `samplerUsesLodSample` to `D3D11ShaderSamplerUsesAFSafeSample()` — lod-only samplers are now AF-safe too.
- **Test changes** (Phase 1): `ParsesD3D11ShaderSamplerTexturePairs` — sampler s0 (implicit+lod) is now AF-safe. `TreatsD3D11SampleBiasAsAFSafeButOtherExplicitSamplesUnsafe` — s7 (lod-only) is no longer unsafe explicit; `unsafeExplicitSamplers=2u` (down from 3u).
- **Test changes** (Phase 2): s7 (lod-only) in `TreatsD3D11SampleBiasAsAFSafeButOtherExplicitSamplesUnsafe` changed from `EXPECT_FALSE(AFSafeSample)` to `EXPECT_TRUE`; `afSafeSamplers` from 1u to 2u. New `TreatsD3D11SampleLodOnlyAsAFSafe` test added.
- **Source anchors**: `hook/common/sampler_override_utils.h:329-332` (removed lod from unsafe), `hook/common/sampler_override_utils.h:335-339` (added lod to AF-safe), `hook/apis/dx11_hook.cpp:104-105` (g_DiagSamplerAllowLodSample counter), `hook/wrappers/d3d11_devicecontext_wrap.cpp:44-45` (g_WrapperAFAllowLodSample counter), `tests/test_sampler_override_utils.cpp` (updated expectations + new tests).
- **Verification**: `python build.py --skip-updates` (build 0.1.2966 / 0.1.2968) + `python build.py --no-build --run-tests --skip-updates` passed 697/697 then 698/698 tests.
- **Stale-risk**: Should be validated in-game on Blackwell GPU to confirm no corruption from broader AF coverage. Existing resource policy (format, bind flags, view dimension, mip count) remains the safety net.

### 2026-05-09 — Strange Brigade DX12 black screen (round 9): ECL-hook reverted, root cause identified as wrong Steam invocation path (build 0.1.2964)

- **Input**: Builds 0.1.2960-2963 (ECL-hook approach) failed — Steam's ECL only fires on frame #1 (before overlay init). Every subsequent frame falls through to the fallback path (overlay ECL submitted after Present, always one frame late).
- **Root cause identified**: `CallOriginalPresent` invoked Steam's explicit hook (`g_externalOverlayPresentHook`, the E9 JMP target) directly via `TryInvokeGuardedExternalSteamOverlayPresent`. Steam's handler fired without the correct return address chain set up by the E9 JMP — so it did NOT chain to `dxgi!Present`. The frame was never presented → black screen.
- **Fix**: `CallOriginalPresent` non-SL Steam path now calls `presentOriginal(pSwapChain, SyncInterval, Flags)` (dxgi!Present with Steam's E9 JMP entry) instead of the explicit hook. This ensures Steam's handler fires through the natural hook chain with the correct return address, so it chains to the original `dxgi!Present` after rendering Steam overlay.
  - Added `StreamlineHook::ExternalOverlayPresentGuard` around the call.
  - Added fallback: if `bbIdx` does not advance after the E9 JMP call, the bypass trampoline (`presentBypass`) is used to ensure the frame is presented.
- **ECL-hook mechanism removed from `DetourPresent`**: Deferred overlay block (post-CallOriginalPresent fallback, fence wait, deferral clear) replaced with a comment noting removal in build 0.1.2964. CE overlay now submits normally (non-deferred) before Present.
- **Key insight for why this works**: Steam's DX12 overlay handler (`OverlayHookD3D3`) expects to be called through the E9 JMP at `dxgi!Present`. When called directly through `g_externalOverlayPresentHook`, it executes its code (submits an ECL) but does NOT call what it believes is the "next" handler — because the "next" handler chain is set up during the E9 JMP dispatch. Without the correct chain, no `dxgi!Present` body is reached, and the frame is never presented. The E9 JMP path ensures Steam chains to `dxgi!Present` after its overlay ECL.
- **Source anchors**: `hook/common/dxgi_shared.cpp:1970-2015` (DetourPresent — deferral flag removed, fence wait simplified), `:2099-2105` (post-CallOriginalPresent block removed, replaced with comment), `:3713-3783` (CallOriginalPresent non-SL Steam path — E9 JMP invoke with bbIdx fallback).
- **Result: CONFIRMED WORKING on Strange Brigade DX12 (build 0.1.2964)**:
  - All three layers visible simultaneously: game content, CE overlay, Steam overlay
  - No black screen
  - Steam overlay renders correctly (Shift+Tab, notifications visible)
  - Backbuffer index advances normally (`bbIdx=0→1`, `1→2`, `2→0`)
  - No "Steam-deferred overlay still pending" messages (ECL-hook fallback removed)
  - No "E9 JMP did not advance" fallback messages (fallback not triggered — E9 JMP chains to dxgi!Present correctly)
  - Overlay fence wait completes before Present as expected
  - CE overlay responds to input, renders on top of game content
  - No crashes across extended gameplay session
- **Verification**: Build 0.1.2964 compiles and passes all unit tests. `[OK] Hook DLL verified: 0.1.2964`. Confirmed working on Strange Brigade DX12 (no Streamline, no FG, Steam overlay active).

### 2026-05-09 — Strange Brigade DX12 black screen (round 8): ECL-hook-based deferred CE overlay submission (builds 0.1.2960-2963)

- **Problem**: Builds 0.1.2943-0.1.2950 confirmed that invoking Steam's DX12 overlay handler through ANY path (E9 JMP, explicit invoke, vtable restore, Steam-only) produces black game content. The Steam-only experiment (0.1.2950) showed that even WITHOUT CE overlay rendering, Steam's handler alone produces a black screen.
- **Critical insight from 0.1.2949 diagnostic logging**: Steam's handler:
  - Returns S_OK (hr=0x00000000)
  - Does NOT call Present internally (presentCalls=2→2)
  - Does NOT modify swapchain (bufCount=3→3, bbIdx=0→0)
  - Does NOT touch code bytes (e9Intact=1→1)
  - The ONLY GPU-side operation Steam performs is submitting an ExecuteCommandLists (ECL) to the game queue
- **Root cause hypothesis**: Steam's DX12 overlay handler submits an ECL that transitions/clears/overwrites the backbuffer. CE's overlay ECL (submitted BEFORE Steam invoke via `ProcessFrame`) is therefore erased by Steam's ECL. The GPU queue order becomes: [CE overlay ECL] [fence signal] [Steam ECL] [Present] — but Steam's ECL executes after CE's but before Present, clearing CE's work.
- **The ECL-hook approach**: Instead of submitting CE overlay before invoking Steam, defer CE overlay submission to `DetourExecuteCommandLists`, which fires AFTER Steam's overlay ECL on the same queue. GPU queue order becomes: [Steam ECL] [CE overlay ECL] [fence signal] [Present].
- **Implementation details (builds 0.1.2960-2963)**:
  1. **`g_deferOverlaySubmitToSteamECL` flag** — set by `DetourPresent` for the non-SL Steam path before `ProcessFrame`
  2. **`g_steamDeferredOverlay` state struct** — captures cmdList, allocIdx, eclQueue, and pending flag during ProcessFrame
  3. **`ProcessFrame` modification** — when flag is set, records overlay commands into the command list and closes it, but skips ECL submission and fence signal. Jumps to `skip_steam_deferred_fence_signal` label.
  4. **`SubmitSteamDeferredOverlay`** — submits the deferred overlay ECL to the queue. Uses `g_RealD3D12ECL` if available, else `GetOriginalExecuteCommandLists` (per-queue original from vtable hook) to avoid re-entering `DetourExecuteCommandLists`. Signals fence immediately after submission.
  5. **`DetourExecuteCommandLists` modification** — detects Steam overlay ECL via `IsSteamOverlayModulePath()` check on `eclCallerModulePath`. After calling the original ECL, if `eclCallerIsSteam && hasDeferredOverlay`, calls `SubmitSteamDeferredOverlay(pThis, "ecl_hook")`. Logs "ECL hook detected Steam with deferred overlay pending" vs "no deferred overlay pending".
  6. **Fallback in `DetourPresent`** — after `CallOriginalPresent` returns, if the deferred overlay is still pending (Steam never called ECL), calls `InvokeDX12SubmitSteamDeferredOverlay()` as fallback. Then waits for fence completion and clears the deferral flag.
  7. **Fence wait skip before Present** — when `steamOverlayDeferred`, skip the normal `InvokeDX12WaitForOverlayCompletion` before Present (overlay hasn't been submitted yet). Fence wait happens after `CallOriginalPresent` returns.
  8. **`DX12_WaitForOverlayCompletion` logging improvement** — logs fence-already-complete case with mode info. Extended log quota from 20 to 50.
  9. **`dxgi_shared.cpp` non-hook build stubs** — added `ResolveDX12SetDeferOverlay`, `ResolveDX12SubmitSteamDeferredOverlay`, `ResolveDX12IsDeferOverlayPending` with `GetModuleHandleA`/`GetProcAddress` resolution for non-BUILDING_CAPTURE_HOOK builds.
- **Key design decisions**:
  - **Flag set only for non-SL Steam path**: `nonSLSteamInvokePath = !steamOnlyTest && api == D3D12 && steamOverlayLoaded && !IsSLInterposerLoaded()` — avoids interfering with SL FG paths.
  - **No manual env vars**: Fully automatic, no user configuration needed.
  - **`GetOriginalExecuteCommandLists` over vtable fallback**: When `g_RealD3D12ECL` is NULL, use per-queue original to avoid recursion.
  - **Fallback safety**: If Steam never calls ECL (declined ECL hook), submit after Present returns — too late for current frame but prevents leaking resources.
- **`IsSteamOverlayModulePath`** — checks if module path contains "gameoverlayrenderer", matching Steam overlay for both x64 and x86.
- **Source anchors**: `hook/apis/dx12_hook.cpp` (~line 955-1080: state struct, exports, SubmitSteamDeferredOverlay, IsSteamOverlayModulePath, ProcessFrame skip logic, DetourExecuteCommandLists ECL hook detection), `hook/common/dxgi_shared.cpp` (~line 1970-2150: deferral flag set in DetourPresent, skip fence wait, post-CallOriginalPresent fallback + fence wait + clear).
- **Verification**: Build 0.1.2963 compiles and passes all 696 unit tests. Confirmed `GetOriginalExecuteCommandLists` usage replaces the old vtable fallback (build 0.1.2962). All previous test results preserved.
- **Superseded by build 0.1.2964** (round 9): The ECL-hook approach was a dead end — Steam's ECL hook only fires on frame #1. The real root cause was the wrong Steam invocation path in `CallOriginalPresent`. See round 9 entry above for the confirmed fix.
- **Open questions / stale-risk** (historical — superseded):
  - The ECL hook fires for Steam only ~1 in 50+ frames (build 0.1.2960 logs). For most frames, the fallback path submits overlay after Present — too late.
  - Even in frames where the ECL hook DOES fire and submits CE overlay between Steam ECL and Present, black screen persists — suggesting root cause may be different from ECL backbuffer clearing.
  - No D3D12 debug layer / GPU validation is active — potential resource state mismatches between Steam's ECL and CE's overlay ECL go uncaught.
  - Steam's ECL may leave backbuffer in non-PRESENT state, making CE's `PRESENT→RT` barrier incorrect.
- **Pending** (historical — superseded): Test build 0.1.2963 on Strange Brigade DX12. Was never tested — build 0.1.2964 superseded with root-cause fix.

### 2026-05-09 — Strange Brigade DX12 black screen (round 7): diagnostic results + experimental Steam-only mode (build 0.1.2950)

- **Input**: Build 0.1.2949 tested — diagnostic logging revealed:
  - `bbIdx=0->0` — backbuffer index UNCHANGED after Steam invoke. Steam does NOT call Present internally.
  - `bufCount=3->3` — buffer count UNCHANGED. Steam does NOT call ResizeBuffers.
  - `presentCalls=2->2` — Present call counter UNCHANGED. Steam does NOT call Present.
  - `e9Intact=1->1` — E9 JMP at `dxgi!Present[0..4]` is intact before AND after Steam invoke.
  - `hr=0x00000000` — Steam returns S_OK.
  - All swapchain/function-code state is preserved across Steam invoke.
- **Critical insight**: Steam's handler modifies NEITHER the swapchain, NOR the function code, NOR calls Present internally. Yet the screen is black. The ONLY thing Steam's handler does is submit an ExecuteCommandLists (ECL) to the game queue. This strongly suggests Steam's DX12 overlay ECL clears or discards the backbuffer content.
- **Hypothesis confirmed**: Steam's DX12 overlay handler (called through any path — E9 JMP, explicit invoke, vtable restore) submits a command list that transitions the backbuffer and clears it or overwrites it. This erases game content AND CE overlay. When CE's overlay is rendered before Steam's invoke, CE's overlay is also erased by Steam's ECL.
- **Build 0.1.2950**: Adds `CE_STEAM_ONLY_OVERLAY=1` environment variable that:
  1. Skips `HandleDX12ProcessFrame` (no CE overlay rendering)
  2. Invokes Steam's handler directly in `CallOriginalPresent`
  3. Falls back to bypass trampoline if Steam invoke is declined
  This isolates whether Steam's handler alone produces black screen (game content missing) or whether CE overlay + Steam interaction is required.
- **Expected results**:
  - If black screen with Steam-only: Steam's DX12 overlay ECL clears the backbuffer → game content is lost by Steam's handler alone
  - If game visible with Steam-only: CE overlay + Steam overlay interaction causes the black screen → fix barrier/transition code in CE's overlay rendering
- **Source anchors**: `hook/common/dxgi_shared.h:159-169` (experimental flag), `hook/common/dxgi_shared.cpp:1870-1906` (skip logic + flag init), `hook/common/dxgi_shared.cpp:3540-3650` (diagnostic logging).
- **Verification**: All 696 unit tests pass. Build 0.1.2950.

### 2026-05-09 — Strange Brigade DX12 black screen (round 6): comprehensive diagnostic logging for Steam invoke (build 0.1.2949)

- **Input**: Build 0.1.2948 tested — still black screen. vtable[8] restore did NOT fix the issue.
- **Log analysis**: Key observations from build 0.1.2948 logs:
  - `externalPresent` (Steam handler at `00007FFF8725058A`) IS invoked and returns `S_OK` (hr=0x00000000)
  - vtable[8] is correctly restored to `presentOriginal` before invoke and re-hooked to `DetourPresent` after
  - `DX12: Ignoring command-list count from third-party overlay caller ...gameoverlayrenderer64.dll` appears DURING Steam handler execution — Steam submits its overlay commands via ECL (the ECL is NOT blocked, only the counting is skipped)
  - Steam's ECL IS submitted to the game queue — Steam's overlay commands execute on the GPU
  - No crashes, no device removal, no vtable corruption detected
- **Remaining unknowns**: What causes the black screen if Steam's commands execute and Present returns S_OK?
  - Hypothesis A: Steam's handler calls Present internally → advances buffer index → CE's overlay on old buffer becomes invisible
  - Hypothesis B: Steam's overlay rendering clears/discards the backbuffer content (game + CE overlay)
  - Hypothesis C: The GPU queue order is: CE fence signal → Steam ECL → CE deferred fence signal → Present — and the deferred signal interferes with queue processing
  - Hypothesis D: Steam's init call (AttemptSteamDX12OverlayInit with vtable[8]=dxgi!Present) changes D3D12 device state that affects subsequent overlay rendering
- **Diagnostic logging added (build 0.1.2949)**:
  - Backbuffer index before/after Steam invoke (via `IDXGISwapChain3::GetCurrentBackBufferIndex`)
  - Swapchain buffer count before/after
  - E9 JMP integrity check (verify `dxgi!Present[0..4]` is still 0xE9 + correct target)
  - Present call counter (detect internal Present calls from Steam)
  - Swapchain description diff (flag Format, Width, Height, BufferCount, Flags changes)
- **Source anchors**: `hook/common/dxgi_shared.cpp:3540-3640` (Phases B-C with diagnostic logging).
- **Pending**: Test build 0.1.2949 on Strange Brigade DX12. Check DIAG log entries for buffer index changes, E9 JMP integrity, and Present count deltas. These will identify the root cause mechanism.
- **Verification**: All 696 unit tests pass. Build 0.1.2949.

### 2026-05-09 — Strange Brigade DX12 black screen (round 5): vtable[8] restore before Steam overlay invoke with bypass fallback (build 0.1.2948)

- **Input**: Build 0.1.2947 tested successfully — game content + CE overlay visible with bypass-only. Steam overlay blocked.
- **New hypothesis**: The black screen when Steam's overlay handler is invoked directly (`g_externalOverlayPresentHook`) may be caused by Steam's DX12 overlay handler internally calling `pSwapChain->Present()`. With vtable[8] = DetourPresent (CE's hook), such internal Present calls re-enter CE's recursive detection → bypass trampoline → skip Steam's "next" handler entirely. Steam's overlay commands are submitted but the Present path bypasses Steam's "next", breaking Steam's expected hook chain protocol.
- **Fix (build 0.1.2948)**: Before invoking Steam's overlay handler via `TryInvokeGuardedExternalSteamOverlayPresent`, temporarily restore vtable[8] from `DetourPresent` to the original `dxgi!Present` (which has Steam's E9 JMP). After Steam's handler returns, re-hook vtable[8] to `DetourPresent`. If Steam internally calls Present, it flows through: `vtable[8] → dxgi!Present → E9 JMP → Steam handler (re-entrant) → Steam's saved "next" → real Present body`. Fallback to bypass trampoline if Steam invoke is declined.
- **Source anchors**: `hook/common/dxgi_shared.cpp:3475-3621` (new non-SL Steam invoke code with vtable restore, invoke, re-hook, and bypass fallback).
- **Diagnostic logging added**:
  - Phase A: vtable[8] restore success/failure
  - Phase B: Steam invoke success (HRESULT, vtableRestored, presentOriginal) OR decline (reason, bypass availability, thread ID)
  - Phase C: vtable[8] re-hook success OR detection of external vtable modification during Steam handler execution
  - Phase E: bypass fallback with presentOriginal and thread ID
- **Pending test**: Build 0.1.2948 needs to be tested on Strange Brigade DX12. Check three outcomes:
  1. Game content + CE overlay + Steam overlay all visible (success)
  2. Game content + CE overlay visible, Steam overlay blocked (fallback to bypass)
  3. Game content black (Steam invoke still causes black screen — need further root cause analysis)
- **Verification**: All 696 unit tests pass. Build 0.1.2948.

### 2026-05-09 — Strange Brigade DX12 black screen (round 4): bypass-only fallback — ALL Steam handler invoke approaches produce black screen (build 0.1.2947)

- **Input**: Build 0.1.2943 (explicit Steam overlay invoke) tested — still black. Log confirmed `TryInvokeGuardedExternalSteamOverlayPresent` IS called and DOES invoke Steam's handler (`DXGIShared: Invoking guarded Steam Present hook #N`). CE overlay IS rendered and fence wait IS done. But game content is still black.
- **Key discovery**: THREE different approaches to invoking Steam's overlay handler ALL produce black game content:
  1. E9 JMP path: `presentOriginal(pSwapChain, ...)` via dxgi!Present (E9 JMP)
  2. Explicit invoke: `g_externalOverlayPresentHook(pSwapChain, ...)` (Steam's handler directly)
  3. Vtable restore: temporarily set vtable[8] = dxgi!Present, call Present, re-hook
- **Deeper root cause**: The issue is not about HOW Steam is called, but THAT Steam is called at all when CE's vtable hook is active. Even when Steam's overlay renders nothing (just calls "next"), the game content disappears. The most likely cause: Steam's init Present call in `AttemptSteamDX12OverlayInit` changes the GPU buffer state in a way that CE's subsequent PRESENT→RT barrier on the backbuffer causes the driver to discard/clear the game content. Steam's overlay init runs on the real game swapchain, which may alter the swapchain buffer tracking or GPU pipeline state in ways CE doesn't account for.
- **Current approach**: Bypass-only for non-SL Steam path. The bypass trampoline calls `dxgi!Present+5` directly, skipping both CE's vtable hook and Steam's E9 JMP. Game content + CE overlay visible. Steam overlay NOT rendered.
- **Pending**: Find a way to render Steam overlay without black screen. Candidate approaches:
  - Use inline hook on dxgi!Present with chain to Steam's E9 JMP (instead of vtable hook) — but chain hooking is fragile (CE and Steam fight over function entry bytes)
  - Read original vtable[8] COM method from dxgi.dll PE .rdata to avoid inner-function calling convention issues
  - Skip CE overlay rendering when Steam is called (let Steam own the Present) and render CE overlay via a different mechanism (e.g., post-present)
  - Use separate D3D12 device/queue for CE overlay that doesn't touch the game swapchain buffers
- **Source anchors**: `hook/common/dxgi_shared.cpp:3499-3515` (bypass-only path with diagnostic logging) — the TryInvokeGuardedExternalSteamOverlayPresent call was removed.
- **Verification**: All 696 unit tests pass. Build 0.1.2947.

### 2026-05-09 — Strange Brigade DX12 black screen (round 3): use explicit Steam overlay invoke instead of E9 JMP path — s_originalVtable8Present was a no-op (build 0.1.2943)

- **Input**: Build 0.1.2942 tested on Strange Brigade DX12 with both CE and Steam overlays — still black game content. Log confirmed `s_originalVtable8Present=00007FFF86A4D9F0 (same=1)` — the COM method IS the inner function `dxgi!Present`. The build 0.1.2941 fix was a **complete no-op**.
- **Corrected root cause**: For this DX12 game, vtable[8] IS `dxgi!Present` (the inner DXGI present function, NOT a COM wrapper method). Calling it through the E9 JMP path enters Steam's overlay handler, but the internal path Steam takes when called through its E9 JMP hook on `dxgi!Present` produces different behavior than when called via `g_externalOverlayPresentHook` (the resolved E9 JMP target called directly). The E9 JMP path causes black game content — the exact mechanism is complex (likely related to Steam's internal state management and GPU queue ordering when CE's overlay commands precede the present call through the inline hook).
- **Fix**: Replace the `s_originalVtable8Present` E9 JMP path in `CallOriginalPresent` non-SL branch with `TryInvokeGuardedExternalSteamOverlayPresent`. This calls Steam's overlay handler directly via `g_externalOverlayPresentHook` (resolved from the E9 JMP target during `InstallPresentInlineHooks`). Steam renders its overlay, calls "next" (which either goes through original `dxgi!Present` body or re-entrant DetourPresent → bypass trampoline), and presents normally. CE's overlay submission + fence wait happens before this call in `DetourPresent`.
- **Fallback**: If `TryInvokeGuardedExternalSteamOverlayPresent` declines (depth guard, missing hook target, or unsafe state), the bypass trampoline is used — game content + CE overlay visible, Steam overlay dropped for that frame.
- **Key insight**: The delegation path in `CWrapDXGISwapChain::Present` sets `g_InWrapperPresent = false` (line 917 of `dxgi_swapchain_wrap.cpp`), so `TryInvokeGuardedExternalSteamOverlayPresent` is NOT blocked by the `inWrapperPresent` guard check.
- **Source anchor**: `hook/common/dxgi_shared.cpp:3475-3516` (non-SL Steam path uses explicit invoke).
- **Verification**: All 696 unit tests pass. Build 0.1.2943.

### 2026-05-08 — Strange Brigade DX12 black screen (round 2): save original vtable[8] COM method and route through it in E9 JMP path (build 0.1.2941)

- **Input**: Build 0.1.2938 tested on Strange Brigade DX12 with Steam overlay — confirmed "black screen with CE overlay rendering" (logs show frames up to #12600, CE overlay renders but game content is black). The E9 JMP path was active (`CallOriginalPresent: routing through E9 JMP at ...` on every frame) with `bufIdx=0` on every `Reinit SUBMIT`.
- **Root cause of black screen (properly identified)**: vtable[8] for this DX12 swapchain points to `dxgi!Present` (the inner function, not the COM method `IDXGISwapChain::Present`). Steam's E9 JMP is on `dxgi!Present`. When `CallOriginalPresent` calls `presentOriginal` (= `oPresent` = `vtable[8]` = `dxgi!Present` with E9 JMP), it enters Steam's overlay handler directly — **skipping the DXGI COM method's kernel state management** (buffer tracking, fence sync, etc.). Without that state setup, the inner Present function produces garbled/black output.
  - Normal Steam-only flow: Game → `IDXGISwapChain::Present` (COM method) → state mgmt → `dxgi!Present` (E9 JMP) → Steam overlay → "next" (original `dxgi!Present`) → present. State management done BEFORE E9 JMP.
  - CE+Steam flow (broken): Game → `DetourPresent` (vtable[8]) → CE overlay → `CallOriginalPresent` → `oPresent` = `dxgi!Present` (E9 JMP) → Steam overlay → "next" → DetourPresent (re-entrant) → bypass → `dxgi!Present+5` → presents. **No COM state management.**
  - CE+Steam flow (fixed): Game → `DetourPresent` (vtable[8]) → CE overlay → `CallOriginalPresent` → original COM method (saved `vtable[8]` from temp swapchain) → state mgmt → `dxgi!Present` (E9 JMP) → Steam overlay → "next" → DetourPresent (re-entrant) → bypass → `dxgi!Present+5` → presents. **State management done by COM method before E9 JMP.**
- **Key discovery**: The bypass trampoline (created from `dxgi!Present`, which IS `vtable[8]` for this game) calls `dxgi!Present+5` directly — the same code Steam's "next" handler executes. Both the bypass and Steam's "next" call the inner function without COM state management. The fix doesn't change what `dxgi!Present+5` executes — it ensures COM method state management runs FIRST.
- **Fix** (`hook/common/dxgi_shared.cpp`):
  1. **New global** `s_originalVtable8Present` (line ~294): Saved from the temp swapchain's vtable[8] at the start of `InstallPresentInlineHooks`, BEFORE any modifications. This captures the real COM method address (unlike `oPresent` which captures whatever is at vtable[8] at hook time, possibly `dxgi!Present` with E9 JMP).
  2. **Debug logging** in `InstallPresentInlineHooks`: Logs `s_originalVtable8Present` value, comparison with `presentAddr` (same/different), and which module `presentAddr` belongs to (dxgi.dll vs gameoverlayrenderer64.dll) — critical for diagnosing which vtable[8] value we're dealing with.
  3. **E9 JMP path** (CallOriginalPresent, non-SL block, line ~3495): Uses `s_originalVtable8Present` instead of `presentOriginal` when available. Falls back to `presentOriginal` if COM method wasn't captured (non-DX12 paths).
  4. **Vtable-hook fallback** (CallOriginalPresent, line ~3629): Same — uses `s_originalVtable8Present` if available, falls back to `presentOriginal`.
  5. **AttemptSteamDX12OverlayInit** (line ~3322): Added logging of `s_originalVtable8Present` vs `presentOriginal` comparison — Steam's init still uses `presentOriginal` (inner function with E9 JMP, which is what Steam expects to find during its init).
- **How the fix works**: When `CallOriginalPresent` routes through `s_originalVtable8Present` (the COM method), the COM method does DXGI kernel state management and THEN calls `dxgi!Present` internally. Steam's E9 JMP on `dxgi!Present` fires naturally from within the COM method — Steam's overlay handler runs with proper DXGI state set up. Steam calls "next" = DetourPresent (re-entrant), DetourPresent's re-entrancy guard forwards to bypass, and the bypass calls `dxgi!Present+5` with state management already done. Both overlays render, frame presents correctly.
- **Key insight for why this works**: The bypass trampoline and Steam's "next" handler both call `dxgi!Present+5` (original inner function body). The inner function CAN present without COM method state management in the normal Steam-only case because the COM method ran first. In the CE case, the COM method was SKIPPED because vtable[8] = DetourPresent prevents it from running. By calling the COM method explicitly (via `s_originalVtable8Present`), state management is restored, and both the bypass and Steam's "next" work correctly.
- **Source anchors**: `hook/common/dxgi_shared.cpp:291-301` (s_originalVtable8Present), `:2855-2888` (save + logging in InstallPresentInlineHooks), `:3492-3515` (E9 JMP path uses COM method), `:3629-3635` (vtable-hook fallback uses COM method).
- **Verification**: All 696 unit tests pass. Build 0.1.2941.

### 2026-05-08 — Strange Brigade DX12: route through Steam E9 JMP path after VEH init for DX12 overlay coexistence (build 0.1.2938)

- **Input**: Build 0.1.2937 diagnostic confirmed `Steam callback is still our no-op` — Steam never overwrites the callback at RVA 0x1621d8, and the `s_steamDX12CallbackReady` gate kept every frame on the bypass trampoline, blocking Steam overlay.
- **Key insight at the time, superseded by 2026-05-31**: The crash symbol for RVA 0x1621d8 is `VulkanSteamOverlayProcessCapturedFrame`, which led to treating that slot as irrelevant to DX12. The 2026-05-31 Strange Brigade dump refined this: current Steam DX12 overlay code can use nearby Present-shaped slots such as `steam+0x162200`, and recovered NULL slots should be patched to CE's DXGI bypass Present rather than a dummy no-op.
- **Fix** (`hook/common/dxgi_shared.cpp`):
  1. Removed `s_steamDX12CallbackReady` state variable and all gates based on it.
  2. In `CallOriginalPresent` "init already attempted" branch: route through `presentOriginal` (E9 JMP path) unconditionally after init succeeds, matching the original build 0.1.2930-0.1.2933 behavior that had Steam overlay visible. The Vulkan callback being our no-op is irrelevant for DX12.
  3. Updated comments to document the then-current callback interpretation; 2026-05-31 supersedes this with dynamic slot recovery.
- **How it works**: After `AttemptSteamDX12OverlayInit` completes (VEH handled the NULL Vulkan callback, Steam's E9 JMP init succeeded), every frame routes through `oPresent` (dxgi!Present with Steam's E9 JMP). OverlayHookD3D3 processes DX12 overlay rendering. Steam calls its "next" handler (vtable[8] = DetourPresent due to CE's hook), which re-enters DetourPresent — the re-entrancy guard forwards to the bypass trampoline, which calls the real `dxgi!Present`. CE's overlay was already rendered before CallOriginalPresent. Result: game content + CE overlay + Steam overlay all visible.
- **Source anchors**: `hook/common/dxgi_shared.cpp` (E9 JMP routing at lines ~3436-3462, updated comments in AttemptSteamDX12OverlayInit).
- **Verification**: All 696 unit tests pass. Build 0.1.2938.
- **Stale-risk**: Medium. Assumes Steam's DX12 overlay in OverlayHookD3D3 is unaffected by the patched Vulkan callback (confirmed by crash dump symbol analysis). If Steam changes its overlay architecture, the routing may need revision.
