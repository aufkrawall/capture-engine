# llm-wiki Log Archive 2026-W38b

### 2026-09-13 - CE was taking NVIDIA's native Vulkan present path away from the game (two causes)

**Report**: with the inject active, DOOM Eternal always presented through DXGI even with the driver's
Vulkan/OpenGL present method set to prefer native - visible as the Windows volume OSD compositing over the game.
Without CE it stayed native. Session `20260913_180809` confirms it from CE's own side: the driver called
`CreateSwapChainForHwnd` *inside* `vkCreateSwapchainKHR` (18:08:25.359, between the layer's entry at .097 and the
driver's return at .374), which CE logged three times as `Vulkan layer owns presentation - exact DXGI
swapchain-create pass-through`. That line is the authoritative "NVIDIA's WSI went layered" signal.

**Reproducer**: `build/vk-wsi-probe` (standalone, ~15 s, not part of any gate). It creates an ordinary Vulkan
FIFO swapchain and traces the ICD's own imports (`nvoglv64.dll` IAT: `GetProcAddress`, `LoadLibrary*`,
`GetModuleHandle*`, plus the GDI pixel-format and `D3DKMTEnumAdapters2` entries). The two paths are trivially
separable from inside the process:
- **native**: the ICD resolves `wglDescribePixelFormat`/`wglCreateLayerContext`/`wglShareLists`/`wglDeleteContext`/
  `wglMakeCurrent`/`wglSwapLayerBuffers`/`wglGetCurrentContext`, then loads `nvppex.dll` (`ppeGetVersion`,
  `ppeGetExportTable`) and `dispbroker.dll`/`winsta.dll`. It never touches D3D.
- **layered**: the same run additionally resolves `dxgi!CreateDXGIFactory2`, `d3d12!D3D12CreateDevice`,
  `dwmapi!DwmGetCompositionTimingInfo` and `dcomp!DCompositionCreateDevice3`, and maps `nvwgf2umx.dll`,
  `nvldumdx.dll`, `d3d12core.dll`, `dcomp.dll`.
Module-presence alone is not a detector: OBS's `graphics-hook64.dll` and RTSS's `rtssvklayer64.dll` map `dxgi.dll`
into every Vulkan process here regardless.

**Cause 1 - the Streamline preload (`streamline_dll_path`)**. `PreloadConfiguredGraphicsRuntimeDlls` mapped
`sl.interposer.dll`, `sl.common.dll`, `sl.dlss*.dll` into *every* injected process whose profile configured any
DLSS/Streamline override path, as a name-registration trick so later name-based loads resolve to CE's copies.
DOOM Eternal never loads Streamline. The probe reduces it to a single fact: **mapping `sl.interposer.dll` alone is
enough** for the ICD to build the layered presenter - it is how Vulkan DLSS-G has to present. The `nvngx_*.dll`
snippets are inert (probe `--preload-ngx`: native). Bisected away from every other suspect first: CE's added
device/instance extensions (`VK_KHR_external_memory_win32`, `external_semaphore_win32`, `timeline_semaphore`,
`get_physical_device_properties2`), the reserved overlay queue, DOOM's `imageUsage=0x1f`,
`VK_EXT_full_screen_exclusive`, a real D3D12 device plus DXGI flip swapchain in the process, CE's `opengl32`/
`gdi32` swap-entry inline hooks, and CE's `GetProcAddress` router (traced: it never intercepts the ICD's own
lookups) - all stayed native.
- **Fix**: `ce::graphics_runtime::ShouldPlaceStreamlinePluginSet` + `PlaceStreamlinePluginSet` in
  `hook/main_redirect.cpp`. The sl.* set is placed only once the process shows Streamline use - the core is
  already mapped, `sl.interposer.dll` ships beside the process image, or a sl.* load/request has been observed
  (`NoteStreamlineUseObserved`, latched from `GetRedirectedPath` and `NoteRuntimeModuleLoadedForOverridePolicy`).
  The deferred half runs from the hook thread's 100 ms monitor loop (`PlaceConfiguredStreamlinePluginSetIfObserved`),
  off the loader-lock path. The NGX snippets keep their eager placement. The loader redirect is unchanged and still
  serves the first real request, so a Streamline game gets the same copies as before.

**Cause 2 - `vsync_mode=fifo|adaptive`**. The layer asked for `VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT` on every
forced-FIFO swapchain (`abcafbeb`). The probe separates the pieces: the device extensions (`VK_EXT_present_timing`,
`VK_KHR_present_id2`, `VK_KHR_calibrated_timestamps`) and the feature node are **inert**; the swapchain flag alone
flips the path, because NVIDIA's native presenter cannot serve a present-timing swapchain.
- **Fix**: `ShouldEnableSwapchain` now also requires `meteredPresentationPossible` - the application enabled
  `VK_NV_present_metering` on this device (recorded at `vkCreateDevice`). That is the only case where a FIFO
  swapchain can outrun its display; a plain FIFO swapchain already waits for the vertical blank, so the flag bought
  nothing and cost the native presenter.

**Validated**: `installed/testapp/vulkan_test.exe` with a DOOM-shaped profile (`vsync_mode=fifo`,
`streamline_dll_path`, `dlss_sr_dll_path`) - `nativeTiming=0`, `flags=0x0`, zero DXGI swapchain-create
pass-throughs. The hold-back path is proven separately with the probe process (no Streamline beside it):
`Runtime preload: sl.* plugin set held back ... (mapped=0 shippedWithApplication=0 loadObserved=0)` and no sl.*
module in the process. The test app ships the whole sl.* set next to its exe, so it takes the
`coreShippedWithApplication` branch and still gets the full placement - the intended positive. **DOOM Eternal
itself is still unrun.**

**Found alongside, fixed separately** (see the D3DKMT entry below): `hook/wrappers/d3dkmt_hook.cpp` typed
`D3DKMT_HANDLE` as `UINT64`, so every field it read was at the wrong offset.

### 2026-09-13 - D3DKMT hook: the mirrored structures were off by eight bytes

`hook/wrappers/d3dkmt_hook.cpp` declared `D3DKMT_HANDLE` as `UINT64`; `d3dukmdt.h` defines it as `UINT32`. Every
member after the first handle was therefore shifted:
- `D3DKMT_QUERYADAPTERINFO::Type` read the low dword of `pPrivateDriverData` and `PrivateDriverDataSize` read past
  the structure. DOOM Eternal session `20260913_180809` shows it directly: `QueryAdapterInfo - Type=3086977864,
  Size=52413`, where 52413 is 0xccbd - the low half of the adapter LUID the Vulkan layer logged for the same GPU in
  the same session.
- `D3DKMT_QUERYVIDEOMEMORYINFO` was off by eight from `MemorySegmentGroup` onwards. `hProcess` is a `HANDLE`
  (8 bytes) and `hAdapter` a `UINT32`, not two 64-bit handles. The read-only path only mislogged, but the VRAM
  override branch writes `Budget`/`CurrentUsage`/`CurrentReservation`/`AvailableForReservation` back into the
  caller's structure and would have written the budget over `CurrentUsage` while reading `Budget`'s low dword as
  the segment group. Only `InitializeConfig` leaving the override off by default kept that latent.
- `D3DKMT_ADAPTERINFO` named its last two fields `VidPnSourceId`/`NodeCount`; the real ones are
  `NumOfSources`/`bPrecisePresentRegionsPreferred`, and `AdapterLuid` is a `LUID` (4-byte aligned, so it packs at
  offset 4 behind the handle), not a 64-bit handle.

**Fix**: the layouts moved to `hook/wrappers/d3dkmt_abi.h` with every offset and size pinned by `static_assert`,
documented as a mirror of `d3dkmthk.h`/`d3dukmdt.h` (those headers are not in the MSYS2 toolchain, so mirroring is
required - same arrangement as `vulkan_present_metering_policy.h` for `VK_NV_present_metering`). The hook now logs
real values (`hProcess` resolved to a PID, the adapter LUID, `NumOfSources`, `PhysicalAdapterIndex`), the dead
no-op switch in `Hook_D3DKMTQueryAdapterInfo` is gone, and `Hook_D3DKMTEnumAdapters` clamps its loop to
`MAX_ENUM_ADAPTERS` instead of trusting the reported count. `tests/test_d3dkmt_abi.cpp` asserts the offsets and
checks that a write to `Type` is what a 32-bit read at offset 4 returns.



### 2026-09-13 - DOOM Eternal black window: overlay views outlived their swapchain

Session `20260913_174040`: the first launch stayed black, the second worked. The layer log shows the startup
swapchain destroyed at 17:41:45.923 and the replacement created at .927, CE tearing its overlay state down at
.942-.951 inside the next `InitializeOverlay`, and the first present on the new swapchain failing with
`Vulkan Prerender: wait failed result=-4` plus `device loss latched from submission-slot fence probe` at 46.134.
The Windows System log pins the cause between the two: `nvlddmkm` event 153 ("Error occurred on GPUID: 700") at
45.9535, i.e. inside CE's own teardown. Capture was not involved - `RetireCaptureSwapchain` only moves state to a
retired list and owns no swapchain-derived objects.

Root cause: the overlay builds a `VkImageView` per presentable image, a `VkFramebuffer` over each, and compute-route
descriptor sets and command buffers bound to them, and CE released all of it at the *next* `vkCreateSwapchainKHR`.
Presentable images die with their swapchain, so between the game's destroy and its next create CE held views over
freed images and then handed those stale views back to the driver. That is a use-after-free, which is exactly why
the second launch of the same build survived the identical sequence.

Fix (0.1.6537): `Capture_vkDestroySwapchainKHR` now calls `ReleaseOverlayForSwapchain` before the driver destroy,
gated by `ce::overlay_swapchain_lifetime::Decide` - release only the state whose recorded `OverlayState::swapchain`
is the one being destroyed, and skip the device-idle wait on a latched device loss. `OverlayState` gained the
`swapchain` field that makes that identification possible. Seven regression tests cover the policy plus the source
ordering (release before `fp_vkDestroySwapchainKHR`, and `InitializeOverlay` recording the owning swapchain).
`--verify` passed. Hardware re-check pending: a cold DOOM Eternal start has to survive the startup swapchain
recreate several times over, and the log should show `Releasing overlay state built over swapchain ...` instead of
`InitializeOverlay - Existing state found`.

### 2026-09-13 - DOOM Vulkan compute-present capture and authoritative freeze evidence

DOOM Eternal recording `20260913_163446` is a healthy 3840x2160/120 inject capture from a source capped near
140 FPS. The compute-present compositor stayed active after the live swapchain moved from graphics family 0 to
compute family 2. Present cadence averaged 7.148 ms with a 9.442 ms maximum gap; capture CPU averaged 40.8 us
(p95 50 us). The 62.875 s output has exactly 7,545 video packets, no encoder skip/duplicate/backpressure, and two
48 kHz audio tracks of exactly 3,018,000 samples each. Strict analysis found no media/audio/visual fault; only the
bounded startup-publication backlog and external-overlay contexts.

The configured 140 FPS general cap plus disabled capture sync intentionally produces timestamp-nearest 140-to-120
decimation: 1,258 candidates were superseded, with zero missing CFR slots/duplicates and a 3.726 ms maximum residual.
For absolute motion uniformity, capture sync multiplier 1 is preferable because it makes source and output cadence
120-to-120; retaining 140 is a valid gameplay-latency/source-choice tradeoff.

The trace exposed two generic hot-path issues. The common one-semaphore Vulkan overlay/capture/present chain allocated
three temporary vectors per captured frame; it now uses inline storage and retains allocation only for uncommon
multi-wait submissions. The swapchain also changed present family without recreation after bounded prerender topology
learning had ended, which could leave `cpu_prerender_limit=1` attached to the startup route. A stable queue now costs
one atomic comparison, while a live family move retires the cached producer decision and safely re-arms bounded
dependency learning. Focused capture/overlay/prerender tests pass.

Freeze session `20260913_154630` confirms a separate false-positive family. Vulkan presents stopped normally at
15:57:01 and resumed on the same game instance at 15:59:33, but a historical D3D12 ECL helper heartbeat kept the old
watchdog armed and it dumped at 15:57:33. The named last-present worker (tid 20308) was merely waiting on an idTech
event; its stack contained no CE, Vulkan, or driver stall. While the Vulkan layer owns final presentation, only a
currently published `vkQueuePresentKHR` is now authoritative: a truly stuck call remains published and targetable,
whereas a returned worker cannot trigger a timeout dump. Worker-pool target-switch logs are rate-limited and status
reports historical versus current evidence explicitly. Focused watchdog policy tests pass.

### 2026-09-13 - Front-loading has to budget the GPU half too, or it buys nothing

Run `20260913_132320` showed the overrun controller doing its job - `overruns=3` total, `headroomUs` decaying
23 -> 2 us, late-frame rate back to 0.30% from 0.63% - and the 1%/0.1% low only recovering 84.9 -> 85.0 and
83.2 -> 84.0. So missed deadlines were not the main jitter source.

The two timelines disagreed, which is the clue. On the PRESENT timeline front-loading was already BETTER than the
back edge (stddev 194 -> 169 us, |frame-to-frame delta| 189 -> 126 us, p99.9 11911 -> 11740 us). On the DISPLAY
timeline it was worse (published 1% low 86.9 -> 85.0, stddev 148 -> 202 us). The overlay publishes percentiles from
`m_display` (screen times) when the effective source is `DisplayChange`, and screen time is the right thing to
measure - so the regression was real and the CSV comparison was the misleading one.

Root cause: the budget covers the CPU half of a frame, but the flip cannot happen until the GPU half finishes.
Strange Brigade DX12 is GPU-bound - ~1.8 ms CPU in front of ~8.5 ms GPU - so a CPU-sized budget released the game
far too late and the GPU ran past the deadline. `presentToDisplay` rose 0.4 -> 6.8 ms, and with the screen time then
set by GPU completion instead of by CE's grid, the game's own frame-to-frame variance landed directly on the display
timeline.

The algebra says the placement is worthless below that threshold. With L = input-to-photon, B = budget, W = whole
CPU+GPU work, F = irreducible flip latency: `B >= W` gives `L = B + F` and grid-pinned screen times; `B < W` gives
`L = W + F` and GPU-driven screen times. Shrinking B below W buys **zero** latency and pays for it in jitter. The
optimum is exactly `B = W`. Numbers for this session: W ~= 8.8 ms (independently consistent with the p2d excess and
with the measured latency delta), so the optimum budget ~= 9.1 ms costs ~0.3 ms against the current 2.1 ms budget
and buys back the whole percentile regression, while still sitting ~2 ms below the back edge.

Also note the published latency estimate over-reports the front-load gain: `anchorToPresent` is
`modelled base interval + measured hold`, so CE's own hold is counted twice. The true back-edge-to-front-load gain
is ~0.8 ms, not the 2.2 ms the overlay showed. The estimator limitation is documented in
`system_latency_frame_begin.h`; it was deliberately NOT touched here, because changing the measurement in the same
change as the behaviour would make the next A/B unreadable.

Fixes:
- `ResolveFrontLoadGpuExcessUs()` / `UpdateFrontLoadGpuHeadroom()`: grow the reservation by the measured excess of
  present-to-display over its own floor, fed from `PerformanceMetrics::ConsumeDisplayTiming` (one call; the overlay's
  own metrics are untouched). `DecayFrontLoadGpuHeadroomUs()` walks it back in by one timer margin per clean
  64-frame window - a bounded probe, not a proportional decay that would periodically put the GPU a large step past
  the deadline just to discover it no longer needs to be there.
- `HasUsableGpuCompletionEvidence()`: no seeded present-to-display floor, no front-loading. The floor may only be
  seeded while the placement is at the back edge, the only state in which the GPU is known to have finished before
  the present. Without the evidence the back edge stays the default and the withholding is logged.
- `SmartWait()` no longer arms the kernel timer for less than a scheduler tick. `EnsureTimerResolution()` puts the
  scheduler on a 1 ms tick and a shorter arm cannot land inside it. Invisible while the limiter's waits were whole
  milliseconds; front-loading made the pre-present wait hundreds of microseconds and the measured overshoot went
  from a 37 us median (212 us worst) on ~9 ms coarse waits to an 88 us median (561 us worst) on ~500 us ones.

Tests: GPU excess/decay/evidence tables, an integration case proving the reservation grows when presents start
waiting on GPU work, one proving no displayed-transition evidence keeps the back edge, and a sub-tick SmartWait
accuracy case. Hardware run pending: expect `gpuHeadroomUs` to climb for a few seconds then settle, `p2dUs` to fall
back towards `p2dFloorUs`, the published 1%/0.1% low back near the back-edge figures, and the latency estimate to
settle between the two previous runs.
