# GPU API plan: where D3D11 stays and where D3D12/Vulkan comes in

Last cross-checked: 2026-10-26 (inventory against `main` 76a02877; decisions by the user 2026-10-24; near-term items 1–3 implemented 2026-10-26).
Status: near-term items 1, 2 and 3 implemented in code and verified with unit + GPU/WARP tests (2026-10-26). Deletion of the residual SDR `VideoProcessor` fallback (`EnsureVideoProcessorObjects`, VP view caches, colour-space helpers) awaits the 3-vendor hardware validation round (NVIDIA, AMD, Intel) and webcam runs (NV12/YUY2 UVC + MJPEG).

Primary sources:
- `mediaengine/video/{scale_kernel,frame_scaler,frame_scaler_shader,sampleable_source_copy,shader_conversion_route,face_camera_format}.{h,cpp}`
- `mediaengine/video/video_encoder_{convert_bgra,convert_yuv,convert_shaders,conversion,codec}.cpp`,
  `mediaengine/video/face_camera_{capture,renderer,shader}.{h,cpp}`, `mediaengine/engine/mediaengine.h`
- `captureengine/media/{wgc_capture_pool,dxgi_dup_capture}.cpp`, `hook/vulkan_layer/layer_capture_*.cpp`,
  `hook/{d3d8,d3d9,ddraw,opengl}/*_capture_*.cpp`
- `hook/d3d12/{dx12_hook_types.h,dx12_hook_types_impl.cpp,dx12_hook_overlay_d3d11on12.cpp}`,
  `hook/sharpen/sharpen_dxgi_format.{h,cpp}`
- `tools/build/build_ffmpeg.py`; FFmpeg `libavcodec/{nvenc,amfenc,qsvenc,mfenc}.c`, `libavutil/hwcontext_amf.c`

## Decisions (user, 2026-10-24)

- **Goal:** D3D11 must stop constraining CE's own media pipeline. This prepares FFmpeg's `*_d3d12va` and `*_vulkan`
  encoders and, later, a native Linux port.
- **Not replaced:** anything that serves D3D10/11 games (D3D11 is the game's own API), and WGC and DXGI duplication
  capture.
- **Windows keeps a native D3D11 media core, also after a Linux port.** WGC and duplication only hand out D3D11
  textures. On the D3D11 core, WGC converts on the media engine's own device (`wgc_same_device_capture=true`, the
  default) with no cross-API handoff, and the inject producers already arrive as an NT handle plus a shared fence. A
  Linux port adds a Vulkan backend instead of moving Windows onto it. The best solution wins over the least code: a
  second GPU backend is accepted.
- **Rule for that second backend:** one owner for the policy (formats, colour transforms, scaler, cursor and
  face-camera composition, CFR, leases) with thin per-API backends. HLSL stays the single shader source, built as DXBC
  (fxc) for D3D11 and SPIR-V (DXC) for Vulkan. No backend gets its own copy of a policy (`code-structure.md`).
- **Rejected: a D3D12/Vulkan core on Windows with WGC and duplication as D3D11 edges.**
  - The WGC surface is not shareable, so the ingress pass (`RenderFrameToPoolSlot`, the retained-YUV conversion in
    `captureengine/media/wgc_capture_pool.cpp`) would have to stay in D3D11 anyway.
  - Default same-device WGC would become a cross-API split, at a cost that was never measured: a fence per frame
    (almost always already signalled behind the 300 ms reservoir), one more GPU context against a GPU-bound game, and
    possibly no colour compression on shared surfaces.
  - Duplication already runs on its own device with a keyed-mutex handoff, so it would gain nothing.
- **Rejected: a native D3D11 pipeline for WGC/dup next to a D3D12/Vulkan pipeline for inject on Windows.** That is two
  cores on one platform, and every CFR/sync property would need validating twice.

## Inventory

### Stays D3D11

| Area | Where | Why |
| --- | --- | --- |
| D3D10/11 game hooks | `hook/d3d11/` (about 11k lines: present, overlay, capture producer, screenshots, AF, prerender, sharpen), `hook/wrappers/d3d11_*` (about 2.5k), `hook/sharpen/sharpen_d3d11`, the D3D11 parts of `hook/present/` and `hook/hooking/iat_hook_init.cpp`, D3D11 flow tests, `testapp/dx11_test.cpp` | the game's API |
| WGC | `captureengine/media/wgc_capture*` | `Direct3D11CaptureFramePool` needs a D3D11 device and returns D3D11 surfaces |
| DXGI duplication | `captureengine/media/dxgi_dup_capture.*` | `DuplicateOutput`/`DuplicateOutput1` accept only D3D10/11 devices |
| Desktop screenshots, privacy blackout | `captureengine/media/screenshot.cpp`, `screen_grab_privacy*` | work on the WGC/dup frames on the same device |
| Face-camera capture | `mediaengine/video/face_camera_capture.cpp` (`MFCreateDXGIDeviceManager`) | the Media Foundation camera reader's GPU path uses a D3D11 device manager |
| D3D9 in-game producer | `hook/d3d9/dx9_hook_capture_*` | D3D9Ex shared surfaces have non-NT handles, which only D3D11 can open; the alternative is CPU readback |
| Windows media core | `mediaengine/video` conversion, cursor, face-camera rendering, YUV patch, retained NV12/P010 frames, GPU timing probes; the `MediaEngine_*D3D11*` exports in `mediaengine/engine/mediaengine.h` | the decision above |
| Existing encoders | NVENC, AMF, QSV and MF on `AV_HWDEVICE_TYPE_D3D11VA` (`video_encoder_codec.cpp`, `video_encoder_submit_gate.cpp`, `encoder_auto_probe.cpp`) | FFmpeg's `nvenc.c` takes D3D11 or CUDA frames, `qsvenc.c` QSV frames derived from D3D11/DXVA2, `mfenc.c` D3D11 only; `hwcontext_amf.c` could derive from D3D12VA but gains nothing here |

### Could change, no gain while the Windows core is D3D11

- **D3D8 and DirectDraw producers:** the CPU locks the surface, then `UpdateSubresource` writes a D3D11 shared
  texture (`dx8_hook_capture_frame.cpp`, `ddraw_hook_capture_frame.cpp`). A D3D12 upload would work just as well.
- **OpenGL producer:** `WGL_NV_DX_interop` on a helper D3D11 device. `GL_EXT_memory_object_win32` plus
  `GL_EXT_semaphore_win32` could replace it, but Intel support is unverified. A Linux port would use the `_fd`
  variants.
- **Vulkan layer:** a helper D3D11 device in the game (`layer_capture_d3d11_interop.cpp`) creates the shared textures
  and fence that Vulkan imports (`VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE[_KMT]_BIT`). A D3D12 helper would still
  be a helper device.
- **Small ones:**
  - `GetAdapterLuid(ID3D11Device*)` in `captureengine/app/windows_gpu_scheduling.cpp` reads the LUID; DXGI alone would
    do.
  - `ProbeD3D11` in `hook/d3d12/dx12_device_creation_report.cpp` is a deliberate D3D11 check: it tells a driver-level
    refusal apart from others.
  - `captureengine/elevation/elevation_integration.cpp` is a test fixture that presents a D3D11 swapchain.

### Dead code (near-term item 3 — completed 2026-10-26, commit `0717c656`)

- **D3D11On12 leftovers in `hook/d3d12/` (removed):**
  - Removed the never-assigned `d3d11on12Device`, `d3d11on12Context`, `d3d11on12`, `d3d11WrappedBBs`, `d3d11RTVs` and
    `d3d11on12Init` fields in `dx12_hook_types.h` and their release code in `dx12_hook_types_impl.cpp`.
  - Removed the unused `#include <d3d11on12.h>` in `dx12_hook_internal.h`.
  - Renamed `CleanupD3D11On12` (`dx12_hook_overlay_d3d11on12.cpp`) to `CleanupNativeDx12Overlay` and
    `dx12_hook_g_D3D11On12Adapter` to `dx12_hook_g_NativeDx12OverlayAdapter`, and updated the `dx12_hook_g_SLFGAdapter`
    comment.
- **`hook/sharpen/sharpen_d3d11.h` in DX12 code (extracted):** moved the API-neutral DXGI format helpers
  (`FormatAppliesSrgbConversion`, `RawUnormViewFormat`, `ResolveDxgiEncoding`, ...) into
  `hook/sharpen/sharpen_dxgi_format.{h,cpp}`, so `dx12_hook_ffx.cpp`, `dx12_hook_ffx_owner_queue.cpp`,
  `dx12_hook_fsr_screenshot.cpp`, `dx12_ffx_suspend_overlay_internal.h` and `dx12_hook_ffx_topmost_batch.cpp` no longer
  include `sharpen_d3d11.h`.

## Near-term item 1: the shader scaler replaces the VideoProcessor fallback (implemented 2026-10-26, commit `513350a0`; VP deletion pending HW)

**Verdict (2026-10-24): worth doing.** The same move already fixed encoder starvation for unscaled SDR:
- On NVIDIA, 3D work after a `VideoProcessorBlt` only completed behind NVENC's earlier encodes, so inject p4/p5
  overloaded with NVENC headroom left (`20261016_003600`/`011829`, 16-22 ms GPU stalls).
- With CE's YUV shaders, inject p5 4K120 10-bit held with no repeats (`20261016_014920`), and at p6 NVENC was the limit
  again (`015331`; `nvenc-encoding.md`).
- The remaining VideoProcessor paths still share their device with AMF, QSV and MF, because the encoder device bridge
  is NVENC-only. The coupling was never ruled out there.

**Today:**
- `VideoEncoder::ConvertBGRAtoNV12` (`video_encoder_convert_bgra.cpp`) tries CE's SDR YUV shaders first.
  `CanConvertSdrYuvWithShader` (`video_encoder_convert_yuv.cpp`) sends a frame to `VideoProcessorBlt` for one of these
  reasons:
  - `output format`: the output is not NV12/P010.
  - `input format`: the SRV format is unknown, or the input is linear FP16 SDR (scRGB inject swapchains).
  - `input not sampleable`: no SRV bind flag, or MSAA.
  - `scaling`: input and output sizes differ, or the output has odd dimensions.
- One more case: after a shader failure, `sdrYuvShaderFailed` sends later frames to the VideoProcessor.
- The once-per-recording line `[SDR YUV] VideoProcessor conversion used (<reason>)` names the reason.
- HDR output never uses the VideoProcessor. `ConvertHdrRgb10ToP010` already scales in the shader (bilinear, with a
  luma sharpen of `sharpness/400` capped at 0.25), and the driver's PQ VideoProcessor fallback stays refused because it
  produced corrupt colours.

**Why replace it:**
- The VideoProcessor is the core's only D3D11 *video* API, and it has no D3D12/Vulkan counterpart. A Vulkan backend
  needs the shaders anyway.
- Scaling quality and chroma siting are up to each driver.
- RGBA input needs an extra R/B swap pass first (`SwapRBChannels`).
- Retained NV12/P010 frames exclude scaled output ("scaling (VideoProcessor scaler)", `wgc-reservoir-vram-design.md`).
  A shader scaler makes scaled WGC/dup recordings eligible for that VRAM saving.
- HDR scaling improves too: it is bilinear today, which aliases above 2x downscale.
- Behind the encoder device bridge, NVENC no longer shares a device with the VideoProcessor (`nvenc-encoding.md`,
  backlog item 2). Scaled output on that route has not been measured, though.

**Drawbacks and mitigations:**
- **GPU time on the game's engine.** On Intel, and possibly some AMD APUs, the VideoProcessor may run on fixed-function
  video hardware; a shader shares the 3D engine with a GPU-bound game. The cost is bounded by today's unscaled shader
  conversion (a downscale reads the source once and writes a smaller frame) plus the extra filter taps. Measure per
  vendor, an Intel iGPU in particular, with `encoder_gpu_stage_probe` before deleting the VideoProcessor.
- **Scaled SDR recordings will look different.** `sharpness` defaults to 100 (template and unset key) and is today the
  driver's edge enhancement, which varies by vendor and is missing on some drivers; `quality=best` is only a
  VideoProcessor usage hint. A uniform shader sharpen will not match any driver. Choose the mapping deliberately and
  add a CHANGELOG note.
- **No catch-all fallback once deleted.** Every refusal reason needs a shader route first. Delete only after hardware
  runs on NVIDIA, AMD and Intel.
- **Linear FP16 SDR:** reproduce the VideoProcessor's clipping above 1 and below 0.
- **Kernel cost:** more taps in one pass, or two passes with an intermediate (about 16 MB at 16 bit for 4K to 1080p).
- **Scaled output in the retained NV12/P010 frames is new.** Scaling then runs for every captured frame, not only the
  recorded ones, and cursor and face camera compose at output size. Validate this separately.

**Scope:**
- **One scaler for SDR and HDR.** Bilinear under-samples above 2x (4K to 720p). Downscaling needs a footprint that
  covers the ratio (for example a separable multi-tap kernel widened by the ratio); upscaling needs bicubic or
  bilinear. Choose by GPU cost at 4K120: one pass, or two separable passes with an intermediate.
- **Chroma at output size with left siting,** like today's shaders (`chroma=left`).
- **Keep the meaning of `[VideoScaling]`:**
  - `enabled` and `output_resolution` stay as they are.
  - `quality` (`normal`/`best`) stays.
  - `sharpness` (0-100, 100 when unset) is today the VideoProcessor's edge-enhancement level on SDR and the luma
    sharpen on HDR.
  - The legacy `filter` key maps to quality/sharpness.
- **Linear FP16 SDR:** encode to sRGB in the shader, or reuse the existing RGB10 normalization, so `input format` no
  longer reaches the VideoProcessor.
- **`input not sampleable`:** resolve or copy into a CE-owned sampleable texture; duplication's canonical surface is the
  precedent.
- **After hardware validation (NVIDIA, AMD, Intel):**
  - Delete `EnsureVideoProcessorObjects`, the VideoProcessor view caches, the `vpCompatible` gate and the edge-enhancement
    setup in `video_encoder_conversion.cpp`.
  - A shader failure becomes a logged hard failure instead of a fallback.
  - Delete `GetVideoProcessorInputColorSpace`/`GetVideoProcessorOutputColorSpace` (`video_encoder_format.cpp`) with the
    Blt.
  - Rename what only sounds like the VideoProcessor:
    - `ResolvedFormat::usesVideoProcessor` means "4:2:0 YUV encoder input, CE converts".
    - `PrepareVideoProcessorFaceCameraInput` is the RGB precomposition before any conversion.
    - `InitVideoProcessor` only sets the output geometry.
    - Comments in `retained_frame_route.h`, `encoder_device_bridge.h`, `gpu_memory_breakdown.h`,
      `video_color_conversion_shader.h` and `video_encoder_codec.cpp`.
- **Tests (implemented):**
  - `tests/test_scale_kernel.cpp`: pure kernel/route policy tests (`ScaleKernelTest.*`, `ShaderConversionRouteTest.*`).
  - `tests/test_frame_scaler_gpu.cpp`: GPU tests (`FrameScalerGpuTest.*`) covering upscaling, >2x downscaling without
    checkerboard aliasing, anti-ringing step edges at `sharpness=100`, HDR PQ monotonic ramps, FP16 scRGB SDR clipping,
    non-SRV and MSAA sampleable copy/resolve, and `4:2:0`/`4:2:2`/`4:4:4`/`RGB` output layouts.
  - `tests/test_sdr_yuv_shader.cpp` and `tests/test_video_encoder_source*.cpp` updated.
- **Plan lifetime:** `FrameScaler::EnsurePlan` compares geometry, kernel, effective intermediate precision and output ownership before calling the CPU planner. `ScaleIntermediatePrecision` is shared with `PlanScale`, so equivalent 8/10-bit requests reuse tables; geometry/kernel/precision changes, Reset and device changes rebuild them. `FrameScalerGpuTest.ReusesCpuWeightTablesUntilThePlanChanges` injects a counting planner and fails on the old per-frame rebuild (four unchanged draws built four plans). The original 4K-to-1080p Lanczos3 planner cost about 1.37 ms/frame in a bounded local probe; the fixed cache hit does no planning.
- **Diagnostics (implemented):** `[Scaler] Plan: ...` and `[SDR YUV] Direct shader conversion active: ... scaler=...`
  lines, plus the refusal reason line if a route ever falls back.

## Near-term item 2: the face camera without Media Foundation's VideoProcessor (implemented 2026-10-26; webcam HW runs pending)

This is its own commit in the same series, so one hardware round covers both VideoProcessor removals.

**Today:**
- `FaceCameraCapture` (`face_camera_capture.cpp`) ranks the camera's native modes NV12 > YUY2 > RGB32/ARGB32 > MJPG >
  H264 (`SubtypePreference`).
- It then always requests RGB32 output, with `MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING` and CE's D3D11 device
  as `MF_SOURCE_READER_D3D_MANAGER`. Media Foundation's Video Processor MFT converts every camera frame to RGB32 with
  the D3D11 VideoProcessor on that device.
- Without a device manager, the CPU path (`isolated CPU frame transport`) receives RGB32 buffers.
- `FaceCameraRenderer` then draws the RGB texture, scaled, into the video frame.

**Why:**
- It is CE's last VideoProcessor use.
- It runs on the device AMF, QSV and MF share with the encoder, so the coupling seen on NVIDIA is possible there
  (unverified; NVENC has its own device).
- It removes one GPU pass: the renderer samples YUV and converts in the composite draw.
- Kept camera frames get smaller (NV12 is 1.5 B/px against RGB32's 4).
- CE decides the colour matrix and range from `MF_MT_YUV_MATRIX`/`MF_MT_VIDEO_NOMINAL_RANGE` instead of the driver.

**Scope:**
- Request NV12 or YUY2 output with advanced video processing off.
  - NV12 and YUY2 cameras pass through unchanged.
  - MJPG and H264 cameras keep Media Foundation's decoder (decode only), with NV12/YUY2 output.
  - RGB32 cameras need no conversion.
- The renderer samples NV12 (R8 and R8G8 plane views) or YUY2 (packed 4:2:2) and converts in the composite draw with
  the camera's matrix and range.
- Decoder output textures may lack `D3D11_BIND_SHADER_RESOURCE`, so the GPU path copies them once into a sampleable
  texture. The CPU path uploads the YUV planes.
- **Tests:** shader tests with known YUV patterns (NV12/YUY2, BT.601/709, limited/full range), and a source-policy test
  for the negotiated type.
- **Diagnostics:** a one-shot `[FaceCamera] Native YUV path: subtype=... matrix=... range=... gpu=0|1` line.
- **Hardware:** the user's runs need at least one NV12 or YUY2 UVC webcam and one MJPEG webcam.

**Media Foundation itself stays (assessed 2026-10-24).**
- It is the camera API Windows still develops: Frame Server, camera privacy settings, Windows Studio Effects, and
  Windows 11 virtual cameras via `MFCreateVirtualCamera`.
- DirectShow is in maintenance mode. Its only extra reach is DirectShow-only devices: some capture cards and older
  virtual cameras.
- WinRT `MediaCapture` is built on Media Foundation, and its frames are D3D11 surfaces too.
- After this item, Media Foundation only enumerates devices, delivers frames and decodes MJPEG/H264.
- A DirectShow source would be an addition for those devices, not a replacement. A Linux port gets its own camera
  source (V4L2 or the PipeWire camera portal) behind the same interface.

## Later

- **D3D12/Vulkan encoders.** `h264/hevc/av1_d3d12va` and `h264/h265/av1_vulkan` become consumers of the encoder device
  bridge: they open the shared encoder texture and the shared fence and wait on the GPU, not the CPU gate, while
  conversion stays on the D3D11 core.
  - The FFmpeg 8.0-git checkout has `d3d12va_encode_*.c` and `vulkan_encode_*.c`, and `CONFIG_D3D12VA`/`CONFIG_VULKAN`
    are 1 in `ffmpeg_build/working/ffmpeg/config.h`.
  - Missing: the encoder whitelist in `tools/build/build_ffmpeg.py` (`--enable-encoder=...`), a device and frames
    context per API, and `encoder=auto` probe entries.
  - These encoders expose fewer controls than the vendor ones (no lookahead; `hardware-encoding.md`).
- **Linux port.**
  - Conversion and compositing: a Vulkan backend behind the same policy owner.
  - Desktop capture: PipeWire/xdg-desktop-portal with dmabuf import.
  - Game capture: the Vulkan layer, plus OpenGL via `EXT_memory_object_fd`.
  - Encoding: Vulkan Video, VAAPI, and NVENC on CUDA frames imported from Vulkan.
  - Audio: PipeWire.
  - Injection works completely differently there (implicit layer / preload).
  - The media DLL interface gains an API-neutral submission (handle or fd, fence, device identity); the D3D11 exports
    stay on Windows.

## Open questions and unverified claims

- Can D3D12 and Vulkan open the bridge's D3D11-created shared NV12/P010 textures? D3D12 most likely can; Vulkan import
  of two-plane images depends on the driver.
- Do FFmpeg's d3d12va encoders accept frames created outside FFmpeg with an external fence (`AVD3D12VAFrame.sync_ctx`)?
- Driver support for Vulkan Video encode on Windows: Intel at all, AMD for AV1.
- Which scaler kernel, and what it costs at 4K120 against the VideoProcessor (GPU timestamps via
  `encoder_gpu_stage_probe`).
- What Media Foundation's processor assumes when a camera type has no matrix or range attribute; the native YUV path
  must default the same way, or the camera colours shift.
- Which output the MJPEG decoder offers on supported Windows versions: NV12, YUY2, or both.
- Loss of colour compression on shared surfaces per vendor. This only matters if the rejected cross-API core is ever
  revisited; it would need a 4K120 A/B against same-device WGC.
