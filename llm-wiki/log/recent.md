# llm-wiki Log

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

### 2026-10-15 - Capture binding inherited a reused swapchain address

- `FlowInjectFGSwitch.CallbackFSRKeepsRecording` failed once in the closing gate (19.8 s under parallel load,
  `generations.size()` 1 vs 2) and passed 3/3 alone. The failing run's hook log was overwritten by the reruns; a
  passing log showed transport 1 (DLSS chain `...75E0`), transport 2 (FSR chain `...51E00`) and no transport 3:
  the returning Streamline chain was allocated at `...51E00` (handoff `life=2 -> life=3`, same address).
- Root cause: `SwapChainIdentityBinding` keyed on the identity address only, and the back-buffer desc check only
  catches a different size/format. Whether DXGI reuses the block depends on what else the process allocated, so
  the generation count was heap luck; under load the FSR chain evidently also landed on the DLSS chain's block.
- Fix: per-binding stamp in the chain's DXGI private data (see `cfr-capture-sync.md`). The flow test landed as
  `>= 2` in c77bfcc4 after a 2026-10-13 pre-commit run saw 2 vs 3 for the same reason; it now asserts a new
  generation in the FSR phase and in the return phase.
- Verified: 4 consecutive flow-suite passes (78 scenarios each, ~185 s under the suite's parallel load) plus the
  0.1.7168 gate. In CallbackFSRKeepsRecording the returning chain again reused the FSR chain's address and logged
  `new swapchain at the bound identity address ... (stamp=2) re-initializes`, then `stamp=3 ... transport=5`
  (generations 3-4 were spent on retries deferred by outstanding leases). 1-2 scenarios per suite hit the reuse.

### 2026-10-15 - GPU engine breakdown to tell NVENC saturation from queued work

- FFmpeg drives NVENC synchronously (`enableEncodeAsync = 0`, blocking `nvEncLockBitstream`), so a per-frame
  encode duration cannot be observed without blocking or patching FFmpeg. The sensors process's existing
  `\GPU Engine(*)\Utilization Percentage` poll answers the same question: `captureengine/app/gpu_engine_breakdown.h`
  keeps each engine type's busiest physical engine and splits the busiest 3D engine into target and others.
  `host_metrics.cpp` logs `[Metrics] GPU engines polls=N avg/max: ... videoEncode=a/b%` in `sensors.log` every 10 s,
  only while a video-encode engine was busy.
- Reading: videoEncode near 100% means NVENC is saturated and only the preset or resolution helps. videoEncode
  clearly below 100% while `[VideoEncoder Timing]` sendAvg is large means the encoder waits on queued work,
  so moving the conversion to its own queue or raising its priority could help. NVENC busy time per frame is about
  videoEncode% x 10 ms / encoded fps. Pending: a p5 run at gpu_load=120 on build 0.1.7168+.

### 2026-10-15 - Deeper NVENC pipeline validated: p3 now holds, p5 sits at NVENC's own limit

- Session `20261015_213903` (build 0.1.7167, dx9_test x64, gpu_load=120, vsync=1, av1_nvenc p5 10-bit 4K120). The
  user reports p3 now records without overload across several graphics APIs with inject capture. Before the fix,
  p1, p3 and p5 all stalled alike.
- p5 run: `pipeline: surfaces=13 inFlight=12`. `[VideoEncoder Timing]` shows sendAvg 8.1-8.6 ms, inFlight peak 12/12
  and submitToPacket ~91-96 ms. That is ~122 fps of encoder throughput against the 120 fps target (`encode=8.21ms
  budget=8.33ms`). The overload pacer repeats ~45% of slots (`hardEncoderOverload=1 holdWithCandidate=71 unique≈52/s`).
  Final debt was 0, peak 126 ms, and the A/V end was exact (`videoEnd=audioEnd=12333333`).
- GPU Stage: fresh submitToReady 80-99 ms, repeat 108-113 ms, always ready after send. With 3 in flight (session
  `205007`) it was 13-17 ms. In both runs it is about (depth - 1) x the per-frame interval, so the conversion waits
  behind earlier frames' encodes, not behind a fixed game-side delay. The preset now changes the outcome, so the
  remaining limit is NVENC throughput under this bandwidth-heavy synthetic load (120 full-screen 4K clears per
  frame). It is not a CE pipeline stall.
- Side effect: `[Inject SourceRead]` holdMaxUs is ~101-110 ms with up to 11 ring leases pending, because the
  conversion read completes late. The source was not starved in steady state (`srcLimited=0`).
- Open: whether the fresh conversion (gpuSpan 2.5-5.8 ms, interleaved with the game) and the encode are serialized
  on one queue, so a fresh frame costs their sum. A per-frame encode-duration measurement would decide it.
### 2026-10-15 - dx9_test overload: GPU-queue latency outruns NVENC's 3-frame pipeline

- Session `20261015_205007` (build 0.1.7165, dx9_test x64, vsync=1, av1_nvenc p5 10-bit 4K120, `gpu_priority=7`)
  with the `[VideoEncoder GPU Stage]` probe:

  | gpu_load | path | gpuSpan avg/max | submitToReady avg/max | send avg/max | ready b/d/a |
  | --- | --- | --- | --- | --- | --- |
  | 70 (healthy) | fresh | 1741/2708, 818/2724 us | 9769/15743, 8407/8763 us | 146/260, 71/111 us | 0/0/5 |
  | 120 (overload) | fresh | 395/446, 2495/5735 us | 12771/28179, 16779/34744 us | 5748/7457, 8011/11984 us | 0/3/2, 0/2/3 |
  | 120 (overload) | repeat | 50/51 us | 38649/42688, 52275/97644 us | 7548/7960, 8060/11153 us | 0/0/5 |

  `[VideoEncoder Timing]` submitToPacket was ~16.7 ms in both runs (2 frame periods at 120 fps).
- Mechanism (FFmpeg `ffmpeg_build/working/ffmpeg`): with no B-frames and no lookahead `nvenc.c` picks
  `nb_surfaces = max(4, frameIntervalP*4) = 4` and `async_depth = nb_surfaces - 1 = 3`; CE sets neither
  `surfaces` nor `delay`. `avcodec_send_frame` runs `encode_receive_packet_internal` (`encode.c`), and once
  3 frames are in flight `output_ready` makes it take the oldest packet with a blocking `nvEncLockBitstream`
  (`doNotWait=0`). So a frame must finish CE's copy/convert plus NVENC within ~2 frame periods (16.7 ms at 120)
  or every send blocks; that explains receiveAvg=0 and the preset-independence.
- Reading: CE's GPU work is tiny (fresh 0.4 ms, repeat 0.05 ms) but waits 8-10 ms on the 3D engine at 70% load and
  13-17 ms (fresh) / 39-52 ms avg, 98 ms max (repeat) at 96% load; a fresh span stretched to 5.7 ms means the
  conversion was time-sliced by the game. The repeat copy writes an encoder input surface, so part of its wait may
  be on NVENC itself (not separated). The current frame's readiness is not what send waits on (repeat: always
  ready after send); it waits on frame N-2's packet.
- Levers: a deeper NVENC pipeline (`surfaces`/`delay` plus CE's hw-frame pool and ring leases sized to match) turns
  latency back into throughput; RGB input straight into NVENC removes the 3D-engine conversion; higher GPU
  scheduling class shortens the queue wait. `gpu_priority=7` (max thread priority) was already active.
- Fix (same day, hardware run pending): `surfaces` sized by a 100 ms budget (13 at 120 fps); details and the audio
  in-flight exclusion in `nvenc-encoding.md` "Pipeline depth". RGB-direct was not pursued: NVENC's RGB input is
  believed to be converted by a GPU kernel too (unverified) and would give up CE's color control.
- Diagnostics audit: the screen-grab (WGC/dxgi_dup) submit had no send/packet timing and no GPU-stage probe, and
  repeat packets were never matched (`pendingPts` grew to 256). All three paths now share `SendFrameToEncoder`;
  GPU Stage gained `path=screengrab` and an end-of-recording flush. Format/HDR/bit depth were already covered by
  the `Color config` line; capture-side WGC copies by `[WGC GPU Timing]`.
- Expect in the next run: `pipeline: surfaces=13 inFlight=12`, `[VideoEncoder Timing] ... inFlight=x/y pipeline=12`
  with a small sendAvg at gpu_load=120. If sendAvg stays high with the peak at 12, the GPU queue wait exceeds
  100 ms and the next lever is the scheduling class.

### 2026-10-15 - Encoder overload in dx9_test is a blocking NVENC submit, not capture

- Setup: RTX 5070, av1_nvenc 10-bit 4K120, inject `D3D9-SHARED-DIRECT` zero-copy (same as Mirror's Edge).
  `[PERF]` showed convert=0.02 ms (CPU submit only) and encode=7.5-9.7 ms. `[VideoEncoder Timing] sendAvg`:
  `avcodec_send_frame` blocks, with receiveAvg=0. Repeats from the cached texture cost the same as fresh frames.
- Session `20261015_202600` (dx9_test x64, vsync=1):

  | gpu_load | GPU load | Frame rate | Preset | sendAvg |
  | --- | --- | --- | --- | --- |
  | 120 | ~96% | ~184 fps | p5, p3, p1 | ~9.5 ms each |
  | 50 | ~37% | 144 fps | p5 | 0.19 ms (healthy) |

  At gpu_load=5 (session `20261015_195839`, ~25 W, memory clock 7001 instead of 17001 MHz), p1 helped.
  Mirror's Edge at 99% load and ~500 fps was healthy at p5 (`20261015_161856`).
- Reading: the preset makes no difference under high load, so NVENC's own work is not the wait. The likely wait
  is the encoder's input mapping waiting for CE's D3D11 copy/VP conversion on the 3D engine, behind the test
  app's long monolithic frames (120 full-screen 4K clears). The low-load case looks like power-managed clocks.
  Neither is proven.
- Diagnostics added: `mediaengine/video/encoder_gpu_stage_probe.*` brackets the fresh copy/convert and the
  repeat copy with timestamp + event queries, once per second per path, and checks completion before and after
  the submit. It logs `[VideoEncoder GPU Stage] path=... gpuSpan=avg/max submitToReady=avg/max send=avg/max
  ready(before/during/after send)=a/b/c disjoint=n` every 5 samples.
  - "during" with send close to submitToReady and a small gpuSpan: the submit waited for queued CE work.
  - "before" with a large send: the wait is inside the encoder/driver.
- Next: a hardware run at gpu_load=120 and gpu_load=50 with these lines. CE-side levers depend on the outcome:
  RGB input straight into NVENC (off the 3D engine), REALTIME GPU scheduling class via the elevation service,
  or a power-profile mitigation for the low-load case.

### 2026-10-15 - Inject judder: ring lease released before the encoder's GPU read

- Mirror's Edge x86 (D3D9-SHARED-DIRECT, session `20261015_142836`): reported forward/backward motion with
  roughly 420 published frames/s, 16 slots, `ownedDepth` 12-16, and 110-120 cap trims/s. After startup selection
  stayed within 0.6 ms; only one duplicate among 2383 outputs. This supports a source-content race rather than
  ordinary CFR repetition, but visual confirmation on the original game remains pending.
- Code defect: acceptance submitted the GPU conversion read, then reset the producer slot's lease without waiting
  for that read. The producer's next write on its own device could execute first. This lifetime rule is generic.
- Fix: encoder source-read fence with a pre-reserved event-query fallback; owned completion tokens keep ring leases
  until GPU completion. Both fresh submission paths participate. Retry/read ownership shares only on retry;
  shutdown drains before resetting the ring and retains unproven slots on timeout.
- Focused regressions include actual D3D11 WARP fence/query event wakes and deterministic retry/teardown ownership.
  The old session's module-boundary failure is fixed through a forward declaration, without a policy exception.
  Hardware/game validation pending; expect `[Inject SourceRead]` windows and a final summary.
- Full validation exposed repeated crash-test directory names on Windows: deletion can succeed while an external
  handle keeps the name delete-pending. Per-setup sequence names and an open-directory regression remove that race.
- Closing gate 0.1.7162 passed native tests, Python tool self-tests, and all 78 FG flow scenarios; installer packaged.
- D3D9 direct-ring all-busy drops now distinguish `CpuLease` and `GpuBusy` in `[Inject Contention]`;
  the shared slot-selector source contract covers both counters and the producer's pending-query check.
