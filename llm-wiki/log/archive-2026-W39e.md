# llm-wiki Log Archive 2026-W39e

Covers 2026-09-26 - 2026-09-25. Newest first.

### 2026-09-26 - Present detour stage cost: attributing CE's ~80 us on AMD's presenter thread

- Goal: attribute the ~78-82 us mean (p99 ~140 us) CE owns per Present on AMD's FSR FG presenter thread
  (GTA sessions `20260925_233000` / `20260925_235838`: pacing-trace detour ~350 us minus forward ~270 us).
  Loader-lock lookups and the per-present `DetectSLPresentHook` lookup were already removed; the number did not
  move. No behaviour change in this entry, measurement only.
- `hook/present/present_stage_cost.h`: per-thread lap clock. The outermost `DetourPresent` owns a
  `DetourRecorder`; `EnterStage` advances the linear detour flow (entry, keepalive, context, startup_routing,
  core_policy, post_present) and `StageScope` wraps nested regions (metrics, fsr_topmost, overlay, limiter,
  overlay_wait, forward). Stages are disjoint and sum exactly to the detour; `kForward` is excluded from `own`.
  A detour re-entered inside the forward (SL route) charges its work to CE and hands the clock back to forward.
- Every Present forward on the detour path now goes through `DXGIShared::ForwardPresentThrough`
  (`dxgi_shared_internal.h`) or `CallOriginalPresent`, both open `kForward`; a source test forbids raw
  `presentBypass(pSwapChain` / `dxgi_shared_oPresent*(pSwapChain` / `externalPresent(pSwapChain` calls in
  `dxgi_shared_present{,_core,_routing}.cpp` and `dxgi_shared_steam.cpp`.
- Roles: game (== `DX12_GetGamePresentThreadId`), runtime_presenter (FFX/SL caller, runtime-owned swapchain,
  SL FG running), other. Always on (<1 us/present); drained + logged by `main_hookthread.cpp` only.
- Known coverage gaps (land in `core_policy`/`startup_routing`, not lost): limiter calls on the rare
  startup/third-party/overlayless-handoff early routes, post-present work after the guarded-Steam/SL-bypass
  early returns in core. Present1 is not instrumented. The ECL topmost-overlay recording on the presenter
  thread is separate (`HookExecuteCommandListsCpuCost`, debug log level).
- Hardware run pending: read `[PRESENT STAGE COST] role=runtime_presenter` lines under FSR FG; `own` mean
  should match the pacing-trace detour-minus-forward (~80 us). Stage fields are mean/p95/max us (n=entered).

### 2026-09-25 - GTA follow-up (0.1.6820 run): FG latency count, per-present lookups (0.1.6823)

- Session `20260925_233000` (two GTA runs; run 2 with `dlss_sr_preset=M`, forced vsync at the FFX proxy,
  `cpu_prerender_limit=1`). Previous fixes confirmed: FFX sweeps only at load/evidence (13 in 5 min, 64-85 ms
  each), hook-thread `hooks` stage ~7-9 ms/s under FG (was ~115), no index-buffer re-creates.
- PC latency under FSR FG read 110-155 ms with `appQueue=8` in both runs: FSR FG shows no display for ~500 ms
  after switch-on while GTA presents 25-35 frames, the conservation count saturated and stepped the anchor back
  7 frames all session. `RejectImpossibleQueueCountLocked` (`system_latency_metrics.h`) now latches the count
  unmeasurable past `kMaximumQueueDepth + 1` (one frame in transit) or when displays over-retire by >1;
  `queueCountRejects=` in the chain line. Earlier session read 33 ms only by chance (count drained to 0).
- `DetectSLPresentHook` re-resolved a foreign E9 target outside any module on every Present (~145 module-cache
  misses/s, 15k in 5 min). Memo `hook/present/present_hook_target_memo.h`, valid per module-set generation.
- `StreamlineHook::Init` took a Toolhelp module snapshot (loader lock) every second; the module half now reruns
  only on module-set change, feature resolution (`ResolveStreamlineFeatureHooks`) stays per pass.
- Not CE overhead: run 2's ~5 ms `ProcessFrame innerOther` during the loading screen is the configured CPU
  prerender limit waiting on the GPU. Run 2's even 6946 us flips are the vsync override, with ~6-13 ms
  present-to-display queueing as the price.

### 2026-09-25 - GTA FSR FG "worse lows than RTSS": CE hot-path costs removed (0.1.6819)

- Session `20260925_225006` (0.1.6818). Post-load 12.6 s stall is NVIDIA: inside original NGX
  `CreateFeature(ID=1)`, ComputeCache JIT (cache 994 MB vs 1 GiB CUDA default). Lows: CE's 1%/0.1% low is
  mean-of-worst over 5 s (`performance_metrics.cpp`), percentile on the same data reads ~7 fps higher; flips
  alternate ~4.8/8.2 ms at 146 fps output on a 144 Hz panel while AMD presents evenly. Metric definition
  deliberately unchanged (user decision).
- Fixed CE costs: (1) `ffx_hook_InstallHooksForModule` repeated 3x `PatchIATAllModules` +
  `ffx_cached_pointer_router::Refresh` (lock cmpxchg over GTA's 39 MB `.data`) every second, ~115 ms/s;
  now gated by `hook/ffx/ffx_module_rescan_policy.h` (runtime image, module-set generation, unrouted-call
  evidence from the create breakpoint / configure VEH) and the scan reads plainly. (2) Present/ECL caller
  classification called `GetModuleFileNameA` (loader lock) per call; now
  `overlay_compat_detail/module_address_cache.h`, invalidated by the LdrRegisterDllNotification unload
  callback, disabled without it. `[HookThreadStages]` reports `moduleIdCache(hits misses)`. (3) Overlay
  renderer init: font atlas cached per (font,size,scale); DX12 upload ring is one arena
  (`dx12_overlay_policy/upload_slot_arena.h`), index budget 3:1 (FG graph needed 16.5 KB > old 16 KB).
- Not done / open: CE still owns ~80 us per AMD presenter-thread Present (detour minus forward) plus the
  topmost overlay record per output frame; needs `HookCpuCostMeasurementEnabled` data to attribute. Two
  DX12 backends are still built at FSR FG start (owner-queue renderer + topmost adapter), now cheap.
  Hardware run pending: expect `FFX Hook: import/cached-slot sweep ... reason=` lines only at load/evidence,
  `hooks(avg=...)` in `[HookThreadStages]` back near pre-FSR levels, no `ResizeIndexBuffer` at FG start.

### 2026-09-25 - Video/audio/sync audit: rational CFR grid, cross-pull track fades (0.1.6818)

- Audit of the CFR/audio/mux path found no broken invariant in encoder PTS, hole accounting, finalization,
  ring buffer or Matroska timing, but one long-duration sync defect: the CFR real-time grid strided
  `qpcFrequency / fps` truncated (4-28 ppm fast). Fixed with `common/capture/cfr_rational_grid.h`; see
  `cfr-capture-sync.md` (Exact rational output grid). Hardware run pending: multi-hour 240 fps recording.
- Audio: resume/startup track fades now span pulls; VFR drop crossfade length and backlog-trim seam fixed;
  encoder intake resampler tail drained at flush (`test_audio_encoder.cpp` test fails without it: 16 zero
  frames). New `[STOP AUDIO PLACEMENT]` diagnostic for the open device-clock-drift-at-placement question
  (`multi-audio-capture.md`).
- Left as-is (diagnostic only): process-static log gates in `media_main_encoder_*`; the session header sits
  at the 800-line ceiling.
