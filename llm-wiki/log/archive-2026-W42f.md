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
