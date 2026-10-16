# llm-wiki Log

### 2026-10-16 - VALIDATED: NVENC encoder device bridge at 4K120 p6 under load; dxgi_dup reservoir stays RGB

- Session `20261016_183018` (0.1.7183, av1_nvenc 10-bit 4K120 SDR p6, Strange Brigade DX12 benchmark, high GPU
  load, variable game fps ~115-144). r0001 inject (74 s), r0002 `capture_method=dxgi_dup` (72 s). User: no encoder
  overload warning in either; files look normal.
- Both: `[EncoderBridge] av1_nvenc runs on its own D3D11 device: pool=p010le 3840x2160 shared-nt, gate depth=4`;
  `[VideoEncoder Timing]` every second `sendAvg=88-122us`, `gate=1/2..3/4`, `gateWaits=0` in 147 of 148 lines (one
  wait of 129 us), `submitToPacket` ~91.7 ms (`pipeline=12`), `inFlight=12/13-14`. Finalization healthy,
  `peakDebtMs=0`, every audio track `diff=+0`.
- Screen-grab GPU line: `NVIDIA GeForce RTX 5070 ... reason=encoder-vendor-drives-monitor adapters=2
  crossAdapterCapture=0` (single GPU plus the software adapter: unchanged behaviour, as intended).
- Inject: repeats only with source shortfall (`dup=2-3 srcLimited=2-3`, `hardEncoderOverload=0`). dxgi_dup: one
  CFR debt drop at the first encode (`Slow loop iteration: work=60.4ms dominant=encode`; inject's first loop 120 ms);
  later `Dup` only when the game ran below 120; a 206 ms duplication callback gap (`CbGap` max) absorbed by the
  reservoir (`BufMin` 34-43 of 45, `PoolLease max=58 freeMin=6 satDrop=0`).
- Gap found: the retained-YUV reservoir never engages on dxgi_dup. `[RetainedYuv] Route query: ... sameDevice=0 ->
  split capture device`, pool `retainedFmt=R10G10B10A2_UNORM 2025MB`: duplication always uses a dedicated capture
  device (`wgc_capture_duplication.cpp`), and `DecideRetainedFrameRoute` keeps every split-device pool RGB, so the
  dxgi_dup rows of the VRAM table in `wgc-reservoir-vram-design.md` (-25% / -62%) cannot occur. The encoder bridge
  shows shared-NT NV12/P010 plus a fence work across devices on this GPU (`EncoderDeviceBridgeTest`), which is the
  mechanism a split-device YUV route would need.

### 2026-10-16 - Retained NV12/P010 reservoir and face-camera timing implemented (hardware run pending)

- WGC/DXGI reservoir now retains the encoder's own NV12/P010 (`wgc_retained_yuv=true`): media-engine ingress
  conversion with the encoder's shared draws, YUV overlay patch for cursor/camera, NV12/P010 privacy black, same-device
  D3D11 lock between the capture and encoder threads (pre-existing race). Design, accuracy and logs:
  `wgc-reservoir-vram-design.md`. Real-GPU tests: untouched pixels exact, overlay pixels <= 1 code over smooth
  backgrounds; partial-alpha edges over sub-chroma-grid saturated detail up to 57 codes (10-bit chroma).
- Face camera now shows the frame taken at each slot's content time (`CaptureState::contentQpc`, GPU history ring);
  it led the microphone by about the 300 ms content delay before (`face-camera-overlay.md`, "Timing").
- Hardware wanted: dxgi_dup 4K120 10-bit p6 and WGC under contention (estimatedPool, smoothness and A/V summaries
  unchanged vs `wgc_retained_yuv=false`), HDR (full 300 ms, `capLimited=0`), static desktop with a moving cursor,
  face camera clap test (`slotLagUs`).

### 2026-10-16 - HOTS hook-service scan bursts: native load edges own discovery

- `20261016_170643`, CE 0.1.7180: hundreds of hook passes/s while native notification counts stayed
  fixed, with 6.8/7.7 s of service wall work during two loading bursts; source gaps reached 1.19/1.21 s.
  Capture counters stayed healthy. The scan overhead is established; its causal contribution to hitches is unproven.
- `main_overlay_detect.cpp` preserves synchronous per-return hooks but schedules background identity/UE
  discovery only from real image notifications when registered; failure retains the original fallback.
  Existing stage summaries count coalesced/fallback loader returns. No sleep, throttle or feature removal.
- `main_ue5_scan.cpp` clears pending work before its full snapshot so concurrent loads remain queued;
  failed enumeration requests retry with metered diagnostics. Real-loader flow coverage includes repeated
  references, unload/reload, fallback, and a deterministic load after the scan snapshot. Live retest pending.
- Startup spikes also include required live-device pipeline/resource creation. No speculative GPU
  prewarming or deferred overlay/capture initialization was introduced. See `dx12-injection-bootstrap.md`
  and `ue5-cvar-overrides.md` for current ownership and handoff invariants.

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
