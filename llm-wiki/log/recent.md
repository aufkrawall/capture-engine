# llm-wiki Log

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
