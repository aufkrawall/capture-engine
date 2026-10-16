# WGC/DXGI reservoir VRAM: design (proposal, not implemented)

Last cross-checked: 2026-10-16 (against source; no hardware run, no code written)
Status: awaiting user approval. Backlog item 4 in `nvenc-encoding.md` ("Starvation and VRAM backlog").

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

HDR today: 47 FP16 surfaces fit the budget, so the retained cap is ~33 and the extra frames ~27 instead of 45, i.e.
HDR 4K120 cannot reach the 300 ms target now (budget math; a hardware log with `capLimited` would confirm). YUV
retention fixes that, a reliability gain rather than a trade.

Also freed on the encoder side: the RGB repeat-source cache used while cursor or camera are dynamic (one full RGB
frame) and the HDR RGB10 intermediate, because the retained slot itself is the repeat source.

## Lever 1 design

1. **Route decision, fixed at pool build.** New `ResolveRetainedFrameRoute` (`video_format_policy.h`): YUV when the
   encoder consumes NV12/P010, there is no output scaling, the frame size is even, the encoder is not on the direct
   RGB path, and (split-device only) the device supports keyed-mutex sharing of planar formats
   (`D3D11_FEATURE_D3D11_OPTIONS` `ExtendedResourceSharing`, plus a create/open probe). Otherwise the current RGB
   retention stays, with a logged reason. Scaled output stays RGB until a shader scaler exists (item 2): retaining
   at output size would draw the cursor at output resolution, which changes its look.
2. **One set of conversion parameters.** The encoder resolves an immutable `YuvConversionParams` (format, range,
   matrix, transfer: sRGB, scRGB->PQ or HDR->SDR tone map, SDR white, sharpen strength) and publishes it through the
   media engine API before the pool is built. The pool converts with exactly these parameters, using the same HLSL
   (`video_color_conversion_shader.h`). Each submission carries a params hash; a mismatch is a session fault
   (logged, handled like a format change through the existing retarget/reinit), never a silent reinterpretation.
   A live SDR-white change applies at ingress, so to content time instead of output time.
3. **Ingress conversion.** `RenderFrameToPoolSlot` writes Y and UV plane RTVs of a P010/NV12 slot instead of one
   R10/FP16 RTV. Same thread, device, keyed-mutex and lease handling as the retained copy today. Unmodified frames
   then reach the encoder with the same Y'CbCr codes as today's encode-time conversion (bit-identical where the source
   was already R10/BGRA8; FP16 sources skip the intermediate R10 quantization, so they may differ by 1 LSB, which
   is a precision gain).
4. **Output.** For a fresh or repeated slot the encoder takes a pool frame, `CopyResource`s the retained slot into it
   (24.9 MB at 4K P010, well under 0.1 ms) and applies overlay patches to the copy. The retained slot is never
   written after ingress, so repeats with a moving cursor need no backup/restore, and the lease still ends at the
   copy in command order (the current contract). The bridge gate, the fence and the NVENC depth are unchanged.
5. **Overlay patches in YUV.** Cursor and face camera keep their timing (cursor from the resolver at
   `scheduledQpc - contentDelay`, camera as today) and their order (camera, then cursor). A patch shader covers the
   overlay rectangle extended by the chroma footprint:
   - SDR (blend in coded R'G'B', as today): Y'CbCr is an affine function of R'G'B', so the blended luma is
     `a*Y_overlay + (1-a)*Y_bg`, exact up to one extra rounding (+-1 LSB) at partial-alpha pixels. Chroma is exact
     wherever alpha is constant over the footprint (opaque interior, untouched area, uniform opacity) and approximated
     only where alpha varies within a 3x2 footprint, from the stored background chroma.
   - HDR (blend in linear light, as today): reconstruct R'G'B' from Y and bilinear-upsampled chroma, apply the PQ
     EOTF, blend, re-encode. Opaque and untouched pixels are exact; only partial-alpha edge pixels depend on the
     upsampled background chroma.
   - Pixels outside the overlay footprint stay bit-identical. Sharpening (if configured) is not re-applied inside the
     patch: the cursor no longer gets sharpening halos. This is the one visible behaviour difference besides chroma
     at overlay edges, and it is arguably more correct.
6. **Other users.** The privacy blackout fills Y/UV black codes directly. Encoder-overload repeat degradation stays,
   but a dynamic repeat now costs one copy plus small patches instead of a full RGB-to-YUV pass, so it should engage
   less often.
7. **Budget.** `ComputeWgcSmoothnessSurfaceBudget` gets bytes per surface for planar formats (`w*h*3/2*sample`)
   instead of integer bytes per pixel. Slot counts are unchanged; only bytes drop.
8. **Diagnostics.** One-time `[WGC] Retained route=yuv|rgb format=... reason=... slotMiB=... poolMiB=...` and
   `[RetainedYuv] params hash=... range=... transfer=...`; rate-limited `[RetainedYuv] patch cursor|camera ...` with
   counts and rect sizes; existing `estimatedPool`/`PoolLease` fields report the new bytes.

### Why not use the retained slots as encoder pool frames (zero copy)

- Overlays: a slot that repeats with a moving cursor would need in-place drawing while NVENC may still read it (up
  to 12 frames in flight), so it needs a separate surface anyway.
- Lifetime: the lease would have to last until NVENC completes, so the reservoir needs ~12 more slots, which cancels
  the 13 pool textures it would save.
- FFmpeg's NVENC wrapper registers at most `MAX_REGISTERED_FRAMES=64` resources; 64 retained slots plus overlay
  frames would churn `NvEncRegisterResource`.
- The copy it saves costs well under 0.1 ms of GPU time per frame.

### GPU cost

Today: one full-frame pass per admitted source frame (retained copy), plus one RGB-to-YUV pass per fresh output frame,
plus repeat-cache copies or recompositions. Lever 1: one RGB-to-YUV pass per admitted source frame (it replaces the
retained copy and costs about the same), plus a P010/NV12 copy per output and small patches. Frames admitted but
never selected (at most the 1.25x metering surplus) are converted for nothing, which the retained copy already does
today.

### Risks (lever 1)

- Parameter drift between ingress and encoder (mitigated by the single params object and the per-frame hash).
- Fidelity at partial-alpha overlay edges (bounded by tests below; interiors and untouched pixels exact).
- Planar keyed-mutex sharing on the split-device path (capability probe, RGB fallback).
- Two retained routes to maintain (YUV and RGB for scaled, odd-size and direct-RGB cases). Both stay covered by
  tests.
- AMF/QSV/MF pool frames: the copy must target their pool layout (array slices: `CopySubresourceRegion`). Gate the
  route to `*_nvenc` first if those encoders cannot be tested.

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

## Found in passing (not part of this design)

The face camera uses the newest camera frame at encode time, but the screen content and microphone audio of that
slot are from `scheduledQpc - contentDelay`. With the 300 ms reservoir the camera picture should therefore lead the
microphone by about the content delay minus the webcam's own latency (from code; not measured). A fix would select
from a short timestamped camera ring (~D x camera fps frames, ~40 MB at 720p) at the slot's content time. This is
independent of either lever.

## Files lever 1 touches

- `common/capture/capture_policy/smoothness_budget.h` (planar surface bytes), tests in
  `tests/test_capture_pipeline_policy_part2..4.cpp`.
- `mediaengine/video/video_format_policy.h` (`ResolveRetainedFrameRoute`, `YuvConversionParams`),
  `tests/test_video_format_policy.cpp`.
- `mediaengine/video/video_color_conversion_shader.h` (shared plane shaders; YUV patch shaders).
- New `mediaengine/video/retained_yuv_converter.{h,cpp}` (ingress converter: shaders, CB, plane RTV cache).
- `mediaengine/engine/mediaengine.{h,cpp}` (publish params; `D3D11FrameSubmissionDesc` route flag + params hash).
- `captureengine/media/wgc_capture_pool.cpp`, `wgc_capture_pool_budget.cpp`, `wgc_capture_internal.h`,
  `dxgi_dup_capture.cpp` (planar slots, plane RTVs, prewarm hint).
- `mediaengine/video/video_encoder_encode.cpp` or a new `video_encoder_retained_yuv.cpp` (copy + patches),
  `video_encoder_textures.cpp` (repeat source = retained slot), `cursor_renderer.cpp`,
  `face_camera_renderer.cpp`, `video_encoder_face_camera.cpp` (YUV patch variants), privacy black fill.
- Unchanged: `media_main_encoder_05b_loop_wgc_select_uniform.cpp` and all CFR selection/timing code.

## Validation plan

Automated (GPU tests on the real adapter, like `EncoderDeviceBridgeTest`):
- Budget: 4K120 P010 = 64 slots / 1519 MiB; HDR not `capLimited`; slot counts equal to today for every case.
- Route matrix: YUV vs RGB per format, bit depth, scaling, odd size, direct RGB, split device without planar sharing.
- Conversion equivalence: for R10, BGRA8, SDR FP16 and HDR FP16 inputs, ingress YUV equals today's encode-time
  conversion (bit-identical for R10/BGRA8, <= 1 LSB for FP16).
- Overlay patch vs reference (RGB composite, then convert): pixels outside the footprint bit-identical; SDR luma
  <= 1 LSB everywhere; chroma and HDR partial-alpha pixels within a stated bound on high-contrast, high-chroma
  backgrounds; opaque interiors exact.
- Repeat immutability: hash the retained slot before and after several repeats with a moving cursor and camera.
- Lease: the slot returns to the pool after the copy, never later.

Hardware (user-driven):
- dxgi_dup 4K120 10-bit p6 (the `015331` r0002 setup): `estimatedPool` ~1519 MiB vs 2025, `PoolLease` maxLeased/
  minFree as before, smoothness summary (`excessRepeats`, dup%) and `maxAudioDelta` unchanged, encoder GPU stages no
  slower.
- WGC 4K 10-bit SDR under GPU contention (VRR, a Strange Brigade/Fortnite class game): same smoothness verdict and
  realized delay as before.
- WGC and dxgi_dup HDR 4K120: `capLimited=0` and the full 300 ms realized.
- 8-bit NV12 run; static desktop with a moving cursor (cursor smooth through repeats); face camera on; scaled output
  (stays RGB and logs the reason).
- Visual check of cursor edges and shadow over saturated, high-contrast backgrounds (zoomed side by side).
