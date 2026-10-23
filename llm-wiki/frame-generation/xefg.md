# Intel XeSS frame generation (XeFG) and OptiScaler-wrapped frame generation

Last verified: 2026-10-23 (code and flow harness, build 0.1.7301). **No hardware run yet** with this support:
native XeFG, OptiScaler `FGOutput=XeFG` and `FGOutput=FSRFG` are all pending (see "Validation pending").

Primary sources:
- `hook/xefg/xefg_hook.cpp`, `xell_hook.cpp`, `xefg_api.h` (the SDK 3.0.2 ABI subset CE reads)
- `hook/fg/fg_presenter.h/.cpp` (which runtime presents which swapchain, its queue, its cadence)
- `hook/d3d12/dx12_hook_runtime_swapchain.cpp`, `dx12_runtime_swapchain.h` (registration, takeover release, the
  per-output frame), `hook/present/dxgi_shared_present_runtime_output.cpp` (the runtime-output Present route)
- `hook/streamline/streamline_front_signal.cpp` (the DLSS-G "front")
- `hook/present/present_origin.cpp`, `present_originator_policy.h`, `hook/overlay/overlay_compat_detail/module_table.h`
- Tests: `tests/test_fg_presenter_policy.cpp`, `test_fg_runtime_xefg_classification.cpp`,
  `test_present_originator_policy.cpp`, `test_overlay_coverage_inheritance.cpp`; flow
  `tests/flow/test_flow_xefg*.cpp` with the fakes in `tests/flow/fakes/xefg/`
- External: XeSS SDK v3.0.2 (`inc/xess_fg/xefg_swapchain*.h`, `doc/xess_fg_developer_guide_english.md`,
  `inc/xell/xell.h`); OptiScaler `1db5042` (`framegen/xefg/XeFG_Dx12.cpp`, `hooks/FG_Hooks.cpp`,
  `hooks/Streamline_Hooks.cpp`).

## Summary

XeFG is a proxy-swapchain frame generator like FFX's, but it **presents its own native swapchain itself**: the game
presents into XeFG's `IDXGISwapChain4` proxy, XeFG composes and presents the generated frames from a high-priority
thread of its own on an internal high-priority direct queue (non-Intel GPUs; on Intel GPUs the driver paces), and
the real frame follows. CE therefore keys every decision on **who presents a swapchain** (`ce::fg_presenter`), not
on which module's stack a Present came from:

1. **Attribution at create time.** `xefgSwapChainD3D12InitFromSwapChain[Desc]` opens a create scope for its window;
   the native swapchain created inside it (through the game's factory, CE's factory wrapper included) is registered
   as `xefg-swapchain` with its queue. `libxess_fg.dll` on the create stack is the fallback evidence.
2. **One route for all its outputs.** Every Present on a registered chain, from any thread, takes
   `ExecuteRuntimeOwnedOutputPresent`: serialized (mutex), one overlay draw on the chain's present queue, no overlay
   fence wait, the limiter as a runtime output, then the original Present and the deferred-signal flush. The chain
   is returned **unwrapped** to XeFG.
3. **The queue follows `ResizeBuffers1`.** XeFG rebinds its native chain to its internal queue there (the SDK
   ignores the caller's queues); CE records the queue before forwarding (`NoteRuntimeOwnedSwapchainPresentQueue`,
   also on the reconcile-only detour).
4. **Status `XeFG Nx`** (overlay type 4, `RuntimeMode::kXeFG`): presenter registered and `SetEnabled(1)`. The factor
   is the measured output/application cadence (`CadenceTracker`, outputs per Present, application frames per
   `SetPresentId`); `SetNumInterpolatedFrames + 1` only until one is measured. Native NVAPI/Streamline latency reports
   are ignored for it (as for FSR FG).
5. **The application frame is `xefgSwapChainSetPresentId`** (game thread, before the proxy Present by contract, also
   inside OptiScaler's present hook): the limiter's input site, the application frame rate and the cadence input.

## Session evidence (decisive lines; the logs are not kept)

`20261023_183119`, Talos Reawakened + OptiScaler `10.0.0-dev (1db5042)` as `dxgi.dll`, `FGInput=DLSSG`,
`FGOutput=XeFG`, RTX 5070, CE 0.1.7298 (before this support):
- `libxess_fg.dll` and `libxell.dll` load from OptiScaler's `OptiScaler` folder; the overlay token "optiscaler"
  matched them, so CE skipped XeFG's frames as an overlay's in both originator walks.
- One native swapchain (`...C020`) received two terminal presents per game frame, 2.2 ms apart, then an 11.5 ms
  gap (72 fps rendered, 144 displayed). First on T534C (the game's RHI thread, also the caller of
  `slDLSSGSetOptions`): classified Streamline, PostSL ran (`Invoking PostSL on confirmed standalone Streamline
  Present1`). Second on T4D14: "the application's own Present", ProcessFrame skipped the draw
  (`Routing state ... skip=1`, `PostSlRecent`). `PostSL stats: calls=500` per 6.8 s = overlay on 72 of 144 frames.
- Coverage hid it: `[OVERLAY HANDOFF] ... covered=1 inheritIfNoDraw=1` (fixed, `coverage_inheritance.h`).
- CE published `DLSS_FG 2x`: the game's `slDLSSGSetOptions(on)` went to OptiScaler's dummy (its `slGetFeatureFunction`
  returns its own SetOptions/GetState; `slInit` strips `kFeatureDLSS_G`), so no DLSS-G existed.
- OptiScaler resolves XeFG's exports with `GetProcAddress` from its folder; the export entries are `E9` thunks that
  match the image on disk, so CE's inline detours prepend (module-owned, not foreign).

## Invariants

1. **A presenter-owned chain never seeds CE's original game queue.** Its first frame took XeFG's queue as the game's,
   the game queue's ECL hook was then skipped as an "FG runtime queue", and after `xefgSwapChainDestroy` every native
   present drew nothing (`gate=zero-ecl-skip`, 899 uncovered; `FlowXeFG.DestroyAndRecreate...`).
2. **`InitFromSwapChain` needs the game's last reference**: CE drops all of its own first
   (`DX12_ReleaseSwapchainForRuntimeTakeover`); the fake refuses otherwise, as the SDK does. `appSwapchainRef=` logs it.
3. **XeFG's chain is never wrapped.** A CE wrapper put CE's application route on XeFG's outputs: post-process on the
   game queue, FFX error 907 and `0x887A0005` in the flow (`AssignCreatedSwapchain` returns it unwrapped).
4. **Destroy retires the presenter** (`RetireOwner`), resets the cadence and publishes the status; a new create for the
   same window retires the previous registration. Address reuse across recreate is expected (flow: both the
   capture binding and the overlay re-initialize by lifetime, not by pointer).
5. **A DLSS-G API served outside Streamline is a front** (`IsStreamlineFGSignalFront`): the module serving the hooked
   SetOptions/GetState target (CE, Streamline, foreign; an overlay-folder proxy is never Streamline), or any
   runtime-owned presenter, keeps CE's DLSS-G state off and retires one already on; NGX FG observations are ignored.
6. **Inheritance of FG-composed coverage** only on presents the composing runtime itself makes (unresolved, Streamline,
   FFX originators); XeFG and application presents count uncovered when nothing drew.
7. **A generation toggle on a presenter-owned chain gets no draw cooldown** (`IsPresenterOwnedRuntimeModeToggle`):
   presentation does not move.
8. **The scene-transition gap is never measured across a swapchain handoff** (`ShouldMeasureSceneTransitionGap`,
   `dx12_hook_g_TrackedSwapchainGeneration`): FSR FG -> XeFG measured the FSR chain's last normal-route run 4170 ms
   earlier and blanked 30 XeFG outputs (`FlowXeFGSwitch`).
9. **A registration lifetime is a swapchain identity.** XeFG's chain can take the game's released chain's address;
   `fg_presenter::PresenterLifetime` against `dx12_hook_g_TrackedRuntimePresenterLifetime` makes ProcessFrame treat
   it as a new swapchain (`IsNewRuntimePresentedSwapchainLifetime`), rebuilt at once on its creation-proven queue
   (`ShouldReinitOverlayImmediatelyOnRuntimePresentedSwapchain`), and a Streamline OFF observed there (the front's
   retirement) keeps that state (`ShouldKeepOverlayLiveAcrossStreamlineOffOnRuntimePresentedSwapchain`): the outer
   OFF teardown had blanked XeFG's first output (`FlowXeFGSwitch.AStreamlineFGSignalBesideXeFGIsAFront`).

## Limiter, latency, capture

- **Limiter:** `g_SharedFpsLimiter.Apply(false, kRuntimeInputPresent)` in the `SetPresentId` detour for the presenting
  context; XeFG's outputs pass (`runtime_input_site_policy`, 12 outputs without an input re-arm output pacing). No
  `ApplyPostPresent` pairing (the proxy Present is not CE's; OptiScaler detours its slot bodies). A 72 fps output cap
  paces 36/18 rendered frames at 2x/4x, 72 with generation off.
- **XeLL cap:** the game's `xellSetSleepMode(minimumIntervalUs)` sleeps at the frame start, CE at its end; in series
  the lower cap waits and the other finds every frame late. CE leaves the game's parameters alone and logs both
  (`XeLL: frame caps in series ... the lower paces: ...`). Flow: a 30 fps XeLL cap holds over CE's 36, 60 does not.
- **PC latency:** XeLL `SIMULATION_START`/`PRESENT_START` per present ID feed `NoteMarkerFrameBegin` (the PCL
  numbering is identical); no native report.
- **Switching:** `FlowXeFGSwitch` covers XeFG <-> FSR FG (callback and no-callback), XeFG <-> DLSS FG, XeFG -> off
  and a DLSS-G signal beside XeFG (before and after XeFG exists); `FlowXeFG` covers toggles, 2x-4x, resize,
  InitFromSwapChain and destroy/recreate.
- **Capture:** inject capture runs inside the per-output ProcessFrame on XeFG's present queue; every output is
  published (the hook publishes up to 4x the capture rate; the media side builds CFR) with its own timestamp, one
  output interval apart at 2x and 4x (`FlowXeFGCapture`).

## Diagnostics

`[FG PRESENTER]` (registration, queue rebinds, retirement), `[RUNTIME OUTPUT]` (per swapchain/thread), `[PRESENT
ORIGIN]` (originator per stream, 10 s summary), `XeFG: ...` (every state call, init result, `appSwapchainRef=`,
cadence windows, `SetPresentId paces`), `XeLL: ...` (sleep mode, caps, first marker, anchors), `Streamline Hook: FG
signal ... is a front`, `[OVERLAY COVERAGE] ... uncoveredByOriginator=`, `DX12: Scene transition gap rebased`.

## Rejected approaches

- Pacing on the proxy Present: OptiScaler detours the proxy's vtable-slot bodies; CE would sit in its chain.
- Identifying XeFG's presents by stack only (the original design): the game-thread present carries Streamline
  frames on its stack under OptiScaler, and the presenter thread carries none.
- Same-originator-as-last-draw coverage inheritance: failed `FlowDLSS` reactivation (27 uncovered).
- Rewriting the game's XeLL interval to CE's: not needed for "lower cap wins", and it changes the game's own pacing.

## Validation pending (hardware)

Native XeFG (a game that ships it), Talos + OptiScaler `FGOutput=XeFG` (`FGXeFGDebugView` marks generated frames),
OptiScaler `FGOutput=FSRFG`, an Intel GPU (driver pacing may hide generated presents: contingent UI-texture path,
not built), exclusive fullscreen (XeFG refuses it), Steam overlay on XeFG's presenter thread. Look for
`[FG PRESENTER] ... presented by xefg-swapchain`, `[RUNTIME OUTPUT]` on two threads, status `XeFG 2x`, no
`Streamline Hook: FG state transition ON` under OptiScaler, `uncovered=0` summaries.

## Open questions

- Who presents the real frame under OptiScaler (game thread or XeFG's thread); the route does not depend on it.
- Whether XeFG tolerates CE's create-time swapchain changes inside its own native create (descriptor overrides are
  skipped there today).
- XeFG + Smooth Motion stacking; OptiScaler's other inputs (FSR3 input -> XeFG); Vulkan (XeFG is D3D12-only).
