# 10 - Phase 2 audit (M12)

Read-only audit of what keeps the inject, media and logger roles out of the client process, at
`07b7853d` (2026-10-20). Decisions are DR-37..DR-46 in [08](08-decisions-and-open-questions.md); the
plan is [09](09-in-process-engine.md). Line numbers drift; names are the stable anchors.

## Role code and its process

| Role | Entry | Size | Command source | Exit |
| --- | --- | --- | --- | --- |
| inject | `InjectProcessMain` (`captureengine/injection/inject_main.cpp`) | ~5.1k lines; 13 units already in `cengine.dll` (DR-15), not the main loop | pipe, polled from the main loop, doorbell wait | Shutdown command or pipe break |
| media | `MediaProcessMain` -> `MediaProcessSession::Run` (`captureengine/media/media_main_start.cpp`) | ~28k lines | pipe, polled from `Loop()`; StopRecording runs inline after an early Ack | after every StopRecording (one recording per process) |
| logger | `LoggerProcessMain` (`captureengine/diagnostics/logger_service.cpp`) | one drain loop moving shared log rings (hooks, layers) into session files | none: named shutdown event `GenerateShutdownEventName(controllerPid)` plus the parent handle | event |
| per-app audio worker | `--process-loopback-worker` (`captureengine/media/process_loopback_worker_host.cpp`) -> `MediaEngine_RunProcessLoopbackWorker` | one process per captured app | SPSC ring + events | stop event, then forced exit after 5+1 s |

Roles take `const AppConfig&`; `RuntimeConfiguration()` is read only by the role prelude
(`captureengine/app/main_entry.cpp`). The pipe has no thread and no queue: one message in flight,
handlers inline, reply from the same thread. `Ping` and `RecordingStopped` have no production sender.

## Process globals

| Area | Mutable process state | Where it must go |
| --- | --- | --- |
| media role | 65 `media_main_g_*` (`media_main.cpp`, `inject_source_read.cpp`), 5 other globals, 76 non-const statics (21 semantic, 48 log-rate, 7 D3DKMT priority), 6 `thread_local` | 82 per media session, 14 once per process, 54 per thread (DR-41) |
| media semantic statics | `Loop()` flags `sessionInitialized`, `sharedTexturesCreated`, `receivedFirstFrame`, `injectModeStartTime`, `localReadIdx`, `lastUseEncoderTextures`; monotonic guards `s_lastInjectTimestamp`, `s_lastWgcTimestamp`, `s_lastWgcSrcQpc`; `s_cursorWindow`; counters in 05a/05c/09/threads_inject/wgc/inject_source_read/frame_submission_internal.h | correct today only because the process exits per recording; session or encoder members |
| cross-module | `g_SourceLossHealthFlags` (`common/capture/capture_retarget_policy.h`, reset by every StartRecording), `g_RecordingId` and `g_SessionDirName` (`common/ipc/process_ipc.cpp`), secure-DLL-loading state | source-loss latch and recording id per session; session dir stays a runtime singleton |
| mediaengine | 52 variables: singleton `mediaengine_g_Engine` + API mutex + pending audio-only latch; shared D3D11 device/context and its lock; retained converter (leaked on purpose); `video_encoder_g_*` statistics (non-atomic) and handle-failure cache; inject source-read sequencer + wake event; latency channel; auto-encoder last resolution | 32 instance, 19 process-wide (log callback, once flags, device-keyed probe cache, latency cache, log throttles), 1 delete (DR-42) |
| inject role | main-loop state in `inject_main.cpp`; package paths from `GetModuleFileNameW(NULL)` (`injection.cpp`, `injection_security.cpp`, `inject_main.cpp`) | component state; paths from the package layout (DR-38) |
| logging | one process sink: `g_LogFile`, one mutex across format+write+flush, one level, ~25 function-static rate gates (`common/logging/logging.cpp`) | thread-bound sinks, process level (DR-40) |
| IPC | `g_pipeNameSequence`, `g_invalidMessageLogCount`, `RegisterHost(controllerPid)` in `ProcessIPCServer::Init` | pipe-only; the runtime registers its own identity once |

## Process-wide effects of the roles

| Effect | Site | In-process (DR-39) |
| --- | --- | --- |
| `SetPriorityClass` (default `process_priority=high`) | `media_main_priority.cpp` `ApplyMediaPrioritySettings` | only with `CE_FEATURE_PROCESS_TUNING` |
| `D3DKMTSetProcessSchedulingPriorityClass` (default `auto` -> high/above normal) | `media_main_priority.cpp` `ApplyMediaGpuSchedulingPriority` | only with `CE_FEATURE_PROCESS_TUNING` |
| `SetProcessWorkingSetSize(-1,-1)` | media Init/StopRecording/idle release; inject; logger | process host only |
| `SetConsoleCtrlHandler` returning TRUE for every event, never removed | media, inject, logger | process host only |
| `InstallCrashHandler` (inject: even at `log_level=none`), WER purge, VEH | `inject_main.cpp`, role prelude | process host only; in-process `CE_FEATURE_CRASH_HANDLER` |
| `Log_Init`/`Log_Shutdown`/`Log_SetLevel` | prelude, media Init and refresh, inject reload | process host only; the runtime owns the level |
| `CoInitializeSecurity` (first caller wins) | `injection_manager.cpp` | process host only; thread host relies on proxy blankets or the poll fallback |
| `SetDefaultDllDirectories` + `AddDllDirectory(<exe>\ffmpeg)` (one-way) | `common/platform/secure_dll_loading.cpp` from the mediaengine loader, screenshot, worker host | process host only; thread host loads with explicit search flags |
| `timeBeginPeriod(1)` per recording (8 end sites) | `media_main_start.cpp` | kept, documented (per process on Windows 11) |
| MMCSS `Capture` + `THREAD_PRIORITY_HIGHEST`, thread power throttling, COM/WinRT MTA per thread | capture and encoder threads | kept (thread-scoped, RAII) |
| `MFStartup`/`MFShutdown`, `av_log_set_callback`, `avformat_network_init` | mediaengine | kept: refcounted or CE's own FFmpeg |
| `MeasureRenderEndpointLatency` plays a marker tone, deletes a legacy cache file | mediaengine audio probe | unchanged, gated by settings as today |

## Teardown that relies on process exit

- `MediaProcessSession::Run` returns on Init failure without `Shutdown()`: shared-memory views, latency
  channel, result slot and the mediaengine module stay mapped.
- `Shutdown()` stops WGC but never publishes `nullptr` for the capture object, never calls
  `ipc.Shutdown()`, never restores priorities.
- `JoinThreadWithTimeout` (`media_main_recording.cpp`) joins unconditionally after its timeout;
  `~VideoEncoder` joins the writer unconditionally after a finalize that gave up. In a process the
  supervisor waits (DR-17); in-process the same wait holds a role thread (DR-44).
- Inject source-read leases stay held after a 5 s drain timeout (`media_main_start.cpp`).
- Encoder session timer handle is closed but not nulled.
- The mediaengine shared device and converter are raw globals; the converter is leaked on purpose.

## Two media sessions in one process: collisions

All 65 media globals; the hook shared-memory views; the single-slot `cmdStartRecording`/`cmdStopRecording`
flags (only the active session polls them: the retired one is inside StopRecording); overlay
notification fields; the auto-reset `CE_InjectFrame_<hostPid>` event; `g_ControllerPid` and the
status-dark events; result slot and latency channel; manifest path; priority statics;
`mediaengine_g_Engine` (Init reuses it, Shutdown kills it for both), its API mutex, the shared device;
the source-read sequencer; `g_SourceLossHealthFlags`; `g_RecordingId`. Fixed names (`Local\CE_Disc_71`,
`Local\CE_InjectHostStopping_71`) are fine under one runtime per session (DR-7).

## Package paths that assume the executable

`MediaProcessSession::Init` (`GetModuleFileNameA(NULL)` -> `MediaEngine_Load`), `GetLocalConfigPath`,
the media cache dir and latency-probe cache, `injection.cpp`/`injection_security.cpp` (hook DLL must be
inside the exe dir), `inject_main.cpp`, the per-app audio worker
(`mediaengine/audio/process_loopback_capture.cpp`: launches `GetModuleFileNameW(nullptr)`, i.e. the
client), `SpawnChildProcess`'s fallback (`process_ipc_client.cpp`), logger's exe-dir logs fallback. The
right model exists: `RuntimePackagePaths::FromModule`.

## Self-exclusion and names

Injection exclusion is identity based (`injection_policy.h`, `runtime_process_identity.h`), registered by
`ProcessIPCServer::Init`. Name rules remain: `common/capture/screen_grab_privacy.cpp` (`captureengine.exe`),
`media_main_window.cpp`, hook dormancy in `hook/**/main_dllmain.cpp`; the Vulkan layer has no identity
check (a Vulkan client of its own would see the implicit layer; same in phase 1). `InjectControlChannel`
checks the bound process handle's PID against the discovery PID (DR-46).

## Per-app audio worker: why a process

Teardown of the AudioSes `CLoopbackMixer` graph crashes its owning process (AudioLimiterAPO double free on
the mixer's own thread; commit `5fdc11df` comment), so the code abandons the COM graph without Release
(`4e1f30ed`) and bounds the leak to one disposable process per capture (`9ff8b41d`, folded into
`captureengine.exe` by `a7a80e2e`; [multi-audio-capture.md](../multi-audio-capture.md)). A thread cannot
reclaim an abandoned graph, and the crash would kill the client. Verdict: stays a worker process in both
modes (DR-43); only its proxy (`ProcessLoopbackCapture`) runs in the media session.

## Reusable tests for the seam

`test_child_process_lifecycle.cpp` (fake `ChildProcessEffects`), `test_host_children.cpp`,
`test_media_finalizations.cpp`, `test_media_finalization.cpp`, `test_child_recording_stop.cpp`,
`test_ipc_message_validation.cpp`, `fuzz_ipc_deserialize`, `test_engine_runtime.cpp` (fake children).
`test_process_ipc.cpp` has ~8 source-text cases to replace when the seam lands.

## Open (verify during the milestone that needs it)

- FFmpeg DLL base names: a client that already loaded its own `avcodec-*.dll` makes the loader bind
  mediaengine's imports to it. Candidate fix: an FFmpeg `--build-suffix` (full FFmpeg rebuild). M19.
- Whether `StopCapture()` releases the WinRT capture session and pool (media teardown, M17).
- Unregister paths of `IMMNotificationClient` and the audio session notification (M16).
- Thread attribution of media globals is file based; verify with the call graph while moving (M17).
