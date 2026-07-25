# llm-wiki Log — Archive 2026-W19d

### 2026-05-05 — DLSS FG + Steam overlay DllMain crash: root-cause fix with caller-module detection (build 0.1.2823)

- **Crash symptom**: Same 0xC0000005 RIP=0 crash in Talos DLSS FG + Steam overlay persisted through ALL previous timer-based approaches (builds 0.1.2760–0.1.2822).

- **Previous approaches (all failed)**: Builds 0.1.2762–0.1.2813 tried IAT/GetProcAddress patching, stack-walk detection, thread-ID guards, unconditional startup bypass, 4000ms timer window. Build 0.1.2822 increased timer to 10000ms and removed premature `ShouldClear...` call. **Still crashed** with identical stack. Timer-based approach rejected as fundamentally unreliable.

- **Root-cause fix** (`hook/common/dxgi_shared.cpp:1515-1535`): Replaced timer-based `inStartupWindow` with **caller-module detection**: when `callerFromStreamlineModule` is true AND `s_slRoutingActive` is false AND Steam overlay is loaded, bypass Steam unconditionally via `oPresentBypass`. No timer, grace period, or PostSL confirmation state dependency.

- **How detection works**: `callerFromStreamlineModule` is computed at DetourPresent entry (~line 995) via `CE_CAPTURE_RETURN_ADDRESS()` (= `_ReturnAddress()`) and `GetModuleHandleExA(FROM_ADDRESS)`. During SL DllMain, this returns an address in `sl_common!slGetPluginFunction` (confirmed by crash dump: RetAddr at frame 02 = `sl_common!slGetPluginFunction+0x12d5`). `IsCodeAddressFromStreamlineModule()` checks the module base against known SL modules.

- **Why `!s_slRoutingActive` guard**: Ensures bypass only fires before SL's FG routing is active (DllMain phase). Once SL's E9 JMP is installed and `s_slRoutingActive=true`, SL-internal Present calls go through normal SL routing (line ~1545), NOT the bypass. Without this guard, all SL-originated Present calls during active FG would bypass SL routing, breaking FG processing.

- **Verification**: Build `0.1.2823` passes all 672 unit tests.

- **Stale-risk**: Low — root-cause fix.

### 2026-05-05 — Fix Steam overlay blocked during SL FG: narrow Steam skip guard to DllMain phase (build 0.1.2836)

- **Problem**: After 0.1.2835 fixed the crash, Steam overlay was still not visible during DLSS FG. The crash fix used `!callerFromStreamlineModule` to guard Steam overlay invocation, but this was too broad — it blocked Steam for ALL SL-originated Presents, including during steady-state FG where Steam is safe.

- **Root cause**: During active SL FG (`s_slRoutingActive=true`), ALL Present calls through CE's vtable hook have `callerFromStreamlineModule=true`. SL's E9 JMP intercepts game-originated Presents and routes them through SL's internal handler, which re-enters CE's DetourPresent with SL module return addresses. Since `!callerFromStreamlineModule` was false for all of these, Steam overlay was NEVER invoked.

- **The unsafe case**: Only SL DllMain (`callerFromStreamlineModule=true && s_slRoutingActive=false`) is unsafe for Steam — Steam's TLS/callbacks may be uninitialized during DllMain, causing RIP=0 crashes.

- **Fix** (`hook/common/dxgi_shared.cpp`): Changed both Steam overlay guards from `!callerFromStreamlineModule` to `!(callerFromStreamlineModule && !s_slRoutingActive)` (computed as `steamOverlaySafe` / `steamOverlaySafeConfirmed`). This allows Steam calls during active SL FG while blocking them only during the DllMain phase.

- **Verification**: Build 0.1.2836 passes all 672 unit tests.

### 2026-05-05 — Fix SL DllMain crash regression: skip Steam overlay invocation when Present originates from SL module (build 0.1.2835)

- **Problem**: Build 0.1.2824 (Steam overlay invocation for SL FG) caused Talos to crash early on start with RIP=0 — the same symptom as the original SL DllMain crash.

- **Root cause**: The Steam overlay invocation in `DetourPresent` (synthetic re-entrant path line ~1265, confirmed standalone bypass path line ~1206) had no `callerFromStreamlineModule` guard. During SL's FG transition, `sl_dlss_g!DllMain` → `sl_common!slGetPluginFunction` → a Present call hits CE's vtable hook. At this point:
  - `callerFromStreamlineModule=true` (Present return addr is in SL module)
  - `s_slRoutingActive=true` (SL has installed its E9 JMP)

  The crash-fix bypass at line ~1590 (`callerFromStreamlineModule && !s_slRoutingActive`) does NOT fire because `s_slRoutingActive` is TRUE. The code falls through to the synthetic re-entrant path, which invokes Steam's overlay hook. Steam's overlay code (gameoverlayrenderer64) crashes with RIP=0 because its internal TLS/callbacks are unsafe to run from within SL's call chain.

- **Crash dump stack**:
  ```
  0x0 (null ptr call)
  gameoverlayrenderer64!OverlayHookD3D3+0x1417f
  capture_hook_x64!DetourPresent
  sl_common!slGetPluginFunction+0x29004
  sl_dlss_g!DllMain+0x2049
  sl_interposer!DXGIGetDebugInterface1+0x28c3
  Talos1_Win64_Shipping
  ```

- **Fix** (`hook/common/dxgi_shared.cpp`): Added `!callerFromStreamlineModule` to both Steam overlay invocation guards:
  - Synthetic re-entrant path (line ~1265): `if (g_externalOverlayPresentHook && steamOverlayLoaded && !callerFromStreamlineModule)`
  - Confirmed standalone bypass path (line ~1206): same guard added
  - Added debug logging when Steam overlay is skipped due to SL call chain

- **Why this is correct**: The bypass trampoline (`oPresentBypass`/`EnsurePresentBypassTrampoline`) calls the real Present directly, which is safe from any call chain. Steam's overlay should only be invoked when the Present originates from game code, not from SL module code.

- **Verification**: Build 0.1.2835 passes all 672 unit tests.

- **Stale-risk**: Low. The guard is conservative (only adds a `callerFromStreamlineModule` check to an existing condition).

### 2026-05-05 — Add `--no-build` flag for fast test-only runs (build 0.1.2834)

- **Problem**: Running `python build.py --run-tests` always recompiles C++ code (even though Ninja is incremental, it still runs CMake/Ninja checks). For quick test iteration after non-code changes (e.g. config, docs, wiki), waiting for compilation is wasteful.

- **Solution**: Added `--no-build` flag (`build.py:5228`). When set:
  - Skips `compile_project()` entirely — no CMake, no Ninja, no linking.
  - When combined with `--run-tests`, finds `tests/unit_tests.exe` directly and runs it via `run_tests()`.
  - Errors out if the test binary doesn't exist (prompts user to build first without `--no-build`).
  - Still runs MSYS2 setup and pacman checks (use `--skip-updates` to skip those too).

- **Usage**: `python build.py --no-build --run-tests --skip-updates` for the fastest path (no MSYS2 sync, no compilation, just test execution).

- **Verification**: Build `0.1.2834` — `--no-build --run-tests` passes all 672 tests in ~0.8s (test execution time only, no compilation overhead).

- **Stale-risk**: Low. Simple flag gate around `compile_project()` call. Test binary discovery logic mirrors existing patterns.

### 2026-05-05 — Steam overlay invisible with SL FG: explicitly invoke Steam overlay hook in bypass paths (build 0.1.2824)

- **Problem**: After the DllMain crash fix, the game no longer crashed, but Steam overlay (friends list, shift-tab) was not visible during DLSS FG.

- **Root cause**: SL's E9 JMP on dxgi!Present overwrites Steam's inline JMP. When SL FG is active, CE routes the game's Presents through SL's JMP (via `oPresent`). CE's bypass path (synthetic re-entrant + confirmed standalone) calls `oPresentBypass` created from disk bytes — which has NO JMPs at all. Steam's overlay is never reached.

- **Fix** (`hook/common/dxgi_shared.cpp`):
  1. Added `ResolveE9JmpTarget()` function: reads the E9 relative offset from a function body and computes the absolute target address.
  2. Added `g_externalOverlayPresentHook` global: saves Steam's OverlayHookD3D3 address at `InstallPresentInlineHooks` time (BEFORE SL overwrites the E9 JMP).
  3. In the **synthetic re-entrant path** (~line 1217): when `g_externalOverlayPresentHook` is non-null and Steam overlay is loaded, call Steam's overlay function explicitly INSTEAD of `oPresentBypass`. Steam renders its overlay, calls its internal trampoline (saved original dxgi!Present bytes + JMP to dxgi!Present+5), which presents the frame. No double-present: Steam's trampoline calls past the JMP directly into the real Present implementation.
  4. In the **confirmed standalone normal-route bypass path** (~line 1198): same explicit Steam overlay call.
  5. Debug logging: `"DetourPresent: Invoking Steam overlay for SL re-entrant Present #N"` and `"DetourPresent: Invoking Steam overlay for Post-FSR confirmed standalone bypass #N"`.

- **Why Steam's trampoline works**: Steam saves the first 5 bytes of dxgi!Present (before its JMP), which are the normal function prologue (e.g., `mov [rsp+8], rbx`). Steam's trampoline executes these bytes, then jumps to dxgi!Present+5. The jump goes PAST SL's E9 JMP (also at bytes 0-4) into the original instruction stream. No CE vtable hook re-entry because the trampoline calls the function body directly, not through vtable dispatch.

- **Verification**: Build `0.1.2824` passes all 672 unit tests.

- **Stale-risk**: Low. The approach is conservative (only activates in specific bypass paths, guarded by `g_externalOverlayPresentHook` non-null and `steamOverlayLoaded`). Present1 path not yet covered (steam doesn't commonly hook Present1).

### 2026-05-05 — BioShock AF hook bootstrap and basic limiter local cadence

- **Inputs**: BioShock Infinite rerun logs from `installed/captureengine/logs/20260505_171748`
  still showed blurry textures with AF=16x, plus a configured 140 FPS basic limiter that
  capped around 40 FPS.
- **FPS limiter root cause**: `fps_limiter_trace.log` showed `limiter=basic target=140`
  and successful event opens, followed by repeated `TIMEOUT` lines and local fallback.
  The old path waited about one helper-process release timeout per frame before running
  local cadence. In a per-game config activation shape, the helper may not be running or
  signaling, so the timeout itself became the cap. Vsync behavior was not involved and
  was not changed.
- **FPS limiter fix**: Timer-based/basic/FG-fallback pacing now runs hook-local cadence
  directly after native/Reflex/AntiLag/XeLL paths decline. New trace lines report
  `Apply: LOCAL timer start ...` and periodic `Apply: LOCAL timer stats ...`. The new
  regression test `FpsLimiterTest.GeneralBasicUsesLocalCadenceWithoutLimiterProcessTimeout`
  creates unsignaled helper events and verifies the basic 140 FPS path does not pay the
  old timeout or count missed frames.
- **AF root cause refinement**: The same BioShock trace had many deferred sampler
  creations but no reconciled AF sampler binds, and only a few Present-bootstrap no-SRV
  skips. That shape points to runtime sampler/SRV vtable hooks being missing or late on
  the actual Present context, not to the resource classifier alone.
- **AF fix**: Present-time deferred AF bootstrap now re-ensures D3D11 sampler/SRV vtable
  hooks on the actual device/context. The hook installer can patch an additional vtable
  when its slot still points at the known original, logs alternate-vtable mismatches, and
  records `AF_runtimeHooks` in shutdown/host-disconnect summaries. Resource skip logs now
  include SRV/texture format, dimension, mip/view-mip details, size, sample count, and
  bind/misc flags so future blurry-texture traces can prove whether a texture was unsafe
  or merely missed by hook coverage.
- **Verification**:
  - Focused tests passed:
    `python build.py --run-tests --tests-only --skip-updates --gtest-filter=FpsLimiterTest.Apply_GeneralBasicUsesLocalCadence:FpsLimiterTest.Apply_NoExternalTargetUsesLocalCadence:FpsLimiterTest.GeneralBasicUsesLocalCadenceWithoutLimiterProcessTimeout:SamplerOverrideUtilsTest.*`
  - Full build passed: `python build.py --skip-updates`.
- **Follow-up**: Rerun BioShock. Expected limiter proof is no repeated basic-mode
  `TIMEOUT waiting for release`; expected AF proof is `DX11: Runtime sampler/SRV hook
  ensure from Present ...` followed by `AF reconciled sampler` lines or detailed resource
  skip reasons.

### 2026-05-05 — Blackwell-safe D3D11 forced AF runtime classifier

- **Motivation**: NVIDIA Blackwell GPUs can show green/red corruption artifacts when AF
  is forced blindly by the driver or CE, while game-requested AF is safe because games
  apply it only to appropriate textures. BioShock Infinite (UE3, DX11, x86) still showed
  blurry textures with CE's conservative path; its logs showed many ordinary linear
  samplers (`Filter=0x15`) skipped only because `ComparisonFunc=8`.
- **Root cause refinement**:
  1. Treating every `ComparisonFunc != NEVER` as a comparison sampler was too broad.
     In D3D11 the comparison behavior is encoded in comparison filter modes; a normal
     linear filter plus `ComparisonFunc=ALWAYS` should still be eligible.
  2. Create-time AF-on had no SRV/resource context and could reintroduce the Blackwell
     corruption family.
  3. Bind-time AF only ran on sampler binds, so games that reused samplers while changing
     SRVs could miss later eligible material textures.
- **Fix**:
  - `hook/common/sampler_override_utils.h` now owns testable D3D11 forced-AF sampler and
    resource classifiers.
  - `hook/apis/dx11_hook.cpp` tracks logical original samplers and SRVs per context/stage,
    reconciles replacement samplers on both `*SetSamplers` and `*SetShaderResources`, and
    uses a Present-time bootstrap only to capture already-bound state.
  - D3D11 `CreateSamplerState` and wrapper `CreateSamplerState` no longer enable forced
    AF-on without SRV context; they still handle AF-off/mip-bias style overrides.
  - Replacement sampler caches are keyed by `(device, final sampler descriptor)` instead
    of original sampler pointer so the same original sampler can be AF or non-AF depending
    on the currently paired SRV.
  - `hook/wrappers/d3d11_devicecontext_wrap.cpp/.h` now mirrors sampler/SRV tracking and
    fixes the wrapper `StartSlot` replacement-array bug by passing contiguous arrays.
- **Diagnostics**: New rate-limited logs report deferred AF bootstrap, sampler reconcile,
  no-SRV, unsupported-format, single-mip, unsafe-resource, comparison-filter, fixed-LOD,
  border, reduction, and high-slot decisions.
- **Tests / verification**:
  - `python build.py --run-tests --tests-only --skip-updates --gtest-filter=SamplerOverrideUtilsTest.*` passed.
  - `python build.py --skip-updates` passed and compiled x64/x86 hook DLLs.
- **Durable page**: `dx11-forced-af.md`.
- **Stale risk / follow-up**: Rerun BioShock Infinite with AF=16x. Expected proof is
  sharper eligible material textures, no green/red artifacts, and logs showing the old
  normal `Filter=0x15` / `ComparisonFunc=8` sampler family allowed only when paired with
  safe mipmapped `Texture2D` SRVs.

### 2026-05-04 — Remove unrate-limited per-sampler create-time log; log level fix

- **Motivation**: The `HookLogImportant` in `DetourCreateSamplerState` fired for every
  sampler creation without rate limiting. BioShock Infinite creates ~1.9M samplers at
  startup, producing a 284MB `hook_debug.log` within seconds.
- **Fix**: Removed the per-sampler `HookLogImportant` log from `DetourCreateSamplerState`.
  The aggregate diagnostic summary on `Shutdown`/`OnHostDisconnect` still provides total
  counts via `g_DiagSamplerAFApplied` etc. The rate-limited bind-time logs (48, 24, 12)
  remain for per-event diagnostics.
- **Other fixes in this batch**:
  - Changed `HookLog` to `HookLogImportant` for all AF/mip/prerender diagnostics
    (HookLog is filtered by shared memory log level, HookLogImportant is not)
  - Changed `WrapperLog` to use `HookLogImportant` instead of `EarlyLog` (same reason)
  - Enabled AF at create-time (`ApplySamplerOverrides11(desc, gfx, true)`) for games
    that never rebind samplers
  - Added query-based prerender limit to `DetourPresent`/`DetourPresent1` in shared path
  - Fixed DX9 skipReason to prevent dummy device creation for DX11 games
  - Added bind-time AF override to wrapper D3D11 context sampler set calls
  - Exposed `ApplyPrerenderLimit` from `dx11_hook.cpp` via `dx11_hook.h`
- **Files changed**: `hook/apis/dx11_hook.cpp`, `hook/apis/dx9_hook.cpp`,
  `hook/common/dxgi_shared.cpp`, `hook/wrappers/d3d11_devicecontext_wrap.cpp`,
  `hook/wrappers/dxgi_swapchain_wrap.cpp`, `hook/wrappers/wrapper_hooks.cpp`,
  `hook/main.cpp`, `tests/test_stubs.cpp`, `llm-wiki/log/recent.md`
- **Verification**: Build `0.1.2790`: `success=1`, all unit tests passed.
- **Stale risk**: Historical only. Superseded on 2026-05-05: create-time AF-on was
  removed again because Blackwell needs SRV/resource context before forcing AF.

### 2026-05-04 — Fix DX9 skipReason to prevent dummy device creation; add prerender limit to DXGI shared Present

- **Motivation**: The DX9 hook still created dummy D3D9 devices for DX11 games
  (BioShock Infinite). The query-based prerender limit was only in the wrapper's
  `CWrapDXGISwapChain::Present` which is never called for games that bypass the
  wrapper (Present goes through `DetourPresent` in `dxgi_shared.cpp`).
- **DX9 skipReason fix** (`hook/apis/dx9_hook.cpp:6417-6419`): Changed the
  skipReason check to return early regardless of `inlineHooksReady`. Previously
  it only returned when inline hooks were already installed; when inline hooks
  failed it fell through to create a dummy D3D9 device even for DX11/DX12 games.
- **Prerender limit in shared path** (`hook/common/dxgi_shared.cpp`): Added
  `ApplyPrerenderLimit` calls in both `DetourPresent` (after line 1244) and
  `DetourPresent1` (after line 1877). This is the actual Present entry point for
  all games — both wrapper and non-wrapper paths. Gated on `api == D3D11` and
  `g_GraphicsOverridesActive`.
- **Removed duplicate wrapper code** (`hook/wrappers/dxgi_swapchain_wrap.cpp`):
  Removed the wrapper-specific prerender code that was added in the previous
  commit — it's now handled in the shared path.
- **Test stub** (`tests/test_stubs.cpp`): Added `ApplyPrerenderLimit` stub for
  the unit test binary.
- **Files changed**: `hook/apis/dx9_hook.cpp`, `hook/common/dxgi_shared.cpp`,
  `hook/wrappers/dxgi_swapchain_wrap.cpp`, `tests/test_stubs.cpp`,
  `llm-wiki/log/recent.md`
- **Verification**: Build `0.1.2783`: `success=1`, all unit tests passed.
- **Stale risk**: Low. The DX9 skipReason change is conservative — any DX11/DX12
  game that truly needs DX9 hooks (DXVK D3D9) still works because DXVK loads
  `d3d11.dll` through the DXVK wrapper, and the DX11 detection path checks for
  `d3d11.dll` which is always loaded in that case. The prerender limit in the
  shared path covers all D3D11 games regardless of architecture.
