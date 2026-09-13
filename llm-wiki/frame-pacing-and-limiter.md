# Frame Pacing And The FPS Limiter

Last cross-checked: 2026-09-13 (GPU-completion-aware front-load reservation; split out of `graphics-overrides-and-frame-pacing.md`; `PresentSite` call-site
contract, DXGI top-level presents gated on the cadence grid, front-loaded cadence release with an overrun-learned
reservation)

Producer-queue depth enforcement (`cpu_prerender_limit`, `backbuffer_count` present depth) and everything the FPS
limiter owns: the rational cadence grid, where in a period the wait is spent, how call sites declare what their
Apply() entries mean, frame-generation output-group admission, and the native low-latency (Reflex) handoff.

Primary sources:
- `hook/common/{fps_limiter,fps_limiter_policy}.h`
- `hook/common/fps_limiter_detail/{apply,frame_pacing,front_load,cadence_diagnostics,lifecycle}.h`
- `hook/common/reflex_limiter.h`
- `hook/common/performance_metrics.cpp` (`ConsumeDisplayTiming` publishes present-to-display to the limiter)
- `hook/common/{dxgi_shared_present_core,dxgi_shared_present1,dxgi_shared_present_routing}.cpp`
- `hook/wrappers/dxgi_swapchain_wrap_present.cpp`
- `hook/vulkan_layer/{vulkan_layer_present,vulkan_layer_swapchain,vulkan_reflex_limiter}.*`
- `hook/vulkan_layer/vulkan_present_boundary.h`
- `tests/{test_fps_limiter,test_fps_limiter_part2,test_fps_limiter_output_groups,test_fps_limiter_present_site,test_fps_limiter_front_load,test_present_pacing_policy}.cpp`

Related: `graphics-overrides-and-frame-pacing.md` (sampler/config semantics and the NGX/DLSS surface),
`display-change-timing.md` (how a present's screen time is established), `vulkan-forced-fifo.md`,
`cfr-capture-sync.md`.

## Queue-depth and limiter invariants

- D3D10 limit zero uses a native event query; D3D10 limits 1-6 use DXGI maximum frame latency. D3D11 query rings and
  DX12 fence rings are serialized and rebound when device/queue identity changes. Configured waits do not silently
  escape after an 8/16 ms timeout while GPU- or vblank-bound.
- During DX12 frame generation, the CPU prerender fence ring advances only on a proven application-source Present
  and stays pinned to the retained original game queue, with the fence device queried from that exact queue for
  multi-device Streamline topologies. Streamline/FFX output workers, opt-in eager startup draws, and
  unknown-provenance runtime Presents skip only this limiter; their overlay/capture routing remains unchanged.
  Waiting on a runtime-generated Present or rebinding the ring to a runtime wrapper/presenter queue can deadlock
  because that queue may not retire until the same Present returns.
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
  (`DXGIShared::WaitFlipQueuePacingObject`, `hook/common/dxgi_shared_present_pacing.cpp`) serves every transport; the
  Present path's inlined 16 ms copy and `CWrapDXGISwapChain::WaitFrameLatency`'s `INFINITE` copy are gone. The ceiling
  is `ce::present_pacing_policy::kFlipQueuePacingWaitCeilingMs` (1000 ms), chosen to sit far *above* the slowest
  healthy wait (24 Hz x 6 queued frames is ~250 ms) so it cannot silently escape while GPU- or vblank-bound, and far
  below anything a player would call a freeze. A wait that misses the ceiling latches pacing off process-wide rather
  than paying the ceiling on every later present. Both halves matter: commit ccbdeac5 fixed the freeze with a 16 ms
  ceiling that broke the pacing, and dd30a5b6 restored the pacing by restoring the freeze.
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
  exceeds a tick; below that the existing yield/spin loop owns the whole wait.
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
  250us)`; only the final 50us is a tight spin.
- **The Apply() call-site contract is `ce::fps_limiter_policy::PresentSite`, not a clock.** `kDuplicateProne` is the
  legacy default (DXVK Present+PresentEx, the D3D9/D3D8/DDraw/OpenGL wrappers): a second call there really is the same
  logical frame, so the 0.5-2 ms duplicate-present window still classifies it. `kUniqueApplicationPresent` is a site
  that structurally cannot deliver a second entry for one application present - the DXGI `Present`/`Present1` detours
  and `CWrapDXGISwapChain` are mutually exclusive (`IsInWrapperPresent()`) and guarded by `IsRecursivePresent()`, so
  nested/cross-thread re-entries return before `Apply()`. `kFinalOutputBoundary` is a site that observes every final
  presented output including generated frames (native-Vulkan present/acquire) and is the only contract allowed to own
  output-group admission. `ShouldGateEveryApplyOnCadenceGrid()` maps the contract onto the strict grid.
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
  `ce::fps_limiter_policy::OutputGroupAdmission` (`hook/common/fps_limiter_policy.h`) classifies each real
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
- The configured general limiter value always denotes the final displayed/output rate, independent of `basic`,
  `fg_fallback`, or native/Reflex selection. Every mode therefore divides its base-present target by a producing 2x-4x
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

