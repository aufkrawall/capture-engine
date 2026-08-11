# DX12 Injection Bootstrap

Last cross-checked: 2026-08-11

Primary sources:
- `captureengine/injection.cpp`
- `captureengine/injection_manager.cpp`
- `captureengine/injection_inject.cpp`
- `captureengine/injection_policy.h`
- `captureengine/inject_main.cpp`
- `captureengine/inject_lifecycle.cpp`
- `captureengine/media_main.cpp`
- `captureengine/pseudo_overlay.cpp`
- `common/inject_overlay_policy.cpp`
- `common/shared_defs.h`
- `hook/main.cpp`
- `hook/main_host_lifecycle.cpp`
- `hook/common/ipc_client.cpp`
- `hook/vulkan_layer/layer_ipc.cpp`
- `hook/wrappers/inline_hook.cpp`
- `tests/test_crash_handler.cpp`
- `tests/test_shared_runtime_state.cpp`
- `tests/test_capture_coordinator_source.cpp`
- `tests/test_inject_capture_source.cpp`

## Scope
This page describes how DX12 injection and overlay bootstrap currently work, with emphasis on how to make inject and overlay behavior work optimally for DX12 games without turning the wiki into a substitute for the code.

## Facts
- Host-side injection currently uses a delayed-injection thread instead of injecting blindly at process start.
- The startup scan also discovers already-running whitelisted processes. It queues them through the same graphics-probe injection path, so starting CaptureEngine after a DirectX/OpenGL title is supported without changing the established game-start path. The globally installed Vulkan implicit layer remains resident/dormant before a host exists and can activate when CaptureEngine signals that target.
- The delayed-injection thread polls for graphics API readiness and tries to detect `d3d12.dll` with `EnumProcessModules()`.
- If `d3d12.dll` is detected, the current logic waits for a short additional settle window before attempting injection.
- If module enumeration fails, the injector logs the failure and falls back to conservative non-D3D12 timing instead of aborting the target entirely.
- Pending startup-scan injection is processed while `InjectionManager::Update()` owns the injection mutex. Any already-injected / recently-failed checks from that loop must use the locked variants instead of public helpers that re-lock the same mutex. `ShouldLaunchPendingInjection(whitelisted, alreadyInjected, recentlyFailed)` captures the intended gate: launch only for a live whitelisted process that is not already injected and not in the recent-failure window. Healthy logs include `Launching deferred injection thread for ...` shortly after the startup scan sees a whitelisted target.
- Host generations use a global host-stopping event plus target-PID-specific DirectX/OpenGL and Vulkan reactivation events. CaptureEngine retains each target-event handle while injection is pending/active, so a signal sent before `LoadLibrary` completes cannot disappear. The resident side consumes an old signal before attempting discovery/IPC reconnection; a newer host signal arriving during that attempt therefore remains set for the next attempt.
- Closing CaptureEngine performs a cooperative **resident deject**, not a remote `FreeLibrary`. The hook/layer first enters dormant pass-through, disables capture/overrides, releases host-bound API capture resources, clears its old source publication, and acknowledges dormancy. Vulkan retains only the dispatch, queue, and swapchain metadata required for correct forwarding and later reactivation. The image, COM wrappers, trampolines, and saved foreign-chain targets remain mapped until process exit because forcibly unloading them cannot be made safe while game or third-party code may retain those addresses. A later CaptureEngine process can reactivate the same resident image.
- The Vulkan layer pins its own module before starting its process-lifetime host watcher. That watcher and its proc-address entry points must never execute from an image that a loader reference-count transition can unmap.
- IPC reconnection atomically publishes the new generation's shared mapping. Prior generation views/handles are deliberately retained until target exit so an already-entered detour cannot dereference unmapped memory while shutdown races it. This is bounded by host restarts within one game process, not a per-frame allocation.
- DirectX 8/9/11/12, DDraw, OpenGL, DXGI wrappers, and Vulkan Present entry points all check the dormant/shutdown state before CE work and forward to the exact saved predecessor. API `OnHostDisconnect` paths serialize teardown of host-owned capture transports with their capture mutexes; OpenGL defers context-owned deletion to the owning GL context.
- `inject_main.cpp` sets shared runtime flags so the controller-side pseudo-overlay suppresses itself while the injected overlay handoff is pending or active.
- Fresh application routing comes from named `[Profile.*]` sections. `video_capture=inject` implies the injector's full video/overlay/graphics route without a second setting; WGC, DXGI, and no-video sources normally remain non-injected. Optional `dll_injection=always` selects overlay/graphics-only injection for those non-injected sources, while `never` blocks injection and `when_needed` remains accepted as the redundant default spelling. Legacy `injection_mode`, `injection`, `whitelist`, and `overlay_whitelist` remain parser inputs with their historical behavior.
- Hook-side video publication is a separate runtime decision. The raw `captureRequested` bit remains the recording/session signal used by REC UI and capture-synced policy, while `kCaptureRuntimeFlagInjectVideoCaptureRequested` is set only while the active video path consumes injected frames. The injector clears this path flag before publishing every new start request and again on stop, so an interrupted prior media session cannot leak stale inject publication into a screen-grab start. WGC/DXGI recordings therefore keep injection features without paying for unused injected texture copies.
- With global `capture_method=auto`, an explicit `video_capture=inherit` profile normally resolves to inject and resolves to WGC when `dll_injection=never` is set. Explicit per-profile WGC/DXGI/none routes override that global choice. New DLL-only profiles that omit `video_capture` have no video route; compatibility injection keys retain their historical implicit inherited route. During a live inject-to-WGC fallback, inject publication remains enabled until WGC first-frame proof; the media coordinator clears the inject-video flag only after committing the WGC path and before stopping the inject capture pipeline.
- In current wrapper builds, DX12 hook bootstrap is for state tracking and `ExecuteCommandLists` tracking. Present and `ResizeBuffers` interception comes from wrappers rather than DXGI vtable hooks.
- In wrapper builds, DX12 hook init is deferred until real D3D12 device creation is observed. In no-wrapper builds, the hook instance is initialized more eagerly so late injection does not miss the recovery path.
- Late injection must not feed the pre-ECL-hook warmup presents into the
  ECL-pattern FSR heuristic as "interpolated" evidence: until the game queue's
  ECL hook is live, every present looks zero-ECL. The heuristic only counts a
  zero-ECL frame after a real frame has been observed and only once per real
  frame (interleaved cadence), and a latched heuristic deactivates after 120
  consecutive real frames without interpolation evidence unless direct FFX API
  confirmation exists. Session `logs/20260811_211623` (Strange Brigade DX12,
  no FG) latched phantom `FSR_FG` from 12 warmup zero-ECL presents + 5 real
  frames and skipped the overlay forever with `scQueue=null`.
- Late injection into a game whose Streamline/DLSS-G modules are already
  loaded misses the Streamline FG signal and the runtime-ownership latch, so
  `g_StreamlineFGRunning` and `dx12_hook_g_FGRuntimeOwnsSwapchain` stay false
  even when the FG planner correctly classifies `DLSS_FG` (via the NVNGX
  `CreateFeature` hook). The dedicated overlay queue must stay disabled for
  NVIDIA DLSS FG in that planner-only state too: the first backbuffer-drawing
  overlay submit on the dedicated queue returns `DXGI_ERROR_ACCESS_DENIED
  (0x887A002B)` and removes the device (session `logs/20260811_214252`, Talos
  DLSS FG resume after Alt+Tab). Fix (build 0.1.5921):
  `ShouldDisableDedicatedOverlayQueueForNvidiaFrameGeneration` covers both
  detection states, and the submit sites reserve the dedicated queue for
  pure-offscreen lists (`ShouldUseDedicatedQueueForOverlaySubmit`). At FG
  resume the warm overlay therefore keeps drawing on the live present queue
  with no reinit and no blank, matching the healthy startup sessions.
- Resident-hook reactivation re-binds all session-scoped diagnostics to the
  replacement host's log directory: the crash dump directory, the perf_metrics
  CSV (`PerfLogger::Init(..., true)` finalizes the old file and restarts frame
  numbering), and the cached `fps_limiter_trace.log` path. Session
  `logs/20260811_212728` previously wrote the perf CSV into the prior
  session's folder because these were initialized once at injection time.
- **The DXGI present/resize "Vulkan pass-through" decision is evidence-based, not module-presence-based.** `hook/common/vulkan_renderer_policy.h` owns the decision: `HasD3DUsageEvidence` (D3D12/11 device creation, d3d12.dll/d3d11.dll presence, legacy D3D modules; under DXVK only a real D3D12 device counts) and `ShouldTreatVulkanAsActiveRenderer` (Vulkan layer ownership, or vulkan-1.dll loaded without D3D evidence). `CheckAndInstallHooks` publishes the result through `DXGIShared::SetVulkanActiveForDXGIPresentPath`, and `DXGIShared::IsVulkanActive()` (consulted by the DX12 present routing, Present1, and ResizeBuffers) reads that flag. A DX12 UE5 title that merely loads vulkan-1.dll as a transitive dependency (RoboCop: Rogue City, session `logs/20260809_134642`) must keep full DXGI processing and the overlay; the old one-shot `GetModuleHandleW(L"vulkan-1.dll")` latch permanently bypassed `HandleDX12ProcessFrame`, so the original game queue was never captured and PostSL activation never completed. The inner guards in `DX12Hook::Init` / `DX11Hook::Init` use the same flag, so late-loaded vulkan-1.dll cannot suppress hook installation in a D3D process.
- Inline-hook trampolines in a CFG-enabled x64 host begin on a `PAGE_TARGETS_INVALID` page, are built without write/execute overlap, sealed RX with `PAGE_TARGETS_NO_UPDATE`, and register only their aligned entrypoint. MinGW's Kernel32 import library lacks `SetProcessValidCallTargets`, so the exact export is resolved from already-loaded KernelBase/Kernel32 and invoked through one x64 `guard(nocf)` bootstrap wrapper. Do not turn that into a general unchecked-call helper or widen its use: the exception exists only because a normal dynamically resolved call is CFG-checked before it can register the new target. Session `20260716_013421` and a debugger reproduction proved the old indirect call fast-failed with subcode 10 in `InlineHook::FinalizeExecutableTrampoline` before hook initialization.
- Windows export suppression also makes some dynamically resolved system exports invalid guarded indirect-call targets even when their address is genuine. `IATHook::DetourGetProcAddress` therefore reaches the real API through the hook DLL's deliberately unpatched static `GetProcAddress` import, never a cached self-resolved function pointer. Supplied session `20260716_021732` and CDB sessions `20260716_022257`/`20260716_022313` put the resulting `FAST_FAIL_GUARD_ICALL_CHECK_FAILURE` in the old detour call while NVIDIA and Vulkan components initialized.
- Fatal-dump hook bootstrap is transactional with respect to callable originals: each surviving termination/fail-fast trampoline is atomically published before its inline target patch becomes live and before any IAT route can reach it. Fatal IAT patching is limited to application modules; modules anywhere under the Windows directory retain their original imports so forwarded system implementations cannot recurse through CE. Fallback calls use the hook DLL's own unpatched static Kernel32/ntdll imports, including a direct static `RtlExitUserProcess` fallback rather than the recursively equivalent `ExitProcess`. The bootstrap never publishes a raw dynamically resolved OS export as a callable original. Live exception-raising primitives (`RaiseException`, `RtlRaiseException`, `RtlRaiseStatus`, and `Nt`/`ZwRaiseException`) remain byte-identical to avoid a process-wide patch race; VEH plus application-import/dynamic interception retain diagnostic coverage. CDB sessions `20260716_023403`/`20260716_023432` proved the old ordering could route `OutputDebugStringA` through a suppressed exception export and fast-fail; all seven dumps in supplied session `20260716_025345` proved the old Rtl fallback recursively re-entered normal shutdown until `0xC00000FD`.

## Working Guidance
- For DX12 games, prefer bootstrap-aware injection over eager process-start injection.
- Base injection and hook-init decisions on observed runtime state such as module load, device creation, and shared runtime flags instead of executable names.
- Keep the pseudo-overlay and injected overlay handoff explicit. The current tree already has runtime flags for this; extending those flags is safer than adding guesswork in UI code.
- Keep dejection cooperative and resident. Do not reintroduce remote `FreeLibrary`, self-unload timeouts, or freeing hook trampolines/mappings that foreign hooks or in-flight detours can still reach.
- Keep reactivation target-specific and generation-safe: reset/consume the current wakeup before the reconnect attempt, validate discovery and whitelist data with bounded reads, and retain old-generation address space until process exit.
- Keep recording/session state separate from the active video producer. New capture methods must not disable injection features, and screen-grab paths must not leave unused inject texture publication active.
- Log each bootstrap phase with enough context to reconstruct failures later: wait-loop start, D3D12 detection, fallback path, wait-loop exit, hook-init deferral, and overlay handoff state.
- For capture-method/injection separation diagnostics, look for `[Inject] Video method ... keeps injection active` and `[Media] Inject video publication enabled|disabled (...)`.
- When changing pending-injection scan logic, keep lock ownership explicit. Avoid calling public query helpers from code that already holds `injectMutex`; add or use locked helpers and policy tests instead.
- Preserve the one-call CFG registration bootstrap boundary when changing trampoline allocation or API resolution. The host CFG policy and the hook DLL's own CFG instrumentation must remain enabled; never solve a bootstrap failure by disabling x64 test-app CFG or marking whole trampoline pages valid.
- Preserve static-import fallbacks, publish every callable trampoline before activating its inline patch, and only then patch application-module imports to its hook. Do not patch Windows-directory module imports, translate the Rtl exit fallback into `ExitProcess`, cache dynamically resolved Kernel32/ntdll exports for guarded indirect calls, or inline-patch live exception-dispatch primitives during process-wide bootstrap.
- Do not treat the current polling sleeps as permission to add more timing bandaids. Existing polling is part of bootstrap orchestration; it is not a general-purpose fix for overlay or FG correctness bugs.

## Open Questions / Stale-Risk
- Stale risk is medium because injection timing, resident lifecycle, and hook bootstrap are coupled to runtime behavior and can drift when wrapper or startup logic changes.
- Vulkan capture after a truly late host activation can require swapchain/device recreation if the already-created device did not enable CE's external-memory extensions. Overlay/pass-through activation does not have that restriction.
- Re-check this page after changes to `InjectionManager`, lifecycle event names/order, IPC mapping ownership, runtime handoff flags, or graphics hook bootstrap conditions.
