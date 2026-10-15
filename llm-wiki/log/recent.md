# llm-wiki Log

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

### 2026-10-15 - Inject p4 overload is a CE-side coupling; dxgi_dup holds p6 at NVENC 100%

- Session `20261015_223320` (build 0.1.7169, av1_nvenc 10-bit 4K120 SDR, tune=hq, vbr, `surfaces=13 inFlight=12`,
  same test-app load as the user's other runs). User: inject p3 holds and p4 overloads in both dx9_test and
  dx12_test; dx12_test with `capture_method=dxgi_dup` showed no overload up to p6. All 8 recordings finalized with
  exact A/V ends (`maxAudioDelta` <= 1 us).

  | rec | capture | preset | sendAvg | submitToReady (fresh/screengrab) | videoEncode avg/max | 3d (target/others) | health |
  | --- | --- | --- | --- | --- | --- | --- | --- |
  | r0001/r0003 | inject dx9/dx12 | p3 | ~0.1-0.2 ms | 9.5 ms | 77-79/81-82% | 80-81% (76-78/3-4) | healthy |
  | r0002/r0004 | inject dx9/dx12 | p4 | 7.8-8.4 ms | 81-106 ms (repeat 106-160) | 84/86% | 79-80% (76-77/3) | pressure, peak 34/59 ms |
  | r0006 | dxgi_dup dx12 | p4 | 93-98 us | ~8-9 ms | 80/86% | 86% (78/8) | healthy |
  | r0008 | dxgi_dup dx12 | p6 | 84-94 us | 8.2-9.3 ms | 96/100% | 88% (80/8) | healthy |

- dxgi_dup is not missing an overload: at p6 NVENC runs at 96-100% and still keeps the 12-deep pipeline fed with
  120 fresh frames/s (`WGC Perf Input: 144 Dup: 0`). So 4K120 p6 fits this NVENC, and the inject ~122 fps ceiling
  is not NVENC's raw limit. With inject p4 NVENC idles ~15% while 11 frames are queued and every submit blocks, so
  NVENC waits on CE work; the 3D engine is not saturated either.
- Inject-only operations on CE's D3D11 context per fresh frame: a GPU `Wait` on the game's shared fence (always
  issued, although `WaitForFrameFence` only proceeds once the CPU saw the value complete, so it is redundant), and
  `SignalSourceReadCompletion` (`ID3D11DeviceContext4::Signal` + `Flush`, since `ffac1467` at 15:21, before the first
  overload run `205007`). Hypothesis, unproven: the NVIDIA D3D11 driver orders these device-level fence operations
  after the NVENC work submitted on the same device, so conversion N+1 queues behind encode N-1 and NVENC and the
  3D queue ping-pong; p3's shorter encode fits the cycle, p4's does not. Caveat: a D3D11 event query may share the
  same device-level semantics, so `submitToReady` alone cannot tell cause from effect; `videoEncode` < 100% while
  submits block is the decisive symptom.
- Also seen: `[Metrics] GPU engines` logs single-poll `videoEncode=0/0%` windows (a residual below 0.5% passes
  `IsEncodingPoll`); dx9 shared-handle opens log `DuplicateHandle ... failed` + `Trying KMT path` 8-9 times per
  recording start before the KMT path succeeds; controller `Main-thread blocked 329-500ms in config-reload service
  sync` on config changes (known synchronous child-process sync).

### 2026-10-15 - dx12_test p4 overloads, p3 holds (superseded: NVENC was not the limit, see above)

- Session `20261015_221846` (build 0.1.7168, dx12_test x64, av1_nvenc 10-bit 4K120 SDR, tune=hq, vbr, inject,
  `pipeline: surfaces=13 inFlight=12`; the test-app load settings were not supplied). r0001 = p4, r0002 = p3.
- p3: sendAvg 186-213 us, submitToPacket ~91.7 ms (= 11 queued frames), `dup=0 unique=120/s`, healthy.
- p4: the first second sends at 231 us, then sendAvg 8.2-8.3 ms per send at inFlight 11/12 from 22:19:34.9 on,
  `encode=8.37ms budget=8.33ms sustain=119.5-122.8fps`. The pacer repeats ~1/3 of the slots
  (`hardEncoderOverload=1 dup=41 DupTimer=35-41/s unique=81/s` at source 126 fps). Peak debt 50 ms, final debt 0,
  `capacity_pressure_observed cause=encoder`. Same shape as the dx9 p5 run in `213903` (sendAvg 8.1-8.6 ms, ~45%
  repeats), so heavier presets land on the same ~122 fps ceiling and differ only in how many slots repeat.
- The conversion is not the limit here: `[VideoEncoder GPU Stage] path=fresh gpuSpan=61-66 us` (dx9 had 2.5-5.8 ms),
  and the 3D engine was not saturated (`3d=66/81%`, CE's share `others=2/3%`). That settles the open queue-
  serialization question for DX12; for dx9 it stays open.
- Measurement defect found in `7f7427c8`: `[Metrics] GPU engines` averaged every poll of a 10 s window, so the one
  window per 10 s recording was about half pre-recording idle (`videoEncode=40/85%` p4, `50/82%` p3), and the
  recording's last ~5 s window was dropped at the session change. Undiluted estimates are p4 ~73-85% and p3 ~79-82%,
  so the counter did not separate a saturated preset from one with headroom; whether NVENC's PDH counter under-
  reports, or an unreported engine (OFA, logged as `other` now) contributes, is open. Fixed: only polls with a busy
  Video Encode engine enter the window, the tail is logged on the first idle poll (`end=encoderIdle`) or a target
  change (`end=targetChange`), and the line carries `spanMs` and `other=`.
