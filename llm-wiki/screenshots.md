# Screenshot Capture And Publication

Last cross-checked: 2026-10-18 (published PNGs are opaque whatever the source alpha; earlier 2026-10-13: native FSR screenshot retirement survives failed signals, UI alpha follows registration, and post-processing precedes optional overlay drawing; WARP shader/readback regressions added; earlier backend/publication invariants retained).

Primary sources:
- `common/ipc/shared_defs.h`
- `hook/capture/screenshot_hook.{h,cpp}`
- `hook/ddraw/ddraw_hook_capture{,_frame}.cpp`
- `hook/ddraw/ddraw_hook_overlay_route.cpp`
- `hook/d3d12/dx12_hook_screenshot.cpp`
- `hook/d3d12/dx12_hook_postsl_render_submit.cpp`
- `hook/d3d12/dx12_hook_fsr_screenshot.cpp`, `hook/d3d12/dx12_hook_ffx_proxy_present.cpp`,
  `hook/d3d12/dx12_ffx_suspend_overlay.{h,cpp}` (native FSR no-callback route)
- `hook/d3d12/fsr_screenshot_{composite,policy}.h`, `tests/test_fsr_screenshot_{d3d12,policy}.cpp`
- `captureengine/media/screenshot.{h,cpp}`
- `captureengine/media/screenshot_encoding.{h,cpp}`
- `captureengine/media/screenshot_hdr_encoding.cpp`
- `common/capture/reserved_capture_output.{h,cpp}`
- `tests/test_screenshot_encoding.cpp`
- `tests/test_screenshot_source.cpp`
- `tests/test_screenshot_hook_worker.cpp`

## Summary

Injected screenshots use the current exact shared-memory ABI 38 and the generation-like request-ID protocol introduced with ABI 32. The controller publishes the request ID, pending status, raw path, and a unique completion-event name. The hook copies mapped pixels before returning from the graphics backend and queues one managed filesystem worker. The worker writes a request-bound raw payload to a new `.part` file, flushes and closes it, atomically renames it to `.ready`, publishes the matching result, and signals that request's event. Stale results never satisfy a newer request.

The request state is explicit: request ID, completed request ID, `Idle|Pending|Writing|Succeeded|Busy|Failed`, Windows error, payload kind, raw path, and completion-event name. A busy worker produces an explicit `Busy` result rather than spawning a detached writer or racing the existing task.

`[Screenshot] color_space=both|auto|bt709` controls the published screenshot rather than the capture source. `both` is the default and publishes an HDR source twice from the one capture: the preserved 10-bit BT.2020/PQ AVIF and the tone-mapped SDR PNG. `auto` preserves an HDR source only as AVIF and writes ordinary SDR as PNG. Explicit `bt709` always publishes a conventional SDR PNG; HDR FP16 scRGB and packed PQ inputs are tone-mapped using the same Windows-SDR-white-calibrated luminance knee and luminance-preserving Rec.2020-to-Rec.709 gamut compression as forced-SDR video. Invalid values log and fall back to `both`. This is independent of `[Video] color_space`.

## Combined HDR And SDR Publication

Both capture routes converge on one `RawScreenshot` before any encoding: the inject hook publishes a raw payload the controller reads back, and WGC/DXGI readback fills the same structure. The combined mode therefore needs nothing from the backends — it encodes the single captured payload twice, so the two variants are pixel-identical in origin and neither route can produce a different pair.

`IsHdrScreenshotSource` decides whether a second variant exists at all, from the payload's declared contract (R10 with BT.2020/PQ, or FP16 with linear scRGB HDR) rather than from storage precision. A capture taken while Windows and the game are in SDR has no HDR variant to preserve, so every policy publishes exactly one PNG for it; the combined mode never fabricates an AVIF out of SDR pixels.

The two encodes run concurrently. The AVIF branch takes the worker thread because it touches no COM, while the WIC PNG writer stays on the caller's already-initialized apartment; a failed `std::thread` construction logs and falls back to sequential encoding instead of dropping a variant. The AVIF pipeline dominates the wall clock by roughly an order of magnitude (about 950 ms versus about 250 ms at 4K per the measurements below), so the pair costs little more than the HDR-only case. Both row-parallel conversions keep their existing bounded worker policy; the brief overlap of two `min(16, hardware_concurrency)` row passes is short relative to the libaom encode that is the actual critical path.

Both variants publish through one `OutputNameSeed` from `ce::capture_output::MakeOutputNameSeed()`, so the pair shares a published name and differs only by extension. Distinct extensions cannot collide under a shared seed, and the ordinary collision-retry loop still applies. A partial result is reported as a failure — the log names which variant failed — but whatever did publish is already a complete, atomically renamed file and is kept rather than deleted. `TakeScreenshot` returns a `ScreenshotResult {saved, paths}` (2026-10-19, library M6): `paths` lists the published files as UTF-8, HDR AVIF first then SDR PNG (`PublishedScreenshotPaths`), empty when nothing published, and non-empty with `saved == false` for that kept partial variant. The controller logs the collapsed paths; notification behavior is unchanged.

## Raw Payload ABI

`ScreenshotRawHeaderV2` is an exact 64-byte packed header with magic `CSR2`, version 2, header/payload/total sizes, dimensions, row pitch, pixel format, color encoding, and request ID. Supported pairs are:

- BGRA8 or RGBA8 with sRGB.
- R10G10B10A2 with BT.2020/PQ.
- R10G10B10A2 with SDR BT.709/G2.2 or sRGB.
- RGBA16F with linear scRGB, distinguished as HDR scene content or an SDR presentation.

**Published screenshots are opaque; the payload's alpha is never image data.** A swapchain's alpha is ignored by
scanout and DWM and GDI leaves it undefined, but until 2026-10-18 the 8-bit BGRA/RGBA path copied it into the 32bpp
PNG. BioShock Infinite x86 DX11 (`20261018_020443`) keeps stale loading-screen UI in its back buffer's alpha, so its
screenshots viewed washed out and partly transparent with ghost "Loading..." text. `ConvertSdrToTightBgra()` writes
alpha 255 for every format and logs `[Screenshot] Source alpha ignored: N of M pixels not opaque (min alpha A)`;
`SavesEightBitSourcesOpaqueWhateverTheirAlpha` decodes the PNG through WIC (fails without the fix).

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
- `screenshot_include_overlay` decides *where* in the Present the D3D12 copy is taken, not whether a copy is filtered afterwards. Whoever draws the overlay owns that ordering, and under DLSS/FSR frame generation that is the PostSL callback — which runs **earlier in the same Present than `ProcessFrame`** (`ExecuteStartupRouting` precedes `ExecutePresentCore`). So when `PostSLOwnsThisFramesOverlayDraw()` is true, both variants are taken inside the PostSL submit chunk: the overlay-free copy immediately ahead of the overlay list on the same queue, the overlay-included copy after it. `ProcessFrame` yields both to PostSL in that case; taking the overlay-free shot from `ProcessFrame` captured a backbuffer the overlay was already drawn into (session `20260818_205615`). The submit chunk has sixteen mutually exclusive submit branches, and `ScreenshotPresentThreadPolicyTest` asserts every one of them is immediately preceded by the capture call for the same queue.

DLSS-G suspension changes who owns a particular Present without immediately retiring the proven PostSL backend. The global PostSL active/confirmed latches therefore are not sufficient screenshot evidence: a cutscene cooldown or a contended render lock can make that callback return before it submits, and yielding solely from those latches strands the pending request. Inside a real presented-output callback PostSL still owns ordering; after it returns, ProcessFrame yields only when that same Present records a successful inactive PostSL draw. Otherwise ProcessFrame takes the still-pending request on its live route. The overlay-included tail rechecks that the request remains Pending after `ProcessFrame`, preventing an in-ProcessFrame fallback callback from submitting the same request twice.

Injected overlay pixels must already obey the captured swapchain contract. Session `20260719_214733` showed the CPU screenshot tone map correctly retained the game while a secondary DX12 adapter had written sRGB endpoints directly into the PQ target; those nominal 10,000-nit saturated pixels were necessarily gamut-compressed toward white. Synchronizing that adapter's HDR/format state fixes the producer. A deterministic packed-RGB10 fixture proves correctly encoded 320-nit Rec.709 green returns as saturated SDR green with opaque PNG alpha.

PNG/AVIF encoding occurs in an owned `.part` reservation. After flush and close, that exact file is moved atomically to a fresh collision-safe `.png`/`.avif` name without ever exposing a zero-byte final-extension placeholder. A destination collision is preserved and retried under a suffixed name; no existing output is replaced. Success notification occurs only after publication, while a failed capture now produces an explicit red `Screenshot failed!` notification in both pseudo and inject overlays instead of looking like an ignored hotkey.

## Native FSR No-Callback Route

Native FSR frame generation with AMD owning the swapchain (no app present callback) draws CE's overlay through the
UI-resource route (`active-ui-resource`) or the topmost same-batch route. On that route the presenter-thread
`DetourPresent` takes `kSkipBundleCovers` and runs neither `ProcessFrame` nor PostSL, so none of the servicing points
above ever sees the request. Before this route had its own servicing, the request stayed Pending until the 15-second
hook timeout (`Hook request N failed (wait=258 ...)`: 258 is `WAIT_TIMEOUT`), then the GDI desktop fallback ran in
about 1 s (session `20261012_194528`, GTA5 Enhanced, SDR `B8G8R8A8_UNORM` swapchain).

`DX12_ServiceFSRNoCallbackScreenshotIfPending` (`hook/d3d12/dx12_hook_fsr_screenshot.cpp`) services the request from the
FFX proxy-present prework on the game thread, before the overlay composite writes CE onto the game's UI resource:

- Scene: the proxy backbuffer (the application frame), `PRESENT -> PIXEL_SHADER_RESOURCE -> PRESENT`.
- UI: the registered game UI resource in its FFX state, excluding the CE substitute (overlay storage, not HUD). Blended
  with straight alpha, `ui.rgb * ui.a + scene.rgb * (1 - ui.a)`, or with premultiplied alpha,
  `ui.rgb + scene.rgb * (1 - ui.a)`, according to `g_CachedFFXUiFlags` / `kUiCompositionUsePremulAlpha`.
- Post-processing: `RecordPostProcess` appends the configured sharpen/display-gamma pass to the completed scene/UI
  composite's command list, before closing/submitting it. This applies independently of overlay inclusion; HDR
  gamma-only requests remain passthrough. Borrowed recordings are abandoned if the list cannot be submitted.
- Overlay (when `screenshot_include_overlay` is set): `ce::dx12_ffx_suspend_overlay::Render` with `screenshotSlot = true`
  on the renderer's reserved slot `kScreenshotSlot = kFrameSlotCount - 1`. Backbuffer-indexed and inline final-batch
  slots stay below it. An in-flight reserved slot refuses the render; the request retries on a later prework (at most
  60 Presents) instead of waiting.
- Readback: the existing `SaveDX12TextureAsScreenshotRaw` path, asynchronous and completed by the worker.
- Lifetime: each capture's command lists, heaps, pipelines and target are retained in `RetiredCaptures` before ECL;
  the post-submit guard allocates no retention storage. The same queue signals a retirement fence after the last
  submission. `GpuCompletion` requires both a successful signal and its completed value; failed signals retain the
  generation until device removal (`UINT64_MAX`). Collection runs on every Present, and pending references survive
  loader-lock destruction. Never-submitted entries are removed immediately. The swapchain backbuffer
  and the game's UI resource are not retained: holding a swapchain buffer reference past submission makes the game's
  `ResizeBuffers` fail, so their local references end on return, as in the normal screenshot path.

Invariants: nothing waits on the GPU on a Present path; the owner queue is the game queue, never AMD's FG queue;
nothing runs unless a request is pending. `FSRScreenshotPolicyTest` (`tests/test_fsr_screenshot_policy.cpp`) pins the
prework order, the reserved slot, no GPU waits, failed/successful signal completion, constant wiring and processing
before overlay inclusion. `FSRScreenshotD3D12Test` uses the production composite shader and pipeline defaults
(`hook/d3d12/fsr_screenshot_pipeline.h`) on WARP. Disabled blend/depth/stencil stages carry valid enum defaults;
the pixel tests cover both alpha conventions and absent/transparent UI, and verify gamma/CAS/RCAS with/without overlay plus
disabled/HDR passthrough. Restoring the defective completion/blend/processing behavior makes its targeted
regression fail. Diagnostics use `ChangeGate`: queued records name `uiAlpha` and `postProcess`; signal failures name
retained resources. No per-Present noise is added.

Open questions and gaps:

- The presenter fallback (`!proxyDriving`, FFX proxy hook not live) still does not service requests; they stall to the timeout.
- WARP verifies the registered alpha equations and SDR post-processing; actual HDR blending and AMD's displayed
  output still need live hardware comparison.
- The composite is the application frame, like the DLSS PostSL path. Alternate interpolated frames are not captured.
- Hardware validation is pending: no GTA FSR FG run has exercised this path yet.

## Backend Invariants

DirectDraw/Direct3D 7, D3D9, D3D10/11, D3D12, OpenGL, and Vulkan all route mapped pixels through the same request-ID worker boundary. DirectDraw consumes the request at `ComposePresentation`, before the runtime publishes the selected Flip/blit image. Standard BGRX32, BGR24, RGB565, and RGB555 surfaces convert directly to opaque BGRA8; unusual or non-lockable layouts use a one-request GDI DIB fallback. Both routes release the surface before queuing owned pixels, and neither waits for encoding. `screenshot_include_overlay=true` reads after the native or CPU overlay; `false` restores any still-matching CPU composite, reads first, then redraws for the screen. A pending excluded request suppresses the native D3D7 `EndScene` draw for that frame. If the request arrives after native pixels were already written, it remains Pending for the next real frame rather than publishing the wrong variant. Recording and screenshot inclusion phases are selected independently.

**The D3D12 readback is asynchronous and must stay that way**: the render thread records the backbuffer copy, submits it on the queue it is already presenting on, signals a fence and hands the resources to the worker. Waiting there froze the game outright under DLSS frame generation (session `20260818_172155`) — the copy rides the game's own swapchain queue, which a frame-generation runtime only drains once its presenter thread makes progress, and that thread cannot progress while the present call it drives is blocked inside our hook. The producer therefore reserves the single worker slot *before* recording anything, so a submitted copy can never be stranded by a busy queue; the worker waits on the fence in one-second slices, checks `GetDeviceRemovedReason()` between them, and strands (never releases) resources a live GPU may still be reading. The D3D12 producer obtains the device from the exact current swapchain resource, retains the selected queue for the submission, and rejects a queue whose COM device identity differs; it never combines a transition-stale global device with a fresh proxy backbuffer. PostSL passes the live base swapchain into this helper rather than retaining an already-released `IDXGISwapChain3` interface. The other backends still copy their mapped pixels before the backend returns, and the mapped memory is never referenced after that. There is no file-size polling, stable-size heuristic, fixed sleep, unchecked BMP parsing, or replace-existing write to an unreserved destination.

Modern inject backends resolve the presentation contract rather than the texture format alone: successful DXGI `SetColorSpace1` calls distinguish SDR R10, HDR10/PQ R10, and linear scRGB; Vulkan uses the swapchain's `VkColorSpaceKHR`. The WGC fallback passes the captured frame's HDR state into R10/FP16 payload classification. Unsupported format/color-space combinations fail instead of publishing a wrong-color file.

Desktop fallback must not wait 15 seconds merely because the injector service exists. The host validates the shared mapping and skips the hook request immediately when `GetSourcePid()==0` or when the source PID is no longer alive. The hook sets `sourcePid` but dies with its process, so a stale PID after a game exit would otherwise block the desktop fallback for the full 15-second hook timeout (session `20260809_165328`: hook screenshot in Robocop OK at 16:53:51, game exited 16:54:00, desktop screenshot 16:54:07 stalled until `Hook request 2 failed (wait=258 ...)` at 16:54:22). `TryHookScreenshot` now verifies source-process liveness before publishing a request (protected sources that deny even the limited query are treated as alive) and resets the request protocol state; the injector also clears the stale hook identity when it detects the source exited, and a failed/timed-out hook request resets the full protocol state so a later injected game never inherits a stale Pending/Writing status. HDR desktop fallback uses WGC. A split-device WGC frame is a consumer-side shared texture published at keyed-mutex key 1: screenshot readback queries the texture's owning D3D11 device/context, acquires key 1 before `CopyResource`/`Map`, and releases key 0 afterward. Using the bootstrap device or omitting mutex ownership can report a successful readback of the allocation's initial all-black contents.

## Diagnostics And Tests

`ScreenshotHookWorkerTest` covers successful `.part` to `.ready` publication and worker failure. `ScreenshotReservationTest`, `ScreenshotDx12ReadbackTest` and `ScreenshotPresentThreadPolicyTest` cover the exclusive worker reservation, single-owner readback move semantics, the producer never waiting or mapping, the DirectDraw presentation request handoff, the worker's sliced wait, every PostSL submit ordering branch, suspended-output fallback ownership, and exact resource/queue device validation. `DDrawPresentPolicyTest` covers before/after-overlay selection, all independent recording/screenshot inclusion combinations, and native-sidecar suppression for an excluded screenshot. `ScreenshotEncodingTest` covers header validation, exact/truncated reads, dimension and size boundaries, deterministic SDR/PQ/scRGB conversion, correctly encoded saturated overlay color, the bounded AVIF thread/tile plan, and publication failure. `ScreenshotColorTest` additionally covers HDR-source classification (ten-bit and FP16 SDR presentations are not HDR), the combined mode publishing a paired AVIF/PNG with one shared stem and no third file for both packed-PQ and scRGB HDR captures, and the combined mode publishing exactly one PNG for an SDR source. `ScreenshotAvifTest` decodes a produced AVIF and verifies dimensions, 10-bit 4:4:4, BT.2020/PQ metadata, and bounded pixel error. `ReservedCaptureOutputTest` proves placeholder-free atomic publication plus collision retry without changing the existing file.

Build `0.1.5116` was validated live on the HDR desktop. Native `auto` produced a non-black 3840x2160 AVIF with AV1 High, `yuv444p10le`, full-range BT.2020-NCL/PQ metadata and a visually correct decoded tone-map preview. Forced `bt709` produced a visually correct 3840x2160 RGBA PNG with full-range RGB, BT.709 primaries and sRGB transfer; the 16-worker 4K CPU tone-map took 57.833 ms and the full WGC-readback/conversion/PNG publication completed in about 450 ms. Both modes logged keyed-mutex acquisition, and the idle injector caused no hook wait.

Session `20260719_221759` exposed two independent latency/UX failures. Its first forced-SDR screenshot completed the 4K tone map in 161.672 ms but failed during the old combined WIC/flush/`ReplaceFile` publication path, so no success notification could appear. The native HDR request reached the encoder at 22:19:44.076 and published at 22:19:59.377, blocking the controller for 15.3 seconds. The new path logs WIC stage/HRESULT, flush Win32 status, HDR-to-PQ time, AVIF encode time, and total time separately. An offline same-frame-class benchmark encoded the old quality profile at 2.864 seconds/2.16 MB; realtime CPU-8 CRF-8 completed in 0.950 seconds/3.24 MB and improved average decoded PSNR from 46.73 dB to 49.51 dB. Fresh end-to-end timing remains a manual runtime check per the user.

## Open Questions / Stale-risk

- Fresh runtime validation is still useful across injected graphics backends and overlay-included HDR game screenshots. Per the user, the remaining inject check is manual; no more interactive test applications should be launched by an agent for this task.
- AVIF interoperability should be rechecked whenever FFmpeg or libaom is upgraded; the source-built encoder and its 4:4:4 capability are part of the packaging contract.
- Native AVIF is high-quality lossy (`crf=8`), not mathematically lossless, and forced-SDR PNG is an 8-bit output. Neither path promises that a hostile synthetic gradient can never reveal quantization; the live checks establish correct color/metadata and absence of the prior black/channel-corruption failures.
