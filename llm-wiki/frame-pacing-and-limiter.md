# Frame Pacing And The FPS Limiter

Last cross-checked: 2026-10-19 (DLSS-G enabled-but-idle pacing; pre-limiter capture present-release stamps; 2026-10-18: limiter modes: auto default, fg_fallback retired; FSR FG game-thread proxy site; earlier: D3D10 prerender routing, D3D10/11 pre-wait submission, SmartWait wake stats)

Producer-queue depth enforcement (`cpu_prerender_limit`, `backbuffer_count` present depth) and everything the FPS
limiter owns: the rational cadence grid, where in a period the wait is spent, how call sites declare what their
Apply() entries mean, frame-generation output-group admission, and the native low-latency (Reflex) handoff.

Primary sources:
- `hook/pacing/{fps_limiter,fps_limiter_policy,limiter_mode_policy,runtime_input_site_policy}.h`
- `hook/pacing/fps_limiter_detail/{apply,frame_pacing,front_load,cadence_diagnostics,lifecycle,runtime_input}.h`
- `hook/d3d12/dx12_hook_ffx_proxy_present.cpp` (`DX12_PaceFFXProxyApplicationPresent`, the FSR FG game-thread site)
- `hook/pacing/reflex_limiter.h`
- `hook/metrics/performance_metrics.cpp` (`ConsumeDisplayTiming` publishes present-to-display to the limiter)
- `hook/present/{dxgi_shared_present_core,dxgi_shared_present1,dxgi_shared_present_routing}.cpp`
- `hook/wrappers/dxgi_swapchain_wrap_present.cpp`
- `hook/vulkan_layer/{vulkan_layer_present,vulkan_layer_swapchain,vulkan_reflex_limiter}.*`
- `hook/vulkan_layer/vulkan_present_boundary.h`
- `hook/ddraw/ddraw_hook_present_overrides.{h,cpp}`
- `hook/ddraw/ddraw_present_policy.h`
- `tests/{test_fps_limiter,test_fps_limiter_part2,test_fps_limiter_output_groups,test_fps_limiter_present_site,test_fps_limiter_front_load,test_present_pacing_policy}.cpp`
- `tests/test_fps_limiter_runtime_output_{site,bursts}.cpp` (runtime-output rate and duplicate-filter contracts)
- `tests/test_fps_limiter_sleep_mode_recorder.h` (manual rearm recorder, included only by its two consuming suites)
- `tests/{test_limiter_mode_policy,test_fps_limiter_runtime_input_site}.cpp`, flow `tests/flow/test_flow_fsr_limiter.cpp`

Related: `graphics-overrides-and-frame-pacing.md` (sampler/config semantics and the NGX/DLSS surface),
`display-change-timing.md` (how a present's screen time is established), `vulkan-forced-fifo.md`,
`cfr-capture-sync.md`.

## Limiter modes (2026-10-18)

- Modes are `auto` (the shipped default for both `capture_sync_limiter_mode` and `general_limiter_mode`), `basic` and
  `reflex`. `ce::limiter_mode_policy` decides: the winning constraint's configured mode (the vblank ceiling resolves
  like auto), auto = game-activated native pacing (D3D Reflex Sleep with `IsGameActivated()`, or the Vulkan backend's
  `isGameActive`) else basic, and an unavailable native API demotes to basic. Auto never starts a CE-owned Reflex
  Sleep in a game without Reflex: without the game's markers that is basic plus a second driver wait, and it would
  call NvAPI in games that never initialized it. Explicit `reflex` remains the opt-in for that.
- `fg_fallback` (value 1) was retired: it ran exactly the basic timer path, because the FG adjustment (displayed-rate
  target divided by the producing multiplier) is common to every mode. Its spellings parse as basic with a
  per-key warning; value 1 stays unused in shared memory. `GetResolvedLimiterMode()` reports the decision;
  `FPS Limiter [AUTO]: sync=... -> selected=...` logs it per constraint.
- Existing `config.ini` files keep the mode they name (a template change only affects new installs).

## FSR FG: the game-thread proxy Present owns the cadence (2026-10-18, validated in Talos 2x at a 120 cap)

- Before: CE paced FSR FG only below AMD's swapchain, i.e. every output on AMD's presenter thread
  (`kRuntimeOutputPresent`, output grid, non-blocking) or, unclassified, as a base-rate application site. That is a
  second pacer behind AMD's own busy-wait pacer, and the game itself was throttled only by AMD's queue filling and
  blocking it inside the proxy Present. AMD's own guidance (FidelityFX frame-interpolation swapchain docs, "Frame
  pacing and presentation", local copy `build/diagnostics/ffx-sdk-source`) is to limit the rendered rate to about
  half the output and leave output spacing to the swapchain.
- Now: `DX12_FFXProxyDetourPresent{,1}` call `Apply(true, PresentSite::kRuntimeInputPresent)` on the game thread,
  outermost only, before the VSync override/prework and before forwarding into AMD (AMD's critical section is not
  yet held), and `ApplyPostPresent()` after a successful forward. Skipped for `DXGI_PRESENT_TEST`, shutdown and
  hook quiescing. The site is a strict-grid group boundary: `presentCadenceTargetFps = output target`,
  `cadenceScale = multiplier`, no duplicate window, no output-group admission (every entry owns a slot).
- Ownership (`ce::runtime_input_site_policy`, `FpsLimiter::PassBecauseRuntimeInputOwnsPacing`) is counted in
  presents, never measured on a clock: each proxy Apply resets a counter; every other Apply passes while fewer than
  `4 x 3 = 12` non-input Applies followed it, and the 13th re-arms output pacing (logs `... paces again (lapses=)`).
  Before the first proxy Present nothing changes. Ownership starts log `the game's Present into the
  frame-generation runtime owns pacing`.
- Capture source at the proxy site is inherited from the output sites (`runtimeOutputStreamIsFinal_`: the last
  non-input Apply was a callback-proven output) or a published final-output route.
- **The post-present half belongs to the thread whose Apply() armed it** (`postPresentOwnerThreadId_`, set by every
  non-passed `Apply(true, ...)`). Otherwise AMD's presenter could run the game thread's armed native sleep, Reflex
  post-present cadence or front-load release: the FFX presenter-thread freeze class. All existing
  `ApplyPostPresent()` callers run on their own Apply's thread, so nothing else changes.
- Flow evidence (`FlowFSRLimiter.*`, both callback and no-callback routes; limiter waits move the virtual hook
  clock via `CEFlow_EnableLimiterVirtualWaits`): 72 fps cap, 2x: scale 2, >=120 passed outputs per 120 frames,
  120 frames span 120 x 27.8 ms; FG off: scale 1, 120 x 13.9 ms. Without the proxy wiring the scale is 1 and zero
  outputs pass, but the span still held, because the lock-step fake throttles the game through backpressure just
  as AMD's queue does - the latency/pacing difference needs hardware (Talos, GTA; overlay 1% low and stddev
  columns, FFX pacer wait, and how often the FSR FG bad-start pacing state appears).
- **The game's release waits for the frame's generated outputs** (2026-10-18, `generated_output_release.h`,
  `ShouldGateReleaseOnGeneratedOutputs`). AMD's presents queue behind the game's GPU work: released at the
  handoff, the game built its next frame while AMD presented the generated output, and that Present waited on the
  GPU until the game's work drained. Talos 2x at a 120 cap (`20261018_195528`/`200054`, vsync forced and not):
  CE's callback commands for the generated output (`CE_FG_GPU_TIMING`) ran 0.29-0.48 ms after its callback, done
  before AMD's Present, yet `gen p2d=1862us` vs `app p2d=470us`; the next command on AMD's present queue started
  ~1.4 ms late, tracking the game's frame end per frame (corr 0.87-0.89, slope 0.6); display intervals 6.5/10.1 ms.
  Now the Apply at the proxy site resets an output counter at the handoff and arms a release that waits for
  `multiplier-1` runtime outputs presented (counted in the passed output's `ApplyPostPresent`, event + the
  high-resolution timer, no polling), bounded by the CPU work budget before the next deadline (the
  present-to-display GPU reservation is excluded: under FG that series is the outputs being fixed). Unmeasurable
  work -> back edge. VALIDATED `20261018_202233` (vsync forced and not): `gen p2d` 0.35-0.50 ms vs `app`
  0.46-0.90 ms, display stddev 40-64 us (was 1.6 ms), steady `byOutputs=600 byBound=0`. The lock-step flow fake presents generated outputs only after the game's next Present, so flow
  runs always take the bound.
- **A passed Apply's post-present call belongs to that output** (`t_FpsLimiterPassedApplyPending`): with FSR FG
  off AMD presents on the game thread inside the proxy Present, and that nested `ApplyPostPresent` used to consume
  the release armed for after the proxy Present, blocking inside AMD's Present. Every `Apply()` clears the mark and
  only a pass with `allowPostPresentReflexCadence` (the caller's promise of a matching `ApplyPostPresent`) sets it
  (2026-10-19): the third-party overlay bypass (`Apply(false, kDuplicateProne)`) and Vulkan acquire-time gating pass
  without a post-present call, and their stale mark skipped the thread's next real release (the game's own after a
  nested present, or the first paced frame after ownership lapsed). The ownership log lines count them as
  `unpaired=`; `FpsLimiterGatedReleaseTest.UnpairedNestedPassDoesNotConsumeTheRelease` and
  `FpsLimiterRuntimeInputApplyTest.UnpairedPassDoesNotLeakIntoTheNextPacedPostPresent` failed before the fix. A
  paired pass whose present fails (no post-present) still leaves the mark until the thread's next `Apply()`.
- Open: plain front-loading stays off under FG (the present-to-display evidence is distorted by AMD's hold); the perf
  CSV's `fps_limit_wait_us` is filled by the DXGI detour only, so the game-thread wait is visible in the limiter
  stats lines, not the CSV; FFX on Vulkan and other proxies CE does not hook keep the output-site pacing.

## DLSS-G enabled but idle: pace the full target (2026-10-19, hardware pending)

- Symptom (Talos `20261019_182822`, capture sync 120, DLSS FG 2x): in the menu 18:31:29.7-36.9 the runtime
  reported `[DLSSG HEALTH] ON but NOT interpolating` while the limiter kept `fg=1/2x fgProof=d3d-sleep
  effective=60`; game and recording ran at 60 fps for 7 s.
- Idleness needs two proofs (`FGCompatibility::IsDLSSGGenerationIdle`, `hook/fg/fg_detection.h`): the health
  streak (`IsDLSSGIdleForPacingEvidence`, >= 12 GetState samples with `numFramesActuallyPresented <= 1`,
  `hook/ngx/dlssg_health_policy.h`) AND about one final output per application frame over the last 16 frames
  (`ce::fg_runtime::OutputRatioWindow`, `hook/fg/output_ratio_window.h`, sliding: 3/4..5/4 outputs per frame; outputs from
  the PostSL final-output plan/skip sites, frames from the title's own PCL PresentStart, or from
  `DX12_ObserveApplicationSourcePresentTiming` only while no title marker was seen). Health alone is unsafe: under
  hardware flip metering MFG reports presented==1 while generating; the callbacks still show 2-4 outputs per
  frame. Resumed 2x generation ends idleness within 5 frames (sum > 5/4 window); a DLSS FG on/off edge resets both
  proofs; auto/dynamic GetState results (`ok` without a fixed count) clear the health half. `FG: DLSS-G output
  ratio ...` logs each verdict change (16, then every 64th). The first version (30 samples, tumbling 32-frame
  windows) showed the base rate for ~1 s at every menu entry (`20261019_195220`: `Input: 40 Dup: 78`).
- Why a window, not per-group counts (2026-10-19): after FSR FG, Talos's fresh DLSS-G swapchain presents every
  output from Streamline's thread (`Post-FSR confirmed standalone normal-route bypass`, T48D0) and the game's
  Present into Streamline never reaches the DXGI hook, so the first design (30 groups of exactly one output) never
  completed a group there. The title marker is the boundary on every route, but it runs on the game thread,
  asynchronous to the outputs: an idle 1,1,1 reads 2,0,1 across the boundary. Once markers are seen they own the
  frame count exclusively: counting both sources would halve 2x into a fake 1.0. The markers deliberately do not
  feed the final-output timeline: `ObserveFinalOutputSourcePresent` replaces the interval on every group (weight 0),
  so boundary jitter would random-walk the virtual clock (see `cfr-capture-sync.md`).
- While idle: `IsFrameGenerationProducingForPacing(..., dlssgGenerationIdle)` is false, so the cadence is the
  full target; the D3D Reflex driver interval becomes target x runtime factor
  (`ResolveIdleGenerationDriverTargetFps`, capped at 6x), so a driver that divides by the FG factor lands on the
  target and one that does not is still capped by CE's cadence. The external native backend is unchanged.
  `fgProof=dlss-g-idle` in the limiter line; `[DLSSG HEALTH] ON but idle for pacing` /
  `interpolating or off for pacing` per transition (16, then every 64th).
- Coverage: `tests/test_fps_limiter_dlssg_idle.cpp` (60 -> idle 120 -> 60, flip-metered case stays 60, markers vs
  double-counted boundaries), `tests/test_fg_output_ratio_window.cpp` (jitter, resume, uncounted outputs, wiring).

## Post-FSR DLSS-G bypass is a final-output boundary (2026-10-19, hardware pending)

- `ApplyFpsLimiterBeforeBypassedFinalOutputPresent` (the post-FSR confirmed-standalone bypass, Present and Present1)
  carries every Streamline output; the game's Present never reaches the DXGI hook there. It applied the limiter as
  `kUniqueApplicationPresent`, so no output-group admission: two evaluations per game frame at 4x. After each
  >250 ms FG-switch gap (`HasRecentLargePresentGap` re-bases the fresh-Sleep streak) the Reflex warmup needs a new
  game Sleep at every evaluation (`ResolveReflexPacingDecision`, `useGameSleepWarmup` requires `gameSleepAdvanced`);
  the second evaluation never had one, logged `REFLEX sleep stalled` with `recent=1`, fell to timer fallback and
  re-based the streak again: CE timer-paced Streamline's thread forever (`Local timer stats avgFps=30.1
  lastWait=29835us`, PC latency 115-145 ms at 4x, 57 ms at 3x; 2x was fine because it entered without a gap).
- Now `ResolveBypassedFinalOutputSite` (`kFinalOutputBoundary`, FSR callback-proven outputs keep
  `kRuntimeOutputPresent`): one owner per multiplier-sized group, warmup completes in 3 frames, the driver owns
  pacing. Coverage: `tests/test_fps_limiter_bypass_site.cpp` (auto mode, 4 outputs per frame: boundary hands off
  with zero CE wait; the application-present site keeps timer-pacing).
- Open: the warmup's per-evaluation progress requirement still assumes one evaluation per game frame on every site.

## Queue-depth and limiter invariants

- D3D10 limit zero uses a native event query; D3D10 limits 1-6 set the device's DXGI maximum frame latency
  (`IDXGIDevice1`, re-applied when it differs). Until 2026-10-18 none of it ran: both DXGI Present detours admitted
  `APIType::D3D11` only (now `ShouldApplyD3D10Or11PrerenderLimit`), and `ApplyPrerenderLimit` asked for
  `ID3D11Device` first, which a D3D10 swapchain also answers (D3D10-on-D3D11; WARP D3D10.1 confirmed by
  `FlowD3D10Prerender`). The swapchain's classification (`SelectPrimarySwapChainAPIType`) now picks the device; the
  other device is only a fallback; `Prerender limit N on the <d3d10|d3d11> path` logs the route on change. D3D11
  (`hook/d3d11/dx11_hook_prerender.cpp`) uses an `ID3D11Fence` marker stream: the marker is signaled after the
  forwarded Present (`D3D11PrerenderPresentScope` in both DXGI Present detours), and before the next Present the thread
  waits in the kernel (`SetEventOnCompletion`) for the marker `limit` presents back
  (`hook/pacing/prerender_lookback.h`); a needed wait first flushes what the frame recorded, so the GPU is never idle
  behind the CPU's wait. d3d11.dll's `Signal` runs `NDXGI::CDevice::Flush` before the kernel signal (10.0.26100,
  cdb disassembly 2026-10-18), which is why the marker goes after Present, where the context is empty, and never before
  it. Limit 0 signals and waits before the Present. Devices without fences fall back to the event-query ring, the only
  D3D11 path that polls (`GetData` + `SwitchToThread`); until 2026-10-18 it was the only path and spun the present
  thread 0.05-2 ms per frame in GPU-bound BioShock Infinite (20261018_005433), hidden in `startup_routing`. The wait is
  charged to the `limiter` present stage, fills the perf CSV's `prerender_wait_us` and is summarized every 10 s
  (`[D3D11 PRERENDER] path= limit= presents= blocked= wait(meanUs maxUs totalMs) markers=`); flow test
  `tests/flow/test_flow_d3d11_prerender.cpp` (WARP). D3D11 rings and DX12 fence rings are serialized and rebound when
  device/queue identity changes. Configured waits do not silently escape after an 8/16 ms timeout while GPU- or
  vblank-bound.
- During DX12 frame generation, the CPU prerender fence ring advances only on a proven application-source Present
  and stays pinned to the retained original game queue, with the fence device queried from that exact queue for
  multi-device Streamline topologies. Streamline/FFX output workers, opt-in eager startup draws, and
  unknown-provenance runtime Presents skip only this limiter; their overlay/capture routing remains unchanged.
  Waiting on a runtime-generated Present or rebinding the ring to a runtime wrapper/presenter queue can deadlock
  because that queue may not retire until the same Present returns.
- DirectDraw owns one fixed-capacity ring across recognized application presents. Limit `0` queries completion of the
  just-submitted operation; limits `1-6` wait for the successful submission exactly that many presents behind before
  replacing its slot. Flip uses `GetFlipStatus(DDGFS_ISFLIPDONE)` (Surface slot 18), while Blt/BltFast use
  `GetBltStatus(DDGBS_ISBLTDONE)` (slot 13). Each queued COM interface is referenced until waited or reset, and a
  primary-chain or configuration transition drains ownership without retaining stale raw surface pointers. The retired
  implementation called slot 13 as if it were `GetFlipStatus`, stored unreferenced pointers, and approximated
  fractional limits with a sleep; all three violated the integer queue contract.
- Vulkan `cpu_prerender_limit=1-6` uses a per-queue seven-fence marker ring; `0` waits the current marker. OpenGL uses
  the same lookback semantics per context. Vulkan drains and resets outstanding markers when the configured depth
  changes so a previously signaled fence is never resubmitted. A non-graphics present queue does not disable the
  setting: one-shot present-topology learning propagates graphics-producer ancestry through every submit wait/signal
  dependency, follows every final present wait transitively across compute queues, and caches the same-device graphics
  queue per swapchain. It learns the bounded ring of exact graphics signal semaphores that feed presentation. For a
  same-thread producer, Present appends the marker directly; for a producer owned by another host thread, the matching
  `vkQueueSubmit*` wrapper appends the marker immediately after the application's boundary submit, before releasing
  that thread's queue-serialization scope. This preserves Vulkan external synchronization without borrowing the queue
  cross-thread, silently disabling the override, or inserting a marker on the compute/present engine. Dependency-map
  learning stops after at most the swapchain image count (capped at eight); steady-state matching retains only the
  small registered boundary set.
- Flip-model latency waitables are requested at creation whenever `backbuffer_count` is active. Wrapped DXGI waits at
  the post-Present/next-frame boundary so simulation/render work cannot begin behind a full vsync queue.
- **CE's D3D presentation policy applies only while CE owns presentation.** `WaitBackbufferFrameLatency`,
  `ApplyPresentFrameLatencyOverrides`, the create-time descriptor override, and the resize-time override are all gated
  on `DXGIShared::IsVulkanActive()` - the same evidence-based decision `CheckAndInstallHooks` already publishes. When
  the CE Vulkan layer owns presentation, every DXGI swapchain CE can reach is the graphics runtime's WSI transport
  behind `VkSwapchainKHR`: the ICD creates it, presents it from a driver-owned thread, and *joins that thread inside
  `vkDestroySwapchainKHR`*. Blocking it there deadlocks the game, and the layer has already applied image count and
  prerender depth on the real Vulkan swapchain, so descriptor/latency policy on the D3D-side copy is a second
  application on an object CE does not own. The sole FIFO exception is the non-blocking final-DXGI body path above:
  it leaves every COM object/vtable untouched and changes only `SyncInterval`/tearing arguments because session
  `20260828_162056` proved the Vulkan present mode does not communicate that required generated-output contract. See
  `llm-wiki/log/recent.md` 2026-08-19 (DOOM Eternal) and 2026-08-28 (Portal shared-vtable crash).
- **The flip-queue pacing wait is bounded and retires itself.** One implementation
  (`DXGIShared::WaitFlipQueuePacingObject`, `hook/present/dxgi_shared_present_pacing.cpp`) serves every transport; the
  Present path's inlined 16 ms copy and `CWrapDXGISwapChain::WaitFrameLatency`'s `INFINITE` copy are gone. The ceiling
  is `ce::present_pacing_policy::kFlipQueuePacingWaitCeilingMs` (1000 ms), chosen to sit far *above* the slowest
  healthy wait (24 Hz x 6 queued frames is ~250 ms) so it cannot silently escape while GPU- or vblank-bound, and far
  below anything a player would call a freeze. A wait that misses the ceiling latches pacing off process-wide rather
  than paying the ceiling on every later present. Both halves matter: commit ccbdeac5 fixed the freeze with a 16 ms
  ceiling that broke the pacing, and dd30a5b6 restored the pacing by restoring the freeze.
- **CE paces only on a waitable object CE added (2026-10-02).** Every create that adds
  `FRAME_LATENCY_WAITABLE_OBJECT` tags the created swapchain with DXGI private data
  (`ce::swapchain_flag_policy::NoteCeAddedFrameLatencyWaitable`, `swapchain_flag_apply.h`); both pacing transports
  wait only when `DidCeAddFrameLatencyWaitable` says so (`ShouldWaitForFlipQueueRoom(..., ceAddedWaitableObject)`).
  A waitable the game or an FG runtime requested itself is waited on by its creator once per frame. DXGI refills it
  once per retired present, so CE's extra wait takes the count the creator's next frame needs. At
  `SetMaximumFrameLatency(1)` the wait before a present can only be satisfied by that same present, so it runs
  into the 1000 ms ceiling. Evidence (GTA V Enhanced `20261002_060100`, logs not kept):
  - the first DLSS-G enable after a save load created `sl.dlss_g`'s swapchain with flags `0x842`, BufferCount 6;
    CE logged `Preserving swapchain descriptor for authoritative FG runtime create`, so the flag was not CE's;
  - `SetMaximumFrameLatency(1) OK (cpu_prerender_limit)`, then on that chain's first present `flip-queue pacing
    wait did not complete within 1000 ms` and `DetourPresent TOTAL SLOW 1001.5ms`;
  - later DLSS-G and FSR chains did not stall only because the miss had latched pacing off for the process.
  `SetMaximumFrameLatency` still applies to a creator-owned waitable: the creator's own wait then enforces the depth.
- **The cadence wait is front-loaded: the deadline decides when a frame is PRESENTED, not when the game may BUILD it.**
  Spending the whole wait after the game already finished rendering ages the finished frame by the wait. Strange
  Brigade DX12 (session `20260913_124032`, 90 fps cap) built a frame in a median 1.8 ms (stddev 0.15 ms) and then sat
  in CE's present hook for a median 9.3 ms of every 11.1 ms period; the overlay's PC-latency chain measured exactly
  that - `anchorToPresent=20.5ms presentToDisplay=0.4ms`, against `11.2ms/7.5ms` and 24.2 ms published for a
  front-edge third-party limiter in the same scene. `ApplyPostPresent()` now releases the game
  `ResolveFrameWorkBudgetUs()` before the next deadline instead, so the frame is built last and presented
  immediately. The budget is the measured high-water of recent frame work (release -> next `Apply()` entry, 64-sample
  ring) plus the adaptive timer margin - a ceiling rather than a percentile, because overrunning it makes the present
  late and a late present re-phases the general cadence; one hitch saturates the ceiling and parks the placement back
  at the back edge until it ages out. **The budget is a latency control only, never a rate or correctness one:**
  `localTargetTime_` and the pre-present wait are untouched, so a skipped release (`CancelPostPresentPacing()`, a
  failed Present, a call site that never runs the post-present half), an unmeasurable work time, or a budget of a
  whole interval all degrade to the original back-edge placement with the cap and the grid phase intact.
  `ShouldFrontLoadCadenceWait()` requires a grid-gated site that runs the post-present half, no active frame
  generation (the present stream is not CE's to re-phase, and blocking after a runtime-owned present is the FFX
  freeze class), **no capture sync** (see below), and no explicit Reflex/native post-present cadence already owning
  the slot. Diagnostics ride the 120-frame stats line as
  `frontLoad=/budgetUs=/workCeilingUs=/headroomUs=/gpuHeadroomUs=/p2dUs=/p2dFloorUs=/overruns=/releaseWaitUs=/releases=`.
- **An isolated stutter spike costs nothing once it is over (2026-10-18, `front_load_spike_policy.h`).** The cadence
  never needed to recover from a spike (a late general frame re-bases `now + interval`, capture sync skips whole
  slots, no catch-up burst), but the budget did: the ceiling was the max of 64 samples (one sub-interval spike held
  the release early for 64 frames) and one overrun grew `frontLoadHeadroomUs_`, which decays an eighth per clean
  64-frame window (~10 s for a 4 ms spike at 120 fps). Now the ceiling is the 3rd-largest sample
  (`ResolveSpikeTolerantWorkCeilingUs`) and an overrun grows the reservation only if another one happened within
  the last 64 released frames (`ResolveRecurringOverrunUs`, grows to the larger of the two); isolated ones log
  `front-load isolated overrun ignored`. The GPU reservation already used a 64-sample median. Recurring variance is
  still covered: the 3rd-largest ceiling lets the top two samples overrun, the second overrun grows the reservation.
  Tests: `FpsLimiterSpikeTest.*` (the isolated case fails on the old code), policy tests. The FG generated-output
  release bound uses the same ceiling and reservation.
- **Unreachable frames and cadence changes (2026-10-18).** A frame whose own release-to-Apply work is an interval
  or more is not an overrun (`ResolveRecurringOverrunUs(..., frameWorkUs)`, `lastFrameWorkUs_`): GTA V Enhanced
  at a 120 cap reached ~62 fps, every frame ~8.2 ms late (< 8.33 ms), and grew `headroomUs=8288`. And all budget
  state resets when the cadence target or scale changes (`frontLoadCadenceKey_` in
  `NoteFrameWorkForFrontLoadedRelease`, trace `front-load state reset for a new cadence`): that 8.3 ms reservation
  survived into FSR FG's 60-group cadence and kept the generated-output release disarmed all session
  (`20261018_204650`). Tests: `FramesThatCannotReachTheCapNeverGrowTheReservation`,
  `ACadenceChangeStartsTheBudgetFresh` (both fail without the fix).
- **The reservation above the work ceiling is learned from real overruns, not padded for everyone.** A ceiling over a
  sliding window of N samples is by construction exceeded by roughly one in N+1 later frames: at 90 fps with the
  64-sample ring that is over one missed deadline per second, which is exactly where a 1% low is measured. Session
  `20260913_130052` showed the cost - front-loading moved the published 1% low 86.9 -> 84.9 fps, the 0.1% low
  86.5 -> 83.2, the overlay frame-time stddev 148 -> 225 us, and the limiter's late-frame rate 0.24% -> 0.63% with
  overruns of 24-455 us (hitches of 6.9-718 ms are a separate population and are excluded by the interval bound).
  `GrowFrontLoadHeadroomUs()` raises the reservation to the worst sub-interval overrun observed while the placement
  owned the frame; `DecayFrontLoadHeadroomUs()` removes an eighth of it per clean 64-frame window and reaches zero,
  so a one-off overrun cannot hold it for the session. Only a frame whose release actually ran can move it -
  a hitch, a back-edge frame, or a frame with no release says nothing about the budget.
- **Capture sync keeps the back edge.** Front-loading removes the slack the game had before its deadline, and under
  capture sync a missed deadline does not cost a fraction of a millisecond of frame time - it skips whole CFR grid
  slots through `AdvanceCaptureSyncDeadlineAfterLateFrame()`, which is a repeated frame in the recording. While a
  recording is the product the capture grid outranks input latency, so `usingCaptureSync` disqualifies the
  placement outright rather than relying on the overrun controller to stabilise it.
- **The budget must cover the frame's GPU half, not just its CPU half - and below that threshold the placement buys
  nothing.** Writing L for input-to-photon, B for the budget, W for the frame's whole CPU+GPU work and F for the
  irreducible flip latency: `B >= W` gives `L = B + F` with every screen time pinned to the grid, and `B < W` gives
  `L = W + F` with screen times following GPU completion. Shrinking B below W therefore buys **zero** latency and
  pays for it in jitter; the optimum is exactly `B = W`. Strange Brigade DX12 is GPU-bound (a 1.8 ms CPU frame in
  front of ~8.5 ms of GPU work), so the CPU-sized budget landed well under W: session `20260913_132320` measured
  present-to-display at 6.8 ms against a 0.4 ms floor, and because the screen time was then set by GPU completion
  the game's own variance reached the display timeline. `ResolveFrontLoadGpuExcessUs()` reads that excess and
  `UpdateFrontLoadGpuHeadroom()` integrates it into the reservation; `DecayFrontLoadGpuHeadroomUs()` walks it back
  in by one timer margin per clean 64-frame window, a bounded probe rather than a proportional decay that would
  periodically put the GPU a large step past the deadline just to discover it no longer needs to be there.
- **No displayed-transition evidence means no front-loading.** `HasUsableGpuCompletionEvidence()` requires a seeded
  present-to-display floor plus a full sample window. The floor may only be seeded while the placement is still at
  the back edge, because that is the only state in which the GPU is known to have finished before the present - a
  floor taken while front-loaded would measure a frame CE released too late. Without that evidence there is no way
  to tell whether releasing the game later pushes its GPU work past the deadline, and the relation above says a
  budget below W buys no latency at all, so the back edge stays the proven default and the withholding is logged.
- **A D3D10/11 frame must reach the GPU before the cadence blocks, not at Present.** The immediate context holds the
  game's recorded frame until a Flush or Present; the limiter waits in the present hook BEFORE the forwarded
  Present, so the GPU idled through the wait and rendered the frame afterwards. Then `p2d` is the frame's whole GPU
  time, the screen time follows GPU completion (the `B < W` case above, but caused by CE, not the budget), the
  `gpuHeadroomUs` reservation saturates at a whole interval and front-loading never arms. Evidence, BioShock Infinite
  x86 DX11 at a 140 fps cap (`20261018_020443`, `cpu_prerender_limit=1`, vsync fifo, 4K): `[Overlay] Pacing health`
  display-change stddev 350-615 us and `displayJagUs` ~680 against a 138 us present-start stddev in the CSV
  (`qpc_us + pre_present_us`), `p2dUs` 3.5-3.9 ms against a 422 us floor, and a two-frame alternation in the CSV:
  `prerender_wait_us` 0 / ~1.3 ms, `fps_limit_wait_us` ~4.8 / ~3.4 ms - only the frames the prerender wait had
  flushed finished on the GPU during CE's wait. The uncapped run of the same session showed display stddev 50-80 us.
  Fix: `FpsLimiter::PreWaitSubmission` - the DXGI sites arm it per `Apply()` through
  `DXGIShared::ApplyFpsLimiterStage()` (`hook/present/dxgi_limiter_stage.cpp`, thread-local, nest-safe), and
  `RunLocalCadence()` runs it once when it is about to wait: `SubmitD3D1xRecordedWork()` flushes the swapchain
  device's immediate context (D3D10: the device). A late frame (no wait) submits nothing; D3D12 swapchains resolve
  to no D3D10/11 device. D3D9, D3D8, DirectDraw and OpenGL pace after their Present, so nothing is pending there.
  Diagnostics: `preWaitSubmits=`/`submitMaxUs=` on the 120-frame stats line, one `DXGI: FPS limiter submits the
  recorded frame before it waits` line per device. Unit tests: `test_fps_limiter_pre_wait_submission.cpp`.
- **Pre-limiter captures carry the present release, not the copy's moment (2026-10-19, validated `20261019_173119`).**
  The DXGI detours capture in ProcessFrame and only then wait, so a capture stamped with its own QPC carried
  "previous release + build time": BioShock Infinite at capture sync 120 (`20261019_164020`) presented on an exact
  grid while its stamps sat 1.2-6.1 ms early and moved ~4.5 ms with load, and media's phase lock answered with
  rephases, releases and repeat+skip pairs (evidence: `cfr-capture-sync.md`). Mechanism
  (`hook/pacing/present_release_forecast.h`, `fps_limiter_detail/release_forecast.h`): every `Apply()` holds a
  `ReleaseForecastScope`; on the timer cadence path after `RunLocalCadence()` it settles the previous capture's
  claim against `cadence.releaseQpc` and publishes `localTargetTime_` (the next pre-present deadline) with the grid
  interval and its thread id - only for `kUniqueApplicationPresent`, FG off, cadence scale 1, and inside an open
  `ScopedPreLimiterCapture`; every other path (inactive, Reflex/native, post-present Reflex, FG, dedup, bypass
  branches) withdraws this thread's forecast. `StampPreLimiterCapture(captureQpc)` uses it only inside the scope
  and only for the owner thread: a deadline 0-2 intervals ahead becomes the stamp, a passed deadline keeps the
  capture clock (the limiter presents at once), anything further is stale. The scope is opened before ProcessFrame
  and closed after the limiter stage in `ExecutePresentCore`, `DetourPresent1` and the wrapper's
  `Present`/`Present1` (closed before the wrapper's DX11 pass-through Present). Sites that pace after Present (D3D9,
  D3D8, OpenGL, DirectDraw) or capture after the wait (Vulkan) never open it - opening it there would stamp a frame
  with the NEXT frame's deadline. Known limit: a cadence that re-bases between capture and wait (target, mode or FG
  change) leaves that one frame on the old grid's slot; it is counted, not hidden. Diagnostics: one
  `FPS Limiter: pre-limiter captures stamped by <source>` line per source change (first 16, then every 64th) and
  ` stamp(release= clock= passed= far= claims=matched/missed/unreleased leadMeanUs= leadMaxUs= claimErrMaxUs=)` on
  the 120-frame stats line (`TraceLog`'s buffer grew to 1024: the stats line alone had reached its ~480-character
  limit). Unit tests: `test_present_release_forecast.cpp`.
- **The wait primitive's own lateness is reported, not inferred.** The same session's present starts landed 0 us
  late on about half the frames and 25-300 us late on the rest (phase against the rational grid, frames that
  waited), while the stats line said `late=0` - that counter only measures arrivals after the deadline, not wakes.
  `NoteSmartWaitWake()` records every real-clock return: `wake(n= lateMeanUs= lateMaxUs= late= timerLate=
  marginUs=)`, where `timerLate` means the kernel timer itself woke past the deadline (fine margin too small,
  clamped at 250 us) and the rest of `late` means the yield phase lost the CPU (`SwitchToThread`). Which of the two
  it is decides the fix; it was not changed blind.
- **The answer was the timer, and the 250 us margin cap.** Next run `20261018_025335` (0.1.7229): `late == timerLate`
  in every window, 30-45% of waits (55-126 of 120-240), `lateMaxUs` 270-685, `marginUs=250` pinned at the cap,
  `fineLate=0`. A local probe on the same machine (Windows 11, Zen 3; 300 high-resolution 3 ms waits per run,
  repeated) explains it: with the system tick at 1 ms (`timeBeginPeriod(1)`, `NtQueryTimerResolution` 10000) a
  third of the wakes form a cluster 500-600 us late (median 56-254 us); at the default 15.6 ms tick the median is
  32-64 us and the cluster is a few percent (p99 still 515-670 us). CE's basic limiter never requested the tick, so
  BioShock holds it itself. Fix: `ResolveSmartWaitFineMarginUs()` = measured p99 + 25 us, bounded by the final spin
  and one scheduler tick (1000 us), over a 256-wake ring (`SmartWaitP99Index`, nth_element) so two outliers do not
  set it; and `EnsureTimerResolution()` only requests the 1 ms tick when the high-resolution timer is missing (the
  explicit Reflex cadence used to request it). Cost: the fine phase yields/spins for about the margin per wait,
  reported as `fineMeanUs` - at 140 fps and a ~650 us margin roughly 9% of one core, twice with a front-loaded
  release. Same run, display-change stddev was 128 us with `frontLoad=0` and 303-442 us with `frontLoad=1` (late
  release wakes start the frame late); re-check that split once the wakes are on time.
- **Which timeline a pacing claim is about matters.** The overlay publishes its FPS percentiles and frame-time
  stddev from the display series (`PerformanceMetrics::ActiveSeries()` selects `m_display` when the effective source
  is `DisplayChange`), i.e. from screen times, which is the right thing to measure and must stay that way. The perf
  CSV's `qpc_delta_us` is the present-hook entry timeline. The two can move in opposite directions: on
  `20260913_132320` front-loading improved the PRESENT timeline (stddev 194 -> 169 us, |frame-to-frame delta|
  189 -> 126 us) while the DISPLAY timeline regressed (published 1% low 86.9 -> 85.0 fps, stddev 148 -> 202 us).
  Compare the overlay's own `source_1pct_low_x100` / `source_frametime_stddev_us` columns over a whole run; a
  hand-picked CSV window will happily show the opposite conclusion.
- **`SmartWait()` must not arm the kernel timer for less than a scheduler tick.** `EnsureTimerResolution()` puts the
  scheduler on a 1 ms tick and a wait armed for less cannot land inside it - it sleeps to the next tick, past the
  deadline. Invisible while the limiter's waits were whole milliseconds; front-loading made the pre-present wait
  hundreds of microseconds and the measured overshoot went from a 37 us median (212 us worst) on ~9 ms coarse waits
  to an 88 us median (561 us worst) on ~500 us ones. The coarse timer is now only armed when the coarse portion
  exceeds a tick; below that the existing yield/spin loop owns the whole wait. The decision is the pure
  `fps_limiter_policy::SmartWaitArmsKernelTimer`, tested exactly.
- **Tests never time the limiter on the real clock.** Every limiter time read goes through `FpsLimiter::ReadClock()`
  and every wait through `SmartWait`, so `SetClockSourceForTesting` (unset in production: QPC + SmartWait) lets
  `FpsLimiterTest::UseVirtualClock()` make a wait land exactly on its deadline. Reflex game-sleep recency
  (`HasRecentGameSleep(500)` gates DLSS-G pacing) reads `ReflexLimiter::NowTickMs()`, which the fixture ties to the
  same virtual clock. A new limiter test that asserts WHEN a caller is released must use the virtual clock and
  `clock.Advance()` for simulated game work or hitches - never `sleep_for` or measured QPC spans.
- **Front-loading does not recover the whole hold, because the presentation queue takes what the limiter gives up.**
  On `20260913_130052` `anchorToPresent` fell 20.5 -> 11.8 ms exactly as designed, but `presentToDisplay` rose
  0.4 -> 6.8 ms, so the published estimate went 26.4 -> 24.2 ms - a real 2.2 ms, not 9. Presenting earlier moves the
  frame's wait from CE's sleep into the flip queue; what is left is a presentation-queue depth question
  (`backbuffer_count`, maximum frame latency), not a limiter-placement one. The same session's third-party
  front-edge limiter published 24.2 ms, i.e. the same floor. A corollary that matters for the overrun controller:
  while the queue still holds the frame, a few hundred microseconds of extra budget do not reach the screen at all.
- The timer limiter uses a rational QPC/Bresenham grid, never emits a short catch-up interval after a missed deadline,
  and arms a high-resolution timer before the deadline. Capture-sync late recovery advances by whole rational-grid slots
  until the next deadline has at least half an interval of headroom, preserving source/CFR phase through a hitch;
  general limiting retains now-relative recovery. The fine margin is `clamp(p99 timer wake overshoot + 25us, 50us,
  1000us)` (250 us until 2026-10-18, see above); only the final 50us is a tight spin.
- **The Apply() call-site contract is `ce::fps_limiter_policy::PresentSite`, not a clock.** `kDuplicateProne` is the
  legacy default (DXVK Present+PresentEx, the D3D9/D3D8/DDraw/OpenGL wrappers): a second call there really is the same
  logical frame, so the 0.5-2 ms duplicate-present window still classifies it. `kUniqueApplicationPresent` is a site
  that structurally cannot deliver a second entry for one application present - the DXGI `Present`/`Present1` detours
  and `CWrapDXGISwapChain` are mutually exclusive (`IsInWrapperPresent()`) and guarded by `IsRecursivePresent()`, so
  nested/cross-thread re-entries return before `Apply()`. `kFinalOutputBoundary` is a site that observes every final
  presented output including generated frames (native-Vulkan present/acquire) and is the only contract allowed to own
  output-group admission. `ShouldGateEveryApplyOnCadenceGrid()` selects blocking cadence-lock serialization;
  `ShouldUseDuplicatePresentWindow()` separately selects time-based duplicate filtering.
- **DXGI top-level presents are gated on the grid, FG off.** Strange Brigade DX12 (session `20260913_122208`, cap 90,
  `general_limiter_mode=basic`) presented ~130 fps with alternating short/long frame times while the limiter's own
  stats read a perfect `waited=120 late=0 avgFps=90.0`: the game renders a frame in 1-2 ms, so ~46 genuine presents
  per second landed inside the 2 ms `activeDedup` window, were classified as duplicate presents and reached the
  swapchain completely unpaced (`activeDedup` grew ~58 per 120 paced frames; 90 paced + 44 escaped = the observed
  129/s). The duplicate window can only misfire on a recursion-guarded boundary, so those four sites now pass
  `kUniqueApplicationPresent`. While frame generation is producing, the same DXGI stream also carries runtime-owned
  generated presents (FFX presents an interpolated frame from its own proxy swapchain) that CE cannot yet classify
  structurally there, so FG keeps the established window - gating every entry would both spend a base-rate grid slot
  on a generated present and block the runtime's presenter thread inside CE's cadence lock (the FFX freeze class).
  That qualification is NOT the rejected `strictGrid = boundary && !FGActive` escape: a real final-output boundary
  stays unconditionally strict and is owned by `OutputGroupAdmission`.
- **A callback-proven FG runtime output is its own site (`PresentSite::kRuntimeOutputPresent`, 2026-09-30).**
  Callback-owned native FSR FG presents EVERY output (generated and application) from AMD's presenter thread through
  CE's DXGI detour, and the game's own Present never reaches it (it goes into AMD's proxy). As
  `kUniqueApplicationPresent` this stream was modelled as base frames while every Apply was one output: the general
  cap divided by the FG multiplier per output (120 cap -> 60 displayed), and capture sync flipped from
  `captureSource=final effective=60` before media's inject handshake to `captureSource=base captureEq=240` after it
  (Talos `logs/20260930_032355`: ~1.2 s at half rate on every recording start, then FFX pacing took seconds to settle;
  a 144 general cap would have replaced capture sync). The detours now read the present-callback verdict at entry
  (`present_association::PeekPresentFrameVerdict`, before ProcessFrame consumes it; `ctx.callbackProvenRuntimeOutput`)
  and select the site through `ResolveDxgiPresentSite`, the bypass helper included. For that site
  `IsInjectCaptureFinalOutputForSite` makes the capture source final output (DX12 capture records every
  callback-proven output since eec94472), and `ResolveLocalCadencePlan` splits two grids: the present grid paces each
  output at the output target (scale 1), the render grid (game-owned Reflex hybrid spin, once per rendered frame)
  takes target/multiplier on the exact rational group grid. The Reflex driver interval keeps the base target, since
  FSR's generated frames are invisible to it. The site stays non-strict (presenter thread, FFX freeze class), but
  `ShouldUseDuplicatePresentWindow` excludes it from BOTH dedup paths independently of lock policy. A non-strict
  lock is not evidence that two calls are the same frame: a 100 us output burst otherwise escaped the general cap
  (also when that cap beats capture sync), and native handoff's inactive dedup skipped its next evaluation.
  The existing duplicate windows remain for `kDuplicateProne` and FG-active unknown application sites only.
  `LOCAL timer start` reports `site=3 strictGrid=0 dedupWindow=0`; `ACTIVE dedup` now reports the site too. Tests:
  `tests/test_fps_limiter_runtime_output_{site,bursts}.cpp`, `PresentCallbackAssociationTest.Peek*`. Burst tests
  compile the real inline limiter under an isolated clock/wait namespace and assert cadence/native-call counts,
  without scheduler timing or real waits. The same fixture covers GPU deadline reservation growth: overriding
  CPU work alone does not remove host-driven wait overshoot, which can otherwise saturate the budget before
  the test supplies GPU delay. Three burst tests fail before the dedup fix. Expected trace:
  `Apply: ACTIVE ... target=120 effective=60 group=120/1 ... captureEq=120 captureSource=final site=3`, unchanged
  across the handshake. Hardware validation pending.
- **Every DXGI routing branch that forwards a real Present early must run the limiter stage itself.** The normal
  route's `Apply(true, kUniqueApplicationPresent)` sits late in `ExecutePresentCore`; any branch in
  `ExecuteStartupRouting` / `DetourPresent1` that returns through a bypass trampoline skips it. The post-FSR
  confirmed-standalone Streamline bypass (`ShouldBypassPresentForConfirmedStandaloneStreamlinePresentOnNormalRoute`,
  taken after an FSR FG -> DLSS FG switch with the stale Steam hook chain) is steady state, not startup: it carries
  every final output for the rest of the process. Until 2026-09-29 it never called the limiter, so capture sync and the
  general cap stayed `Inactive` until a game restart (Talos session `20260929_040222`: no `FPS Limiter: Active` line,
  ~140 fps against a 120 capture cap). Both bypasses now call
  `DXGIShared::ApplyFpsLimiterBeforeBypassedFinalOutputPresent()` before the vsync override and
  `ApplyPostPresent()` after a successful forward; wrapper-owned presents are skipped (the wrapper already paced
  them). The helper logs `FPS limiter pacing|idle on <route> bypass` on transitions. Guarded by
  `tests/test_dxgi_shared_bypass_limiter.cpp`. The transient startup-handoff bypasses and the synthetic re-entrant
  route (nested inside an already-paced app Present) deliberately stay limiter-free.
- Native Vulkan presents are paced through the grid with `Apply(PresentSite::kFinalOutputBoundary)` on EVERY present
  (both `vkQueuePresentKHR` and the async `vkAcquireNextImageKHR` path), not only the first present entering the hook.
  Strange Brigade Vulkan presents several real swapchain images per frame period from concurrent present streams;
  the old first-present-only gating plus the 2ms dedup fast path let those extra images reach the driver unpaced, so
  a 60fps target displayed ~120fps (vsync-capped in intros) with alternating short/long frame times and bad 1% lows.
  The strict grid takes the cadence lock blocking (concurrent streams serialize onto the grid: exactly one present
  per target interval, evenly spaced) and bypasses both dedup fast paths. DXVK keeps the legacy first-present gating +
  dedup because its CS thread presents once per frame while the DX9/DXGI hooks already pace the game thread. FG-scaled
  LEGACY (`kDuplicateProne`) call sites keep the dedup so generated frames are not pushed onto the base-frame grid;
  the FG-active real-boundary path instead uses the grouped admission below. The strict path
  works identically with FIFO vsync enabled or off: the wait happens before the driver call and the game's present
  mode is left untouched.
- **FG-active real boundaries use deterministic multiplier-sized output-group admission, not a time window.**
  `ce::fps_limiter_policy::OutputGroupAdmission` (`hook/pacing/fps_limiter_policy.h`) classifies each real
  final-boundary callback (native-Vulkan present/acquire) by a pure ordinal: for an active FG multiplier m, exactly
  one callback per m consecutive callbacks owns a cadence slot (`pace_group`) and the remaining m-1 are the generated
  outputs of that already admitted group (`pass_generated_slot`, lock-free fast path that never touches the cadence
  mutex). Six rapid callbacks at 3x therefore always produce pace/pass/pass/pace/pass/pass, even when they arrive
  back-to-back. This replaced the time-based `activeDedup` window (0.5-2 ms after the last paced return) that
  previously governed FG-active gate-every-present callbacks: that window could not distinguish the next real group
  arriving inside it from generated spillover, so Portal with RTX Remix admitted ~86 extra 3-callback groups and ran
  ~146 fps (167 peak) against a 130 cap. Classification never reads a clock.
- **Admission is serialized, serialized owners never skip, and transitions re-base before classifying.** The ordinal
  lives under a short `admissionMutex_` that is never held across a wait, so a generated slot can always classify
  while a group owner is waiting on the cadence. Owners block on the cadence mutex (no failed-try-lock escape);
  `concurrentApplySkips_` can therefore no longer increase from a real-boundary site, and any nonzero delta in the
  120-frame stats is logged as an invariant violation. An admission-epoch key (limiter activity, capture source,
  FG state, multiplier, boundary kind, cadence grid) is compared BEFORE classification, so the first callback after
  any activation/deactivation, target/source change, FG on/off, multiplier change, IPC/session reset
  (`Shutdown`), or pacing-boundary move (`ResetOutputGroupAdmission()`, called by the Vulkan layer on
  async-present detection edges via `vulkan_present_boundary.h`) owns a clean slot and no partial group leaks across
  the transition.
- **The group cadence is an exact rational rate.** The local grid interval is
  `QPC_frequency * cadenceScale / configured_target` (`NextRationalGroupIntervalTicks`, Bresenham remainder), where
  `cadenceScale` is the FG multiplier for final-output observers (general cap, WGC/DXGI capture sync, and explicit
  DX12/Vulkan final-output inject capture) and 1 for an ordinary/base inject route whose source contains only
  application-rendered frames. A 130 fps cap with 3x FG paces
  130/3 = 43.333... groups/s with zero long-term drift instead of the floored 43 (which capped output at 129). The
  floored integer base target survives only where an integer API demands it (legacy non-boundary sites) and in the
  `effective=` log field next to the exact `group=130/3` ratio; driver-owned intervals take the output rate instead
  (see the frame-generation-aware driver cap below). Capture-sync late
  recovery advances by whole GROUP slots on the scaled grid (`AdvanceCaptureSyncDeadlineAfterLateFrame` takes the
  scale), and the post-present Reflex cadence stays on the unscaled base interval.
- **Pacing-boundary observability.** `hook/vulkan_layer/vulkan_present_boundary.h` (header-only sibling of
  `vulkan_layer_present.cpp`) owns async-present detection for the present hook: both routes - acquire-thread
  mismatch and submit-thread mismatch - now emit a one-time edge log naming the route and the moved boundary (the
  submit-thread route previously switched to acquire pacing silently, which made acquire-time pacing invisible in
  session logs), and every detection edge resets the limiter's output-group admission. One-shot boundary-identity
  logs report `FPS limiter boundary = vkQueuePresentKHR|vkAcquireNextImageKHR` with swapchain, FG multiplier, and
  grouped admission on every change. The 120-frame limiter stats extend with boundaryCallbacks / pacedGroups /
  generatedPasses / groupResets / concurrentSkips deltas. Telemetry stays unclamped: `PerformanceMetrics` keeps
  reporting observed presentation activity (a correct 3x trace under a 130 cap converges near 130; the pre-fix
  ~146 burst pattern stays honestly visible as the higher rate it was).
- Reflex integration resolves `NvAPI_D3D_SetSleepMode` and `NvAPI_D3D_Sleep` from `nvapi64.dll` and calls the original
  entry points directly. NvAPI code bytes/prologues are deliberately not patched because some DLSS FG integrations
  validate them during Reflex setup; `minimumIntervalUs` is pushed proactively, and pacing hands to the game-owned
  Reflex sleep path once stable.
- Native Vulkan never passes a `VkDevice` to that D3D `IUnknown` contract. The layer first cooperates with a game's
  `VK_NV_low_latency2` path by forwarding `vkSetLatencySleepModeNV`/`vkLatencySleepNV`, retaining the game's original
  mode, and overriding only the persistent interval plus explicit low-latency enable while CE's Reflex limiter is
  active. A recent game sleep remains the owner at the correct pre-input point; CE does not issue a duplicate sleep.
  If the game uses the legacy NvAPI Vulkan contract, CE detects and preserves an already-owned context; otherwise it
  initializes its own low-latency device, calls `NvAPI_Vulkan_Sleep` once per base frame after Present, and waits on
  the driver-signalled timeline semaphore before the next simulation/input frame. Failure at any native stage is
  logged and leaves the existing rational-timer fallback available. Auto mode recognizes an already-active modern or
  legacy Vulkan game path without initializing a CE-owned context merely to probe it. FG state/multiplier is imported
  from shared NGX state before mode resolution.
- The configured general limiter value always denotes the final displayed/output rate, independent of `basic`
  or native/Reflex selection (and of the retired `fg_fallback`). Every mode therefore divides its base-present target by a producing 2x-4x
  FG multiplier; mode changes and factor changes reset cadence and emit a new active-state diagnostic. A capture-sync
  target is interpreted in the source domain: WGC/DXGI and explicit DX12/Vulkan final-output inject routes include
  generated outputs, while an ordinary/base inject route contains only application-rendered frames.
- **A nominal Streamline DLSS-G `ON` state is not production evidence.** Gothic Remake session `20260831_223114`
  reported `optionsMode=on`, one presented frame, and the default 2x factor for about 19 seconds before its first
  DLSS-G feature creation or Reflex Sleep. Treating that state as generating divided a 120-fps cap to 60 while the
  game still issued one real Present per refresh. `IsFrameGenerationProducingForPacing()` now requires both the
  DLSS-G runtime signal and a recent successful game-owned Reflex Sleep (or the Vulkan backend's equivalent active
  native ownership). FSR FG and confirmed Smooth Motion retain their presentation-derived active signal. A Reflex
  suspend or stale sleep returns DLSS-G to the unscaled one-slot-per-frame grid; fresh sleep evidence re-enters the
  exact 2x/3x/4x group cadence. Active-state diagnostics separate `fgSignal=` from pacing `fg=` and report
  `fgProof=inactive|pending|d3d-sleep|api-native-sleep|runtime`.
- **Capture sync and the general limiter are concurrent constraints, not a priority list.**
  `ResolveLimiterTargetSelection()` expresses both in the final-output domain and selects the lower rate. The capture
  constraint is `captureFps * capture_sync_multiplier`; an ordinary/base inject route multiplies that value by the
  effective 2x-4x FG factor for comparison, while a final-output source does not. Equal constraints prefer capture
  sync so its phase-preserving CFR grid remains active. Thus a 120-fps general cap remains 120 displayed fps when a
  120-fps base-inject capture starts under 2x/3x/4x FG instead of being replaced by a 240/360/480-fps driver request;
  an explicit final-output route keeps the same 120-fps capture grid and driver request. FG suspension likewise keeps
  the 120-fps contract. Both the game's selected factor and `dlss_fg_factor=` override converge into the same effective
  `FGCompatibility` multiplier before this decision, so limiter behavior is independent of factor provenance.
- **A third simultaneous constraint: the display's vertical-blank ceiling** (`kDisplayVblankCeiling`, 0.1.6560). It
  exists only while CE's Vulkan present-mode override has stood down for a metered frame generator, which is the one
  case where CE itself removed the bound on the presented rate - see
  [vulkan-forced-fifo.md](vulkan-forced-fifo.md), "the ceiling on the rendered rate". The layer publishes the
  display's own maximum refresh (`SetDisplayVblankCeilingFps`, read from the swapchain window's display mode at
  swapchain creation, never a configured constant) as an *output* rate, and the limiter divides it by the live FG
  multiplier like every other final-output target. `refresh / multiplier` rendered frames per second are what make a
  metered batch land one image per vertical blank; Portal RTX 4x asked for 41 x 4 = 164 fps on a 144 Hz panel and
  NVIDIA's generator responded by not scheduling its flips at all. Ranked in the same final-output domain as the
  other two and always last on ties, so a configured cap is never loosened; it carries no limiter mode, so AUTO hands
  it to the driver's own frame-generation-aware interval where Reflex is active. Diagnostics report it as
  `constraints=... vblankCeiling:N` and `sync=vblank-ceiling` in the `FPS Limiter: Active` line. Validated on
  hardware in `20260914_120049`, which reports `effective=36|48|72, group=144/4|3|2, driver=144` across live
  4x/3x/2x changes with the Vulkan Reflex native handoff armed.
- DX12/Vulkan publish final-output inject-route availability directly to the process-local limiter before media's
  delayed inject handshake. The handshake says that inject transport is requested; it cannot by itself say whether
  that transport currently publishes base application frames or final generated outputs. Active-state diagnostics
  report both constraints and `captureSource=base|final`, making future route/factor transitions auditable.
- **NVIDIA's driver-owned low-latency interval is itself frame-generation aware, so it must NOT be given the divided
  base target.** `minimumIntervalUs` (`NvAPI_D3D_SetSleepMode`, `NvAPI_Vulkan_SetSleepMode`,
  `vkSetLatencySleepModeNV`, and the Streamline `frameLimitUs` CE forwards from the same value) constrains the FINAL
  presented rate whenever NVIDIA generates the extra frames: the driver stretches the application's render loop by the
  DLSS-G/MFG factor itself. Portal RTX (`general_limiter_mode=reflex`, 130 cap, 3x MFG, session `20260829_015534`)
  proved it - CE pushed `target=43 intervalUs=23256`, and `perf_metrics` then showed a 69.8 ms group period
  (3 x 23.256 ms) with three bursted presents per group, i.e. 14.3 rendered / 43 displayed fps against a 130 cap.
  `general_limiter_mode=basic` was unaffected because CE's own cadence paces output groups at the present boundary.
  `ce::fps_limiter_policy::ResolveNativeDriverPacingTargetFps()` now feeds every driver-owned interval the output
  rate (`targetFps`, or `targetFps * multiplier` for ordinary/base inject capture sync whose configured value is the
  rendered rate), while CE's local cadence, the post-present Reflex cadence and the hybrid spin keep pacing base frames.
  Third-party generated frames (FSR FG) stay on the base target: they never reach the NVIDIA cap, which throttles the
  game's own Reflex sleep, i.e. the render loop. `DriverLowLatencyIntervalCoversGeneratedFrames()` is the single
  discriminator and reads the same `FGCompatibility::GetRuntimeMode()` snapshot as production qualification, so the
  driver interval and local cadence cannot classify different runtime kinds. The `FPS Limiter: Active (...)` line
  reports the value as `driver=`.
- The hybrid Reflex spin (`ConfigureHybridPacing`) runs once per LOGICAL game Sleep / rendered frame and therefore
  takes the scaled output-group period (`freq * cadenceScale / cap`); deriving it from the floored base target capped
  a 130 fps / 3x configuration at 129. Streamline's `slReflexSleep` may synchronously traverse CE's NvAPI Sleep inline
  hook. A thread-local boundary depth now gives only the outer call ownership of hybrid pacing and successful-Sleep
  evidence; the nested NvAPI observation is coalesced and logged sparsely. Before that ownership rule, one 4x logical
  sleep advanced two 33.3 ms hybrid slots, yielding about 65 ms base groups / 60 displayed fps under a 120 cap, and
  doubled the evidence counter used by native handoff.
- A large Present gap re-bases the game-Sleep evidence counter. Three recent successful game Reflex Sleep calls after
  that edge now prove native pacing has recovered even while the 500 ms gap-diagnostic grace is still active. Waiting
  out the entire grace window layered CE's local cadence over an already healthy game-owned cadence during cutscene/FG
  transitions and could add temporal jitter. On each of the first recovery calls, an accepted driver target plus an
  actually advanced Sleep count suppresses CE's fallback for that frame only; unchanged merely-recent evidence resumes
  fallback on the next evaluated frame, while the third fresh call enables stable hybrid handoff. Old pre-gap Sleep
  observations still cannot trigger either recovery path. A Reflex off/on activation starts a new Sleep-counter epoch
  and discards any larger baseline from the prior epoch, so repeated cutscene suspension cannot strand native recovery.
- Concurrent/re-entrant Present streams cannot advance one cadence: the first caller owns the cadence mutex and other
  callers skip without blocking. VFR disables capture-grid synchronization only, not an independently configured
  general cap.
- Frame-generation scaling depends on the captured source. WGC/DXGI and explicit DX12/Vulkan final-output inject
  routes see presented/generated frames and scale the base target; only ordinary/base inject capture keeps its
  application-rendered capture-sync target undivided.
