# llm-wiki Log Archive

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
