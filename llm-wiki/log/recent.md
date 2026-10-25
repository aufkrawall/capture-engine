# llm-wiki Log

### 2026-10-25 - Resident-hook reconnect: direct logs stayed in the old session, workers stayed parked (HotS report)

- User session `20261025_212736` (0.1.7157, elevated CE, Heroes of the Storm DX12 PID 34108): not a fresh late
  inject but `Adopted resident hook ... for host reconnection`; the user had restarted CE because the overlay was
  already missing on the first (late-inject) attempt, whose session folder was not supplied. Evidence after the
  reconnect at 21:27:36: `IPC: Connected!` plus the `DLSS indicator` line from the bootstrap-complete branch of
  `TryReactivateHookRuntime`, then 15 min with `perf_metrics_34108.csv` header-only (rows 1-8 are flushed
  immediately, so no present reached CE's frame processing) while ETW counted ~225 presents/s.
- First attempt `20261025_211938` (late inject at 21:19:39, CSV header-only too) has the cause: `RTSSHooks64.dll`
  owned dxgi's Present entry (E9), and every inline patch in the running game failed with
  `QuiesceFailure::kSuspend` (`WriteOwnedEntryPatch quiescence failed reason=3`, `DeepHook: Refusing live patch
  ... quiesce=suspend retryable=0`, batches `installed=0`): Present body/entry, CreateSwapChain*, ResizeBuffers*,
  even the FatalExitDump hooks. A live HotS thread refuses `SuspendThread`; kSuspend is non-retryable, so CE never
  got a Present view (no `DetourPresent: ENTRY`/`heartbeat` in 65 min). The maintainer's startup-injected HotS
  (`20261025_211430`) shows the same `quiesce=suspend` 18 s after start; its Present hook had landed before that
  thread existed. Open: which thread and why (the log names neither tid nor error), and a late-inject route that
  does not need to suspend it. The same folder holds the 8620 post-reconnect direct lines, including
  `[InjectLifecycle] Reactivated resident hook for host PID 26512` - direct proof of defect 1 below.
- Defect 1: `LogToFileAtomic` cached its direct-file directory (`s_logDir`) for the process lifetime, so every
  `HookLogImportant` line (most present/overlay diagnostics) went to the first session after a reconnect: 2708 of
  2725 hook lines (`#1378..#4102`) were absent; logger.log showed no ring overflow. The Vulkan layer's
  `vulkan_layer.log` handle had the same flaw. Fix: `common/logging/direct_log_file_sink.{h,cpp}` owns directory,
  handle cache and lost-line accounting; `RebindHookDirectLogFiles()` right after `Reconnect()`,
  `LayerRebindLogFile()` on every layer connection.
- Defect 2: `SystemMetricsCollector` and `FreezeWatchdog` loops exit on `HookIsShuttingDown()` and only device
  init restarted them (`adapterLuid=0x0 graphicsLuidPublisherPid=0` all session; the watchdog's `running_` stayed
  true, so even `Start()` refused). Latent crash on the same path: `Initialize` assigned a new `std::thread` over the
  exited, unjoined one (`std::terminate`) when a device was re-created after a host restart. Fix:
  `hook/runtime/resident_worker.h` (`Lifecycle`: park on dormancy under one lock, generation-safe resume) used by
  both; `ResumeParkedHookWorkers()` after `ResumeHookRuntime()`, layer resumes the collector in `TryReactivateLayer`.
- Tests: `DirectLogFileSinkTest` (rebind moves records, frees the old folder, lost-line notice, re-entrancy busy),
  `ResidentWorkerLifecycleTest` (6), wiring added to `ResidentHookReactivationRebindsSessionDiagnostics`. No
  hardware run; no threaded collector/watchdog test (waiting for a 10/500 ms loop to park would be timing-based).

### 2026-10-25 - Flow-test pass cache: four holes closed (review of 0b398a2f)

- Reproduced on HEAD with the real key/cache code: a changed `.res.o` (from `sl_version.rc`) or `.def` kept
  `flow_test_input_key()` (no depfile names them); a forced rerun that failed left `passed_inputs.json`, so the next
  ordinary gate skipped as passed. Own finds: a listing that exited non-zero or listed nothing passed vacuously, and
  `--verify-runtime` / a resumed `--verify` skipped (sys.argv lacked `--verify`). Fixes: `flow_test_link_inputs()`
  (link-cache manifests minus depfile-covered objects; windres output checked byte-stable),
  `withdraw_flow_tests_passed`, `list_flow_tests` returns None on failure, `rerun_flow_tests` from the CLI's `verify_flag`. Details: `build.py.md`.

### 2026-10-25 - PC latency blanked at every FG factor change (DLSS-G dynamic MFG)

- User saw `PC Latency --` for about a second during plain gameplay (not reproducible, no log). Cause found in code:
  `Tracker::SetFrameGeneration` emptied every ring and both sample windows on any factor change; dynamic MFG publishes
  2/3/4 many times a second. Now only FG on/off or a type change starts an epoch; a factor change keeps measuring and
  drops just the queue count (`factorChanges=` in the chain line). New `test_system_latency_generator_factor.cpp`:
  70 of 120 frames blank with the old reset, 0 now. Remaining blank causes and log markers: `overlay-rendering.md`.

### 2026-10-25 - Border-access freeze: HDR, not UAC; tests now model the deadlock

- Code precondition missing from the 2026-10-24 write-up: only an HDR desktop takes desktop screenshots through WGC on
  the engine STA (SDR uses GDI); the changelog no longer names UAC. With the old blocking wait restored in a test build,
  Windows answered `allowed` in ~41 ms without calling back into the asker on the maintainer's machine (UAC on, not
  elevated), so HDR alone is not sufficient and the affected system's difference stays unexplained.
- `tests/test_wgc_border_access.cpp`: a C++/WinRT coroutine model of GraphicsCapture's apartment-resuming completion
  (fidelity-checked); with the old pattern it deadlocked at its 60 s bound, with the fix it passes. New gate check
  `tools/check_winrt_blocking_waits.py` (`winrt_blocking_wait_tree`). Details: `wgc-capture.md`.

### 2026-10-25 - Implemented source-phase fitting for WGC/DXGI timestamp smoothing

- Replaced integration of the interval EMA with a bounded 16-sample fit of raw QPC against source ordinal.
  It removes persistent phase error after source-rate changes while retaining the rate/jitter estimator,
  calibration, monotonicity, deviation cap, real-stall snap/reset and ingress source-span accounting.
- Eight new GoogleTest regressions cover both rate-change directions, gradual changes, large QPC values,
  quantized 135/140/144 fps input, stalls, reset, duplicate stamps and 300000 mixed-span source frames.
  Five fail with the old implementation; all eight plus the frame-timing and capture-policy suites pass
  with the fix. The thinning negative control now tests rate semantics rather than requiring a bad lag.
- Once-per-recording model log: `model=source_phase_fit sourceSpans=1 rawUnchanged=1`; existing deviation,
  snap and raw/predicted residual metrics remain. No analyzer threshold or sync guard was relaxed.
  Root cause, semantics and hardware acceptance checks: `wgc-capture.md`. Hardware confirmation below.
- Closing gate passed: 0.1.7329, full native suite, 47 Python tool groups, PE/PDB and privacy checks,
  installer packaging (44748158 bytes). FG flow inputs match the 182-scenario pass from earlier today,
  so the gate reused it. The installer version/PE format and compiled model-log string were inspected.
- Hardware confirmed in `20261025_140044` (0.1.7329, Talos 4K dxgi_dup 120 fps, 61.758 s): reported mean
  raw-minus-predicted 0 us (per-window means -386..+345 us), raw absolute residual 3411 us; timestamp-domain
  and visual/audio fault flags clear. Zero startup ingress drops, excess/policy repeats, pool/encoder/mux faults.
  One model line and one converter-release line; 7411 packed unpacks, zero failures. All 20 sources and two
  AAC tracks end at 2964400 samples; corrected post-mux video/audio endpoints match exactly (`maxAudioDelta=0`).
  Source-limited repeats remain expected; longest contiguous repeat is one tick. No product change needed.

### 2026-10-25 - Remaining timestamp warning: reproducible interval-integrator phase bias

- `InputFrameRatePredictor::SmoothMonotonicTimestamp` integrates its interval EMA without phase correction.
  Native probes using the pinned test compiler: steady 11200 <-> 13000 us intervals leave +/-4199 us of
  permanent offset; 7143 <-> 8333 us leave +/-2777 us. This explains a real failure mechanism consistent
  with `20261025_130636` smoother deviation; the raw and predicted times use the same QPC clock.
- An isolated 16-sample phase-fit proposal removes the offset and preserves the tested quantization,
  monotonicity, deviation, real-stall, reset and variable-span thinning contracts. A naive phase/8 correction
  breaks the 140 fps quantization case and was rejected. Details and required follow-up: `wgc-capture.md`.
- No product/analyzer code changed. The proposal is not yet fully regression-tested, gated or hardware-tested.
  Local report and patch are in ignored `build/verification/timestamp_phase_investigation/`; preserve the strict
  raw timing diagnostic and require fresh hardware evidence before claiming the session warning resolved.

### 2026-10-25 - Startup freeze and duplicate converter summary confirmed fixed (20261025_130636)

- 0.1.7328, Talos 4K dxgi_dup 120 fps AV1 10-bit, 35.267 s, GPU ~100%; user sees a smooth opening.
  Startup gathers 325897 us of history, `lowWater=28` versus output 40, no fallback/ingress drops/grid-debt
  holds/playout catch-up. First cadence window: 34 repeats/121 ticks; longest contiguous repeat in the run:
  one tick (8 ms). No excess/policy repeats, overload or mux pressure. Converter release logged once;
  4232 packed unpacks, zero failures. Twenty sources/two AAC tracks: 1692800 samples, all `diff=+0`.
- Post-mux corrected video/audio endpoints differ by 1 us (rounding). No logged live audio loss, drift
  compensation or ingest starvation. Game frames during the first live second: median 11.762 ms,
  maximum 13.301 ms; live maximum 20.448 ms. The startup/prewarm window has no large game-frame hitch.
- One open analyzer flag: raw residual average absolute 5908 us versus predicted 3431 us triggers
  `timestamp_domain_mismatch`; raw-minus-predicted +2557 us average, 5559 us maximum. Cause unverified;
  separate from the fixed startup failure. Details: `wgc-capture.md` startup section. Packed/unpacked
  same-scene performance A/B and native WinRT WGC validation remain pending. No product code changed.

### 2026-10-25 - Packed P010 + precompiled shaders validated; sub-CFR start froze ~0.3 s (20261025_120518)

- Validation (0.1.7327): desktop `20261025_120018` and Talos 4K at 100% GPU, NVENC p5 AV1 10-bit, 205 s
  `20261025_120518`: `P010_PACKED16`, `runtimeCompiled=0` everywhere, `unpackFailures=0`, `excessRepeats=0`,
  `encoderOverload=0x0`, audio `diff=+0`. First ingress conversion 2.4 ms CPU (51.5 ms with runtime compiles). The
  only game-frame bump at start: one 15.6 ms frame (local median 11.4) inside NVENC `avcodec_open2` (83 ms).
- Found there (pre-existing, not packing): the video began with ~0.3 s of one held frame and ~0.45 s of gameplay
  missing. Cause and fix: `wgc-capture.md` "Startup reserve wait below the output rate". Also: the retained
  converter's counts were never reset by `Release()`, so `[RetainedYuv] Ingress converter released` repeated with
  the same numbers (3x in `20261025_120018`); now reset.
- Continuation review: regression checks with the former policy/counter behavior fail nine tests. Extending the
  simulation to exactly 120 fps found another permanent low-water miss (39 newer frames versus 40 required).
  Count-qualified selection now permits one output interval of extra history; the delivery-hitch test prevents
  reaching farther back through a hole to meet the count. Startup trim logs use a session-owned `ChangeGate`.
- Verified in continuation: all 218 focused capture-policy/packed-P010 tests passed. The unfiltered incremental
  closing gate produced 0.1.7328: native suite, all 47 Python tool groups, 182 fresh FG scenarios, PE/PDB checks,
  privacy scan and installer packaging passed. The setup artifact is 44,745,562 bytes. No new hardware recording
  was made; the startup-freeze fix still needs the Talos 4K rerun described in the topic page.
- Not attributed (needs an A/B): game frame time 10.85 -> 11.4 ms when duplication started and -> ~11.9 ms once
  encoding went live (~9% fps at 100% GPU); CE's own measured passes are ~0.2 ms/frame.

### 2026-10-25 - First recording start compiled ~1 s of HLSL; now built by build.py

- User requirement: the first recording after app start must not stutter the game or the video in any mode. Known
  CPU cost on that path: d3dcompiler_47 for every media engine shader, ~1 s per process (overlay patch 0.65 s,
  retained converter 63 ms, encoder conversion 52 ms, camera 23 ms, cursor 7 ms, unpack 6 ms; measured). The
  driver shader cache does not cover it: it caches DXBC -> ISA, not HLSL -> DXBC.
- Fix: build-time bytecode (`build.py.md` "Precompiled media engine shaders"). Not yet measured on hardware: other
  first-start costs (encoder session open, ~1 GB pool allocation, driver ISA translation) and whether any of them
  shows in game frame times; needs a first-recording log per mode.

### 2026-10-25 - Packed P010 retained slots (VRAM, no picture change)

- Goal: cut the 10-bit retained reservoir without changing anything visible or smoothness-relevant. P010 pads each
  10-bit code to 16 bits; packed `R16_UINT` slots (16 bpp) hold the same codes: dxgi_dup/WGC 4K 64 slots 1519 ->
  1012.5 MiB, slot count, delay, admission, selection and NVENC depth unchanged. Design, layout, costs and
  evidence: `wgc-reservoir-vram-design.md` item 11.
- Rejected on the way: a separate pack pass over a full P010 scratch (+~6 GB/s ingress traffic at 150 fps), and a
  runtime packed/planar branch in the overlay patch shader (`PS_PatchChroma` compile 600 -> 970 ms at
  configuration). Lossy options (8-bit retention for 10-bit output, BC4/BC5) were not pursued: visible by design.
- Bit-exactness verified on the RTX 5070 for every SDR/HDR source format and range plus 4K; hardware recording
  (A/B `wgc_retained_packed_p010`) pending.

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
