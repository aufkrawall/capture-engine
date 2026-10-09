# llm-wiki Log

### 2026-10-09 - Opus tracks ran up to a frame longer than the video

- Codec-finalization pass of the A/V matrix: Opus failed on WGC, inject and dxgi_dup (`audio_timeline`), every other
  codec passed. Decoded Opus was `target + 1448` samples (e.g. 394248 vs 392800) and CE's own post-mux probe warned
  `Post-mux audio duration mismatch maxDelta=13500`. Cause: for `target % 960` in 1..648 libopus drains a final packet
  that starts after the target; its own end skip (648) plus CE's 800 exceeded the packet, so decoders ignored it.
  The mux test only used `target % 960 == 0`. Fix and tests in `multi-audio-capture.md` (Opus final packet).
- The probe double-counted the short tail packet's implied skip; fixed separately
  (`ComputeResidualTerminalDiscardSamples`). 0.1.7067 live: all Opus tracks `delta=+0`, zero mismatch warnings.
- Open: inject raw offset varies 28-52 ms per recording; a marginal inject mean offset (17 ms vs 15) after
  self-calibration is run noise, not the Opus fix.

### 2026-10-09 - WGC 60 fps from a 144 Hz window starved; A/V matrix leads were stale

- Release-readiness audio audit. The matrix quick gate failed WGC (inject passed). Two findings, one product, one
  harness. Product: WGC ALAC/60 against the 144 Hz stimulus repeated 40.6% of frames (722 pool trims, 8-frame
  runs) in both the 229 ms jitter-floor and the product-default 331 ms reservoir configurations; the 120 fps case
  was clean. Cause: the reservoir/pool is sized for output fps * 1.25 but fills with every source frame; admission
  throttled only at `retainedHigh`, which collides with the reserved-slot soft pressure and the newest-frame trim.
  Fix: budget-rate admission meter + per-frame `sourceFrameSpan` into the cadence predictor (without it the
  thinned stream lagged the source by a constant 9.2 ms). Details: `wgc-capture.md` (Ingress budget meter).
- Same audit: the end-of-session smoothness deficit used the 300 ms reservoir target even for a 22 ms floor and
  raised a false visual fault (`GetWgcSmoothnessRequestedDelayQpc`).
- Harness: the fixed stimulus leads (WGC 76, inject 45/50, tuned 2026-06-28) no longer matched this machine (raw
  offsets WGC ~0, inject ~35-52) and produced a constant -70 ms WGC "fail". `run_av_sync_matrix.py` now
  measures the lead per method and warns on drift; `--audio-latency-autodetect` added. Open product question
  recorded in `cfr-capture-sync.md`: the probe's video delay over-corrects WGC (-15..-25 ms) when the video stamp is
  not the present time. Earlier same day: matrix runner paths/triage tool forwarding, five analyzer self-test groups
  that never ran (`if False:`), two-window track correlation for short clips.
- Evidence: 0.1.7064 quick gate (WGC/inject x ALAC/AAC) passes; WGC/60 duplicates 0%, offsets +1..+3 ms. Gate
  `--incremental --run-tests` passes. Real-game hardware runs (Talos/GTA FG switching, WGC 60 fps) still pending.

### 2026-10-09 - Orphan override Streamline copies beside a resident game interposer

- Follow-up of the entry below: when the game's `sl.interposer.dll` is resident, `PlaceStreamlinePluginSet` no longer
  preloads override `sl.common`/`sl.dlss*` (`IsForeignStreamlineInterposer`, final, logged once). The orphan had
  captured CE's single `slGetPluginFunction` forward pointer in `20261009_110607`. Details in
  `graphics-overrides-and-frame-pacing.md` and `frame-generation/dlss-driver-settings.md`. Hardware run pending.

### 2026-10-09 - Game window never appeared: nvapi_QueryInterface detour forwarded to itself

- Report `logs/witcher3windownotappear` (0.1.7058, Witcher 3 + ReShade, second launch p13924, manual dump
  `witcher3_13924_*.dmp`): no window, `FreezeWatchdog` silent (`monitoredTid=0`, no present in flight). Dump: the
  game main thread (`slInit` -> sl.dlss DllMain -> `nvngx_dlss` -> `LoadLibraryExW` -> `HookedLdrLoadDll` ->
  `NotifyHookModuleLoaded` -> `ReflexLimiter::Init`) and the hook thread (loader-notification sweep ->
  `RetargetCachedNvApiPointers` -> `driverQueryInterface(...)`) both sat at `ReflexDetour_QueryInterface+0x73`, two
  frames deep, constant stack. `g_ReflexLimiter.origQueryInterface_` was `nvapi64!nvapi_QueryInterface+0` itself: a
  tail-jump loop through the inline patch, not a lock.
- Cause: nvapi64 was already mapped when the hook thread armed (`Inline hook installed on NvAPI_QueryInterface ...
  orig=<trampoline>`), so `origQueryInterface_` held the trampoline; `Init()` (run on every `LoadLibrary("nvapi64.dll")`
  while `available_` is false) then re-stored `GetProcAddress` = the patched entry. In the healthy launch p28376 nvapi64
  loaded after the arm, so `Init()` ran first and no inline hook existed. Racy by load order, not by timing.
- Fix: `ce::fps_limiter_policy::ShouldAdoptExportedNvApiQueryInterface` + compare-exchange in `Init()`; the trampoline is
  recognised by identity (`directQueryInterfaceTrampoline_` is published before `origQueryInterface_`). The earlier
  loader-notification sweep (same day) was only a victim here, not the cause. Hardware run pending.

### 2026-10-09 - Dynamic MFG factor lost to a startup race (Witcher 3 + ReShade)

- Session `20261009_110607` (0.1.7057): the in-game factor won. DRS answers were 2 (preset, forced mode) instead of 6;
  `sl.common` of the game was never wrapped. Passing neighbours (105819, 105938) had identical config/build.
  Race: CE preloaded override `sl.common` (hook thread, 13.984) while the game mapped its own (game thread, 14.001),
  between the startup sweeps and the LdrLoadDll hook (14.062); the loader notification saw it but did not patch.
- Fix: loader notification patches the `GetProcAddress` import of any DRS consumer inline + hook-thread sweep flag.
  Details and the open orphan-`sl.common` observation: `frame-generation/dlss-driver-settings.md`. Hardware run pending.

### 2026-10-09 - DLSS OFF lost overlay ownership behind distinct device views

- Session `20261009_101805` (0.1.7056, Witcher 3 with ReShade): two queue-derived device views alternated
  while the physical presentation route stayed live. ECL adoption treated pointer inequality as migration
  and cleared exact PostSL/normal/capture identities. First OFF at 10:20:05.637 blanked 97 presents;
  final OFF initially rendered, then discovery resumed after its 600-frame grace and blanked at 10:20:28.026.
  Device removal stayed zero and the confirmed queue remained retained; this is an ownership failure.
- Discovery is now fallback-only before GetDevice. Completed swapchain captures explicitly bind the
  queue/device before publishing exact proofs; replaced references retire outside the queue lock. No
  game/module special case, delayed recovery, backbuffer copy or weakened ownership gate is added.
- The native policy regression and both new real-hook flows fail on the original implementation. `FlowQueueBinding` adds
  distinct WARP COM device views, repeated DLSS ON/OFF beyond teardown grace, both Present methods,
  native return, explicit binding replacement and reference-balance checks. Closing build 0.1.7057 passes
  the full native suite, Python tool self-tests, all 36 FG flows and binary/privacy checks; its fresh installer
  is 39060080 bytes. Real-game ReShade retest remains pending. Current invariant: `overlay-rendering.md` queue ownership.

### 2026-10-09 - Automatic dump missed a CE fault consumed by game handling

- The same Witcher 3 startup session retained one unresolved CE access violation in its first-chance slot, but
  had no CE crash dump or crash log and ultimately exited with code zero. The manual dump's render stack had
  already returned to game code. Record-only first-chance handling relied on an unhandled filter or crash exit;
  those never arrived. Background freeze suppression also cannot substitute for preserving the original fault.
- The classifier now captures undebugged hardware faults whose instruction address is inside the module hosting
  CE's crash handler. `crash_first_chance::Install` caches that immutable image range before VEH registration;
  classification performs only atomic reads and range arithmetic. Foreign faults, breakpoints, managed and C++
  exceptions retain their existing behavior. The fault is also recorded for retry if immediate capture fails.
- The external-helper routing regression fails before the fix (zero calls), then passes with exactly one call and
  the original thread/address/registers. Module-identity and policy tests cover foreign/null/noncanonical
  addresses, debugger ownership and ignored software exceptions. Full build 0.1.7056 passes native tests,
  Python self-tests and all 34 FG flows and produces a fresh 39057612-byte installer. Real-game retesting remains pending.

### 2026-10-09 - Witcher 3 DX12/ReShade startup renderer type confusion

- Session 20261009_093652 (0.1.7053): second Present switches between a ReShade device view and the native view,
  causing descriptor-free backend replacement. The manual dump has no exception stream, but CE's first-chance
  slot retains the render thread's access violation in `OverlayAdapter::DestroyResourcesLocked`, inlined
  `DX12Backend::HasInlineUploadsInFlight`: it reads a noncanonical glyph-data pointer as a completion buffer.
  x64 CDB verified the archived CE PDB GUID/age and loaded matching Microsoft ntdll symbols.
- `DX12DescFreeBackend` derives from `RendererBackend`, not `DX12Backend`. The shared DX12 label was used as
  proof for five unsafe casts, including retirement. Adapter binding now caches an explicitly advertised texture
  interface; descriptor-free resources and generic virtual upload-slot dispatch remain intact.
- A native custom-renderer regression fails before the fix because texture helper calls overwrite unrelated
  storage. Added poisoned-storage shutdown/rebind checks and real WARP texture/custom lifecycle coverage to the
  descriptor-free format probe. Closing build 0.1.7056 passes the full native suite, Python tool self-tests and all
  34 FG scenarios; fresh installer is 39057612 bytes. The real-game cold-start check after installation remains pending.

### 2026-10-08 - Streamline UI-tag log flood (Witcher 3 Remastered)

- Session 20261008_220437: 11466 `Official UI tag record opportunity` + ~11.5k tag lines (85% of hook_debug.log) although the lines were already
  gated. W3 sends single-tag `slSetTagForFrame` calls cycling 13 buffer types, all one stream, so each call was a "change". Tag types now key the
  stream, 64 slots, per-stream heartbeat (details in `regression-testing-and-logging.md`). 0.1.7052 cut it to 1915 lines of 4438 (20261008_221807):
  types 0/1 alternate set (1280x720) / clear (null resource) calls; 0.1.7053 adds "has a resource" to the stream. Expect ~15 tag records per session.
- Same session confirmed dynamic MFG end to end (all five DRS keys answered by `sl.common`, multiplier 2x/3x/4x at ~138 fps on 144 Hz, no dump).

### 2026-10-08 - Dynamic MFG, second cause: cached NvAPI pointers in sl.common

- Session 20261008_214202 (0.1.7050): the startup sweep patched `sl.common`'s `GetProcAddress` import, yet no lookup was routed and
  `NvAPI_DRS_GetSetting` was still wrapped only for `nvngx_dlssg`. Disassembly: `sl.common`'s static NvAPI layer caches the driver's
  `nvapi_QueryInterface` in `.data` from `slInit`, before CE attached. 0.1.7051 retargets such cached copies (`RetargetCachedNvApiPointers`,
  restored in `ShutdownIATHooks`); details in `frame-generation/dlss-driver-settings.md`. Hardware run pending; expect
  `had already cached the driver's NvAPI entry points; retargeted nvapi_QueryInterface x1` for `sl.common.dll`, then
  `wrapping NvAPI_DRS_GetSetting for ...sl.common.dll (streamlinePlugin=1` and `answered` lines for 0x10562D0F / 0x10CF4125.

### 2026-10-08 - Dynamic MFG never reached Witcher 3 Remastered's own Streamline core

- Session 20261008_211749 (0.1.7049, `dlss_fg_mode=dynamic`, native SL 2.14.1): only `nvngx_dlssg.dll` was wrapped, so the preset and forced
  mode were answered but the count/target/VSync keys (read through `sl.common`) never were. `sl.common` was mapped before CE's loader
  notification and the Streamline-skipping IAT sweep, so its `GetProcAddress` import was never patched. Added a pinned startup sweep over
  already-loaded DRS consumers plus per-module logging (`frame-generation/dlss-driver-settings.md` "Consumers mapped before CE arrived").
  Hardware run pending; look for `via=startup sweep` and `wrapping NvAPI_DRS_GetSetting ... sl.common.dll (streamlinePlugin=1`.

### 2026-10-08 - Overlay fonts oversaturated for one HDR10 frame (Witcher 3)

- Session 20261008_184332: the game flipped `R8G8B8A8` -> `R10G10B10A2` + HDR10 -> `R8G8B8A8` (3 presents, the middle one 288 ms).
  CE's ResizeBuffers entry is refused behind Steam's overlay, so CE never saw it; the descriptor-free pipelines stayed `fmt=28` while the RTV
  was `fmt=24`. Fixed with per-format pipelines + live format sync (`overlay-rendering.md` HDR invariants). Hard evidence: with the retarget
  disabled the flow probe gets D3D12 error 613 from the debug layer. In-game re-test pending.
- Also fixed (pre-existing, found as a ~1/12 flake of `FlowDLSS.NativeReturn...` under load, identical on HEAD): the post-process pass cached its RTV
  by back buffer pointer, stale after a swapchain replacement (`post-processing-sharpen.md`). Always rewritten now.
- HDR10/scRGB frames of a gamma-only config were `pass-failed` in the DX12 post-process ledger (`SharpenDX12PresentedFrame` mapped the
  deliberate `gamma_hdr_passthrough` idle to `PassResult::Failed`). Fixed with `RequestHasWork` (`post-processing-sharpen.md`);
  the SDR/HDR10/SDR flow scenario asserts the full ledger again.

### 2026-10-08 - Gamma build crashed Witcher 3 on DLSS FG off

- Session 20261008_172629 (0.1.7034): CreateRTVs got DEVICE_REMOVED 3 ms after the first FG-off frame; the game's
  own int3 message says GPU crash. Earlier gamma builds (7032, same FG-off sequence x3) survived; the new element
  was the early post-process call that bypassed `skipOverlayDraw`. Reverted to the gated placement + regression
  test. Unproven by GPU capture; the 154811 AV dump (7032) is a game-side fault on another thread, unrelated.

### 2026-10-08 - Generic display gamma correction

- `Graphics.display_gamma`/`gamma_source` implemented in the CAS/RCAS post-process stage (D3D11, native D3D12,
  Vulkan, FSR FG callback output). Details: `post-processing-sharpen.md` "Generic display gamma".
- Closing gate 20261008_172*_build_7034 passes (unit, Python self-tests, 32 FG flow scenarios with `srgb`,
  setup 0.1.7034). Fixed two source-scan tests broken by the change (Vulkan registry key now includes the
  queue; resolved-queue scan window 3000 -> 4000 chars). Hardware run pending.
