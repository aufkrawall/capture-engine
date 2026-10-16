# llm-wiki Log

### 2026-10-16 - D3D11 AF CPU work and sampler ownership through translation

- `api-translation-overrides.md` records the HOTS/DXUP `20261016_154448` audit of every override family.
  Native Present ownership already prevents duplicate frontend pacing/post-processing; limiter inactive,
  zero recorded prerender waits, 4K RCAS/gamma active. No matched AF-off control run.
- Compact shader dependencies and exact changed-SRV invalidation reduce raw D3D11 reconciliation; unrelated
  overrides no longer activate sampler replacement. Creation provenance and nested thread-local translation
  scopes prevent applying one sampler policy again at bind/native creation. CE 0.1.7180 closing gate passed
  remaining native tests and all 79 flow scenarios; one source assertion in pre-existing cursor work was
  excluded after the unfiltered run identified it. Eight real-DXUP 12-frame scenarios passed filtering,
  native pacing and overlay checks; one earlier cleanup timeout remains unlocalized. Fresh game A/B pending.

### 2026-10-16 - Reservoir VRAM design: retained YUV recommended, packet-held delay rejected (awaiting approval)

- Design in `wgc-reservoir-vram-design.md`, summary in `nvenc-encoding.md` backlog item 4. Retain P010/NV12 converted
  at ingress with the encoder's own parameters; copy the selected slot into an encoder pool frame per output; patch
  cursor and face camera into that copy in YUV at the unchanged per-slot timing. Depth, selection and A/V contract
  unchanged. 4K dup 10-bit 2025 -> 1519 MiB, 8-bit -> 759 MiB; HDR (FP16 retained, budget-capped below 300 ms today)
  -> 1519 MiB at full depth.
- Rejected: arrival-time cursor composition (static desktop = all repeats, cursor would freeze); retained slots as
  NVENC pool frames (overlays and in-flight lifetime need separate surfaces anyway); delay held as packets
  (decide-on-arrival turns delivery gaps into NVENC bursts a 98%-busy p6 encoder cannot absorb; raw backlog stays
  about one gap deep).
- Found in passing (from code, unmeasured): the face camera draws the newest camera frame into slots whose content
  and mic audio are `contentDelay` older, so the camera likely leads the mic by ~300 ms with the reservoir.

### 2026-10-16 - Media Foundation on the bridge + input retention; Quick Sync depth auto (no AMD/Intel to test)

- mfenc unreferences the input AVFrame right after ProcessInput while the async hardware MFT may still read the
  pool texture: CE now retains MF inputs until their packet (`encoder_input_retention.h`). MF also uses the
  encoder device bridge (NVIDIA's MFT is NVENC underneath); `EncoderDeviceBridgeNvencTest` encodes bridge-written
  frames through h264_mf with `hw_encoding=1` on the RTX 5070.
- qsvenc blocks in SyncOperation at `async_depth` (default 4), the NVENC starvation pattern: `async_depth=auto`
  now sizes it from the 100 ms budget. AMF (16) unchanged. See `hardware-encoding.md`.

### 2026-10-16 - NVENC on its own D3D11 device (encoder device bridge) and lazy VideoProcessor (run pending)

- `c5fb0690`: VP objects and the BGRA staging texture only on the Blt fallback path.
- Encoder device bridge (`encoder_device_bridge.h`, `video_encoder_submit_gate.cpp`; see `nvenc-encoding.md`): NVENC
  pool textures live on a second device with shared NT handles; CE writes them through cached work-device aliases;
  each frame is handed over at a work-device fence and sent once the CPU sees it complete (wait only beyond 33 ms of
  gated frames). Rejected: a GPU `Wait` on the encoder device (NVENC would either order behind that device's 3D turn
  again or not honour it). Real-GPU tests prove cross-device NV12/P010/BGRA visibility and an h264_nvenc encode of
  bridge-written frames.
- Hardware runs wanted: inject and dxgi_dup 4K120 10-bit at p5/p6, without and with contention (higher `gpu_load`,
  3D ~99%). Look for `[EncoderBridge] ... runs on its own D3D11 device`, timeline `submit` GPU ~0, `gate=`/`gateWaits`
  in `[VideoEncoder Timing]`, and NVENC `videoEncode` near 100% at p6.

### 2026-10-16 - Starvation/VRAM audit after the fixes (backlog, nothing implemented)

- The full prioritized backlog with evidence is in `nvenc-encoding.md` ("Starvation and VRAM backlog"). Next item to
  implement: NVENC on its own D3D11 device. The timeline `submit` step still waits ~7-11 ms GPU after every encode
  submit, which turns into NVENC starvation under GPU contention. Then: VP-free scaled/FP16 SDR, lazy VP init
  (`bgraStagingTexture` ~33 MB wasted), the retained reservoir (dxgi_dup 4K 10-bit prewarms 64 R10 slots ≈ 2 GB for
  the 300 ms target), and the NVENC depth (13 surfaces).
- The user wants optimal results both with and without GPU contention. Hardware runs needed after item 1: a
  contention run (higher `gpu_load`, 3D ~99%) for inject and dxgi_dup at p5/p6.

### 2026-10-16 - VALIDATED: inject p6 holds at NVENC 98-100%; dx9 inject p5 and dxgi_dup p5 healthy

- Session `20261016_015331` (build 0.1.7176, 4K120 av1_nvenc 10-bit, same test-app load):
  - r0001 dx9_test inject p5 (`inputFmt=87`, shader YUV): sendAvg 133-159 us, `dup=0` steady, leases 8.5-9.5 ms,
    `videoEncode=82-84/87%`, `maxAudioDelta=0 us`.
  - r0002 dx9_test dxgi_dup p5 (`inputFmt=24`, shader YUV): sendAvg 77-92 us, `submit=6884-7348` GPU, healthy,
    `maxAudioDelta=1 us`.
  - r0003 dx12_test inject p6: sendAvg 131-160 us, `dup=0` steady, leases flat 8.5-9.4 ms, `videoEncode=95-98/100%`
    (NVENC itself is now the limit, as dxgi_dup p6 was in `223320`), `maxAudioDelta=1 us`.
- The remaining `submit` GPU gap (~7-11 ms at the Done stamp after `avcodec_send_frame`, CPU ~0.15 ms) does not
  accumulate and does not limit throughput even with NVENC saturated; treat it as the driver ordering the stamp
  after NVENC's input acquisition, not as a coupling. Open: scaled and FP16 SDR conversions still use the
  VideoProcessor.

### 2026-10-16 - VALIDATED: inject p5 at 4K120 holds (shader YUV + staged repeat cache)

- Session `20261016_014920` (build 0.1.7176, dx12_test x64 inject, av1_nvenc p5 10-bit 4K120, same `gpu_load=120`
  `vsync=1` load as the overload runs): 24 s recording, sendAvg 135-185 us in every window (first 1.4 ms),
  `hardEncoderOverload=0 dup=0 unique=120` in every steady window, no capacity warning, source-read `holdMaxUs`
  flat at 8.5-8.9 ms (one frame; was climbing to ~97 ms), `videoEncode=82-85/86%`, `maxAudioDelta=0 us`.
  Timeline `convert=82/82|20-26 sourceRead=0 submit=6895-10358/6951-12630|133-168 next=0|8151-8223`.
- Before (same load, inject): p4 overloaded since `ffac1467`-era builds; the fix chain was `7f72319e` (SDR YUV via CE
  shaders instead of `VideoProcessorBlt`) plus `1e804301` (repeat-cache copy staged before the encoder submit).
  `submit` still shows ~7-10 ms GPU wait at the Done stamp (after `avcodec_send_frame`) without accumulating; it is
  the remaining 3D<->NVENC coupling to check at p6/p7 against dxgi_dup.

### 2026-10-16 - Shader YUV held p5 ~7 s; the repeat-cache copy after the submit was the second coupling

- Session `20261016_013801` (build 0.1.7175, dx12_test inject p5): `[SDR YUV] Direct shader conversion active ...
  P010 BT709 full`; convert dropped to `86/88 us` GPU and sendAvg stayed 134-163 us for ~7 s (no repeats, NVENC
  `videoEncode=83/86%`), then rose to 5-7.5 ms with ~10-27 repeats/s. The source-read lease hold grew steadily
  (26, 33, 50, 58, 66, 75, 94 ms; pending 3 -> 11) until the 12-deep pipeline filled. Timeline from the first
  window on: `submit=10480/13772|897/3918` (healthy phase) and `9772-10723/14257-19388|4081-8568` later.
- The `submit` step holds `CacheRepeatFrameTexture(d3d11Frame)`, a 3D copy of the encoder input issued after
  `avcodec_send_frame`: it waits for NVENC's encode of that surface, and the next conversion queues behind it, so
  CE's 3D queue loses ~1% per frame at p5 (E + copy turn > 8.33 ms). dxgi_dup has the same copy (`submit=7-11 ms`
  in `003600`), which is why its own ceiling sat near NVENC 96-100%.
- Fix: `StageRepeatFrameTexture` copies the converted frame into a spare cache texture before the submit (inject,
  screen-grab and recomposed repeats); `CacheRepeatFrameTexture` after a successful submit swaps it in (pointer swap,
  frame-scoped via `RepeatFrameStageScope`, same-frame repeat calls dedupe). Cost: one extra cache texture (~25 MB at
  4K P010). Pinned by `tests/test_repeat_cache_stage.cpp`.

### 2026-10-16 - The coupling is the D3D11 VideoProcessor; SDR now uses CE's own YUV shaders (run pending)

- Session `20261016_011829` (build 0.1.7174, dx12_test inject p5, no per-frame Flush): overload unchanged (sendAvg
  8.3-9.1 ms, `dup=43-45/s`, leases 109-125 ms, `videoEncode=80-83%`), but the GPU wait moved INTO the conversion:
  timeline `convert=15853-21887/26546-33497|30-36 sourceRead=0|1 submit=93-105|7534-10770 next=0`, and `gpuSpan`
  rose from ~61 us to 16-22 ms.
- Across 003600/010712/011829 the stall always sits at the first 3D-queue step that must follow
  `VideoProcessorBlt`: after a Flush boundary when CE flushed, at the timestamp right after the Blt without one, and
  in dxgi_dup at the repeat-cache copy that reads the Blt's output (`submit=7-11 ms`). The Blt completes only when
  NVENC has worked off its earlier frames, so on NVIDIA the VideoProcessor work of the NVENC device queues behind the
  encodes (inject waits ~2 encodes: its R/B swap + cursor pass put more 3D work in front of each Blt).
- Fix: SDR RGB -> NV12/P010 with CE pixel shaders (`PS_YuvY`/`PS_YuvUV` in `video_color_conversion_shader.h`,
  `VideoEncoder::ConvertSdrRgbToYuv` in `video_encoder_convert_yuv.cpp`), as HDR already did: BT.709 or BT.2020 NCL,
  full/limited range from `color_range`, left chroma siting (two bilinear taps = 1-2-1 on the even column, rows
  averaged), RGBA sampled directly (no swap pass). Used when the source is SRV-sampleable, not FP16 and unscaled;
  scaling and FP16 SDR keep the VideoProcessor (open: same coupling there). GPU tests in
  `tests/test_sdr_yuv_shader.cpp` check exact codes, RGBA=BGRA, BT.2020 and left siting on the real GPU.
  Logs: `[SDR YUV] Direct shader conversion active ...` or `[SDR YUV] VideoProcessor conversion used (<reason>)`.

### 2026-10-16 - The stall is CE's own Flush ahead of the encoder submit (fix pending run)

- Session `20261016_010712` (build 0.1.7173, dx12_test inject, preset p5): recording stable (no device loss, the
  encoder-thread marker polling works, `maxAudioDelta=1 us`), but the overload is unchanged: sendAvg 8.1-8.4 ms,
  `dup=46-47/s`, leases ~97-109 ms, `videoEncode=85/86%`. Timeline `convert=60/61|24/29 sourceRead=16213/25614|155/239
  submit=104/105|7161/7425 next=0/0|34/67`: the GPU still stood ~16 ms in the sourceRead step although it now held
  only `End(query)` + `Flush` (no Signal). dxgi_dup (`003600`) waited ~one encode in its `submit` step, right after
  the stage probe's once-per-second `Flush`.
- Reading: on NVIDIA each D3D11 submission CE forces on the NVENC device before the encoder submit waits for
  NVENC's outstanding work, and the encoder's input then waits for that submission, so conversion and encode
  serialize. dxgi_dup normally lets the encoder submit carry CE's work (one submission per frame); inject added its
  own per-frame `Flush` in `ffac1467` (15:21), before the first overload run `205007`.
- Fix: no `Flush` on the per-frame path: source-read marker, stage probe `End` and timeline `Next` ride on the
  encoder submit (pinned by `InjectSourceReadTokenTest.PerFrameEncoderPathForcesNoGpuSubmission`). Teardown still
  flushes inside `FinishSourceReadMarkers`. Expected: inject p4/p5 like dxgi_dup (sendAvg ~0.1 ms, no repeats);
  the probe's "before send" readiness can no longer occur.

### 2026-10-16 - Query marker polled off-thread removed the device; markers now encoder-thread only

- Session `20261016_004939` (build 0.1.7172, dx12_test inject p4): recording failed ~0.5 s after going live.
  `av1_nvenc EncodePicture failed!: invalid param (8): EncodeAPI Internal Error`, the shared fence reported removed
  with `deviceReason=0x887A0020` (`DXGI_ERROR_DRIVER_INTERNAL_ERROR`), then every texture creation/open failed with
  `887a0005` until the 5 s terminal video failure ended the recording. Only change against 0.1.7171: `e98655e2`
  made the source-read marker an event query whose token ran `GetData`/`Flush1(ALL)` on the encoder's immediate
  context from the inject ingest thread, while NVENC used that context on the encoder thread (NVENC calls are not
  under CE's D3D11 lock). The fence token had only used thread-safe `ID3D11Fence` methods.
- Fix: queries stay, but only the encoder thread polls them (after every `avcodec_send_frame`, like the stage
  probe); completion crosses threads as `SourceReadSequencer` (atomic sequence + wake event). ABI: token export
  takes `uint64_t* sequence`; new `MediaEngine_IsInjectSourceReadComplete`, `MediaEngine_SetInjectSourceReadWakeEvent`,
  `MediaEngine_FinishInjectSourceReads` (teardown after the encoder join: one fence wait, stall irrelevant there).
  Lease release now lags completion by at most one encoder submit (~8 ms at 120 fps).

### 2026-10-16 - GPU timeline names the stall: the source-read fence Signal (fix pending run)

- Session `20261016_003600` (build 0.1.7171, dx12_test, same load as `001030`). r0001 inject p4: overload as before
  (sendAvg 8.1-8.6 ms, `dup=49/s`, leases ~90 ms, `videoEncode=84/87%`). Steady timeline windows:
  `convert=61/62|27/30 sourceRead=16237/21489|130/153 submit=104/107|7231/7431 next=0/0|45/77` and
  `sourceRead=16858/25968|117/130`: the GPU stood 16-26 ms at the `ID3D11DeviceContext4::Signal` that marks the
  source read, which CE submitted in ~0.13 ms. r0002 dxgi_dup p4 healthy (sendAvg ~90 us, `videoEncode=78/86%`):
  `convert=352/369|220/335 sourceRead=0 submit=6849-10799|209-229 next=0|7937-7979`, i.e. its 3D queue waits
  about one encode at the repeat-cache copy of the encoder input but never stalls ahead of a conversion for two.
  Both recordings `maxAudioDelta` <= 1 us.
- Conclusion: on the NVENC encoder device the fence Signal is ordered behind outstanding NVENC work and holds CE's
  following 3D work, so conversion N+1 waits for encode N or later and NVENC idles between frames.
- Fix: the source-read marker is now an event query (`End` + `Flush`, released through the existing query-token
  path with `Flush1(ALL, event)` wake-up); the encoder creates no fence. Expected: inject p4 sendAvg ~0.1 ms and
  timeline `sourceRead` GPU ~0. If the stall moves to `submit`/`next` instead, `Flush1(ALL, event)` joins NVENC too
  and the wake-up needs `D3D11_CONTEXT_TYPE_3D`.

### 2026-10-16 - Fence-coupling fix had no effect; GPU timeline diagnostic added

- Session `20261016_001030` (build 0.1.7170, dx12_test x64, 4K borderless, `gpu_load=120`, `vsync=1`, inject, av1_nvenc
  10-bit, `surfaces=13 inFlight=12`, `gpu_priority=7`). r0001 p3: sendAvg 75-99 us, `dup=0`, source-read
  `holdMaxUs` 5-12 ms, `videoEncode=82/83%`, fresh `submitToReady` 8.5-10 ms. r0002 p4: sendAvg 8.1-8.6 ms,
  `hardEncoderOverload=1 dup=45-47/s`, `capacity_pressure_observed cause=encoder peakDebtMs=75`, fresh
  `submitToReady` 86-97 ms (repeat 99-117), `gpuSpan` 61-83 us, source-read `holdMaxUs` 90-97 ms with 7-11 pending,
  `videoEncode=81/87%`, 3D 79-80% (target 77). Both finalized with `maxAudioDelta=1 us`. Same as 223320, so the
  redundant GPU Wait and the marker position were not the cause.
- Facts: p3 and p4 read the same `videoEncode` (~81-82%, ~6.8 ms/frame at 120 fps) and dxgi_dup p4 80% (223320);
  only dxgi_dup p6 reached 96-100%. The source-read Signal, now issued before the encoder submit, completes about
  one pipeline depth late at p4, like the probe's event query, so CE's 3D work around frame N completes only as
  NVENC catches up. Consistent with CE's queue being serialized behind NVENC (conversion N+1 behind encode N, so
  per-frame cost = encode + CE's 3D turn and no NVENC frame overlap) in inject only; the remaining inject-only
  operations are the source-read fence Signal + Flush and reading the producer's cross-process shared texture.
  The repeat-cache copy of the encoder input after the submit happens in both paths.
- Added `[VideoEncoder GPU Timeline]` (once per second, fresh and screengrab): GPU vs CPU time for convert,
  sourceRead, submit (encoder submit + repeat-cache copy) and next. Decisive run: inject p4 and dxgi_dup p4 in one
  session; the step with GPU ~ one encode time and small CPU time names the waiting operation.

### 2026-10-16 - Inject fence coupling removed (hardware A/B pending)

- From the `20261015_223320` hypothesis: `WaitForFrameFence` no longer queues `d3d11Context->Wait` on the producer's
  fence (the frame is only admitted once the CPU saw the value complete; a pending fence still defers the frame), and
  `EncodeFrame` signals the source-read fence right after the last source read, before `SubmitFrameForEncode`,
  instead of at scope exit behind the NVENC submit (guard destructor still covers early exits). The slot's lease is
  also released one encode sooner. `IsEncodingPoll` now needs `videoEncode >= 1%` (`kEncodingPollMinPercent`).
- Tests: `FrameFenceFailureTest.CpuVerifiedFenceQueuesNoGpuWait`,
  `CaptureCoordinatorSourceTest.EncoderSignalsSourceReadCompletionOnEveryExitAfterTheFenceWait` (marker between the
  last read and the submit), `GpuEngineBreakdownTest.OnlyPollsWithABusyVideoEncodeEngineAreEncodingPolls`.
- Decisive check: inject p4 (dx9 and dx12) at the same load. Success = sendAvg ~0.1 ms, no repeats and `videoEncode`
  ~80-86% like dxgi_dup p4; then p5/p6 should approach dxgi_dup's 96-100% before overloading. If p4 still blocks with
  `videoEncode` < 90%, the hypothesis is wrong and the remaining inject-only difference is reading the producer's
  shared texture itself (cross-device implicit sync), not CE's fence operations.
