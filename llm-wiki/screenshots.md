# Screenshot Capture And Publication

Last cross-checked: 2026-07-15 (shared ABI 32, request-specific completion, raw payload v2, WIC PNG, and libaom 10-bit 4:4:4 AVIF)

Primary sources:
- `common/shared_defs.h`
- `hook/common/screenshot_hook.{h,cpp}`
- `captureengine/screenshot.{h,cpp}`
- `captureengine/screenshot_encoding.{h,cpp}`
- `tests/test_screenshot_encoding.cpp`
- `tests/test_screenshot_hook_worker.cpp`

## Summary

Injected screenshots use shared-memory ABI 32 and a generation-like request ID. The controller publishes the request ID, pending status, raw path, and a unique completion-event name. The hook copies mapped pixels before returning from the graphics backend and queues one managed filesystem worker. The worker writes a request-bound raw payload to a new `.part` file, flushes and closes it, atomically renames it to `.ready`, publishes the matching result, and signals that request's event. Stale results never satisfy a newer request.

The request state is explicit: request ID, completed request ID, `Idle|Pending|Writing|Succeeded|Busy|Failed`, Windows error, payload kind, raw path, and completion-event name. A busy worker produces an explicit `Busy` result rather than spawning a detached writer or racing the existing task.

## Raw Payload ABI

`ScreenshotRawHeaderV2` is an exact 64-byte packed header with magic `CSR2`, version 2, header/payload/total sizes, dimensions, row pitch, pixel format, color encoding, and request ID. Supported pairs are:

- BGRA8 or RGBA8 with sRGB.
- R10G10B10A2 with BT.2020/PQ.
- RGBA16F with linear scRGB.

Both producer and consumer require dimensions `1..16384`, checked 64-bit size arithmetic, a format-aligned row pitch no smaller than the packed row, bounded extra pitch, an exact payload size, and an exact file size. Reads must be complete. Corrupt, truncated, mismatched-request, unknown-format, and overflowed payloads are rejected before allocation or encoding.

## Encoding And Color

- SDR payloads are encoded through WIC as PNG.
- HDR payloads are converted under strict floating-point flags into `YUV444P10LE` and encoded as AVIF with source-built `libaom-av1`.
- Packed R10 input is treated as full-range BT.2020/PQ RGB and converted directly to full-range BT.2020/PQ YUV.
- FP16 scRGB is interpreted as linear BT.709, transformed to linear BT.2020, scaled at 80 nits per scRGB unit, then encoded with ST.2084 before YUV conversion.
- AVIF uses still-picture mode, `cpu-used=6`, `crf=12`, row multithreading, and BT.2020/PQ metadata. Every codec, mux, packet-write, drain, trailer, and close result is checked.

PNG/AVIF files use the shared collision-safe output reservation described in `recording-output-paths.md`. Encoding occurs in an owned staging file; publication and overlay notification happen only after flush/close and atomic commit.

## Backend Invariants

D3D9, D3D10/11, D3D12, OpenGL, and Vulkan all route mapped pixels through the same request-ID worker boundary. Graphics readback may remain synchronous, but filesystem work is not detached and the mapped memory is never referenced after the backend returns. There is no file-size polling, stable-size heuristic, fixed sleep, unchecked BMP parsing, or replace-existing write to an unreserved destination.

## Diagnostics And Tests

`ScreenshotHookWorkerTest` covers successful `.part` to `.ready` publication and worker failure. `ScreenshotEncodingTest` covers header validation, exact/truncated reads, dimension and size boundaries, deterministic SDR/PQ/scRGB conversion, and publication failure. `ScreenshotAvifTest` decodes a produced AVIF and verifies dimensions, 10-bit 4:4:4, BT.2020/PQ metadata, and bounded pixel error. Collision tests live in `tests/test_reserved_capture_output.cpp`.

## Open Questions / Stale-risk

- Fresh runtime validation is still useful across every graphics backend, especially HDR desktop/game swapchains and overlay-included screenshots.
- AVIF interoperability should be rechecked whenever FFmpeg or libaom is upgraded; the source-built encoder and its 4:4:4 capability are part of the packaging contract.
