# llm-wiki Log

### 2026-10-26 - Implement gpu-api-plan near-term items 1–3: D3D11On12 cleanup, shader scaler/FP16/copy routes, and native-YUV face camera (pending HW)

- **Item 3 (`0717c656`):** removed never-assigned `d3d11on12*` fields in `dx12_hook_types.h` and `<d3d11on12.h>` in
  `dx12_hook_internal.h`, renamed `CleanupD3D11On12` -> `CleanupNativeDx12Overlay` and
  `dx12_hook_g_D3D11On12Adapter` -> `dx12_hook_g_NativeDx12OverlayAdapter`, and extracted API-neutral DXGI format
  helpers from `sharpen_d3d11.h` into `hook/sharpen/sharpen_dxgi_format.{h,cpp}` for the 5 DX12 FFX/screenshot files.
- **Item 1 (`513350a0`):** replaced the D3D11 `VideoProcessor` scaling, linear FP16 SDR (`scRGB` swapchains) and
  non-sampleable/MSAA routes with CE's own shaders:
  - `mediaengine/video/scale_kernel.{h,cpp}` + `frame_scaler.{h,cpp}` + `frame_scaler_shader.h`: two-pass separable
    Lanczos2 (`quality=normal`) / Lanczos3 (`quality=best`) scaler with `sharpness=0..100` side-lobe gain, main-lobe
    min/max anti-ringing clamp, downscale filter-window stretch over the ratio, cheaper axis first, and `RGB10A2` vs
    `RGBA16` intermediate precision. Ready for `4:2:0`, `4:2:2`, `4:4:4` (`ChromaLayout` in `rgb_yuv_draw.{h,cpp}` and
    `video_color_conversion_shader.h`) and direct `RGB`.
  - `sampleable_source_copy.{h,cpp}`, `shader_conversion_route.h` (`DecideShaderConversionRoute`), and
    `VideoEncoder::ConvertRgbWithShaders` (`video_encoder_convert_yuv.cpp`) handle scaling, FP16 SDR -> sRGB
    normalization (`CS_NormalizeRgb10`), and non-SRV/MSAA inputs before `EnsureVideoProcessorObjects`. The SDR
    `VideoProcessor` path remains only as a shader-failure fallback until the 3-vendor hardware round completes.
  - Covered by `ScaleKernelTest.*` (10), `ShaderConversionRouteTest.*` (5), and `FrameScalerGpuTest.*` (9).
- **Item 2:** `FaceCameraCapture` now sets `MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING = FALSE` and negotiates
  `CandidateOutputSubtypes(mode.subtype)` (`face_camera_format.h`), keeping uncompressed `NV12`/`YUY2`/`RGB32` modes
  without MFTs and decoding `MJPG`/`H264` to `NV12` or `YUY2`. `FaceCameraRenderer` samples `NV12` (`R8_UNORM` +
  `R8G8_UNORM` views) and `YUY2` (packed macropixels with full-resolution horizontal luma interpolation) and converts in
  `PS_Main` using `MF_MT_YUV_MATRIX` and `MF_MT_VIDEO_NOMINAL_RANGE` (`[FaceCamera] Native YUV path: ...`). Covered by
  `FaceCameraSourceTest.*` and `FaceCameraRendererHistoryTest.RendersNv12AndYuy2WithConfiguredMatrixAndRange`.

### 2026-10-26 - Flaky media-helper LOST test: the recording reconciled on a later readiness read than the HELPER step

- `EngineRuntimeTest.AMediaChildThatCrashesWhileRecordingIsLostAndTheNextRecordingBringsItBack` timed out (20 s
  waiting for media LOST) once in gate `20261026_181900_build_7348`, under 4 parallel flow scenarios; ~6 ms alone.
- Cause (product): `ServiceOnce` stepped HELPER events on its tick-start `HelpersReady()`, but the recovery callback's
  `RecordingOwner::Reconcile(true)` re-read `Ready(Media)`. A child that died between the two reads failed the
  pending recording, `StopChildEndpoints` retired it, and the next step saw it not ready and not wanted
  (`Present` false): silent end of episode, no LOST.
- Repro: `ChildrenScript::downAtNextService` takes the child down at `FakeChildren::Service` entry, between the two
  reads. New `EngineRuntimeTest.AMediaChildThatDiesBetweenTheHelperSampleAndTheRecoveryIsStillAnnouncedLost` saw
  `{READY}` only before the fix, `{READY, LOST}` after.
- Fix: the recovery callback steps HELPER events on a fresh sample and passes it to
  `Reconcile(ChildReadiness)`, which no longer reads readiness itself. The supervisor-recovery window that remains is
  recorded in `library/04-runtime-modules.md`.

### 2026-10-26 - GTA vkd3d crash #2 is Epic's overlay feeding DXVK's adapter to native D3D11; adapter guard (pending HW)

- Session `20261026_175321` (0.1.7347): the resolver fix held (interop adapter `VendorId=10de`, capture initialized,
  overlay drawn), then `vkd3d_queue` died ~0.9 s later. The dump's context was again only CE's termination hook,
  but `crash_first_chance::g_Slots[0]` was in the dump: AV reading 0 at
  `d3d11!CCreateDeviceCache::CAdapterCache::GetWddmVersion+0x2f`, `rdx=E_NOINTERFACE`. The caller was the Epic Online
  Services overlay (`EOSOVH-Win64-Shipping.dll`): its `CreateDXGIFactory1` was forwarded to DXVK at 17:53:50, and it
  intercepts `GetProcAddress` for `D3D11CreateDevice`.
- Fix: `module_replacement_adapters.cpp` guards System32 D3D10/D3D11 device-creation entries while dxgi.dll is
  replaced and they are not, exchanging a replacement adapter for System32's by LUID. Flow regression with a fake
  translation adapter. `vulkan_native_interop.h` moved to `hook/runtime/com_object_module.h`; its tests are now
  `ComObjectModuleTest.*`.
- Diagnostics: `ShouldDumpWithRecordedFault` puts the recorded fault into thread-start-handler terminations; forwarded
  entry calls log each new caller module; the recovery recipe is in `debug-tools.md`.
- FIFO: the CE-caller bypass had removed the only factory capture in this run (armed, nothing captured).
  `CaptureFromOwnSystemFactory` now captures on arm. The arm-timing race against a swapchain created inside the
  first `vkCreateSwapchainKHR` is pre-existing and still open.

### 2026-10-26 - GTA vkd3d crash: dump residue confirms native D3D11 on DXVK's adapter; fail-closed interop guard (pending HW)

- Re-examined `20261026_165613`. Thread `45e4` (`vkd3d_queue`) is the one that logged the interop and then
  terminated. Its unwound stack below CE's dump handler still holds
  `d3d11!CCreateDeviceCache::CAdapterCache::GetWddmVersion+0x2f` beside DXVK's `dxgi_7ffb3a2a0000+0x4556`.
  System32's D3D11 faulted reading the DXVK adapter's private state, so the resolver correction (`2db4be50`)
  addresses the actual fault, not only a suspicious mix.
- Added `hook/vulkan_layer/vulkan_native_interop.h`: `CreateD3D11InteropDevice` refuses any adapter whose vtable is
  not in System32's `dxgi.dll` (path-checked, because the fallback bare-name load can return a replacement) and logs
  the implementing module. `VulkanNativeInteropTest.*` (3) passes, including a real System32 adapter.
- Side effect of `2db4be50` recorded as an open question in `third-party-dll-loading.md`: the layer's factory call
  no longer triggers the FIFO path's system factory body capture. `vulkan-forced-fifo.md` also no longer claims
  the export body hooks that are absent from the code.

### 2026-10-26 - GTA reaches vkd3d in 0.1.7345; Vulkan capture interop receives DXVK's factory (resolver fixed, rerun pending)

- Session `20261026_165613`: replacement D3D12 device/root-signature calls, successful 3840x2160 Vulkan swapchain
  and successful CE overlay pipeline creation. The new routing is effective; the instance-only ownership issue
  is gone (ownership begins at actual swapchain creation).
- At 16:56:53.399 the Vulkan layer explicitly requested System32's DXGI factory; CE's dynamic resolver returned
  its application wrapper, which selected `...\vkd3d\dxgi.dll`. Its adapter then went to native D3D11 interop;
  the log reported NVIDIA's description with DXVK's AMD vendor/device identity. No present completed.
- `DetourGetProcAddress` excluded `capture_hook` and wrapper callers but omitted CE's Vulkan layer. Reuse the
  existing `ClassifyRequesterPath` exemption for every CE module. The flow scenario loads its importer fake as
  `VK_LAYER_CE_overlay.dll`: failed on both wrong export and replacement count before the fix, passes afterwards.
- Matching archived CE PDBs and Microsoft symbols loaded in x64 CDB. The pre-termination dump shows
  `vkd3d_queue` exiting through `RtlUserThreadStart` -> `NtTerminateProcess(0xC0000005)`; original fault already
  unwound, so do not attribute the instruction to CE or the driver from this termination-only stack. The unsafe
  runtime mix is proven; whether it was the sole cause still needs a corrected game run.
- Filtered closing product gate 0.1.7346 passed IAT/module-pin/replacement/Vulkan/DXGI-factory regressions and the
  extended flow scenario; fresh installer 45,639,050 bytes. The full native/Python/184-flow gate passed on 7345;
  the small resolver follow-up used the affected-test filter, not a second unfiltered run.

### 2026-10-26 - finish runtime replacement for cached exports; stop Vulkan adapter probes claiming presentation (pending HW)

- Handoff reported that the first replacement build still ran GTA V Enhanced on native D3D12, while DXVK's
  enumeration-only Vulkan instance made CE stand down. The original follow-up logs were not supplied here.
- Reproduced the cached-entry gap in `FlowModuleReplacement`: save System32's `CreateDXGIFactory2` directly
  from its export table before adoption, then call the same address afterwards. With entry forwarding omitted,
  the replacement count stayed 1 instead of 2. A normal `GetProcAddress` probe had accidentally saved CE's wrapper
  and passed, so the test asserts that its cached address belongs to the native image.
- Native-only entry detours then exposed a tail-call loop: the proxy tail-called System32 with the game's return
  address. Both native and replacement exports now hold a native-runtime scope around their callable predecessors,
  using paired published inline hooks. The scenario passes cached entries, static/late imports, proxy forwarding,
  explicit CE native calls and adoption surviving host shutdown. Caller scope uses the existing unload-aware module
  address cache; native Agility components and forwarded native aliases stay native.
- Vulkan presentation ownership now follows actual swapchain creation/lifetime, not IPC connection or instance
  enumeration. Temporary creation ownership protects the ICD's DXGI WSI before the first swapchain exists; failure
  releases it, replacement has no claim gap, overlapping swapchains retain it, final destruction releases it, and
  reconnect republishes existing presentation. Seven policy regressions cover these transitions without sleeps.
- Current behavior, limits and diagnostics: `third-party-dll-loading.md`. GTA translation, overlay and capture
  remain field-validation work; no game session is claimed by these focused regressions.
- Closing gate `python build.py --incremental --run-tests --skip-updates --concise` passed as 0.1.7345:
  full native suite, Python self-tests and all 184 WARP flow scenarios; fresh setup package, 45,637,974 bytes.

### 2026-10-26 - custom_dll_paths DLLs named like a resident runtime were unused second images (GTA V Enhanced + vkd3d-proton, fixed, pending HW)

- Session `20261026_145243` (0.1.7342): `ThirdParty preload: dxgi.dll/d3d12.dll/d3d12core.dll loaded from ...\vkd3d\`
  succeeded, yet the game ran on native D3D12. System32's `dxgi.dll`/`d3d12.dll` were already mapped at CE's DllMain
  (`ModulePin: pinned name=dxgi.dll ... SYSTEM32`), and Windows resolves every by-name request to the first image of a
  name, so the vkd3d copies (and vkd3d's own `d3d12core.dll` -> `dxgi.dll` import) were never reached.
- Fix: a custom DLL whose name still resolves to another image after the preload replaces it for application code
  (`hook/runtime/module_replacement*`): CE's forwarding originals move, application imports are rebound (late modules
  from the loader notification, before their DllMain), and bare-name / superseded-path / superseded-handle lookups
  from application callers are answered with the replacement; Windows modules and CE keep System32's image, a
  configured proxy keeps its full-path load. `Wrapped_CreateDXGIFactory*` used a cached System32 "unhooked export"
  and would have kept creating native factories; it now follows the application's `dxgi.dll` (the flow scenario
  failed on exactly that before the change). Rules, limits and log markers: `third-party-dll-loading.md`.
- Verified: `ModuleReplacementPolicyTest.*`, `ModuleReplacementLoaderTest.LoaderNotificationSeesBoundImports` (Windows
  11 26300 binds imports before the load notification), `FlowModuleReplacement.*`. Not verified: a GTA run on
  vkd3d-proton, and how CE's overlay/capture behave there (the Vulkan layer should own presentation).
