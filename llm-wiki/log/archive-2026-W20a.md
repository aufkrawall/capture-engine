# llm-wiki Log — Archive 2026-W20a

### 2026-05-17 — WGC high-precision FP16 source to 8-bit NV12 conversion fix (build 0.1.3304 / tests 0.1.3305)

- **Input**: WGC logs at `installed/captureengine/logs/20260517_151109` for a 10-bpc SDR source with `[Video] bit_depth=8` produced black/empty video.
- **Root cause**: WGC correctly detected 10-bpc SDR and, after `R10G10B10A2` frame-pool creation failed with `0x80070057`, fell back to `R16G16B16A16_FLOAT` (`fmt=10`). The encoder's FP16-to-NV12 compatibility path then tried to create an SRV with `DXGI_FORMAT_R16G16B16A16_TYPELESS` (`fmt=9`), which D3D11 rejected with `E_INVALIDARG`. All 247 frames failed GPU color conversion and no frame was encoded. Earlier visible-but-dark attempts were consistent with missing SDR linear-to-sRGB encoding before RGB10/VP conversion.
- **Fix**:
  - Added `mediaengine/video_format_policy.h` with testable RGB SRV policy: FP16 and R10 typeless resources resolve to typed FP16/R10 SRVs, and helper paths never return typeless SRV formats.
  - `mediaengine/video_encoder.cpp` now uses the helper in direct RGB preparation and VP fallback paths. If native FP16 VideoProcessor staging fails, the fallback does a GPU fullscreen blit to `R10G10B10A2_UNORM` with a typed FP16 SRV; SDR FP16 fallback applies linear-to-sRGB before RGB10 so 8-bit NV12 output is not too dark.
  - FP16 native-vs-RGB10 compatibility choice is cached for both 8-bit and 10-bit VP outputs. Explicit 10-bit output still resolves to P010; compatibility fallback does not introduce BGRA8/NV12 before the final 10-bit encoder surface.
  - Removed the unused private `ConvertRGBtoP010_GPU()` path because it performed GPU readback and CPU P010 staging, violating the GPU-only conversion invariant if ever reused.
- **Regression tests**: Added `tests/test_video_format_policy.cpp` covering typed FP16/R10 SRV selection, no typeless SRV returns, and SDR FP16 gamma-policy behavior.
- **Verification**: Focused `python build.py --run-tests --tests-only --skip-updates --gtest-filter=VideoFormatPolicyTest.*:CapturePipelinePolicyTest.WgcExplicitTenBitDisallowsBgraFallback` passed 6/6 tests. Required `python build.py --skip-updates` passed (build `0.1.3304`). Full `python build.py --no-build --run-tests --skip-updates` passed 759/759 tests (displayed metadata `0.1.3305`).
- **Manual validation still needed**: Reproduce WGC on the same 10-bpc SDR setup with `bit_depth=8`; expected logs show FP16 source, typed FP16 SRV, SDR gamma-enabled RGB10 compatibility only if native FP16 VP input fails, successful NV12 conversion, and encoded frames greater than zero. Also spot-check `bit_depth=10` remains P010 with no 8-bit intermediate.

### 2026-05-17 — Audio-only recording mode with dedicated hotkey (build 0.1.3249)

- **Feature**: New `audio_only` hotkey in `[Hotkeys]` config (e.g. `Ctrl+F10`) starts an audio-only recording. Captures system audio, microphone, and app audio sources to a `.mka` (Matroska Audio) file — no video capture, encoding, or muxing.
- **Flow**:
  - Controller sets `audioOnly=true` in shared memory (or sends `"audio_only"` IPC payload directly to media process when no inject is active).
  - Inject process clears `audioOnly` on normal `StartRecording` (so normal recording is unaffected).
  - Media process `StartRecording()` checks `g_AudioOnly`: skips WGC/inject capture pipelines, encoder thread, all video frame handling. Calls `MediaEngine_StartRecording()` which initializes an audio-only muxer.
  - MediaEngine: skips `VideoEncoder` creation; creates a lightweight `AVFormatContext` with `"matroska"` format and `.mka` extension. Starts audio sources immediately (no video frame sync). `WritePacket()` routes directly to `av_interleaved_write_frame()` on the audio-only muxer.
  - `StopRecording()` stops audio thread, stops sources, writes trailer, closes muxer. No encoder thread join or capture pipeline teardown.
  - Overlay shows `AUDIO HH:MM:SS` (inject overlay) and red indicator dot (pseudo-overlay).
- **Source anchors**: `common/shared_defs.h` (CaptureState::audioOnly), `common/config.h/cpp` (hotkeyAudioOnly), `captureengine/main.cpp` (ToggleAudioOnlyRecording, WithInjectSharedMem), `captureengine/media_main.cpp` (g_AudioOnly, IPC payload), `mediaengine/mediaengine.cpp` (audio-only muxer, Init/Start/Stop paths), `hook/common/overlay_adapter.cpp` (AUDIO label), `captureengine/mediaengine_loader.h/cpp` (MediaEngine_SetAudioOnly export).
- **Verification**: `python build.py --skip-updates` passed (build `0.1.3249`). `python build.py --no-build --run-tests --skip-updates` passed all 754 tests.
- **Bugfixes** (build 0.1.3250+):
  - Race: inject's StartRecording handler cleared `audioOnly=false`, overwriting controller's value for audio-only recording. Moved clear to controller's `ToggleRecording()`.
  - Init order: MediaEngine was initialized once at startup with a VideoEncoder before audioOnly flag was set. Fixed by forcing a shutdown/reinit in `ensureMediaEngineReady` when `g_AudioOnly` is true.
  - No encoding: PullAndEncodeAudio only fired from video frame processing. Fixed by calling it from the AudioLoop with wall-clock elapsed time when audioOnly is true.
  - Corrupt output: `WritePacket` now rescales audio PTS from codec time_base to muxer stream time_base before `av_interleaved_write_frame`, and writes to the configured output directory.
- **Status**: Audio-only recording mode implemented and working:
  - Hotkey `audio_only=` in `[Hotkeys]` starts/stops audio-only `.mka` recording
  - All audio sources (system audio, mic, app audio) captured to separate tracks
  - Tracks are padded with silence to equal length at stop (FIFO overflow was root cause of shorter tracks — padding now in 4k-sample chunks)
  - Stale `audioOnly` flag leak fixed (clear in shared memory after read, also clear at top of normal StartRecording in media_main.cpp)
  - F9 video recording was broken by accidental `videoEnc->Start()` deletion during restructuring; restored
  - Normal recording audio encoding (`encodedSamples=0`) was caused by missing `videoEnc->Start()` — restored
  - This also means the audio-only `PullAndEncodeAudio` approach was abandoned; audio-only uses direct `EncodeSamples` in the AudioLoop with resampled float data, plus a `SetRecordingStart(0)` call for each encoder
- **Known issue**: Track lengths match within ~38ms (ALAC frame rounding). Post-processing with `ffmpeg -i input.mka -filter_complex "[0:a:0][0:a:1][0:a:2]amix=inputs=3:duration=shortest" output.mka` can produce a single mixed track if needed.

### 2026-05-17 — DX11 forced-AF stable-resource and traversal-overhead follow-up (build 0.1.3246 / tests 0.1.3247)

- **Input**: BioShock Infinite DX11 32-bit logs at `installed/captureengine/logs/20260517_085737_lowslow` with app-forced AF and `installed/captureengine/logs/20260517_090137_ingame16af` with app AF commented out and the game set to 16x AF.
- **Findings**:
  - The game-owned 16x AF control run created normal anisotropic sampler descriptors (`Filter=0x55`, `Aniso=16`), so the sampler descriptor itself is not the corruption trigger. The difference is which shader/resource contexts receive replacement and how much wrapper bookkeeping runs around streaming.
  - The control run still emitted wrapper forced-AF draw/resource stats and millions of candidate-registry misses even though app AF was disabled. Other graphics overrides kept the wrapper active, and forced-AF bookkeeping was leaking into a configuration where it could not change the draw.
  - In the app-forced run, stable safe material SRVs could still be demoted to `pending-streaming-quiet` when unrelated material-streaming noise reopened the global gate, explaining blurry/crisp oscillation after load.
- **Fixes**:
  - Added cached runtime forced-AF enabled gating to wrapper sampler/SRV/shader/draw/resource-write paths. If app AF is default/off, the wrapper forwards state and restores any leftover forced samplers, but skips AF draw stats, SRV tracking, and resource-write candidate checks.
  - D3D11 sampler creation diagnostics now distinguish `deferred runtime AF` from `passthrough AF disabled`, so a game-owned AF control run is easier to read.
  - Stable allowed SRVs now bypass the global material-streaming quiet gate. New or unstable SRVs still wait for warm-up and global quiet, but already-stable material textures should not flip back to originals due to unrelated streaming.
  - Candidate-resource tracking now has a small per-thread negative cache plus an epoch invalidation path, reducing repeated shared-map/private-data checks for hot non-candidate resources during traversal.
  - Diagnostics now expose disabled-runtime pass-throughs, `stableGlobalBypass`, `registryNegHit`, quiet-reopen delayed dirty events, and role probation/recovery counters.
- **Source anchors**: `hook/apis/dx11_hook.cpp`, `hook/common/sampler_override_utils.h`, `hook/wrappers/d3d11_devicecontext_wrap.cpp`, `hook/wrappers/d3d11_devicecontext_wrap.h`, `tests/test_sampler_override_utils.cpp`, `llm-wiki/dx11-forced-af.md`.
- **Verification**: `python build.py --skip-updates` passed (build `0.1.3246`). Focused run `python build.py --no-build --run-tests --skip-updates --gtest-filter=SamplerOverrideUtilsTest.*` passed 21/21 tests (displayed metadata `0.1.3247`).
- **Stale-risk / next validation**: Re-run BioShock Infinite with app-forced AF=16x. Expected improvement is no forced-AF wrapper draw stats when app AF is disabled, lower traversal `registryMiss` with visible `registryNegHit`, stable color SRVs staying crisp through unrelated streaming noise via `stableGlobalBypass`, and remaining blurry textures explained by explicit skip reasons rather than silent non-reapplication.

### 2026-05-16 — Pseudo-overlay: fix process_list multi-line parsing, add debug logging (build 0.1.3213)

- **Bug**: `Trim(rest)` with default charset `" \t\r\n\"()"` stripped the `(` from `process_list=(`, making `rest == "("` always false. The multi-line parenthesized block was never detected — every entry silently skipped. `pseudoProcessListSet` stayed false, the `GetStr` fallback returned `"("` (useless), and the process list remained empty.
- **Fix**: `common/config.cpp:982` — `Trim(rest, " \t\r\n\"")` preserves parentheses.
- **Debug logging added** to `captureengine/pseudo_overlay.cpp`:
  - `IsForegroundTarget()`: logs processList value, foreground PID, OpenProcess/QueryFullProcessImageNameA failures (with GetLastError), normalized exe name vs each list item, match result, cache hit/miss.
  - `OnTimerTick()`: logs warning activation with reason, deactivation with reason (recording started / foreground lost / mode changed).
  - `UpdateOverlay()`: logs shouldHaveVisibleOverlay/suppressOverlay decisions, warning window show/hide with screen coordinates and dimensions.
- **Regression tests** in `tests/test_config.cpp`:
  - `PseudoOverlayProcessListMultiLine`: multi-line with uncommented entries.
  - `PseudoOverlayProcessListMultiLineWithComments`: multi-line with one `;`-commented entry.
  - `PseudoOverlayProcessListMultiLineEmpty`: multi-line with all entries commented (empty result).
- **Source anchors**: `common/config.cpp:982`, `captureengine/pseudo_overlay.cpp:293-347+524-552+630-1040`, `tests/test_config.cpp:236-285`.
- **Verification**: `python build.py --skip-updates` passed (build 0.1.3213). `python build.py --no-build --run-tests --skip-updates` passed 750/750 tests.
- **Stale-risk**: Low. The Trim fix is a one-character charset change. Debug logging uses LogDebug — no impact at info/warn level.

### 2026-05-16 — Video output path: default "captures" subfolder, fallback + I/O hang fixes (build 0.1.3188)

- **Problem 1**: When `[Video] output_dir` (e.g. `Z:\captures`) is inaccessible (network drive disconnected), `GenerateOutputFilename()` fell back to `exeDir` (the program's own install folder), cluttering it with MKV files. The default `output_dir=` was empty, meaning files landed directly in the exe directory.
- **Problem 2**: No writability check for existing directories. If the configured dir existed on a disconnected network drive, `fs::exists()` returned true, creation was skipped, and `avio_open2`/`avio_open` could hang indefinitely.
- **Problem 3**: `VideoEncoder::Stop()` used unbounded `writerThread.join()`. If the async writer thread was blocked on FFmpeg I/O (disconnected network drive mid-recording), `Stop()` blocked forever. The controller's 10-second `TerminateProcess` was the only safety net.
- **Fix 1 — Default config**: Changed default `output_dir` from empty to `captures` (relative, resolves to `exeDir/captures`) in both `common/config.cpp:401-402` and `captureengine/config.ini.template:124-125`.
- **Fix 2 — Fallback to captures**: `GenerateOutputFilename()` in `mediaengine/video_encoder.cpp:624-683` now falls back to `exeDir / "captures"` (instead of bare `exeDir`) when the configured path fails. Added `IsDirectoryWritable()` helper using `GetFileAttributesW` that detects inaccessible paths (e.g. disconnected network drives).
- **Fix 3 — Join timeout**: `VideoEncoder::Stop()` at `mediaengine/video_encoder.cpp:4359-4373` now uses `WaitForSingleObject(hThread, 5000)` instead of unbounded `writerThread.join()`. If the writer thread doesn't finish in 5 seconds, it's detached (the controller's 10-second `TerminateProcess` still handles the zombie).
- **Source anchors**: `common/config.cpp:401-402`, `captureengine/config.ini.template:124-125`, `mediaengine/video_encoder.cpp:624-683` + `4359-4373`.
- **Verification**: `python build.py --skip-updates` passed (build 0.1.3188). `python build.py --no-build --run-tests --skip-updates` passed 747/747 tests.
- **Stale-risk**: Low. The `WaitForSingleObject` on `native_handle()` is Microsoft STL-specific but this is a Windows-only project. The `GetFileAttributesW` check is a best-effort pre-check — the avio_open2/avio_open retry is not implemented (too complex due to fmtCtx stream rebuilding), so a race condition where the drive disconnects between the pre-check and file-open could still produce a hang (mitigated by the 5s join timeout). The `captures` directory is lazily created (not at startup); first recording will create it if missing.

### 2026-05-16 — Linux cross-compile: PKEY_Device_FriendlyName link error fix (commit 3ef86ff)

- **Problem**: Debian mingw-w64 cross-compile failed with `undefined reference to PKEY_Device_FriendlyName`. The `<functiondiscoverykeys_devpkey.h>` header uses `DEFINE_PROPERTYKEY` which only produces an `extern` declaration when `INITGUID` is not defined — MinGW on Linux never defines `INITGUID`, so no backing symbol is emitted.
- **Fix**: Replaced `#include <functiondiscoverykeys_devpkey.h>` with a local `static const PROPERTYKEY` definition using the raw GUID `{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}` and PID 14.
- **Source anchor**: `mediaengine/audio_capture.cpp:3-7`, commit `3ef86ff`.
- **Stale-risk**: Low. The local GUID must stay in sync with the canonical Microsoft value. If cross-compile support is no longer needed, restoring the header would be simpler.

### 2026-05-16 — Idle power optimization: reduced wakeup frequency across processes (build 0.1.3176)

- **Problem**: On Ryzen 5700X (poor C-state efficiency), CaptureEngine added ~2W idle power despite ~0.1% CPU load. Frequent thread wakeups from Sleep/MsgWait prevented deep C6 package sleep.
- **WMI and early injection**: WMI uses `WITHIN 0.5` via `ExecNotificationQueryAsync` on COM thread pool — kept unchanged. Inject main loop sleep does NOT gate WMI timing (WMI callbacks are async on COM thread pool, independent of main loop). No changes affect early-injection timing for anisotropic filtering/render overrides.
- **Changes** (7 files, +71/-48 lines):
  1. **Inject adaptive sleep** (`inject_main.cpp:592`): `Sleep(250)` when no game attached (`sourcePid == 0`), `Sleep(100)` when game is active. 10→4 wakeups/sec when idle.
  2. **Inject IPC poll throttle** (`inject_main.cpp:471`): `PeekNamedPipe` only every 250ms when idle (sourcePid==0), every iteration when game active. Reduces kernel-mode transitions from 10→4/sec.
  3. **Controller config polling 1000ms** (`main.cpp:1136`): Changed from 250ms to 1000ms. Config changes detected within 1s — still instant from user perspective.
  4. **Controller MsgWait timeout 2000ms** (`main.cpp:172-198`): Default wait increased from 1000ms to 2000ms, config polling cooldown from 250ms to 1000ms. Combined with #3: controller loop wakeups drop from ~4/sec to ~0.5/sec.
  5. **Sensor DiscoveryInfo caching** (`sensor_service.cpp:49+59+62+168`): OpenFileMapping/MapViewOfFile done once at startup instead of every 1-second iteration. Eliminates 4 kernel calls/sec (OpenFileMapping, MapViewOfFile, UnmapViewOfFile, CloseHandle per loop).
  6. **Media timeBeginPeriod scoped to recording** (`media_main.cpp:5098+5265-6268`): `timeBeginPeriod(1)` moved from `MediaProcessMain()` startup into `StartRecording()`, released in `StopRecording()`. 1ms timer resolution (which blocks deep C-states system-wide) now only active during active recording. All error-path cleanups verified. Only applies when media process is running (auto-record enabled).
  7. **Pseudo-overlay timer 500ms** (`pseudo_overlay.h:128`): Timer interval increased from 100ms to 500ms. `OnTimerTick` already rate-limits indicators to 500ms in mode 0. Cuts WM_TIMER wakeups from 10/sec to 2/sec when pseudo-overlay is enabled.
- **Source anchors**: `captureengine/inject_main.cpp`, `captureengine/main.cpp`, `captureengine/sensor_service.cpp`, `captureengine/media_main.cpp`, `captureengine/pseudo_overlay.h`.
- **Verification**: `python build.py --skip-updates` passed (build `0.1.3176`). `python build.py --no-build --run-tests --skip-updates` passed 747/747 tests. All changes are timing-invariant for tests.
- **Stale-risk**: Low. All changes are simple interval increases or handle caching. Media timeBeginPeriod scoping is the riskiest — verify that recording start/stop transitions in edge cases (rapid toggle, error during recording init) properly release the timer.

### 2026-05-16 — Idle power optimization follow-up: bugfixes and final tuning (build 0.1.3184)

- **Bug #1: GetControllerLoopWaitMs threshold mismatch**. Changed config polling interval from 250ms to 1000ms but forgot to update the wait-function threshold (remained at 250). This caused `GetControllerLoopWaitMs` to return 0 after only 250ms, creating a 446 KHz busy-wait for 75% of the config cycle.
  - Fix: line 177, `>= 250` → `>= 1000`.

- **Bug #2: Config check boundary race**. Config check used `> 1000` while `GetControllerLoopWaitMs` used `>= 1000`. When `configElapsed` hit exactly 1000ms, the wait function returned 0 (busy-wait) but the config check didn't fire (`1000 > 1000` is false). Created ~16ms spin loop every second.
  - Fix: line 1172, `> 1000` → `>= 1000`.

- **MWMO_INPUTAVAILABLE removed** from `MsgWaitForMultipleObjectsEx`. Not the root cause but the flag was unnecessary — the standard pattern (PeekMessage drain + MsgWait without INPUTAVAILABLE) works correctly.

- **Final iteration rate**: ~2.5 Hz (down from 446,000 Hz), limited only by the pseudo-overlay timer at 500ms. Controller loop properly blocks at MsgWait between WM_TIMER events.

- **Power improvement**: From ~2W to ~1W extra idle power on Ryzen 5700X. The remaining 1W is the intrinsic process overhead: inject Sleep(250), sensor 1s loop, pseudo-overlay GDI rendering 2/sec, child process context switches.

- **Lessons**:
  - Always keep `GetControllerLoopWaitMs` threshold and config check threshold synchronized.
  - Use `>=` (not `>`) for timeout comparisons to avoid 1-tick boundary races at default timer resolution.
  - `MWMO_INPUTAVAILABLE` can cause spurious wakeups on some Windows configurations; prefer standard PeekMessage+MsgWait pattern without it.

### 2026-05-15 — DX11 forced-AF shader-slot mixed-role gate (build 0.1.3173 / tests 0.1.3174)

- **Input**: BioShock Infinite DX11 32-bit logs at `installed/captureengine/logs/20260515_140702`. User reported GPU underutilization was mostly gone and the game no longer crashed after loading the save game, but material textures still looked blurry.
- **Finding**: The latest stable run had no crash dumps. `fps_limiter_trace.log` showed the basic limiter settling at the configured 140 FPS after load transients. `hook_debug.log` showed the object-level mixed-role gate dominating the AF decision: final wrapper stats reached about 5.0M `mixedRole` skips, while there were only about 65k allowed decisions. The logs also showed proper sRGB material candidates (`srvFmt=72`, full mip chains) on color slots sharing the same raw linear/wrap `ID3D11SamplerState` object that was used on unsafe/problematic normal-map slots (`srvFmt=83`). That made the previous per-sampler-object taint too broad.
- **Fix**:
  - Replaced the raw-sampler-object mixed-role cache with a shader-slot role cache keyed by current pixel shader pointer plus sampler slot.
  - The gate still blocks a shader-slot role if that exact slot is observed with both allowed material SRVs and unsafe/non-color/problem resources, preserving the anti-churn stability behavior.
  - Unrelated color slots can now receive AF even when UE3 reuses the same `ID3D11SamplerState` object for nearby normal or mask slots.
  - Updated diagnostics from sampler-object wording to `shader slot mixed safe/unsafe resource role` and `shader-slot role blocked after unsafe resource`; draw stats now label the count as `mixedRole(shaderSlotSkips=... blocks=...)`.
- **Source anchors**: `hook/common/sampler_override_utils.h`, `hook/wrappers/d3d11_devicecontext_wrap.cpp`, `tests/test_sampler_override_utils.cpp`, `llm-wiki/dx11-forced-af.md`.
- **Verification**: `python build.py --skip-updates` passed (build `0.1.3173`). Focused run `python build.py --no-build --run-tests --skip-updates --gtest-filter=SamplerOverrideUtilsTest.*` passed 17/17 tests (displayed metadata `0.1.3174`).
- **Stale-risk / next validation**: Re-run BioShock Infinite with AF=16x. Expected proof is many more `Wrapper: AF allow` / `Wrapper: AF reconciled` events for sRGB color slots, lower object-style mixed-role suppression, no renewed crash after save-game load, and no return of major GPU underutilization. If a specific shader-slot role still logs mixed-role skips, inspect whether that exact slot alternates color and unsafe resources before broadening further.

### 2026-05-15 — log_level=none/off disables all log directory creation, crash handler, and session manifest (build 0.1.3172 / tests 0.1.3172)

- **Problem**: When `log_level=none` was set in `config.ini`, the engine still unconditionally created `logs\` root directory and a timestamped session subfolder (`logs\YYYYMMDD_HHMMSS\`), wrote `session_manifest.txt`, installed the crash handler (writing minidumps to that folder), and did `CleanupOldSessionDirs`. This violated the expectation that `log_level=none` means zero debug machinery for maximum performance.
- **Root cause**: Directory creation and crash handler setup ran in `WinMain` *before* `LoadConfig`, so the log level was not yet known. The code used the pattern "create dirs early for crash handler safety, then check log level later" — but the directories and crash handler were never gated afterward.
- **Fix**:
  1. **`captureengine/main.cpp`**: Moved `LoadConfig` before all log directory creation. Wrapped `CreateDirectoryA`, `SetCrashDumpDirectory`, `InstallCrashHandler`, `CleanupOldSessionDirs`, `WriteSessionManifest`, and the later `SetCrashDumpDirectory` update in `if (IsAnyLoggingEnabled(g_Config.logLevel))` blocks.
  2. **`captureengine/logger_service.cpp`**: Added early-return guard `if (!IsAnyLoggingEnabled(config.logLevel)) return 0;` (defense-in-depth; the logger is never spawned when logging is off).
  3. **`hook/common/hook_common.cpp`**: Gated `CreateDirectoryA` in `BuildLogFilePathForModuleAddress` on `HookDebugLoggingEnabled()` so the hook-side fallback log directory is also not created when debug logging is off.
  4. **Diagnostics**: Added `OutputDebugStringA` notification when `log_level=none` causes skipped directory/machinery setup.
- **Source anchors**: `captureengine/main.cpp:1254-1289` (restructured startup), `captureengine/main.cpp:1337-1342` (gated session dir + manifest), `captureengine/main.cpp:1392-1394` (gated crash dir update), `captureengine/logger_service.cpp:18-21` (early return), `hook/common/hook_common.cpp:144-148` (gated dir creation), `tests/test_config.cpp` (new `LogLevelNoneMapsToOff` and `LogLevelOffMapsToOff` tests).
- **Verification**: `python build.py --skip-updates` passed (build 0.1.3172). `python build.py --no-build --run-tests --skip-updates` passed 747/747 tests including the 2 new regression tests.

### 2026-05-15 — Waitable-object Present pacing replaces backbuffer_count override (build 0.1.3147 / tests 0.1.3148)

- **Problem**: Changing `BufferCount` from 3→2 at swapchain creation or ResizeBuffers causes crashes in games that hardcode per-buffer D3D12 resource arrays (command allocators, fence pools). GetBuffer dummy resources (aliasing or committed) can't fix the mismatch because the crash is in the game's internal allocator management, not GetBuffer.
- **Root cause**: The game's Asura engine sizes per-buffer resources at D3D12 init time based on its OWN copy of the creation descriptor, not from `GetDesc()` or runtime BufferCount.
- **Fix**: Add `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT` to swapchain creation in ALL creation paths (15 total). Before every Present call, wait on the waitable object handle. This paces Present submissions so the DWM flip queue never fills beyond the effective depth of 2 frames — achieving the same vsync latency reduction as `backbuffer_count=2` WITHOUT changing the physical BufferCount.
- **Key insight**: The waitable object signals when DWM has consumed a frame from the flip queue. Waiting on it before Present limits the queue depth without touching BufferCount, so games never see a count change and their internal resource management is never disrupted.
- **Source anchors**: `hook/apis/dx12_hook.cpp:5247+5421+5594+5715` (DX12 creation flags), `hook/apis/dx11_hook.cpp:236+259` (DX11 creation flags), `hook/wrappers/dxgi_factory_wrap.cpp:221+319+379+461`, `hook/wrappers/wrapper_hooks.cpp:61`, `hook/wrappers/dxgi_swapchain_wrap.cpp:1059` (WaitFrameLatency in Present), `hook/wrappers/dxgi_swapchain_wrap.cpp:1382` (WaitFrameLatency in Present1), `hook/common/dxgi_shared.cpp:825` (WaitBackbufferFrameLatency), `hook/common/dxgi_shared.cpp:2299+2835` (detour Present wait), `hook/common/dxgi_shared.cpp:3787+4237` (CallOriginalPresent/1 waits).

### 2026-05-15 — Flip-model backbuffer_count override experiments

- **Input**: Feature request — capture system audio from more than one audio output device simultaneously, and capture from more than one microphone simultaneously, using the existing mixing/sync/resampling/codec infrastructure.
- **Context**: The pipeline already supported multiple audio sources per track with mixing via `std::vector<AudioConfig> audioSources` in `mediaengine.cpp`. Each source already had its own `AudioCapture`, `AudioRingBuffer`, and `AudioResampler`. The gaps were: (1) config parsing only produced one `[Audio]` and one `[Microphone]` section, (2) `AudioCapture::Start()` ignored the `deviceId` parameter and always used `GetDefaultAudioEndpoint()`.
- **Changes**:
  - **`mediaengine/audio_capture.cpp`**: Replaced the "deviceId ignored for now" stub with proper device selection. First tries `IMMDeviceEnumerator::GetDevice()` by opaque WASAPI ID (UTF-16). If that fails, enumerates via `EnumAudioEndpoints()` and matches by `PKEY_Device_FriendlyName`. If both fail, falls back to `GetDefaultAudioEndpoint()` with a warning log.
  - **`common/config.h`**: Added `kMaxAudioSections = 8` constant.
  - **`common/config.cpp`**: Replaced single-section parsing for `[Audio]` and `[Microphone]` with loop-based multi-section parsing following the `[AppAudio.N]` pattern. `[Audio.N]` and `[Microphone.N]` sections (N=1..8) are parsed with minimal fields (enabled, device, track). Codec/bitrate/sample_rate/bit_depth/downmix inherit from the global `[Audio]` section. Smart suppression: if any `[Audio.N]` section exists, the legacy `[Audio]` section is only added when it explicitly sets `enabled=true` — prevents a footgun where writing only `[Audio.1]` would also create an unwanted default loopback source. The legacy `[Microphone]` section now only pushes when `enabled=true` (was always pushed before, matching MediaEngine's internal skipping). Backward compat preserved.
  - **`captureengine/config.ini.template`** and **`common/config.cpp` (CreateDefaultConfig)**: Added documented `[Audio.N]` and `[Microphone.N]` template entries.
  - **`tests/test_config.cpp`**: Added 7 new tests covering: numbered audio sections with correct device/track/codec, numbered microphone sections, suppression of legacy `[Audio]` when numbered sections exist, explicit legacy + numbered coexistence, disabled numbered sections, empty sections skipped, and codec inheritance from `[Audio]`.
- **Default tracks**: Legacy `[Audio]` → 1, legacy `[Microphone]` → 2, `[Audio.N]` → 10+N, `[Microphone.N]` → 20+N, `[AppAudio.N]` → 2+N (unchanged).
- **Source anchors**: `mediaengine/audio_capture.cpp:63-98`, `common/config.h:13`, `common/config.cpp:1201-1310`, `captureengine/config.ini.template:199-220`, `tests/test_config.cpp:501-650`.
- **Verification**: `python build.py --skip-updates` passed (build 0.1.3117). `python build.py --no-build --run-tests --skip-updates --gtest-filter="*Config*"` passed 38/38 tests (displayed metadata 0.1.3118). All 7 new ConfigTest tests pass.

### 2026-05-14 — DX11 forced-AF streamed-SRV warm-up and mixed-role sampler gate (build 0.1.3115 / tests 0.1.3116)

- **Input**: BioShock Infinite DX11 32-bit logs and dumps at `installed/captureengine/logs/20260514_160317`. User reported improved but still incomplete GPU utilization and the same later render-thread crash while the game streamed textures after save-game load.
- **Dump analysis**: `cdb -z ... -y "srv*;%USERPROFILE%\\Programme\\build\\captureproject\\installed\\captureengine" -c ".ecxr; k; q"` again showed the game-side x86 access violation in `BioShockInfinite!AK::MemoryMgr::SetMonitoring+0x157943` (`eax=0`, read from `[eax]`). The external dump comment still reported `Fatal error! Map: S_TWN_P` with NVIDIA driver `32.0.15.9649`; the freeze dump only captured the post-crash UI dialog thread.
- **Finding**: The wrapper/vtable forwarding guard reduced duplicate path activity, but draw-time sampler churn was still excessive. The last draw stats before crash included about 1.45M real sampler set calls and over 2.19M unchanged dirty deferrals. SRV cache misses rose sharply during the load/streaming window, and logs showed UE3 material passes alternating otherwise-allowed color SRVs with unsafe/problematic non-color resources such as BC5-like normal maps on nearby samplers. This makes freshly streamed SRVs plus sampler objects reused across mixed resource roles the current leading generic root-cause candidate for both underutilization and late crash pressure.
- **Fix**:
  - Added `PendingStableObservation` to the D3D11 forced-AF resource decision model and diagnostics.
  - Added wrapper SRV cache warm-up metadata. Otherwise-allowed SRVs are skipped until observed repeatedly and old enough in draw-count terms; unsafe/non-color/problematic resources still block immediately.
  - Added sampler-role private data on original `ID3D11SamplerState` objects. If a sampler object is seen with both allowed material resources and unsafe/non-color/problem resources, forced AF is blocked for that sampler object to avoid rapid replacement/original sampler flipping.
  - Added wrapper diagnostics for warm-up skips, mixed-role sampler skips/blocks, real sampler set calls, deferred unchanged dirty marks, and SRV cache activity.
  - Added focused unit coverage for mixed-role sampler blocking in `SamplerOverrideUtilsTest.*`.
- **Source anchors**: `hook/common/sampler_override_utils.h`, `hook/wrappers/d3d11_devicecontext_wrap.cpp`, `hook/wrappers/d3d11_devicecontext_wrap.h`, `tests/test_sampler_override_utils.cpp`, `llm-wiki/dx11-forced-af.md`.
- **Verification**: `python build.py --skip-updates` passed (build `0.1.3115`). Focused run `python build.py --no-build --run-tests --skip-updates --gtest-filter=SamplerOverrideUtilsTest.*` passed 17/17 tests (displayed metadata `0.1.3116`).
- **Stale-risk / next validation**: Re-run BioShock Infinite with AF=16x. Expected proof is lower `realSetCalls`, lower reconcile/bind churn, visible `warmupSkips` and possible `mixedRole` blocks during streaming, no crash after save-game load, and less GPU underutilization. Texture sharpness may remain conservative for samplers/resources classified as pending or mixed-role until runtime evidence shows a safe broader policy.

### 2026-05-14 — DX11 forced-AF wrapper/vtable forwarding guard (build 0.1.3107 / tests 0.1.3108)

- **Input**: BioShock Infinite DX11 32-bit logs and dumps at `installed/captureengine/logs/20260514_113020`. User reported continued GPU underutilization and the same render-thread crash after save-game load.
- **Dump analysis**: `cdb -z ... -y "srv*;%USERPROFILE%\\Programme\\build\\captureproject\\installed\\captureengine" -c ".ecxr; k; q"` showed the game-side x86 access violation still in `BioShockInfinite!AK::MemoryMgr::SetMonitoring+0x157943` (`eax=0`, read from `[eax]`). The external dump metadata reported `Fatal error! Map: S_TWN_P` with NVIDIA driver `32.0.15.9649`.
- **Finding**: The logs now proved the returned wrapper context and the raw D3D11 context vtable hooks were both processing the same real context. Wrapper-origin calls to `m_pReal->PSSetSamplers`, `*SetShaderResources`, `PSSetShader`, and draws re-entered the raw vtable detours. This created two AF state machines on one context: `Wrapper: AF allow ...` would be followed by raw `DX11: AF allow ...`, then wrapper reconciliation back to the original sampler. The resulting draw/reconcile/bind churn matched the GPU underutilization reports.
- **Fix**:
  - Added a thread-local DX11 wrapper-forwarding guard exported from `dx11_hook.cpp` / `dx11_hook.h`.
  - Wrapped `CWrapD3D11DeviceContext` forwarding of sampler binds, shader resources, `PSSetShader`, and draw calls with an RAII forwarding scope.
  - Raw vtable detours now forward straight to the original function under that guard and skip raw AF state tracking/reconciliation for wrapper-origin calls. The raw path remains active for unwrapped contexts.
  - Kept the conservative implicit-only shader-sampling rule: `sample_b` and `sample_l` are tracked and logged but not force-AF eligible after the BioShock load-scene instability.
- **Source anchors**: `hook/apis/dx11_hook.cpp`, `hook/apis/dx11_hook.h`, `hook/wrappers/d3d11_devicecontext_wrap.cpp`, `hook/wrappers/d3d11_devicecontext_wrap.h`, `hook/common/sampler_override_utils.h`, `tests/test_sampler_override_utils.cpp`, `llm-wiki/dx11-forced-af.md`.
- **Verification**: `python build.py --skip-updates` passed (build `0.1.3107`). `python build.py --no-build --run-tests --skip-updates` passed 736/736 tests (displayed metadata `0.1.3108`).
- **Stale-risk / next validation**: Re-run BioShock Infinite with AF=16x. Expected proof is wrapper AF logs without matching raw `DX11: AF allow ...` for wrapper-forwarded calls on the same context, lower wrapper reconcile/bind churn, no crash on save-game load, and improved GPU utilization. Bias/LOD-only material textures may still be skipped by the conservative shader rule and remain blurry until a broader Blackwell-safe policy is validated.

### 2026-05-14 — DX11 forced-AF bootstrap and graphics override diagnostics (build 0.1.3099 / tests 0.1.3100)

- **Input**: BioShock Infinite DX11 32-bit logs at `installed/captureengine/logs/20260514_053147` with `anisotropic_filtering=16x`, `cpu_prerender_limit=1`, `backbuffer_count=2`, `vsync_mode=fifo`, and a 140 FPS basic limiter. Textures were still blurry.
- **Findings**:
  - `inject.log` proved shared-memory config updated before injection (`af=16x`, `cpuPrerender=1.00`, `backBuffer=2`, `fpsLimit=140(ON)`).
  - `fps_limiter_trace.log` showed the basic limiter settling around 140 FPS after startup transients; do not chase this through vsync behavior.
  - `hook_debug.log` showed `SetMaximumFrameLatency(1)` failed with `0x887A0001`, but DX11 created the manual prerender query ring and logged `Prerender buffered wait lookback=1`, so `cpu_prerender_limit=1` was active through the fallback path.
  - AF config and shader metadata existed, but there were no AF draw/bind/allow/reconcile lines. That made missed draw-context coverage more likely than the material-resource classifier being too strict.
- **Fix**:
  - Deferred D3D11 AF bootstrap is now per immediate context instead of process-wide. Temporary UE3 devices can no longer consume the only bootstrap pass before the real game context appears.
  - `Wrapped_D3D11CreateDevice` returns a wrapped immediate context; `Wrapped_D3D11CreateDeviceAndSwapChain` keeps raw device/swapchain compatibility while returning a wrapped immediate context. This gives games that cached raw vtable draw pointers a COM-wrapper draw interception path.
  - Pixel-shader metadata is registered into both the vtable-hook and wrapper caches, so a raw-device plus wrapped-context path can still make shader-aware AF decisions.
  - New diagnostics include `Wrapper: AF draw hook hit`, additional wrapper bind/reconcile proof, and `AF_bootstrap(complete=... retry=... disabled=...)` in DX11 summaries.
  - D3D11 creation paths now apply/log `backbuffer_count` in `CreateDeviceAndSwapChain`, factory `CreateSwapChain`, and `CreateSwapChainForHwnd`, with actual post-create buffer counts. DX12 refresh logs actual swapchain buffer count when a backbuffer override is configured.
- **Source anchors**: `hook/apis/dx11_hook.cpp`, `hook/wrappers/wrapper_hooks.cpp`, `hook/wrappers/d3d11_devicecontext_wrap.cpp`, `hook/apis/dx12_hook.cpp`, `llm-wiki/dx11-forced-af.md`, `llm-wiki/regression-testing-and-logging.md`.
- **Verification**: `python build.py --skip-updates` passed (build `0.1.3099`). `python build.py --no-build --run-tests --skip-updates` passed 735/735 tests (displayed metadata `0.1.3100`).
- **Stale-risk / next validation**: Re-run BioShock Infinite with AF=16x. Expected proof is `Wrapped_D3D11CreateDevice: Returned wrapped immediate context`, `Wrapper: AF draw hook hit`, `Wrapper: AF sampler bind tracked`, and either `Wrapper: AF allow` / `Wrapper: AF reconciled` for eligible material textures or detailed skip lines. If backbuffer override still appears to require a resolution change, compare creation-path requested/actual logs against later `ResizeBuffers` override logs.

### 2026-05-13 — Inject CFR audio/content-sync drift fix (build 0.1.3091 / tests 0.1.3092)

- **Input**: `installed/captureengine/logs/20260513_133907` from injected CFR capture. Subjectively, audio lagged behind video by the end of playback.
- **Finding**: Final duration diagnostics were clean (`Final packet timeline ... maxPacketDelta=1 us`, metadata `maxDelta=0 us`), so the issue was not an audio tail past video. The trace still ended with `TimerRebase=64`; inject CFR was allowed to discard accrued CFR timer debt during stalls. That can delete scheduled visual repeats and make video content jump forward while audio remains continuous, producing content-level desync even though stream packet ends match.
- **Fix**: CFR timer-rebase debt is now preserved for both WGC and inject. Stop-time draining is generalized to CFR: inject can close already accrued CFR debt with cached last-frame repeats because it has no separate source drain after capture stop, while WGC keeps the stricter queued/buffered-captured-frame requirement to avoid frozen repeat-only tails. CFR catch-up can also use the last frame when no fresh inject frame is available, and logs now say `CFR stop drain` with `path=WGC|inject`.
- **Source anchors**: `common/capture_pipeline_policy.h`, `captureengine/media_main.cpp`, `mediaengine/mediaengine.cpp`, `mediaengine/audio_sync_utils.h`, `mediaengine/audio_encoder.cpp`, `mediaengine/video_encoder.cpp`, `mediaengine/mux_invariants.h`, `tests/test_capture_pipeline_policy.cpp`, `tests/test_audio_sync_utils.cpp`, `tests/test_mux_invariants.cpp`, `llm-wiki/cfr-capture-sync.md`.
- **Verification**: `python build.py --skip-updates` passed (build `0.1.3091`). `python build.py --no-build --run-tests --skip-updates` passed 726/726 tests (displayed metadata `0.1.3092`).
- **Stale-risk / next validation**: Re-test inject CFR under encoder/GPU stress, especially 4K120 10-bit AV1 `preset=p5`. Expected: no audible lag, pitch shift, crackle, or tail mismatch; packet-level and metadata durations should still match, and stop-drain logs should show CFR debt being closed rather than discarded.
