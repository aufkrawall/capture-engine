# llm-wiki Log

### 2026-11-06 - Smooth Motion status from NvPresent's worker runs

- **Report:** the status is often lost or never appears, depending on the frame rate. Session `20261106_171629`, Trine 5 DX11, 0.1.7434: 2x for 22 s, status off after 4 s.
- **Root causes:** NvPresent's D3D11 device was CE-wrapped through an import patch, so its draws and CE's overlay counted as the game's submissions and every present read as an application frame. An undecided window (below 20 fps) cleared the status. `DetectPattern` and the windows fought each other.
- **Decisive evidence:** per-present thread IDs reconstructed from the flood in both Trine sessions: 51574 presents in 25711 runs, 25639 of length 2. NvPresent64's RTTI has no swapchain class to find the proxy's vtable by.
- **Fix:** `present_interposer_evidence.h` (application presents, then worker runs, then submissions), status latch, longer windows for slow games, no wrappers in NvPresent's pipeline, no AF bootstrap on its chain, on-screen output rate, every output in the presentation series. Vulkan's layer feeds the same path. Details in `present-interposers.md`.
- **Verified by replay:** those sessions' thread sequences through `InterposerEvidence` and `GenerationLatch`: 305 windows over nine game runs, all decided 2x, status never lost, 20 to 234 base fps.
- **Not run on hardware.** Stale-risk: the runs are a driver implementation detail; the Vulkan layer order is inferred.

### 2026-11-06 - AMF B-frame configuration: emit both "bf" and "max_b_frames"

- **Symptom:** Setting `[Video] b_frames` (> 0) worked with NVENC but produced 0 B-frames with AMD AMF (`h264_amf`, `av1_amf`), requiring explicit `custom_options="bf=N"` for B-frames to work on AMF.
- **Root cause:** In FFmpeg's `libavcodec/amfenc_h264.c` and `amfenc_av1.c`, the private AVOption `"max_b_frames"` sets `AMFEncoderContext::max_consecutive_b_frames`, whereas `"bf"` sets `AMFEncoderContext::max_b_frames`. In non-adaptive mode, AMF initialization inspects only `ctx->max_b_frames != -1` to program `AMF_VIDEO_ENCODER_B_PIC_PATTERN`, `AMF_VIDEO_ENCODER_MAX_CONSECUTIVE_BPICTURES`, B-frame reference, and delta QP properties. Furthermore, `amfenc.c` uses `ctx->max_b_frames` to calculate `dts_delay` and `output_delay`. Because CE only passed `"max_b_frames"` into dictionary options and `amfenc` does not read `AVCodecContext::max_b_frames`, `ctx->max_b_frames` remained at `-1`, causing AMF to disable B-frames entirely.
- **Quick Sync check:** Intel Quick Sync Video (`h264_qsv`, `hevc_qsv`, `av1_qsv`) is unaffected: it has no private `bf` or `max_b_frames` options shadowing the context and consumes `AVCodecContext::max_b_frames` directly in `libavcodec/qsvenc.c` to compute `GopRefDist = FFMAX(-1, avctx->max_b_frames) + 1`.
- **Fix:** In `mediaengine/video/video_encoder_backend_options.cpp`, generate both `"bf"` and `"max_b_frames"` for H.264 and AV1 AMF encoders. Added unit test `VideoEncoderOptionsTest.AmfAndQuickSyncBFrameOptionMapping`.

### 2026-11-06 - DXGI retry review and live Mirror's Edge/DXUP/Steam validation

- Review of `41938bd4` and `2531374e`: retain strict system-DXGI discovery; admit factory bootstrap before its mutex only outside loader callouts and outside active/resident CE Vulkan ownership. Two independent guard mutations failed the new behavioral tests; restoring the guards passed. Full native tests and all 197 isolated FG scenarios passed. A transient atomic-rename access error in the unrelated toolchain-state self-test did not reproduce in its focused retry or the final unfiltered native/Python test pass; product resume produced setup 0.1.7433.
- User session `20261106_151030`, 0.1.7433, Mirror's Edge x86 + DXUP + Steam: initially absent native DXGI resolved at 15:10:36.610; native factory body hooks installed by .646; DX12 overlay initialized at 15:10:40.192 and rendered 3,258 frames under the correct `DX9 -> DX12 (DXUP)` label. Normal exit, no dump/device removal. SetColorSpace1's refused body hook recovered through a successful inline hook. FG was off and no recording ran.
- One incoming-Present gap of 274.682 ms at 15:10:44.364 (CSV frame 1171) is not explained by the measured hook work: adjacent CSV processing times were 0.152/0.575/0.129 ms and forwarded Present times 0.218/0.228/0.220 ms; no fence wait. Its cause remains unproven. After the first second, CSV processing median/p95/p99 were 0.121/0.172/0.248 ms over 3,101 frames. This short run validates startup/overlay recovery, not recording sync or live FG switching.

### 2026-11-06 - Late dxgi.dll module-load retry for factory vtable and swapchain hooks (Mirror's Edge on DXUP)

- **Session `20261106_131542` and `20261106_135827`, Mirror's Edge x86 with DXUP and Steam overlay:**
  When DXUP's `dxgi.dll` was present in the game directory alongside `d3d9.dll`, the CE in-game overlay did not appear and logged 0 frames.
- **Root cause:**
  DXUP's `d3d9.dll` explicitly calls `GetSystemDirectoryA` and `LoadLibraryA("C:\Windows\system32\dxgi.dll")` to create its DXGI factory and swapchain on the system runtime. When `dxgi.dll` is not in the game folder, Steam overlay loads system DXGI at startup, so `InstallGlobalVTableHooks()` finds it immediately on its initial HookThread pass. But when DXUP's `dxgi.dll` is also in the game folder, Steam loads that copy at startup, and system DXGI is only loaded later when `d3d9.dll` executes its first `Direct3DCreate9` call.
  Previously, `InstallGlobalVTableHooks()` deferred when system DXGI was not loaded yet at startup, but was never retried: `CheckAndInstallHooks()` did not invoke it, `NotifyHookModuleLoaded()` had no `dxgi.dll` handler, and `DX12Hook::Init()` was deferred by `ShouldSuppressSpeculativeDX12Bootstrap` until D3D12 device evidence was seen (which required the very swapchain creation hooks that were deferred).
- **Fix in `main_overlay_detect.cpp` and `main_install.cpp`:**
  1. `NotifyHookModuleLoaded()` triggers `InstallGlobalVTableHooks()` on completed `dxgi.dll` loads. The 2026-11-06 review added shared installer admission before its mutex: nested returns still inside DllMain defer to the hook thread (`RtlIsThreadWithinLoaderCallout`); ordinary completed loads remain synchronous. Vulkan ownership or a resident CE Vulkan layer also veto bootstrap before renderer publication. Tests inject these contexts and cover recovery; the subsequent live DXUP/Steam result is recorded above.
  2. `CheckAndInstallHooks()` retries `InstallGlobalVTableHooks()` when `!s_vulkanActive`, ensuring fast recovery if module notifications were missed.
  3. `GetSystemDXGIModuleHandle()` continues to resolve system DXGI strictly by full path under `GetSystemDirectoryA`, avoiding premature binding to unused proxy images in the game folder.
- **Verified:**
  Unit tests `Dx12FactorySlotPolicyTest.ColdDxgiLoadTriggersFactoryVTableHookInstall`, `Dx12FactorySlotPolicyTest.*`, `DXGISharedSourceTest.*` all compile and pass.

### 2026-11-05 - Overlay review: viewport wrapping and retained hardware detail slots

- Hero/List/Meters ignored the viewport limit, clipping a 656 px Hero panel in a 640 px window at 200% scale. Sections now wrap at their original atlas sizes; bars and the graph remain. Detail fields also lost their slots when samples became unavailable: `HardwareHistory` now owns per-renderer presence, with reserved field widths and `--` placeholders, including retained clock rows.
- Regression evidence: real-atlas bounds, overlap and value checks at 100/150/200%, all layouts through dropout/recovery/reset, long unbroken names, blink stability, all four adapter corners and graph-only redraws. CPU previews inspected at 600 px / 200%; no GPU/game run. Current policy and limits in `overlay-rendering.md`.

### 2026-11-05 - Overlay layouts: hero, list, meters, strip; the old row renderer is gone

- **Why:** the user found the overlay crowded and slow to search. FPS sat on row 7 of 10, one bold size carried everything, seven hues signalled nothing, and values had no columns.
- **What:** `overlay_panel` builds a positioned scene per `[Overlay] layout`; hero is the default. The atlas gained real sizes, fixed digit cells, a degree glyph and working font detection, and `font`, `font_weight`, `font_size` now work. Current rules in `overlay-rendering.md`.
- **Verified:** unit tests plus BMPs from the real renderer, atlas and CPU rasterizer at 150% on dark and white backgrounds, in the recording, FG and stress states. Not run in a game or on any GPU backend.
- **Config:** `bg_alpha` now defaults to 0.72 and `frametime_color` to `fps_color`, but a config.ini written from the old template keeps 0.50 and 00FF00.
