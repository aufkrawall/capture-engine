# WGC/DXGI reservoir VRAM: retained NV12/P010

Last cross-checked: 2026-10-17 (face-camera history growth lifetime; earlier hardware boundaries below)
Status: lever 1 implemented (`wgc_retained_yuv=true`, default), hardware run pending. Backlog item 4 in
`nvenc-encoding.md` ("Starvation and VRAM backlog").

## Goal and constraints

Cut the VRAM of the retained WGC/DXGI-duplication smoothness reservoir while keeping the fixed 300 ms smoothness delay
(`wgc_smoothness_buffer_max_ms`), the frame count it implies, the CFR selection, the A/V anchors and every
smoothness/reliability property. The 300 ms absorbs 170-200 ms WGC delivery gaps under GPU-bound VRR
(`wgc-capture.md`, sessions `20260625_180512`, `20260626_050554`); the user does not want it reduced.

## Recommendation

Implement lever 1: store retained frames already converted to the encoder's YUV format (P010 for 10-bit/HDR, NV12
for 8-bit), copy the selected slot into an encoder pool frame at output time, and composite cursor and face camera
into that copy in YUV. Do not use the retained slots themselves as encoder pool frames, and do not implement lever 2
(holding the delay as encoded packets). Both rejections are argued below.

Lever 1 changes only the storage format and where the RGB-to-YUV pass runs. The reservoir depth, slot count,
ingress metering, nearest-target playout, repeat decisions, leases and A/V contract stay the same, so smoothness and
sync are unchanged by construction. GPU work also drops (see "GPU cost").

## Source facts this rests on

- Retained format: `GetRetainedPoolFormat` (`wgc_capture_pool.cpp`) gives R10 only for SDR FP16 sources
  (`ShouldApplySdrLinearToSrgbBeforeRgb10`). HDR keeps FP16 (8 B/px); 8-bit keeps BGRA8. Every frame already goes
  through one full-frame shader pass at ingress (`RenderFrameToPoolSlot`: FP16->R10, or the canonical dup copy).
- Budget: `ComputeWgcSmoothnessSurfaceBudget` (`smoothness_budget.h`) takes integer bytes per pixel, caps slots at
  `kWgcSmoothnessBufferMaxPoolFrames=64`, targets `300 ms x 1.25 x fps` retained frames (45 at 120 fps) plus 6
  reserved and 8 headroom slots. Compact WGC source buffers: `max(8, (fps+14)/15)` capped at 12, so 8 at 120 fps.
- Cursor: composited per output slot, including repeats, from the shared resolver at `scheduledQpc - contentDelay`
  (`wgc-capture.md`, cursor section). DXGI pointer-only updates create no video frame, so on a static desktop every
  output is a repeat and the cursor moves only through this per-slot composition.
- Face camera: one draw per output frame before conversion; repeats redraw it (`face-camera-overlay.md`). The frame
  is `FaceCameraCapture::LatestFrame()`, with no timestamp (see "Found in passing").
- Encoder input: `ConvertBGRAtoNV12` writes an `av_hwframe_get_buffer` frame (bridge pool texture, written through
  its work-device alias, `EncoderFrameTexture`). The lease contract is "copied before `MediaEngine_ProcessFrameD3D11`
  returns" (`wgc-capture.md`, lease paragraph).
- Chroma: SDR 4:2:0 uses left siting, a 3x2 luma footprint (`PS_YuvUV`); HDR P010 a 3x3 1-2-1 footprint
  (`PS_P010UV`). Optional luma sharpening reads the 4 neighbours (`PS_P010Y`).

## VRAM at 3840x2160, 120 fps, default 3000 MB budget (computed from the budget code)

| Case | Today | Retained YUV |
|---|---|---|
| dxgi_dup 10-bit SDR | 64 x R10 = 2025 MiB | 64 x P010 = 1519 MiB (-25%) |
| dxgi_dup 8-bit SDR | 64 x BGRA8 = 2025 MiB | 64 x NV12 = 759 MiB (-62%) |
| WGC 10-bit SDR | 8 FP16 source (506) + 64 R10 = 2531 MiB | 506 + 1519 = 2025 MiB |
| WGC 8-bit SDR | 8 BGRA8 source (253) + 64 BGRA8 = 2278 MiB | 253 + 759 = 1012 MiB |
| WGC HDR | 8 FP16 + 39 FP16 = 2974 MiB, `capLimited` | 506 + 1519 = 2025 MiB, full depth |
| dxgi_dup HDR | 47 x FP16 = 2974 MiB, `capLimited` | 1519 MiB, full depth |

dxgi_dup rows: duplication always runs on a dedicated capture device (`wgc_capture_duplication.cpp`). The first
implementation kept every split-device pool RGB, so `20261016_183018` r0002 logged `sameDevice=0 -> split capture
device` and a 2025 MB R10 pool; the split-device route (item 9) makes these rows reachable.

HDR today: 47 FP16 surfaces fit the budget, so the retained cap is ~33 and the extra frames ~27 instead of 45, i.e.
HDR 4K120 cannot reach the 300 ms target now (budget math; a hardware log with `capLimited` would confirm). YUV
retention fixes that, a reliability gain rather than a trade.

Also freed on the encoder side: the RGB repeat-source cache used while cursor or camera are dynamic (one full RGB
frame) and the HDR RGB10 intermediate, because the retained slot itself is the repeat source.

## Lever 1 as implemented (2026-10-16)

1. **Route, decided by the encoder once per pool build.** `MediaEngine_QueryRetainedFrameRoute` (loader entry, export
   in `mediaengine_retained_frame.cpp`) runs `VideoEncoder::ResolveRetainedFrameRoute`
   (`video_encoder_retained_yuv.cpp`), which evaluates the pure `DecideRetainedFrameRoute` (`retained_frame_route.h`)
   with the encoder's own config, `ResolveVideoFormat` and the 10-bit decision it will make for this source. YUV
   only when: `wgc_retained_yuv=true` (default), same-device capture or a split capture device that shares the
   chosen planar format (item 9), no output scaling, even size, encoder geometry not locked to another size, a
   resolved 4:2:0 D3D11 format, and either SDR (BGRA8/RGBA8/R10/FP16 source) or HDR output from an FP16 scRGB
   source. Kept RGB: split devices without planar sharing (`split capture device cannot share P010|NV12`), scaling
   (VideoProcessor scaler), HDR-to-SDR tone map (live SDR white), odd sizes, PQ RGB10 HDR sources. Each decision logs `[RetainedYuv] Route query` and
   `[WGC] Retained reservoir route: ... -> NV12|P010|RGB (reason) bitsPerPixel=a->b`. The capture side caches the
   route per (format, size, HDR, device); the query waits for the media engine API lock, so it never runs with the
   D3D11 lock held.
2. **Ingress conversion = the encoder's own passes.** The encoder's normalize and RGB->NV12/P010 draws moved into
   `rgb_yuv_draw.cpp`; `VideoEncoder` and `RetainedYuvConverter` both issue them (same HLSL, entry points, samplers,
   constants). SDR FP16 goes through `PS_Main` linear-to-sRGB into an RGB10 intermediate first (the old pool pass had
   the same function), HDR FP16 through `PS_Main` scRGB-to-PQ into RGB10, then the plane shaders. So an
   overlay-free frame reaches NVENC with the codes the RGB route produced. The converter is called from the capture
   thread through `MediaEngine_ConvertToRetainedFrame`, which takes the D3D11 lock and its own mutex, never the
   engine API or mux lock (an encode holds those). Lock order: D3D11 lock, then converter mutex.
3. **Pool.** `EnsureTexturePool` refreshes the route; NV12/P010 slots get two plane RTVs each
   (`CreateRetainedYuvPlaneViews`); `CopyFrameToPool` converts through `ConvertFrameToRetainedYuvSlot` (source SRV or
   the existing staging copy). Budget in bits per pixel (`ComputeWgcSmoothnessSurfaceBudgetForBits`). Slot counts,
   leases, admission and every CFR decision are unchanged.
4. **Encoder input.** `ConvertBGRAtoNV12` dispatches NV12/P010 inputs to `ConvertRetainedYuvFrame`: one
   `CopySubresourceRegion` into the AVHWFrame (bridge alias), then overlays. `PrepareFrameD3D11` treats P010 as
   10-bit input. Repeats reuse the existing caches (static repeats copy the converted frame; dynamic-overlay repeats
   cache the uncomposited NV12/P010 slot copy instead of an RGB frame and re-run the same path).
5. **Overlays in YUV (`YuvOverlayPatcher`, `yuv_overlay_patch_shader.h`).** Camera and cursor are drawn alone into
   small grow-only R32G32B32A32 layers by their unchanged renderers (new viewport-origin argument), in the domain the
   RGB route blended them in (sRGB for SDR, linear scRGB with SDR white for HDR). The patch repeats the RGB blend per
   pixel at the old surface's precision (UNORM 8/10-bit, FP16 for HDR, then the RGB10 PQ intermediate), camera
   first, cursor topmost, and converts with the encoder's forward functions. Untouched pixels and chroma samples are
   discarded (bit-identical). Partially covered pixels reconstruct the background from exact luma plus chroma
   bilinearly upsampled at the format's siting, shifted per chroma footprint so the footprint sums to the stored
   sample; a fully covered sample therefore equals the encoder's own average exactly. Measured against
   "blend in RGB, then convert" (`RetainedYuvGpuTest`, real adapter): untouched exact; opaque luma and covered chroma
   <= 1 code (float rounding); over smooth backgrounds every touched pixel <= 1 code, SDR and HDR; with saturated
   chroma detail finer than the chroma grid under a translucent edge, chroma up to 57 (10-bit) / 16 (8-bit) codes
   and HDR luma up to 6. That residual is information 4:2:0 storage does not keep.
6. **Privacy blackout.** `BlackFrameTextureCache` clears NV12/P010 references to video black (limited Y 16/64, full
   Y 0, neutral chroma); the range comes from `[Video] color_range`. Without this the blackout would have failed
   closed and stopped the recording.
7. **Same-device context lock (pre-existing hazard, fixed).** Same-device WGC/DXGI capture drew its retained copy on
   the encoder's immediate context from the WinRT/duplication thread while the encoder thread drew its conversion,
   with only per-call multithread protection: one thread's `OMSetRenderTargets` could land between the other's
   state setup and draw. The pool's slot write now holds `MediaEngine_LockD3D11` (same device only), and the
   encoder's conversion and dynamic-repeat population hold `D3D11ScopedLock`. Not seen in a log; found in review.
8. **Logs.** `[RetainedYuv] Route query ...`, `[WGC] Retained reservoir route ...`, `[WGC] Texture pool created ...
   retainedFmt=P010 retainedRoute=...`, `[RetainedYuv] Ingress conversion shaders created`, `[RetainedYuv] Encoder
   consumes retained P010 ...`, every 30 s `[RetainedYuv] frames=... overlayPatched=... patchFailures=...`, failures
   rate-limited (`Ingress conversion failed`, `Overlay patch failed`, `does not match the encoder input`), and the
   converter/encoder summaries at release.
9. **Split capture device (DXGI duplication, 2026-10-16).** `WGCCapture::Impl::ProbeSplitYuvSharing` asks both
   devices (`wgc_shared_planar.h`: `D3D11_FORMAT_SUPPORT2_SHAREABLE`, plus `ExtendedNV12SharedTextureSupported`
   for NV12) and passes `splitShareNv12/P010` in the route query. NV12/P010 slots are created on the encoder's
   device with `SHARED_KEYEDMUTEX | SHARED_NTHANDLE` (RGB slots keep the legacy handle) and opened on the capture
   device with `OpenSharedResource1` (`OpenSplitYuvPoolSlot`); plane RTVs are on that alias. The capture thread
   converts on its own context under keyed mutex 0 and publishes at key 1 like RGB slots. The ingress export takes
   the MediaEngine D3D11 lock only when the views are on the shared device, so duplication never waits for an
   encode. `ConvertRetainedYuvFrame` acquires the slot's keyed mutex at the caller's key around the copy and the
   overlay patch and returns it to 0 (repeat staging then owns key 0, as for RGB). A create/open/view failure logs
   `Shared retained-YUV pool unavailable (...)`, sets `retainedSplitShareFailed_` and rebuilds the pool RGB for
   that capture. New logs: `splitShare=nv12:%d,p010:%d` in both route lines, `[RetainedYuv] Encoder reads shared
   retained slots at keyed mutex 1->0 (split capture device)`. Verified on the RTX 5070
   (`RetainedYuvApiTest.SplitCaptureDevice*`): the probe matches create/open for both formats, and the export on a
   second device writes P010 940/512 and NV12 235/128 read on the encoder device after the handoff.
10. **Where the rest of the process VRAM goes (`[GpuMemory]`, 2026-10-16).** `20261016_190423` (dxgi_dup 4K120
   10-bit, P010 route): pool 1519 MB, but `processUsage` rose to 4014 MB within ~3 s and stayed (the RGB run
   `183018` r0002 peaked at 5038 MB). The encoder now logs `[GpuMemory] stage=... processLocal nonLocal budget |
   captureReservoir (slots) captureOther | encoderFrames (frames) repeat conversion overlays | accounted
   unaccounted codecOpenDelta peakLocal` (`video_encoder_gpu_memory.cpp`, `gpu_memory_breakdown.h`), on 64 MB moves,
   at least once a minute, and at stop with the peak; plus `[GpuMemory] Codec open: <encoder> processLocal a -> b`.
   The capture side reports its pool through `MediaEngine_ReportCaptureGpuMemory` (recording capture only,
   `SetReportsGpuMemory`). `unaccounted` = encoder session buffers, per-device driver allocations, duplication
   surfaces. Estimates ignore pitch/alignment. Found in passing: `WGCCapture::ForceReset` dropped
   `retainedYuvEnabled_` and the reservation mode (RGB reservoir after any capture recovery); now carried.
   **Per-process counters count shared textures per device** (measured on the RTX 5070, 13 shared 4K P010
   textures opened on a second device and drawn there): DXGI `QueryVideoMemoryInfo` +311 MB, PDH `GPU Process
   Memory(pid_*)\Dedicated Usage` (Task Manager per-process) +311 MB, PDH `GPU Adapter Memory(*)\Dedicated Usage`
   (Task Manager total) +0 MB. So the split reservoir and the bridge's frame pool appear twice in every
   per-process number, including the `[WGC] Video memory ... processUsage` line; the line now reports
   `countedTwice` and `physical~`. `20261016_192321` (first [GpuMemory] run, dxgi_dup 4K120 10-bit av1_nvenc p5):
   processLocal 3990 MB = reservoir 1519 x2 + encoder frames 308 (13) x2 + repeat 47 + ~290 MB encoder
   session/drivers/duplication; nonLocal 339 MB (13 NVENC bitstream buffers, ~26 MB each); codec open +1 MB (the
   session commits lazily on the first encodes). Real dedicated use ~2.16 GB; the RGB build was ~2.7 GB
   (5038 - 2025 - 308). Nothing large is avoidable without a shorter reservoir or a shallower NVENC pipeline.

### Why not use the retained slots as encoder pool frames (zero copy)

- Overlays: a slot that repeats with a moving cursor would need in-place drawing while NVENC may still read it (up
  to 12 frames in flight), so it needs a separate surface anyway.
- Lifetime: the lease would have to last until NVENC completes, so the reservoir needs ~12 more slots, which cancels
  the 13 pool textures it would save.
- FFmpeg's NVENC wrapper registers at most `MAX_REGISTERED_FRAMES=64` resources; 64 retained slots plus overlay
  frames would churn `NvEncRegisterResource`.
- The copy it saves costs well under 0.1 ms of GPU time per frame.

### GPU cost

Before: one full-frame pass per admitted source frame (retained copy), plus one RGB-to-YUV pass per fresh output
frame, plus repeat-cache copies or recompositions. Now: the RGB-to-YUV pass per admitted source frame (plus the RGB10
normalize for FP16 sources, as before), a NV12/P010 copy per output, and small patches. Frames admitted but never
selected (at most the 1.25x metering surplus) are converted for nothing, as the retained copy already was.

### Remaining risks

- Partial-alpha overlay edges over high-frequency saturated chroma (bounded above).
- Two retained routes to maintain; the RGB route stays the fallback for every excluded case.
- AMF/QSV/MF consume the same NV12/P010 pool frames through the same copy; only NVENC has been exercised so far.
- `ConvertRetainedYuvFrame` itself has no unit test (it needs a live `VideoEncoder`); its parts (converter, patch,
  route, black frame, export) do.

## Lever 2: delay held as encoded packets (rejected)

Design examined: decide each CFR slot when its successor frame arrives (WGC and duplication deliver in timestamp
order, so the nearest-target choice equals today's), encode at once, and let compressed packets (a few MB for
300 ms) wait for audio in the muxer. Repeats are decided when the next frame proves a gap, or at a wall deadline on a
static desktop. PTS stay grid-assigned, so A/V sync holds if every slot gets exactly one packet in order. Cursor and
camera are composited at decision time; the cursor history already covers it.

Why it is rejected:

- **It moves the reservoir's job onto the encoder queue.** Today the reservoir feeds NVENC at a uniform output rate
  even through a 200 ms delivery gap. With decide-on-arrival, NVENC idles during the gap and then gets the whole gap
  (`G x fps` encodes, about 24 at 120 fps) at once. At p6 4K120 NVENC runs at 98% (`015331`): the burst drains at
  ~2% spare capacity, ~50 G (about 10 s for G=200 ms), and the idle time during the gap is lost capacity. Bursty
  arrivals at that utilization grow the queue without bound (queueing theory: wait ~ rho/(1-rho)), which ends in
  encoder-limited drops. That is a smoothness regression where today's design holds.
- **The VRAM does not go away.** Queued fresh frames must stay raw until encoded, so the worst-case raw backlog is
  about one gap of frames, and the pool must be prewarmed for that worst case (lazy growth under game VRAM pressure
  is not reliable). The variant that keeps NVENC paced (a raw jitter depth J >= the worst delivery gap, packets for the
  rest) saves at most D-J, about a third, since the 170-200 ms gaps are what set D=300 ms.
- **Redesign scope.** The encoder loop is tick-driven (grid vs wall); encoder-limited mode, the overload repeat pacer,
  grid-debt holds, catch-up, live recovery and the stop drain all assume selection at encode time. Every one would
  need a new decision-time vs encode-time form and fresh hardware validation.

It could be revisited only as an addition to lever 1 if measurements showed large, stable NVENC headroom, which
the p6 runs contradict.

## Face camera at the slot's content time (fixed with this work)

The encoder used to draw `FaceCameraCapture::LatestFrame()`, while the slot's screen content and microphone audio
are from `scheduledQpc - contentDelay`, so with the 300 ms reservoir the camera led the microphone by about that
much. Now: the capture loop stores each slot's content time on the cursor state it already selects per slot
(`CaptureState::contentQpc`, also without cursor capture); camera frames carry a capture QPC (the Media Foundation
sample time when plausible, else arrival; `ResolveCaptureQpc`); the renderer uploads each camera frame into a GPU
history ring (4 frames, growing on demand up to 48 when the content lag exceeds it) and shows the newest frame taken
at or before the slot time (`face_camera_history.h`). Submissions without slot timing fall back to the frame's own
time, then the newest frame. Logs: `[FaceCamera] Frame time basis: ...`, `Camera history grown to N frames for a
content lag of X ms`, and `slotLagUs=avg/min/max timed=... olderThanHistory=...` in the periodic composite line.
Inject capture uses the scheduled time, as the cursor does. Details: `face-camera-overlay.md`.

History growth must not retain a reference to a vector entry across `resize`: the renderer snapshots the
selected slot by value before growing, including its timestamp for lag accounting. The original reference
caused an ASan-confirmed heap-use-after-free at a normal 4-to-11-slot growth (300 ms, 30 fps).
`FaceCameraRendererHistoryTest` seeds the real renderer with WARP textures and checks selection and lag across
that reallocation and the following update. The existing growth and lag log lines cover this transition.

## Files

- Policy: `common/capture/capture_policy/smoothness_budget.h`, `mediaengine/video/retained_frame_route.h`,
  `mediaengine/video/face_camera_history.h`.
- Media engine: `rgb_yuv_draw.*`, `retained_yuv_converter.*`, `runtime_shader_compiler.*`, `yuv_overlay_patch*`,
  `video_encoder_retained_yuv.cpp`, `retained_yuv_overlays.h`, `overlay_source_restore.h`,
  `engine/mediaengine_retained_frame.cpp`, `engine/retained_frame_desc.h`, cursor/face-camera renderers.
- Capture: `wgc_capture_retained_yuv.cpp`, `wgc_capture_pool*.cpp`, `app/mediaengine_loader.*`,
  `common/capture/screen_grab_privacy.*`, `[WGC] wgc_retained_yuv` in `common/config`.
- Tests: `test_wgc_retained_yuv_budget.cpp`, `test_retained_frame_route.cpp`, `test_retained_yuv_gpu.cpp`,
  `test_retained_yuv_api.cpp`, `test_face_camera_history.cpp`, config/source tests.

## Validation plan

Automated, done (`RetainedYuv*`, `RetainedFrameRouteTest`, `WgcRetainedYuvBudgetTest`, `FaceCameraHistoryTest`;
GPU tests run on the real adapter):
- Budget: 4K120 P010 keeps 64 slots at 1519 MiB; HDR FP16 retention is `capLimited`, P010 is not.
- Route matrix: every exclusion with its reason; levels and transforms per source format.
- Ingress conversion: identical passes by construction (shared `rgb_yuv_draw`); the export path converts an R10
  white frame to P010 Y 940 / C 512; the RGB route is refused.
- Overlay patch vs "blend in RGB, then convert": numbers in item 5 above, for SDR P010 (R10), SDR NV12 (BGRA8,
  BT.2020 full range), a translucent camera, and HDR P010 (FP16 scRGB).
- Privacy black frame for NV12/P010 in both ranges; config default/override of `wgc_retained_yuv`.
- Split capture device: the route accepts it only with sharing of the chosen format; the shared slot round trip
  on two devices (item 9).
- Not automated: `ConvertRetainedYuvFrame` in a live `VideoEncoder` (repeat immutability, lease timing); covered
  by construction (the slot is only read, the lease contract is unchanged) and by the hardware runs below.

Hardware (user-driven, pending; A/B with `wgc_retained_yuv=false`):
- dxgi_dup 4K120 10-bit p6 (the `015331` / `183018` r0002 setup): `Retained reservoir route: ... splitShare=
  nv12:1,p010:1 ... -> P010 (yuv)`, `estimatedPool` ~1519 MiB vs 2025, `PoolLease` maxLeased/
  minFree as before, smoothness summary (`excessRepeats`, dup%) and `maxAudioDelta` unchanged, encoder GPU stages no
  slower.
- WGC 4K 10-bit SDR under GPU contention (VRR, a Strange Brigade/Fortnite class game): same smoothness verdict and
  realized delay as before.
- WGC and dxgi_dup HDR 4K120: `capLimited=0` and the full 300 ms realized.
- 8-bit NV12 run; static desktop with a moving cursor (cursor smooth through repeats); face camera on; scaled output
  (stays RGB and logs the reason).
- Visual check of cursor edges and shadow over saturated, high-contrast backgrounds (zoomed side by side).
