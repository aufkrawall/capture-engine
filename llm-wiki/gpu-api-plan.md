# GPU API plan: where D3D11 stays and where D3D12/Vulkan comes in

Last cross-checked: 2026-10-24 (inventory against `main` 76a02877; decisions by the user the same day).
Status: plan only, nothing implemented. Near-term items: the shader scaler and the dead-code removal below.

Primary sources:
- `mediaengine/video/video_encoder_{convert_bgra,convert_yuv,convert_shaders,conversion,codec}.cpp`,
  `mediaengine/engine/mediaengine.h`
- `captureengine/media/{wgc_capture_pool,dxgi_dup_capture}.cpp`, `hook/vulkan_layer/layer_capture_*.cpp`,
  `hook/{d3d8,d3d9,ddraw,opengl}/*_capture_*.cpp`
- `hook/d3d12/{dx12_hook_types.h,dx12_hook_types_impl.cpp,dx12_hook_overlay_d3d11on12.cpp}`
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

### Dead code (near-term item 2)

- **D3D11On12 leftovers in `hook/d3d12/`:**
  - Nothing calls `D3D11On12CreateDevice`.
  - The `d3d11on12Device`, `d3d11on12Context`, `d3d11on12`, `d3d11WrappedBBs`, `d3d11RTVs` and `d3d11on12Init` fields
    in `dx12_hook_types.h` are only ever released (`dx12_hook_types_impl.cpp`), never assigned.
  - The `#include <d3d11on12.h>` in `dx12_hook_internal.h` is unused.
  - `CleanupD3D11On12` (`dx12_hook_overlay_d3d11on12.cpp`) and `dx12_hook_g_D3D11On12Adapter` are old names for the
    native DX12 descriptor-free overlay backend, and the `dx12_hook_g_SLFGAdapter` comment still calls it a D3D11On12
    adapter. Rename the live parts, delete the dead ones.
- **`hook/sharpen/sharpen_d3d11.h` in DX12 code:** `dx12_hook_ffx.cpp`, `dx12_hook_ffx_owner_queue.cpp`,
  `dx12_hook_fsr_screenshot.cpp`, `dx12_ffx_suspend_overlay_internal.h` and `dx12_hook_ffx_topmost_batch.cpp` include
  it only for the DXGI format helpers (`FormatAppliesSrgbConversion`, `RawUnormViewFormat`, `ResolveDxgiEncoding`, ...).
  Move those into an API-neutral header next to the sharpen policy.

## Near-term item 1: the shader scaler replaces the VideoProcessor fallback

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
- Its old cost, NVIDIA queueing the Blt behind NVENC (sessions `20261016_003600`/`010712`/`011829`), went away with the
  encoder device bridge (`nvenc-encoding.md`, backlog item 2).

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
  - `InitVideoProcessor`'s geometry setup stays under a fitting name.
- **Tests:**
  - Extend `tests/test_sdr_yuv_shader.cpp`, which runs on a hardware D3D11 device, with scaled outputs: ramps for the
    kernel response, a checkerboard for aliasing, chroma siting, odd input sizes, and the FP16 SDR normalization.
  - Source-policy tests for the route selection.
- **Diagnostics:** a one-shot `[SDR YUV] Shader scaler active: in=WxH out=WxH kernel=... sharpen=...` line, and the
  reason line for every route that still refuses.

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
- Loss of colour compression on shared surfaces per vendor. This only matters if the rejected cross-API core is ever
  revisited; it would need a 4K120 A/B against same-device WGC.
