# DX12 Injection Bootstrap

Last cross-checked: 2026-08-17

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
- The startup scan also discovers already-running whitelisted processes. It queues them through the same graphics-probe injection path, so starting CaptureEngine after a DirectX/OpenGL title is supported without changing the established game-start path.
- **Vulkan late injection depends entirely on the implicit-layer registration being resident, because it cannot be repaired in-process.** The Vulkan loader composes a process's layer chain exactly once, inside `vkCreateInstance`, from `SOFTWARE\Khronos\Vulkan\ImplicitLayers` as it reads at that moment. CE's whole Vulkan present/overlay path lives in `VK_LAYER_CE_overlay.dll`, not in the injected hook DLL, so a title that started without the layer in its chain can never gain an overlay later no matter how the hook is injected. `captureengine/main_vulkan_residency.h` therefore registers at controller startup and **never unregisters**; there is deliberately no destructor, no `Unregister()`, and no `ApplyRegistrationPlan(plan, false)` on any controller teardown path (`tests/test_vulkan_layer_registration.cpp` asserts all of that). Until 0.1.6156 this was an `ScopedVulkanRegistration` RAII that unregistered on exit, which made Vulkan late injection structurally impossible: session `logs/20260818_224257` (Strange Brigade Vulkan started before CE) contains no `vulkan_layer*.log` at all because the layer DLL was never loaded, `vulkanLayerActive` never got set, and the hook fell through to the D3D path with no overlay.
- **Discovery compatibility is judged on the compiled layout, never on build identity.** Residency makes the Vulkan layer the one CE component that is routinely a *different build* from the host that later wakes it, so `ValidateDiscoveryInfo` checks `DiscoveryInfo::abiSignature == SHARED_MEMORY_ABI_SIGNATURE`; `buildNumber` is diagnostics only. Until 0.1.6162 it required exact build equality, which stranded every resident layer as soon as CaptureEngine was rebuilt or updated while a Vulkan title was running: the layer could not read the whitelist, could not reach the host, and could not even resolve `logsPath` to say why, so the session contained no `vulkan_layer*.log` at all and looked identical to "the layer was never loaded" (session `logs/20260818_231619`, reproduced deliberately with host 6157 against a resident 6156 layer). The first 16 bytes of `DiscoveryInfo` (`injectPid`/`magic`/`buildNumber`/`abiSignature`) are a cross-build contract and their offsets are asserted; nothing past them may be parsed until the signature matches. `ComputeSharedMemoryAbiSignature` therefore also covers `sizeof(DiscoveryInfo)` and the `processWhitelist`/`logsPath` offsets. **Any semantic change to what these shared fields mean must bump `SHARED_MEMORY_VERSION`** — which renames every mapping and event — because the build number no longer keeps incompatible builds apart.
- A resident layer that really is incompatible reports itself through `LayerReportIncompatibleDiscovery`, which writes one line to `<layer dll dir>\logs\vulkan_layer_incompatible.log` naming both builds and both layout signatures. It cannot use any normal log path, because all of them sit behind a discovery mapping it must not parse; without this the only symptom is a missing overlay and no output anywhere.
- Residency is safe because the layer is inert without a host: `PerformEarlyWhitelistCheck` finds no discovery mapping, the layer reports itself not whitelisted, every entry point stays in passthrough, and `IsLayerDebugLoggingEnabled()` is false so it writes nothing. Passthrough still registers instance/device/queue/surface/swapchain metadata (with `runtimeInitialized=false`), which is what lets a later wake late-initialize the overlay from `Capture_vkQueuePresentKHR`. `DISABLE_CE_VULKAN_LAYER=1` disables it per app and `layer_register.exe --unregister` removes it entirely. This matches how Steam, OBS, RTSS, and EOS register their own implicit layers.
- `RepairOwnedRegistrations` prunes only *superseded* CE entries (a previous install directory, the wrong registry view, a manifest no longer on disk) and retains the entries this instance is about to rewrite, so the live registration is never momentarily absent for a title starting right now. `SelectStaleOwnedEntries` is the pure policy behind it and matches CE manifest file names only — a foreign implicit layer in the same key is never eligible for deletion.
- **`CheckAndInstallHooks`'s Vulkan decision must stay re-openable on layer ownership.** On late injection the resident layer only wakes after CE signals its per-PID reactivation event, which normally lands *after* the hook thread has already weighed D3D evidence. A process such as Strange Brigade Vulkan has `d3d12.dll` loaded, so the first evaluation latches "not Vulkan"; if that latch were permanent the DXGI present/resize path would keep doing CE work for the rest of the process while the layer owns presentation. The gate is therefore `!s_checkedForVulkan || s_vulkanActive || vulkanLayerOwned`. Only ownership re-opens it — re-opening on plain `vulkan-1.dll` presence would restore the RoboCop DX12 regression the latch exists to prevent.
- The delayed-injection thread polls for graphics API readiness and tries to detect `d3d12.dll` with `EnumProcessModules()`.
- If `d3d12.dll` is detected, the current logic waits for a short additional settle window before attempting injection.
- If module enumeration fails, the injector logs the failure and falls back to conservative non-D3D12 timing instead of aborting the target entirely.
- Pending startup-scan injection is processed while `InjectionManager::Update()` owns the injection mutex. Any already-injected / recently-failed checks from that loop must use the locked variants instead of public helpers that re-lock the same mutex. `ShouldLaunchPendingInjection(whitelisted, alreadyInjected, recentlyFailed)` captures the intended gate: launch only for a live whitelisted process that is not already injected and not in the recent-failure window. Healthy logs include `Launching deferred injection thread for ...` shortly after the startup scan sees a whitelisted target.
- Host generations use a global host-stopping event plus target-PID-specific DirectX/OpenGL and Vulkan reactivation events. CaptureEngine retains each target-event handle while injection is pending/active, so a signal sent before `LoadLibrary` completes cannot disappear. The resident side consumes an old signal before attempting discovery/IPC reconnection; a newer host signal arriving during that attempt therefore remains set for the next attempt.
- Closing CaptureEngine performs a cooperative **resident deject**, not a remote `FreeLibrary`. The hook/layer first enters dormant pass-through, disables capture/overrides, releases host-bound API capture resources, clears its old source publication, and acknowledges dormancy. Vulkan retains only the dispatch, queue, and swapchain metadata required for correct forwarding and later reactivation. The image, COM wrappers, trampolines, and saved foreign-chain targets remain mapped until process exit because forcibly unloading them cannot be made safe while game or third-party code may retain those addresses. A later CaptureEngine process can reactivate the same resident image.
- The Vulkan layer pins its own module before starting its process-lifetime host watcher. That watcher and its proc-address entry points must never execute from an image that a loader reference-count transition can unmap.
- IPC reconnection atomically publishes the new generation's shared mapping. Prior generation views/handles are deliberately retained until target exit so an already-entered detour cannot dereference unmapped memory while shutdown races it. This is bounded by host restarts within one game process, not a per-frame allocation.
- DirectX 8/9/11/12, DDraw, OpenGL, DXGI wrappers, and Vulkan Present entry points all check the dormant/shutdown state before CE work and forward to the exact saved predecessor. API `OnHostDisconnect` paths serialize teardown of host-owned capture transports with their capture mutexes; OpenGL defers context-owned deletion to the owning GL context.
- **Process exit latches the same dormant state.** `DllMain(DLL_PROCESS_DETACH, lpReserved != NULL)` and the `atexit` handler registered at attach both call `RequestHookShutdown()` (a lock-free store, loader-lock safe) in addition to `g_ProcessTerminating`. Either one runs before this module's globals are destroyed - `atexit` is LIFO and registers after the namespace-scope constructors - so no guarded entry point can read CE state that teardown has already released. Until 0.1.6143 only `g_ProcessTerminating` was set here, and `HookIsShuttingDown()` stayed false for the whole of `LdrShutdownProcess`: hooks that other modules' detach routines call (version.dll opens resource-only images with `LoadLibraryExW`, foreign overlays probe modules from their own detach paths) still ran full CE work. Wukong `20260817_052857` faulted that way in `GetRedirectedPath` on the already-destroyed `AppConfig`.
- The hook's `AppConfig` (`g_pLocalConfig`) is constructed once into module storage by `EnsureLocalConfigAllocated` and **never destroyed**. CE's hooks stay callable until the process dies and the image is pinned, so anything CE owns for the process lifetime must outlive static destruction rather than rely on destructor ordering; the OS reclaims it when the image is unmapped.
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
  `CreateFeature` hook). Two hazards follow, both fixed (builds 0.1.5921 +
  0.1.5922):
  1. The dedicated overlay queue must stay disabled for NVIDIA DLSS FG in the
     planner-only state: a backbuffer-drawing submit on it returns
     `DXGI_ERROR_ACCESS_DENIED (0x887A002B)` and removes the device (session
     `logs/20260811_214252`). `ShouldDisableDedicatedOverlayQueueForNvidiaFrameGeneration`
     covers both detection states and the submit sites reserve the dedicated
     queue for pure-offscreen lists (`ShouldUseDedicatedQueueForOverlaySubmit`).
  2. Queue routing must send the overlay's backbuffer draws to the
     swapchain-owning queue (`origGame`), not to the DLSS-G render queue that
     `g_CommandQueue` flips to at FG resume. Session `logs/20260811_221202`
     (build 0.1.5921) still crashed because the generic routing fallback
     (`scQueue ?: last ECL queue`) picked the render queue. Fix:
     `DecideSwapchainOverlayRouting` treats planner-classified DLSS FG
     (`IsDLSSFrameGenerationActive()`) exactly like the Streamline latch and
     routes pure DLSS to `kUseStreamlineOriginalQueue`.
  3. The FG multiplier report needs the Streamline feature exports hooked.
     Talos conveys 4x MFG exclusively via `slDLSSGSetOptions(numFramesToGenerate=3)`
     (no CreateFeature parameter carries it; `MultiFrameCount`/
     `FrameGenerationMultiplier` reads on the NVNGX params cover only games
     that set them). Under late inject the game resolved the feature function
     before injection and never re-resolves, so
     `ScanLoadedStreamlineModules()` must proactively run
     `TryResolveDLSSGFeatureHooks()`/`TryResolveReflexFeatureHooks()` after the
     loaded-module scan (build 0.1.5925) to inline-hook the cached
     slDLSSGSetOptions pointer; the next FG resume then flows through CE and
     the overlay reports the real multiplier (session `logs/20260811_230524`).
  At FG resume the warm overlay therefore keeps drawing on `origGame` with no
  reinit and no blank, matching the healthy startup sessions.
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
