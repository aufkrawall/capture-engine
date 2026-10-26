# llm-wiki Log Archive

### 2026-10-25 - HotS late inject: a thread HotS keeps at suspend count 127 refused every patch (fixed, VALIDATED 0.1.7341)

- Validated in `20261026_000201` (CE started with HotS + DXUP + RTSS already running, `[StartupScan]`):
  7 quiescences logged `1 peer(s) already held suspended ... tid=2296 count=127
  start=HeroesOfTheStorm_x64.exe+0x2529D0 ... ready=1`, no refusal; LdrLoadDll, all 9 FatalExitDump inline
  hooks, SetColorSpace1 body, resize reconciliation and the Present/Present1 body hooks below RTSS installed.
  `DXGI translation: native output ... published its output marker` and `D3D12 use: first evidence from
  presented swapchain` both at 00:02:02.969, so `DX11 check #1` (00:02:02.976) skipped; first DX12 overlay
  draw at 00:02:03.030; perf CSV api=DX12 on all 1889 rows, limiter at its 140 target. The label reads
  `DX11 (DXUP)` by design (`DxupOverlayApiLabel` names DXUP's frontend API). Late inject has no swapchain
  queue from creation, so `No swapchain queue captured ... falls back to cmdQ` (rate-limited) is expected.
  Hitches of 60-195 ms at 9-11 s with CE's frame work at ~0.2 ms and no limiter wait in those frames: not
  CE's Present work, no CE-off baseline.

- Session `20261025_230718` (0.1.7340, late inject: remote thread, processAge 43 s, RTSS loaded) named it with the
  new diagnostic: `ThreadQuiescence: live peer tid=8976 refused suspension (error=156
  start=HeroesOfTheStorm_x64.exe+0x2529D0)`. 156 = `ERROR_SIGNAL_REFUSED` = `STATUS_SUSPEND_COUNT_EXCEEDED`.
  Verified on this PC (C# probe): the 128th `SuspendThread` fails with 156 and `NtQueryInformationThread`
  class 35 (`ThreadSuspendCount`) returns 127; class 35 also reads 0 after resuming. So HotS keeps one of its own
  threads suspended 127 times - deterministic, not a race. The exiting-thread hypothesis below was wrong for HotS
  (that change stays correct; its log line found this). The maintainer's startup-injected run hit the same
  refusal 18 s after start, so the count builds up after launch.
- Why the overlay sometimes still works (the "random" part): both runs use DXUP (`Support64\d3d11.dll`). The DX11
  hook decision `DX11 check #N: ... d3d12Created=X` skips DX11 once `WasD3D12DeviceCreated()` is set. In
  `20261025_230718` the check ran at 23:07:20.799 with `d3d12Created=0 => INSTALL`: temp D3D11 swapchain, DX11
  front-end Present hooked through the vtable (no quiescence needed), `[GraphicsAPI] 'DX11 (DXUP)'`, perf CSV
  api=DX11 x1174. In `20261025_211938` the flag was already 1 at 21:19:39.730 (`=> skip`), so CE relied on the
  DX12 inline Present route, which the 127-suspended thread blocks -> no Present view at all. CE's own WARP
  bootstrap device did NOT set the flag (0 right after it at 23:07:20.777); some game-side D3D12 call (DXUP)
  marks it at a varying moment - which marker fired is not logged.
- Even when the overlay works, every inline patch is refused: FatalExitDump pre-termination hooks, LdrLoadDll
  hook, CreateSwapChain*/ResizeBuffers* body hooks, SetColorSpace1 body, DX12 Present (native-output route).
- Fix: `ThreadQuiescence::ClassifyRefusal` treats a refused peer with `ThreadSuspendCount` > 0 as held (tracked,
  handle open, context captured, never resumed); `ConfirmHeldPeersStillStopped` re-checks after every other peer
  is suspended. Only a running refuser stays `kSuspend`. Test holds a real thread at 127
  (`ThreadQuiescenceHeldPeerTest`). Details: `dx12-injection-bootstrap.md`. No service-pass retry: the blocker
  was deterministic.
- `MarkD3D12DeviceCreated(source)` now logs the first evidence once (`D3D12 use: first evidence from ...`), so
  the DX11/DX12 route race is explainable from the next log.
- Found on the way: with both routes hooked, a late-injected DXUP frontend and its native output would each
  have processed every frame, because the native output's marker is published only at its observed creation.
  `ObserveNativePresent` publishes it from the first processed Present; DXUP forwards private data (checked
  on its current master, 94ab9e2). Flow test fails without it. `api-translation-overrides.md`.
- Decided (user asked whether sharpen, AF etc. should run on DX12 under DXUP): presentation features belong
  to the native D3D12 output and the delegation enforces that; forced AF stays on the D3D11 frontend because
  safe AF needs D3D11 shader/view knowledge the translated D3D12 descriptors lack.
- Found on the way: the third-party preload's `ToolThreadSuspension` has suspended nothing since
  `cfa42fb4` (2026-08-13): it opened threads with `THREAD_QUERY_LIMITED_INFORMATION`, through which the
  start-address query reads nullptr, so no thread matched a tool module. Also its retry suspended nothing
  (processed count never reset) and a failed pass resumed threads it had not suspended. Moved to
  `hook/runtime/tool_thread_suspension.{h,cpp}` with real-thread tests; first field run with the suspension
  actually active is pending (`toolThreadsSuspended=N` on the load line). `third-party-dll-loading.md`.
- Corrected: the 940112bd CHANGELOG bullet and the wiki attributed the HotS refusals to an exiting thread.

### 2026-10-25 - Late inject: an exiting thread made every live patch fail permanently (HotS, fix pending HW)

- Follow-up of the entry below. `kSuspend` (reason=3) is set only where `SuspendThread` fails on an
  unsignaled peer; for a live thread that essentially happens only once the kernel has marked it terminated
  (`PsSuspendThread`: terminated flag or unavailable rundown -> `STATUS_THREAD_IS_TERMINATING`), the other
  case being a 127 suspend count. HotS churns threads (the maintainer's run refused the Present body hook 3x on
  `unstable-thread-snapshot` before it landed, then a GetBuffer body hook on `quiesce=suspend`). Fix: skip
  exited/exiting peers (`QueryThreadExitState`), keep them tracked, name a live refuser after resume.
  Not proven from the user's log which thread it was - the next log names it either way.
- Remaining gap: a failed postponed Present install clears `EarlyPresentHookInstallDeferred`, so any other
  transient refusal on a late inject is still not retried later. Details: `dx12-injection-bootstrap.md`.

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
