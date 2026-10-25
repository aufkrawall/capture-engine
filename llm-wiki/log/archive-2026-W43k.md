# llm-wiki Log Archive

### 2026-10-24 - Hardware cursor missing from dxgi_dup recordings (20261024_231652)

- 0.1.7319, monitor dxgi_dup 4K120, av1_nvenc 8-bit, retained route `B8G8R8A8 -> NV12 (yuv)`. User: only the
  software cursor (window drag) was visible. The log looked healthy: `Encoder cursor composition active`, cursor
  timeline `visible=1 embedded=0`, but `[RetainedYuv] ... cursor=0` and the summary
  `frames=3281 overlayPatched=0 patchFailures=0`. `[Cursor Color] SDR cursor mapping: targetFmt=2` proved the cursor
  renderer reached its draw with the FP32 layer as target.
- Root cause: `CursorRenderer` and `FaceCameraRenderer::GetTargetRenderView` took the RTV format from
  `GetRgbShaderResourceViewFormat`, which has no R32G32B32A32 entry, so both overlays returned false silently on every
  retained frame since 2d1ba763 (v0.1.7155). The patch GPU tests used synthetic layers. Fix and invariant:
  `wgc-reservoir-vram-design.md` item 5. New WARP tests fail without the fix (checked by restoring the old mapping).
- Software cursor: DXGI reports `embedded=1` during a drag, encoder composition is suppressed and the duplicated
  image carries the cursor, which is why that case worked.
- Earlier 10-bit full-range sessions (`20261024_134711`, `_165220`, 0.1.7314, P010 route) also show
  `overlayPatched=0`, but every cursor sample there was `visible=0` (cursor hidden by the game): range and bit
  depth play no part; only 4:4:4 (direct RGB), output scaling and HDR-to-SDR keep the route off.
- Retest `20261024_233518` (0.1.7321) did not reach the fixed path: `Retained-YUV plane RTV creation failed for
  pool[0] fmt=NV12: 0x80070057/0x80070057`, then the RGB fallback (`split capture device cannot share NV12`). Cause:
  `DxgiDuplicationSource::Start` returned on first-frame proof while the thread already delivered that frame, so the
  thread's `EnsureTexturePool(BGRA8)` and the session's `UpdateSmoothnessBudget(BGRA8)` both ran
  `RefreshRetainedRoute` unsynchronized (the `-> RGB ()` route line with no reason is the torn write; it appears in
  both runs). The pool was created BGRA8 while the route said NV12. Fix: `DuplicationFirstFrameGate`
  (`wgc-capture.md`). Also fixed: `Stop` stored `shutdown_` outside `shutdownMutex_` (lost wakeup of a parked thread).

### 2026-10-24 - Screenshot froze the engine thread; close hung (another user's 20261024_222834)

- 0.1.7319, UAC fully off, controller `elevated=true`, HDR 4K desktop. Ctrl+0 logged `HDR desktop detected; using
  WGC readback` and the WGC prewarm, then nothing; no file, and the processes "hung on close" (user report). The
  dump is the controller (PID 14044, uptime 94 s): the engine thread waits in `EnsureBorderlessAccessRequested` on
  `RequestAccessAsync(...).get()`; a thread-pool thread holds GraphicsCapture's completion in `resume_apartment` ->
  `MTAThreadDispatchCrossApartmentCall`, waiting for that STA. Root cause and fix: `wgc-capture.md` (border access
  never blocks a capture thread). A runtime shutdown request is now logged once.
- Service question: the user thought it was installed without the elevation service. The logs show the service in use
  (`Service process-start trace leased`, `privileged service backend`), and both paths check the user's stored
  `service` preference, so it was enabled on that machine. Setup already removes an existing service when the box is
  unchecked; an update offers it checked while a service exists. The rule now lives in `PlanService`
  (`installer/install_policy.h`) with a test. Their `%TEMP%\CaptureEngine-Setup.log` would show the choice.

### 2026-10-24 - Missing config.ini killed every hotkey (20261024_192815)

- User renamed config.ini to config.ini.old under Program Files; unelevated CE logged `error=5` and
  `Global hotkeys started (owned=0/5 record=0 screenshot=0 ...)`. With the old file back: `owned=5/5`.
- Cause: `SettingsStore::Initialize` replaced an incoherent first read with bare `AppConfig{}`; the roles' `LoadConfig`
  used parser fallbacks. Both now load the embedded template (`configuration.md` first-run invariants).
- The template itself loads with zero diagnostics. Its hotkeys are Ctrl+9/Ctrl+0/Ctrl+8/Ctrl+Minus/Ctrl+7, not F-keys.
- New tests fail without the fix (4 of them, checked by disabling both fallbacks).
- Follow-up `20261024_200056` (0.1.7315): hotkeys 5/5 without the file, but it stayed missing (error 5). The user
  asked why the elevation service (installed, running, Manual start) cannot restore it: new `RestoreConfig` operation;
  updates restage the service runtime (hash of `runtime-*/captureengine_elevation_service.exe` matched the install).
  Also found: `CreateDefaultConfig` followed a link planted at `config.ini` (elevated run, user-writable folder).
- Restore VALIDATED on 0.1.7319 (`20261024_222619`, `result=0, 31 ms`, hotkeys 5/5).

### 2026-10-24 - D3D11 inventory and GPU API plan (no code changed)

- New `gpu-api-plan.md`. The user's decisions: D3D11 games and WGC/DXGI duplication stay on D3D11, and Windows keeps
  a native D3D11 media core also after a Linux port. A Vulkan backend is added for Linux behind one policy owner; the
  best solution wins over the least code.
- Rejected: a D3D12/Vulkan core on Windows with D3D11 WGC/dup edges. The WGC ingress pass must stay in D3D11 anyway
  (the surface is not shareable), and the default same-device WGC path would become a cross-API split.
- Encoder facts from the local FFmpeg 8.0-git source: `nvenc.c` takes D3D11/CUDA frames; `qsvenc.c` takes QSV frames
  derived from D3D11/DXVA2; `mfenc.c` takes D3D11 only; `hwcontext_amf.c` can derive from D3D12VA. `d3d12va_encode_*`
  and `vulkan_encode_*` are present, and `CONFIG_D3D12VA`/`CONFIG_VULKAN` are already 1.
- Found: the D3D11On12 overlay state in `dx12_hook_types.h` is dead (released, never assigned); five DX12 units include
  `sharpen_d3d11.h` only for its DXGI format helpers. Near-term: a shader scaler in place of the VideoProcessor
  fallback, plus removing that dead code.
- Follow-up the same day:
  - Verdict on the shader scaler: worth it. The unscaled SDR shader switch fixed the NVIDIA Blt-behind-NVENC stall
    (`20261016_014920`/`015331`), and the remaining VideoProcessor paths share their device with AMF/QSV/MF.
  - Drawbacks recorded: Intel fixed-function cost, a sharpness look change, the lost fallback.
  - New near-term item 2: the face camera requests native NV12/YUY2 instead of Media Foundation's RGB32 video
    processing. Afterwards CE uses no D3D11 VideoProcessor.
  - Media Foundation stays; DirectShow would only add DirectShow-only devices.
  - Dead code is now item 3.
