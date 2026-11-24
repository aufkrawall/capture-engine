# llm-wiki Log

### 2026-11-24 - FFmpeg 9.1 stable pin and release-gate robustness

- Upstream released FFmpeg 9.1 as annotated tag `n9.1` (`9d106a4c...`), peeling to commit
  `74e4393595cf2b5857f0da925c4e0dd74ea306e5` with tree `eed36c7cb77a542a4dcbda4805fafeec0ade05e7`.
  Both `git.ffmpeg.org` and `github.com/FFmpeg/FFmpeg` report the same tag object and peeled commit.
  The release source contains `AAC_CODER_NMR` and the `aac_nmr_speed` encoder option.
- Replaced the pre-release `fb306509...` pin with `n9.1`. All seven CE FFmpeg patches apply in order, the
  clean source build records commit `74e43935`, the bundled `avcodec-ce-63.dll` reports ProductVersion
  `n9.1` (file version `63.24.100`), and the full native/Python suite passes. Focused evidence:
  `AudioEncoderContractTest`, `AacNmrRuntimeTest`, and `AacNmrBudgetTest` (15/15).
- Release run `45309314055` failed at the final ratchet because three GPU-load test argument comments
  named the wrong parameters; its fatal lint exit also raced concurrent packaging and produced an
  interpreter-shutdown error. Commit `8be40e7b` fixed the labels, refreshed the full clang-tidy scope,
  and drains packaging before re-raising the lint exit. Run `45316494885` was cancelled when the
  FFmpeg-stable requirement landed; neither failed/cancelled run created a tag or GitHub release.

### 2026-11-22 - AMD AMF option compatibility fixes, codec-open trace capture, template curation

- Audited `mediaengine/video/video_encoder_backend_options.cpp` against bundled FFmpeg `amfenc_{h264,hevc,av1}.c` and
  AMF 1.5.2 docs/samples (`AMF_Video_Encode*_API.md`, `AMF_Video_PreAnalysis_API.md`, `DisplayDvrPipeline.cpp`):
  - **Planner fixes (`bb3cec2a`):** made `[AMF] preanalysis` tri-state (`auto|disabled|enabled`, default `auto`) so
    `CQ`/`QVBR`/`HQVBR`/`HQCBR` enable PreAnalysis automatically instead of forcing `preanalysis=0`; prevented
    simultaneous encoder-side (`vbaq`/`aq_mode`, `high_motion_quality_boost_enable`) and PA AQ/boost flags; always
    emitted `pa_lookahead_buffer_depth` (raising `0 -> 1` for TAQ/PA boost, using DX11 gaming `pa_taq_mode=2`, and
    raising `async_depth > lookahead`) so AMF's 11-frame default cannot deadlock a smaller `async_depth`; guarded CQP
    (incl. `av1_amf` `aq_mode`), non-CBR `filler_data`, low-latency `preencode`, 10-bit P010 PreAnalysis (fail closed
    on PA-dependent RCs, clean degradation for optional PA assists), and `1..51` QVBR / `1..255` AV1 CQP bounds.
  - **Open-failure trace diagnostics (`9aecb93c`):** `hwcontext_amf.c` logs `AMF_TRACE_WARNING`/`ERROR` through
    `av_log(..., AV_LOG_DEBUG, ...)`, which `CaptureEngineAvLogCallback` previously dropped at `AV_LOG_INFO`. Added
    `ScopedAvOpenDebugCapture` (`mediaengine/engine/av_log_open_capture.{h,cpp}`) to retain a bounded, redacted tail
    during `avcodec_open2` and flush it only on failure (zero noise on success; tail line also in `encoder_auto_probe`).
  - **Template & hardware probe:** commented out advanced/streaming-only `[AMF]` keys in `config.ini.template` while
    keeping full parser compatibility, and added `tests/test_amf_matrix_probe.cpp` (always-run FFmpeg `av_opt_set`
    matrix check + opt-in `DISABLED_PrintHardwareCompatibilityMatrix` for real Radeon hardware). Live AMD hardware
    validation remains pending (no Radeon GPU on dev host).

### 2026-11-21 - ReLimiter 4.0 / ReShade add-on coexistence (5 root causes fixed)

- User report: with Capture Engine injected alongside ReLimiter 4.0 (`relimiter.addon64`), ReLimiter disabled frame
  pacing due to "unknown FG source" and its performance overlay stopped working. Source audit of ReLimiter v4.0.0
  against CE identified five generic hook-coexistence bugs (fixed across 4 commits: `070ae4c8`, `2371b142`,
  `e987ab5`, `922667a0`):
  1. **DACL-protected peer threads** (`070ae4c8`): ReLimiter's permanent hook-installer thread denies
     `THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT` (`0x1A`) to `WinWorldSid`, causing CE's
     Toolhelp thread walk (`WalkCurrentProcessThreadsViaSystemSnapshot`) to abort with `ERROR_ACCESS_DENIED` ->
     `QuiesceFailure::kEnumeration`. Both `NtGetNextThread` and Toolhelp walks now fall back to
     query-only handles (`ThreadAccessVisitor`) and `ThreadQuiescence` skips DACL-protected peers (kept open across
     passes so snapshots stay stable, logged post-resume) while still failing closed on normal `SuspendThread` errors.
     Verified with a real-kernel DACL test on both `NtGetNextThread` and forced Toolhelp paths (`test_thread_quiescence.cpp`).
  2. **ReShade add-on IAT/GetProcAddress & NVAPI flip-metering exclusion** (`2371b142`): `.addon64`/`.addon32`/`.addon`
     modules were missing from `IsThirdPartyOverlayModulePath` (`module_table.h`), so CE patched `relimiter.addon64`'s
     IAT (`GetProcAddress` -> `DetourGetProcAddress`) and `flip_metering_bridge::MaybeAnswerQueryInterface` handed it
     `Observed_SetFlipConfig` instead of `nvapi64.dll`'s real `NvAPI_D3D12_SetFlipConfig` (breaking `FlipMeteringFlowing`).
     ReShade add-ons are now excluded from IAT/GetProcAddress interception (without inflating tracked-overlay counts),
     and third-party overlay/add-on callers receive native `NvAPI_D3D12_SetFlipConfig` unwrapped.
  3. **Streamline feature-function wrapper substitution** (`e987ab5`): `Hooked_slGetFeatureFunction` unconditionally
     replaced `*outPtr` for `slDLSSGSetOptions`/`slDLSSGGetState` even when CE's direct inline hook on `sl.dlss_g.dll`
     succeeded — breaking ReLimiter's `StreamlineQueue_SetFeatureModule` range discovery (`capture_hook_x64.dll` instead
     of `sl.dlss_g.dll`) and letting games bypass ReLimiter's MinHook entry hooks. Wrapper substitution now runs only
     when the inline hook is unavailable, and never for third-party overlay/add-on callers.
  4. **Native D3D12 command queue vtable patching** (`922667a0`): CE patched `ID3D12CommandQueue` vtable slots `[10]`
     (`ExecuteCommandLists`) and `[14]` (`Signal`) in place, breaking ReLimiter's `RuntimeMethods`/`MatchesQueue`/
     `IsD3D12Method` checks that require those slots to stay inside `D3D12Core.dll`/`d3d12.dll`. Native D3D12 queue
     methods are now intercepted via `InlineHook::InstallPublished` (leaving vtable slots untouched in `D3D12Core.dll`),
     separating native identity from callable inline trampolines; non-native/proxy queues keep `VTableHook::Create`.
     Verified with unit tests (`Dx12QueueDispatchPolicyTest`) and headless WARP flow test
     `FlowQueueDispatch.NativeD3D12QueueKeepsVTableSlotsInD3D12Core`. Live in-game ReLimiter 4.0 hardware retest pending.

### 2026-11-20 - Reliable overlay GPU load: NVML PMU + D3DKMT node statistics + stabilizer (implemented)

- `MirrorsEdge.exe` (x86 native DX9, `megpuload`, PID 28952, `0.1.7535`, RTX 5070 with HAGS `HwSchMode=2`): while
  rendering steadily at 948–1008 FPS (`perf_metrics_28952.csv`, `hook_debug.log`), overlay GPU load jumped
  `26% -> 0% -> 0% -> 0% -> 100%` because `host_metrics.cpp` published raw, unfiltered 1 s PDH
  `\GPU Engine(*)\Utilization Percentage` polls. WDDM/HAGS per-process `RunningTime` both drops to 0%/100% bursts
  on high-FPS DX9 and inflates toward ~100% under FIFO V-Sync when queued frames wait on scanout fences (whereas a
  CPU FPS limiter drains the queue).
- Live RTX 5070 probes (`nvml.dll` + `D3DKMTQueryAdapterInfo`/`D3DKMTQueryStatistics`):
  - `D3DKMTQueryAdapterInfo(KMTQAITYPE_ADAPTERADDRESS=6)` matches `nvmlDeviceGetPciInfo_v3` (`bus=6 dev=0 fn=0`;
    software WARP returns `0xFFFFFFFF`), enabling exact multi-GPU LUID-to-NVML matching.
  - `nvmlDeviceGetSamples(NVML_GPU_UTILIZATION_SAMPLES=1)` provides 200 ms hardware PMU GR-engine samples
    (excluding NVENC/NVDEC), reading steady `22–23%`, `62–63%`, `93–94%` on DX9 and `3–8%` on light/moderate FIFO
    V-Sync with ~200–400 ms step response.
  - `KMTQAITYPE_NODEMETADATA=25` uses `#pragma pack(1)` (`sizeof == 78`) and classifies all 14 RTX 5070 nodes
    (3D, Copy, VideoEncode, VideoDecode, OFA, JPEG), allowing direct adapter-wide `D3DKMTQueryStatistics(NODE)`
    running-time deltas for non-NVIDIA GPUs without PDH wildcard/per-process churn.
- Implemented `captureengine/app/gpu_load_policy.h` (`IntegrateNvmlGpuSamples`, `IsD3dkmtVideoEngineNode`,
  `ComputeD3dkmtAdapterLoadPercent`, `EvaluatePdhGpuLoadPoll`, `GpuLoadStabilizer`) and `gpu_load_sampler.{h,cpp}`,
  wired into `host_metrics.cpp` (NVML -> D3DKMT -> hardened PDH) and `sensor_service.cpp` (500 ms active-target
  polling, `[Metrics] GPU load source:` transition log, `gpuSrc`/`gpuHeld`/`gpuDrop` in `[Sensors] Summary`).
- Tests: `tests/test_gpu_load_sampler.cpp` (13 tests) + `HostMetricsSourceInvariantTest.HostMetricsUsesLayeredGpuLoadSamplerAndStabilizer`.
  In-game Mirror's Edge and FIFO V-Sync hardware retest pending.

### 2026-11-15 - RoboCop overlay blank on DLSS-G activation: v1 slDLSSGState OOB read + cold-start PostSL/pre-SL gates (fixed)

- `RoboCop-Win64-Shipping.exe` (`20261115_142942`, `0.1.7527`, Steam Overlay + Streamline active): the overlay
  blanked during startup DLSS-G transitions via three interlocking bugs:
  1. `slDLSSGState` (`stateVer=1`) tail-padding/stack-neighbor OOB read: `Hooked_slDLSSGGetState` and
     `HasDLSSGRuntimeFenceEvidence` read v2 `numFramesToGenerateMax` (padding `190` -> `191x`) and v3 completion
     fence fields (adjacent caller `ViewportHandle::structType` GUID `0x4FC89B3C171B6435`), falsely firing
     `OFF->ON via GetState` at `19:49:28.101` while DLSS-G was off. Fix (9e89baee): gate v2/v3 `slDLSSGState`
     fields on `state.structVersion` (`hook/ngx/dlssg_health_policy.h`, `streamline_hook_{state,dlssg}.cpp`,
     fake `sl_dlss_g.cpp`); unit `DLSSGStateMaxFramesAndCompletionFenceAreGatedOnStructVersion`.
  2. Cold-start same-queue `PostSL` takeover blocked inside the 3 s `ProbeRealD3D12ECL` deferral: at the real
     `slDLSSGSetOptions(ON)` (`19:50:26.519`), `dx12_hook_g_SLWrapperQueue` was captured while `RealD3D12ECL`
     was still null, so `PrepareRouteActivation` required `originalEcl == nativeEcl` and rejected immediate
     takeover even though `SubmitAndConfirmOutput` dispatches via `kSelectedQueueOriginal`. Fix:
     `HasExplicitFinalOutputNativeSubmitForSameQueueColdStart` (`postsl_queue_selection.h`, `dx12_hook_postsl_render_entry.cpp`)
     accepts `DX12_HasNativeStartupOutputSubmitPath(sqScQueue)` while `!nativeEcl`.
  3. Pre-SL toggle-on make-before-break suppressed by `runtimeOwnsSwapchain=true`: startup Streamline wrapping
     set `dx12_hook_g_FGRuntimeOwnsSwapchain` from Frame 1 even with `sqScQueue == sqOrigQueue`. Fix:
     `ShouldEagerlyDrawPreSLOverlayDuringDLSSToggleOn` / `ShouldKeepPreSLOverlayLiveThroughDLSSToggleOn`
     (`fg_metrics_and_transitions.h`, `dx12_hook_fg_startup.cpp`) allow `runtimeOwnsSwapchain` when
     `exactNormalOverlayOriginalQueueSwapchainProof` holds. Units: `tests/test_dxgi_shared_part15.cpp`. Hardware
     retest in RoboCop pending.

### 2026-11-13 - DLSS-off menu frame drops: Windows' "Variable refresh rate" setting overrides V-Sync

- Sealed trace `display_stall_28620_ep3_switch.etl` (`20261113_185020`): DXGI `Present/Start SyncInterval=1` is followed
  by DXGI `SwapChain/Info NewSyncInterval=0` on every GTA present; dxgi symbols name it `EventOverrideSyncInterval`,
  raised by `IsVariableRefreshRateOverrideEligible` when the user's `VRROptimizeEnable=1` is set on a VRR output.
  DLSS-off: 132 cancelled flips in 8 s, none on the FFX chain (FidelityFX paces). Driver profile has no V-Sync value
  (drs_tool, status -160), so not the driver. Details: graphics-overrides-and-frame-pacing.md.
- Supersedes the flip-metering hypothesis below (sl.dlss_g's 1-frame `SetFlipConfig` while off is not the cause).
- Detection: display-timing session now takes DXGI id 0x4A; one `Windows replaces SyncInterval=1` warning per process
  and `vrrSyncOverrides=` in the health line. Behaviour change (Windows setting off vs CE-side cap) left to the user.

### 2026-11-13 - GTA cold FG black screen root-caused: not CE (GTA's ClipCursor loop, DWM queue)

- The user saw the black screen with Capture Engine closed. Seals `20261113_185020` (normal overlay, DLSS FG, 5.9 s)
  and `20261113_190605` (`overlay_observer_only=1`, FSR FG, 6.8 s), 0.1.7526: dwm.exe's channel-2 flood (~48k
  batches/s, from -19.2 s) runs past `SwapChainBind`; the new surface is realized in the millisecond it ends.
- Standalone repro with CE closed: only ClipCursor (unchanged rectangle) creates DWM work, one batch per call, never
  coalesced, ~50.6k/s capacity. 45k/s: new swapchain shown +6 ms; 56k/s for 10 s: +1063 ms, exactly when the backlog
  drained. GTA calls ClipCursor from its own code (`+0x6d218c`) in a loop right at that capacity.
- "DWM slows with session age" (morning reading) refuted for these loads. Details and table:
  [display-stall-trace.md](../display-stall-trace.md). Repro and decoders now in `tools/tracing/` (`run_flood_repro.py`,
  `dwm_flood_digest.py`, `etl_payload.py`).

### 2026-11-13 - Hardware run 0.1.7526 (`20261113_185020`): graph fixes hold; DLSS-off menu drops ~16 frames/s

- Off -> FSR -> DLSS -> FSR in one process. DLSS -> FSR: `[SL UNLOAD ORDER] forwarded reason=chain-gone` after 205 ms,
  FFX chain by `sl.interposer.dll` BufferCount=3; FSR menus <= 144 presents/s (147/s only while alt-tabbed).
- Switch hitch kept (8eb17982): present gaps 480 ms (FSR -> DLSS) and 526 ms (DLSS -> FSR, 781 ms composed episode);
  `source_0_1pct_low_x100` 177-190 (a 525-565 ms interval) stays for ~5 s, through and after the episode.
- `[OverlayGraph]` per-route maxStep 9-12 ms at every switch (cf895d89; was 3596 slots).
- DLSS-G OFF on sl.dlss_g's chain (perf CSV 18:54:09-16 and 23-32.5): 160-161 presents/s rendered, display series
  exactly 144.0/s, present interval sd 1.2-1.5 ms (FSR chain 0.56 ms) - ~16 frames/s submitted and never shown
  (sensors `expired` grows; no immediate flips while off). sl.dlss_g calls `NvAPI_D3D12_SetFlipConfig` on EVERY present
  while off: `numFramesPerBatch=1 presented=1 timePerBatch=0.00ms`, ~1190 calls in 7.5 s; with FG on 2/2 batches,
  driver-scheduled immediate MPO flips. Corrects the entry below: not "every present flipped". sl.dlss_g is told
  driver VSync FORCEON by CE's DRS answer. Hypothesis (unproven): an engaged 1-frame metered batch makes the driver
  replace queued flips instead of honouring DXGI `sync=1`. The bridge log's key omits `timePerBatch`, so the logged
  ms values are each change's first call only.
- First FSR enable from off (18:52:14, uptime 5.44 h): new chain on screen only after 5.9 s, 769 presents discarded,
  `[SCANOUT STALL] ep=1` 6.3 s, ETL `display_stall_..._ep1_ended.etl` sealed - the first-handoff DWM stall seen so far
  only with DLSS-G, here with FSR; later handoffs 37-47 ms.

### 2026-11-13 - Graph: a switch hitch vanished after FG changes; DLSS-off menu above refresh (diagnostic only)

- User: a genuine spike near an FG change "disappeared" afterwards. `20261113_181813`: every DLSS->FSR switch has a
  present gap (800/584/556/642 ms, perf CSV) inside a 500-766 ms composed episode (sensors.log "expire without a flip
  completion" ... "Flip completions resumed"). While composed the graph shows the presentation series (gap visible);
  the display series dropped the whole stretch (`lastFrameTimeUs = 0` at resume), so returning to it lost the spike
  (`[OverlayGraph] resets=2` per switch). Fix 8eb17982: the first composed transition after a run of own flips stays
  in the display series (the screen's next change of the process's content); an own flip that only restarted the
  series inside an episode starts no interval. Tests: `PerformanceMetricsComposedHandoffTest.*` (fails without the fix);
  `DisplayTimingModeTest.OnlyTheConfirmedEnd...` now expects that one 6.945 ms interval. Caveat: a game restored from
  a long minimize straight into composition now records that gap like own flips already would.
- `[OverlayGraph]` per-route audit (cf895d89): a route's first draw after the other route is not judged (was
  `maxStep` 3596 at every switch).
- DLSS-G set OFF by GTA's menu on sl.dlss_g's own chain: 161-163 presents/s, `in(sync=0 flags=0x200)
  out(sync=1 flags=0x0)` at DXGI, every present flipped, no `nvFlipSchedule` applied, GTA has no driver V-Sync DRS
  value (`NvAPI_DRS_GetSetting(0x00A879CF)` driver status -160). Cause open; f393fbf8 wraps the resident
  `NvAPI_D3D12_SetFlipConfig` (forwarded unchanged) and logs `driver SetFlipConfig numFramesPerBatch=... calls=...`.
  Next run: whether sl.dlss_g still calls it (and with which batch) while DLSS-G is off in the menu.

### 2026-11-13 - GTA FSR menu judder after DLSS->FSR: CE's held DLSS-G unload never ended (root cause, fixed)

- User A/B (session `20261113_175053`, 0.1.7524): direct FSR start -> menu fine; FSR -> DLSS -> FSR -> menu judder /
  ghosting again. Good menu: exactly 144 presents/s (mean 6946 us); bad menu ~158-160/s (mean 6263-6324 us). Both
  `DXGI final present VSync: route=runtime-output in(sync=1 flags=0x0) out(sync=1 flags=0x0)`, DXGI forward ~0.2 ms in
  both, same overlay route (`suspend-backbuffer`), clean `[OverlayGraph]` counters in both.
- Difference: the good FSR chain was created through `sl.interposer.dll` with BufferCount=3 (GTA had unloaded DLSS-G
  first: `slSetFeatureLoaded(DLSS-G,0)` forwarded, no live chain); the bad one by `sl.dlss_g.dll` with BufferCount=6.
  At the switch CE held the unload (`[SL UNLOAD ORDER] deferred` 17:53:35.108). The proxy's release took the native
  chain to 0 refs at .534 (callers `dxgi.dll+0x14C03E/0x14C0B6` = ReShade's proxy, stack via `sl.interposer+0x2A68F`)
  with NO `slHookSwapChainDestroyed`, so `proxy-released` never matched and the lazy `chain-gone` check needs another
  `slSetFeatureLoaded`, which GTA never makes. DLSS-G stayed loaded all session and its plugin made FidelityFX's chain.
- Same signature in every DLSS->FSR session since the hold (gtajudder, gtafsroverlaystutteryghosty,
  gtaoverlaylatefsrgraphstuttery, gtafsr, 20261112_*): `deferred`, never `proxy-released`; once `chain-gone` at a later
  load call. Supersedes the "tearing / driver ignores sync=1" reading of the gtafsr entry below: the unlocked cadence
  belongs to sl.dlss_g-created chains (also 163/s with DLSS-G off on the DLSS chain itself), not to FSR.
- Fix: `UnloadOrderPolicy::OnNativeReleaseReturned` forwards (`reason=chain-gone`) at the release that leaves the held
  chain with no reference, except while that chain's destroy hook runs (`OnDestroyEntered`, Streamline's hook loop).
  Forward context is the same as the existing proxy-released path (after the native Release returned, inside the
  caller's Release). Not hardware-verified: whether real Streamline accepts `slSetFeatureLoaded` re-entrantly there.
- Tests: `StreamlineUnloadOrderPolicy.AHeldChainDestroyedWithoutTheDispatchForwardsAtItsLastRelease`,
  `.TheLastReleaseInsideTheDestroyHookWaitsForTheHookToReturn`; flow
  `FlowDLSSUnloadOrder.AChainDestroyedWithoutTheDestroyHookStillReleasesTheHeldUnload` (fake interposer
  `flowInterposerSetDestroyHookDispatch(false)`: plugin lets go without the hook CE observes) - fails without the fix
  ("DLSS-G stayed loaded", no forward), passes with it; 44 DLSS/switch/menu scenarios pass.
- **VALIDATED** `20261113_181813` (0.1.7525, user: "seems fixed"): 4 DLSS->FSR switches, each `deferred` -> `forwarded
  reason=chain-gone residualRefs=0` after 208-433 ms (caller `dxgi.dll+0x14C0B6`, no deadlock in real Streamline), FFX
  chain by `sl.interposer.dll` BufferCount=3, FSR windows <= 144 presents/s, 12 swapchain handoffs `missing=0`.
- Open (pre-existing, DLSS side): with DLSS-G set OFF by the menu on sl.dlss_g's own chain (18:20:07.6-16) the game ran
  161-163 presents/s with `out(sync=1 flags=0x0)`, every present flipped (+1486/+1488, 0 discarded), no flip schedules
  applied. Same in 20261113_175053 and gtafsr. Likely the same overlay judder in DLSS menus; not investigated yet.
- Diagnostic flaw (mine, e8cb83a9): `[OverlayGraph]` per-route residual/maxStep compare a route's draw with its own
  last draw, seconds old after the other route drew (`out=1072/50/4/3.47/3595.96`); a handoff artifact, not a jump.

### 2026-11-13 - GTA FSR-suspended menu "overlay at lower fps" (session gtafsr, 0.1.7520): presentation, not the graph

- Timeline: DLSS-G on 22:35:35, off 22:36:01; FFX takeover 22:36:08; FSR configure on 12.53 / off 14.99 / on 17.59 /
  off 20.24 (menu, `suspend-backbuffer` route) until exit 22:36:31.
- Overlay side clean: one suspend-route draw per game frame on the owner queue; `[OverlayGraph]` 14.7-24.7 s
  `app=1114/2/0/0.03` (2 holds, 0 jumps, mean residual 0.03 slot); substitute UI texture cleared once on entry, no
  `UI-composite overlay drawn` (old CE-queue path never ran).
- Presentation in the menu: prework count and perf CSV agree on ~157 game frames/s (CSV qpc deltas mean 6.38 ms,
  sd 0.6-0.8 ms) on a 144 Hz panel under `vsync_mode=fifo`; game input `sync=0 flags=0x200`, CE forwards 1/0 at the
  FFX proxy. sensors.log 22:36:17.9-27.9: runtimePresents +1504, flipped +1503, discarded +0, published +1404
  (display series refresh-bounded, sd 0). Every present flipped at > refresh = tearing presents; no driver flip
  metering then (`nvFlipSchedule applied` frozen at 1032 after DLSS-G off).
- Same unlocked cadence already after DLSS-G off without FFX (22:36:02-07, ~162/s). The only earlier 144 lock
  (22:34:39-59) was during focus loss: `overlay_wait` 5.2 ms of CE's own fence wait, i.e. GPU back-pressure under
  composition, not a foreground baseline.
- Open: the final-flip parameters were logged once per thread (first present: `sync=1 flags=0x0` during the protected
  FSR startup), so "interval lost above DetourPresent" vs "driver ignores sync=1" is undecided. Added
  `DXGI final present VSync: route=... in(...) out(...) ... rate=` (hook/present/final_present_vsync_report.h) on
  change + 10 s heartbeat. Next run: out(sync=1) at rate > refresh names the driver (check NVCP V-Sync for GTA);
  out(sync=0, 0x200) names the layer that dropped it. No overlay or pacing behaviour changed.
