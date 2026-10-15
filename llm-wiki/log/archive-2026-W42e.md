# llm-wiki Log

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
