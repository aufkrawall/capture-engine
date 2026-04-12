# Frame Generation Switching

Last cross-checked: 2026-04-12

Primary sources:
- `AGENTS.md`
- `tests/test_dx12_fg_trace_replay.cpp`
- `tests/test_overlay_fg_status_publication.cpp`
- `hook/common/dx12_overlay_policy.h`

## Scope
This page records current guardrails and tested transition families for no-FG, DLSS FG, and FSR FG switching. The goal is generic support across games, not a pile of title-specific hacks.

## Project Guardrails
- We must be careful not to break DLSS FG and FSR FG in GTA again when fixing any FG issue in Talos.
- Talos and GTA are validation families, not excuses for game-specific runtime hacks.
- Switching FG must work gracefully in both Talos and GTA validation scenarios, in all directions and combinations, without crashes, without lost overlay rendering, and with the correct visible FG status.
- Fixes must stay generic so DLSS FG and FSR FG support scales across multiple games instead of accumulating title-specific branches.

## Facts
- Current trace-replay tests explicitly cover:
  - Talos-style `off -> DLSS -> off -> FSR`
  - GTA-style `FSR -> DLSS` without a clean `off`
  - GTA-style DLSS suspension and resume during loading
  - startup coexistence using startup bypass first and normal routing after settle
- Current overlay status tests explicitly cover visible label switching between `FSR FG` and `DLSS FG`, plus clearing back to `FG` when frame generation turns off.
- Current DX12 overlay policy code treats a runtime-owned swapchain alone as insufficient proof of FSR FG. That matters because GTA can keep a runtime-owned swapchain during non-FSR windows, and misclassifying that state can break post-FSR or post-DLSS recovery.
- Current DX12 overlay policy comments repeatedly preserve queue and recovery choices specifically to avoid Talos device-removed paths and stale-overlay teardown behavior during mixed FSR and DLSS transitions.
- Effective FSR runtime mode is now also used as the guard for SL routing suppression. That prevents a stale SL hook from re-activating Present routing after FSR takeover.
- Runtime-owned native FSR FG must not use the old separate injected DX12 `FG overlay SUBMIT` path on the FFX-owned queue. That path was the one observed freezing in GTA V Enhanced.
- Native FSR FG now has a runtime-cooperative path: CE chains FFX's own `presentCallback` from `ffxConfigure()` and renders the overlay on the FFX-supplied command list / output surface instead of issuing a separate injected queue submission.
- The FFX callback bridge preserves the runtime's own default composition callback when the game does not provide one, then renders CE's overlay on top of the callback's output surface.
- Short-run GTA V Enhanced validation in `installed/captureengine/logs/20260411_233821` confirms the current runtime-owned native FSR path no longer reproduces the immediate crash with the overlay active: the log shows authoritative FFX takeover, `FG publication` switching to `runtime=FSR_FG active=1`, repeated `DX12: FFX present callback rendered overlay on runtime-owned FSR path` entries, and no non-zero device-removal, watchdog-fire, or crash markers.
- That confirmation is still scoped to a short manual run. It proves the current approach survives the previously failing GTA V Enhanced overlay + FSR FG case, but it is not yet a long-soak guarantee.
- GTA V Enhanced `FSR -> DLSS` can drive native FFX teardown through `ffxDestroyContext()` while the inline-hook trampoline is still in use. The `20260412_011505` crash bundle showed the destroy trampoline at `00007FF8011D0300` copying `test rcx, rcx; jne +0x0B` from `amd_fidelityfx_dx12.dll` without relocating that short branch. When GTA destroyed the FFX context during the handoff, the taken `jne` landed inside the trampoline's appended `FF 25` jump stub data and crashed with a null write at `00007FF8011D031B`.
- Inline-hook trampolines must therefore treat short control transfers that leave the copied block as relocations, not raw memcpy. This is a generic trampoline correctness invariant, not a GTA-specific rule.
- GTA V Enhanced DLSS FG startup can route `CreateSwapChainForHwnd` through EOS startup-overlay frames even when the live swapchain/queue actually belongs to Streamline. In `installed/captureengine/logs/20260412_014238`, CE classified the new DLSS startup swapchain for the game HWND as an EOS third-party overlay swapchain and skipped normal swapchain side-effects, then later saw `OFF->ON->OFF->ON` DLSS FG churn followed by `Present STALLED` warnings. The generic fix is to let Streamline frame-generation stack evidence override startup-overlay caller identity during create-swapchain classification, just as native FFX stack evidence already does for FSR.
- GTA V Enhanced can also perform a later pre-FG runtime-owned swapchain handoff after CE has already observed a few stable startup overlay draws on the original game queue. In `installed/captureengine/logs/20260412_022704`, CE marked startup compatibility as settled on the first successful overlay submit, then EOS/Streamline handed the live swapchain to a non-game queue before any real FG activation occurred. CE no longer considered startup compatibility active at that point, immediately ran the normal overlay path on the new runtime-owned queue, and GTA raised `ERR_GFX_STATE`.
- The generic fix is to let startup compatibility re-arm when a startup-blocking overlay is still present, the session has not yet observed any real FG activity, startup had previously been marked settled, and a later runtime-owned swapchain handoff appears. That keeps the conservative single-queue startup suppression active through the full startup handoff without re-entering startup-only behavior for later normal post-startup popups or FG suspension windows.
- In the later `installed/captureengine/logs/20260412_023846` bundle, the same late pre-FG runtime-owned handoff did not crash immediately after the re-arm, but the overlay still disappeared before DLSS FG became active. The log shows CE resuming after startup-overlay warm-up while `ProcessFrame` continues routing on the runtime-owned non-FSR queue for almost a minute without any further overlay submit logs. The generic correction is to keep startup compatibility logically active through that whole late pre-FG runtime-owned window while allowing rendering again once the handoff queue has actually settled, instead of requiring runtime ownership to disappear entirely.
- The same `20260412_023846` bundle also showed `OFF->ON->OFF->ON` Streamline churn followed by repeated `Present STALLED` warnings. Fresh DLSS reactivation now clears stale Streamline-off teardown grace before the new activation, so CE no longer keeps ignoring newly resurfacing wrapper/direct queues from the old OFF transition during the next ON transition.
- In `installed/captureengine/logs/20260412_034953`, the late pre-FG runtime-owned swapchain handoff still re-armed startup compatibility on the correct queue, but the re-arm condition was level-triggered instead of handoff-edge-triggered. Once CE had settled and resumed rendering on the original game queue, `ProcessFrame` immediately re-armed startup compatibility again on the next frame because the runtime-owned swapchain was still present. That produced thousands of repeated `Re-arming startup overlay compatibility after late runtime-owned swapchain handoff before any real FG activity` / `Startup overlay probe complete - rendering stably` cycles right up until DLSS activation instead of converging once per handoff.
- The same `20260412_034953` bundle also showed that Streamline stall detection was only watching `DetourPresent` traffic. If the runtime/game moved forward through `Present1`, CE could falsely conclude that Present had stalled even though top-level present traffic was still alive. The generic stall signal therefore needs aggregate top-level DXGI present traffic, not `Present`-only traffic.
- The `20260412_034953` automatic freeze dump further showed that the 30-frame Streamline stall dump request was executing synchronously on the target present thread itself (`targetTid=18308`, same as the calling thread), and the later dump captured that thread inside `capture_hook_x64!DX12_WaitForOverlayCompletion` while dump capture was in progress. Immediate dump requests targeting the current thread must therefore be deferred to the watchdog thread; otherwise the diagnostic path itself can become the freeze.
- In `installed/captureengine/logs/20260412_133431`, CE got past the earlier wrapped-startup classification bug and the repeated startup re-arm churn, but DLSS FG still hit a startup-time `OFF->ON->OFF->ON` sequence. The hook log shows a fresh authoritative Streamline runtime-owned swapchain handoff at `13:36:03.404-13:36:03.430`, then `Streamline Hook: FG state transition OFF->ON via GetState`, a synthetic Streamline re-entrant Present, `ON->OFF via GetState`, and only afterward the explicit `OFF->ON via SetOptions` activation. The freeze dump for that run puts the target thread inside `sl_dlss_g` throwing a C++ exception rather than inside CE's dump path, so the bad transition happened before the watchdog dump machinery intervened.
- The generic correction is to treat a newly observed authoritative Streamline runtime-owned swapchain handoff as a short fresh-activation suppression window for `slDLSSGGetState`-only DLSS FG activation while the runtime still reports `Off` / `StreamlineNoFG`. Explicit `slDLSSGSetOptions` enable requests already clear that suppression immediately, so CE still converges once the real activation arrives, but it no longer lets a transient `GetState` activation race ahead of the startup handoff and collide with the later explicit enable.
- In the later `installed/captureengine/logs/20260412_135934` bundle, CE got past that earlier startup race and reached the real `OFF->ON via SetOptions` activation, but top-level `DetourPresent` traffic stopped immediately after a later authoritative Streamline runtime-owned swapchain handoff while SL wrapper ECL traffic kept flowing. The dump `GTA5_Enhanced.exe_FREEZE_2026-04-12_14-01-19_400.dmp` again puts the target present thread down inside `sl_dlss_g`, and the hook log shows the second runtime-owned swapchain came through `CreateSwapChainForHwnd INLINE` without the same Present-hook refresh that the deep/global create-swapchain paths already perform.
- The generic correction is to refresh Present hooks for newly created real DX12 swapchains in the inline `CreateSwapChainForHwnd` success path too, not just in the deep/global create-swapchain paths. A runtime-created swapchain can expose a different Present implementation or vtable than the startup swapchain, and if CE only tracks the new swapchain queue without refreshing the detour path, top-level Present traffic can disappear right when Streamline takes over that new swapchain.
- In `installed/captureengine/logs/20260412_142710`, the inline CreateSwapChainForHwnd Present-hook refresh was already in place, but GTA V Enhanced DLSS FG still froze after the `OFF->ON->OFF->ON` churn. The freeze dump `GTA5_Enhanced.exe_FREEZE_2026-04-12_14-28-54_777.dmp` shows the game's present thread (T:5C88) stuck inside `sl_dlss_g` throwing `std::_Throw_Cpp_error` (a C++ standard library threading/mutex error), with `sl_interposer` catching the exception and trying to write its own crash dump via `MiniDumpWriteDump`. The new swapchain was on a different D3D12 device (`0000019D3E6D7670`) than the game's original device (`0000019A02992040`), and during the churn CE was repeatedly installing and removing the PostSL callback, resetting the PostSL lifecycle epoch, draining overlay GPU queues, and clearing cached real-queue state. This repeated full teardown and re-init cycle on every OFF/ON transition during the startup transition window likely interfered with DLSS FG's fragile multi-device initialization threading.
- The generic correction is to keep the PostSL callback pointer installed but dormant when DLSS FG goes OFF via GetState during the startup transition window. Instead of clearing the callback pointer, waiting for in-flight callbacks, draining overlay GPU work, and resetting the lifecycle epoch, CE just disables callback execution. On the subsequent ON via SetOptions, CE re-enables the existing callback rather than re-installing it, and does not reset the lifecycle epoch. This avoids the repeated install/remove/drain cycle that can interfere with DLSS FG's initialization on multi-device configurations.
- In `installed/captureengine/logs/20260412_151606`, the dormant-callback fix was already in place, but GTA V Enhanced DLSS FG still froze after the `OFF->ON->OFF->ON` churn on a multi-device configuration (third swapchain on device `0000022B8D0C2C50` vs game's `00000228DA094240`). The freeze dump again shows the game's present thread stuck inside `sl_dlss_g` throwing `std::_Throw_C_error` (threading/mutex error). The key observation: during the OFF via GetState at 37.471ms, `g_StreamlineFGRunning` was set to `false`, changing CE's Present routing mid-initialization. Then at the second ON via SetOptions at 37.747ms, `g_StreamlineFGRunning` went back to `true`. This toggling of the routing signal during SL's fragile multi-device startup likely changed timing enough to expose the threading race in `sl_dlss_g`.
- The generic correction is to defer `g_StreamlineFGRunning` OFF updates during the Streamline startup transition window. When `ApplyCombinedStreamlineRuntimeState(false)` is called while the window is active, `g_StreamlineFGRunning` keeps its current value (true) instead of being set to false. The `DX12_OnStreamlineFGStateChanged` callback is also suppressed during this deferral, so the PostSL callback state machine stays stable. After the window expires, the next GetState or SetOptions call processes the real state normally. This keeps CE's Present routing stable throughout DLSS FG initialization churn, avoiding timing changes that can trigger threading races in SL's multi-device configuration.

## Current Practical Guidance
- Any FG fix should be reasoned through as a state-transition problem, not as a one-off title quirk.
- If a change touches queue ownership, runtime classification, startup bypass, post-FSR recovery, or metrics publication, assume both Talos-style and GTA-style regressions are possible until proven otherwise.
- Preserve generic signals such as runtime mode, queue provenance, swapchain ownership, and stack evidence. Do not replace those with per-title conditions.
- If a change would make the correct path depend on the executable name, stop and justify that carefully before proceeding.

## Regression Families Already Present
- `off -> DLSS -> off -> FSR`
- `FSR -> DLSS -> off`
- `DLSS suspend -> DLSS resume`
- `startup bypass -> normal routing`
- `FSR FG <-> DLSS FG` visible overlay label switching
- `DLSS FG -> off` visible overlay reset
- `native/runtime-owned FSR FG` must not emit injected `FG overlay SUBMIT` traffic on the FFX-owned queue
- `native/runtime-owned FSR FG` should render via the chained FFX `presentCallback` path instead of separate injected overlay queue work

## Regression Families Worth Expanding
- `off -> FSR -> off`
- `off -> DLSS -> off` with more routing detail than the visible-status tests currently cover
- `DLSS -> FSR`
- `DLSS -> FSR -> off`
- `FSR -> DLSS -> FSR`
- `FSR off while third-party startup overlay windows are still active`
- `heuristic FSR takeover while SL hook remains present`
- `FSR -> DLSS` with native FFX context destruction while CE inline hooks are active
- `startup-blocking overlay wraps DLSS FG swapchain creation`
- `startup overlay appears settled, then runtime-owned swapchain handoff arrives before first real FG activation`
- `startup overlay re-arms for a late pre-FG runtime-owned handoff, then must resume visible overlay before DLSS FG actually turns on`
- `late pre-FG runtime-owned handoff re-arms exactly once per handoff instead of on every subsequent frame`
- `DLSS stall detection still sees forward progress when the runtime uses Present1 instead of Present`
- `automatic immediate dump request targeting the current present thread is deferred to watchdog thread`
- `authoritative Streamline startup handoff suppresses fresh GetState-only DLSS activation until explicit SetOptions enable arrives`
- `PostSL callback stays dormant during DLSS FG OFF->ON->OFF->ON churn in startup transition window`
- `g_StreamlineFGRunning OFF signal deferred during startup transition window to prevent multi-device DLSS FG threading race`

## Open Questions / Stale-Risk
- Stale risk is high because FG switching behavior is spread across runtime classification, queue routing, startup coexistence, and visible metrics publication.
- `installed/captureengine/logs/20260411_233821` is only a short validation pass; longer GTA V Enhanced soak coverage is still worth re-checking after future queue-routing or callback-bridge changes.
- Any future inline-hook / trampoline rewrite can silently re-break native FFX teardown paths even if the higher-level FG state machine looks correct. Re-check `hook/wrappers/inline_hook.cpp` whenever a crash lands inside the `00007FF8011D....` trampoline pool.
- Re-check this page after changes to `dx12_overlay_policy.h`, FG transition tests, or overlay metrics publication behavior.
