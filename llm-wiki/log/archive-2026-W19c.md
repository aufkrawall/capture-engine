# llm-wiki Log — Archive 2026-W19c

### 2026-05-06 — Re-entrant SL crash: ALL Steam overlay invoke paths from within SL's context are unsafe — revert to bypass trampoline + SlNullFunctionStub defense-in-depth (build 0.1.2869)

- **Input**: `logs/20260506_204051` — Talos Principle, build 0.1.2866 with the fix that added Steam overlay invocation to startup-bypass path (3 locations: startup bypass, synthetic re-entrant, confirmed standalone). Crash dump `crash_20260506_204135_1212_pid19760_tid38236.dmp`: RIP=0, same signature as all prior crashes.
- **Crash analysis**: Two crash dumps from different runs (build 0.1.2866 and 0.1.2861-based branch). Both show:
  ```
  0x0 (null call)
  gameoverlayrenderer64!OverlayHookD3D3  ← Steam called through NULL
  capture_hook_x64!DetourPresent          ← our code
  sl_common!slGetPluginFunction           ← Streamline
  sl_dlss_g!DllMain                       ← Streamline code context
  ```
  The key finding: Steam's `OverlayHookD3D3` internally calls `slGetFeatureFunction`/`slGetPluginFunction` to query SL function pointers. When this happens re-entrantly (i.e., Steam was called FROM within SL's own code path), SL returns `kSlResultOk` with NULL function pointer — Steam calls through NULL → RIP=0.
- **Discovery: ALL paths crash** — Three separate experiments proved the Steam overlay cannot be invoked from ANY code path within SL's execution context:
  1. **Direct g_externalOverlayPresentHook call** (Fix 1 in 0.1.2866): startup bypass block directly calls Steam's OverlayHookD3D3 → crash.
  2. **oPresent routing (natural SL→Steam chain)**: SL's E9 JMP on dxgi!Present routes through CE bypass trampoline which includes Steam's inline E9 JMP → Steam → re-entrant slGetFeatureFunction → NULL → crash.
  3. **PostSL synthetic re-entrant path**: Same Steam invoke through explicit `g_externalOverlayPresentHook` → crash.
  4. **CallOriginalPresent forced-bypass / SL-fast-path**: Steam overlay invoke at lines ~3036, ~3071 → crash.
  5. **CallOriginalPresent1 forced-bypass**: Line ~3140 → crash.
- **Root cause**: SL's internal state is NOT re-entrant safe. When `slGetPluginFunction`/`slGetFeatureFunction` is called from within SL's own execution (DllMain, internal SL processing), it returns `kSlResultOk` with NULL function pointers. Steam's overlay hook internally calls these SL functions (to check if SL is active, get function pointers, etc.), and crashes when it gets NULL. This is a fundamental constraint: Steam overlay MUST NOT be invoked from any code running within SL's call chain.
- **Fix** (`hook/common/dxgi_shared.cpp`):
  1. **Reverted ALL 5 g_externalOverlayPresentHook call sites** to use the bypass trampoline unconditionally (disk bytes, no hooks, no Steam overlay invoke):
     - Startup bypass block (DetourPresent, line ~1652)
     - Confirmed-standalone normal-route bypass (line ~1227)
     - Synthetic re-entrant bypass (line ~1283)
     - CallOriginalPresent forced-bypass and SL-fast-path (lines ~3036, ~3071)
     - CallOriginalPresent1 forced-bypass (line ~3140)
  2. **Why bypass trampoline is safe**: It reads the original dxgi!Present bytes from disk, copies them to executable memory, and calls them. This JMP executes PAST any inline E9 JMP hooks (both SL's and Steam's), calling the real dxgi!Present implementation directly. No SL code, no Steam overlay hook code, no re-entrancy.
  3. **Why ALL oPresent/synthetic paths are unsafe**: `oPresent` points to dxgi!Present which has SL's E9 JMP. Calling through `oPresent` enters SL's code → SL processes internally → SL's own trampoline includes Steam's E9 JMP → Steam → re-entrant slGetFeatureFunction → NULL → crash. The synthetic re-entrant path also uses `oPresent` or `g_externalOverlayPresentHook`, both of which lead to Steam → crash.
- **Defense-in-depth: SlNullFunctionStub** (`hook/apis/streamline_hook.cpp`): In `Hooked_slGetFeatureFunction`, when the original function returns `kSlResultOk` with a NULL function pointer, substitute a no-op stub (`SlNullFunctionStub`) and return `kSlResultOk` (instead of returning an error code). This provides defense-in-depth: even if a caller ignores return-error checks and calls the function pointer directly, it calls a harmless stub that returns immediately rather than crashing through NULL.
- **Steam overlay deferred**: Steam overlay rendering through the bypass path produces no Steam overlay on screen during SL FG. This is a known limitation that needs a future safe-path mechanism (background thread, timer, or non-SL caller context). The immediate priority is crash-free behavior.
- **Source anchors**: `hook/common/dxgi_shared.cpp:1227` (confirmed-standalone bypass), `:1283` (synthetic re-entrant bypass), `:1652` (startup bypass), `:3036/:3071` (CallOriginalPresent), `:3140` (CallOriginalPresent1), `hook/apis/streamline_hook.cpp:2018` (SlNullFunctionStub).
- **Tests**: All 676 unit tests pass. Build 0.1.2869.

### 2026-05-06 — Steam overlay invisible in ALL vtable-hook paths: startup bypass and synthetic re-entrant need explicit Steam invoke too (build 0.1.2866)

- **Input**: `logs/20260506_013327` — Talos Principle, build 0.1.2863 with the partial fix.
  Log showed:
  - `g_externalOverlayHook=00007FF997EA008A` (Steam hook saved)
  - `slLoaded=1 streamlineFGRunning=0`
  - `steamRisk=1 threadSafeBypass=0` on every frame
  - ALL frames went through `"DetourPresent: Startup bypass #N"` (before DLSS FG) or
    `"Skipping Steam overlay for SL re-entrant Present (SL DllMain phase, unsafe)"` (after DLSS FG)
  - **NO** "Invoking Steam overlay" messages ever appeared
  - Build showed our new logging (DetourPresent steam state, DetectSLPresentHook vtable skip) confirming 0.1.2863 code was active
  - First `DetourPresent: Steam overlay state` appeared at line 567 confirming the new logging IS present
- **Root cause**: The 0.1.2863 fix added Steam overlay invoke in `CallOriginalPresent` but ALL
  Present calls were intercepted by EARLIER return paths in `DetourPresent`:
  1. **Startup bypass (DllMain guard, line 1669)**: `callerFromStreamlineModule=true && !s_slRoutingActive && steamOverlayLoaded` — returned early via disk-bytes bypass trampoline. Never reached `CallOriginalPresent`.
  2. **Synthetic re-entrant path (line 1283)**: After DLSS FG activation, this path handled all SL-originated Present calls. `steamOverlaySafe` (line 1318) required `postSLConfirmedRendering=true`, which never happened during the PostSL warm-up phase → Steam skipped.
  3. **Confirmed standalone normal route (line 1227)**: Same restrictive `steamOverlaySafeConfirmed` guard.
  The early paths caught ALL frames because `callerFromStreamlineModule` remained true for ALL Present calls
  when SL's interposer wraps the game's Present chain.
- **Fix** (`hook/common/dxgi_shared.cpp`, 3 locations):
  1. **Startup bypass (line 1669)**: Added Steam overlay invocation before the bypass trampoline,
     guarded by `!postSLConfirmedButStartupSettling` (prevents DllMain-phase RIP=0 crashes).
     Steam's overlay hook presents the frame through Steam's own trampoline, so no separate
     bypass call is needed. Rate-limited logging.
  2. **Synthetic re-entrant (line 1318)**: Relaxed `steamOverlaySafe` from
     `!callerFromStreamlineModule || (postSLConfirmedRendering && !postSLConfirmedButStartupSettling)`
     to `!callerFromStreamlineModule || !postSLConfirmedButStartupSettling`.
     The PostSL warm-up phase (pre-confirmed rendering) is well past DllMain — SL modules are
     fully loaded and Steam TLS is initialized. Only the startup settling window (DllMain phase)
     requires the skip.
  3. **Confirmed standalone normal route (line 1241)**: Same relaxation for `steamOverlaySafeConfirmed`.
- **Safety rationale**: `postSLConfirmedButStartupSettling` is the single authoritative guard for
  DllMain-phase safety. When true → startup settling window active, DllMain may still be running,
  Steam TLS may be uninitialized → skip Steam overlay. When false → DllMain has completed, Steam TLS
  is initialized on all threads that have called Present through our hooks → safe to invoke.
- **All 676 unit tests pass. Build 0.1.2866.**

### 2026-05-06 — Steam overlay invisible when SL loaded but FG not running: explicit Steam overlay invoke in forced-bypass path (build 0.1.2863 - PARTIAL fix, superceded by 0.1.2866)

### 2026-05-06 — Steam overlay + DLSS FG: null function pointer fix and Steam overlay invocation (build 0.1.2861)

- **Inputs**: `logs/20260506_003158` — Talos Principle with Steam overlay + DLSS FG.
  `crash_20260506_003218_803_pid16304_tid25552.dmp`: `0xC0000005 at RIP=0` on the game
  render thread (TID 0x63D0).
- **Crash analysis**: Crash stack showed:
  ```
  0x0  ← null call
  gameoverlayrenderer64!OverlayHookD3D3+0x1417f  ← Steam called through null
  capture_hook_x64+0x803ec  ← our code
  sl_common!slGetPluginFunction+0x29004  ← Streamline
  sl_dlss_g!DllMain+0x2049  ← sl_dlss_g code
  ```
  The root cause: Steam's `OverlayHookD3D3` calls `slGetFeatureFunction` to get Streamline
  function pointers. Our `Hooked_slGetFeatureFunction` forwards to the original
  (`slGetPluginFunction`), which returns `kSlResultOk` with a NULL function pointer when
  called re-entrantly from within Streamline's own code during FG processing. Steam then
  calls through the NULL pointer → RIP=0.
- **Fix 1** (`streamline_hook.cpp:2018`): In `Hooked_slGetFeatureFunction`, after calling
  the original, check if `result == kSlResultOk` but `function == NULL`. If so, return
  `kSlResultErrorInvalidState` instead of propagating the NULL pointer. This prevents the
  caller from crashing when Streamline returns NULL during re-entrant calls.
- **Fix 2** (`dxgi_shared.cpp:2950`): In `CallOriginalPresent`, when Steam overlay is
  loaded and SL is loaded, call `g_externalOverlayPresentHook` (Steam's `OverlayHookD3D3`)
  instead of `oPresent`. Steam renders its overlay and calls Present through its trampoline
  (saved original dxgi!Present bytes), which goes to the real dxgi!Present starting at
  byte 6 (past any E9 JMP). This presents the frame with Steam's overlay but without
  going through SL's FG processing. SL misses one frame of FG interpolation but recovers
  on the next game-thread Present. The NULL function pointer guard (Fix 1) prevents the
  crash that previously occurred.
- **Source anchors**: `hook/apis/streamline_hook.cpp:2018` (NULL function guard),
  `hook/common/dxgi_shared.cpp:2950` (Steam overlay invocation).
- **Tests**: All 676 unit tests pass.

### 2026-05-05 — BioShock forced-AF crash and slowdown: shader-aware AF, draw-time reconciliation, and D3D11 draw slot fix (build 0.1.2854)

- **Inputs**: BioShock Infinite UE3 DX11 x86 reruns through `logs/20260505_223901`
  still crashed after savegame load, even with the FPS limiter disabled. The run had
  several mirrored dumps plus `hook_debug.log`, `perf_metrics_27896.csv`,
  `fps_limiter_trace.log`, and `session_manifest.txt`.
- **Dump evidence**: CE and external dumps both showed the same game/render-thread AV:
  null read at `BioShockInfinite.exe!AK::MemoryMgr::SetMonitoring+0x157943`
  (`0x0089bc23`, `mov ecx,dword ptr [eax]`). The stack stayed inside the game binary,
  but the timing lined up with the new forced-AF runtime path.
- **Limiter/perf evidence**: The latest run was not limiter-driven:
  `fps_limiter_trace.log` had only an inactive apply line (`general_enabled=0`,
  `general_fps=140`). `perf_metrics_27896.csv` showed CE per-frame work around a few
  hundred microseconds while game frame gaps were roughly 18-19 ms, so the remaining
  slowdown was more consistent with AF state churn or hook wiring than with the limiter.
- **AF root-cause refinement**:
  1. Shader/SRV-safe was still too broad for Blackwell. BioShock was getting AF on
     shader-proven material-like resources, but some shaders can use explicit LOD,
     bias, gradient, or comparison sample instructions where changing the sampler can
     alter driver behavior in ways game-requested AF would avoid.
  2. Rebinding on every sampler/SRV/shader-resource update created avoidable state
     churn. The safer model is to track state on binds and reconcile only just before
     a draw that actually consumes the dirty pixel state.
  3. The first draw-time hook implementation had a D3D11 vtable slot bug:
     `DrawAuto`, `DrawIndexedInstancedIndirect`, and `DrawInstancedIndirect` were
     installed at 39/40/41 instead of the SDK-correct 38/39/40. Slot 41 is `Dispatch`;
     on x86, hooking it with an indirect-draw `stdcall` signature can corrupt the call
     frame and is a plausible savegame-load crash trigger once gameplay hits compute
     dispatch.
- **Fix**:
  - `hook/common/sampler_override_utils.h` now parses D3D11 shader disassembly for
    sampler-to-texture sample pairs and records whether each sampler is used by plain
    implicit `sample` only versus explicit/non-implicit sample opcodes.
  - `hook/apis/dx11_hook.cpp` and the D3D11 wrapper path now track active pixel shader
    metadata, require pixel-stage implicit sample-only usage, require every sampled SRV
    to pass the existing material texture classifier, and skip explicit sample opcodes
    with new diagnostics.
  - Sampler/SRV/shader hooks now mark pixel state dirty; draw hooks consume that dirty
    flag and reconcile pixel samplers immediately before `Draw*`. Bind-time eager
    reconciliation was removed to cut state churn.
  - D3D11 draw hook slots were corrected to `DrawAuto=38`,
    `DrawIndexedInstancedIndirect=39`, and `DrawInstancedIndirect=40`; slot 41 is left
    as `Dispatch`.
  - Basic/general FPS limiter local cadence keeps the helper-timeout fix, adds richer
    waited/late/reset stats, and deduplicates only immediate duplicate **general**
    limiter calls while active. Capture-sync cadence remains fully paced, as existing
    tests require.
- **Diagnostics added**: AF logs now include pixel shader metadata creation/failure,
  shader-unused sampler skips, no-shader/no-metadata skips, explicit sample skips,
  shader-paired allow lines, draw-reconcile counts, and extended shutdown summaries.
  Limiter logs now include first-wait/late values plus waited/late/reset/active-dedup
  stats for local cadence.
- **Verification**:
  - `python build.py --skip-updates` passed and produced build `0.1.2854`.
  - `python build.py --no-build --run-tests --skip-updates` passed 676/676 tests.
- **Follow-up**: Rerun BioShock with AF=16x and limiter disabled first. A good trace
  should show `Runtime AF hook ensure`, `AF pixel-shader metadata`, bounded
  `drawReconcile`, no slot-41 draw hook install, and either stable `AF reconciled
  sampler` lines or explicit/sample/resource skip reasons for blurry textures. If FPS
  is still low with limiter inactive, compare frame gaps with AF counters before
  changing limiter or vsync behavior.

### 2026-05-05 — Fix Steam overlay still invisible with DLSS FG: DetectSLPresentHook bails when oPresentTrampoline=null (vtable hook path) (build 0.1.2839)

- **Problem**: After builds 0.1.2824-0.1.2836 added explicit Steam overlay invocation with caller-module guards, the log showed the synthetic-reentrant path was entered continuously (#1-#3500+) but NO "Invoking Steam overlay" messages and only 5 "Skipping" (DllMain phase) messages. Steam overlay was still invisible during SL FG.

- **Root cause chain** (`hook/common/dxgi_shared.cpp`):
  1. `oPresentTrampoline` is NULL when using vtable hooking (external E9 JMP detected at InstallPresentInlineHooks — line 2617+). The inline hook trampoline is never created in this path; only `oPresentBypass` is.
  2. `DetectSLPresentHook()` at line 790 has guard `if (!oPresent || !oPresentTrampoline) return;` — always returns early because `oPresentTrampoline` is NULL.
  3. Consequently, `s_slRoutingActive` is NEVER set to true (line 848).
  4. With `s_slRoutingActive=false`:
     - `steamOverlaySafe = !(callerFromStreamlineModule && !s_slRoutingActive) = false` for SL-originated calls → Steam overlay never invoked in synthetic-reentrant path (line 1279).
     - SL routing block (line 1646) never entered → game-originated calls go through `CallOriginalPresent` (line 1687) which correctly routes through SL's E9 JMP via `oPresent`, but Steam overlay check there was never reached.
     - Confirmed standalone bypass (line 1198) never fires → Steam overlay check at line 1210 never reached.
  5. **Net result**: Steam overlay is NEVER invoked from any code path, in any thread.

- **Fix** (`hook/common/dxgi_shared.cpp`, 3 changes):
  1. **`DetectSLPresentHook` line 790**: Changed from `if (!oPresent || !oPresentTrampoline)` to `if (!oPresent) return;` with `oPresentTrampoline && oPresent == oPresentTrampoline` guard only for the trampoline-equality check. This allows JMP detection when `oPresentTrampoline` is NULL (vtable hook path).
  2. **Trampoline bytes log (line 821)**: Guarded with `if (oPresentTrampoline)` to prevent null pointer access when trampoline is NULL. Added `else` log for vtable-hook path.
  3. **Activation log (line 855)**: Changed `oPresentTrampoline` reference to `oPresentTrampoline ? oPresentTrampoline : oPresentBypass` for the routing info log.

- **Why this works**: With `s_slRoutingActive=true`:
  - The SL routing block (line 1646) routes game-originated calls through `oPresent` (SL's E9 JMP), preserving FG processing.
  - `steamOverlaySafe = true` for SL-originated synthetic-reentrant calls → Steam overlay is invoked in the synthetic-reentrant path.
  - The re-entrant path uses `oPresentBypass` (available in vtable hook path) instead of `oPresentTrampoline`.

- **Verification**: Build 0.1.2839 passes all 672 unit tests. No regressions in existing tests.

- **Stale-risk**: Medium — the fix changes the guard condition in a function used by FG switching. Another path that depends on `oPresentTrampoline` non-null must be verified not to cause issues (the early re-entrant path at line 929 and SL re-entrant path at line 1385 both fall through to `oPresentBypass` when trampoline is null).

### 2026-05-05 — Fix DLSS FG + Steam overlay DllMain crash: add postSLConfirmedButStartupSettling guard to steamOverlaySafe (build 0.1.2844)

- **Problem**: Build 0.1.2841 (`postSLConfirmedRendering` for `steamOverlaySafe`) still crashed with same RIP=0 at call #3734. Crash stack: `sl_dlss_g!DllMain → sl_common → capture_hook_x64 → gameoverlayrenderer64!OverlayHookD3D3 → RIP=0`.

- **Root cause**: `postSLConfirmedRendering` can become `true` while SL's DllMain is still running. PostSL confirms its first render during a synthetic-reentrant Present call originating from DllMain. Once `postSLConfirmedRendering=true`, `steamOverlaySafe = !(callerFromStreamlineModule && !true) = true` for SL-originated calls, causing `g_externalOverlayPresentHook` (Steam's OverlayHookD3D3) to be invoked during DllMain → crash.

- **Log evidence**: At 20:40:23.379, "Invoking Steam overlay for SL re-entrant Present #1" fired just 5ms before crash at 20:40:23.384. All 3 prior FG SNAPSHOT entries (14s, 16s, 19s) showed `postSLConfirmed=0`. Between 19.595s and 23.379s PostSL confirmed, then the very next SL DllMain Present invoked Steam → crash.

- **Fix**: Changed both `steamOverlaySafe` guards (confirmed-standalone bypass and synthetic-reentrant path) from `!(callerFromStreamlineModule && !postSLConfirmedRendering)` to `!callerFromStreamlineModule || (postSLConfirmedRendering && !postSLConfirmedButStartupSettling)`. The additional `!postSLConfirmedButStartupSettling` check ensures Steam is only invoked after PostSL has confirmed AND the startup settling window has ended, providing an extra safety buffer that guarantees DllMain completion.

- **Increased diagnostic logging**: Skip limit raised from 5 to 50 (with periodic %500 logging). Both Invoking and Skipping messages now include `callerSL`, `confirmed`, and `settling` state components for better debugging.

- **Verification**: Build 0.1.2844 passes all 672 unit tests.

### 2026-05-05 — Fix Steam overlay still invisible with DLSS FG: use postSLConfirmedRendering for steamOverlaySafe instead of s_slRoutingActive (build 0.1.2841)

- **Problem**: Build 0.1.2839 (enabling `DetectSLPresentHook` without `oPresentTrampoline`) caused early startup crash — same RIP=0 crash as the original DllMain crash. The crash dump showed `sl_dlss_g!DllMain → sl_common → capture_hook_x64!DetourPresent → gameoverlayrenderer64!OverlayHookD3D3 → RIP=0`.

- **Root cause**: Setting `s_slRoutingActive=true` at the first game Present (via fixed `DetectSLPresentHook`) made Steam overlay appear safe (`steamOverlaySafe = !(callerFromStreamlineModule && !true) = true`) even though SL's DllMain was still running on background threads. Steam's TLS/callbacks are unsafe during DllMain.

- **Fix**: Keep `DetectSLPresentHook()` fully reverted (line 790 `!oPresentTrampoline` guard restored). Instead, change `steamOverlaySafe` from `!(callerFromStreamlineModule && !s_slRoutingActive)` to `!(callerFromStreamlineModule && !postSLConfirmedRendering)` in BOTH places:
  1. Confirmed standalone bypass path (line 1208): uses `postSLConfirmedRendering`
  2. Synthetic re-entrant path (line 1278): uses `postSLConfirmedRendering`

- **Why postSLConfirmedRendering works**: This flag is only true AFTER SL's DllMain completes and PostSL renders its first confirmed frame. During DllMain it stays false → Steam is skipped for SL-originated calls. During steady-state FG it becomes true → Steam is safe to call. No `s_slRoutingActive` dependency.

- **Verification**: Build 0.1.2841 passes all 672 unit tests.

- **Stale-risk**: Medium. The fix assumes `postSLConfirmedRendering` correctly tracks DllMain completion. If PostSL confirmation timing changes, Steam overlay availability during early FG startup could regress.
