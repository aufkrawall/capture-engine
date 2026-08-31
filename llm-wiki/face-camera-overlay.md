# Face-camera overlay

Last verified: 2026-08-31

## Summary

`[FaceCamera]` adds an optional USB/webcam picture-in-picture layer to both inject and WGC/DXGI recordings. Camera acquisition is isolated on a below-normal-priority Media Foundation worker. The video encoder consumes only the newest immutable camera frame, so camera startup, a slow device, disconnects, and camera-frame backlog never block game capture or the CFR output clock.

The compositor is D3D11-native. A new camera frame is copied or uploaded to the encoder GPU once at camera cadence, optional mipmaps are regenerated at that cadence, and each encoded output frame uses one antialiased shader draw before the existing RGB-to-NV12/P010 conversion. Cursor composition follows the camera draw and therefore remains visually topmost.

Primary sources:

- `common/face_camera_config.h`
- `common/config_load_face_camera.cpp`
- `mediaengine/face_camera_capture.{h,cpp}`
- `mediaengine/face_camera_renderer.{h,cpp}`
- `mediaengine/face_camera_shader.h`
- `mediaengine/video_encoder_face_camera.cpp`
- `mediaengine/video_encoder_convert_bgra.cpp`
- `mediaengine/video_encoder_textures.cpp`
- `mediaengine/video_encoder_framegrab.cpp`
- `mediaengine/video_encoder_encode.cpp`
- `captureengine/config.ini.template`
- `tests/test_face_camera_config.cpp`
- `tests/test_face_camera_source.cpp`

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

`FaceCameraCapture` dynamically resolves Media Foundation device enumeration and SourceReader creation from the Windows system DLLs. It asks the SourceReader for hardware transforms, advanced video processing, low latency, and the encoder's `IMFDXGIDeviceManager`. D3D allocation is optional so drivers that cannot provide compatible surfaces fall back to system-memory frames; do not enable the limited `MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING` path alongside the D3D manager because that path is software-only and contractually incompatible with it.

- If the driver supplies a compatible BGRA/BGRX/RGBA DXGI buffer on that device, the immutable `IMFSample` retains the GPU resource until the encoder uploads it.
- Otherwise the worker copies the sample into a tightly packed BGRA vector. The encoder performs one `UpdateSubresource` per new camera frame; blending and final conversion remain GPU-only.
- The worker publishes through an atomic `shared_ptr`. There is no camera queue, polling delay, frame wait, or sleep on the encoder/game path.
- Stop flushes and shuts down the active source before joining the worker. Device references and retained samples are released before the encoder D3D device is destroyed.

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
- When camera or cursor state can change, the repeat cache retains one uncomposited RGB source per accepted game frame. Encoder-owned compatible RGB caches gain render-target binding, so CFR duplicates normally update only transactional overlay rectangles before rerunning the existing GPU conversion instead of making another full-frame compatibility copy. The camera therefore continues moving while the game frame is repeated.
- Inject capture stages the source cache before encoder submission but publishes it only after the frame is accepted. A failed encoder submission cannot replace the last valid repeat source.
- The first accepted dynamic source also seeds one converted fallback. Later successful dynamic repeats refresh it, so a transient recompose failure still preserves CFR continuity without paying for two full-frame cache copies on every fresh frame.
- While the camera is starting, repeat-source retention is enabled for the greater of five seconds or `stale_timeout_ms`, so it can appear during a run of CFR-held game frames without a hung/no-frame driver imposing a permanent RGB-copy cost. A stale retained image is retired only during one camera-free recomposition, which first refreshes the converted fallback; a later camera sample is picked up on the next fresh game frame. After startup definitively fails or that retirement completes, the encoder returns to the smaller standard repeat cache instead of freezing the stale image or paying a permanent RGB-copy cost.
- Static-overlay-disabled recording retains the existing already-converted repeat texture path.

This pre-encode placement also means any recording or live-stream output that consumes the `VideoEncoder` frame sees identical face-camera composition. It does not introduce a separate stream-specific camera pipeline.

## Diagnostics and validation

High-signal logs cover selected mode, GPU versus CPU transport, renderer allocation, first composite, periodic composite/upload counts, stale hiding, device startup/read errors, and bounded transfer failures. Logs do not print the configured device identity.

The policy tests cover parsing, invalid-value fallback, process-profile overrides, placement under output scaling, circle cropping, custom-position clamping, template documentation, runtime shader compilation, latest-frame-only source structure, draw ordering, and accepted-frame-only repeat-cache publication.

Runtime stale-risk remains medium: test GPU-resident and CPU-fallback webcams, device disconnect/reconnect between recordings, SDR/scRGB/HDR10 output, output scaling, WGC/DXGI and inject capture, and CFR repeats on representative Intel/AMD/NVIDIA systems.
