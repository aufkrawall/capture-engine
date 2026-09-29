# llm-wiki Log

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

### 2026-09-28 - Composed Vulkan present: overlay display timing matched 10 s-old frames

- `logs/20260928_042343` (0.1.6847, DOOM Eternal native Vulkan, 4K -> 1440p): the game recreated its swapchain 18x
  in 11 s (alternating 2560x1440 / 3840x2160, black window, ended at an alt-tab). CE passes present/acquire results
  through unchanged and touches no FSE call, so nothing tied that to CE, but no result was logged. Now
  `LogSwapchainInvalidationResult` (`vulkan_swapchain_result_policy.h`) names OUT_OF_DATE/SUBOPTIMAL/SURFACE_LOST/
  FSE_LOST/DEVICE_LOST per call. Open: re-run to see which result drives the flapping.
- After the switch the sensor showed `presentToDisplay` 5.9-10.5 s mean, `latchInterval n=0`, published ~29 fps
  while the game ran at 140. The cause was stale unflipped submissions claimed by unrelated VSync completions.
  Fix: 1 s completion bound and short submission retention, see `display-change-timing.md` "Completion bound and composed presentation".
  Only verified by Linux-built unit tests (no MinGW or Windows run in this session).
- Follow-up `logs/20260928_044654` (0.1.6848): catch-up gone, transition logged, but the latency row showed
  `PC Latency -`: the estimate needs display samples and composed frames had none. Added compositor-based timing
  (`display_timing_composed.h`, DWM via `QueuePacket_Stop` readiness) and split the service into
  `_internal.h` + `display_timing_service.cpp` + `display_timing_service_events.cpp` (was 820 lines). Syntax-checked on
  Linux against stub Windows/ETW headers only; next session must confirm `composed(ready>0 published>0)`.
- Same session answers the flapping: every 2560x1440 swapchain got `vkQueuePresentKHR` -> `VK_ERROR_OUT_OF_DATE_KHR`
  from the driver right away (occurrences #1-#4), the game rebuilt at 3840x2160 and later switched back to 1440p by
  itself; the other 9 of 14 recreations had no invalidation result. CE passes results through untouched: game/driver
  mode-change behaviour, not a CE defect.

### 2026-09-28 - Vulkan resize mid-recording froze video: fence published to the wrong slot

- `logs/20260928_001303` (0.1.6845, DOOM Eternal native Vulkan with NVIDIA present-on-DXGI, inject capture):
  4K -> 1440p at 00:14:00. Layer retired the swapchain and published generation 9/10 (2560x1440, new fence
  `0x2184`, logged "Published capture fence handle ... to encoderTextures"). Media opened the new textures and fitted
  1440p into the 4K recording, but frames stopped at 00:14:06.5: every tick `Deferred inject frame`, `DupDef=120`,
  `TickUnique=0` until stop. The first deferred frame had fence 802; the old generation ended at fence 801.
- Root cause: `InitializeCapture` wrote the fence to `encoderTextures` whenever `encoderTextures.ready` was set.
  Media sets it at every recording start ("Created encoder KMT textures early"), but reads that slot only when
  `useEncoderTextures` is set, which native Vulkan never sets. Media re-read the stale shared-slot handle of the
  retired fence (still alive, completed value 801) and `WaitForFrameFence` deferred every new-generation value
  above it (the ~6 s before that were encoded without a real GPU wait). Earlier generations were published before
  recording start (`ready=0`), so they used the shared slot and the bug stayed hidden.
- Fix: `ce::PublishInjectFenceHandle` / `ce::InjectFenceUsesEncoderTextureSlot` in
  `common/inject_transport_snapshot.h` hold the single slot rule for producer and media; the layer uses it and logs
  slot/usingEncoderTextures/ready. Tests: `CaptureBaseShmTest.FenceRepublishedAfterResizeReachesTheSlotMediaReads`,
  `FencePublishFollowsEncoderTextureAdoption`.
- Follow-up (same day, static analysis only, no hardware run): the DXVK late-adoption path in
  `layer_capture_capture.cpp` flipped `useEncoderTextures` without a fence. The encoder-texture slot still held
  media's own fence handle (`video_encoder_textures.cpp`, media-process handle, never signaled by anyone), and
  `ResolveFrameInput`'s encoder-owned branch opens it directly in-process, so every frame would defer at completed
  value 0. Media clears the flag at every recording stop, so this was the normal DXVK path for any game running
  before the recording. After adoption the entry has no IPC relay, so frames are signaled on the exported timeline
  fence (`state.sharedFenceHandle`, `encoderFenceValue == vulkanSignalValue`). Fix: `ce::AdoptEncoderTexturesWithFence`
  publishes that fence before the flag. Test `LateEncoderTextureAdoptionPublishesTheProducerFence`.
- Follow-up 2 (static analysis + MinGW syntax check, no hardware run): `RepublishCaptureTransportForHost` after an
  adoption published the import entry's `textureHandles` (all null) and `ipcFenceHandle` (unsignaled since
  adoption). Returning false did not help: `InitializeCapture` returns early for an unchanged initialized state,
  and `GetOrCreateSharedTextures` returned the same import. A contended try-lock also fell through to that early
  return while the present path recorded the host generation as served. Fix:
  `hook/vulkan_layer/vulkan_capture_transport_policy.h` decides Published/Rebuild/Retry. A rebuild invalidates
  the import and the current state, so `InitializeCapture` retires it and builds a layer-owned transport (deferred
  rebuilds are retried by the existing `captureStateMissing` path). Retry leaves the generation unrecorded.
  `GetOrCreateSharedTextures` retires any valid import it meets, which also fixes a same-size swapchain rebuild
  between recordings. `SharedTextureEntry::encoderTextureImport` marks imports. Tests:
  `tests/test_vulkan_capture_transport_policy.cpp`.

### 2026-09-27 - Recording-start latency: probe early stop, truthful live timing, WGC reserve-wait finding

- `logs/20260927_195021` (0.1.6844, DXGI-dup desktop + Brave audio, first recording of the session): hotkey ->
  media live 5453 ms, not the logged 6375 ms (`CheckChildProcessHealth` polls once per second). Split: spawn 0.05 s,
  `[AVSyncProbe]` 3.17 s, engine/D3D/dup init 0.28 s, start + 19 audio sources 0.24 s, pre-live warmup 0.55 s,
  encoder prewarm 0.16 s, `WGC startup delay-reserve wait` 1.00 s (budget exhausted, `partial_span_timeout`).
- Probe: every shot captured the fixed 620 ms window (`capFrames=119040` at 192 kHz) with the marker at 95 ms.
  Shots now stop at `DetectCompletedMarkerCenterFrame` (burst + 40 ms decayed guard); measured value unchanged,
  full window kept as the bound. Shot spread 5.4 ms (engine-period Start jitter), so an adaptive 3-shot exit
  was rejected. Hardware-verified in `logs/20260927_201549` (0.1.6845, same endpoint, DXGI-dup desktop): 5/5
  `stop=marker_complete`, `shotMs` 167-176, `probeMs=859.2`, latency 32.738 ms (was 32.896); hotkey -> live 2640 ms.
- Controller `Recording is live` now uses media's `recordingStartTime` stamp (`ResolveRecordingStartupTiming`);
  verified: `2640 ms ... observed after 2812 ms, liveStamp=media`. Recording healthy, CFR 4801/4801, post-mux
  audio/video end delta 0 us.
- **Open (not changed):** `SelectWgcStartupReserveCandidate` takes the frame NEAREST `latest - target` and
  rejects it when younger than `target - tol` (tol = min(half output interval, 5 ms)). For a steady source the
  phase `target mod period` is constant, so it fails on every evaluation: synthetic check with target 332.9 ms
  never succeeds at 25/28/31/40 fps. Independently, `startupReserveBelowLowWater` needs
  `ceil(delay / outputInterval)` = 40 newer frames, which a sub-output-rate source cannot supply within the
  delay. Net: sub-CFR sources (desktop, 30 fps video) likely always wait the full 1 s smoothness-attempt budget.
  In this log the timeout contract realized 320.6 ms vs 332.9 target and the extra ~0.6 s only discarded frames.
  Caution: the 1 s budget is deliberate (`GetWgcStartupReserveWaitBudgetQpc`) and the 250/500 ms input-rate
  windows feeding smoothness decisions fill during the wait - validate before shortening. In `20260927_201549` the
  source mostly ran above CFR (SourceFps 20.66..143.53) and the wait settled normally after ~0.27 s, consistent
  with the full budget being paid only by sources that stay below the output rate.

### 2026-09-27 - Session 20260927_040737 review: clean; two logging gaps closed

- 0.1.6843, 10 recordings (r0001 inject Talos with FSR FG, r0002-r0010 WGC): all `healthy`, CFR coverage
  `missing=0`, post-mux audio/video ends within 1 us, no underruns/trims, no ERROR lines.
- r0005: mux write queue grew to 421/512 MB over ~45 s (output on a network share) and drained in ~5 s; no
  backpressure, but only `QUEUE STATS` INFO recorded it. Added band/recovery warnings with writer attribution
  and a rate-limited slow-write line (`mux_queue_pressure.h`).
- r0001: swapchain rebuild -> transport generation 1 -> 2 with `frameIndex` restarting at 1 logged one
  `Inject lineage regression` + 16 `Texture slot reuse anomaly` false positives. Checks are now per
  generation (`inject_lineage.h`); the stale `lastEncodedFrameByTextureIndex` also was never reset per session.
- Not hardware-verified yet: expect `Inject lineage restarted ... generation 1 -> 2` instead of those warnings,
  and `Mux write queue reached 25%` on the next slow-output session.

### 2026-09-27 - Talos "windowed" start fatal: hidden-window create dropped the swapchain queue

- `logs/20260927_034946` (0.1.6842): the resize fix above is confirmed on hardware (4K -> 1440p with Steam, no crash).
- New case, with or without Steam: Talos options say windowed, actual borderless native 4K. The Streamline swapchain
  was created while the HWND was hidden -> `Invisible-window swapchain ... bypassing` -> no `Swapchain queue captured`
  (in `logs/20260927_031545` the same create was visible and `scQ=` its create queue). First visible Present chose `path=primaryQ` (render queue),
  `Reinit SUBMIT #1 ... devRemoved=0x887A002B`. UE's fatal path used TerminateProcess, hence no CE `.dmp`.
- Fix: park the create queue and evidence, promote them on the first visible Present (see `dx12-injection-bootstrap.md`).
- Next repro: expect `Parked create-time queue ownership ...`, then `First visible Present of hidden-window swapchain
  ... promote ... presentedQueue=same`, `Swapchain queue captured`, and `ProcessFrame ... path=scQueue`.

### 2026-09-27 - Talos resize fatal: CE's hooks sat where Steam patches (slot, then function entry)

- `logs/20260927_031545` (0.1.6841, no FG): still `FAILED ... [6,6,6,6,6,6]`, `gameoverlayrenderer64.dll=+6`. The
  0.1.6841 factory-slot handback ran twice (`CreateDXGIFactory`, `CreateDXGIFactory2`) and both times logged
  `unchanged - nothing hooked it`: **Steam does not hook factory vtable slots**; hypothesis disproved and reverted.
- Decisive: the new `slot owners` line. All five swapchain slots point into dxgi, but `[8]Present` and `[22]Present1`
  start with `E9` into a private RWX page right after dxgi's image (Steam's relay page; `CreateSwapChainForHwnd`
  already jumped into it at CE start), while `[13]ResizeBuffers` jumped to `capture_hook+0xDE860`. Steam hooks
  by patching function ENTRIES and skips one already jumping into another module. Present was fine only because
  CE hooks it below the entry (deep body). `logs/20260927_023858` (0.1.6840) is the same shape.
- Fix: `InstallResizeReconciliationHooks` installs ResizeBuffers/ResizeBuffers1 as deep body hooks
  (`kAssumedForeignEntryPatchSize=14`, like Present without a visible jump); with a third-party overlay loaded a
  refused body hook takes no site (reconciliation unavailable, so no waitable flag), otherwise the entry prepend
  stays the fallback (`resize_reconcile_hook_policy.h`). Both methods must be hooked before the flag may be added.
  `BackBufferRefTrace` GetBuffer is a deep body hook too. The `slot owners` diagnostic stays.
- The user's "with CE" Steam log was CrashReportClient's (Steam rewrites the file per process).
- Next repro: expect `resize flag reconciliation ready (... site=body-below-entry ...)`, and at the first resize
  `[13]ResizeBuffers=dxgi... entryJump->` into the same relay page as Present, then no refused resize. If the body
  hook is refused, the line says why (`refused (<reason>)`).
