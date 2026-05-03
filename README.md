# CaptureProject

Game capture, recording, overlay, and FPS limiting for Windows.

## Overview

CaptureProject captures and records game video + audio to MKV files. It runs as a system-tray application that injects a hook DLL into games. All rendering, capture, overlay, and encoding is custom code — no third-party libraries are used beyond FFmpeg.

## FFmpeg Integration

Video/audio encoding and MKV muxing uses FFmpeg (`libavformat`, `libavcodec`, `libavutil`, `swresample`). Hardware encoders (NVENC, AMF, QSV) are supported.

See [patches/ffmpeg/](patches/ffmpeg/) for our custom patches.

## Zero-Copy Capture

Capture transfers game frames to the encoder without an extra GPU copy:

| API | Mechanism |
|-----|-----------|
| D3D9Ex | Shared handle (upgraded from D3D9 via `Direct3DCreate9Ex`) |
| D3D10/11 | Shared D3D11 textures |
| D3D12 | Shared heap via D3D11 interop |
| Vulkan | KMT handle import |
| OpenGL | Interop path |
| DXVK (D3D9/10/11 on Vulkan) | Encoder KMT textures imported directly into Vulkan |

## WGC vs Hook Capture

Two capture paths:

- **WGC** (Windows Graphics Capture) — runs out-of-process, compatible with anti-cheat. Uses WinRT `Windows.Graphics.Capture` API. Supports DirectFlip content. Default non-injected path.
- **Hook capture** — runs in-process via Present/swapchain hooks. Shares captured textures to the encoder via shared memory + GPU fences. Used when the overlay is active.

## Custom Overlay with Frame Generation Support

100% custom overlay renderers for DX9–DX12, Vulkan, and OpenGL. Custom font rasterizer, pre-compiled HLSL shaders. No Dear ImGui, no external overlay libraries.

- **DLSS FG**: Hooked via NVIDIA Streamline API (`slDLSSGSetOptions`, `slDLSSGGetState`) and NGX (`NVNGX_CreateFeature`). Four interception seams: inline hook, wrapper substitution, `GetProcAddress` hooks, and direct-import fallback.
- **FSR FG**: Hooked via AMD FidelityFX API (`ffxCreateContext`, `ffxConfigure`, Present-callback bridge).
- **Switching**: All combinations work — `off ↔ DLSS FG ↔ off ↔ FSR FG ↔ DLSS FG` without crashes or lost overlay rendering.

## Audio Capture and Mixing

- **System audio**: WASAPI loopback capture.
- **Per-application audio**: Windows 10/11 process-loopback API (`ActivateAudioInterfaceAsync` with `AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK`). Captures by PID or process name.
- **Lock-free ring buffer**: SPSC float buffer decoupling capture from encoding.
- **Drift compensation**: Pitch-adjustment (tier 1) + sample trimming (tier 2) for clock mismatch between audio device and render pipeline.

## FPS Limiter and Capture-Sync

Multiple limiter backends with automatic selection:

| Mode | Description |
|------|-------------|
| Basic | Timer-based `SmartWait` with hybrid sleep/spin (waitable timer, `Sleep(0)`, `SwitchToThread`, `_mm_pause`) |
| Reflex | NVIDIA Reflex (`NvAPI_D3D_SetSleepMode` / `Sleep`) |
| Anti-Lag 2 | AMD driver extension (DX12 only) |
| XeLL | Intel XeLL (DX12 only) |
| Auto | Priority: Reflex > Anti-Lag 2 > XeLL > FG fallback > Basic |

**Capture-sync**: When recording, game FPS = capture FPS × multiplier (e.g., 60 fps capture × 2 = 120 fps game). Uses local cadence in the hook DLL instead of cross-process event round-trips (~3–4 ms saved).

## Injection and Hooking

- **Injection**: `CreateRemoteThread` (post-loader) and APC-based early injection (pre-loader, requires `CREATE_SUSPENDED`).
- **Signature verification**: Authenticode + SHA-256 hash check before injection.
- **Hook types**: IAT hooks, inline hooks (x86/x64 trampolines), COM vtable hooks.
- **COM wrappers**: Full DXGI/D3D device, swapchain, and queue wrapping for transparent hooking.
- **Vulkan**: Implicit layer (`VK_LAYER_CE_overlay`) for capture and overlay.
- **IPC**: Named pipes between controller, hook, media, sensor, and logger processes.
- **Shared memory ABI**: Frame ring buffer with GPU fence synchronization.

## Multi-Process Architecture

Separate processes for isolation and stability:

- **Controller** (captureengine.exe): system tray, injection, WGC capture, IPC hub
- **Hook DLL**: injected into the game, runs all API hooks and overlay
- **Media encoder**: separate process for encoding/muxing
- **Sensor service**: CPU/GPU/RAM/VRAM metrics
- **Logger service**: dedicated logging
- **Limiter service**: optional standalone FPS limiter process

## FFmpeg Patches

See [patches/ffmpeg/README.md](patches/ffmpeg/README.md) for details.

- **`0001-matroska-add-timestamp-precision-option.patch`**: Adds a `timestamp_precision` option to the MKV muxer. At 120 fps, the default 1 ms TimecodeScale forces ±8% frame interval jitter; 1 µs precision reduces it to ±0.008%.
- **`0002-nvenc-bframe-cfr-improvements.patch`**: NVENC improvements for game capture — weighted prediction for AV1, reduced lookahead margin, auto `b_ref_mode=middle`, graceful flush drain of B-frame reorder buffer.

## Smooth Video Capture

- Microsecond MKV timestamps (patch above eliminates frame-interval jitter)
- Capture-sync pacing keeps game and capture cadence aligned
- Async capture thread with lock-free ring buffer
- Fence-based GPU synchronization (no CPU stalls)
- WGC texture pool with keyed mutex
- Clock-mismatch compensation in audio path

## Render Overrides

Forced anisotropic filtering (2x–16x), mip/LOD bias, and SGSSAA, applied per-API:

| API | Hook Point |
|-----|------------|
| D3D9 | `SetSamplerState` interception |
| D3D10/11 | `CreateSamplerState` / `PSSetSamplers` override |
| D3D12 | Root-signature static sampler + `CreateSampler` IAT hook |
| OpenGL | `glTexParameterf` interception |
| Vulkan | `vkCreateSampler` override (via layer) |

AF selectively skips textures/mip maps where applying anisotropic filtering would cause rendering artifacts on Blackwell (RTX 50 series) GPUs.

## HDR Support

HDR-aware capture and overlay rendering. Probes DXGI output color space to distinguish FP16 (always HDR) from 10-bit UNORM (probe-dependent). *Note: HDR support is still experimental.*

## Test Infrastructure

- **Unit tests**: GoogleTest in `tests/` (FPS limiter, FG session state, DXGI shared routing, sampler overrides, crash dump policies, overlay FG status, FFX API parsing, config)
- **Test applications**: Per-API test apps in `testapp/` (DX6–DX12, OpenGL, Vulkan, DirectDraw)
- **Python test runner**: Automated test execution

## License

MIT — see [LICENSE](LICENSE).

## Donations

If you find this project useful, consider tipping via [GitHub Sponsors](https://github.com/sponsors/aufkrawall).
