# llm-wiki Log Archive 2026-W35b

### 2026-08-29 - Display-change frame timing: the correlation was keyed on the wrong thread

`[Overlay] frametime_source=display_change` (ABI 48) ships: the sensor child runs the graphics event session,
correlates runtime presents with display completions, and publishes screen-change timestamps into a 512-slot
shared ring that each DXGI and Vulkan overlay consumes with its own cursor. `PerformanceMetrics` keeps a second
series for it and falls back to presentation timing whenever the stream is unavailable, denied, failed, or two
seconds stale.

The first end-to-end run looked like a success and was not: `[DisplayTiming] Screen-change timing service
started` appeared, the session ran for twelve seconds against a 135 fps DX11 target, and the overlay logged
`Frame timing source: presentation (requested=display-change sensorStatus=1)` - status never left `Starting`,
because not one timestamp was ever published. Nothing in the log said which stage broke.

A scratch ETW probe replaying the production chain stage by stage found it. Every event the design needs does
arrive at a non-elevated session (`Present_Start` 641, `QueuePacket_Start` 987, `HSyncDPCMultiPlane` 230,
`MMIOFlipMultiPlaneOverlay` 230 in six seconds), and every property the code reads decodes. The break was the
join: the process emitted 865 runtime presents and 865 present-marked kernel submissions **from two different
threads of the same process**, and the association was keyed on `ThreadId` alone, so it matched zero times.
D3D11/D3D12 submit the present packet from a runtime worker thread, not from the thread that called Present.

Fix: key the outstanding presents by process and let the thread only refine the choice within it -
`SelectDisplaySubmissionPresent` (`captureengine/display_timing_policy.h`) prefers an exact thread match so
several render threads keep their own order, and otherwise takes that process's oldest outstanding present.
Same run afterwards: `runtimePresents=1348 submitAssociations=1347 queued=1338 published=1335 suppressed=0
regressed=0`, and both overlays reached `display-change (sensorStatus=2)`. Vulkan is the same path - NVIDIA's
WSI presents through DXGI - and correlated 1436/1436.

Two things the failure taught, both now permanent: the service logs its stage counters every ten seconds (a
warning while `published==0`, debug otherwise), and `ProcessTrace`'s return status is no longer discarded.
Also measured on this hardware: `VSyncDPC` carries `FlipFenceId=0` and `VSyncDPCMultiPlane` carries
`FlipEntryCount=0`, so completions come from `HSyncDPCMultiPlane`/`MMIOFlipMultiPlaneOverlay`. A display path
offering neither would need the `InterruptTargetPresentId` route; it is deliberately not implemented, because
it can publish a second timestamp for a flip the submit-sequence route already published.

Unvalidated: real-game behaviour, and every path under frame generation - the frame-type provider produced no
events here because nothing was generating frames, so the generated-flip and completion-suppression policy has
unit coverage only.

### 2026-08-29 - FIFO vsync held the rate but CE's overlay ring became the frame pacer

Follow-up run `20260829_153457` on the present-metering fix below. `vulkan_layer.log` confirms the mechanism
exactly as predicted - `frame-generation present metering numFramesPerBatch=4 (swapchain presentMode=2 images=6
chainNodes=1 chainHead=1) - suppressed` - and `perf_metrics_11660.csv` now shows 143-146 presents/s against the
143 Hz refresh instead of 172. Rate: fixed. Pacing: not yet.

The residual judder is CE's own. `fence_wait_us` blocks the game's present thread for **941 ms of every second**:
7.5 ms on the first present of each 4x generated group and 19-26 ms on the last, all of it *before* the present
down-call (`pre_present_us` carries the same numbers, `present_call_us` stays at 40-90 us). `vulkan_layer.log`
reports `All 6 overlay submission slots are in flight` hundreds of times. The rendered frame period then beat
between 2 and 6 vertical blanks in a repeating six-group pattern - a 6-slot ring aliasing against a 4-present
group - and because generated frames carry scene time, an uneven base period is judder even though every vertical
blank received a new image.

Root cause: an overlay submission waits on the present's own wait semaphores and retires only when they are
signalled. A frame-generation runtime signals those from its pacer, one display interval apart, while handing CE
the whole group as a burst - so submissions arrive at burst cadence and retire at display cadence, and a ring
sized at the swapchain image count is exhausted every group. The same signature is already visible in the
uncapped session `20260829_022419` (18% of presents blocked, up to 18.9 ms); FIFO only promoted it to the
dominant pacing authority.

Fix: `ResolveSubmissionSlotCount` sizes the ring at `imageCount + 4` - one full generated group beyond everything
the runtime can already have outstanding, 4 being the DLSS multi-frame-generation maximum rather than a tuned
depth, kept small because each slot also owns a full-resolution offscreen target on the compute-composite path.
`framebuffers`/`imageViews` stay image-indexed; the command/fence/semaphore ring, `timestampWritten` and the
timestamp query pool follow the slot count. Coverage in `tests/test_overlay_submit_queue_policy.cpp`: the sizing
rule including the zero and overflow edges, plus a replay of the failing shape (six outstanding presents, a fresh
4x group) asserting no slot choice asks for backpressure. Real-game verification pending; the confirmation signal
is `fence_wait_us` collapsing to microseconds and the group period settling on 4 vertical blanks.

### 2026-08-29 - Portal RTX `vsync_mode=fifo` behaved like mailbox: frame-generation present metering

`vsync_mode=fifo` prevented tearing but not a frame rate above the 143 Hz refresh. Session `20260829_022419`
settles why. CE forced Remix's Immediate swapchain to FIFO and the driver accepted it, yet
`perf_metrics_29472.csv` climbs to 172 presents/s, delivered as bursts of four inside ~700 us with a ~23 ms gap:
43 FPS base x the configured 4x MFG (`rtx.dlfg.maxInterpolatedFrames = 3`). The DXGI interception recorded the
decisive transition - with frame generation off the driver presented the FIFO swapchain with `SyncInterval=1`, and
on the swapchain generation DLFG created it presented with `SyncInterval=0` plus `DXGI_PRESENT_ALLOW_TEARING`.
Present counts are 1:1 (exactly 1024 `vkQueuePresentKHR` calls between the `final Present1 #1024` and `#2048`
cadence lines), so nothing was being dropped in between; the vertical-blank wait was simply not being applied.

Root cause: `VK_NV_present_metering` is a competing pacing authority for the same presents. Remix chains
`VkSetPresentConfigNV` with `numFramesPerBatch = 4` and asks the driver to spread the batch across one *rendered*
frame interval - an interval derived from the base rate that knows nothing about the display - so the driver drops
the swapchain's FIFO wait. Remix's own strings state both halves: `rtx.dlfg.enablePresentMetering` is "Use hardware
present metering for DLSS 4.0 frame generation instead of CPU pacing", and its V-Sync option says "When Frame
Generation is active, V-Sync is automatically disabled".

Fix: `hook/vulkan_layer/vulkan_present_metering_policy.h` removes the metering request from the present chain when
the profile asked for `fifo`/`adaptive` **and** the tracked swapchain really was created FIFO/FIFO_RELAXED, a single
swapchain is presented, and `numFramesPerBatch >= 2`. FIFO is then the only pacing authority: the group's frames land
on consecutive vertical blanks and backpressure lowers the base rate to refresh / N - no timer, cap, or Reflex
interval. The node is unlinked only when it is the chain head, because a deeper one would mean writing the game's
own `const` chain; that case is logged rather than forced. Hardware metering has a documented fallback (Remix's own
`DxvkDLFGPresenter` CPU pacer), so nothing is left unpaced.

Coverage: `tests/test_vulkan_present_metering_policy.cpp` (16 cases: both discriminators, every decline reason, chain
scan including head/deeper/self-referential). Real-game verification in Portal RTX is still pending; the confirmation
signal is `hook_debug.log` reporting `SyncInterval=1->1 Flags=0x0->0x0` on the final DXGI present once
`vulkan_layer.log` says `frame-generation present metering ... suppressed`.

### 2026-08-29 - Reflex FPS limiter capped Portal RTX at target/multiplier

`general_limiter_mode=reflex` with a 130 fps cap and 3x DLSS MFG limited Portal with RTX Remix to 43 fps;
`basic` was correct at the same settings. Session `20260829_015534` carries the whole proof. `vulkan_layer.log`
shows CE resolving `effective=43, group=130/3, fg=1/3x` and handing the game-owned NvAPI Vulkan context
`driver pacing configured target=43 intervalUs=23256`. `perf_metrics_2436.csv` then shows, starting at the exact
QPC of that push, a rigid three-present burst (~200 us / ~350 us apart) followed by a ~69.3 ms gap - a 69.8 ms
group period, which is 3 x 23.256 ms, with `source_current_fps_x100` pinned at 4299 and `fps_limit_wait_us` at
0-2 us (CE was not the one waiting).

That arithmetic only closes one way: NVIDIA's driver applied the interval to the FINAL presented frame, stretching
the render loop by the MFG factor itself. CE's `ResolveFrameGenerationBaseTarget()` had already divided the cap by
the same factor, so the divisor was applied twice. The fix is
`ce::fps_limiter_policy::ResolveNativeDriverPacingTargetFps()`: driver-owned low-latency intervals get the output
rate, CE's own cadences keep pacing base frames. FSR FG stays on the base target because those generated frames
never reach the NVIDIA cap - `DriverLowLatencyIntervalCoversGeneratedFrames()` is the discriminator, keyed on one
`GetRuntimeMode()` snapshot that also decides `fgActive`. `ConfigureHybridPacing` additionally moved to the scaled
group period, which alone had capped 130/3x at 129. `FPS Limiter: Active (...)` now reports `driver=`.

Regression coverage: pure-policy cases for both discriminators plus three Apply-level fixture tests (DLSS 3x ->
output rate, FSR FG -> base rate, inject capture sync -> base x multiplier) in
`tests/test_fps_limiter_output_groups.cpp`; the stale `VulkanNativeTargetScalesToBaseRateForMfg` expectation was
inverted into `VulkanNativeTargetStaysTheOutputRateForMfg`. Real-game verification in Portal RTX is still pending.

### 2026-08-28 - Vulkan DLSS-G FIFO shared-vtable crash and below-chain body fix

Portal RTX sessions `20260828_014434` and `20260828_022342` both showed CE changing the driver-bound swapchain from
Immediate to FIFO, yet 3x DLSS-G still arrived in three-output bursts and the latter run visibly tore. The temporary
refresh-derived 144 FPS limiter merely paced one 48 Hz group at `vkAcquireNextImageKHR`; it was removed and never
committed. Aggregate 144 FPS was not VSync and made frame pacing worse.

NVIDIA's public `sl.interposer` source established the first missed boundary: its exported `vkCreateSwapchainKHR`
invokes DLSS-G's before hooks with the original create info, then calls the downstream Vulkan dispatch. CE now hooks
that stable export and substitutes guaranteed-supported FIFO before Streamline sees the call; the existing layer still
enforces the downstream call.

Session `20260828_162056` then falsified the assumption that this was sufficient. Its bounded diagnostics proved FIFO
on both sides (`before Streamline DLSS-G hooks` and downstream driver `presentMode=2`), yet generated output remained
about 158.8 FPS with repeated ~18.3 ms / ~0.2 ms / ~0.2 ms bursts. The bridge also resolved the system
`CreateDXGIFactory*` exports late. NVIDIA's Vulkan WSI is implemented over an internal DXGI flip swapchain, so the
remaining real VSync contract is that final swapchain's `Present`/`Present1` call.

The first implementation violated the Vulkan pass-through invariant by writing the shared system factory and
swapchain vtables. Portal session `20260828_212805` falsified it immediately. All four dumps were inspected: the
initiating x64 renderer dump has the `dxvk-dlfg-present` thread executing a null indirect call in
`gameoverlayrenderer64!OverlayHookD3D3`; its stack contains CE `DetourPresent1` and NVIDIA Vulkan WSI. The renderer
termination dump and both x86 `hl2.exe` dumps are post-crash teardown. `hook_debug.log` gives the decisive ordering:
CE wrote Present/Present1 slots at 21:28:17.526, the first final Present1 arrived at 21:28:18.835, and Steam's
uninitialized worker-thread callback failed immediately. CE changed neither argument on that call
(`SyncInterval=1->1`, `Flags=0->0`), so COM lifetime/identity tracking cannot fix it; participating through the shared
vtable was itself the unsafe ownership change. This matches the older `iat_hook.cpp` guard that excludes Streamline
internal factories after the same Steam null-RIP failure mode.

The replacement keeps the real factory and swapchain objects exact and never writes a COM vtable. The intercepted
system factory export is used only to read stable system-DXGI `CreateSwapChain*` method addresses; CE inline-hooks
those function bodies. When the real NVIDIA WSI swapchain is returned, CE installs a deep Present/Present1 body hook
past the widest recognized foreign entry patch. Steam therefore retains its outer entry ownership and CE runs below
that chain before the system DXGI body. The final detours force `SyncInterval=1` and clear
`DXGI_PRESENT_ALLOW_TEARING`; the hot path reads only the armed/Vulkan/shutdown atomics. No driver profile/DRS state,
descriptor, probe object, wait, maximum-frame-latency setting, overlay/capture route, timer limiter, or Reflex cap is
involved. Source-policy regression coverage rejects vtable writes, QueryInterface/Release detours, and pacing
fallbacks. Portal field validation remains required because NVIDIA officially excludes Vulkan from DLSS-G VSync
support.

### 2026-08-27 - FPS limiter: FG-aware output-group admission fixes Portal RTX cap escape (130 configured, ~146 observed)

Session `20260827_211303` (build 0.1.6280, Portal with RTX Remix, `FpsLimiter.general_enabled=true`, cap 130, mode
`basic`, DLSS FG 3x) falsified "the limiter is disabled": it resolved 130, activated in the bridge, and its clock was
exact (~43.0 FPS local cadence) - but the callback stream reached 146.1/s (167 peak), with 92 six-callback / 31 five /
33 four batches beside 531 clean 3-batches and `activeDedup=1458` at 600 paced frames (ideal 2 bypasses/frame = 1200;
the ~258 excess = ~86 admitted extra groups explains the ~17-18 FPS overshoot). Root cause: while FG was active,
`strictGrid = gateEveryPresent && !IsFGActive()` disabled serialized per-presentation admission and the time-based
0.5-2 ms `activeDedup` window decided what "belongs to the current group" - the next real 3-frame group arriving
inside the window was indistinguishable from generated spillover and was admitted unpaced. The submit-thread mismatch
detection route had also switched pacing to `vkAcquireNextImageKHR` silently (no log), so acquire-time pacing was
invisible in the session.

Fix (all in the limiter + Vulkan layer, no game/executable/image-count/timing branches):

- `ce::fps_limiter_policy::OutputGroupAdmission`: deterministic multiplier-ordinal classification (`pace_group` vs
  `pass_generated_slot`) for real final-boundary callbacks; never reads a clock. Generated slots pass through a fast
  path that never touches the cadence mutex; owners block on it. Replaces the FG-active dedup escape; FG-off strict
  grid (Strange Brigade multi-present) and DXVK Present/PresentEx legacy dedup are unchanged.
- Admission epoch key compared BEFORE classification: first callback after any activation/deactivation, target/source
  change, FG on/off, multiplier change, IPC/session reset, or pacing-boundary move owns a clean slot.
- Exact rational group cadence: interval = `QPC_frequency * cadenceScale / configured_target` with Bresenham
  remainder (130/3 = 43.333... groups/s, zero drift); cadenceScale = FG multiplier for final-output observers, 1 for
  inject capture sync. Floored integer target remains only for integer driver APIs and legacy sites; logs now show
  `group=130/3`.
- `hook/vulkan_layer/vulkan_present_boundary.h`: both async-detection routes log edge transitions (submit-thread was
  silent), boundary-identity logs report QueuePresentKHR vs AcquireNextImageKHR with swapchain/FG/grouped, detection
  edges reset output-group admission, 120-frame stats extended with boundary/paced/generated/resets/skips deltas and
  a concurrentSkip invariant-violation tag.
- Tests: new `tests/test_fps_limiter_output_groups.cpp` (pure ordinal sequences incl. the 3x six-callback burst,
  2x/3x/4x windows, reset semantics, exact 130/3 + 100/3 + 141/4 sums, overflow guard, integration bursts, transition
  resets, native post-present arming by owners only, capture-source scale selection, hitch recovery, 4-thread
  concurrent admission with zero contention escapes); `GateEveryPresentDefersToDedupWhileFGActive` (the bug as a
  requirement) replaced by `GateEveryPresentUsesGroupedAdmissionWhileFGActive` in the moved-out unit;
  `PerformanceMetrics` trace test proves ~130 convergence and unclamped telemetry.

Portal RTX matrix validation (2x/3x, non-divisible caps, toggles, VSync variants, Reflex smoke, FG-switch overlay
capture continuity) remains required at runtime. Derived numbers: temp/fpslimitfix-notes.md (not committed).
