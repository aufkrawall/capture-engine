# llm-wiki Log

### 2026-10-17 - One D3D11 forced-AF engine for the wrapper and the raw hooks

- Inventory of wrapper/raw duplication: the AF state machine (two copies, only the raw one had
  the sampler-keyed memo), CreateSamplerState policy (two copies; wrapper recorded hash 0 when
  inactive), two replacement caches (wrapper: per-sampler private-data variants; raw: (device,
  desc) cache), two cost formats. Raw-only gaps: no ClearState/FinishCommandList/
  SwapDeviceContextState hooks (after an unseen ClearState CE kept the replacement as physical and
  swallowed the game's rebind of the same sampler), no PSGetSamplers translation, a new deferred
  context inherited state at a reused address, config changes applied only on slot changes.
- Now: `hook/d3d11/d3d11_af_engine*` (state steps + templated Reconcile over Access/Host),
  `d3d11_af_metadata.*` (moved from wrappers, neutral names), `dx11_hook_af_raw.cpp` (raw
  access/host, map of unique_ptr states moved out before release), slots in
  `d3d11_context_slots.h` verified by `D3D11ContextSlotsTest` (probe vtable). Flow tests run each
  scenario on both paths (+ClearState rebind, read-back, live 16x->4x->off->16x without rebinds).
- Not shared on purpose: draw evidence for the DX11 present-interposer classifier
  (`NoteApplicationSubmission`) stays wrapper-only: raw draw hooks also fire for CE's own D3D11
  contexts (shared vtables), which would count as application rendering. Open.
- Raw bind steps still take the process mutex + map lookup per call; the wrapper takes none.

### 2026-10-17 - BioShock rerun (0.1.7197): Steam got CE's swapchain proxy; Present1/resize retries; wrapper AF cost

- `20261017_003122`: `DeepHook(x86): Installed at 60731170+8` on attempt 4, Steam then patched the entry
  (`E9 at 60731170 -> 7600078a`), yet no Steam overlay. Steam's `gameoverlay_renderer.txt`: `IWrapDXGIFactory2::
  CreateSwapChain called`, `VTable for swap chain in unexpected module`, `Tracking new swap chain: 73e3ad0` = CE's
  `CWrapDXGISwapChain` (`returning wrapper 073e3ad0`). D3D11 creates below a foreign chain were always wrapped; the
  DX12 identity rule never covered them. Fix: D3D10/11 rule + resize coverage + DX11 RTV release in reconcile detours.
- Present1 (`606cbd08`), ResizeBuffers (`606ac238`), ResizeBuffers1 (`607327b8`) all `unstable-thread-snapshot` at both
  swapchain events with one strict attempt each; only Present had the 4-attempt rule. Shared rule: deep_hook_retry.h.
  `InstallDeepHookPublished` now clears the last quiesce failure first (a pre-patch refusal reported a stale reason).
- `[D3D11 AF COST wrapper]` second window: srv 714k/s, sampler 692k/s, shader 204k/s binds, 58.8k passes/s,
  workUs 0.07, estMsPerS 121.9 (~0.8 ms/frame at ~150 fps); raw line srv=368/s. Raw bind path takes
  `dx11_hook_g_D3D11ContextStateMutex` + map lookup per bind (`UpdateStageSamplers`). Next: one shared AF engine.

### 2026-10-17 - BioShock Infinite (x86): no Steam overlay = no x86 body hook; DX11 per-frame log; wrapper AF line

- `20261017_000225` (x86, SysWOW64 dxgi 10.0.26100.9711, Steam loaded first): `reserving the Present
  entry for third-party overlays (loaded=1)`, then `DeepHook: Only supported on x64` and `falling back
  to the entry prepend`. Steam hooks DXGI only after the game's swapchain and skips an entry that
  already jumps elsewhere, so it never drew. New `InstallDeepHookImplX86`: the body patch undoes the
  frame-pointer prologue (`pop ebp` at +5, else `mov esp,ebp; pop ebp`) and jumps to the detour;
  Present `8B FF 55 8B EC 83 E4 F8 83 EC 38 A1..` resumes at +8. Non-stack prologue work is refused
  (x86 `SetColorSpace1`: `mov ecx,[ebp+8]` at +5) and keeps the entry. x86 reserved span 6 everywhere.
  Unit tests: `DeepHookX86PolicyTest` (byte rules only; no x86 runtime test). Hardware run pending.
- 92% of that hook log (3.1 of 3.35 MB in 85 s) was `DX11: [frame N] pre-render` + `calling
  RenderOverlay`, two lines per frame; now a ChangeGate on device/context/RTV, every 10000 frames.
- BioShock's AF runs through `CWrapD3D11DeviceContext`, not the raw hooks, so `[D3D11 AF COST]` never
  covered it; `[D3D11 AF COST wrapper]` now does (numbers pending a run).

### 2026-10-16 - HOTS/DXUP rerun: AF memo keyed by sampler; per-frame translation recheck fixed

- `20261016_224237` (0.1.7192): `[D3D11 AF COST]` ~68 ms/s (~0.48 ms/frame at 140 fps), 29% slow
  slots: the per-slot choice reset on each of ~570 sampler rebinds per frame. Choices now live in a
  bounded per-context cache keyed by the referenced sampler object; bind-hook time is sampled too.
- 91% of hook_debug.log (162,867 lines) was one VTableHook self-collision warning, once per frame on
  the game thread: DXUP loaded from the game folder, the game's first D3D11 device+swapchain went
  through CE's export hook and got wrapped, and the wrapper's diagnostic attribution patched DXUP's
  swapchain AddRef/Release for the process. The translation owner could not cover the frontend
  Release, so it re-validated and re-patched every Present. Attribution limited to DXGI's class,
  refused patches remembered per slot value, warning metered. Flow: `FlowTranslationLifetime`.

### 2026-10-16 - HOTS/DXUP overhead audit: memoized raw D3D11 AF, non-pixel binds unhooked

- `20261016_194516` (0.1.7186, limiter 140 active, inject recording): CE's present-path work, limiter,
  overlay and capture all run on DXUP's submission thread, which waited ~5.2 ms/frame in the limiter;
  recording hitches (0.42% over 20 ms) carried normal CE present cost (245 us mean). The game-thread
  raw D3D11 AF hooks were the only CE work on the critical thread and had no cost measurement.
- Per dirty pixel slot the old path took ~5 global-lock round trips, 4-8 runtime calls (GetDevice,
  sampler/view GetDesc, GetPrivateData), AddRef/Release pairs, a config string hash and two linear cache
  scans. Now memoized per tracked sampler and replacement-cache epoch; view classes read back once per
  changed view; VS/GS/HS/DS/CS bind hooks removed (no reader). `[D3D11 AF COST]` added. Details and
  invariants: `api-translation-overrides.md`. Live HOTS cost of the new path pending.

### 2026-10-16 - Per-process VRAM counters double count shared textures; real dxgi_dup recording use ~2.2 GB

- `20261016_192321` [GpuMemory]: processLocal 3990 MB with "unaccounted" 2.1 GB appearing on the first encodes.
  Scratch GPU measurement: a shared texture opened on a second device adds its size again to the per-process
  counters (DXGI and Task Manager per-process) but 0 to the adapter total. Reservoir (capture + encoder device)
  and bridge frame pool (encoder + work device) are counted twice; real use ~2.16 GB = reservoir 1519 + frames
  308 + repeat ~50 + ~290 session/drivers. The breakdown line now subtracts it (`countedTwice`, `physical~`);
  details `wgc-reservoir-vram-design.md` item 10. Open: CE's `[WGC] Video memory` over-budget check still uses
  the per-process counter.

### 2026-10-16 - dxgi_dup P010 route confirmed; [GpuMemory] breakdown added for the remaining VRAM

- `20261016_190423` (0.1.7184, desktop dxgi_dup 4K120 10-bit av1_nvenc): `splitShare=nv12:1,p010:1 ... -> P010
  (yuv)`, pool `retainedFmt=P010 estimatedSmooth=1519MB`, `Encoder reads shared retained slots at keyed mutex
  1->0`, converter `conversions=657 failures=0`. User: VRAM still rises a lot. `processUsage` 14.7 MB -> 949 ->
  2649 -> 3942 -> 4014 MB (flat after ~3 s, no leak); the RGB dxgi_dup run `183018` r0002 peaked at 5038 MB.
- ~2.5 GB is outside the reservoir and unnamed by the logs; added the `[GpuMemory]` per-component line and the
  codec-open delta (`wgc-reservoir-vram-design.md` item 10). Fixed in passing: ForceReset reset the retained-YUV
  and reservation settings. Next run: read `[GpuMemory]` to size encoder frames vs `unaccounted`.

### 2026-10-16 - dxgi_dup reservoir retains P010/NV12 through shared slots (hardware run pending)

- Follow-up to the `183018` gap below: DXGI duplication's dedicated capture device now converts into NV12/P010
  slots created on the encoder's device as shared NT keyed-mutex textures, when both devices report the format
  shareable. Conversion takes no encoder D3D11 lock on that device; the encoder holds the slot's keyed mutex for the
  copy and overlay patch. Failure to create/open/view rebuilds the RGB pool. Details:
  `wgc-reservoir-vram-design.md` item 9. Real-GPU tests on the RTX 5070 pass for P010 and NV12.
- Hardware wanted: dxgi_dup 4K120 10-bit p6 as in `183018` r0002: route line `-> P010`, pool ~1519 MB, the
  `[RetainedYuv] Encoder reads shared retained slots` line, no keyed-mutex failures, smoothness and A/V unchanged.

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
