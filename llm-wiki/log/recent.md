# llm-wiki Log

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

### 2026-10-21 - binary size: hook rings, TLS scratch, CRT DWARF, shader copies (0.1.7293)

- Why the hooks were 10-11 MB: `pacing_trace` rings as statics with a non-zero slot sentinel = 5.66 MiB initialized
  `.data` (PDB symbol map, `S_LDATA32 ... Ring<49152>` 4,325,392 bytes); a `static thread_local std::array<float, 8192>`
  in `ComputeWorstPercentileFPS` = 32 KiB of a 40 KiB TLS template (also in both Vulkan overlay layers); static-CRT
  DWARF 0.2-0.6 MiB in every first-party image; DXBC blobs `static const` in headers, one copy per including TU.
- Fixed in `335ceccd` (rings heap-allocated at first arm), `9fd6ade7` (82-entry stack min-heap, exactness test over a
  full 8192 window), `92bb39c3` (`inline constexpr` blobs), `40f767f1` (`-Wl,--strip-all` with `--pdb` =
  `/debug:nodwarf,nosymtab`; PE verifier image budgets). `273a230a` removes stale x86 FG test-app copies from
  2026-05-21 that the new DWARF check caught in the 0.1.7292 gate. Gate 0.1.7293 green.
- x86 hook 10.6 -> 4.5 MiB file, 11.8 -> 5.6 MiB image; all numbers, measurement method and the open decisions
  (SVT-AV1 ~7 MiB in avcodec used only by tests, libaom scope, exe/`cengine.dll` duplication) in `binary-size.md`.

### 2026-10-21 - overlapping recordings and the in-process encoder-device leak fixed (0.1.7290)

- Overlap (`c9f6ff9e`): before acknowledging a stop, a media session detaches its view and its media engine's from
  the inject session's `SharedMemoryLayout` to a private copy (`common/capture/detached_live_state.h`,
  `common/ipc/shared_memory_ref.h`); the hook drops the session's encoder textures then. Media logs
  `[RecordingLifecycle] Detached from the shared live state (...)`. SDK run with overlapping recordings: 4/4 in default,
  isolated and self mode (was: every second recording canceled in all modes).
- Leak (`adfd78ec`, found with `8de02aa0`'s thread attribution): `av_hwframe_get_buffer` overwrites
  `frame->hw_frames_ctx`; three encode paths pre-set it, so every frame leaked a frames-context reference and the
  encoder bridge's device survived every in-process recording (`[EncoderBridge] Encoder D3D11 device released
  (references left=16)` before, `=0` after). In-process warm rounds now: threads +0, VRAM +0; over six recordings per
  runtime +3 handles and ~+4 MiB after destroy remain (was +45 threads, +311 MiB, +335 MiB VRAM for two).
  Details: `nvenc-encoding.md` (Encoder device bridge).
- Flaky `FlowD3D11SamplerAFCost.RawShaderMemoHoldsNoShader` (254 memo hits, 2 evictions of 256 shaders, once in a
  parallel gate): the shader memo's single 8-entry window per hash overflowed at a quarter of the capacity depending on
  heap layout (Python simulation of the old placement: 20 of 2000 random 256-shader sets evicted). Fixed by two-bucket
  placement (`d3d11_af_shader_memo.h`, `dx11-forced-af.md`): 0 of 20000 sets at 256 and 0 of 5000 at 512 in the same
  simulation; `D3D11AFShaderMemoTest` includes the seed-27 set the old window lost an entry on. Gate 0.1.7291 green;
  the 18 AF cost scenarios ran 8x three in parallel and the memo scenario 60x four in parallel without a failure.

### 2026-10-21 - SDK outside-repo run on 0.1.7285: sequential passes, overlap and in-process residue fail

- `tools/run_sdk_outside_repo.py --helpers all --no-overlap`: default, isolated and self all 4/4 recordings; self
  ran every helper as `sdk_outside_client.exe` with no `captureengine.exe` deployed (M25 route works). Needed
  `95531666`: the sample remembered FINALIZED events across runtimes, so round 2 never waited (recording ids restart).
- Overlap bug (all three modes, pre-existing; finalizers retire since `859954a3`, the stop-path resets are older):
  starting a recording while the previous one finalizes never goes live. Evidence (isolated, session
  `20261021_190748`): r0002 armed warmup at 56.466; r0001's `StopRecording` finished finalizing at 56.69 and called
  `ResetRuntimeDiagnostics`, which stores `capturePhase=idle` in the session-wide `SharedMemoryLayout::runtimeState`;
  r0002 then logged `Warmup-to-live commit rejected (phase=idle requested=1)` 3,336 times until shutdown. r0001's own
  final stats read `phase=warmup live=0` (r0002's phase). The same stop path also clears the inject texture
  handshake (`useEncoderTextures`, `encoderTextures.ready`), resets the inject frame ring and the encoder's
  `SetCapturePipelinePhase(kIdle)`: everything a finalizing session writes there after a newer one started is a
  cross-talk hazard. Fix direction: session-private lifecycle phase for decisions, one owner of the shared live state.
- In-process residue (default mode, after warm-up): every media session leaves about +135 handles, +22 threads,
  +3 USER objects, ~+155 MiB private and +167.7 MiB VRAM; `[RuntimeTeardown]` after destroy is the sum (+269 handles,
  +45 threads, +311 MiB, +335 MiB VRAM for two recordings). Isolated and self mode end at +1 handle. Root cause not
  investigated yet.

### 2026-10-21 - phase 3 implemented in code (M20-M26); SDK run and hardware matrix pending

- Commits `3c144005`..`6cc496ef` in local `main` (not pushed). Per-part results and open items:
  `library/11-helper-isolation.md` ("Implemented" sections), DR-49..DR-51.
- M24 evidence: the process-loopback teardown probe (`tests/test_process_loopback_teardown_probe.cpp`, DISABLED_)
  ran 21 configurations x 150 cycles twice plus 450 cycles of CE's capture class on Windows 10.0.26300 without a
  crash in any historical teardown order; the worker now stops and releases its clients and stays a process (DR-50).
- M23 result: the `ffmpeg` folder holds only `avcodec-ce-63`, `avformat-ce-63`, `avutil-ce-61`, `swresample-ce-7`,
  `swscale-ce-10`; their import tables name Windows DLLs and each other only (`verify_private_ffmpeg_names`).
- M25 found latent phase-2 bugs for clients: the first-run config template was loaded from the running executable
  (no client has it; now embedded in `cengine.dll` too), and the Vulkan layer residency registered from the
  executable's folder. The module-boundary checker ratchets exceptions against HEAD, so `ce_role_host_main` lives in
  `runtime/core/runtime_create.cpp` behind the approved `role_entries.h` interface instead of a new exception.
- A self-hosting client is the runtime host and runs the role-host image; `OwnsWindows` would have blacked out its
  own windows. `HostIdentity::applicationImage` (`<package>\captureengine.exe`) now decides.
- Closing gate on 0.1.7285: the first run failed one source-text pin (`InjectionPassesUtf16PathsToRemoteLoadLibraryW`,
  fixed in `6cc496ef`); `--resume` built and packaged `captureengine-setup-0.1.7285.exe`, then `--no-build
  --run-tests` passed the full unit suite and every Python self-test. Not run: `tools/run_sdk_outside_repo.py`
  (the user's CaptureEngine held the session), lint, `--verify`, hardware.

### 2026-10-21 - phase 3 planned: the app back to helper processes, third parties without captureengine.exe

- Plan and evidence: `library/11-helper-isolation.md` (M20-M26), DR-49. Scope decided by the user; four mechanism
  decisions open (feature-bit semantics, 1 s Matroska clusters, tray balloon, silent loopback reproducer). No code.
- Facts found while planning (all at `fa5770ff`): a crashed MKV loses up to ~5 s (FFmpeg's open cluster, 5 s / 5 MiB
  default) plus CE's packet queue; a crashed audio-only `.mka` keeps its final name and looks finished; a crash while
  live emits no FINALIZED and leaves `capture_stage_*` behind; nothing on disk records in-flight recordings;
  `config.ini` from a phase-2 template says `isolated_helpers=false`.
- FFmpeg: CE's own binaries import only Windows DLLs plus FFmpeg, but ten third-party DLLs in `ffmpeg\` share
  base-name risk in a client process; their static archives already exist in `ffmpeg_build/dependencies/prefix/lib`.
- Process-loopback crash: history reconstructed in `multi-audio-capture.md` (Open Questions); no root cause was
  ever established.
- The SDK harness only recorded audio (the sample's `--video` was never passed); the integration matrix runs one
  helper mode; crash dumps are already in-process except with a foreign overlay loaded.
- `temp/libcengine` (git-ignored) mirrors `llm-wiki/library/` again; its 2026-10-07 archive was deleted (the same
  plan is in git at `a50d1600`, two pages differing by a few lines).

### 2026-10-21 - config.ini under Program Files: three ways edits ended up needing UAC

- The installer already gave `config.ini` a Users Modify ACE (folder stays admin-only), so editors that write in place
  never needed UAC. Three gaps remained, all fixed: (1) setup granted it only to a `config.ini` it created, never on
  update; (2) the tray's "Open config" ran the editor elevated when CE is elevated, and a temp-file-and-rename save
  then replaced `config.ini` with one carrying only the folder's inheritance; (3) an elevated CE recreating a missing
  `config.ini` did the same, and an unelevated one failed silently (OutputDebugString only, before the log exists).
  Details: `installer.md` (Permissions), `configuration.md` (first-run invariants).
- Rejected: Users write access to the install folder (would let user-level code plant DLLs beside
  `captureengine.exe`). Editors that put swap/backup files beside the document (Kate's `.kate-swp`, Vim, Emacs)
  stay limited there until the planned move of `config.ini` to LocalAppData.
- Open-config route has no automated test (it starts the user's editor) and was not run elevated here; the COM chain
  to Explorer's `IShellDispatch2` was resolved unelevated in a scratch harness without executing anything.
  Manual check: run CE elevated, tray -> Open config, confirm the editor process is not elevated (Task Manager
  "Elevated" column) and `[Controller] Open config: handed to the desktop shell` is logged.

### 2026-10-21 - README split into a user guide and docs/technical-overview.md

- README review against code found stale claims, now fixed: tray right-click opens a menu (it does not exit); the
  inject/media/logger roles run on threads by default (`[Runtime] isolated_helpers`); limiter modes are
  `auto|basic|reflex` (`fg_fallback` retired); shared-memory ABI 71 (the docs no longer name a number); the Vulkan
  layer is implicit with a gate library; WGC needs 1903 (interop), per-app audio effectively Windows 11; CPU sensors
  work through the elevation service without an elevated CE (the template and plugin README said admin-only);
  `dlss_fg_factor` does not turn FG on; `crash_dump_dir` is a subfolder of `logs`; the default
  `[DesktopOverlay] mode=2` shows only the warning.
- README now covers the installer (defaults, `config.ini.new`, uninstall), tray menu, CLI (`--launch`, `--config=`,
  `--auto-record=`), the SDK, and the resident Vulkan layer's anti-cheat note. Internals moved over, corrected where
  stale.
- Future work trimmed per the user: the PresentMon plug-in is dropped for good (CE's own display-timing ETW
  implementation replaces it); DX11 object-wrapping proxy coexistence is done; the Streamline 1.x-to-2.x bridge is
  done (validated W3, 4x MFG, 2026-10-01). The template's `streamline_upgrade` STATUS still said "non-functioning"
  (since `1d8808af`, 2026-08-22) and now says validated in W3, other 1.x titles untested.

### 2026-10-21 - DX9 smoke crash: HookThread released the game's D3D9 textures during Present

- Root cause of the open `smoke_dx9` item below (x64 and x86): the run auto-recorded and then exited CaptureEngine.
  The hook saw its host go and `DeactivateHookRuntimeAndWaitForHost` -> `QuiesceHostBoundCaptureResources` ->
  `DX9Hook::OnHostDisconnect` -> `ForceCleanup` released the direct shared-ring textures on HookThread
  (`CMipMap::Release` -> `d3d9!DdDestroySurfaceLH`) while the game's thread was inside `Present`
  (`d3d9!DdBltLH -> Flush`). The device has no `D3DCREATE_MULTITHREADED` and the game holds no CE lock while it
  draws, so NVIDIA's driver thread then read freed state (`nvd3dumx+0x20ae5e`). Not phase-2 specific: a host stop
  in either helper mode takes this path. DX8 did the same with its D3D8 surfaces; OpenGL already deferred its reset
  to the owner context's next Swap.
- Fix: `RetireHostGeneration` (HookThread) only stops capturing, joins the capture worker and closes kernel handles;
  `ReleaseRetiredHostGeneration` runs the full cleanup on the game's thread at its next `Reset`/`ResetEx` (also while
  dormant: Reset fails with a live default-pool resource) or in `Init` on the next host's first Present. A Reset of
  another device leaves the captured device's objects alone.
- No automated test: `DX9Capture` needs a real D3D9 device with D3D9Ex/D3D11 sharing, which neither the unit tests
  nor the D3D12 flow harness provide, and the race is timing-dependent. Reproduction: the `smoke_dx9` integration
  run (crashed 2/2, x64 and x86, on the RTX machine). Hook log lines: `[InjectLifecycle] DX9: host generation
  retired on HookThread`, `... released on the game's thread (site)`.
- DirectDraw had the same hazard (`OnHostDisconnect` -> `ResetDirectDrawPresentationOverrides` + `CleanupDDraw(true)`
  on HookThread; the native D3D7 sidecar's reset deletes its state block on the game's device). Now
  `DDrawCapture::RetireHostGeneration` only stops capture and `ServiceRetiredDirectDrawHostGeneration` releases on
  the game's thread at the top of every Flip/Blt/BltFast (legacy, 4, 7) and CreateSurface detour and of
  `DetourD3D7DeviceRelease`, also while dormant: the D3D7 Release interception and primary-creation bookkeeping pass
  through then, and a sidecar reference outliving the application's last Release faulted Gothic II
  (`20260924_235830`). It skips CE's own bootstrap and internal calls (per-thread scopes), so it never runs on
  HookThread. Open: CE's tracked D3D7 device reference (`ReleaseTrackedLegacyD3D7DeviceIf`) still outlives an
  application Release made while CE is dormant; that is older than this change.

### 2026-10-21 - phase 2 gated and fast-forwarded into main; user retest pending

- SDK outside-repo run on 0.1.7287: both helper modes recorded every round with an output path.
- `--verify-runtime` (twice): clean build, ASan/UBSan, unit suite, 153 FG flow scenarios passed; lint failed on
  findings the phase-2 work exposed, all fixed: 10 unchecked optionals in the role thread tests; cengine.dll's
  runtime compiles never reached `compile_commands.json` (dropped since `8e4dd879`), so 7 cengine-only units were
  never linted; the unit tests' compile of product files won the dedupe over the product's (now the product wins);
  that surfaced mediaengine compiling with FFmpeg `-L/-l` (528 warnings) and two VK headers redefining
  `VK_NO_PROTOTYPES`. Baseline tightened (scope 1358 units, total 532). Fuzz (60 s per target) passed.
- Open, not analyzed further (the user retests first): the integration smoke `smoke_dx9` failed on x64 and x86. The
  recording saved (400 frames, `result=saved`); 26 ms after the stop `dx9_test.exe` faulted on an NVIDIA driver
  thread (`nvd3dumx+0x20ae5e`, read of `0x1D00000004`, every frame of that stack in `nvd3dumx`) while its main thread
  was in `d3d9!DdBltLH -> Flush` and another in `d3d9!DdDestroySurfaceLH`. Not known whether main before phase 2
  shows it. Dumps: `installed/captureengine/logs/20261021_121132` and `_121153` (not kept in git).
- The in-process media role no longer logs `[WARN] Controller IPC disconnected` after a normal stop (`6d677d2e`).

### 2026-10-21 - first outside-repo SDK run: audio-only stops were reported as canceled

- `python tools/run_sdk_outside_repo.py --helpers both` on 0.1.7286: both modes recorded all four rounds (7 KB
  `.mka` each), yet every finalized event said `canceled` with no path and the script found no file.
- Product bug (since `4db81fec`, 2026-09-19, both helper modes and the app): the audio-only branch of
  `StartRecording` never set `recordingEverStarted`, so the authenticated stop ran `CompleteAbortedRecordingStart`
  after the saved finalization. Media log: `[MediaResult] finalization=1 result=saved` then `finalization=2
  result=canceled`; the exit code carries the last one. The branch sets the latch now (`MediaRecordingLatchTest`).
- Harness: the staged template recorded into `%VIDEOS%\Capture Engine` and the script only looked below the
  deployment (and not for `.mka`). It now points `[Output] output_dir` at `<deployment>\recordings` and expects one
  file per round; the headless sample fails a finalization without an output path (canceled is `CE_OK`).

### 2026-10-21 - the media role is driven by a unit test; loader table in its own namespace

- `unit_tests.exe` links mediaengine's sources, so the loader's `MediaEngine_*` pointer globals had the exports' names
  and linking the media role into the tests aliased them (crash). The table now lives in `ce::mediaengine_api`.
- `MediaRoleThreadTest` runs `RunMediaRole` on a thread: a missing `mediaengine.dll` and a refused start both exit 1,
  close the handles the role was given (checked by the object's handle count, not a reusable handle value) and log
  to the role's sink only. A successful Init stays undriven (the unit-test build identity refuses a product DLL).
