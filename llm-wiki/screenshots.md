# Screenshot Capture And Publication

Last cross-checked: 2026-07-19 (presentation-contract-aware inject/WGC source encoding, native HDR versus forced-SDR output policy, split-device WGC readback ownership, idle-inject fallback, shared ABI 37/request-specific completion, WIC PNG, and libaom 10-bit 4:4:4 AVIF)

Primary sources:
- `common/shared_defs.h`
- `hook/common/screenshot_hook.{h,cpp}`
- `captureengine/screenshot.{h,cpp}`
- `captureengine/screenshot_encoding.{h,cpp}`
- `tests/test_screenshot_encoding.cpp`
- `tests/test_screenshot_source.cpp`
- `tests/test_screenshot_hook_worker.cpp`

## Summary

Injected screenshots use the current exact shared-memory ABI 37 and the generation-like request-ID protocol introduced with ABI 32. The controller publishes the request ID, pending status, raw path, and a unique completion-event name. The hook copies mapped pixels before returning from the graphics backend and queues one managed filesystem worker. The worker writes a request-bound raw payload to a new `.part` file, flushes and closes it, atomically renames it to `.ready`, publishes the matching result, and signals that request's event. Stale results never satisfy a newer request.

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
- AVIF uses still-picture mode, `cpu-used=6`, `crf=12`, row multithreading, and BT.2020/PQ metadata. Every codec, mux, packet-write, drain, trailer, and close result is checked.
- Forced-SDR screenshots query the primary monitor's `DISPLAYCONFIG_SDR_WHITE_LEVEL`, use `(raw / 1000) * 80 nits`, place that white at 80% linear SDR with a smooth highlight knee, apply sRGB transfer, and write PNG color metadata as full-range RGB/BT.709/sRGB. A logged 203-nit fallback is used only when Windows does not expose the value.

PNG/AVIF files use the shared collision-safe output reservation described in `recording-output-paths.md`. Encoding occurs in an owned staging file; publication and overlay notification happen only after flush/close and atomic commit.

## Backend Invariants

D3D9, D3D10/11, D3D12, OpenGL, and Vulkan all route mapped pixels through the same request-ID worker boundary. Graphics readback may remain synchronous, but filesystem work is not detached and the mapped memory is never referenced after the backend returns. There is no file-size polling, stable-size heuristic, fixed sleep, unchecked BMP parsing, or replace-existing write to an unreserved destination.

Modern inject backends resolve the presentation contract rather than the texture format alone: successful DXGI `SetColorSpace1` calls distinguish SDR R10, HDR10/PQ R10, and linear scRGB; Vulkan uses the swapchain's `VkColorSpaceKHR`. The WGC fallback passes the captured frame's HDR state into R10/FP16 payload classification. Unsupported format/color-space combinations fail instead of publishing a wrong-color file.

Desktop fallback must not wait 15 seconds merely because the injector service exists. The host validates the shared mapping and skips the hook request immediately when `GetSourcePid()==0`. HDR desktop fallback uses WGC. A split-device WGC frame is a consumer-side shared texture published at keyed-mutex key 1: screenshot readback queries the texture's owning D3D11 device/context, acquires key 1 before `CopyResource`/`Map`, and releases key 0 afterward. Using the bootstrap device or omitting mutex ownership can report a successful readback of the allocation's initial all-black contents.

## Diagnostics And Tests

`ScreenshotHookWorkerTest` covers successful `.part` to `.ready` publication and worker failure. `ScreenshotEncodingTest` covers header validation, exact/truncated reads, dimension and size boundaries, deterministic SDR/PQ/scRGB conversion, and publication failure. `ScreenshotAvifTest` decodes a produced AVIF and verifies dimensions, 10-bit 4:4:4, BT.2020/PQ metadata, and bounded pixel error. Collision tests live in `tests/test_reserved_capture_output.cpp`.

Build `0.1.5116` was validated live on the HDR desktop. Native `auto` produced a non-black 3840x2160 AVIF with AV1 High, `yuv444p10le`, full-range BT.2020-NCL/PQ metadata and a visually correct decoded tone-map preview. Forced `bt709` produced a visually correct 3840x2160 RGBA PNG with full-range RGB, BT.709 primaries and sRGB transfer; the 16-worker 4K CPU tone-map took 57.833 ms and the full WGC-readback/conversion/PNG publication completed in about 450 ms. Both modes logged keyed-mutex acquisition, and the idle injector caused no hook wait.

## Open Questions / Stale-risk

- Fresh runtime validation is still useful across injected graphics backends and overlay-included HDR game screenshots. Per the user, the remaining inject check is manual; no more interactive test applications should be launched by an agent for this task.
- AVIF interoperability should be rechecked whenever FFmpeg or libaom is upgraded; the source-built encoder and its 4:4:4 capability are part of the packaging contract.
- Native AVIF is high-quality lossy (`crf=12`), not mathematically lossless, and forced-SDR PNG is an 8-bit output. Neither path promises that a hostile synthetic gradient can never reveal quantization; the live checks establish correct color/metadata and absence of the prior black/channel-corruption failures.
