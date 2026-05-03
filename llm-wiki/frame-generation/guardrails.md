# Frame Generation Switching

Last cross-checked: 2026-04-25

Primary sources:
- `AGENTS.md`
- `common/shared_defs.h`
- `common/config.cpp`
- `hook/common/hook_common.h`
- `hook/apis/dx12_hook.cpp`
- `hook/apis/streamline_hook.cpp`
- `hook/common/dxgi_shared.cpp`
- `hook/common/dx12_overlay_policy.h`
- `tests/test_dx12_fg_trace_replay.cpp`
- `tests/test_dxgi_shared.cpp`
- `tests/test_overlay_fg_status_publication.cpp`
- `installed/captureengine/config.ini`
- `installed/captureengine/logs/20260413_192027/hook_debug.log`
- `installed/captureengine/logs/20260423_044543/hook_debug.log`
- `installed/captureengine/logs/20260423_215138/hook_debug.log`

## Scope
This page records current guardrails and tested transition families for no-FG, DLSS FG, and FSR FG switching. The goal is generic support across games, not a pile of title-specific hacks.

## Project Guardrails
- We must be careful not to break DLSS FG and FSR FG in GTA again when fixing any FG issue in Talos.
- Talos and GTA are validation families, not excuses for game-specific runtime hacks.
- Switching FG must work gracefully in both Talos and GTA validation scenarios, in all directions and combinations, without crashes, without lost overlay rendering, and with the correct visible FG status.
- Fixes must stay generic so DLSS FG and FSR FG support scales across multiple games instead of accumulating title-specific branches.

## Facts
- The tree now has a shared FG session/planner layer in `hook/common/fg_session_state.{h,cpp}` that captures the current mixed DX12/Streamline/FFX FG state, computes a concrete `FGActionPlan`, and emits structured transition/session logs. It is currently used as the common authority for diagnostics and publication, while some low-level queue/route/transport execution still remains in the older DX12/DXGI code paths.
- `hook/common/dx12_fg_transition_model.cpp` is now a compatibility adapter over that planner-backed session snapshot instead of a separate competing reducer. Existing transition/replay tests still validate the same public transition contract, but the underlying snapshot now comes from the shared FG session layer.
- The planner currently sees explicit hook events from DX12, Streamline, FFX, and top-level Present observation for at least: authoritative Streamline startup handoff, authoritative FFX takeover, native-FSR configure on/off, PostSL callback install/remove, PostSL activation complete, PostSL first confirmed render, swapchain invalidation, Streamline runtime updates, and top-level Present observation.
- Current trace-replay tests explicitly cover:
  - Talos-style `off -> DLSS -> off -> FSR`
  - GTA-style `FSR -> DLSS` without a clean `off`
  - GTA-style DLSS suspension and resume during loading
  - startup coexistence using startup bypass first and normal routing after settle
- Current overlay status tests explicitly cover visible label switching between `FSR FG` and `DLSS FG`, plus clearing back to `FG` when frame generation turns off.
- Talos `installed/captureengine/logs/20260423_215138` / `20260423_220858` and GTA `installed/captureengine/logs/20260424_020300` showed that generic FG switching correctness also depends on CE actually observing the last Streamline feature-call edge. When Streamline export inline patching fails or a title uses newer feature entrypoints, CE still needs generic direct-import / dynamic-lookup interception for `slDLSSGSetOptions`, `slDLSSGGetState`, current Reflex `slReflexSetOptions`, and legacy `slReflexSetConstants`, and that fallback has to be armed on the actual owner module of the failing export rather than only on the core Streamline DLLs. Otherwise CE can miss either the menu-side `DLSS FG -> off` edge that clears the overlay label or the Reflex `frameLimitUs` traffic needed to diagnose stale half-rate caps after DLSS FG is disabled.
- Talos `installed/captureengine/logs/20260425_002642` on build `0.1.2584` sharpened that rule: the game turned DLSS FG off in the 2D menu, but CE only saw steady active `slDLSSGGetState` and never saw an explicit OFF edge before the user noticed the stale label. Feature-hook discovery now covers every loaded `sl.*.dll`, not just `sl.interposer.dll` / `sl.common.dll`, and direct-import fallback retries are idempotent so late-loaded/menu-side imports can be patched without corrupting the original target.
- Talos `installed/captureengine/logs/20260425_173428` on build `0.1.2587` sharpened the 2D-menu `FSR FG -> DLSS FG` path. A fresh authoritative Streamline handoff created a new runtime-owned swapchain queue and CE detected DLSS FG through active `slDLSSGGetState`, but PostSL remained dormant because `safeBootstrap=0` while no SL wrapper queue appeared in the menu. The current guard now marks that fresh handoff as pending immediately to suppress false queue-change `FSR_FG` relabels, and accepts a runtime-owned Streamline swapchain queue as safe post-FSR bootstrap evidence when it is the live command queue and its ECL submit path is already tracked.
- GTA `installed/captureengine/logs/20260425_000339` showed that a pure-DLSS enable can still produce transient `slDLSSGGetState` OFF jitter after PostSL has already confirmed and submitted more than the short frame 9-12 stale-OFF stabilization window. The current guard now keeps only GetState OFF deferred until the 30-frame PostSL warmup proof threshold; explicit `slDLSSGSetOptions(OFF)` uses the older shorter guard so user/requested disables are not broadened by the GetState-only protection.
- Explicit native-FSR off via `ffxConfigure(frameGenerationEnabled=0)` is now treated as stronger evidence than queue-change/ECL heuristics during runtime-owned teardown. That prevents `FSR_FG -> off` from immediately re-latching heuristic `FSR_FG` on the lingering runtime-owned queue and leaving the overlay status stale.
- Authoritative `SetFSRFGActive(false)` now also **clears** any pre-existing `heuristicFSRFGActive` flag. A heuristic latched during an earlier queue-change detection window must not survive into the post-FSR teardown / DLSS comeback window, or `ClassifyRuntimeMode()` would keep returning `kFSRFG` and the overlay GPU work would be skipped indefinitely.
- After a post-FSR Streamline teardown where both the swapchain queue (`scQueue`) and the last known-good PostSL queue (`lastWorkingQ`) are null, the overlay deferral policy now falls back to the primary game queue as a valid recovery signal. This prevents the overlay from being locked out for the full 600-frame grace window when the only proven-safe queue is the original game queue.
- Build `0.1.2771` adds a **DllMain startup guard** for the Present hook chain. The true root cause of the persistent Talos DLSS FG crash (0xC0000005 RIP=0) was **Present hook interference during SL's DllMain**, not earlier theories about CE wrapping DXGI factories. CE installs Present hooks via **vtable patching** on the D3D12 swapchain (because Steam overlay already has an inline E9 JMP on dxgi!Present, CE uses the vtable hook path). The D3D12 swapchain vtable is **shared by ALL swapchain objects from the same D3D12 factory**, so swapchains created by SL during its DllMain have vtable[8] pointing to CE's DetourPresent. When SL's DllMain calls Present, the call goes through CE's DetourPresent → original dxgi!Present → Steam's inline E9 JMP on dxgi!Present → `gameoverlayrenderer64!OverlayHookD3D3` → crash with RAX=0 (Steam's TLS uninitialized on the SL loader thread). The existing `ShouldForceSteamDX12Bypass` was supposed to prevent this but had a bug: `runtimeMode = kDLSSFG` set by DllMain made `!streamlineFGRunning && runtimeMode != kDLSSFG` evaluate to `false && true == false`, so the bypass was NOT applied. Build `0.1.2771` removes the `runtimeMode != kDLSSFG` condition (now always bypass when SL is loaded and FG not running), adds early bypass in `DetourPresent`/`DetourPresent1` for SL module callers during DllMain, and adds a last-resort bypass fallback in `CallOriginalPresent`.

## Open Questions / Stale-Risk
- Stale risk is high because FG switching behavior is spread across runtime classification, queue routing, startup coexistence, and visible metrics publication.
- `installed/captureengine/logs/20260411_233821` is only a short validation pass; longer GTA V Enhanced soak coverage is still worth re-checking after future queue-routing or callback-bridge changes.
- `Overlay.observer_only=true` has been validated as the clean passive baseline in `installed/captureengine/logs/20260413_203815`; the remaining runtime-validation gap is the staged `Overlay.observer_policy_only=true` probe to confirm whether Streamline startup-policy mutation alone can reintroduce the GTA V Enhanced freeze while PostSL/startup-Present behavior stays passive.
- Any future inline-hook / trampoline rewrite can silently re-break native FFX teardown paths even if the higher-level FG state machine looks correct. Re-check `hook/wrappers/inline_hook.cpp` whenever a crash lands inside the `00007FF8011D....` trampoline pool.
- Re-check this page after changes to Streamline feature-export hook coverage. Talos `20260423_215138`, `20260423_220858`, and `20260425_002642` proved that export-inline failure on `slDLSSGSetOptions` is survivable only if CE also catches the same feature through direct imports / dynamic lookups on the real owner DLL of that export, including feature DLLs discovered by loaded-module scans.
- Re-check this page after changes to post-FSR safe-bootstrap classification. Talos `20260425_173428` proved that 2D-menu `FSR FG -> DLSS FG` may have no SL wrapper traffic before leaving the menu, so the captured runtime-owned Streamline swapchain queue itself can be the only safe bootstrap proof available.
- Re-check this page after changes to Reflex hook coverage. GTA `20260424_020300` proved that `slReflexSetConstants` alone is not enough for current Streamline integrations; missing `slReflexSetOptions` leaves both limiter state and useful diagnostics invisible.
- Re-check this page after changes to source-specific `slDLSSGGetState` OFF handling. GTA `20260425_000339` proved that GetState OFF jitter and explicit SetOptions OFF need separate protection windows during pure-DLSS startup.
- Re-check this page after changes to `ProbeRealD3D12ECL` or `DX12_HookQueueVTable`. Talos `20260502_233427` proved that creating a temporary COMPUTE queue during Streamline's startup window (for the ECL probe) can crash Streamline with a null pointer call (RIP=0). The fix defers `ProbeRealD3D12ECL` while the Streamline startup window is active, skips vtable hooking on non-origGame queues during the window, guards FFX inline hooks (`ffxCreateContext`/`ffxDestroyContext`/`ffxConfigure`) from CE-side processing, and also guards `RepairVTableHooksIfNeeded()` inside `Hooked_slDLSSGGetState` from accessing the swapchain vtable (which triggers Steam's `gameoverlayrenderer64` overlay code during SL's DllMain window). The startup window was also extended from 1500ms to 4000ms because the crash happened 1440ms after arming. Any rework must preserve all four deferral guards plus the `DX12_ServiceDeferredECLProbe()` fallback.
- Re-check this page after changes to FFX module-hook refresh, `ffxConfigure` parsing/interpretation, or the native-FSR callback bridge. `installed/captureengine/logs/20260416_140309` shows that losing FFX callback/API authority can now present as a pure visibility regression rather than a crash: the runtime-owned FSR swapchain stays active, but the overlay falls back into the passive `separate overlay GPU work is unsafe` path forever.
- Re-check this page after changes to `dx12_overlay_policy.h`, FG transition tests, or overlay metrics publication behavior.
- The `20260423_044543` Talos session also surfaced a Streamline internal exception (`0x00008000` in `sl_dlss_g`) during rapid ON/OFF toggling. This is distinct from the older breakpoint-corruption and `std::_Throw_Cpp_error` families. The overlay-deferral fix may reduce its likelihood by shortening the teardown deferral window, but a generic CE-side suppression is not yet known. Re-check if future Talos/GTA traces show the same `0x00008000` exception after build `0.1.2552`.
