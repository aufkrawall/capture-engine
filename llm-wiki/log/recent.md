# llm-wiki Log

### 2026-09-30 - FSR FG recordings had half the motion rate: capture skipped generated outputs

- `logs/20260930_032355` (0.1.6863, Talos, callback-owned FSR FG, capture sync basic 120): on screen 120 fps, but
  media `Inject Perf` showed `Input: 60 | Dup: 60` per second. The callback verdict admitted only
  `callback=application` Presents, although every runtime output (generated too) passes CE's Present on AMD's
  presenter thread with the displayed image in the backbuffer.
- Fix (0.1.6864): `dx12_overlay_policy::IsPresentedFrameForCapture` captures every callback-proven output; the
  no-verdict ECL-count gate is unchanged. Tests: `Dx12EclQueueRegistrationPolicyTest.CallbackProvenGeneratedOutputsAreCaptured`
  and a source check that the wrapper does not re-filter on `verdict.generated`.
- Hardware validation pending: expect `Input: ~120 | Dup: ~0` in the media log and
  `generatedOutputs`/`applicationOutputs` near equal in hook_debug.log. Open: the limiter still reports
  `captureSource=base` on this route (see `cfr-capture-sync.md`).

### 2026-09-30 - Review of v0.1.6772..HEAD: robustness follow-ups

- Reviewed the 68 commits after v0.1.6772 (source diffs only; the four risk-audit commits were not re-read). No
  correctness bug found; eight low-severity hazards fixed with regression tests:
  DX12 inject capture begins its transport generation BEFORE `Initialize()` closes the old handles
  (`ShouldBeginInjectTransportGeneration`); parked hidden-window queues are released when their window is destroyed
  (`Ledger::ForgetWhereWindowGone`, swept on every park and every 256th unmatched Present); the caller-module cache
  pins the module across its header read (`ResolveFromLoader(..., &pinned)`); the UTF-8 config cache confirms
  "racy" entries (written < 2.5 s ago) byte-for-byte (`IsRacyFileTimestamp`, git's racy-index rule); the failed-resize
  holder scan stops at 3 s (`kScanBudgetUs`); `UnregisterCrashPreDumpCallback` removes only its own callback.
- **False alarm worth remembering:** a first reading claimed the `Phase6Tail` overlay-free fallback capture could carry
  CE's overlay because of the independent below-foreign-chain composite. That route is dead code
  (`DecideBelowForeignChainFSRDeepDraw` is a stub returning `kUnavailable` since `cff7a507`); see the SUPERSEDED note in
  `dx12-overlay-third-party-coexistence.md`. Whether the FFX present callback's composite reaches the captured proxy
  back buffer stays the open hardware question from 2026-09-29.
- Release preflight caught `common/crash_dump_policy.h` at 801 lines (already over at HEAD, from `d5b878f0`): the WER
  adoption/registration helpers moved to `common/crash_dump_wer_policy.h` (still included by the old header).
- The first full `--verify` since 0.1.6772 (sanitizers are skipped by the per-change gate) found a real bug at HEAD:
  `SteamOverlayInitVehHandler` (process-wide, runs first for every exception) read a `thread_local`; loader worker
  threads (`LdrpProcessWork`) have no TLS block, so the read faulted inside the handler and recursed to stack
  overflow (silent 0xC0000005 under ASan, moved between tests). Fix: `ce::steam_recovery::g_armedThreads` (thread-id
  set, no TLS) is consulted first, and the handler is `no_sanitize("address")`. Lesson: any process-wide VEH must be
  TLS-free and uninstrumented until it knows the thread is one it armed.
- Deliberately unchanged: `IsUsableHostDirectory` still accepts UNC paths (`test_vulkan_layer_host_directory.cpp` pins
  it; a network-share install is legitimate and the pointer file has the staged layer's trust level); the duplicated
  rejected-timestamp block in `audio_capture_loop.cpp` / `app_audio_capture_loop.cpp` (refactoring audio-critical
  loops for a maintainability gain is not worth the risk without a hardware run).

### 2026-09-30 - Review fixes: full-queue disk timeout and FFX create-byte restoration lifetime

- Full mux queues now service `CancelExpiredOutputIo("backpressure")` outside the queue lock;
  a stalled writer no longer prevents the encoder from reaching the stop/join boundary.
- FFX create suspension/retargeting pins the old export image outside the breakpoint mutex,
  revalidates the binding, and restores while pinned. Missing images are never dereferenced.
- Three source regressions fail before the fixes; native `FFXExportLifetimeTest` proves last-owner
  unload cannot unmap a pinned export and rejection paths release their references. Combined incremental
  product build 0.1.6857, full native suite and Python tool self-tests passed; game validation pending.

### 2026-09-29 - After DLSS FG -> FSR FG no recording went live: overlay-init deferral ended ProcessFrame

- `logs/20260929_142415` (0.1.6853, Talos): the cfc4537b crash fix held (FFX create `hr=0`). The next recording
  (r0002) stayed pre-live; media `wIdx` never moved and the hook never logged a capture init after the switch.
  `Present callback verdict decides base capture ... capture=1` proved `processCapture` was true.
- Cause: `[outer] FG->off - forcing overlay reinit` cleared `overlayInit` at the FFX takeover. On the runtime-owned
  FSR chain Phase3's `ShouldSkipSeparateOverlayGpuWorkForCurrentSwapchain` deferral (`Deferring overlay init because
  runtime-owned native FSR FG swapchain`) returned `kReturn`. That skipped Phase4, where the capture decision and every
  later publish live, on every frame. Phase4's staged sync-init deferral had the same `kReturn`.
- Fix: both deferrals skip only overlay init (Phase3 -> `kSkipOverlayInit`; Phase4 falls through), and Phase4+ overlay
  work stays gated on `overlayInit && syncInit`. `Phase6Tail` publishes `captureBeforeOverlay` when the draw chain
  never reached it (closes the 2026-09-26 open item; that frame may already carry the callback-composited overlay).
  Regression: `tests/test_dx12_runtime_owned_capture_flow.cpp` (source policy; ProcessFrame has no unit harness).
- Hardware validation pending: Talos DLSS FG -> FSR FG, then record. Expect `Shared capture initialized ... sc=<FFX
  chain>` after the switch and a live recording. Also still unproven: a fresh-start FSR FG recording.

### 2026-09-29 - Talos DLSS FG -> FSR FG exit: the D3D12 capture pinned Streamline's swapchain

- `logs/taloscrashdlssfgtofsrfg` (0.1.6852, Steam overlay, sl.* redirected to `npi\sl` 2.14.1 = the game's own
  version). Start with DLSS FG, record 04:31:32-38, switch to FSR FG at 04:31:51: AMD's `ffxCreateContext` ->
  `CreateSwapChainForHwnd` got `E_ACCESSDENIED` on the first try, after the minimal unpin and after full overlay
  cleanup (`retained=0`), then the exhaustion dump and UE's `TerminateProcess(3)` after a null-swapchain AV.
- Comparison: `talosoverlaydisappearedinmenu` (0.1.6813) did the identical DLSS-G-on -> FFX create (also with no
  `slDLSSGSetOptions(off)` first) successfully; the only difference in message types was the recording
  (`Shared capture initialized for swapchain generation sc=<the SL chain>`).
- Root cause: `SharedCaptureD3D12` held `ComPtr<IDXGISwapChain3>` + `ComPtr<IUnknown>` on the chain until a resize
  or re-init, i.e. long after the recording stopped. The minidump has no heap image of the chain, so the refcount
  itself is unobserved; the owned references are certain from code.
- Fix: non-owning identity binding, swapchain passed per frame (see `cfr-capture-sync.md`). The never-instantiated
  `SharedCaptureD3D11` had the same member and was removed afterwards.
- Hardware validation pending: Talos DLSS FG, record, stop, switch to FSR FG. Expect `SharedCapture bound swapchain
  identity ... without a reference` and no `E_ACCESSDENIED` on the FFX create.

### 2026-09-29 - Capture sync never engaged after an in-game FSR FG -> DLSS FG switch (Talos, Steam overlay)

- `logs/20260929_040222` (0.1.6851, Talos profile `capture_sync_enabled=true`, `capture_sync_limiter_mode=reflex`,
  capture 120 fps, Steam overlay loaded). Recordings r0003-r0005 ran at ~135-140 fps. `hook_debug.log` has exactly one
  `FPS Limiter: Inactive (... captureSync=1, captureRequested=0 ...)` at game start and no `Active` line afterwards,
  although `Capture state changed: ENABLED` was seen three times. Pacing health shows FSR FG 2x (04:11:11), FG off,
  then DLSS FG 4x (04:11:37); from then every present logged `Post-FSR confirmed standalone normal-route bypass`.
- Root cause: that bypass (Present and Present1) forwarded Streamline's final output through the bypass trampoline and
  returned before the normal route's limiter stage, so `Apply()` never ran again. A fresh start with DLSS FG has no
  FSR phase, stays on the normal route and paces - matching the user's "worked after restart".
- Fix: `ApplyFpsLimiterBeforeBypassedFinalOutputPresent()` (routing unit) + `ApplyPostPresent()` after the forward in
  both bypasses, wrapper-owned presents excluded; `limiterActive=` added to the bypass log line. Details in
  `frame-pacing-and-limiter.md`. Regression: `tests/test_dxgi_shared_bypass_limiter.cpp` (all 3 fail on old sources;
  verified with a host g++ harness only - the Windows build/gate was not run in this Linux session).
- Unverified on hardware: next Talos FSR->DLSS switch session must show `FPS limiter pacing on post-FSR confirmed
  standalone Present bypass` plus `FPS Limiter: Active (sync=capture, limiter=reflex ...)` when recording starts.

### 2026-09-29 - Portal RTX profile overrides never applied: layer could not load the hook into the renderer

- `logs/20260929_033904` (0.1.6850, Portal RTX profile with `dlss_*_dll_path`, `streamline_dll_path`, SR/RR/FG
  presets, `dlss_debug_overlay`, `vsync_mode=fifo`, `anisotropic_filtering=16x`). The injector published the whole
  resolved config (`srPreset=13 rrPreset=6 fgPreset=2 indicator=on runtimePaths=1111`), AF was applied by the layer
  (`forced AF applied ... 16.0x`), but `hook_debug.log` had no `NvRemixBridge.exe` lines and the client logged
  `Runtime preload: skipped because inherited child renderer PID 4468 owns ...` and `DLSS indicator: skipped ...`.
- Root cause: `vulkan_layer.log` `Inherited renderer bootstrap: failed to load capture_hook_x64.dll (error=126)`.
  `LayerBootstrapInheritedRendererHook` resolved the hook beside the layer DLL, but since the runtime staging the
  layer is a copy under `%LOCALAPPDATA%\CaptureEngine\vulkan_layers\b<build>` holding only layer + gate. The claim
  was already published, so the client stood down too: no process owned the overrides.
- Fix: the controller stages `VK_LAYER_CE_host_directory.txt` (install directory) beside the layer; the layer loads the
  hook from it via `HookLoadCandidates` and `LoadLibraryFromSecurePath`; if no candidate loads it withdraws the claim
  (fail-open) and logs which settings will not apply. No ABI change (deliberately not a `DiscoveryInfo` field, see
  `dx12-injection-bootstrap.md`). Regression: `tests/test_vulkan_layer_host_directory.cpp` (3 of its 11 tests fail
  against the old sources; run under Wine with a MinGW-built gtest).
- Not a bug, still in that profile: `vsync_mode=fifo` stands down for a metered frame generator (`NOT overriding
  present mode 0 -> 2 (fifo) - this device enabled VK_NV_present_metering`), hardware-verified 2026-09-14 in
  `vulkan-forced-fifo.md`; rendered rate is bounded at `refresh / multiplier` instead (`display vertical-blank
  ceiling ... 144 fps` line).
- Unverified on hardware: the hook actually completing its bootstrap inside `NvRemixBridge.exe`. Next Portal RTX
  session must show `Inherited renderer bootstrap: loaded ...\capture_hook_x64.dll` + `... ready before Vulkan
  initialization` and `Inherited renderer: synchronized process-local DLSS/Streamline profile controls` in
  `hook_debug.log`.

### 2026-09-29 - Portal RTX inject recording stuck in "preparing": split-renderer frames dropped by media

- `logs/20260929_031827` (0.1.6849, Portal RTX: `hl2.exe` 32-bit D3D9 client PID 12472, `NvRemixBridge.exe`
  Vulkan renderer PID 17220). Layer claim `rendererPid=17220 clientPid=12472`, final-output capture published
  frames at 137 fps, but media logged `Dropping frame whose slot source PID 17220 does not match the session source
  PID 12472` for every one (`DropFull` = input, `liveFrames=0`), so the stop became a pre-live cancel.
- Root cause: the source-PID pin in `media_main_threads_inject.cpp` only accepted `GetSourcePid()` (the injected
  parent). Split renderers stamp slots with their own PID because the handles live in their handle table.
- Fix: `common/inject_frame_source_policy.h` admits a writer when `vulkanLayerClaim` = {slot PID, session source}
  and a Toolhelp check (cached per pair, unavailable snapshots not cached) proves it is the source's live direct
  child. Cursor window now resolves from the session source (the bridge owns no window). Logs:
  `Admitting split-renderer frames` / `Refusing split-renderer frames`; drop line now names admission + claim.
- Verified only by Linux-built gtest of `tests/test_inject_frame_source_policy.cpp` (no MinGW/Windows run). Next
  Portal RTX session must show the admit line, `First actual inject frame queued`, and a saved file.
