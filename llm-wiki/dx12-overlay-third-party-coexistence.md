# DX12 Overlay Third-Party Coexistence

Last cross-checked: 2026-05-31 (updated: build 0.1.3612 / tests 0.1.3613 Steam DX12 null-callback recovery slot hardening)

Primary sources:
- `hook/common/overlay_compat.h`
- `hook/common/dx12_overlay_policy.h`
- `hook/apis/dx12_hook.cpp`
- `hook/apis/ffx_hook.cpp`
- `hook/main.cpp`
- `hook/common/dxgi_shared.cpp`
- `tests/test_dxgi_shared.cpp`
- `tests/test_fps_limiter.cpp`

## Scope
This page records the current repo knowledge for making our DX12 overlay work well when other external overlays are active, including Steam, Rockstar Social Club, Epic EOS, and similar third-party overlay layers.

## Facts
- The tree currently identifies known third-party overlays primarily by module-path tokens such as `gameoverlayrenderer`, `discord_hook`, `socialclub`, `eosovh`, `eossdk_win64_shipping`, `nvspcap`, `nvoverlay`, `rtsshooks`, and `specialk`.
- A smaller startup-blocking subset is tracked separately. Current tokens include `socialclub`, `eosovh`, and `eossdk` variants.
- If a third-party overlay is already loaded before the real D3D12 device exists, the DX12 policy can defer early temporary-swapchain `Present` hook installation to avoid recursion and stack-overflow startup failures.
- Startup overlay compatibility is driven by observed overlay and runtime state, not by process name.
- During startup compatibility mode, overlay rendering is only considered safe once a live swapchain queue is known and the swapchain is no longer runtime-owned, or the late pre-FG runtime-owned handoff has remained stable long enough to treat that queue topology as settled.
- A few successful startup overlay draws on the original game queue are not by themselves proof that startup compatibility is over. If a startup-blocking overlay is still loaded and a later pre-FG runtime-owned swapchain handoff appears before any real FG activation has been observed, startup compatibility must re-arm and keep the conservative suppression path active through that handoff.
- Once that late pre-FG runtime-owned handoff has settled enough to render again, startup compatibility must still stay active until real FG is observed or runtime ownership returns to the normal non-runtime path. Otherwise a single successful draw on the handoff queue immediately drops CE back onto the normal coexistence path on the next frame even though startup bootstrap is still in progress.
- That late pre-FG runtime-owned handoff re-arm must be keyed off the handoff edge itself, not the steady-state `runtimeOwnsSwapchain` flag. Otherwise CE can re-arm startup compatibility every frame for as long as the runtime-owned swapchain remains present, repeatedly resetting staged startup activation even after the topology has already settled enough to render safely.
- Third-party overlay swapchains and private queues are not allowed to become authoritative game state just because they call into our hooks.
- If an immediate caller looks like a third-party overlay but FFX FG stack or module evidence is present, the FFX evidence can override the misleading caller identity.
- Dynamic `GetProcAddress` caller filtering has a narrow FFX exception: generic D3D/DXGI hooks are still hidden from third-party overlay callers, but `ffxCreateContext`, `ffxDestroyContext`, and `ffxConfigure` stay visible when the target module is an official FFX runtime. GTA/EOS can route native FSR startup through an overlay-looking caller, and hiding those FFX APIs prevents CE from installing the real present-callback bridge before overlay GPU work resumes.
- FFX dynamic export hooks must be registered before process-wide `GetProcAddress` interception is enabled. Otherwise an early FSR preload can call `GetProcAddress(ffxConfigure)` while the router is active but before the FFX names are registered, cache AMD's original function pointer, and later run native FSR without CE's callback bridge. `hook/main.cpp` calls `FFXHook::RegisterDynamicHooks()` before `IATHook::InitializeGetProcAddressHook()` so official AMD modules can use the IAT/dynamic route from the first preload.
- IAT/dynamic FFX routing is not sufficient for every official SDK integration. `installed/captureengine/logs/20260530_234519` showed the switch app entering protected official FFX startup and then staying quiesced while app-side FSR callbacks were firing because CE never saw `ffxConfigure`. Official AMD DX12 modules therefore also arm a guarded re-arming `ffxConfigure` VEH fallback that catches SDK dispatch-table or intra-module calls while standard inline JMP hooks remain disabled. Healthy logs include either `GetProcAddress: Intercepted FFX API ffxConfigure` or `FFX Hook: Armed VEH breakpoint for ...!ffxConfigure`, followed by `Direct FFX API confirmation established from ffxConfigure ENABLED`.
- Dynamic `GetProcAddress` filtering also has a narrow Streamline proxy exception: `CreateDXGIFactory*` exports from `sl.interposer.dll` must remain the real Streamline proxy exports. Hiding them behind CE wrappers makes the application create a CE/raw DXGI factory, prevents Streamline from owning its swapchain interposer, and can later crash the DLSS-G handoff path. This exception is only for Streamline's proxy DXGI factory exports; CE still hooks Streamline feature APIs such as `slDLSSGSetOptions` / `slDLSSGGetState` through the feature-hook paths.
- If the effective runtime mode is FSR FG, SL routing must stay suppressed even if the SL hook remains physically present on `Present`/`Present1`. Re-enabling SL routing in that state can deadlock the render thread inside the FFX runtime.
- Current DXGI startup pass-through windows are short and explicit: normally 3 frames, or 16 frames for Steam when bypass is available.
- The current tests and comments explicitly say the dedicated DX12 overlay queue is FG-only. Startup compatibility stays on the safer single-queue path to avoid cross-queue state conflicts such as GTA `ERR_GFX_STATE` failures.
- A post-FSR `FSR_FG -> DLSS_FG` comeback can hit a distinct third-party coexistence seam from the older unsafe-bootstrap failures: CE may already have enough shared-state evidence to keep the startup family on the normal Streamline route and invoke PostSL there, while Steam's DX12 hook for the fresh swapchain still has a stale or null saved original Present pointer. In that state, falling through `oPresent` re-enters `gameoverlayrenderer64` and Steam can crash even after the first recovered PostSL render if the path is still inside the short confirmed-startup-settling window.
- The current generic rule is therefore split by concern. The startup-routing decision stays topology-driven (`keepStartupPresentOnNormalRoute`), but the actual Present transport on DX12 post-FSR comebacks uses the bypass trampoline until PostSL has both confirmed a successful render and left the short confirmed-startup-settling window. This keeps third-party overlay coexistence generic: CE does not trust Steam's fresh-swapchain vtable hook state just because the higher-level post-FSR startup family is already safe enough to continue normal-route PostSL progress.
- Transition cooldowns are routing hints, not hard reasons to blank an already drawable DX12 overlay. `ShouldHeavySuspendDX12OverlayForSwapchainState(...)` only hard-suspends for zero-sized swapchains or iconic windows; ordinary swapchain/focus/FG-transition cooldowns should keep the initialized overlay visible if the backend remains valid. The expected degraded behavior during a fragile transition is a very brief visual stall, not overlay disappearance.
- Startup-overlay compatibility windows from Social/EOS/Steam-like modules should not blank an already initialized DX12 overlay. `ShouldDelayDX12OverlayRenderAfterSyncInit(...)`, `ShouldSuppressDX12OverlayRenderForLoadedStartupOverlay(...)`, and `ShouldSuppressDX12OverlayRenderForRecentBlockingRendererActivity(...)` accept backend-ready state, and `ShouldKeepDX12OverlayVisibleDuringStartupSuppression(...)` documents the invariant: once CE has a live DX12 overlay backend and sync state, compatibility suppression should stop new-risky initialization work but continue overlay submissions.
- Focus loss is not a reason to blank or rebuild a drawable DX12 overlay. For single-queue D3D12 swapchains, CE waits on its overlay fence in `focus-loss` mode before Present when the process is no longer foreground; this bounds CE's submitted overlay work while keeping the last visible overlay image alive. Wrapper-managed D3D12 `Present` and `Present1` must flush the deferred overlay fence signal after the real Present returns so the next wait observes a real fence signal.
- Steam's DX12 overlay hook can be loaded even when the user-visible Steam overlay is disabled. Current Steam builds can expose more than one lazy Present-shaped callback slot inside `gameoverlayrenderer64.dll`; `installed/captureengine/logs/20260531_141812_strangebrigadedx12crash` faulted at `steam+0x162200`, while older notes focused on `steam+0x1621d8`. CE's Steam null-call VEH recovery must resolve the exact slot from the faulting `mov rax,[rip+disp]` / `call rax` instruction and patch NULL slots to CE's DXGI bypass Present when a bypass exists. `SteamDummyRenderingCallback` is fallback-only when no bypass exists. Guarded Steam invocation still skips invalid low-address sentinels and CE dummy slots, but a bypass-patched slot is a real next-Present path and should allow Steam overlay rendering.

## Working Guidance For DX12 Games With External Overlays Active
- Identify startup coexistence problems from module path, queue ownership, swapchain ownership, and call-stack evidence, not from game-specific branches.
- Treat foreign swapchains and queues as non-authoritative until the real game queue or swapchain is proven.
- Use narrow startup bypass windows, then converge back to normal routing as soon as the live game path is clear.
- When FFX stack evidence and third-party overlay identity disagree, do not blindly trust the immediate caller alone.
- Preserve the FFX API dynamic-hook exception when tightening third-party overlay bypass rules. Losing it can make native FSR appear to work while keeping the injected overlay permanently suppressed or falling back through unsafe recovery paths.
- Do not solve coexistence by blanking an already initialized CE overlay. Prefer a route that preserves visibility and only avoids unsafe new initialization or unsafe separate GPU work. Truly non-drawable swapchains such as minimized/iconic or zero-sized surfaces are the narrow hard-stop case.
- During Alt+Tab/focus changes, prefer overlay-fence pacing over hiding/reinitializing the DX12 overlay. Healthy traces show `focus-loss mode` wait diagnostics rather than overlay suspension.
- Keep fixes generic across Steam, Rockstar, Epic, and similar overlay stacks. The code already leans toward topology and state-driven behavior; preserve that direction.

- **Vtable hook path critical difference from inline hook path**: When external E9 JMP is detected at `dxgi!Present` (inline hook), CE uses vtable hooking instead of inline hooking. In the vtable hook path, `oPresentTrampoline` is NULL (no inline hook trampoline created). `DetectSLPresentHook()` correctly bails early in the vtable path because `oPresent` (saved vtable[8]) is Steam's hook function, not dxgi!Present — checking Steam's function bytes for an E9 JMP would never detect SL's hook. SL routing (`s_slRoutingActive`) stays false in the vtable path by design, and Steam overlay is invoked through `CallOriginalPresent`'s explicit `g_externalOverlayPresentHook` logic.

- **Startup compat pass crash with vtable path + Steam overlay (build 0.1.2901 fix — no Streamline)**: When the vtable path is chosen (inline hooks skipped due to Steam's E9 JMP on `dxgi!Present`), `oPresentTrampoline` is null. The startup compatibility pass (`kPassThroughOriginal`) calls `CallOriginalPresent`, which falls through to `presentOriginal` (= `dxgi!Present` with Steam's E9 JMP). Steam's `OverlayHookD3D3` runs and tries to call its saved "next" handler via `vtable[8]` → gets `DetourPresent` (CE's detour) → resolution fails → NULL → RIP=0. This happens even without Streamline loaded. Fix: when Steam overlay is active AND `oPresentTrampoline==NULL`, the startup compat pass uses the bypass trampoline (`oPresentBypass`) directly instead of `CallOriginalPresent`. The bypass trampoline contains original `dxgi!Present` disk bytes (no E9 JMP), calling real DXGI Present directly. Source: `dxgi_shared.cpp:1636`.

- **Steam overlay invisible when SL loaded but FG not running (build 0.1.2863 - PARTIAL fix)**: When SL (Streamline) is loaded (`sl.interposer.dll` present) but Streamline FG is not running, `ShouldForceSteamDX12Bypass` returns true in `CallOriginalPresent` / `CallOriginalPresent1`. This causes the path to go directly to the disk-bytes bypass trampoline, which skips ALL inline E9 JMP hooks including Steam's overlay. Initial fix: invoked Steam overlay in `CallOriginalPresent` and `CallOriginalPresent1` before the bypass trampoline.

- **Steam overlay invisible when SL loaded (build 0.1.2866 - PROPER fix)**: The 0.1.2863 fix was insufficient because ALL Present calls were intercepted by EARLIER return paths in `DetourPresent`:
  - **Startup bypass (DllMain guard, line 1669)**: When `callerFromStreamlineModule=true && !s_slRoutingActive && steamOverlayLoaded`, the code returned early via the disk-bytes bypass trampoline. Never reached `CallOriginalPresent`.
  - **Synthetic re-entrant path (line 1283)**: After DLSS FG activation, Present calls from SL modules go through the synthetic re-entrant path. The `steamOverlaySafe` guard (line 1318) was too restrictive: it required `postSLConfirmedRendering=true`, which never happened during the PostSL warm-up phase.
  - **Confirmed standalone normal route (line 1227)**: Same restrictive `steamOverlaySafeConfirmed` guard.
  
  Proper fix (3 locations):
  1. **Startup bypass** (line 1669): Added Steam overlay invocation before the bypass trampoline, guarded by `!postSLConfirmedButStartupSettling` (prevents DllMain phase crashes). Steam's overlay hook presents the frame through Steam's own trampoline, so the bypass is not needed.
  2. **Synthetic re-entrant** (line 1318): Relaxed `steamOverlaySafe` from `!callerFromStreamlineModule || (postSLConfirmedRendering && !postSLConfirmedButStartupSettling)` to `!callerFromStreamlineModule || !postSLConfirmedButStartupSettling`. The warm-up phase (pre-confirmed rendering) is well past DllMain — SL modules are fully loaded and Steam TLS is initialized.
  3. **Confirmed standalone normal route** (line 1241): Same relaxation for `steamOverlaySafeConfirmed`.
  
  Safety: `postSLConfirmedButStartupSettling` is the single guard for DllMain safety. When true, Steam overlay is skipped (RIP=0 crash risk). When false, DllMain has completed and Steam TLS is initialized on the calling thread.
  - Primary source anchors: `dxgi_shared.cpp` ~line 1669 (startup bypass), ~line 1318 (synthetic re-entrant), ~line 1241 (confirmed standalone normal route)
  - Root cause: callFromStreamlineModule remains true for ALL Present calls when SL interposer wraps the game's Present calls, causing DetourPresent to take early bypass paths that skip Steam overlay without our explicit invoke.

## Non-SL Steam Overlay Bypass (Strange Brigade DX12 Fix)

### Build 0.1.3612/0.1.3613 — Dynamic Steam null callback slot recovery

- **Inputs**:
  - Strange Brigade DX12 `installed/captureengine/logs/20260531_141812_strangebrigadedx12crash` crashed on the second Steam E9 path after the one-time init had already patched the older `steam+0x1621d8` slot. cdb disassembly showed `OverlayHookD3D3+0x13e3f` loading a NULL function pointer from `steam+0x162200` and calling it with the `Present(swapchain, sync, flags)` signature.
  - Talos `installed/captureengine/logs/20260531_141924_talossteamoverlaydoesnotwork` did not crash, but the Steam overlay never appeared. The log showed the first guarded Steam call patching the legacy slot to CE's dummy callback, then every later frame skipped Steam because the slot was "not a real renderer".
- **Root cause**: Treating the older hardcoded Steam slot as the only relevant callback was stale. Also, patching a Present-shaped Steam slot to a no-op avoids a NULL crash but prevents Steam from chaining to the real Present path, so later policy sees only CE's dummy and bypasses Steam forever.
- **Fix**: `SteamOverlayInitVehHandler` now resolves the exact faulting Steam global slot from the call-site bytes, patches NULL slots to CE's DXGI bypass Present when available, and retries the call. The non-Streamline steady Steam E9 path is now protected by the same scoped recovery guard, not just the first init call. The no-op dummy remains only as a last fallback when no bypass trampoline exists.
- **Validation**: `python build.py --skip-updates` passed with build `0.1.3612`; `python build.py --no-build --run-tests --skip-updates` passed 830 tests with metadata `0.1.3613`. Fresh manual Strange Brigade and Talos Steam-overlay validation is still needed.
- **Source anchors**: `hook/common/dxgi_shared.cpp`, `hook/common/dxgi_shared.h`, `tests/test_dxgi_shared.cpp`, `installed/captureengine/logs/20260531_141812_strangebrigadedx12crash`, `installed/captureengine/logs/20260531_141924_talossteamoverlaydoesnotwork`.

### Build 0.1.2904 — Force bypass for non-SL Steam overlay
- When Steam overlay is loaded without Streamline or NvPresent (e.g. Strange Brigade DX12), `ShouldForceSteamDX12BypassForState` returns `true`. This routes `CallOriginalPresent` through the bypass trampoline instead of calling `oPresent` (dxgi!Present with Steam's E9 JMP), which would re-enter Steam's overlay handler and crash because `vtable[8] = DetourPresent`.
- A safety net in `CallOriginalPresent` fallback path also handles this case directly: when `!slLoaded && presentBypass && IsSteamOverlayModule`, the bypass trampoline is used.
- Source anchors: `hook/common/dxgi_shared.h:239-244`, `hook/common/dxgi_shared.cpp:3111-3128`.
- Regression test: `SteamDX12BypassForNonSLSteamOverlay` in `tests/test_dxgi_shared.cpp`.

### Build 0.1.2906 — Fix: don't invoke Steam overlay hook from forced-bypass path without Streamline
- **Problem**: `CallOriginalPresent`'s forced-bypass block (line 3035) called `TryInvokeGuardedExternalSteamOverlayPresent` for ALL cases where `ShouldForceSteamDX12Bypass` returned true, including the non-Steam-overlay-without-Streamline scenario. But Steam's overlay handler crashes when invoked without Streamline on the stack because:
  - CE uses vtable hooking (vtable[8] = `DetourPresent`)
  - Steam's handler tries to find the "next" real Present by reading vtable[8]
  - Gets `DetourPresent` → can't resolve a valid handler → calls through NULL → RIP=0
- **Fix**: Added `if (slLoaded)` guard around `TryInvokeGuardedExternalSteamOverlayPresent` in `CallOriginalPresent`. When Streamline is not loaded, skip Steam overlay invocation and use the bypass trampoline directly. The guarded Steam invocation is only safe when Streamline is on the Present stack (the `streamlineStackActive` guard in `ShouldInvokeGuardedExternalSteamOverlayPresentForState` protects against re-entrancy).
- Also added improved debug logging: explicit "skipping Steam overlay invoke" log and updated "forcing DXGI bypass" log to include `slLoaded` state.
- **Source anchors**: `hook/common/dxgi_shared.cpp:3035-3055` (call-site fix with `slLoaded` guard + logging), `tests/test_dxgi_shared.cpp` (regression test `StrangeBrigadeSteamOverlayCrashWithoutStreamline`).
- **Edge cases covered**: (a) NvPresent loaded without Streamline also benefits from the same fix, (b) no bypass trampoline case unchanged (fundamental failure), (c) inline hook path (trampoline exists) unchanged.

### Build 0.1.2920 — Missing VirtualProtect around vtable[8]/[22] fixup (Strange Brigade crash regression)

- **Problem**: Strange Brigade DX12 with Steam overlay (no Streamline/FG) crashes on first Present with `0xC0000005` (AV-WRITE) at `vtable[8]`. The game never renders, CE overlay never appears.
- **Root cause**: The vtable[8]/[22] fixup code introduced in build 0.1.2908 writes to the swapchain vtable **without `VirtualProtect`**. CE's `InstallPresentInlineHooks` made the vtable writable, wrote hooks (`DetourPresent`/`DetourPresent1`), then restored the page to read-only. When the fixup code later writes `oPresentBypass` to vtable[8], it crashes on the read-only page. Every other vtable write site in the file uses `VirtualProtect`.
- **Fix** (`hook/common/dxgi_shared.cpp`):
  1. `CallOriginalPresent` (lines 3077-3086): Wrap vtable[8] save/write/restore with `VirtualProtect(PAGE_READWRITE)`/restore. If `VirtualProtect` fails, fall through to bypass trampoline.
  2. `CallOriginalPresent1` (lines 3263-3274): Same for vtable[22].
- **Regression test**: `CallOriginalPresentVtableFixupRequiresVirtualProtect` in `tests/test_dxgi_shared.cpp` validates the pattern on a read-only simulated vtable page.
- **Source anchors**: `hook/common/dxgi_shared.cpp:3076-3102`, `:3268-3297`, `tests/test_dxgi_shared.cpp:2536-2619`.
- **Stale-risk**: Low. VirtualProtect pattern matches all other vtable write sites; regression test catches removal.

### Build 0.1.2908 (SUPERSEDED by 0.1.2922) — Steam overlay visible: invoke directly with vtable[8] fixup (non-SL case)
- **Problem**: The 0.1.2906 fix prevented the crash but also made Steam overlay permanently invisible in the non-Streamline case. The bypass trampoline jumped over Steam's E9 JMP entirely.
- **Original fix** (`hook/common/dxgi_shared.cpp`): In `CallOriginalPresent`'s forced-bypass block, when `slLoaded=0`, invoke Steam's overlay handler directly with vtable[8] fixup:
  1. Save vtable[8], set it to the bypass trampoline (valid forwarding target)
  2. Increment `s_externalOverlayPresentInvokeDepth` (activates recursion guard)
  3. Call `g_externalOverlayPresentHook` directly
  4. Restore vtable[8], decrement depth
  5. Return Steam's HRESULT
- **WHY SUPERSEDED**: The vtable[8] fixup + direct Steam handler call approach incorrectly assumed Steam reads vtable[8] from the swapchain object at call time. In practice, Steam's OverlayHookD3D3 caches the "next" Present handler pointer INTERNALLY when its E9 JMP is first triggered through the natural dxgi!Present entry point. Since CE uses vtable hooking (bypassing Steam's E9 JMP on dxgi!Present), the cached pointer was never initialized and remained NULL → RIP=0 crash on first Present even with the vtable[8] fixup. Replaced by oPresent (E9 JMP) routing in build 0.1.2922.
- **Source anchors**: Same files, superseded by 0.1.2922 code.
- **Stale-risk**: SUPERSEDED. Do not restore the vtable[8] fixup + direct Steam handler call approach.

### Build 0.1.2922 (SUPERSEDED by 0.1.2923) — Steam overlay via oPresent (E9 JMP) routing for non-SL Steam overlay (Strange Brigade DX12 fix)
- **Problem**: Strange Brigade DX12 with Steam overlay (no Streamline, no FG) crashed on first Present with `0xC0000005` (RIP=0) inside Steam's OverlayHookD3D3.
- **Root cause (superseded by 0.1.2923 analysis)**: The 0.1.2922 analysis incorrectly assumed Steam's OverlayHookD3D3 reads vtable[8] and successfully uses DetourPresent as a forwarding target. In reality, Steam lazily initializes its internal "next" Present handler on first E9 JMP entry by reading vtable[8], and the initialization VALIDATES the pointer — if it points to anything other than the real dxgi!Present (e.g. DetourPresent), Steam's validation fails and sets "next" = NULL → RIP=0 crash.
- **Fix**: Routing through `oPresent` (dxgi!Present with Steam's E9 JMP) with the expectation that Steam would initialize its internal pointer from vtable[8] (= DetourPresent) and call it back into DetourPresent's reentrancy guard.
- **WHY SUPERSEDED**: The 0.1.2922 approach crashes because Steam's OverlayHookD3D3 does NOT accept DetourPresent as a valid "next" handler during initialization. Steam reads vtable[8], finds DetourPresent, validation fails, sets "next" = NULL, and the crash occurs inside Steam's overlay code before any reentrancy guard can catch it. The oPresent → DetourPresent → reentrancy guard → bypass chain never completes because Steam's internal init fails immediately on first entry. Replaced by the one-time vtable unhook approach in build 0.1.2923.
- **Source anchors**: `hook/common/dxgi_shared.cpp:3063-3118` (original fix), `:3254-3304` (original Present1 fix).
- **Stale-risk**: SUPERSEDED. Do not restore the oPresent routing approach — it relies on an incorrect assumption about Steam's initialization mechanism.

### Build 0.1.2923 (SUPERSEDED by 0.1.2928) — One-time vtable[8] unhook for Steam DX12 overlay init (Strange Brigade DX12 fix — DID NOT WORK)
- **Problem**: Strange Brigade DX12 with Steam overlay active (no Streamline, no DLSS FG, no FSR FG) crashes on the first Present call with `0xC0000005` (RIP=0) inside Steam's OverlayHookD3D3.
- **Incorrect root cause (build 0.1.2923 analysis)**: Assumed Steam's OverlayHookD3D3 reads vtable[8] to find a "next" handler on first E9 JMP entry, and that DetourPresent fails validation → "next" = NULL → crash.
- **Fix that did NOT work**: Temporarily restored vtable[8] to dxgi!Present, called through E9 JMP, re-hooked. The fix IS executing (confirmed by hook_debug.log) but the crash STILL happens.
- **WHY SUPERSEDED**: cdb disassembly of the crash dump proved the crash is from a **NULL global function pointer** in Steam's overlay data section at RVA ~0x1621d8 in gameoverlayrenderer64.dll, NOT from reading vtable[8]. The instruction at `OverlayHookD3D3+0x1417f` loads `rax = qword ptr [VulkanSteamOverlayProcessCapturedFrame+0x9b378]` (rax = 0 = NULL) and calls `rax` without NULL check. This is an internal Steam overlay rendering callback that was never initialized. Replaced by temp swapchain pre-init in build 0.1.2928.
- **Stale-risk**: SUPERSEDED. The vtable[8] unhook approach alone does NOT fix the crash.

### Build 0.1.2928 (SUPERSEDED by 0.1.2930) — Pre-init Steam overlay on temp swapchain during hook installation (DID NOT WORK)
- **Problem**: Same as builds 0.1.2922/0.1.2923 — still crashing.
- **Root cause**: Same NULL global function pointer at RVA 0x1621d8.
- **Fix that did NOT work**: Called `vtable[8](pSwapChain, 0, 0)` on the detection temp swapchain BEFORE setting vtable[8] = DetourPresent.
- **WHY it didn't work**: The temp swapchain is 2×2 with a hidden window. Steam's OverlayHookD3D3 skips its rendering initialization path when the swapchain is not a "real" game swapchain (no visible window, no buffers). The callback at RVA 0x1621d8 remained NULL. The temp swapchain pre-init did initialize Steam's "next" handler but NOT the rendering callback.
- **Stale-risk**: SUPERSEDED. The temp swapchain pre-init alone is insufficient.

### Build 0.1.2930 — VEH-protected Steam overlay init on game swapchain (Strange Brigade DX12 fix)
- **Problem**: Same — Strange Brigade DX12 crashes on first Present with RIP=0 at `call rax` (RAX loaded from RVA 0x1621d8 = NULL).
- **Root cause** (confirmed by cdb disassembly of crash dump `crash_20260508_192218_047_pid8308_tid23364.dmp`):
  ```
  f11a533e  mov rax,qword ptr [VulkanSteamOverlayProcessCapturedFrame+0x9b378]  ; RAX = [base+0x1621d8] = 0
  f11a5345  mov r8d, ebp                                                          ; arg3 = flags
  f11a5348  mov edx, esi                                                          ; arg2 = 0
  f11a534a  mov rcx, r14                                                          ; arg1 = swapchain
  f11a534d  call rax                                                              ; call through NULL → crash
  ```
  - The NULL function pointer at RVA `0x1621d8` in gameoverlayrenderer64.dll is an internal Steam overlay rendering callback.
  - CE's vtable hook (vtable[8] = DetourPresent) prevents Steam's E9 JMP from ever firing on a real game swapchain, so the callback never gets initialized.
  - The temp swapchain pre-init (build 0.1.2928) doesn't fix this because Steam only initializes the callback when rendering on a real game swapchain with a visible window.
  - The vtable unhook safety net (build 0.1.2923, AttemptSteamDX12OverlayInit) also crashes because it calls through the E9 JMP which triggers Steam's overlay to try to render → NULL callback → crash.
- **Fix** (`hook/common/dxgi_shared.cpp`):
  1. Added `SteamDummyRenderingCallback` — a no-op callback that returns `S_OK`, serving as a safe placeholder.
  2. Added `SteamOverlayInitVehHandler` — a VEH handler that catches the NULL callback crash (RIP=0, RAX=0, return address in gameoverlayrenderer64):
     - Identifies the crash by checking RIP=0, RAX=0, and return address inside gameoverlayrenderer64.dll
     - Patches the NULL pointer at RVA 0x1621d8 to `SteamDummyRenderingCallback`
     - Fixes the context: pops the stale return address from stack, sets RAX to the dummy, sets RIP back to the `call rax` instruction
     - Returns `EXCEPTION_CONTINUE_EXECUTION` so the `call rax` retries with a valid pointer
  3. Modified `AttemptSteamDX12OverlayInit` — wraps the `presentOriginal(pSwapChain, 0, 0)` call with VEH: installs `SteamOverlayInitVehHandler` before the call and removes it after.
  4. Updated comments in `InstallPresentInlineHooks` to note that the temp swapchain pre-init only initializes Steam's "next" handler, not the rendering callback (the VEH fix in AttemptSteamDX12OverlayInit handles the real problem).
- **How it works**:
  1. First non-SL Present call → `CallOriginalPresent` → `AttemptSteamDX12OverlayInit`
  2. vtable[8] restored to dxgi!Present
  3. VEH handler installed
  4. Called through E9 JMP (`presentOriginal`) → Steam fires → hits NULL callback → crash
  5. VEH handler catches the crash, patches RVA 0x1621d8 to `SteamDummyRenderingCallback`, retries
  6. `call rax` retries with valid pointer → dummy executes → returns S_OK → Steam continues
  7. Steam completes its overlay processing → Present returns to AttemptSteamDX12OverlayInit
  8. VEH handler removed, vtable[8] re-hooked to DetourPresent
  9. Subsequent frames route through E9 JMP normally — callback is no longer NULL (Steam may overwrite the dummy with its own function during the first E9 JMP entry, or the dummy stays as a safe no-op that prevents crashing)
- **Architecture support**: The VEH handler is compiled for both x64 and x86 (uses `#ifdef _WIN64` for Rip/Rax/Rsp vs Eip/Eax/Esp register names, and different Steam DLL names).
- **Source anchors**: `hook/common/dxgi_shared.cpp:305-381` (dummy callback + VEH handler), `:3236-3238` (VEH-wrapped call in AttemptSteamDX12OverlayInit).
- **Verification**: All 696 unit tests pass build 0.1.2930.
  8. Clean flow: DetourPresent → CallOriginalPresent → oPresent → Steam overlay → real Present. No re-entrancy into DetourPresent.
- **Why this is safe**:
  - One-time setup: The vtable unhook/re-hook happens only on the very first non-SL Steam overlay Present call
  - VirtualProtect is used for all vtable writes (matches existing pattern from all other vtable write sites)
  - If Steam's E9 JMP is NOT present (oPresent == dxgi!Present), the init call just calls real Present directly — no harm, no Steam overlay
  - `s_steamInitCrashed` flag provides crash-safe fallback: if init crashes (e.g. Steam overlay removed at runtime), subsequent calls use bypass trampoline
  - Thread safety: `s_steamDX12InitAttempted` is atomic. Only one thread performs the init. Other threads arriving during init wait for `s_steamDX12InitAttempted = true` and will use bypass if init crashed.
  - No re-entrancy into DetourPresent: after init, Steam's "next" handler points to real Present, not DetourPresent
  - No stack overflow: at most one level of reentrancy during the init phase (DetourPresent → CallOriginalPresent → init helper → oPresent → Steam → real Present → return)
- **Fallback**: If `AttemptSteamDX12OverlayInit()` crashes (Steam overlay removed or init fails silently), `s_steamInitCrashed = true` and all subsequent calls use bypass trampoline (Steam overlay not visible, but game doesn't crash).
- **Source anchors**: `hook/common/dxgi_shared.cpp` (AttemptSteamDX12OverlayInit, CallOriginalPresent init block), `tests/test_dxgi_shared.cpp` (SteamDX12InitVtableUnhookRestorePattern, SteamDX12InitVtableRehookFailureSafety).
- **Verification**: All unit tests pass. Regression tests cover the VirtualProtect unhook → call → re-hook pattern on read-only vtable pages (SteamDX12InitVtableUnhookRestorePattern) and safe behavior if re-hook fails (SteamDX12InitVtableRehookFailureSafety).

### Build 0.1.2948 — vtable[8] restore before Steam overlay invoke (Strange Brigade DX12)

- **Problem**: Build 0.1.2947 bypass-only confirmed working (game content + CE overlay, no Steam overlay). Need to re-enable Steam overlay without black screen.
- **New root cause hypothesis**: Steam's DX12 overlay handler (`gameoverlayrenderer64!OverlayHookD3D3`) may internally call `pSwapChain->Present()` as part of its hook chain protocol (e.g., post-overlay fence wait and Present sequencing). With vtable[8] = `DetourPresent` (CE's vtable hook), such internal Present calls re-enter CE → `CallOriginalPresent` → either explicit Steam invoke (recursive) or bypass trampoline. The bypass trampoline skips Steam's E9 JMP entirely, breaking Steam's expected "next" handler chain. Steam's overlay commands may be submitted but never properly sequenced with the Present call, leading to buffer corruption.
- **Fix**: Before invoking `TryInvokeGuardedExternalSteamOverlayPresent`, temporarily set vtable[8] back to the original `dxgi!Present` (which has Steam's E9 JMP). After Steam's handler returns, re-hook to `DetourPresent`. This ensures Steam's internal Present calls flow through the natural E9 JMP → Steam handler (re-entrant) → Steam's saved "next" → real Present body.
- **Invariant**: The vtable[8] restore/re-hook window is per-frame and microsecond-scale. If vtable[8] was modified by another component during Steam's handler execution, the re-hook is skipped (logged).
- **Fallback**: If `TryInvokeGuardedExternalSteamOverlayPresent` is declined (guard conditions not met), use bypass trampoline which preserves game content + CE overlay but disables Steam overlay.
- **Source anchors**: `hook/common/dxgi_shared.cpp:3507-3537` (Phase A: vtable restore), `3540-3562` (Phase B: Steam invoke), `3565-3601` (Phase C: vtable re-hook), `3608-3619` (Phase E: bypass fallback).
- **Pending test**: Strange Brigade DX12 with Steam overlay. Check all three outcomes: (1) all overlays visible, (2) bypass fallback with CE+game visible, (3) black screen (further analysis needed).

### Build 0.1.2960-2963 — ECL-hook-based deferred CE overlay submission (Strange Brigade DX12 black screen fix)

- **Problem**: ALL approaches to invoking Steam's DX12 overlay handler (E9 JMP, explicit invoke, vtable restore, Steam-only experimental) produce black game content. Build 0.1.2949 diagnostic logging confirmed Steam's handler ONLY submits an ECL to the game queue — no Present calls, no buffer modifications, no code byte changes. The hypothesis is that Steam's overlay ECL clears/overwrites the backbuffer, erasing both game content and CE's overlay ECL (submitted before Steam invoke via `ProcessFrame`).
- **The ECL-hook approach**: Defer CE overlay ECL submission to `DetourExecuteCommandLists`, which fires AFTER Steam's overlay ECL on the same queue. Queue order: [Steam ECL] [CE overlay ECL] [fence signal] [Present].
- **Implementation**:
  1. `g_deferOverlaySubmitToSteamECL` flag — set by `DetourPresent` for non-SL Steam path
  2. `g_steamDeferredOverlay` state struct — captures cmdList, allocIdx, eclQueue, pending
  3. `ProcessFrame` — when flag set, records overlay commands and closes list but skips ECL submission + fence signal (jumps to `skip_steam_deferred_fence_signal`)
  4. `SubmitSteamDeferredOverlay` — submits deferred ECL via `g_RealD3D12ECL` or `GetOriginalExecuteCommandLists` (per-queue original, avoids recursion into `DetourExecuteCommandLists`), signals fence
  5. `DetourExecuteCommandLists` — detects Steam overlay ECL via `IsSteamOverlayModulePath("gameoverlayrenderer")`, calls `SubmitSteamDeferredOverlay(pThis, "ecl_hook")` after original ECL completes
  6. `DetourPresent` — skip normal fence wait before Present when deferred; after `CallOriginalPresent`, submit fallback if still pending (Steam never called ECL), then wait fence, clear flag
  7. Non-hook build stubs in `dxgi_shared.cpp` — `ResolveDX12SetDeferOverlay`, `ResolveDX12SubmitSteamDeferredOverlay`, `ResolveDX12IsDeferOverlayPending` via `GetModuleHandleA`/`GetProcAddress`
- **Key design decisions**: Automatic (no env vars), non-SL only (`steamOverlayLoaded && !IsSLInterposerLoaded()`), `GetOriginalExecuteCommandLists` over vtable fallback, fallback safety for frames where Steam doesn't call ECL.
- **Diagnostic logging**: "non-SL Steam path — deferring overlay" with SyncInterval/Flags, "Deferring overlay ECL submit to Steam ECL hook #N", "ECL hook detected Steam with deferred overlay pending" vs "no deferred overlay pending", "Submitting Steam-deferred overlay ECL to queue %p (cmdList=%p, allocIdx=%d)", "Deferred overlay submitted #N (queue=%p, fence=%llu)", fallback submit log, "Post-Steam fence wait took X us (wasPending=%d)", fence-already-complete with mode info.
- **Source anchors**: `hook/apis/dx12_hook.cpp` (~line 955-1080: state struct, exports, SubmitSteamDeferredOverlay, IsSteamOverlayModulePath, ProcessFrame skip logic, DetourExecuteCommandLists ECL hook detection), `hook/common/dxgi_shared.cpp` (~line 1970-2150: deferral flag set, skip fence wait, post-CallOriginalPresent fallback + fence wait + clear).
- **Verification**: Build 0.1.2963 compiles, all 696 unit tests pass.
- **Open questions / stale-risk**:
  - ECL hook fires ~1 in 50+ frames — fallback path submits after Present, too late for current frame
  - Even when ECL hook DOES fire, black screen may persist — root cause may differ from ECL backbuffer clearing
  - No D3D12 debug layer / GPU validation active — potential resource state mismatches between Steam's ECL and CE's overlay ECL go uncaught
  - Steam's ECL may leave backbuffer in non-PRESENT state, making CE's `PRESENT→RT` barrier incorrect
- **Pending**: Strange Brigade DX12 testing with build 0.1.2963.

### Build 0.1.2947 — Black screen: ALL Steam handler invoke approaches fail — bypass-only fallback (Strange Brigade DX12)

- **Problem**: Build 0.1.2943 (explicit Steam overlay invoke) still produces black game content. Log confirms `TryInvokeGuardedExternalSteamOverlayPresent` IS called and invokes Steam's handler (`hook=00007FFF8725058A`), but the frame is black.
- **Root cause (refined)**: Three different approaches to invoking Steam's overlay handler ALL produce black game content:
  1. E9 JMP path: calling `dxgi!Present` (with Steam's E9 JMP)
  2. Explicit invoke: calling `g_externalOverlayPresentHook` (Steam's handler directly)
  3. Vtable restore: temporarily setting vtable[8] = dxgi!Present, calling Present, re-hooking

  Even when Steam's overlay renders nothing and just calls "next" to present, the game content on the backbuffer is lost. The most likely cause: Steam's init Present call in `AttemptSteamDX12OverlayInit` alters the GPU buffer state on the real game swapchain, changing how CE's subsequent PRESENT→RT barrier behaves. Some GPU drivers may discard/clear backbuffer contents on PRESENT→RT transition when the buffer's internal state tracking was modified by Steam's init.
- **Current approach**: Bypass-only for non-SL Steam path. The bypass trampoline calls `dxgi!Present+5` directly, skipping both CE vtable hook and Steam E9 JMP. Game content + CE overlay visible, but NO Steam overlay.
- **Pending solutions (to explore)**:
  - **Chain inline hook**: Install inline hook on `dxgi!Present` WITH chain to Steam's E9 JMP, avoiding vtable modification entirely. Risk: CE and Steam fight over `dxgi!Present` entry bytes.
  - **PE-read COM method**: Read original `IDXGISwapChain::Present` COM method from `dxgi.dll` PE `.rdata` section, find the method that does kernel state management before calling `dxgi!Present`.
  - **Skip CE overlay when Steam is active**: Let Steam own the Present call, render CE overlay separately via a different GPU queue or post-present mechanism.
  - **Separate overlay device/queue**: Create a separate D3D12 device and command queue for CE overlay rendering that doesn't touch the game swapchain buffers; composite via shared textures.
- **Source anchors**: `hook/common/dxgi_shared.cpp:3499-3515`.
- **Verification**: All 696 unit tests pass. Build 0.1.2947.

### Build 0.1.2943 — Black screen fix (corrected): explicit Steam overlay invoke instead of E9 JMP — s_originalVtable8Present was a no-op (Strange Brigade DX12)

- **Problem**: Build 0.1.2942 (with the vtable[8] COM method fix) still produced black screen. Log confirmed `s_originalVtable8Present=00007FFF86A4D9F0 (same=1)` — the saved vtable[8] was identically `dxgi!Present` (the inner function). **There is no separate COM method.** The build 0.1.2941 fix was a complete no-op.
- **Corrected root cause**: For this DX12 game, vtable[8] IS `dxgi!Present` (the inner function) — there is no COM wrapper method. Both `s_originalVtable8Present` and `presentOriginal` (= `oPresent`) point to the same address. Calling `dxgi!Present` (with Steam's E9 JMP) from `CallOriginalPresent` enters Steam's overlay handler via the inline JMP hook. The internal path Steam takes when entered through its E9 JMP on the function body differs from the path taken when invoked as a standalone hook target (`g_externalOverlayPresentHook`). The E9 JMP entry path produces black game content — the exact GPU-level mechanism is complex but confirmed by repeated testing.
- **Fix**: Replace the `s_originalVtable8Present` / E9 JMP path with `TryInvokeGuardedExternalSteamOverlayPresent`. This calls Steam's overlay handler directly via `g_externalOverlayPresentHook` (the resolved E9 JMP target, saved during `InstallPresentInlineHooks` at line 2992-2996). Steam renders its overlay, calls "next" (original dxgi!Present body or re-entrant DetourPresent → bypass), and presents normally. CE's overlay submission + fence wait happens in `DetourPresent` before `CallOriginalPresent`.
- **Why the delegation path doesn't block**: `CWrapDXGISwapChain::Present` sets `g_InWrapperPresent = false` before delegating to the detour hook (line 917 in `dxgi_swapchain_wrap.cpp`), so `ShouldInvokeGuardedExternalSteamOverlayPresentForState` does not reject the call.
- **Fallback**: bypass trampoline (game content + CE overlay visible, Steam overlay dropped for that frame).
- **Source anchors**: `hook/common/dxgi_shared.cpp:3475-3516` (non-SL explicit Steam invoke), `dxgi_swapchain_wrap.cpp:916-920` (g_InWrapperPresent = false during delegation).
- **Verification**: All 696 unit tests pass. Build 0.1.2943.

## Open Questions / Stale-Risk
- Stale risk is high because this area depends on call stacks, queue ownership, and third-party module behavior that can change without warning.
- Module-token detection is heuristic. Re-check it whenever new overlay modules appear in traces or bug reports.
- Re-check SL routing suppression whenever FSR FG classification or FFX hook timing changes, because the effective runtime mode is now the authoritative guard.
- The one-time vtable unhook approach (build 0.1.2923) assumes Steam's OverlayHookD3D3 lazily initializes its internal "next" handler on first E9 JMP entry by reading vtable[8], and that the initialization validates the pointer against the real dxgi!Present. This was confirmed by the crash analysis of build 0.1.2922 (which crashed because vtable[8] = DetourPresent failed validation). If Steam's internal initialization mechanism changes (e.g. reads a different vtable slot, uses a non-vtable mechanism, or changes validation criteria), this fix may need revision. The bypass trampoline fallback and `s_steamInitCrashed` flag remain as crash-safe last resorts.
- The vtable[8] fixup + direct Steam handler call approach (build 0.1.2908) is SUPERSEDED by the oPresent routing approach (build 0.1.2922), which is in turn SUPERSEDED by the one-time vtable unhook approach (build 0.1.2923). Neither earlier approach must be restored. The oPresent routing approach incorrectly assumed Steam would accept DetourPresent as a valid forwarding target during initialization; the vtable fixup approach incorrectly assumed Steam reads vtable[8] at call time rather than during E9 JMP initialization.
