# llm-wiki Log

### 2026-11-04 - XeFG status fixed, per-Present layer lookup removed, Present1 reports its result; hardware run pending

- **`entry`:** `ShouldRewriteFinalPresent` takes the metering query as a callable and asks last, so a process without an armed FIFO never looks for the layer.
- **XeFG status:** `SLOffGraceSuppressesFG` exempts XeFG. `CadenceTracker` drops outputs from before its window opens, tells undecided from decided, and an output closes a window
  after a whole one with no application frame (`applicationStopped`); the next frame answers undecided at once. Decided "not generating" makes `ClassifyRuntimeMode` read like generation off, reusing
  a route `SetEnabled(0)` already takes. The input boundary lapses with it, so outputs count as application frames again. A `SetEnabled` edge resets the cadence. Changed verdicts always log.
- **Coverage:** the three flow scenarios failed before their fixes as the run did: type 0 at 30 frames, 4x, 2x in the menu. All 20 XeFG scenarios pass. The fake now generates nothing for a Present without a present ID, on its presenter
  thread, reporting -12. Not tested: real XeFG, and the 1:1 window with present IDs still coming (unit only, the fake stops generating only when IDs stop).
- **Logs:** the three noisy lines log on change. No new tests: `ChangeGate` has its own, and these only choose its key.
- **Present1 result:** only a source-order test, which fails without the fix. Flow cannot reach it: native swapchains are wrapped there, and `IsHookIsolatedFromHost` turns the hold off.
- **Next run should show:** D3D12 `entry` 1-2 us. XeFG on after DLSS or FSR: `FG publication ... runtime=XeFG` holding, no `overlay divergence`, window #1 at 2x. In a menu: `output cadence window ... applicationStopped=1` and
  `FG: XeFG is enabled and generates nothing` within 2 s, overlay off, `may generate again` on return. `FG: XeFG` lines per menu entry, not per frame.
- **Noticed, not changed:** in flow, DLSS FG enabled before the first frame leaves 22 of the first presents unaccounted (`Treating Streamline-originated Present as synthetic re-entrant`, before the overlay is up). Scenarios
  render frames first, so none showed it. No real-game evidence. `present_call_us` is still 0 on XeFG's route.

### 2026-11-04 - 0.1.7413 run: XeFG's wait is gone; `entry` explained; three XeFG status defects found, none fixed yet

- **Session `20261104_183540`, exit 0 both, no device removal:** Talos (DLSS FG, off, DLSS FG, FSR, off, XeFG, off), DOOM Eternal Vulkan. `uncovered=0`
  at all 4 edges, both handoffs `endedWithoutOverlay=0 missing=0`, no `still in-flight`, no lost lines.
- **XeFG fence fix validated (6664ed35):** `Overlay fence completion: deferred Signal after Present (fgActive=1 runtimeOutputChain=1`. `overlay_submit`
  36-57 us mean with generation on (was 2315-6548), p95 49-106; one 13.1 ms max in the first window after the chain arrived. CSV `overlay_us` median 75-108,
  p95 113-193 (was 2378-6588, p95 13221). `deferred_signal` 3.5-17.8. `forward` 5.5-6.1 ms only where XeFG was not generating at 144 outputs/s, so that block
  is XeFG's or DXGI's. Display pacing, one 10 s sample each at base 41-46: XeFG stddev 1053 us, jag 972; DLSS FG 373-427, jag 431-480; FSR 1258, jag 1794.
- **New labels read:** DLSS FG `startup_routing` 1.3-2.5 (was 89-165, as predicted), with `postsl_overlay` 21-25, record 54-69, submit 34-42; `own` 150-180 per output.
  DOOM `overlay` 4.6-5.2 (was 50-65), record 34-47, submit 29-37. `post_process` 55-64 stays the user's sharpen and gamma pass.
- **`entry` 10-25 us (D3D12) vs 0.9-1.2 (x64 DXGI under Vulkan), split ruled out wrapper, API and window (2.5 together). Cause:** `ApplyFinalPresentPolicy`
  evaluates `DeviceEnabledPresentMetering()` as an argument on every Present, and with no resident Vulkan layer that retries two failing `GetModuleHandleW`.
  Under Vulkan the query is cached, which fits the gap. Measured in a warm loop in a small process: 1.8 us net per failed lookup, so about 4 of the ~9 us;
  the rest (a cold thread, more modules) is inferred, not measured. Any D3D11/D3D12 game pays it. `entry` should read 1-2 once only armed and registered presents ask.
- **`overlay_wait` 1.1-2.5 ms is focus loss, not a defect:** only while Talos was in the background (18:47:00-18:48:19), and all 18 `overlay-completion wait SLOW` lines
  fall inside it. `ShouldWaitForOverlayCompletion` waits only when the process lacks the foreground on this route.
- **XeFG status at enable, game toggling it on (18:49:29.737):** XeFG generated from the first frames, but the overlay read Off until 18:49:31.103 (1.36 s), then 4x until 18:49:31.775
  (0.67 s). Off: the inner SL-off grace armed at 18:49:28.386 on the XeFG chain's first ProcessFrame (`grace=300`) although Streamline left 21 s earlier, since the
  FSR route never runs that code. The grace trusts only `IsFSRFGApiActive()`, so `plan(XeFG)` lost to `local(Off)` for exactly 300 frames. 4x: `CadenceTracker` opens its
  window on the first application frame but keeps the output counter; 269 outputs, 185 of them from before (generation off), against 45 application frames.
- **XeFG status in a menu:** from 18:49:45.260 `frameGenResult=-12`, 1 frame queued, `Ray Reconstruction stopped rendering`; status polls ran 139/s against 43/s before, so the
  game ran about 144 fps with nothing generated, yet Pacing health read `x2 base=72`. `ResolveXeFGMultiplier` falls back to the configured factor for a measured "not generating".
  Cadence windows after #5 are not logged, so the application rate there is inferred from the polls.
- **Log noise:** `Protected official FFX startup pending` 409 lines in 2.1 s (12% of the log): its change key holds the list count, which alternates per queue.
  `FG publication preferred override` 118 lines in 1.4 s (key holds a sequence that grows every frame), and `overlay divergence` beside it has no gate.
- **Checked, no change warranted:** XeFG's 16 slots cost nothing measurable (record 51.5 at 16 slots with generation on, 52-66 on the same chain off). `slots=16` writes 29-39 KB
  per frame. Also harmless: `D3D evidence appeared after Vulkan detection` 33 ms before DOOM's process exit; Talos's first Present 121.5 ms (overlay init, once).
- **Real gap, not hit by any run:** `DetourPresent1`'s normal route skips `NoteDX12PresentResultForVtablePath`, which feeds `dx12_hook_g_SwapchainPresentOccluded`, so a Present1 game
  on that route never engages the not-presentable hold. `present_call_us` is 0 in the CSV on XeFG's route, which has no perf row for the forward.

### 2026-11-04 - Release review fixes: spool lifetime/reconnect, visible mip filtering, current ECL entry

- Finalized spools belong to recovery after `Close`: teardown/reuse no longer delete them; cancellation still does.
  Live replay hands its open destination to the writer after the resume seek, removing the failed-reopen state.
  Native byte checks cover teardown, next-open failure, repeated outages, queued writes and trailer patches.
- D3D9's bind shortcut shares visible-mip eligibility with its filtering policy, including `MAXMIPLEVEL`; equality of
  shortcut classes must imply equal desired filtering. The capped transition line includes both mip counts.
- The historical direct-caller ECL gate in the earlier notes below is superseded. The actual queue's current entry
  decides whether an overlay caller can be excluded; a late follower is counted immediately, with no module event.
  Policy tests cover two queues and delayed install/removal. See `dx12-injection-bootstrap.md` and
  `recording-output-paths.md` for current invariants; full task closing gate follows the final fix commit.

### 2026-11-04 - XeFG's fence wait removed, Present1 records its stages; hardware run pending

- **XeFG:** `DecideOverlayFenceCompletion` (`runtime_presented_swapchain.h`) sends a runtime-owned output chain to the deferred Signal the route
  already flushes after the Present, inside its mutex. Slot proof still holds: upload slots are coupled to allocators, and those wait on
  `fenceValues[idx]`, which that Signal now sets. `DX12: Overlay fence completion: <method>` logs each change.
- **Flow coverage:** `FlowXeFG.GeneratedOutputsNeverWaitOnTheOverlayFence` failed with the exemption off and passes with it. The fake XeFG never
  called Present1 before, so the real route was untested; it now does when the game does. All 17 XeFG scenarios pass. Not tested on an Intel GPU
  or real XeFG. The next Talos XeFG run should show `overlay_submit` in tens of us, `overlay_wait` absent and no `Allocator[...] still in-flight`.
- **Stage line:** `DetourPresent1` records like `DetourPresent`, every forward through `kForward`. New stages: `postsl_overlay` plus its record
  and submit, the compute route's record and submit, `entry_wrapper`, `entry_api`, `entry_window`. Under a wrapper, Present1's detour now bills
  CE instead of `forward`, so such a game's `own` can rise with no real change. Flow's clock is virtual: stage durations mean nothing there.
- **Noticed, not changed:** XeFG keeps rotating through 16 slots because `ShouldReuseFinishedOverlaySlot` is off under FG, though every output
  now has a Signal. `DetourPresent1`'s normal route never calls `NoteDX12PresentResultForVtablePath`, unlike `ExecutePresentCore`; effect unknown.

### 2026-11-04 - 0.1.7411 run: the cuts hold; XeFG's presenter is blocked on the overlay fence

- **Session `20261104_144527`, all exit 0, no device removal:** Mirror's Edge and 32-bit `dx12_test` with Alt+Tab (18 focus edges together), Strange
  Brigade Vulkan, Talos (DLSS FG, FSR, off, FSR, off, DLSS FG, XeFG, FSR, off), DOOM Eternal Vulkan, BL2, Gothic II. No `still in flight`,
  no `skipped quad`, `uncovered=0`, all three swapchain handoffs `endedWithoutOverlay=0 missing=0`.
- **Defect, XeFG only:** `DX12: Overlay perf: getBuf+record=41-68us submit=2315-2359us` with the game running, `submit=6496-6548us` with
  XeFG alone at 144 outputs/s; CSV `overlay_us` median 2378 and 6588, p95 13221, alternating ~2.4 and 11-13 ms by output. The route
  documents "no overlay fence wait", but `SubmitOverlayDraw` takes `anyFGForFence && !useDedicated` (`IsFGActive()` is true for XeFG): Signal,
  then a CPU wait on the completion fence, and `CreateEventW` per frame. Pacing effect not measurable from this run. The stage report
  missed it because XeFG presents with Present1 and `DetourPresent1` outside a wrapper has no recorder.
- **Cuts confirmed, us mean:** `origin_walk` n=5-7 per 10 s without a runtime, 4-8 per Present with Streamline. `context` 13-22 -> 2.5
  (Mirror's Edge), 1.9-2.0 (x64 DXGI under Vulkan). Wrapper route `startup_routing` 2.3-3.1, preamble ~42 before. `slots=1-2`, Mirror's Edge
  writes 38-41 KB per frame, was 385-451. Mirror's Edge total without the limiter 311: entry 15.5, overlay 23.8, marker 6.4, post_process
  72.6, reset 7.2, record 61.8, submit 35, deferred_signal 73.4.
- **Vulkan marker is cheap:** `prerender_marker` 4-8 (p95 9-16, max 213) on Strange Brigade, while `prerender_wait` goes 24 -> 1628 when
  GPU-bound. The earlier ~200 us guess was not the marker, so rejecting the fence reuse stands. Record 27-31, submit 19-22.
- **Read these correctly:** DLSS FG `startup_routing` 89-165 is the PostSL overlay draw (`InvokePostSLCallbackForFinalOutputPresent` runs inside
  `ExecuteStartupRouting`), not routing. DOOM's `overlay` 50-65 is the compute route when it presents on family 2 (layer summary: graphics
  record 36, submit 21, compute submit 13), which has no stages. `dx12_test` `overlay_submit` 3684-5560 is the focus-loss same-frame wait
  while unfocused. `post_process` 60-80 is the user's sharpen and gamma pass.
- **`entry` unexplained:** 9-16 on D3D12, 1.6 on x64 DXGI under Vulkan. `ShouldBypassDX12InvisibleWindowPresent` (`GetDesc`, `IsWindowVisible`) is
  the D3D12-only code there, but `GetDesc` elsewhere costs under 1, so it is a suspect only. Split the stage before cutting.
- **ECL gate behaved:** Mirror's Edge ignored 2 lists per frame for about 6 s from second 3 of play, then none, which fits Steam's notification;
  Talos under 256. Game lists stayed counted (`cmdLists=2-5`).
- **BL2 first-frame hitch named:** 261 ms once, `DX9: Pre-initializing capture on first Present` (D3D11 device 154 ms, then the D3D9Ex helper).
  `overlay_us` 28-29. Still no RTSS run.

### 2026-11-04 - Preamble, slot and Vulkan cuts after the 0.1.7409 re-run; next run pending

- **The preamble's main cost was a stack walk I had said was not there:** every 20261104 session ran below a foreign chain, so
  `ResolvePresentOriginatorBelowForeignChain` walked 16 frames per Present, with a loader lookup and path scans per frame. The
  2026-11-03 note "no stack walk on the common path" was wrong; the caller cache only covered the immediate caller. Now a
  `RuntimeModuleWatch` (loader notification plus one scan) says when only the application can originate, and the thread keeps its
  walk until the module set changes, re-walking every 256 presents for log names. With a runtime loaded it still walks per Present.
- **Also gone per Present:** 2 `VirtualQuery` (memo of proved object and table), the second wrapper QI and API tag read in the Steam
  bypass check, the `IDXGISwapChain2` QI of an applied latency override. `present_result` was 0.6 us with `GetDesc` and `IsIconic`
  in it, so those COM calls were not the cost and stayed.
- **Wrapper route:** with no runtime and no third-party overlay, a Present a CE wrapper processed leaves the detour before
  `CapturePresentCallContext` (`WrapperOwnedPresentNeedsNoRouting`, `ExecuteCommonPresentRouting`). The wrappers now own a
  `DetourRecorder`, so that route's overlay is in the stage report; before, `dx12_test` read own=42, overlay n=0.
- **D3D12 slots:** `OverlaySlotPicker` takes the lowest finished slot on the plain route. Finished means a value assigned after the
  slot's last submission completed; rotation had been hiding routes with no Signal behind 16 frames of distance, so those slots stay
  out and such routes end in plain rotation. Upload line gained `slots=`. Vulkan keeps its 32-slot ring: a text change there costs
  about 10 KB per frame, and its submission slots need an acquire-generation proof anyway.
- **Rejected, Vulkan prerender marker on the overlay's fence:** a second thread resetting that fence while the prerender waits on
  it could hang with an infinite timeout, and the ~200 us only appears when the GPU is the bottleneck, where it costs no fps. The
  new `prerender_wait` / `prerender_marker` stages measure it instead. Also not done: moving Signal, see the entry below.
- **ECL caller:** `IsCurrentECLCallerFromThirdPartyOverlay` read its own return address and is out of line in the PDB
  (`S_LPROCREF`), so nothing ever classified as an overlay. Fixed with `EclCallerGate`: an overlay caller is the overlay's own only
  after a direct caller since the module set changed, because an overlay above CE is the immediate caller of every game submission.
- **What to read next run:** `origin_walk` n near presents/256 without a runtime; `context`, `startup_routing` lower; `slots=` 2-3 and
  Mirror's Edge writing about 100 KB; `prerender_marker`; `post_process` on Mirror's Edge; the `[PRESENT STAGE COST]` line in
  `vulkan_layer.log`; `dx12_test` showing overlay stages. Any `[Overlay] DX12 slot ... still in flight` or `Allocator[...] still in-flight`
  would point at slot reuse. FG switching is what these touch most: flow tests pass, games not run yet.

### 2026-11-04 - 0.1.7409 re-run: no regressions, what the stage split really shows

- **Sessions, all exit 0, no device removal:** Strange Brigade Vulkan `20261104_131318`, DX12 `132859`; 32-bit `dx12_test`
  with Alt+Tab spam `133008` (13 focus edges, 4043 frames); Mirror's Edge `133147`; BL2 `133240`; BioShock Infinite `133441`.
- **Mirror's Edge before/after (`20261103_213712`):** `skipped quad` 461 lines -> 0; vertices 61.5-64.3k -> 48.8-51k; graph-only
  70-75 -> 34.5 us; CSV `overlay_us` median 144 -> 90; CE per present without the limiter 429 -> 361 us (~339 without two
  startup frames in that window). The frame stayed under
  65,536, so the base-vertex chunk path has still not run on a GPU (`dx12_test` 46,880 vertices, `commands=1`).
- **BL2 before/after (`20261103_194318`):** `overlay_us` median 38 -> 26, DX9 upload 6.8 -> 4.7, draw 9.1 -> 5.8, `tail-only` 98%
  of uploads. No RTSS run in this batch. `High Present Overhead` ~260-270 ms on the first overlay frame (`capture_us` 259 ms)
  is one frame and was already in the older sessions.
- **Strange Brigade DX12, us mean:** own 311 = entry 10, context 22, routing 9, policy+metrics 6, overlay 45, reset 15, record 53,
  submit 46, deferred_signal 100. About 45 of it is two early frames (max 25, 14.5, 10 ms). No HotS run, so no x64 before/after.
- **`deferred_signal` 100 us is not what Signal costs.** The CSV splits the DX12 run in two. Phase 1, ~144 fps: Present blocks
  6.2 ms, `post_present_us` 104, `overlay_us` 71. Phase 2, ~124 fps: the wait moves to `prerender_wait_us` 6.1 ms, `present_call_us`
  ~230, `post_present_us` 5, `overlay_us` 171. The prerender path's own Signal plus bookkeeping is ~7 us
  (`prerender_wait_us` median while it does not wait). So Signal costs about 5-7 us. The ~100 us follows the long block
  or the load (cold caches or a busy driver; these runs cannot separate them), and removing Signal would probably not save it.
- **`overlay` is not only routing:** ~22 us on Strange Brigade, ~103 on Mirror's Edge, which runs `rcas_gamma` per frame (its own
  list, ECL and Signal, `Sharpen: DX12 running`). HotS logs the same pass. A user-enabled effect, not overlay overhead.
- **Upload saving is smaller than `upload_slot_geometry.h` says:** written per frame of the whole frame: Strange Brigade DX12
  25-27 of 59-65 KB, Vulkan 33-40 of 60-65 KB, Mirror's Edge 385-451 of ~1305 KB. Every slot rewrites the head once after each text change, and
  full frames come every 16-50 frames with 16 (D3D12) or 32 (Vulkan) slots in rotation.
- **Vulkan:** overlay `overlay_us` median 54, GPU 11. `pre_present - prerender - overlay` jumps from ~21 to ~200 us when the game
  drops to ~123 fps. Probably the prerender marker's `vkResetFences` + empty `vkQueueSubmit`, which sit outside `waitUs`
  (`ApplyPrerenderLimitVulkan`); not measured, and the layer has no stage split.
- **Preamble:** entry + context + startup_routing is ~42 us on every DXGI Present (x64 41.7, x86 D3D12 46.8, D3D11 46). `context`
  fell only 25.7 -> 22.3 (different games), so the caller cache was not the main cost there. `log_level=trace` adds the
  `QueryThreadCycleTime` pairs. On the wrapper route (`dx12_test`, no third-party overlay) the inner detour pays ~42 us just to
  early-return, and the stage report does not see the wrapper's overlay work (`overlay` n=0).
- **Background 32-bit D3D12:** `overlay_us` ~7 ms in `dx12_test` while unfocused is the same-frame fence wait of the focus guard,
  by design; ~140 fps held and the foreground median stayed 149 us.
- **Open, not started:** reuse finished upload slots instead of rotating all of them; a finer preamble split; Vulkan stage split and
  a prerender marker that reuses the overlay's submit. In these runs all but the wrapper route had 5-6 ms of idle wait per frame, so no fps was at stake.

### 2026-11-03 - D3D12 and Vulkan per-frame cost: evidence from 0.1.7407, cuts committed, re-run pending

- **32-bit D3D12 hit the 16-bit vertex limit (Mirror's Edge, `20261103_213712`, DPI 1.5):** solid-span text (the
  `handoff-dx12-32bit-crash.md` workaround) made 61,520-64,292 vertices and 92,280-96,456 indices per frame, against
  ~1,600 vertices on D3D9. The frame then overflowed: `skipped quad ... (verts=65532)`, 461 rate-limited lines (first 4,
  then every 200th), so about 91,000 dropped quads in 22 s. `DrawGraphPolyline` returned early when its sections no
  longer fit, so the graph line went first (from the code, not seen on screen). Cost: full frame 439-592 us, graph-only
  70-75 us, about 1.5 MB copied per frame.
- **64-bit D3D12 stage cost (HotS, `20261103_215427`, recording, capped ~140 fps), us mean per present:** entry 12.8,
  context 25.7, startup_routing 18.7, core_policy 10.2, metrics 2.7, overlay stage 243.1, overlay_wait 0.8, post_present
  62.5, so ~376 against ~52 in BL2 on D3D9 without recording. The CSV's `total_us` was ~236: `overlay_us` ~74, `capture_us`
  ~54, ~108 attributed to nothing, because `cmdlist_reset_us`, `render_us` and `execute_us` are all zero on D3D12.
  `DX12: Overlay perf`: getBuf+record 35-44, submit 27-40.
- **Found by reading, then cut:** `PrepareFrame` ran `GetCurrentFPS`, `Get1PercentLowFPS` and `Get01PercentLowFPS` every
  frame even without a CSV (each low walks ~700 cold samples through a heap at 140 fps); D3D11 and Vulkan rows did too. Now
  every 32 rows. `CapturePresentCallContext` and every game ECL without FG scanned the caller's path for a few dozen tokens
  (`IsThirdPartyOverlayModulePath`, `ClassifyOriginatorModule`, the FFX and Streamline checks). Now a per-thread
  `CallerVerdictCache`. `IsWrappedSwapChainObject` ran its QI twice per present. Not cost: `DX12_SetCommandQueueInternal`
  leaves early for the current queue, so a high `registrations=` is harmless; no stack walk runs on the common path.
- **Also fixed:** `~PerfLogger` blocked on its lock at process exit. ExitProcess has killed the other threads by then, and the
  hook thread writing rows (39ad5cfa) made dying with the lock held more likely, which would hang the exit.
- **What to read next run (HotS, Mirror's Edge, a Vulkan game):** `[PRESENT STAGE COST]` has new `overlay_reset`,
  `overlay_record`, `overlay_submit`, `capture`, `deferred_signal`, `present_result`, `perf_row`; `overlay` is now what remains:
  routing and policy. Expect `context` well under 25 us and `overlay`+`overlay_*` under the old 243 us. `[Overlay] DX12 upload`
  and `Vulkan upload` give bytes written per frame. Mirror's Edge: no `skipped quad`, about 50,000 vertices.
- **Open:** after those numbers, cut whatever `overlay` and `deferred_signal` turn out to be. The 2.11 ms/base frame under FSR FG
  (`overlay-rendering.md`, 2026-09-03) has no named cause and has not been measured since the 2026-10-09 queue change.
  `IsCurrentECLCallerFromThirdPartyOverlay` reads `CE_RETURN_ADDRESS()` in its own frame, so unless LTO inlines it, it classifies
  the ECL detour's address in CE's own DLL rather than the game's caller; `20261103_213712` (Steam loaded) logged no
  `Ignoring command-list count`. Behaviour left as it was. Retesting font sampling on 32-bit D3D12 is the user's call: a failure is a
  GPU hang.
