# llm-wiki Log

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
