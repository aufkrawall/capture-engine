# llm-wiki Log

### 2026-10-26 - GTA vkd3d crash: dump residue confirms native D3D11 on DXVK's adapter; fail-closed interop guard (pending HW)

- Re-examined `20261026_165613`. Thread `45e4` (`vkd3d_queue`) is the one that logged the interop and then
  terminated. Its unwound stack below CE's dump handler still holds
  `d3d11!CCreateDeviceCache::CAdapterCache::GetWddmVersion+0x2f` beside DXVK's `dxgi_7ffb3a2a0000+0x4556`.
  System32's D3D11 faulted reading the DXVK adapter's private state, so the resolver correction (`2db4be50`)
  addresses the actual fault, not only a suspicious mix.
- Added `hook/vulkan_layer/vulkan_native_interop.h`: `CreateD3D11InteropDevice` refuses any adapter whose vtable is
  not in System32's `dxgi.dll` (path-checked, because the fallback bare-name load can return a replacement) and logs
  the implementing module. `VulkanNativeInteropTest.*` (3) passes, including a real System32 adapter.
- Side effect of `2db4be50` recorded as an open question in `third-party-dll-loading.md`: the layer's factory call
  no longer triggers the FIFO path's system factory body capture. `vulkan-forced-fifo.md` also no longer claims
  the export body hooks that are absent from the code.

### 2026-10-26 - GTA reaches vkd3d in 0.1.7345; Vulkan capture interop receives DXVK's factory (resolver fixed, rerun pending)

- Session `20261026_165613`: replacement D3D12 device/root-signature calls, successful 3840x2160 Vulkan swapchain
  and successful CE overlay pipeline creation. The new routing is effective; the instance-only ownership issue
  is gone (ownership begins at actual swapchain creation).
- At 16:56:53.399 the Vulkan layer explicitly requested System32's DXGI factory; CE's dynamic resolver returned
  its application wrapper, which selected `...\vkd3d\dxgi.dll`. Its adapter then went to native D3D11 interop;
  the log reported NVIDIA's description with DXVK's AMD vendor/device identity. No present completed.
- `DetourGetProcAddress` excluded `capture_hook` and wrapper callers but omitted CE's Vulkan layer. Reuse the
  existing `ClassifyRequesterPath` exemption for every CE module. The flow scenario loads its importer fake as
  `VK_LAYER_CE_overlay.dll`: failed on both wrong export and replacement count before the fix, passes afterwards.
- Matching archived CE PDBs and Microsoft symbols loaded in x64 CDB. The pre-termination dump shows
  `vkd3d_queue` exiting through `RtlUserThreadStart` -> `NtTerminateProcess(0xC0000005)`; original fault already
  unwound, so do not attribute the instruction to CE or the driver from this termination-only stack. The unsafe
  runtime mix is proven; whether it was the sole cause still needs a corrected game run.
- Filtered closing product gate 0.1.7346 passed IAT/module-pin/replacement/Vulkan/DXGI-factory regressions and the
  extended flow scenario; fresh installer 45,639,050 bytes. The full native/Python/184-flow gate passed on 7345;
  the small resolver follow-up used the affected-test filter, not a second unfiltered run.

### 2026-10-26 - finish runtime replacement for cached exports; stop Vulkan adapter probes claiming presentation (pending HW)

- Handoff reported that the first replacement build still ran GTA V Enhanced on native D3D12, while DXVK's
  enumeration-only Vulkan instance made CE stand down. The original follow-up logs were not supplied here.
- Reproduced the cached-entry gap in `FlowModuleReplacement`: save System32's `CreateDXGIFactory2` directly
  from its export table before adoption, then call the same address afterwards. With entry forwarding omitted,
  the replacement count stayed 1 instead of 2. A normal `GetProcAddress` probe had accidentally saved CE's wrapper
  and passed, so the test asserts that its cached address belongs to the native image.
- Native-only entry detours then exposed a tail-call loop: the proxy tail-called System32 with the game's return
  address. Both native and replacement exports now hold a native-runtime scope around their callable predecessors,
  using paired published inline hooks. The scenario passes cached entries, static/late imports, proxy forwarding,
  explicit CE native calls and adoption surviving host shutdown. Caller scope uses the existing unload-aware module
  address cache; native Agility components and forwarded native aliases stay native.
- Vulkan presentation ownership now follows actual swapchain creation/lifetime, not IPC connection or instance
  enumeration. Temporary creation ownership protects the ICD's DXGI WSI before the first swapchain exists; failure
  releases it, replacement has no claim gap, overlapping swapchains retain it, final destruction releases it, and
  reconnect republishes existing presentation. Seven policy regressions cover these transitions without sleeps.
- Current behavior, limits and diagnostics: `third-party-dll-loading.md`. GTA translation, overlay and capture
  remain field-validation work; no game session is claimed by these focused regressions.
- Closing gate `python build.py --incremental --run-tests --skip-updates --concise` passed as 0.1.7345:
  full native suite, Python self-tests and all 184 WARP flow scenarios; fresh setup package, 45,637,974 bytes.

### 2026-10-26 - custom_dll_paths DLLs named like a resident runtime were unused second images (GTA V Enhanced + vkd3d-proton, fixed, pending HW)

- Session `20261026_145243` (0.1.7342): `ThirdParty preload: dxgi.dll/d3d12.dll/d3d12core.dll loaded from ...\vkd3d\`
  succeeded, yet the game ran on native D3D12. System32's `dxgi.dll`/`d3d12.dll` were already mapped at CE's DllMain
  (`ModulePin: pinned name=dxgi.dll ... SYSTEM32`), and Windows resolves every by-name request to the first image of a
  name, so the vkd3d copies (and vkd3d's own `d3d12core.dll` -> `dxgi.dll` import) were never reached.
- Fix: a custom DLL whose name still resolves to another image after the preload replaces it for application code
  (`hook/runtime/module_replacement*`): CE's forwarding originals move, application imports are rebound (late modules
  from the loader notification, before their DllMain), and bare-name / superseded-path / superseded-handle lookups
  from application callers are answered with the replacement; Windows modules and CE keep System32's image, a
  configured proxy keeps its full-path load. `Wrapped_CreateDXGIFactory*` used a cached System32 "unhooked export"
  and would have kept creating native factories; it now follows the application's `dxgi.dll` (the flow scenario
  failed on exactly that before the change). Rules, limits and log markers: `third-party-dll-loading.md`.
- Verified: `ModuleReplacementPolicyTest.*`, `ModuleReplacementLoaderTest.LoaderNotificationSeesBoundImports` (Windows
  11 26300 binds imports before the load notification), `FlowModuleReplacement.*`. Not verified: a GTA run on
  vkd3d-proton, and how CE's overlay/capture behave there (the Vulkan layer should own presentation).

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
