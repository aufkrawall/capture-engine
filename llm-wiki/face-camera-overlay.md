# Face-camera overlay

Last verified: 2026-10-26 (native YUV code + WARP tests; webcam hardware validation pending)

## Summary

`[FaceCamera]` adds an optional USB/webcam picture-in-picture layer to both inject and WGC/DXGI recordings. Camera acquisition is isolated on a below-normal-priority Media Foundation worker that publishes only its newest immutable frame, so camera startup, a slow device, disconnects, and camera-frame backlog never block game capture or the CFR output clock. The renderer uploads each new camera frame into a small GPU history and shows, for every output slot, the frame taken at or before that slot's content time, so the camera stays in sync with the delayed screen content and the microphone (see "Timing").

Implemented 2026-10-26 (`gpu-api-plan.md`, near-term item 2): `FaceCameraCapture` runs Media Foundation's SourceReader with `MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING = FALSE` so the D3D11 VideoProcessor MFT is never inserted. Uncompressed camera modes (`NV12`, `YUY2`, `RGB32`, `ARGB32`) pass through unchanged, MJPEG and H.264 cameras decode to `NV12` or `YUY2`, and `FaceCameraRenderer` samples `NV12` (`R8_UNORM` + `R8G8_UNORM` plane views) or `YUY2` (packed 4:2:2 macropixels) and converts to sRGB inside its single composite draw using the camera's colour matrix (`MF_MT_YUV_MATRIX`) and nominal range (`MF_MT_VIDEO_NOMINAL_RANGE`).

The compositor is D3D11-native. A new camera frame is copied or uploaded to the encoder GPU once at camera cadence, optional mipmaps are regenerated at that cadence for RGB32 frames, and each encoded output frame uses one antialiased shader draw before the RGB-to-NV12/P010 conversion (or into the retained-YUV overlay layer). Cursor composition follows the camera draw and therefore remains visually topmost.

Primary sources:

- `common/config/face_camera_config.h`
- `common/capture/capture_policy/cfr_overload_recovery.h`
- `common/config/config_load_face_camera.cpp`
- `mediaengine/video/face_camera_format.h`
- `mediaengine/video/face_camera_capture.{h,cpp}`
- `mediaengine/video/face_camera_renderer.{h,cpp}`
- `mediaengine/video/face_camera_shader.h`
- `mediaengine/video/video_encoder_face_camera.cpp`
- `mediaengine/video/video_encoder_convert_bgra.cpp`
- `mediaengine/video/video_encoder_textures.cpp`
- `mediaengine/video/video_encoder_framegrab.cpp`
- `mediaengine/video/video_encoder_encode.cpp`
- `captureengine/config.ini.template`
- `tests/test_face_camera_config.cpp`
- `tests/test_face_camera_source.cpp`
- `tests/test_face_camera_renderer_history.cpp`

## Configuration

The feature is disabled by default. Existing configs are not rewritten, so users opt in by adding `[FaceCamera]`.
Camera microphones remain ordinary `[Microphone]` audio sources and are not implicitly enabled by this video layer.

| Key | Meaning |
| --- | --- |
| `enabled` | Starts camera capture only while an encoder session is active. |
| `device` | `default`, an exact Windows friendly name, or a symbolic-link ID. |
| `resolution` / `fps` | Desired native mode (`WxH`, or `auto`) and cadence. The closest usable mode is selected; defaults are 1280x720 at 30 fps. |
| `position` | Nine anchors or `custom`; custom `x_percent` / `y_percent` describe travel between the margin-constrained edges. |
| `width_percent` / `margin_percent` | Output-space geometry: width is relative to output width and margin to the shorter output edge. Layout is mapped back to the source before output scaling, so the final placement remains stable. |
| `shape` | `rectangle`, `rounded`, or `circle`. Circle uses a centered square source crop. |
| `corner_radius_percent` | Rounded-rectangle radius relative to the shorter overlay edge. |
| `crop` | `fill` preserves aspect ratio with a centered crop; `stretch` shows the complete camera image. |
| `mirror` / `opacity_percent` | Horizontal preview mirroring and layer opacity. |
| `border_width_percent` / `border_color` | Analytic antialiased border, with `#RRGGBB` color. |
| `stale_timeout_ms` | Hides a frozen frame without affecting the recording; `0` retains the last frame indefinitely. |

Every key follows the normal `ConfigReader` override contract, for example `FaceCamera.position=top_left` in a process-backed `[Profile.*]` section.

## Capture and device ownership

`FaceCameraCapture` dynamically resolves Media Foundation device enumeration and SourceReader creation from the Windows system DLLs. It asks the SourceReader for hardware transforms (`MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS = TRUE`), disables advanced video processing (`MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING = FALSE`) so Media Foundation never inserts the D3D11 VideoProcessor MFT, enables low latency, and passes the encoder's `IMFDXGIDeviceManager`. D3D allocation is optional (`MF_READWRITE_D3D_OPTIONAL = TRUE`) so drivers that cannot provide compatible surfaces fall back to system-memory frames; do not enable the limited `MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING` path alongside the D3D manager because that path is software-only and contractually incompatible with it.

- `ConfigureCameraOutput` (`CandidateOutputSubtypes` in `face_camera_format.h`) tries the camera's chosen native subtype first when it is already uncompressed (`NV12`, `YUY2`, `RGB32`, `ARGB32`), then `NV12`, `YUY2`, `RGB32`, `ARGB32` so compressed modes (`MJPG`, `H264`) use their decoder's native `NV12` or `YUY2` output. It resolves the YUV matrix (`MF_MT_YUV_MATRIX`, defaulting to BT.709 for HD `width >= 1280 || height > 576` and BT.601 for SD) and nominal range (`MF_MT_VIDEO_NOMINAL_RANGE`, defaulting to studio limited range `16..235` unless `MFNominalRange_0_255` is declared).
- If the driver supplies a compatible `NV12`, `YUY2` or `BGRA/BGRX/RGBA` DXGI buffer on that device, the immutable `IMFSample` retains the GPU resource until the encoder copies it into the history slot (`CopySubresourceRegion` with `pSrcBox = nullptr` for planar/packed YUV, scaling UVs by `activeSize / textureSize` when a hardware decoder pads 1080p NV12 to 1088 rows).
- Otherwise the worker extracts tightly packed planes via `ExtractCpuPlanes` (`primary` Y + `chroma` UV for NV12, packed macropixels for YUY2, or top-down BGRA8). The encoder uploads `R8_UNORM` + `R8G8_UNORM` textures for NV12 (1.5 B/px), a `(width/2) x height` `R8G8B8A8_UNORM` texture for YUY2 (2 B/px), or `B8G8R8A8_UNORM` for RGB32 (4 B/px) once per new camera frame; YUV-to-RGB conversion and blending stay GPU-only in the composite draw (`[FaceCamera] Native YUV path: subtype=... matrix=... range=... gpu=0|1`).
- **CPU pointer origin:** `ExtractCpuPlanes` takes `CpuBufferOrigin`. `Lock2D` supplies the displayed top scanline, including a negative-pitch RGB32 buffer; only `IMFMediaBuffer::Lock` needs the allocation-start adjustment. Adjusting a `Lock2D` pointer again reads beyond the image. `FaceCameraSourceTest.NegativePitchLock2DKeepsItsDisplayedTopScanline` pins the distinction with sentinel-backed rows; the existing tests retain contiguous bottom-up RGB and positive-pitch NV12 coverage. `[FaceCamera] CPU bottom-up RGB32` reports the actual pitch and dimensions on change with a 30-second summary.
- The worker publishes through an atomic `shared_ptr`. There is no camera queue, polling delay, frame wait, or sleep on the encoder/game path.
- Stop first asks the Source Reader to flush its selected video stream, which is the Media Foundation operation that cancels a pending synchronous `ReadSample`. The active-reader publication stays locked through that synchronous flush so the released worker cannot race ahead to source shutdown. Stop then joins the worker; only that owning worker shuts down the media source it created. Never call `IMFMediaSource::Shutdown` cross-thread before flushing the reader: the 2026-08-31 `20260831_060838` session deadlocked finalization in that inverted order. Device references and retained samples are released before the encoder D3D device is destroyed.

Camera failure is deliberately non-fatal to the main video. It is reported with bounded diagnostics, and a stale last frame follows `stale_timeout_ms`.

## Composition and color

The runtime shader emits a fullscreen-vertex-ID quad clipped to the configured camera rectangle. It performs trilinear camera sampling, optional mirroring, an analytic signed-distance rectangle/rounded/circle mask, derivative-based edge antialiasing, border construction, and straight-alpha blending in one draw.

The camera is treated as SDR sRGB/BT.709 content:

- SDR output blends the camera in sRGB RGB before the existing BT.709 conversion.
- scRGB output linearizes the camera and scales it by the active Windows SDR paper-white level.
- HDR10 output converts linear BT.709 to Rec.2020 and then PQ-encodes it at the same paper-white level.

The video-processor path modifies only the camera rectangle when the capture texture is render-target-capable, backs up that small region, converts the composed RGB once, and restores the shared capture texture transactionally. A source lacking render-target binding uses one full-frame GPU compatibility copy. The direct-RGB encoder path draws after the existing normalization pass.

## CFR and performance invariants

- The camera worker never participates in game capture scheduling or CFR pacing.
- Camera transfer and mip generation happen only when the published camera sequence changes, normally 30 times per second even for a 60/120/240 fps recording.
- Optimized shader bytecode is compiled once when the renderer is configured, outside the video-frame path. Inject capture also creates its fixed device resources during session startup while camera negotiation proceeds asynchronously. WGC/DXGI waits until the encoder has adopted MediaEngine's actual shared frame-grab device, then creates only the lightweight device objects; it never guesses the route or creates an incompatible second device.
- Each output uses one small overlay draw; there is no CPU frame blending or readback.
- When camera or cursor state can change, the repeat cache retains one uncomposited RGB source per accepted game frame. Encoder-owned compatible RGB caches gain render-target binding, so CFR duplicates normally update only transactional overlay rectangles before rerunning the existing GPU conversion instead of making another full-frame compatibility copy. Outside the explicit overload-degradation mode below, the camera therefore continues moving while the game frame is repeated.
- Inject capture stages the source cache before encoder submission but publishes it only after the frame is accepted. A failed encoder submission cannot replace the last valid repeat source.
- The first accepted dynamic source also seeds one converted fallback. Later successful dynamic repeats refresh it, so a transient recompose failure still preserves CFR continuity without paying for two full-frame cache copies on every fresh frame.
- Under sustained encoder pressure, two consecutive fresh submissions above 95% of the CFR interval temporarily make repeat slots reuse that fully composited fallback instead of redrawing the camera/cursor and reconverting the frame. Fresh game frames always update the overlays and refresh that fallback before release while degradation is entering/active; a failed fallback copy disarms the mode rather than permitting an old-frame temporal regression. Eight fresh submissions below 75% restore normal dynamic-repeat rendering, so overload recovery is hysteretic and lasting rather than toggling on individual spikes.
- While the camera is starting, repeat-source retention is enabled for the greater of five seconds or `stale_timeout_ms`, so it can appear during a run of CFR-held game frames without a hung/no-frame driver imposing a permanent RGB-copy cost. A stale retained image is retired only during one camera-free recomposition, which first refreshes the converted fallback; a later camera sample is picked up on the next fresh game frame. After startup definitively fails or that retirement completes, the encoder returns to the smaller standard repeat cache instead of freezing the stale image or paying a permanent RGB-copy cost.
- Static-overlay-disabled recording retains the existing already-converted repeat texture path.

This pre-encode placement also means any recording or live-stream output that consumes the `VideoEncoder` frame sees identical face-camera composition. It does not introduce a separate stream-specific camera pipeline.

## Diagnostics and validation

High-signal logs cover selected mode, GPU versus CPU transport, renderer allocation, first composite, periodic composite/upload counts, overload-degradation entry/exit and frozen-repeat totals, stale hiding, device startup/read errors, bounded transfer failures, and the Source Reader flush plus worker-join phases at stop. Logs do not print the configured device identity.

The policy tests cover parsing, invalid-value fallback, process-profile overrides, placement under output scaling, circle cropping, custom-position clamping, template documentation, runtime shader compilation, latest-frame-only source structure, draw ordering, and accepted-frame-only repeat-cache publication.

The 2026-08-31 hardware smoke on the same default camera as session `20260831_060838` validated the GPU-resident WGC/DXGI path through a 25.2-second recording: reader flush returned `S_OK` in 16 ms, the worker joined in 297 ms, and mux publication completed. Runtime stale-risk remains medium: test CPU-fallback webcams, device disconnect/reconnect between recordings, scRGB/HDR10 output, output scaling, inject capture, and CFR repeats on representative Intel/AMD/NVIDIA systems.

## Timing

Fixed 2026-10-16 (code; hardware clap test pending). The encoder used to draw the newest camera frame into a slot
whose screen content and microphone audio are from `scheduledQpc - contentDelay`, so with the 300 ms WGC/DXGI
smoothness delay the camera led the microphone by roughly that much.

- The capture loop sets `CaptureState::contentQpc` on the cursor state it selects for every output slot (fresh and
  repeat, also without cursor capture), and the cursor timeline selects at the same time (`ComputeSlotContentQpc`,
  `common/capture/frame_timing_utils.h`): `scheduled - (A/V delay + active smoothness delay)` for WGC/DXGI,
  `scheduled - A/V delay` for inject, matching `SelectInjectFrame`'s playout target. Until 2026-10-19 inject used
  the undelayed `scheduled` for both, so with an audio capture latency the camera and the cursor led the game frame
  (and the camera the microphone) by that delay. Submissions without slot timing fall back to the frame's
  `associationQpc`, then to the newest frame.
- `FaceCameraFrame::captureQpc`: the Media Foundation sample time converted to QPC when it lies within the second
  before arrival, otherwise the arrival QPC (`ResolveCaptureQpc`; `[FaceCamera] Frame time basis: ...` once).
- `FaceCameraRenderer` keeps a GPU history ring (`face_camera_history.h`): one upload and mip generation per camera
  frame into the oldest slot (as before, one per camera frame), 4 slots initially, grown on demand up to 48 when the
  slot is older than every retained frame (`Camera history grown to N frames ...`, ~11 at 300 ms / 30 fps, about
  40 MB at 720p). Selection: newest frame captured at or before the slot time, never one from the slot's future.
- Periodic `[FaceCamera] GPU composite ...` lines add `history=N slotLagUs=avg/min/max timed=... olderThanHistory=...`;
  in sync the lag stays within about one camera frame interval.
- Retained NV12/P010 reservoir frames get the camera through the YUV overlay patch (`wgc-reservoir-vram-design.md`);
  the renderer draws into a small layer texture through its new viewport-origin argument.

## Open questions

- Camera lead over the microphone: fixed in code (see "Timing"); verify with a clap test on a WGC recording with
  `slotLagUs` in the log. Whether the device sample time includes the webcam's exposure-to-delivery latency depends
  on the driver; the arrival fallback does not.
