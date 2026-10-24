# llm-wiki Log

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

### 2026-10-24 - Steam fix VALIDATED (20261024_131126); startup stack overflow is OptiScaler's (20261024_131043)

- 131126 (0.1.7313): `view=real-below-foreign-chain`, SetColorSpace1/GetBuffer body hooks on attempt 4, Steam log
  `Tracking new swap chain: 39bbd750` (the real object) + `Creating D3D12 renderer`; user confirms the overlay.
- 131043: `0xC00000FD`, OptiScaler's handler `WINMM+0x86C10` calls its saved original, a trampoline that starts
  with `E9` into its own earlier relay for the same ReShade `dxgi.dll` function: a double detour by one tool, on
  OptiScaler's own thread. No CE frame, CE never patched that function; the user saw it once without CE too.
  Follow-up: `StackOverflow attribution:` line in crash.log (`debug-tools.md`, crash dump triage).

### 2026-10-24 - Steam overlay missing with ReShade + OptiScaler: CE handed Steam its wrapper (20261024_124142)

- Talos + Steam + ReShade (`dxgi.dll`) + OptiScaler (`WINMM.dll`, NGX DLSS-G -> FSR FG): CE overlay and FG fine, Steam
  overlay never drew. Steam's `gameoverlay_renderer.txt` was decisive: `IWrapDXGIFactory1::CreateSwapChain called`,
  then `Unknown opcodes ... F3 0F 1E FA ... module=capture_hook_x64.dll` for each swapchain method and `Tracking new swap
  chain: 68ab200` = CE's `CWrapDXGISwapChain` (hook_debug: `Wrapped Streamline runtime swapchain ... wrapper=...68AB200`).
- Cause: the Streamline create branch wrapped with 2+ overlays and returned before the DX12 identity rule, although
  Present/Present1 deep hooks were installed. Now `ChooseStreamlineRuntimeSwapchainView` (identity first). Same session:
  SetColorSpace1 body hook refused once (`unstable-thread-snapshot`) and fell back to the entry; now retried, as GetBuffer
  diagnostics. Page: `dx12-overlay-third-party-coexistence.md`. Hardware retest pending.

### 2026-10-24 - Review of 2026-10-22..24: four defects confirmed and fixed, one more found

- An external review's four findings all reproduced (its flow probes on 0f74e476) and are fixed with regressions:
  deep-hook prolog copy relocates RIP-relative operands (the FFX destroy cookie load); Streamline presenting evidence
  restarts at another runtime's swapchain create; the front verdict is reconciled on every Present (a wrapped DLSS-G
  enabled before the first present was never applied); XeFG/XeLL forget an unloaded image's hook records before
  hooking a reload at the same address (`InlineHook::ForgetUnloadedHook`).
- Own review: a second concurrently mapped XeFG/XeLL copy re-pointed the first image's detours at its own code (single
  original slots); it now stays unobserved. Contracts in `frame-generation/xefg.md` (rules 5, 10; FFX inputs section).
  Read but no defect found: 95140454, 57df0db6, 0f74e476, 13240aa0, e1b213e1. Hardware retest pending.

### 2026-10-24 - No-UI-resource FSR FG: overlay drawn before interpolation; final-batch route now owns it (20261024_010934)

- Talos + OptiScaler (`EnableFfxInputs=true`): empty UI resource every frame kept the overlay in the proxy backbuffer
  AMD interpolates (graph stepping at ~45 Hz on a 90 fps output, warped on generated frames) although the
  final-batch probe ran on every output. `ChooseFFXProxyPreworkRoute` hands no-UI frames to the proven route.
- Display pacing itself was healthy in same-build session `20261024_010343`; menu-window output gaps under composed
  presentation stay unattributed. Evidence and GTA implications: `frame-generation/xefg.md` ("the overlay was
  interpolated with the scene"). Hardware retest pending.
- Retest `20261024_015207`: 7 outputs without overlay after FG enable (route proven on AMD's game-thread presents;
  anchored on Present-chain start-up messages that stopped). Route now anchors on the runtime image's last batch and
  waits for AMD's presenter thread; Present-chain overlays draw above CE (user's choice: never flicker).

### 2026-10-24 - OptiScaler FFX inputs bypass export observation; front input calls retire the live FSR overlay

- Sessions `20261023_235619` (protected startup never observes enabled FFX configure) and `20261023_235838`
  (`EnableFfxInputs=false`, native calls observed but repeated DLSS-G inputs retire/recreate the FSR overlay).
- Root cause and contracts: `frame-generation/xefg.md`, section "OptiScaler EnableFfxInputs=true". Native body
  observers preserve the foreign input detours and saved originals; front signals no longer request FSR teardown
  or DLSS UI activation. Hardware retest pending; flow and prologue regressions accompany the change.

### 2026-10-23 - OptiScaler FSRFG: no overlay, then device removed; DLSS-G wrapper lost its overlay (session 20261023_221541)

- Run 1 (OptiScaler over real DLSS-G 3x): today's provider-only front rule kept DLSS FG off although `sl.dlss_g`
  originated every present; fixed by requiring that Streamline presents nothing. Run 2 (`FGOutput=FSRFG`): CE hooked
  the engine's FidelityFX image, OptiScaler ran its own same-named copy unseen (no overlay, protected startup 4 s);
  the game's DLSS-G enable that OptiScaler answered retired the FFX startup and CE drew FFX's chain from the game
  queue: `0x887A002B`, Unreal exit 3, no dump.
- Fixes and evidence: `frame-generation/xefg.md` ("OptiScaler FGOutput=FSRFG and two FidelityFX images"). Flow:
  `FlowFSRToolRuntime`, `FlowDLSSGApiTool`; unit: `RuntimePresentedSwapchainPolicyTest`, `FGPresenterPolicyTest`.
  Hardware rerun of both OptiScaler configurations pending.

### 2026-10-23 - XeFG support (native and OptiScaler), phases 0-5 in code; hardware pending

- Plan from session `20261023_183119` (Talos + OptiScaler `FGOutput=XeFG`: overlay on 72 of 144 frames, `DLSS_FG 2x`
  published for XeFG). Commits `4d389d04`..HEAD: FG runtimes by base name, present originator log, coverage
  inheritance only on the composing runtime's presents, `ce::fg_presenter`, `RuntimeMode::kXeFG`/status type 4,
  the DLSS-G front, XeFG/XeLL export detours, the runtime-output route on XeFG's queue, the `SetPresentId` limiter
  site, XeLL anchors, capture coverage. Topic page: `frame-generation/xefg.md`.
- Two defects the new flow scenarios caught: a runtime-presented chain seeded `dx12_hook_g_OriginalGameQueue`
  (after destroy every native present `zero-ecl-skip`, 899 uncovered), and the scene-transition gap measured across
  the FSR FG -> XeFG swapchain handoff (`gap=4170ms`, 30 blank outputs). Both fixed at the root.
- Third: XeFG's first output after a DLSS-G signal turned out to be a front lost its overlay to the Streamline OFF
  teardown (and XeFG's chain may reuse a released chain's address): registration lifetimes are swapchain identities
  now (`frame-generation/xefg.md` invariant 9). `dx12_hook_process_session_stage2*` split (811 lines).
- Not done: Intel-GPU UI-texture path (contingent, no evidence), exclusive fullscreen, Steam on XeFG's presenter
  thread, an OptiScaler `dxgi.dll` fake serving the DLSS-G API (the provider rule is covered by policy tests).

### 2026-10-23 - Talos + OptiScaler `dxgi.dll`: CE/OptiScaler `slGetPluginFunction` hook loop (session 20261023_173151)

- `0xC00000FD` on `GameThread` in Streamline plugin loading, build 0.1.7155, OptiScaler `10.0.0-dev (1db5042)`.
  Dump: OptiScaler's slot-0 `slGetPluginFunction` detour recursing through CE's tail-jumping
  `Hooked_slGetPluginFunction`. OptiScaler hooked `sl.common` first, CE prepended over its `E9`, OptiScaler's
  8-byte liveness fingerprint failed, it re-detoured over CE: its original -> CE, CE's original -> its relay.
- Fix: `ForeignEntryPolicy::kLeaveToOwner` (inline-hook engine, ownership from the image on disk) for the six
  Streamline core exports with GetProcAddress/import routes, except while the takeover owns the import slots.
  Full mechanism, rejected alternatives and consequences: `third-party-dll-loading.md` ("Known failure modes").
- Tests: `test_inline_hook_foreign_entry_policy.cpp` (dump bytes), `test_streamline_foreign_entry_policy.cpp`.
- Retest `20261023_183119` (0.1.7298): no crash, all five exports left to OptiScaler (`leftToForeignOwner=4` + 1), the
  game's GetProcAddress lookups intercepted. The overlay was dim and slow because that OptiScaler was configured
  `FGInput = DLSSG`, `FGOutput = XeFG` (OptiScaler.ini): XeFG (`libxess_fg.dll`, `xefgSwapChainD3D12CreateContext`)
  presents every other frame from its own thread (T4D14, limiter: "the application's own Present"), CE takes the
  game's `slDLSSGSetOptions(on)` for DLSS-G, runs PostSL on the Streamline-originated presents only (`PostSL stats:
  calls=500` per 6.8 s = 72/s at 144 output) and ProcessFrame skips the XeFG presents (`Routing state ... skip=1`,
  `PostSlRecent`). CE has no XeFG support (native or via OptiScaler) yet; not a regression of the hook fix.
- Same session: `EngineRuntimeTest.RecordingStatesFollowTheSessionAndLiveObservation` failed once in a gate. Cause:
  `Engine::PublishRecordingState` (and the STOPPING path) pushed the event before `PublishStatus()`; now state first,
  then the event (the `engine_lifecycle.cpp` rule). Extended test: 58/300 failures without the fix, 0/300 with it.

### 2026-10-22 - DXUP on Mirror's Edge: label over Windows' D3D11, buffer-1 flicker (session 20261022_162106)

- Two runs, CE 0.1.7293, x86, DXUP 1.1.0 (`ProductName "DXUP"`; source `%USERPROFILE%\Programme\build\DXUP`).
  p2520 = DXUP `d3d9.dll` only: CE draws on Windows' D3D11 chain, label `'unset' -> 'DX11'` because the D3D11
  overlay asked `IsDxupModuleLoaded` for `d3d11.dll`/`dxgi.dll` only. p26256 = all DXUP DLLs: label `DX9 (DXUP)`
  correct, but `DX11: GetBuffer(1) FAILED hr=0x887A0001` x3388 and `Creating retained RTV` x3390 of 6788 frames.
- Root cause (DXUP `src/d3d11/dxgi_interfaces.h`): `GetCurrentBackBufferIndex` forwards the real D3D12 chain's
  index (0/1), `GetDesc` reports its FLIP_DISCARD, and `GetBuffer` refuses `Buffer > 0` for discard apps "as
  Windows' D3D11" (buffer 0 = proxy target or, in direct mode, retargeted to the next real buffer). CE's
  `ResolveDX11BackBufferIndex` used the reported index, so every second Present returned before the overlay draw;
  inject capture, screenshots and sharpen read the same index. Windows and DXVK report 0 always
  (`FlowD3D11FlipBufferIndex.WindowsD3D11KeepsTheCurrentBackBufferAtIndexZero`), so buffer 0 is the D3D10/11
  contract: `kDX11ApplicationBackBuffer` (dx11_back_buffer_view_cache.h) replaces the resolver and the per-frame
  forced index. The 2026-10-18 per-index view cache had hidden the DXUP 0.8.1 variant (odd index = real buffer
  DXUP's Present overwrote with its proxy copy).
- Label: the D3D11 overlay now uses `DxupOverlayApiLabel` whenever any DXUP DLL is loaded, as `Dx12OverlayApiLabel`
  already did; other replacements under the same names are excluded by the version resource.
- CPU: CE's own per-Present cost was ~0.1 ms in both runs (perf CSV `total_us` mean 127 us d3d9-only, 104 us
  full DXUP; failing frames `overlay_us` < 10 us; `[D3D11 AF COST] estMsPerS=4.44` on DXUP's static context table).
  Frame time 1.06 ms (d3d9-only) vs 1.6 ms (full DXUP) is not CE's Present work; no CE-off baseline in the session.
- Tests: `FlowDX11OverlayApiLabel.*` (stand-in `d3d9.dll`s, DXUP and ReShade version resources),
  `FlowD3D11FlipBufferIndex.OverlayDrawsOnBufferZeroOfATranslatorsRotatingChain` (forwarding chain over WARP
  FLIP_DISCARD reporting 0/1). Both fail with the old code (label `DX11`/`DX11.1`; 3 refused `GetBuffer(1)`, 4
  `GetBuffer` calls in 6 draws). Hardware retest pending.
