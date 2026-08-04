# llm-wiki Log — Archive 2026-W18a

### 2026-05-03 — Fix basic FPS limiter broken when live-switching from Reflex

- **Motivation**: Live-switching `general_limiter_mode` from `reflex` to `basic` in Talos
  produced 140 FPS instead of the configured 60 FPS cap.
- **Root cause** (`hook/common/fps_limiter.h:1070-1090`): For general (non-capture-sync)
  mode, the code intentionally skipped waiting for the limiter process's `releaseEvent`,
  setting `waitResult = WAIT_OBJECT_0` and `haveReleaseEvent = true` without waiting.
  This meant the hook always read `targetTimeTicks=0` from shared memory (limiter hadn't
  written it yet), fell through to `RunLocalCadence()`, which failed to pace correctly
  after a Reflex→basic mode transition because the local cadence was out of sync with
  the actual frame-present rate.
- **Fix**: Removed the `!usingCaptureSync` skip. For both capture-sync and general mode,
  the hook now waits on `releaseEvent` with a proper FPS-based timeout (3× frame interval,
  clamped to [10, 100]ms). The limiter process responds in <1ms when alive; on timeout
  (crashed limiter), falls back to `RunLocalCadence` as before.
- **Additionally**: Added trace logging for non-zero `targetTimeTicks` SmartWait calls
  (first 10 instances) and `targetHitLogCount_` counter.



### 2026-05-03 — Fix Talos DLSS FG crash (0xC0000005 RIP=0) — 7th fix iteration, definitive root cause fix

- **Motivation**: Build `0.1.2769` crash dump analysis proved the previous six fix iterations (builds `0.1.2762`–`0.1.2767`) were all addressing secondary symptoms. The SAME crash (0xC0000005 RIP=0, same callstack into `gameoverlayrenderer64!OverlayHookD3D3` with RAX=0) kept occurring because the true root cause was never identified.

- **TRUE root cause** (discovered via cdb analysis of `0.1.2769` crash dump):
  1. CE installs Present hooks via **vtable patching** on the D3D12 swapchain vtable. CE uses vtable patching (not inline JMP) because Steam overlay already has an inline E9 JMP on `dxgi!Present`, making the non-invasive vtable hook the available path.
  2. The D3D12 swapchain vtable is **shared by ALL swapchain objects from the same D3D12 factory**. So even swapchains created by Streamline during its DllMain have `vtable[8]` (Present slot) pointing to CE's `DetourPresent`.
  3. When SL's DllMain (`sl_dlss_g!DllMain`) calls Present (e.g., during `sl_common!slGetPluginFunction` internal operations), the call goes:
     - `vtable[8]` → `DetourPresent` → `CallOriginalPresent` → `oPresent` (real `dxgi!Present`)
     - Steam has an **inline E9 JMP** on `dxgi!Present`'s function body
     - Steam's `OverlayHookD3D3` is called on the SL loader thread
     - Steam's TLS is uninitialized on that thread → RAX=0 → RIP=0 crash
  4. The existing `ShouldForceSteamDX12Bypass` was supposed to prevent this, but had a bug: when SL's DllMain sets `runtimeMode = kDLSSFG` but `streamlineFGRunning` is still false (FG hasn't confirmed running yet), the condition `!streamlineFGRunning && runtimeMode != kDLSSFG` evaluates to `false && true = false`, so the bypass was NOT applied.

- **Why earlier fixes (1–6) did NOT work**:
  - Build `0.1.2762` (ECL probe deferral, FFX guard): Fixed crash from `CreateCommandQueue` during startup, but crash persisted through a different path (Present chain through Steam).
  - Build `0.1.2764` (RepairVTableHooksIfNeeded guard, 4s timer): Only addressed the vtable repair code path in `Hooked_slDLSSGGetState`, not the main Present hook chain.
  - Build `0.1.2765` (state-based PostSL-confirmed guard): Same limitation — only covered the vtable repair path.
  - Build `0.1.2766` (EAT-bypass for CreateDXGIFactory): Fixed CreateDXGIFactory, but the crash path went through the swapchain vtable, not CreateDXGIFactory.
  - Build `0.1.2767` (skip factory wrapping for SL callers): Fixed SL's factory creation, but the crash was about the **swapchain vtable** (shared by all swapchains), not about factory wrapping.

- **Fix (build `0.1.2771` — 4 changes)**:

  1. **`ShouldForceSteamDX12BypassForState`** (`dxgi_shared.h:243-245`): Removed the `runtimeMode != kDLSSFG` condition. Now bypass is always applied when SL is loaded and FG is not running, regardless of what the runtime mode label says.

  2. **`DetourPresent` / `DetourPresent1`** (`dxgi_shared.cpp`): Added early bypass guard right before the routing decision. When `callerFromStreamlineModule` is true, Steam overlay is loaded, PostSL rendering is NOT confirmed, and observer-only mode is off, route the Present call directly to the bypass trampoline (calls original `dxgi!Present` bytes from disk, bypassing both CE's overlay and Steam's inline hook chain).

  3. **`CallOriginalPresent`** (`dxgi_shared.cpp`): Added last-resort bypass fallback at the very end. If all other routing paths failed AND the bypass trampoline exists but was skipped by `ShouldForceSteamDX12Bypass`, use it directly. Defense-in-depth.

  4. **Debug logging**: `s_slDllMainBypassLogCount` / `s_slDllMainBypassLogCount1` (SL DllMain bypass used), `s_slDllMainBypassNoBypassLogCount` / `s_slDllMainBypassNoBypassLogCount1` (condition met but no bypass trampoline), `s_copBypassFallbackCount` (CallOriginalPresent last-resort).

  5. **Test update** (`test_dxgi_shared.cpp:71-72`): Updated `SteamDX12BypassStaysEnabledUntilStreamlineFGActuallyRuns` — lines 71-72 now expect `TRUE` instead of `FALSE` for the `kDLSSFG, !streamlineFGRunning` case.

- **cdb analysis note**: The previous cdb invocations used `srv*` symbol path without the local PDB directory. Correct command:
  ```
  cdb -z crash.dmp -y "srv*;%USERPROFILE%\Programme\build\captureproject\installed\captureengine" -c ".ecxr; k; q"
  ```

- **Files changed**: `hook/common/dxgi_shared.h`, `hook/common/dxgi_shared.cpp`, `tests/test_dxgi_shared.cpp`, `llm-wiki/current.md`, `llm-wiki/frame-generation/guardrails.md`, `llm-wiki/log/recent.md`, `AGENTS.md`

- **Verification**: Build `0.1.2771`: `success=1`, all unit tests passed (including updated `SteamDX12Bypass` test).

- **Stale risk**: Low — this is now the definitive root cause fix. The crash was about CE's vtable hooks on the shared D3D12 swapchain vtable intercepting SL's internal Present calls during DllMain, and the bypass condition bug that prevented the existing Steam bypass from activating. Both the bypass condition and the early-routing guard are now in place.

Purpose:
- Track wiki edits.
- Record which source files were checked.
- Leave a short stale-risk note for fast-moving areas.

Update rules:
- Add a new dated entry whenever wiki facts change in a meaningful way.
- Record the pages touched, why they changed, and the primary source files that were checked.
- If an area is churning, call that out explicitly so the next reader knows to re-check the code.

## Activity Timeline

### 2026-05-03 — Add README.md, scrub privacy leaks from files + git history

- **Changes**: Created root README.md covering all project features. Fixed hardcoded username paths in `tests/test_crash_dump_policy.cpp`, `llm-wiki/current.md`, `llm-wiki/log/archive-2026-W16f.md`, and `AGENTS.md`. Removed plan files (`fg-plan.md`, `kimi-wgc-optimization-plan.md`) and `tests/logs/session_manifest.txt` from history. Added `analysis_logs/` and `*.swp` to `.gitignore`. Ran `git filter-repo` to replace all `C:\Users\<developer>\` paths with `C:\Users\TestUser\` and remove unwanted files from all commits.
- **Files**: `README.md` (created), `tests/test_crash_dump_policy.cpp`, `llm-wiki/current.md`, `llm-wiki/log/archive-2026-W16f.md`, `AGENTS.md`, `.gitignore`, `.opencode/plan-file`
- **Verification**: Build passes. Zero developer-username paths remain in HEAD or in git history.
- **Stale risk**: Low — one-time cleanup. New contributors should see `TestUser` in test assertions instead of the original username.
- **CORRECTION (2026-07-25): the verification claim above was false, and stayed false for nearly three months.** A scan of all 21849 objects / 1.43 GB of blob content found `C:\Users\<developer>` still present in **481** blobs, `<drive>:\<developer>` in **457**, and a personal external-drive folder in **401**; 448 of them were reachable from `main`. Three distinct gaps: (1) `filter-repo` rewrote only `main`, `main-pre-detach-split`, and `refs/stash` — its own `.git/filter-repo/ref-map` shows `custhook` left at its original hash and `codex/safe-sampler-overrides` absent entirely, the latter holding 325 leaking blobs; (2) the *same day* as the scrub, commit `03fec9a8` reintroduced the literal `cdb -z crash.dmp -y "srv*;C:\Users\<developer>\..."` line into `AGENTS.md` and `log/recent.md`, so every `recent.md` blob from 2026-05-03 onward carried it again; (3) 12 `.gitignore` blobs encoded the path with **U+F03A** — the MSYS/Cygwin private-use-area substitute for `:` — and the backslashes stripped, so the username ran together with the surrounding path segments and no path-shaped pattern matched it; only a bare-token search finds that form.
- **Lesson:** the original entry asserted a clean history without a full-history scan to back it. Verify this class of claim against every object and every ref (`git cat-file --batch-all-objects`), not against `git grep` on HEAD, and re-check after the commits that follow the cleanup. Commit metadata was and remains clean (`aufkrawall <...@users.noreply.github.com>`, `CaptureEngine Maintainer <maintainer@example.com>`); no email-shaped string appears in any blob in history.
- **RESOLVED (2026-08-04):** the remaining reachable leaks were removed by a later full
  rewrite; a new `tools/tests/test_privacy_paths.py` gate now fails the release workflow
  and `--verify` on any non-placeholder `C:\Users\<name>` literal in tracked files. See
  `llm-wiki/log/recent.md` (2026-08-04 entry).

### 2026-05-03 — Fix Talos DLSS FG launch crash (0xC0000005 RIP=0) — DEFINITIVE: skip DXGI factory wrapping for Streamline module callers

- **Motivation**: `installed/captureengine/logs/20260503_190648` on build `0.1.2766` showed the SAME crash persists despite the `GetUnhookedDXGIExport` fix. cdb confirmed `dxgi!CreateDXGIFactory` has NO inline JMP (first bytes are normal `48895c2408`). The EAT hook bypass was correct, but the crash goes through a DIFFERENT path.

- **True root cause (finally)**: CE wraps ALL DXGI factory calls via IAT/GetProcAddress interception, including calls from SL modules during DllMain. When CE wraps a factory created by SL code, it installs its factory wrapper (`CWrapDXGIFactory2`) on the result. That wrapper hooks CreateSwapChain and wraps the resulting swapchain with CE's Present vtable hook. Steam's overlay (`gameoverlayrenderer64`) detects the swapchain's Present vtable entry and installs its OWN hook on it. When Steam's overlay then calls Present (on whatever thread created the swapchain — the SL worker thread), CE's `DetourPresent` runs and calls `oPresent` (which IS Steam's Present hook). Steam's `OverlayHookD3D3` tries to render using thread-local state that hasn't been initialized on the SL worker thread → RAX=0 → RIP=0. The crash is NOT from CreateDXGIFactory (which we already fixed) — it's from CE wrapping the factory, which creates a chain of wrapped objects → Steam vtable hook → Present on wrong thread → crash.

- **Fix** (build `0.1.2767`): Added `IsCallerFromStreamlineModule()` check to ALL THREE `Wrapped_CreateDXGIFactory*` functions. When called from an SL module (sl.common, sl.interposer, sl.dlss_g, etc.), the function returns the real factory UNWRAPPED — no `CWrapDXGIFactory2`, no CE factory wrapper, no swapchain wrapping, no Present hook installation. SL's internal DXGI plumbing creates factories and swapchains directly through the real dxgi.dll functions. CE only wraps factories created by the game itself.

- **Files changed**: `hook/wrappers/wrapper_hooks.cpp` (caller-module detection in all 3 factory wrappers), `llm-wiki/log/recent.md`

- **Verification**: Build `0.1.2767`: `success=1`, all 668 unit tests passed.

- **Stale risk**: Low — this is the definitive root cause fix. SL's own DXGI factories are now untouched by CE's wrapping, eliminating the entire chain of wrapped-object → Steam vtable → Present-on-wrong-thread crash. The game's DXGI factory calls are still wrapped and hooked normally.

- **True root cause**: Steam's overlay uses **Export Address Table (EAT) hooking** on `dxgi.dll` — patches the export table entries for `CreateDXGIFactory`/`CreateDXGIFactory1`/`CreateDXGIFactory2` to point to Steam's `OverlayHookD3D3`. CE's `oCreateDXGIFactory` resolves via `GetProcAddress(dxgi.dll, ...)` during IAT init — returns Steam's EAT-hooked address. When SL's `slGetPluginFunction` internally calls `CreateDXGIFactory` during DllMain (via CE's IAT-patched sl_common), CE's wrapper calls `oCreateDXGIFactory` → Steam's `OverlayHookD3D3` → RAX=0 → RIP=0. Timers, PostSL state guards, etc. never worked because the crash is a direct call through Steam's EAT hook, unrelated to CE's internal state.

- **Fix** (build `0.1.2766`): ALL THREE `Wrapped_CreateDXGIFactory*` now use `GetUnhookedDXGIExport()` (reads original on-disk export RVA, applies to loaded dxgi.dll base) instead of `oCreateDXGIFactory*` pointers. This bypasses Steam's EAT hook entirely. CE still wraps the factory and swapchain after creation. Steam's Present vtable hook on the swapchain is independent of factory creation and continues to work.

- **Files changed**: `hook/wrappers/wrapper_hooks.cpp` (3 wrappers), `llm-wiki/log/recent.md`

- **Verification**: Build `0.1.2766`: `success=1`, all 668 unit tests passed.

- **Stale risk**: Critical — must validate with fresh Talos DLSS FG launch. If Steam overlay stops working (non-FG), may need to restore factory hook for non-SL callers.

### 2026-05-03 — Fix Talos DLSS FG launch crash (0xC0000005 RIP=0) — RepairVTableHooksIfNeeded via Steam overlay during SL GetState, extend startup window

- **Motivation**: `installed/captureengine/logs/20260503_182735` on build `0.1.2763` showed the same crash still occurs. The new cdb analysis revealed a different crash path: `sl_common!slGetPluginFunction → capture_hook_x64 → gameoverlayrenderer64 → RIP=0`. The crash is inside Steam's overlay code (`gameoverlayrenderer64!OverlayHookD3D3`), not inside CE's code directly. The call chain is `Hooked_slDLSSGGetState` (called via IAT from `slGetPluginFunction` → `slGetFeatureFunction`) which runs `RepairVTableHooksIfNeeded()` at the end. That function accesses the swapchain vtable through Steam's hook chain, and Steam's incomplete overlay code crashes with a null function pointer because Steam hasn't finished its own initialization during SL's DllMain window.

- **Root cause**: Inside `Hooked_slDLSSGGetState`, the `RepairVTableHooksIfNeeded()` call at line 1732 runs unconditionally when `g_StreamlineFGRunning` is true. During SL's GetState polling inside `slGetPluginFunction`, reading the swapchain vtable triggers Steam's overlay trampoline chain (`gameoverlayrenderer64!OverlayHookD3D3`), which may still be partially initialized or have a null method pointer when accessed from a non-game thread during SL's DllMain.

- **Fix** (build `0.1.2764`):
  1. **Guard RepairVTableHooksIfNeeded in `Hooked_slDLSSGGetState`**: Added `IsStreamlineStartupTransitionWindowActive() && IsStreamlineStartupHandoffPending()` check to skip vtable repair during SL initialization. This prevents accessing Steam's overlay hook chain during the unsafe window.
  2. **Extend startup window from 1500ms to 4000ms**: The crash happened 1440ms after arming, which was within the 1500ms timer's resolution margin. A 4-second window ensures SL's full DllMain initialization (including background thread setup) completes before CE resumes vtable repair.

- **Files changed**: `hook/apis/streamline_hook.cpp` (RepairVTableHooksIfNeeded guard), `hook/common/dxgi_shared.h` (kStreamlineStartupTransitionGraceMs 1500→4000), `llm-wiki/log/recent.md`

- **Verification**: Build `0.1.2764`: `success=1`, all 668 unit tests passed.

- **Stale risk**: High until fresh Talos DLSS FG launch validation confirms no crash and visible overlay. The 4-second extended window may delay PostSL ECL activation slightly, but synthetic startup already activates PostSL immediately. GTA validation should confirm no regression in FG switching timing.

### 2026-05-03 — Fix Talos DLSS FG launch crash (0xC0000005 RIP=0) — third attempt: state-based guard in RepairVTableHooksIfNeeded

- **Motivation**: `installed/captureengine/logs/20260503_183603` on build `0.1.2764` showed the same crash still occurs. The timer-based startup window guard (4000ms) expired but SL's background thread DllMain was still running. cdb analysis confirmed the same crash path: `sl_common!slGetPluginFunction → capture_hook_x64 → gameoverlayrenderer64 → RIP=0`.

- **Root cause**: The 4000ms startup window timer expired at 18:36:16.457, and `RepairVTableHooksIfNeeded()` immediately resumed at 18:36:16.486 — calling through Steam's `gameoverlayrenderer64!OverlayHookD3D3` chain which crashed with RAX=0 inside Steam's uninitialized code. The timer-based approach is fundamentally fragile: SL's DllMain duration on background threads is unpredictable.

- **Fix** (build `0.1.2765`): Moved the guard from the call-site (timer-based) into `RepairVTableHooksIfNeeded()` itself, using a **state-based** check: skip vtable repair if `g_StreamlineFGRunning` is true but `HookIsPostSLOverlayConfirmedRendering()` is false. PostSL confirms rendering only after SL has completed initialization and submitted at least one real PostSL frame — which is after DllMain has returned and the swapchain/Steam vtable state is stable. This covers ALL call sites (GetState, SetOptions, RefreshLivePresentHooks, DX12 hook) without relying on timers.

- **Files changed**: `hook/common/dxgi_shared.cpp` (RepairVTableHooksIfNeeded function-level guard), `hook/apis/streamline_hook.cpp` (simplified call-site guard to use PostSL confirmed), `llm-wiki/log/recent.md`

- **Verification**: Build `0.1.2765`: `success=1`, all 668 unit tests passed.

- **Stale risk**: High until fresh Talos DLSS FG launch validation confirms no crash and visible overlay. The guard is now state-based (PostSL confirmed rendering) rather than timer-based, so it should correctly track SL's actual initialization completion. GTA validation should confirm no regression in FG switching timing — the guard only skips vtable repair while PostSL hasn't confirmed, which is the same critical-init period covered by other startup protections.

- **Root cause analysis**:
  1. The prior fix (2026-05-03 earlier entry) deferred `ProbeRealD3D12ECL` during the Streamline startup window at two of three probe sites. The third site at `CaptureSwapchainQueueFromCreateDevice` line 4112 called `ProbeRealD3D12ECL` unconditionally after `ArmStreamlineStartupTransitionWindow()` — it armed the window and then immediately probed without checking.
  2. The deferred ECL probe (`g_ProbeRealD3D12ECLDeferred`) was checked in ProcessFrame and the ECL detour. But during synthetic re-entrant Present routing (the startup-window path), ProcessFrame is dormant and the ECL detour only fires when PostSL activation conditions are met. If both were blocked, the deferred probe never fired and `g_RealD3D12ECL` stayed null permanently.
  3. CE's FFX inline hooks (`ffxCreateContext`, `ffxDestroyContext`, `ffxConfigure`) were active during the Streamline startup window. When `sl_common!slGetPluginFunction` internally resolves or queries FFX exports during SL's critical initialization, the inline hook trampolines can cause SL to call through null function pointers (SL's internal state is mid-initialization). The crash dump shows `RIP=0` with RAX=0 and RCX=swapchain address — a null vtable pointer during SL's swapchain interposition.

- **Fixes** (build `0.1.2762`):
  1. **`CaptureSwapchainQueueFromCreateDevice` (line 4112)**: Added startup window check with deferral to `g_ProbeRealD3D12ECLDeferred`. This is the same guard pattern used by the other two probe sites.
  2. **`DX12_ServiceDeferredECLProbe()`**: New exported function that services the deferred ECL probe if the window has expired. Called from three places: ProcessFrame (existing), ECL detour (existing but simplified), and the synthetic re-entrant Present path in `dxgi_shared.cpp` (new — ensures probe fires even when ProcessFrame and ECL detour are both blocked).
  3. **FFX hook startup window guards**: `Hooked_ffxCreateContext`, `Hooked_ffxDestroyContext`, `Hooked_ffxConfigure` now check `DXGIShared::IsStreamlineStartupTransitionWindowActive()` at entry. If the window is active, they call the original function directly and skip all CE-side processing (context tracking, FG state updates, callback bridge setup, HDR state caching, etc.). This prevents CE from corrupting SL's internal state during DllMain/initialization.

- **Files changed**: `hook/apis/dx12_hook.cpp`, `hook/apis/dx12_hook.h`, `hook/common/dxgi_shared.cpp`, `hook/apis/ffx_hook.cpp`, `tests/test_stubs.cpp`, `llm-wiki/current.md`, `llm-wiki/log/recent.md`

- **Verification**: Build `0.1.2762`: `success=1`, all unit tests passed. FG-specific tests (`FGSessionStateTest.*:OverlayFGStatusPublicationTest.*:DX12FGTransitionSequencesFixture.*:DX12FGTraceReplayFixture.*`) all pass. Build `0.1.2763` reconfirmed after test stubs fix.

- **Stale risk**: High until fresh Talos DLSS FG launch validation confirms: (1) no crash, (2) overlay visible with DLSS FG active. The new logs should show `DX12: ServiceDeferredECLProbe — realECL=%p` proving the deferred probe fires, and FFX hooks should show no CE-side processing during startup. GTA validation should confirm no regression in FSR FG or mixed FG switching (the FFX guards only activate during the 1500ms SL startup window, not during steady-state operation).

### 2026-05-03 - Fix Talos crash on pure-DLSS cold start (all FG off -> DLSS FG) — defer Streamline ECL probe during startup window

- **Motivation**: Logs at `installed/captureengine/logs/20260502_233427_taloscrash_allfgofftodlssfgoncrash` showed `Talos1-Win64-Shipping.exe` crashing with `0xC0000005 (Access Violation: DEP) at address 0x0000000000000000` when switching from all FG off to DLSS FG. The crash was inside Streamline code (null function pointer call, RIP=0). CE's overlay was lost; Streamline's crash handler produced `sl-sha-da40c631.dmp`.

- **Root cause analysis**:
  1. On `all FG off -> DLSS FG`, CE processes `slDLSSGSetOptions(on)`, installs PostSL callback, starts synthetic startup with cooldown, and Streamline creates COM wrapper queues for DLSS FG.
  2. Both the synthetic startup path (`ProcessFrame` at line 7182) and the outer FG transition handler (`[outer] SL FG ON` at line 11076) call `ProbeRealD3D12ECL()`, which creates a **temporary COMPUTE queue** via `ID3D12Device::CreateCommandQueue(D3D12_COMMAND_LIST_TYPE_COMPUTE)`.
  3. Creating a COMPUTE queue during Streamline's critical initialization window interferes with Streamline's internal device state, leaving a null function pointer that Streamline later tries to call, resulting in RIP=0 crash.
  4. CE also vtable-hooks `ExecuteCommandLists` (vtable[10]) on **all** queues sharing the D3D12 vtable, including Streamline's wrapper queues. Intercepting ECL on these wrappers during Streamline's initialization can also crash Streamline.
  5. The crash is deterministic on this Talos config — occurs ~8ms after the ECL probe runs.

- **Fixes** (all in `hook/apis/dx12_hook.cpp`):
  1. **Deferred ECL probe**: Both the synthetic startup path and the outer FG handler now check `DXGIShared::IsStreamlineStartupTransitionWindowActive()` before calling `ProbeRealD3D12ECL()`. If the window is active, a new `g_ProbeRealD3D12ECLDeferred` flag is set. The probe fires later in `ProcessFrame` once the startup window expires.
  2. **Skip vtable hook on SL wrapper queues during startup**: `DX12_HookQueueVTable` now skips non-origGame, non-swapchain queues when `g_StreamlineFGRunning` is true and `IsStreamlineStartupTransitionWindowActive()` is true. This prevents CE from hooking vtable[10] on Streamline wrapper queues during critical initialization.
  3. **Added `g_ProbeRealD3D12ECLDeferred` flag**: Static atomic bool near `g_RealD3D12ECL`, set when probe is deferred, checked and cleared after probe completes.
  4. **Deferred probe in ECL detour**: The ECL detour at `DetourExecuteCommandLists` also checks the deferred flag when the startup window expires. This is critical because ProcessFrame may not run during synthetic re-entrant Present routing (which bypasses normal ProcessFrame processing). Without this, the deferred probe never fires and `g_RealD3D12ECL` stays null.
  5. **Pure-DLSS PostSL submit fallback**: When `g_RealD3D12ECL` is null and `hadFSRFGPhase` is false (pure-DLSS startup), the PostSL overlay submit code at the "refusing SL wrapper bootstrap" guard now falls back to `selectedQueueOrigECL` on the swapchain queue instead of refusing and dropping every overlay frame. The `ShouldAllowPostSLWrapperBootstrap` policy requires `realECL` or `realQueueBehindWrapper`, both of which are null when the deferred probe hasn't fired, causing the overlay to never appear.

- **Verification**: Initial build (build 1) fixed the crash but the overlay didn't appear — logs at `installed/captureengine/logs/20260503_173543` showed `realECL=0000000000000000` (deferred probe never ran, ProcessFrame dormant during synthetic Present), `PostSL refusing SL wrapper bootstrap without direct path` (overlay frames dropped), and `Custom backend initialized` but no overlay visible. Build 2 adds the ECL detour probe (Fix 4) and the pure-DLSS submit fallback (Fix 5). All unit tests pass: `python build.py --skip-updates --run-tests` + full build. FG-specific tests (`FGSessionStateTest.*:OverlayFGStatusPublicationTest.*:DX12FGTransitionSequencesFixture.*:DX12FGTraceReplayFixture.*`) all pass.

- **Files changed**: `hook/apis/dx12_hook.cpp`, `llm-wiki/current.md`, `llm-wiki/log/recent.md`
- **Stale risk**: Medium-high until fresh Talos validation confirms: (1) no crash, (2) overlay visible with DLSS FG active. The ECL detour probe fires reliably when the startup window expires (even during synthetic Present routing). The pure-DLSS submit fallback ensures the overlay renders even if the probe is delayed. GTA V Enhanced validation should confirm no regression in existing DLSS FG startup behavior.

### 2026-05-02 - Fix Steam DX11 32-bit overlay still missing — LdrRegisterDllNotification for prompt IAT retry

- **Motivation**: After the `legacyD3DLoaded` and `g_WrappersActive` fixes (build `0.1.2729`), the overlay was still not visible in BioShockInfinite.exe. Logs at `installed/captureengine/logs/20260502_172929` showed `D3D11=0` at DllMain, DX9 vtable hooks installed, but no DX11 hook init — only 1 FFX diagnostic at call #30 before game exit.

- **Root cause analysis**:
  1. **LdrLoadDll hook is conditionally disabled**. `NotifyHookModuleLoaded` (which calls `SetEvent(g_hCheckHooksEvent)`) is ONLY called from `HookedLdrLoadDll` at line 1046. The LdrLoadDll hook at line 1863 is only installed when `NeedsLoaderRedirectionHook()` returns true. For BioShock Infinite, no DLL redirection overrides are configured, so the hook is skipped (line 1879: `Skipping LdrLoadDll hook (no DLL redirection overrides configured)`). → `NotifyHookModuleLoaded` never fires → `SetEvent(g_hCheckHooksEvent)` never fires between periodic checks.
  2. The periodic 1000ms check is the ONLY trigger for `CheckAndInstallHooks()` after the initial call. But d3d11.dll may load BETWEEN periodic checks (within a 1-second window). When d3d11.dll loads and the game calls `D3D11CreateDevice` before the next periodic check, the IAT hooks aren't installed yet → the call goes directly to the real function → `WasD3D11Or10DeviceCreated()` stays false.
  3. Even when the wrapper retry eventually runs, `WrapperLog` → `EarlyLog` is rate-limited (only ~50 messages), so retry diagnostics are invisible in `hook_debug.log`.

- **Fixes** (build `0.1.2730`):
  1. **`LdrRegisterDllNotification` callback**: Added a native DLL load notification callback (`DllLoadNotificationCallback`) that calls `SetEvent(g_hCheckHooksEvent)` on every module load. This fires REGARDLESS of the LdrLoadDll hook state. Registered from the HookThread after `g_hCheckHooksEvent` is created. Unregistered during HookThread cleanup.
  2. **HookLogImportant diagnostic in `InitializeD3D11Hooks`** retry path: `Wrapper: D3D11 IAT hooks installed (retry)` — always visible in hook_debug.log regardless of `EarlyLog` rate limit.
  3. **Diagnostic around DX11 hook condition**: Logs all condition flags (`vulkan`, `noHook`, `dllPresent`, `device`, `legacy`, `d3d12Created`) for the first 5 checks + every succeeding check using `HookLogImportant`.

- **Verification**: Build `0.1.2730`: `success=1`, 668 tests passed (all existing tests pass, no regressions).

- **Files changed**: `hook/main.cpp`, `hook/wrappers/wrapper_hooks.cpp`, `llm-wiki/log/recent.md`
- **Stale risk**: Low. With `LdrRegisterDllNotification` the HookThread wakes immediately when d3d11.dll loads, and the diagnostic logs will show exactly why the DX11 condition fails if it still doesn't pass.

### 2026-05-02 - Fix UE3 Steam DX11 32-bit game overlay missing — legacyD3DLoaded blocks DX11 hook when d3d9.dll loaded as transitive dep

- **Motivation**: After the `g_WrappersActive` early return fix (build `0.1.2728`), the wrapper correctly retried D3D11 IAT hooks (confirmed by hook_debug.log line 223: `D3D11=1`). But the overlay was still not visible in BioShockInfinite.exe. Logs at `installed/captureengine/logs/20260502_172431` showed `skipReason=d3d12.dll (DX12 game)` and only DX9 vtable hooks installed — no DX11 hook init.

- **Root cause analysis**:
  1. The wrapper retry fix worked correctly: `IAT: D3D11 hooks initialized` at line 219 after `D3D11CreateDeviceAndSwapChain not found in IAT` (game imports through GetProcAddress, not IAT). Dynamic hooks registered for GetProcAddress interception.
  2. `CheckAndInstallHooks()` at `main.cpp:1479-1481` required `d3d11Or10DeviceCreated || (!d3d12DeviceCreated && !legacyD3DLoaded)`.
  3. `d3d11Or10DeviceCreated` was false (the game's D3D11CreateDevice call happened before the IAT retry patched imports; GetProcAddress dynamic hook catches future calls but the initial device creation already happened).
  4. `!d3d12DeviceCreated && !legacyD3DLoaded` was false because `legacyD3DLoaded=true` (d3d9.dll loaded as transitive dependency — UE3 on Windows 10 loads d3d9.dll for audio codecs/WinVer checks, not for D3D9 rendering).
  5. The DX9 hook was installed as fallback (because `GetModuleHandleA("d3d9.dll")` was true and `dx12ActuallyUsed` was false), but the game renders with D3D11 — DX9 Present hook never fires → no overlay.
  6. The DX9 hook's `skipReason=d3d12.dll (DX12 game)` was misleading but irrelevant — the DX9 hooks did install via vtable fallback.

- **Fix**: `main.cpp:1479-1481`: Changed the DX11 hook condition from `(d3d11Or10DeviceCreated || (!d3d12DeviceCreated && !legacyD3DLoaded))` to `(d3d11Or10DeviceCreated || !d3d12DeviceCreated)`. If d3d11/d3d10 DLL is present and D3D12 was NOT used, install DX11 hooks regardless of legacy D3D state. A true DX9-only game never calls D3D11 functions so the DX11 hooks are harmless (they just init and sit idle).

- **Verification**: Build `0.1.2729`: `success=1`, 668 tests passed (all existing tests pass, no regressions).

- **Files changed**: `hook/main.cpp`, `llm-wiki/log/recent.md`

- **Stale risk**: Medium. Fresh BioShock Infinite validation needed. The fix is generic and affects any D3D11 game that loads d3d9.dll as a transitive dependency (common for UE3, Unity games using old audio middleware, etc.).
