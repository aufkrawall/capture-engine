# Archived activity (2026-W36)

### 2026-09-04 - Two unbounded per-frame log lines on the game's render thread under FSR FG

Found while reading the sessions above. With `log_level` defaulting to `trace`, CE wrote
about 135 lines per second from Talos's render thread (`T:5830`) for as long as FSR FG
ran - each a global mutex plus two unbuffered `WriteFile` calls, 1.7-2.1 MB per 45 s:

- `FFX Hook: Armed VEH breakpoint ... (forward-call rearm)` - 1775 lines in 26 s. Only
  the `post-call rearm` reason was metered; the forward-call reason logged every time.
  Both are steady-state heartbeats and are now metered together
  (`ce::log_meter::ShouldLogCadence(n, 20, 300)`); genuine transitions still log always.
- `Streamline Hook: Viewport N state ...` - 3099 lines in 26 s. `stateChanged` compared
  against whether the map held an entry, but the disable path *erases* it, so an
  absent-before/absent-after steady state was indistinguishable from a transition on
  every query. It now compares against the last state actually written to the log
  (`streamline_hook_g_ViewportLoggedStates`).

Open, with evidence, not fixed: the protected-official-FFX `ffxConfigure` entry
breakpoint was hit **zero** times in both sessions (`s_vehHitLogCount` never logged) -
the game reaches `Hooked_ffxConfigure` through the IAT route. Yet
`CallFfxConfigureOriginalGuarded` still pauses and re-arms it around every forward:
one `VirtualQuery`, four `VirtualProtect` on AMD's executable code page and two
`FlushInstructionCache` per application frame, on the render thread, to maintain a
breakpoint nothing hits. The project's own measurement puts `ffxConfigure` at 2 us
without CE and 40 us with it (two calls per frame). The clean fix is a trampoline so the
0xCC never has to come out - which also closes the window where a concurrent thread runs
the runtime's `ffxConfigure` unhooked - but it rewrites a path under the "FG must never
break" constraint and needs a hardware run to land safely.

### 2026-09-04 - FSR FG had no application-source Present at all

User observation: real Reflex/PCL PC latency tracks the screen far better than the
no-Reflex estimate, and Talos reports ~45 ms under FSR FG against ~70 ms under DLSS FG.
Session `20260904_034526` settles which of the two is wrong. In the DLSS-FG window both
sources are live and the cross-check agrees to within a few ms
(`estimate=68.8 vs published 67.7`, ten consecutive samples), so the estimator is
calibrated. The FSR-FG window runs at nearly the same cadence (application 21.0 ms,
output 11.2 ms against 23.2 ms / 11.0 ms) and reports 45 ms, and the entire difference
sits in one term: `presentToDisplay` is 2-4 ms under FSR and 17-25 ms under DLSS.

Root cause: **`applicationPresents_` was empty for the whole FSR-FG window.** The only
caller of `ObserveApplicationPresent` was `DX12_ObserveApplicationSourcePresentTiming`
inside `ProcessFrame`/`DX12_ProcessFrameMinimal`, guarded by `applicationSourcePresent`,
which `ShouldApplyDX12PrerenderLimitOnPresent` grants only on the tracked game Present
thread. Under FSR FG the game presents into AMD's frame-generation swapchain proxy and
AMD's *presenter thread* issues the real DXGI present, so CE's `DetourPresent` never runs
on the game thread and the app-callback route (`DX12_RenderOverlayViaFFXPresentCallback`)
does not reach `ProcessFrame` at all. Consequences, all silent:

- `MatchApplicationPresentLocked` always failed, so the generator hold fell back to the
  modelled `(fgMultiplier - 1) x displayInterval` floor instead of the measured span.
- `ResolveWorkIntervalLocked` fell through to `ResolveFgBaseIntervalLocked`, so the whole
  window's `baseInterval` is `round(1e6 / baseFps)` from the FG runtime's published base
  FPS rather than measured cadence - confirmed exactly on every logged line.
- `presentToDisplay` measures only AMD's presenter-thread present to scanout (~4 ms). The
  render-ahead the association is supposed to expose lives *above* the DXGI present here,
  inside FidelityFX's own queue, so it was invisible from both terms.

Fix: `DX12_ObserveFFXProxyApplicationSourcePresent` in the FFX proxy-present detour
(`hook/apis/dx12_hook_ffx_proxy_present.cpp`), outermost entry only, before any routing
decision - the measurement must not depend on which overlay-composition route is live.
The proxy Present *is* the application's Present: game thread, once per rendered frame.
A same-frame duplicate from a synchronous passthrough is rejected by the existing 3 ms
minimum application interval.

Diagnostics: `generatorHold=` in `[Overlay] PC latency chain` was a bool and printed `1`
for both the measured and the modelled hold, which is precisely why the shortfall looked
healthy. It now prints `measured` / `modelled` / `none`.

Modelled reconstruction of the logged FSR topology (21 ms application, 11 ms output, 4 ms
present-to-display): 46.5 ms modelled versus 63.0 ms measured, against 66-73 ms of real
markers in the DLSS window at the same cadence.

**Confirmed on hardware, session `20260904_042922`.** FSR FG now reads 62-65 ms at
baseFps ~45 / outputFps ~90 - the same cadence that read 45 ms before - so the FSR-FG-on
against all-FG-off delta is ~+25 ms instead of ~+7 ms. The conclusive evidence is not the
value but `frameBeginInterval`: `0us` on every FSR line before, `22268-23328us` after. The
application-source stream carries a frame-begin anchor with it, and **Talos calls its
low-latency sleep even under FSR FG**, so the FSR path is now fully measured
(`frameBegin=low-latency-sleep`, `anchorToPresent=41826-54687us` against a modelled floor
of `baseInterval + displayInterval = 35044us`), not merely hold-corrected.

That immediately caught a defect in the new diagnostic itself: `generatorHold` printed
`modelled` on those lines. `holdMeasured` was only set in the no-anchor branch, but the
step back onto the held application frame is what makes the hold measured in *both*
branches - the anchor form spans from that frame's own boundary, the no-anchor form from
its Present. Only the `expectedGeneratorHoldUs` addition is a model. Corrected: the flag is
now seeded from `holdApplied` and cleared only when the hold could not be applied.

Open: the same structural gap applies to any generator that paces from its own thread
without CE seeing the application Present (Intel XeSS-FG, AFMF, third-party proxies). Only
the FidelityFX proxy is wired.
