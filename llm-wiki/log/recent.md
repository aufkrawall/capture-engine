# llm-wiki Log

### 2026-10-19 - Review fixes: encoder=auto binding and context leak at first-frame Init; inject slot content time

- `encoder=auto` (e16764e6): the first-frame `Init(savedConfig)` (screen grab every recording; inject while
  `initDone` is false) saw the already concrete name and cleared `autoEncoder.request`, so `ResolveAutoEncoder`
  returned early from then on (no real-open retry, no late GPU decision). `ReinitWithSavedConfig` now keeps the
  binding; `EncoderAutoProbeTest.FirstFrameReinitialization*` failed before the fix (request `""`).
- Face camera (b4b3abf9) and cursor (since dd7bb5db, 2026-07-14) on inject: selected at `scheduled` while
  `SelectInjectFrame` targets `scheduled - avContentDelayQpc`; with an audio capture latency both led the game
  frame by it. One `ComputeSlotContentQpc` now feeds both. No hardware run yet (webcam clap test, inject with
  app-audio latency).
- Context leak, fixed: the first-frame re-Init allocated new `fmtCtx`/`codecCtx` over the unopened previous ones
  (no `Stop()` in between). `Init` now frees them first and refuses `initDone`/`fileOpened`; the deferred writer
  thread is already running there, so it cannot be part of the refusal. `VideoEncoderReinitTest` counts the
  release through FFmpeg buffer callbacks (0 before the fix, 1 after, still 1 after the destructor).

### 2026-10-19 - DOOM Eternal 4K -> 1440p "semi-freeze until minimized": game/driver mode change, not CE (no change)

- `20261019_132425` (0.1.7261, `vsync_mode=fifo backbuffer_count=2 cpu_prerender_limit=1`, recording r0002 live).
  DOOM creates every swapchain with `VK_EXT_full_screen_exclusive` (pNext 1000255000 -> 1000255001); CE passes the
  chain through untouched. Its only change to these swapchains is the configured `Overriding present mode 0 -> 2`.
- 13:27:13-13:27:24: ~25 recreations alternating 2560x1440 (`blockers=not-covering-monitor`, window 2560x1440,
  monitor still 0,0-3840,2160) and 3840x2160 (window back at 4K); overlay frame-timing source flips
  `composed=1`/`composed=0` with them (the "DXGI phases": a 1440p window on a 4K desktop is composed). Driver
  create of the 1440p chain 300 ms, of the 4K chain 5 ms; driver `present_call_us` 60-150 ms per frame, ~0.9 s
  frame gaps. After minimize (13:27:29, presentation-path log shows `monitor=0,0-2560,1440`) one stable 1440p
  swapchain at 13:27:37, `composed=0`: the mode change completed only then.
- CE in the loop: prerender wait 2-15 us max, queue-depth pacing idle (`images=2` carries the depth), destroy
  ~22 ms (device-idle wait) + overlay/capture init 0-55 ms per recreation, no window/display-mode calls, CE
  desktop overlay hidden since 13:27:07 (`shouldHaveVisible=0`). No `exact DXGI swapchain-create pass-through`
  and no Vulkan DXGI FIFO registration lines: CE saw no ICD DXGI swapchain this session.
- CE cost found: the capture transport for the 4K chain at 13:27:51 was built on present thread T0CEC in 370 ms
  (~150 ms reclaiming the 1440p texture set, ~220 ms creating/importing 16 4K textures), i.e. one ~370 ms hitch
  per resolution change while capture is prepared. Open, low priority: build it off the present thread.
- Still unproven: whether CE's forced FIFO (game asked IMMEDIATE) changes NVIDIA's exclusive-mode behaviour; an
  A/B with `vsync_mode=default` settles it.
### 2026-10-19 - Vulkan queue-depth window across swapchains VALIDATED (20261019_132152)

- 0.1.7261, Strange Brigade Vulkan, `vsync_mode=fifo backbuffer_count=2 cpu_prerender_limit=1`: three 10 s windows
  printed (`generations=3/2/5`, 0 timeouts) where 0.1.7259 printed none; prerender summaries every ~10 s too
  (`route=present`, markers 1205-1321 per window). A window with `waitMeanUs=0` coincided with a run of swapchain
  changes at a steady 144 fps (pre-present ~60 us): present k-1 was always displayed already, not a defect.

### 2026-10-19 - Strange Brigade Vulkan minimize with present-wait + prerender pacing (no CE defect)

- `20261019_124700`: minimize -> `vkQueuePresentKHR`/`vkAcquireNextImageKHR` `VK_ERROR_OUT_OF_DATE_KHR`, then no
  presents at all for 10.1 s and 37.5 s (perf CSV gaps) until restore recreated the swapchain. `_125351`: focus
  loss without a present gap (130-144 fps, max pre-present 19.5 ms). Both: every generation logged `queue-depth
  pacing ... pacing`, prerender summaries kept `route=present`, zero `missed the ... ceiling`/`failed` lines.
- So the present-wait 1 s ceiling path stays unexercised: this game never presents while minimized. The 10 s
  queue-depth window stats never printed (no swapchain lived 10 s), the known per-swapchain reset gap.
- Closes the open item in `archive-2026-W42n.md` (prerender summary ~2 lines/s at ~2000 fps): the summary is now a
  10 s per-queue window (`IsSummaryWindowDue`), see `graphics-overrides-and-frame-pacing.md`.
- Follow-up same day: the queue-depth window now survives swapchain recreation (`CarryWindow`, hand-over in
  `UnregisterSwapchain`, adoption by HWND + device at create, `generations=` in the line). Not changed, by
  decision: a Vulkan renderer presenting through its own DXGI swapchain (no `VkSwapchainKHR`). Covering it means
  moving the DXGI path's Vulkan ownership from per-process (`IsVulkanActive`) to per-swapchain, overlay included,
  with no title to validate against and the DOOM ICD-transport deadlock as the known failure of that boundary.
  The case is already identifiable: DXGI `exact DXGI swapchain-create pass-through` lines with no
  `vkCreateSwapchainKHR` in `vulkan_layer.log`.

### 2026-10-19 - Vulkan present-wait queue-depth pacing VALIDATED (20261019_025229)

- Strange Brigade Vulkan 4K, backbuffer_count=2, 0.1.7259: pacing on every swapchain generation (4K/1440p/4K),
  `waits=1261/1262 waitMeanUs=6283 timeouts=0`, fps unchanged (141-142), prerender wait 4.3-5.6 ms -> 2 us,
  anchorToPresent median 13.2 -> ~7.3 ms vs `20261019_022104`. Numbers and caveats on the topic page. Not covered:
  minimize/occlusion (ceiling path); window stats are lost when a swapchain retires inside its first 10 s.

### 2026-10-19 - Vulkan backbuffer_count paces the present queue with VK_KHR_present_wait (unit-tested, hardware pending)

- Follow-up to `20261019_022104` (Strange Brigade Vulkan: `minImageCount override skipped configured=2 game=3`, so
  the setting was inert). The layer now keeps the game's image count and waits for present id `k - (N - 1)` before
  presenting id `k`; 1 s ceiling, then non-blocking until a present completes. Design, ownership rules and log
  lines: `graphics-overrides-and-frame-pacing.md` ("Queue depth without taking images away").
- Rejected: lowering `minImageCount` again (the DOOM Eternal `VK_NOT_READY` fatal error, same page) and pacing with
  CE fences on acquire (measures GPU completion, not display, and adds a queue submit per frame).
- Validation still to do: `vulkan_layer.log` on Strange Brigade Vulkan with `backbuffer_count=2` should show
  `queue-depth pacing on this device: enabled`, `ready`, `for swapchain ...: pacing`, then 10 s windows with
  `waits` near `presents`. Minimize/restore and resize should show at most one ceiling miss and a `resumed` line.

### 2026-10-19 - backbuffer_count was withheld in every x64 DXGI game with an overlay (fixed, VALIDATED 20261019_021805)

- Strange Brigade DX12 + Steam (`20261019_014848`): `SetMaximumFrameLatency(1) failed hr=0x887A0001` and "requested by
  the application" skips were a waitable-less chain (Flags=0x802) - diagnostics fixed in b35b8d51; cpu_prerender_limit
  was enforced by the D3D12 fence limiter on all 13,896 frames (prerender_wait p50 6.9 ms).
- Root cause of the missing queue-depth control: `DeepHook: Prolog ... is not a recognized stack shape` for dxgi
  ResizeBuffers/ResizeBuffers1 (`mov rax,rsp` + home-space spills, dxgi 10.0.26100.9711). A survey of 23 session logs:
  every x64 session with an overlay `UNAVAILABLE site=none`, no-overlay x64 `ready` via entry, x86 with overlay
  `ready` (own analyzer). Plus the Witcher 3 ReShade-proxy base-name bug. Details:
  `dx12-overlay-third-party-coexistence.md` (resize bullet). D3D8/9/DDraw rewrite present parameters and Vulkan sets
  the image count in CE's layer - none depend on resize reconciliation.
- `20261019_022104` (Steam loaded, backbuffer_count=2): BioShock Infinite x86 DX11 is a blit chain (flags=0x2),
  `Overriding BufferCount 1 -> 2` at create and both resizes, x86 reconciliation `ready` after the usual
  thread-race retries, D3D11 fence prerender limit 1 active, limiter 140.0 fps 0 late; the skip log wrongly said
  "cannot pace" (fixed in bf377ca6). Strange Brigade Vulkan: `minImageCount override skipped configured=2 game=3` -
  by design (vulkan_swapchain_image_policy.h never takes away declared acquire headroom); see the entry above for
  the present-wait pacing that now carries the setting there.

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
