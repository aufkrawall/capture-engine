# Screenshot Capture And Publication

Last cross-checked: 2026-08-01 (presentation-contract-aware inject/WGC source encoding, native HDR versus forced-SDR output policy, bounded parallel/realtime 10-bit 4:4:4 AVIF, placeholder-free atomic publication, explicit result notification, split-device WGC readback ownership, and shared ABI 38/request-specific completion)

Primary sources:
- `common/shared_defs.h`
- `hook/common/screenshot_hook.{h,cpp}`
- `captureengine/screenshot.{h,cpp}`
- `captureengine/screenshot_encoding.{h,cpp}`
- `tests/test_screenshot_encoding.cpp`
- `tests/test_screenshot_source.cpp`
- `tests/test_screenshot_hook_worker.cpp`

## Summary

Injected screenshots use the current exact shared-memory ABI 38 and the generation-like request-ID protocol introduced with ABI 32. The controller publishes the request ID, pending status, raw path, and a unique completion-event name. The hook copies mapped pixels before returning from the graphics backend and queues one managed filesystem worker. The worker writes a request-bound raw payload to a new `.part` file, flushes and closes it, atomically renames it to `.ready`, publishes the matching result, and signals that request's event. Stale results never satisfy a newer request.

The request state is explicit: request ID, completed request ID, `Idle|Pending|Writing|Succeeded|Busy|Failed`, Windows error, payload kind, raw path, and completion-event name. A busy worker produces an explicit `Busy` result rather than spawning a detached writer or racing the existing task.

`[Screenshot] color_space=auto|bt709` controls the published screenshot rather than the capture source. `auto` preserves an HDR source as 10-bit BT.2020/PQ AVIF and writes ordinary SDR as PNG. Explicit `bt709` always publishes a conventional SDR PNG; HDR FP16 scRGB and packed PQ inputs are tone-mapped using the same Windows-SDR-white-calibrated luminance knee and luminance-preserving Rec.2020-to-Rec.709 gamut compression as forced-SDR video. Invalid values log and fall back to `auto`. This is independent of `[Video] color_space`.

## Raw Payload ABI

`ScreenshotRawHeaderV2` is an exact 64-byte packed header with magic `CSR2`, version 2, header/payload/total sizes, dimensions, row pitch, pixel format, color encoding, and request ID. Supported pairs are:

- BGRA8 or RGBA8 with sRGB.
- R10G10B10A2 with BT.2020/PQ.
- R10G10B10A2 with SDR BT.709/G2.2 or sRGB.
- RGBA16F with linear scRGB, distinguished as HDR scene content or an SDR presentation.

Both producer and consumer require dimensions `1..16384`, checked 64-bit size arithmetic, a format-aligned row pitch no smaller than the packed row, bounded extra pitch, an exact payload size, and an exact file size. Reads must be complete. Corrupt, truncated, mismatched-request, unknown-format, and overflowed payloads are rejected before allocation or encoding.

## Encoding And Color

- SDR payloads are encoded through WIC as PNG.
- SDR R10 is converted from its declared G2.2 or sRGB transfer to a normal sRGB PNG; SDR FP16 is interpreted as linear Rec.709 and converted directly to sRGB. Neither is mislabeled as HDR merely because its storage has 10-bit/FP16 precision.
- HDR payloads are converted under strict floating-point flags into `YUV444P10LE` and encoded as AVIF with source-built `libaom-av1`.
- Packed R10 input is treated as full-range BT.2020/PQ RGB and converted directly to full-range BT.2020/PQ YUV.
- FP16 scRGB is interpreted as linear BT.709, transformed to linear BT.2020, scaled at 80 nits per scRGB unit, then encoded with ST.2084 before YUV conversion.
- AVIF uses still-picture/realtime mode, `cpu-used=8`, `crf=8`, row multithreading, up to 16 explicit workers, resolution-aware tiles (4x2 at 4K), and BT.2020/PQ metadata. The lower CRF offsets the faster search with higher fidelity and a modest file-size increase. Every codec, mux, packet-write, drain, trailer, and close result is checked.
- HDR-to-PQ/YUV preparation uses the same bounded row-parallel worker policy as HDR-to-SDR. This is especially important for FP16 scRGB, whose exact Rec.709-to-Rec.2020 and ST.2084 math otherwise performs several expensive power operations per component on one thread.
- Forced-SDR screenshots query the primary monitor's `DISPLAYCONFIG_SDR_WHITE_LEVEL`, use `(raw / 1000) * 80 nits`, place that white at 80% linear SDR with a smooth highlight knee, apply sRGB transfer, and write PNG color metadata as full-range RGB/BT.709/sRGB. A logged 203-nit fallback is used only when Windows does not expose the value.
- Injected overlay pixels must already obey the captured swapchain contract. Session `20260719_214733` showed the CPU screenshot tone map correctly retained the game while a secondary DX12 adapter had written sRGB endpoints directly into the PQ target; those nominal 10,000-nit saturated pixels were necessarily gamut-compressed toward white. Synchronizing that adapter's HDR/format state fixes the producer. A deterministic packed-RGB10 fixture proves correctly encoded 320-nit Rec.709 green returns as saturated SDR green with opaque PNG alpha.

PNG/AVIF encoding occurs in an owned `.part` reservation. After flush and close, that exact file is moved atomically to a fresh collision-safe `.png`/`.avif` name without ever exposing a zero-byte final-extension placeholder. A destination collision is preserved and retried under a suffixed name; no existing output is replaced. Success notification occurs only after publication, while a failed capture now produces an explicit red `Screenshot failed!` notification in both pseudo and inject overlays instead of looking like an ignored hotkey.

## Backend Invariants

D3D9, D3D10/11, D3D12, OpenGL, and Vulkan all route mapped pixels through the same request-ID worker boundary. Graphics readback may remain synchronous, but filesystem work is not detached and the mapped memory is never referenced after the backend returns. There is no file-size polling, stable-size heuristic, fixed sleep, unchecked BMP parsing, or replace-existing write to an unreserved destination.

Modern inject backends resolve the presentation contract rather than the texture format alone: successful DXGI `SetColorSpace1` calls distinguish SDR R10, HDR10/PQ R10, and linear scRGB; Vulkan uses the swapchain's `VkColorSpaceKHR`. The WGC fallback passes the captured frame's HDR state into R10/FP16 payload classification. Unsupported format/color-space combinations fail instead of publishing a wrong-color file.

Desktop fallback must not wait 15 seconds merely because the injector service exists. The host validates the shared mapping and skips the hook request immediately when `GetSourcePid()==0`. HDR desktop fallback uses WGC. A split-device WGC frame is a consumer-side shared texture published at keyed-mutex key 1: screenshot readback queries the texture's owning D3D11 device/context, acquires key 1 before `CopyResource`/`Map`, and releases key 0 afterward. Using the bootstrap device or omitting mutex ownership can report a successful readback of the allocation's initial all-black contents.

## Diagnostics And Tests

`ScreenshotHookWorkerTest` covers successful `.part` to `.ready` publication and worker failure. `ScreenshotEncodingTest` covers header validation, exact/truncated reads, dimension and size boundaries, deterministic SDR/PQ/scRGB conversion, correctly encoded saturated overlay color, the bounded AVIF thread/tile plan, and publication failure. `ScreenshotAvifTest` decodes a produced AVIF and verifies dimensions, 10-bit 4:4:4, BT.2020/PQ metadata, and bounded pixel error. `ReservedCaptureOutputTest` proves placeholder-free atomic publication plus collision retry without changing the existing file.

Build `0.1.5116` was validated live on the HDR desktop. Native `auto` produced a non-black 3840x2160 AVIF with AV1 High, `yuv444p10le`, full-range BT.2020-NCL/PQ metadata and a visually correct decoded tone-map preview. Forced `bt709` produced a visually correct 3840x2160 RGBA PNG with full-range RGB, BT.709 primaries and sRGB transfer; the 16-worker 4K CPU tone-map took 57.833 ms and the full WGC-readback/conversion/PNG publication completed in about 450 ms. Both modes logged keyed-mutex acquisition, and the idle injector caused no hook wait.

Session `20260719_221759` exposed two independent latency/UX failures. Its first forced-SDR screenshot completed the 4K tone map in 161.672 ms but failed during the old combined WIC/flush/`ReplaceFile` publication path, so no success notification could appear. The native HDR request reached the encoder at 22:19:44.076 and published at 22:19:59.377, blocking the controller for 15.3 seconds. The new path logs WIC stage/HRESULT, flush Win32 status, HDR-to-PQ time, AVIF encode time, and total time separately. An offline same-frame-class benchmark encoded the old quality profile at 2.864 seconds/2.16 MB; realtime CPU-8 CRF-8 completed in 0.950 seconds/3.24 MB and improved average decoded PSNR from 46.73 dB to 49.51 dB. Fresh end-to-end timing remains a manual runtime check per the user.

## Open Questions / Stale-risk

- Fresh runtime validation is still useful across injected graphics backends and overlay-included HDR game screenshots. Per the user, the remaining inject check is manual; no more interactive test applications should be launched by an agent for this task.
- AVIF interoperability should be rechecked whenever FFmpeg or libaom is upgraded; the source-built encoder and its 4:4:4 capability are part of the packaging contract.
- Native AVIF is high-quality lossy (`crf=8`), not mathematically lossless, and forced-SDR PNG is an 8-bit output. Neither path promises that a hostile synthetic gradient can never reveal quantization; the live checks establish correct color/metadata and absence of the prior black/channel-corruption failures.
