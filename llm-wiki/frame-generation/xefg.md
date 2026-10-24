# Intel XeSS frame generation (XeFG) and OptiScaler-wrapped frame generation

Last verified: 2026-10-24 (code, prologue units and flow harness, including foreign FFX input detours).
Hardware validation after these fixes remains pending for native XeFG and OptiScaler `FGOutput=XeFG` / `FGOutput=FSRFG`.
The supplied FSR session with `EnableFfxInputs=false` establishes the workaround's behavior, not a hardware retest
of the default-enabled fix (see "Validation pending").

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
5. **The DLSS-G signal is a front** (`IsStreamlineFGSignalFront`) while a runtime-owned presenter (XeFG) presents,
   or while the DLSS-G API is served outside Streamline (the module behind the hooked SetOptions/GetState target;
   an overlay-folder proxy is never Streamline) **and Streamline presents nothing** (no Streamline-originated present
   among the last 240, `NotePresentOriginator`). A front keeps CE's DLSS-G state off, retires one already on, ignores
   NGX FG observations and never supersedes another runtime's protected startup. The provider alone is no proof:
   OptiScaler wraps a DLSS-G that runs too (see "OptiScaler FGOutput=FSRFG" below).
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

## OptiScaler FGOutput=FSRFG and two FidelityFX images (2026-10-23, session 20261023_221541, fixed in code)

Talos Reawakened, OptiScaler as `winmm.dll` (ReShade as `dxgi.dll`), CE 0.1.7303, two runs:
- **Run 1, OptiScaler over a running DLSS-G (3x):** `sl.dlss_g.dll` created the swapchain and originated every present
  (`[PRESENT ORIGIN] summary ... streamline=1384`), NGX created FG feature 0xB, but the DLSS-G API pointer was
  OptiScaler's (`served by foreign ... WINMM.dll`). The provider-only front rule kept DLSS FG off; CE only re-armed its
  UI bootstrap (664 epochs) and drew no overlay after generation started. Fixed by the presenting evidence (rule 5).
- **Run 2, `FGInput=DLSSG`, `FGOutput=FSRFG`:** two images named `amd_fidelityfx_framegeneration_dx12.dll`: the engine
  plugin's (4.0.0.604, `Engine\Plugins\Marketplace\amd\FSR\...\signedbin`) and OptiScaler's own (4.0.1.2740,
  `OptiScaler\`). OptiScaler resolves its copy through `KernelBaseProxy::GetProcAddress_` (real kernelbase) and,
  with `fg_dx12` loaded, calls that image's `ffxCreateContext`/`ffxConfigure` exports directly (OptiScaler
  `proxies/FfxApi_Proxy.h`). `FFXHook::Init` found runtimes with `GetModuleHandleW(name)` = the first image, so CE
  never saw OptiScaler's create or configure (no `context create result`, no `reached CE through its entry
  breakpoint`), stayed in protected official FFX startup 4 s and drew nothing. The game's `slDLSSGSetOptions(on)`
  (answered by OptiScaler) then hit `Successful explicit Streamline enable superseded provisional official FFX
  startup`, the staged FFX queue was cleared, the next present on FFX's chain took `No swapchain queue captured ...
  falls back to cmdQ` (own queue unreadable) and the first overlay frame removed the device (`0x887A002B`); Unreal
  exited with code 3, no dump.

Fixes: the `ffxCreateContext` entry breakpoint keeps one slot per live FFX image, armed from each image's load
notification (`FFXHook::NoteRuntimeImageLoaded`), so every image's create traps with its own export as the original
and the proxy-present hook anchors to the trapped image; the FFX create stack names the creating image and the next
hook-thread pass moves CE's FFX hooks to it (`FFXHook::ServicePendingRuntimeRetarget`, `Init` prefers that image
for its name); a configure that reached CE only through the breakpoint keeps it armed after the present-callback
bridge (else the next configure restores the game's callback over CE's bridge and FG off goes unseen); a front never
supersedes the FFX startup; an FFX-created chain is registered in `ce::fg_presenter` (`kFFXSwapchain`, no output
route) and the uncaptured-queue fallback never guesses its queue, and never draws when the own queue is readable and
different (`DecideUncapturedQueueFallback`). Flow: `FlowFSRToolRuntime` (both FFX routes, a second same-named fake
image resolved from its export table) and `FlowDLSSGApiTool` (a tool module serving the DLSS-G API: wrapping a
running DLSS-G stays DLSS FG - fails under the old rule -, answering it leaves the tool's FSR FG and its startup).
Open: the dynamic GetProcAddress route still keeps one global original per FFX export (last lookup wins), so a game
calling its own image's export through CE's detour after the hooks moved to the tool's image would forward into
the tool's image; not observed (the game's image was unused).

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

## OptiScaler EnableFfxInputs=true: native output calls bypass the entry (2026-10-24)

Source: OptiScaler `1db5042`, `proxies/FfxApi_Proxy.h:612`: when its input image and output image are the same,
`fg_dx12 = fg_dx12_hooked` copies the Detours originals into its output dispatch. Its native create/configure/destroy
calls resume *after* the entry jump. CE's export breakpoint and cached-export routing cannot observe these calls;
changing or routing the tool's saved originals would also break the owner's DetourDetach contract.
The prior session's OptiScaler log was overwritten; the enabled-input attribution there is inferred from the
reported toggle behavior and the matching native-call bypass, not a preserved configuration record.

Decisive session evidence (build 0.1.7304):
- `20261023_235619`: FFX creates its protected inner chain, but CE sees no enabled configure or native context
  create; 3,892 protected-startup ECL pass-through logs and 550 unsuccessful DLSS UI bootstrap activations. No crash.
- `20261023_235838`: the surviving OptiScaler log explicitly reports `EnableFfxInputs=false`, `FGInput=DLSSG`,
  `FGOutput=FSRFG`. CE sees enabled configure and the proxy, but repeats Streamline enable preparation and retires
  the FSR-suspend renderer 1,228 times. The DLSS-G input is a front, yet the pre-call teardown/UI setup ignored it.

Current mechanism (`hook/ffx/ffx_foreign_observer.*`):
- All loaded official DX12 FFX images are considered, including a same-named tool copy before its first create.
  Only exports with a disk-proven foreign entry jump get a body observer. The existing deep-hook
  engine verifies the native resume boundary, undoes the permitted prologue and publishes the exact native original
  before installing the body patch. The foreign entry and the owner's original-pointer slots remain intact.
- Typed per-export observers use scoped thread-local originals; outer export/IAT hooks delegate so a native call is
  processed once. Module checks and proxy binding use the AMD export as anchor, never CE's private trampoline.
- The two-argument configure signature permits `mov r9,rcx` as scratch; four-argument hooks still refuse it. The
  destroy cookie load into RAX is replayable. Live argument and nonvolatile-register overwrites remain refused.
- Body observers replace the entry breakpoints for those exports. Native modules without foreign entry detours keep
  the established interception. Unload notification invalidates atomics only; the next service pass forgets the
  departed body registration without restoring into a reloaded image. No permanent module pins.
- `DX12_PrepareForStreamlineEnableTransition` refuses a front, also when requested by Reflex. Front SetOptions calls
  do not arm Streamline UI rendering or claim a native Streamline enable call in flight.

Regression: `FFXForeignPrologTest`, `FlowFSRToolRuntime.ForeignFFXInputs*` (five-byte foreign entry hooks, saved
originals, callback/no-callback, resize, detach, off/on), `FlowDLSSGApiTool` (repeated DLSS-G inputs under live FSR).
The saved-original cases also unload and reload both
FFX images, exercising address reuse and generation retirement.
The resize regression exposed retained final-output command-list/backbuffer references in the no-callback path.
`PrepareD3D12Resize` now retires only the resizing chain's `dx12_ffx_suspend_overlay` state: completed work releases
immediately, unfinished work remains fenced. Both plain-export and saved-original routes cover resize.
Validation: the unfiltered closing gate for 0.1.7306 passed native tests, Python tool self-tests and all 175 FG flow
scenarios (384.4 s for the flow matrix), including both saved-original cases. Negative controls without the native
observer/front guard fail the new scenarios. The build produced the version-checked installer and passed shipped
binary privacy checks. Hardware retest of Talos with `EnableFfxInputs=true` remains pending; the user's game configuration was not changed.

## OptiScaler EnableFfxInputs=true: the overlay was interpolated with the scene (2026-10-24, session 20261024_010934)

User report (build 0.1.7306, Talos, OptiScaler DLSS-G -> FSR FG, `EnableFfxInputs=true`): overlay visible, but the
frame time graph animates choppily and the game "looks far less smooth" with CE than without (VRR, 144 Hz panel).

Decisive evidence:
- OptiScaler registers a UI resource without a resource every frame (`native-FSR no-callback UI-resource register
  had empty uiResource`, all 10 capped lines on consecutive frames from FG on). `g_CachedFFXUiTexture` stays null,
  so every prework took `no-ui-resource-backbuffer` for the whole run: CE drew into the proxy backbuffer AMD then
  interpolates. Overlay draws per 10 s: `full=25 graphOnly=459` (~48/s) at base ~45 fps and ~90 fps output.
- The final-batch route was ready all along: marker-only probes on every output (`submit=300` ... `1500`, ~136/s at
  142 fps output, `UI baseline remains sole owner`). Only the UI-resource branch of the prework ever granted it.
- CE's CPU work did not disturb AMD's output cadence: same-build in-game session `20261024_010343` reports
  `[FSRPacingHealth] signature=healthy`, display sd 674-1066 us, 1% low 73 at 91 fps; presenter-thread CE time
  73-76 us mean (p95 115, `[PRESENT STAGE COST] role=runtime_presenter`), game-thread prework 192 us mean
  (`[OVERLAY COST]`, 4 of 600 calls over 1 ms). Present intent forwarded unchanged (`sync=0 flags=0x200`).
- Not attributed (open): in the first FG window (01:09:49.8-55.5, ~71 base / 142 output, likely menu) 15 output gaps of
  17-32 ms in 5.7 s while `[DisplayTiming]` reported composed presentation (`Present submissions ... expire without a
  flip completion`, 01:09:47.8 after a focus loss, until `Flip completions resumed` at 01:09:57.96). The second FG
  window (in game, GPU 99 %) had 3 such gaps in 9 s. No CE event correlates; a run without CE would decide it.

Fix (`ChooseFFXProxyPreworkRoute`, `dx12_overlay_policy/ffx_output_frames.h`): without a UI resource the backbuffer is
the make-before-break baseline only; once `DX12_IsNoCallbackFSRTopmostBatchReadyForOwnership()` holds, the prework
grants the final-batch route, records the frame `kTopmost` and leaves the backbuffer alone (`no-ui-resource-topmost`).
The backbuffer belongs to its frame, so no retirement is needed. Known transient: the first topmost-owned frame's
generated output is interpolated from the previous backbuffer, which still carried the overlay (one faint ghost
output per handover). Post-processing (`sharpen`) moves with the route to every output, as after the UI-resource
handover. Edge logs are capped (`edge #N`, `transitions=N`) because a final batch whose signature alternates flips
the proof per frame (seen briefly at 01:09:53). Regression: `FFXOutputFramesPolicyTest.*Route*`,
`FlowFSR.TogglesWithoutAPresentCallbackOrUiResourceKeepTheOverlay` and `...AnEmptyUiResource...` (`granted=1` with FG
on, every output covered once). Not run as a negative control: before the change the no-UI prework never called the
grant, so `granted=1` cannot hold there (code reading). The first gate also exposed a harness defect: the fake FFX
image numbered presenter lifetimes with its own static, so after `ReloadForeignFFXRuntime` a new proxy at the reused
address reported `lifetime=1` again and its restarted frame numbers counted as 180 attribution mismatches
(`FlowFSRToolRuntime.ForeignFFXInputsPreserveTheNoCallbackRoute`). Lifetimes now come from the game host
(`CEFlowGame_NewRuntimePresenterLifetime`), unique in the process.

Retest `20261024_015207` (0.1.7308): handover worked (`no-ui-resource-topmost`, ~1420 frames covered to FG off), but 7
outputs in the first 7 s after each FG enable had no overlay (`[OVERLAY COVERAGE] AMD FG output of frame N has NO
overlay owner (owner=topmost topmostDrawn=0)`). Two causes: (1) AMD presented the first ~8 outputs after enable from the
game thread (`T6A8C`, `callSite=...97D0` passthrough), the route was proven there and granted, and the first output of
AMD's presenter thread (`T4A80`/`T5F98`) failed the learned-thread check; (2) the "final batch" was a Present-chain
overlay's submission (`...C9B80DF6` presumably Steam's start-up toast, `...3CA5` in OptiScaler's `dxgi.dll`), learned as
the target and then gone when the message ended. Fixed (user choice, 2026-10-24: never flicker over staying topmost):
the anchor is the presenting runtime image's last batch (`ChooseTopmostAnchorBatch`, image range published with the
proxy-present hook), a presenter-thread change relearns the route (`IsNewTopmostPresentation`), and ownership waits until
the runtime presents off the game thread (`HasCompletedNoCallbackTopmostActivation`). Present-chain overlays now draw
above CE while they draw (`[OVERLAY LAYER] No-callback FSR: N submission(s) now follow the runtime's last batch`) and
are not in no-callback FSR recordings. The new fake also showed that outputs were attributed by the last batch's
composing frame: a Present-chain overlay submits after AMD's pacing wait, when the game's next Present may already have
advanced it, so every such output was judged (and, before the anchor, drawn) as the next frame's (`offset 1`, 120
mismatches; the handover output of a baseline frame counted as an undrawn topmost one). Attribution now uses the
runtime's own last batch too (`AttributionBatchIndex`). Negative control: the scenario without the route changes
reports 8 uncovered presents. Regression: `FFXTopmostBatchPolicyTest.Anchor*`, `...OwnershipWaitsFor...`, and
`FlowFSR.PresenterMigrationAndAPresentChainOverlayThatStopsLeaveNoOutputUncovered` (fake FFX runtime: 8 game-thread
presents after enable, a Present-chain overlay submitted from the game's module for 120 outputs, then stopped).

GTA: native FSR FG registers a 1x1 placeholder (CE substitute, UI-resource route); its handover to the final-batch route
is unchanged, but that route now also anchors on AMD's own last batch, so Steam's or Rockstar's Present-time overlays
draw above CE there while they draw (no missed outputs when they stop). GTA with OptiScaler wrapping DLSS-G takes the
no-UI route if OptiScaler registers no UI resource there too; if it registers a real one, the UI-resource handover
applies. Same AMD runtime and same-batch append that GTA validated on 2026-10-03 (`20261003_120641`).

## Validation pending (hardware)

Native XeFG (a game that ships it), Talos + OptiScaler `FGOutput=XeFG` (`FGXeFGDebugView` marks generated frames),
OptiScaler `FGOutput=FSRFG` after the 2026-10-24 fixes (look for `FFX Hook: the frame-generation swapchain was
created by ...OptiScaler...`, `native ffxConfigure observer installed below foreign entry` with default inputs (the entry breakpoint with inputs disabled),
`Explicit Streamline enable is a front`, no
`0x887A002B`; with `EnableFfxInputs=true` also `route transition no-ui-resource-backbuffer -> no-ui-resource-topmost`,
`ownership GRANTED (no UI resource: ...)`, `CE appended a topmost overlay`), OptiScaler wrapping a real DLSS-G (DLSS FG published, PostSL overlay), an Intel GPU (driver pacing may hide generated presents: contingent UI-texture path,
not built), exclusive fullscreen (XeFG refuses it), Steam overlay on XeFG's presenter thread. Look for
`[FG PRESENTER] ... presented by xefg-swapchain`, `[RUNTIME OUTPUT]` on two threads, status `XeFG 2x`, no
`Streamline Hook: FG state transition ON` under OptiScaler, `uncovered=0` summaries.

## Open questions

- Who presents the real frame under OptiScaler (game thread or XeFG's thread); the route does not depend on it.
- Whether XeFG tolerates CE's create-time swapchain changes inside its own native create (descriptor overrides are
  skipped there today).
- XeFG + Smooth Motion stacking; OptiScaler's other inputs (FSR3 input -> XeFG); Vulkan (XeFG is D3D12-only).
