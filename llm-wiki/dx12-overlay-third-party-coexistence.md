# DX12 Overlay Third-Party Coexistence

Last cross-checked: 2026-05-08 (updated: build 0.1.2923 vtable unhook fix)

Primary sources:
- `hook/common/overlay_compat.h`
- `hook/common/dx12_overlay_policy.h`
- `hook/common/dxgi_shared.cpp`
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
- If the effective runtime mode is FSR FG, SL routing must stay suppressed even if the SL hook remains physically present on `Present`/`Present1`. Re-enabling SL routing in that state can deadlock the render thread inside the FFX runtime.
- Current DXGI startup pass-through windows are short and explicit: normally 3 frames, or 16 frames for Steam when bypass is available.
- The current tests and comments explicitly say the dedicated DX12 overlay queue is FG-only. Startup compatibility stays on the safer single-queue path to avoid cross-queue state conflicts such as GTA `ERR_GFX_STATE` failures.
- A post-FSR `FSR_FG -> DLSS_FG` comeback can hit a distinct third-party coexistence seam from the older unsafe-bootstrap failures: CE may already have enough shared-state evidence to keep the startup family on the normal Streamline route and invoke PostSL there, while Steam's DX12 hook for the fresh swapchain still has a stale or null saved original Present pointer. In that state, falling through `oPresent` re-enters `gameoverlayrenderer64` and Steam can crash even after the first recovered PostSL render if the path is still inside the short confirmed-startup-settling window.
- The current generic rule is therefore split by concern. The startup-routing decision stays topology-driven (`keepStartupPresentOnNormalRoute`), but the actual Present transport on DX12 post-FSR comebacks uses the bypass trampoline until PostSL has both confirmed a successful render and left the short confirmed-startup-settling window. This keeps third-party overlay coexistence generic: CE does not trust Steam's fresh-swapchain vtable hook state just because the higher-level post-FSR startup family is already safe enough to continue normal-route PostSL progress.

## Working Guidance For DX12 Games With External Overlays Active
- Identify startup coexistence problems from module path, queue ownership, swapchain ownership, and call-stack evidence, not from game-specific branches.
- Treat foreign swapchains and queues as non-authoritative until the real game queue or swapchain is proven.
- Use narrow startup bypass windows, then converge back to normal routing as soon as the live game path is clear.
- When FFX stack evidence and third-party overlay identity disagree, do not blindly trust the immediate caller alone.
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

### Build 0.1.2923 — One-time vtable[8] unhook for Steam DX12 overlay init (Strange Brigade DX12 fix)
- **Problem**: Strange Brigade DX12 with Steam overlay active (no Streamline, no DLSS FG, no FSR FG) crashes on the first Present call with `0xC0000005` (RIP=0) inside Steam's OverlayHookD3D3. CE uses vtable hooking (vtable[8] = `DetourPresent`) because Steam's E9 JMP is detected on `dxgi!Present`, bypassing Steam's inline hook entirely. Steam's overlay never initializes its internal Present handler.
- **Root cause**: Steam's `OverlayHookD3D3` lazily initializes its internal "next" Present handler pointer on the first entry through its E9 JMP by reading `vtable[8]` from the swapchain. When `vtable[8]` = `DetourPresent` (CE's vtable hook function), Steam's initialization validation determines that DetourPresent is not a valid real Present function pointer and sets "next" = NULL. The overlay then crashes with RIP=0 on its first actual call site. This differs from the build 0.1.2922 assumption (which thought Steam would accept DetourPresent and call it back through the reentrancy guard).
- **Fix** (`hook/common/dxgi_shared.cpp`):
  1. Added file-scoped state vars: `s_steamDX12InitAttempted` (atomic<bool>), `s_steamInitCrashed` (bool)
  2. Added helper: `AttemptSteamDX12OverlayInit()` — performs the one-time vtable unhook → E9 JMP call → re-hook sequence:
     a. Save vtable[8] (= DetourPresent), write real `dxgi!Present` to vtable[8] (with `VirtualProtect`)
     b. Call the real dxgi!Present through `oPresent` (= dxgi!Present with Steam's E9 JMP)
     c. Steam's OverlayHookD3D3 enters through its E9 JMP, reads vtable[8] = real dxgi!Present, succeeds in initializing its internal "next" handler
     d. Steam renders overlay, calls "next" = real dxgi!Present → frame presented
     e. Restore vtable[8] to DetourPresent (with `VirtualProtect`)
  3. Modified `CallOriginalPresent()`: Replaced the non-Streamline Steam overlay block (old oPresent routing from build 0.1.2922) with two-phase logic:
     - **Phase A** (one-time init): If `!s_steamDX12InitAttempted`, call `AttemptSteamDX12OverlayInit()`. Sets `s_steamDX12InitAttempted = true`. On success, routes through `oPresent` (Steam's E9 JMP → Steam's now-initialized handler → real Present). On crash, sets `s_steamInitCrashed = true`, falls through to bypass fallback.
     - **Phase B** (steady state): If init completed, route through `oPresent` normally. If init crashed, use bypass trampoline.
  4. Modified `CallOriginalPresent1()`: Only route through `oPresent1` if Steam init completed; otherwise use bypass.
- **How it works**:
  1. First non-SL Present call enters `CallOriginalPresent`
  2. `AttemptSteamDX12OverlayInit()` temporarily restores vtable[8] to real `dxgi!Present`
  3. Calls `oPresent` (dxgi!Present with Steam's E9 JMP) — Steam's E9 JMP fires
  4. Steam reads vtable[8] = real `dxgi!Present` → init succeeds → internal "next" = real Present
  5. Steam renders overlay → calls "next" (real Present) → frame presented
  6. vtable[8] restored to `DetourPresent`
  7. All subsequent frames: `CallOriginalPresent` → `oPresent` → Steam's E9 JMP → Steam's now-initialized "next" handler (real Present) → frame presented with both overlays
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

## Open Questions / Stale-Risk
- Stale risk is high because this area depends on call stacks, queue ownership, and third-party module behavior that can change without warning.
- Module-token detection is heuristic. Re-check it whenever new overlay modules appear in traces or bug reports.
- Re-check SL routing suppression whenever FSR FG classification or FFX hook timing changes, because the effective runtime mode is now the authoritative guard.
- The one-time vtable unhook approach (build 0.1.2923) assumes Steam's OverlayHookD3D3 lazily initializes its internal "next" handler on first E9 JMP entry by reading vtable[8], and that the initialization validates the pointer against the real dxgi!Present. This was confirmed by the crash analysis of build 0.1.2922 (which crashed because vtable[8] = DetourPresent failed validation). If Steam's internal initialization mechanism changes (e.g. reads a different vtable slot, uses a non-vtable mechanism, or changes validation criteria), this fix may need revision. The bypass trampoline fallback and `s_steamInitCrashed` flag remain as crash-safe last resorts.
- The vtable[8] fixup + direct Steam handler call approach (build 0.1.2908) is SUPERSEDED by the oPresent routing approach (build 0.1.2922), which is in turn SUPERSEDED by the one-time vtable unhook approach (build 0.1.2923). Neither earlier approach must be restored. The oPresent routing approach incorrectly assumed Steam would accept DetourPresent as a valid forwarding target during initialization; the vtable fixup approach incorrectly assumed Steam reads vtable[8] at call time rather than during E9 JMP initialization.
