# llm-wiki Log

### 2026-10-19 - backbuffer_count was withheld in every x64 DXGI game with an overlay (fixed, VALIDATED 20261019_021805)

- Strange Brigade DX12 + Steam (`20261019_014848`): `SetMaximumFrameLatency(1) failed hr=0x887A0001` and "requested by
  the application" skips were a waitable-less chain (Flags=0x802) - diagnostics fixed in b35b8d51; cpu_prerender_limit
  was enforced by the D3D12 fence limiter on all 13,896 frames (prerender_wait p50 6.9 ms).
- Root cause of the missing queue-depth control: `DeepHook: Prolog ... is not a recognized stack shape` for dxgi
  ResizeBuffers/ResizeBuffers1 (`mov rax,rsp` + home-space spills, dxgi 10.0.26100.9711). A survey of 23 session logs:
  every x64 session with an overlay `UNAVAILABLE site=none`, no-overlay x64 `ready` via entry, x86 with overlay
  `ready` (own analyzer). Plus the Witcher 3 ReShade-proxy base-name bug. Details:
  `dx12-overlay-third-party-coexistence.md` (resize bullet). D3D8/9/DDraw rewrite present parameters and Vulkan sets
  the image count in CE's layer - none depend on resize reconciliation; no session had the override set to prove them.

### 2026-10-19 - Horizon playout validated on hardware (DLSS 3x and non-FG Vulkan/DX12)

- `20261019_014259` (0.1.7255, Talos DLSS 3x, 59.7 s): avoidable repeat+skip pairs ~0.8/s -> ~0.35-0.45/s
  against the per-second floor (157/~103 recorded vs 130/84 floor), 165 changed choices (164 avoided skips,
  shown 4.1-4.5 ms early), mean content-age bias +0.15 ms vs the previous run, 0 deferrals.
- `20261019_014848` (Strange Brigade non-FG, Vulkan r0001 105.9 s and DX12 r0002 127.4 s, 76-160 fps, avg ~102,
  interval sd 4-5 ms): non-FG timestamps are the perf CSV Present times, so the replay is exact - replayed horizon
  2601/643 and 2889/727 repeats/skips vs recorded 2608/644 and 2901/730. A full-sequence optimum (fewest
  repeats+skips any selector can reach with shown offset in [-1, +0.5] slot) is 3242 and 3611 events; the
  horizon is 2 and 5 events above it, nearest was 18 and 37. All remaining repeats/skips are the game's
  frame-time variation; audio sample-exact (`audio_vs_target_us=+0`, `starve=0`).

### 2026-10-19 - Inject CFR selection near 1:1: replay simulation of pair-avoiding policies

- Retest `20261019_010437` (0.1.7254, Talos DLSS 3x, uncapped 114-125 fps, VRR, displayed interval sd 0.35-0.49 ms):
  judder fix holds (0 live fence deferrals, lead +12..+21 ms). Per-second floor from `[Inject Perf] Input` vs
  ticks = 57 repeats + 8 skips; recording 80 + ~30, i.e. ~22 excess repeat+skip pairs in 28.6 s.
- Replay (perf CSV runtime Presents -> hook clock -> publication gate -> selector, 8 grid phases) overpredicts
  the current rule's pairs ~2x even with fully smoothed timestamps, so only relative effects are trusted.
  Results (excess pairs / p95 timing error / max), session models: nearest 45 / 4.05 / 11.1 ms; blanket
  continuity (emit oldest fresh within +0.75 slot) 7 / 5.2 / 8.1, but on synthetic 144-240 fps it adds 2-5 ms
  lag and doubles p95 -> rejected. One-tick lookahead: Pareto-safe everywhere but only -10% here. Receding
  horizon (4 slots, cost = repeats + skips + 1.0 x |residual|/slot, residual bounded to one slot): 15 / 4.34 /
  8.1 ms here, synthetic 118/121 fps excess 27-33 -> ~0, 60/144/240 fps unchanged. Implemented for inject
  (`cfr-capture-sync.md`, "Pair-avoiding horizon playout"); WGC/DXGI left unchanged (repeats = delivery lower
  bound). Capture sync remains the stronger lever for a source hovering at the recording rate.

### 2026-10-19 - DLSS MFG recording judder: virtual clock ran behind its callbacks (fixed, hardware pending)

- Session `20261018_222704` (0.1.7252, Talos DX12, DLSS-G 4x/3x, av1_nvenc 4K120, no overload): r0008/r0009
  windows of 63-90/120 unique ticks with `fenceDeferred` 30-49 and `srcLimited` up to 49 plus 25-48
  `targetSuperseded`/s at 113-117 fps source = repeat+skip pairs. Smooth exactly where the PostSL plan line showed
  `virtualLeadUs` +7..+12 ms (r0008 00:32:22-36, 120/120), judder where it was -7..-27 ms. The 31 ms A/V target
  sat on the second-newest frame (`fenceTail=1`, `fence not ready value=N completed=N-2`).
- Fixes: final-output clock never earlier than its callback (was: re-anchor only beyond 8 intervals of lag);
  inject fence tail extended over the provably in-flight newest copies (the measured fence "wait" is a 0 ms poll,
  so the reserve was always 1). Details, rejected alternatives and tests: `cfr-capture-sync.md` (2026-10-19).
  User reported 3x and 4x; no 2x or FSR FG recording in this session.

### 2026-10-18 - GTA FSR FG overlay A/B: CE's overlay does not cause the remaining wobble

- Session `20261018_220700` (0.1.7252, 120 cap, FSR FG 2x): overlay visible 22:10:26-22:11:08, hidden (Ctrl+8)
  22:11:08-22:11:45. Matched steady windows (visible 22:10:51-22:11:08 vs hidden): AMD present intervals >9 ms
  10.6% vs 10.5%, p95 9360 vs 9380 us; display p95 9215 vs 9206 us; p2d p50 730/754, p95 1206/1139 us.
  `[FSRPacingHealth]` disp sd 615-835 us in both phases; the first ~20 s after FG enable were worse (disp late
  95-105 permille vs 24-37). Conclusion: the remaining wobble is GTA + FSR FG, not CE's overlay.
- The `[OVERLAY COST] FFX proxy Present` line had counted the limiter's pre-present wait as CE cost and the
  post-present release as AMD cost (`ceAvgUs=1934 runtimeAvgUs=3728`); both halves now exclude those waits.

### 2026-10-18 - GTA quit-dialog ghost overlay: stale CE substitute UI texture (fixed, hardware pending)

- Session `20261018_210920`: CE substitute `ce-substitute-tex=...05961440` was AMD's UI target 21:11:01.03-01.30 (CE
  composited and re-registered it), then GTA registered `game-tex=...4AF4F610` for the rest of FG-on. Alt+F4 quit
  dialog: `configure transition DISABLED` at 21:11:56.213, target back to the substitute (`changed=1 clear=1`),
  route `active-ui-resource -> suspend-backbuffer`. AMD's no-interpolation `Present` runs `presentWithUiComposition`
  (FidelityFX SDK FrameInterpolationSwapchainDX12.cpp), blending the registered UI resource over the copied proxy
  backbuffer - the opposite of CE's comment that the suspend state presents "without consuming the registered UI
  resource". The substitute's 55-second-old overlay frame was blended over CE's live backbuffer overlay.
- Fix: `ShouldRetireCESubstituteBeforeProxyBackbufferRoute` (`ffx_substitute_retirement.h`); the proxy prework
  clear-only-composites a registered CE substitute once per proxy-backbuffer route entry, before the backbuffer
  draw (`[OVERLAY LAYER] CE substitute UI texture cleared before the suspend-backbuffer route`). Clear-only renders
  use the renderer's reserved slot (`RenderRequest::reservedSlot`, formerly `screenshotSlot`): on the
  backbuffer-indexed slot the same prework's backbuffer draw was refused as in-flight reuse, which
  `FlowInjectFGSwitch.NoCallbackFSRIncludedOverlayKeepsRecording` caught (1 uncovered present). The fake FFX runtime
  does not blend UI resources, so the ghost itself is covered by a policy test plus a source-order check.

### 2026-10-18 - GTA FSR FG after the release fixes: CE's handoff is exact; jitter is downstream (0.1.7250)

- Session `20261018_210920`: generated-output release armed (`byOutputs=214-368/600`, rest at the ~12 ms-work
  bound); display stddev 0.51-1.02 ms, 1% low 63-89 (was 0.53-1.67 ms, 58-79). CE handoff intervals (proxy
  forward) p5/p50/p95 16665/16667/16668 us; AMD present intervals p50 8141, p95 9453, p99 11642 us; p2d p50 800,
  p95 2082, p99 3725 us; GPU ~82-87% (not saturated; the user reports GTA sustains ~120 fps without FG). Steady
  groups are already even: generated output presented ~4.4 ms after the handoff, real one 8.15 ms later, p2d
  0.5-0.95 ms. The unevenness is 9.3% outlier AMD present intervals (>9 ms); 63% of them coincide with GTA's frame
  ending >14.5 ms after the handoff (mean 14.55 vs 13.83 ms). Direction unproven: GTA's longer frame may delay
  AMD's real-output present (+12.6 ms, inside GTA's ~9.3 ms post-release work), or GTA may wait on AMD (frame end
  ~2.8 ms after AMD's previous present, sd 1.7 ms). Either way it is downstream of an exactly regular handoff; the
  user reports the same wobble without CE. GTA's work (~9.3 ms) does not fit the ~8.1 ms gap between AMD's two
  presents, so no release placement avoids both. Untested CE share: the no-callback topmost overlay draw on AMD's
  present path (try once with the overlay toggled off).

### 2026-10-18 - GTA V Enhanced FSR FG at a 120 cap: GPU-bound, and the generated-output release never armed (open)

- Session `20261018_204650` (0.1.7249, no-callback FSR route, vsync forced). Game-thread cadence exact
  (`avgFps=60.0 late=0`), but display stddev 0.53-1.67 ms, 1% low 58-79 (Talos: 0.05 ms, 111-117). GPU 90.7%
  (Talos 43%). Measured game frame work 11.3-15.0 ms of 16.67 (`workCeilingUs`), AMD proxy runtime ~0.6 ms.
- FG-only trace window: presents p5/p50/p95 8.04/8.18/9.28 ms (AMD's own pacer wobbles; Talos 8.19-8.49),
  display p5/p95 6.75/9.58 ms, p2d alternates ~0.45-0.50 / ~0.9-1.0 ms with 1.2-4 ms outliers. Outputs after the
  handoff: first p50 ~2.0 ms, second p50 ~10.3 ms - an ~8 ms gap that a 12.5 ms frame cannot fit into.
- The generated-output release never armed: `headroomUs=8288` was carried over from the FG-off phase, where GTA
  reached only ~62 fps under the 120 cap, so every frame was ~8.2 ms "late" (< one 8.33 ms interval) and counted
  as a recurring overrun; budget = ceiling + 8.3 ms >= 16.67 ms -> back edge. Two defects: lateness of a frame whose
  own work exceeds the interval is not a budget overrun (the game cannot reach the cap), and front-load state
  (ceiling, reservations) survives a cadence-configuration change (FG on/off changes the interval and site).
  Both fixed the same day (unit tests; GTA hardware rerun pending). Expected gain is modest: GTA's frame does not
  fit the gap between its outputs and the GPU is ~91% busy; GTA also wobbles with FSR FG without CE.

### 2026-10-18 - Talos FSR FG at a 120 cap: game cadence exact, generated frames reach the screen 1.6 ms late (open)

- Session `20261018_191159` (0.1.7246, vsync=fifo forced, 144 Hz VRR, 4K, GPU ~43%). The new game-thread site
  owned pacing: `site=4 group=120/2`, `Local timer stats ... avgFps=60.0 late=0 wake late=0`, CE wait ~11 ms per
  16.67 ms frame (game CPU ~5.7 ms, back edge: front-loading is off under FG).
- Pacing trace (steady window): presents even, 8.33 ms p10/p90 8.19/8.49; display intervals bimodal 6.52/10.06 ms
  (p10/p90). Present-to-display: real outputs 0.4-0.6 ms (`app_present_to_display` mean 513 us), generated 2.0-2.25 ms
  (`gen_present_to_display` 2128 us). AMD pacer wait: real 7766 us, generated 608 us; callback-to-display: real 8343,
  generated 2812 us. One group: game forwards at t=13.5 ms, gen callback 17.0, gen Present 17.7 (shown 19.9), real
  callback 18.1, real Present 25.9 (shown 26.4). Overlay: display stddev 1.57-1.73 ms, displayJag ~3.0-3.3 ms vs
  presentJag ~250 us; FG off at 120: stddev 0.54 ms.
- Reading: AMD's pacer runs its schedule slightly fast (delta = avg/2 - variance*0.1 - 0.1 ms) and catches the
  game's arrival, so the generated frame is readiness-bound and presented ~0.6 ms after its composition callback;
  that callback's GPU work (AMD UI composite + CE's overlay) is still running at Present and adds ~1.6 ms to its
  screen time, while the real frame's identical work hides inside the 7.8 ms pacer wait. Also suspect: the game's
  next-frame GPU work starts right at the deadline (back edge) and overlaps that window. Not yet separated:
  `CE_FG_GPU_TIMING=1` gives `gen start/dur` of CE's callback commands (late start = upstream contention, long
  duration = composition/overlay cost). Side issue, FIXED: `outputFps=101` and `displayInterval=9.6-9.8 ms` were
  `MedianRing(displayIntervals_)` - the median of an alternating 6.5/10.1 ms cadence; every output-cadence reader now
  uses `CadenceIntervalRing` (mean, intervals > 4x the median excluded; `tests/test_system_latency_cadence.cpp`).
- Follow-up run (pid 23908, `CE_FG_GPU_TIMING=1`, `[OverlayGpuTiming] ACTIVE`): same signature (stddev 1.8 ms,
  displayJag 3.6 ms, presentJag 0.27 ms) but no pacing trace was saved (no automatic trigger, no Ctrl+Shift+F11), so
  the GPU spans were not captured.
- GPU-timing run `20261018_195528` (0.1.7247, 120 cap, vsync=fifo forced, three traces saved): `gen start=291us
  dur=195us` (CE's commands in the generated output's callback, relative to callback end) - done before AMD's
  Present at `gen pacerWait=670us`; yet `gen p2d=1862us` vs `app p2d=489us`. `gpu app start=1403us`: the real
  output's callback commands, submitted ~0.5 ms after the generated Present on AMD's present queue, start ~1.4 ms
  late. One group (us, absolute): gen compose GPU 69472-69666, gen Present 69797, real compose submitted 70129 but
  GPU 71665-71863, gen display 71831. So the generated Present itself holds the present queue ~1.9 ms until its
  flip latches; neither CE's overlay nor composition GPU cost is the cause (195 us), and `outputFps=120.1` confirms
  the cadence fix.
- `20261018_200054` (vsync_mode=default, AMD got sync=0+ALLOW_TEARING): identical (`gen p2d=1862-1910us`,
  `gpu app start=1399-1442us`), so not the sync mode. Per frame, the present queue's release tracked the game's
  next-frame end (corr 0.87/0.89, slope 0.59/0.60, n=832/534): the generated Present waits behind the game's GPU
  work. Fix: gated release after the generated outputs (see `frame-pacing-and-limiter.md`).
- VALIDATED `20261018_202233` (0.1.7248, Talos, 120 cap, FSR FG 2x). No vsync (p4816): `gen p2d=351-394us` vs
  `app 455-483us`, `gpu app start=159us` (was 1.4 ms), display stddev 46-53 us / displayJag 60-72 us (was
  1.6 ms / ~3.0 ms), 1%low 111-117. Forced vsync (p5768): `gen p2d=465-504us`, stddev 40-64 us, displayJag
  54-80 us. Steady windows `byOutputs=600 byBound=0`, release wait ~3.2-3.3 ms after the handoff; the first
  600-frame window of each run took the bound 232-253 times while the budget settled. Latency estimate's
  `anchorToPresent` ~27-36 ms (was 31-41).

### 2026-10-18 - FPS limiter: auto default, fg_fallback retired, FSR FG paced on the game thread (flow-tested only)

- Review findings: the template shipped `basic` for both limiters while code/README meant `auto`; `fg_fallback` ran
  the identical basic path (label only); under FSR FG CE paced AMD's outputs on AMD's presenter thread, stacked
  on AMD's own pacer, and the game was throttled only by AMD's queue backpressure.
- Changes: `ce::limiter_mode_policy` (auto = game-activated Reflex/native else basic), fg_fallback parses as basic
  with a warning, template defaults `auto`; new `PresentSite::kRuntimeInputPresent` in the FFX proxy Present detour
  owns the cadence (whole output groups, strict grid), outputs pass by a present-counted ownership
  (`runtime_input_site_policy.h`); `ApplyPostPresent()` only runs on the thread whose Apply armed it.
- Evidence: unit matrix + `FlowFSRLimiter.*` (both FFX routes) fail without the proxy wiring (scale 1, 0 passes).
  The fake's lock-step presenter holds the cap either way, so latency/pacing quality is unproven until hardware.
  Pending hardware: Talos/GTA FSR FG at a cap below the panel, FG on/off/switch, capture sync on/off.

### 2026-10-18 - `[Video] encoder=auto` is the default (unit/hardware probe on RTX 5070 only)

- Before: default `av1_nvenc`, no capability probing and no encoder fallback anywhere; AMD/Intel or pre-Ada NVIDIA
  failed at `avcodec_find_encoder_by_name`/`avcodec_open2` until configured by hand.
- Now: resolved in the media engine once the encode GPU is known (inject LUID, encoder device, or the screen-grab
  device chosen with a probe predicate); vendor wrapper AV1>HEVC>H.264 then MF, 10-bit-capable first unless
  `bit_depth=8`; real-open probes cached per adapter; `EnsureDevice` retries the next candidate after a failed real
  open. Live streaming maps auto to `h264_auto`. See `encoder-auto-selection.md`.
- Evidence: `tests/test_encoder_auto_probe.cpp` on the RTX 5070: `auto` -> `av1_nvenc` (`av1_nvenc:10=ok`, 217 ms,
  second resolution `hw=0`), `h264_auto` -> `h264_nvenc`. Pending: AMD, Intel, hybrid laptop, a GPU without AV1.

### 2026-10-18 - Strange Brigade Vulkan prerender, round 2: the async latch was the gate (VALIDATED)

- Evidence (`20261018_172054`, 0.1.7241): `Prerender=1`, `depth changed to 1`, `Async present detected
  (submit-thread mismatch submit=7820 present=27076)`, PC latency 52-56 ms. The round-1 re-arm line never fired,
  so the present queue's last submitter was the present thread at every present: the round-1 "late split"
  diagnosis was wrong. The CSV's `prerender_wait_us` was 0 for every row because Vulkan never filled it.
- Fix: the present-queue marker ignores the async latch (`CanAppendMarkerAtPresent`; `vkQueuePresentKHR` holds
  the queue); the split re-arm stays for separate producer queues only. Vulkan perf rows fill `prerender_wait_us`,
  plus a `Vulkan Prerender: summary` per 1024 markers. Prerender tests moved to `tests/test_vulkan_prerender_policy.cpp`.
- Validated `20261018_173529` (0.1.7242): gameplay `avgWaitUs` ~7.5-7.8 ms on every marker, PC latency 26 ms at
  ~108 fps (was 55 ms at ~105 fps). Open: the summary is per 1024 markers, ~2 lines/s in a ~2000 fps menu.

### 2026-10-18 - Strange Brigade Vulkan: cpu_prerender_limit inert after a late render-thread split (no hardware rerun)

- Evidence (`20261018_170350`): run 1 `Prerender=-1`, run 2 `Prerender=1` (`fence ring ready`, `depth changed to
  1`); both ~105 fps GPU-bound, PC latency 54-56 ms. Learning window (3 samples at 17:05:03) saw one thread; at
  17:05:14 `Async present detected (submit-thread mismatch submit=26380 present=24196)`. No `producer-submit pacing
  armed` line -> neither route paced. DX12 build of the game reduces latency with the same setting.
- Fix: present path re-arms prerender boundary learning on a late split (`ObservePrerenderThreadSplit`); prerender
  topology learning moved from `vulkan_layer_state.cpp` (794 lines) to `vulkan_prerender_topology.cpp`. See
  `graphics-overrides-and-frame-pacing.md` (present topology bullets). Pending hardware validation.

### 2026-10-18 - Mirror's Edge/DXVK "always vsync": composed presentation + graph source flapping (no hardware rerun)

- Evidence (`20261018_155920`, `_161139`): swapchain `presentMode=0` (IMMEDIATE, per-present mode 0 too), flags
  0x2c4 (DXVK's own present-timing/id2/wait2), no CE present-mode or DRS vsync override (`VSync=default`). Perf CSV
  `qpc_delta_us` ~900-2000 = the game presents ~500-1100/s; sensors: presents expire without flip completion ->
  composed via dwm, `completion(...)` all 0, so the screen gets at most 144 pictures, no tearing. A DXUP/D3D11 run in
  `20261018_155158` (PID 1176) flipped independently (`hsyncDpcMpo`) at ~600 fps.
- CE bug fixed: one own completion (`completed=1 expired=86`) ended the composed state while dwm still showed the
  game; 10 s rate limit then published almost nothing (n=297, one 9.3 s gap) -> overlay fell back to presentation
  (~460 fps). See `display-change-timing.md` (composed presentation).
- Open: why DXVK is composed with CE. Pseudo-overlay was suppressed/destroyed, D3DKMT hooks pass-through, no DXGI
  swapchain create passed CE's hooks. Added `presentation path ... blockers=` layer line for the next run.
- `20261018_162817`: `blockers=none`, `dispbroker=1 dcomp=0` (native NVIDIA presenter). The user's actual complaint
  was the readout: display-change showed dwm's 144 as the game's fps. Composed samples are now flagged and the
  overlay reports presentation timing while composed (see `display-change-timing.md`). VALIDATED `20261018_165208`
  (0.1.7240): `source=presentation composed=1 fps=917.5 1%low=618.6`; one stray own completion refused
  (`completed=1 expired=70 composedShown=39`) with no source flap; alt-tab swapchains logged
  `blockers=minimized|not-covering-monitor`, then `none` at 3840x2160.

### 2026-10-18 - Overlay CPU cost at 630 FPS (Mirror's Edge on DXUP, 20261018_151515; no hardware rerun)

- User: 630 FPS CPU-bound with CE vs 660 with RTSS (~72 us/frame). CSV: `total_us` 132 = `overlay_us` 97 + ~30 other
  pre-present. Unit measurement of the adapter build with a counting backend: graph on 12.9 us/frame (full rebuild
  every frame), cached 0.3 us. Fixed: graph-only frames + faster polyline (graph frame 3.0 us). The rest of the 97 us
  is D3D11 calls through DXUP; removed per frame: 4 `GetDevice` (D3D11 hook classification, now
  `ClassifySwapChainAPICached`), the hook's duplicate RTV/viewport save (and its RTV 1-7 unbinding), the CB map and
  unchanged VB/IB maps, ~7 `GetModuleHandleA` + a `d3d11.dll` version-resource parse (`IsDXVKD3D10OrD3D11Loaded`,
  the DXUP label; now cached against `LoadedModuleSetGeneration`, the DLL-notification count).
- Not done: `SwapDeviceContextState` instead of ~70 Get/Set calls (DXUP implements it with a full backend reapply,
  and CE's context hooks intercept it; needs a hardware A/B). The other ~30 us of pre-present is unattributed.
- DXUP's QueryInterface debug log is lazy (`logMessage` checks the level first): no per-call string cost.
- Follow-up done the same day: `IsDXVKD3D9WrapperLoaded` meant "any non-System32 d3d9.dll" and named DXUP's DXVK.
  Behavior for translators unchanged (the only DX9Hook install is gated on it, so the DX9-internal "DXVK" branches
  never ran for any replacement); split into `IsNonSystemD3D9RuntimeLoaded` + `ResolveD3D9RuntimeKind` for logs and
  the label. Session 20261018_151515 also shows the DXUP label fix live: `'unset' -> 'DX9 (DXUP)'`.

### 2026-10-18 - Mirror's Edge on DXUP: "DX11.1 (DXUP)" and per-frame RTV churn (session 20261018_143747)

- x86 D3D9 game with DXUP's `d3d9.dll` in `Binaries`; CE's D3D11 overlay drew on DXUP's D3D11 swapchain and labeled
  it with the D3D11 label + ` (DXUP)`, minor 1 from `external D3D11 context QueryInterface caller=d3d9.dll`. Fix:
  the D3D11 overlay uses `DxupOverlayApiLabel` (frontend selection), DXUP callers are `kDeviceRuntime`.
- Same session: `Creating retained RTV ... buffer=0/1` + `RTV created OK` 16438 times each (81% of the hook log), one
  per Present at ~450-750 FPS; `overlay_us` averaged 91 us. The D3D10/D3D11 overlay retained ONE view keyed by
  buffer index, and DXUP's D3D11 FLIP chain rotates `GetCurrentBackBufferIndex` with distinct textures per index.
  Now `BackBufferViewCache` (`hook/d3d11/dx11_back_buffer_view_cache.h`) keeps one view per index; the pre-render
  log gate keys on device/context/swapchain. DX12 already kept per-buffer descriptors (no churn); DX9/DX8/D3D7
  overlays create their state block and font once. Not re-measured on hardware.
- Who rotates the index: Windows' D3D11 keeps `GetCurrentBackBufferIndex` at 0 on FLIP_DISCARD and FLIP_SEQUENTIAL
  (WARP flow test `FlowD3D11FlipBufferIndex`; the rotation is the DXGI runtime's, not the driver's); FLIP_DISCARD
  refuses `GetBuffer(1)` with DXGI_ERROR_INVALID_CALL. DXVK's `D3D11SwapChain::GetImageIndex` returns 0 and
  rotates image storage behind fixed textures (upstream source). Kept logs agree: BioShock (swapEffect 0) built
  one view per session; HOTS on DXUP rebuilt 14112+14111 times (20261017_144320). So only DXUP churned.

### 2026-10-18 - API misclassification audit (source + upstream DXVK; no hardware run)

- DXVK D3D11 swapchains classified D3D10 since 3a41978e; caller-blind D3D11.x evidence (CE's prerender fence query
  made "DX11.4"); D3D12 overlay missed vkd3d-proton and one path hard-coded "DX12". All fixed, see
  `graphics-api-reporting.md`. Negative checks: the DXVK matrix fails with the old preference; the flow test reads
  minor 4 with caller attribution disabled.
- Audited without finding a defect: D3D9/DDraw/D3D8/OpenGL labels (creation evidence), D3D10 vs 10.1 (creation
  identity), DXUP (native D3D12 output owns overlay and label "DX12" by design of the frontend delegation).
- Follow-up the same day: DXUP games are labeled by the application's API ("DX9 (DXUP)" ...), not "DX12";
  `graphics-api-reporting.md` D3D12 bullet. DXUP's DLL set and frontends read from its local source.
- Remaining: Vulkan-layer label for a DXVK D3D11 game that also loads a D3D10 runtime ("DX10 (DXVK)"), no feature
  level on that side; perf CSV `api` column is per-path and was not audited.
