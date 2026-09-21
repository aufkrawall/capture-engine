# DX12 Injection Bootstrap

Last cross-checked: 2026-09-14

Primary sources:
- `captureengine/injection.cpp`
- `captureengine/injection_manager.cpp`
- `captureengine/injection_wmi_events.cpp`
- `captureengine/injection_inject.cpp`
- `captureengine/injection_policy.h`
- `captureengine/inject_main.cpp`
- `captureengine/inject_config_publication.cpp`
- `captureengine/inject_lifecycle.cpp`
- `captureengine/media_main.cpp`
- `captureengine/pseudo_overlay.cpp`
- `common/inject_overlay_policy.cpp`
- `common/shared_defs.h`
- `hook/main.cpp`
- `hook/main_hookthread.cpp`
- `hook/main_install.cpp`
- `hook/main_host_lifecycle.cpp`
- `hook/common/ipc_client.cpp`
- `hook/vulkan_layer/layer_ipc.cpp`
- `hook/apis/dx12_sampler_hooks.cpp`
- `hook/apis/dx12_device_creation_report.cpp`
- `hook/apis/dx12_hook_hook_install.cpp`
- `hook/apis/streamline_inline_hook_batch.{h,cpp}`
- `hook/common/d3d12_device_creation_policy.h`
- `hook/wrappers/wrapper_hooks.cpp`
- `hook/wrappers/iat_hook.cpp`
- `hook/wrappers/iat_import_table.h`
- `hook/wrappers/inline_hook.cpp`
- `hook/wrappers/inline_hook_deep.cpp`
- `hook/wrappers/inline_hook_pristine_image.h`
- `hook/wrappers/inline_hook_batch.cpp`
- `tests/test_d3d12_device_creation_policy.cpp`
- `tests/test_crash_handler.cpp`
- `tests/test_shared_runtime_state.cpp`
- `tests/test_capture_coordinator_source.cpp`
- `tests/test_inject_capture_source.cpp`
- `tests/test_inject_capture_source_part2.cpp`
- `tests/test_process_ipc.cpp`

## Scope
This page describes how DX12 injection and overlay bootstrap currently work, with emphasis on how to make inject and overlay behavior work optimally for DX12 games without turning the wiki into a substitute for the code.

## Facts
- Host-side injection currently uses a delayed-injection thread instead of injecting blindly at process start.
- The startup scan also discovers already-running whitelisted processes. It queues them through the same graphics-probe injection path, so starting CaptureEngine after a DirectX/OpenGL title is supported without changing the established game-start path.
- **Process monitoring begins only after the pre-injection target callback exists.** The
  `InjectionManager` constructor resolves its two DLL paths but does not subscribe or scan.
  `inject_main` installs the callback that publishes the exact profile target and resolved config,
  then calls `StartMonitoring`; a discovery callback can therefore never race ahead of target
  publication. The direct suspended-launch helper intentionally constructs a manager without
  monitoring because it invokes `InjectEarly` for one already-known PID.
- **Process-start tracing is opportunistic; the unelevated fallback is native, not WMI.** WMI first
  requests `Win32_ProcessStartTrace`, which is event-driven and has no intrinsic `WITHIN` sampling
  interval, but subscribing requires Administrators membership. An ordinary CE run receives
  `WBEM_E_ACCESS_DENIED` (`0x80041003`) and falls back. An asynchronous trace failure only queues
  that transition; `Update` performs the handoff because a sink callback must not call back into
  WMI. One atomic subscription state makes synchronous failure, late completion, shutdown, and
  fallback activation mutually exclusive. Duplicate scan/event notifications are coalesced per PID.
  - Until 0.1.6654 the fallback was `__InstanceCreationEvent WITHIN 0.5 WHERE TargetInstance ISA
    'Win32_Process'`. That is **not** a poll inside CE: it asks the WMI service to enumerate and
    fully materialise every `Win32_Process` instance twice a second, with all the per-instance
    properties (command line, owner, paths) CE never reads, and diff them. Session
    `20260918_162809` ran that way for about four hours.
  - It is now `ce::process_start::Poller` (`captureengine/process_start_poll.{h,cpp}`): one
    `NtQuerySystemInformation(SystemProcessInformation)` sweep every 250 ms inside CE's own process,
    taking only PID and image name, feeding the same `IsWhitelisted` + `LaunchDelayedInjectionThread`
    path under the `ProcessPoll` source tag. The first sweep only establishes the baseline; the
    existing-process scan owns everything alive at startup. The known-PID set is replaced rather
    than merged each sweep, which bounds it and correctly re-reports a recycled PID.
  - **This was never an injection-latency fix, and the wiki should not claim it was.** In
    `20260918_162809` the WMI fallback notified CE 419 ms after Alan Wake 2 started, and it cost
    nothing: CE's hooks were fully installed by 19:53:39.98 and the game did not create its real
    D3D12 swapchain until 19:53:44.273, ~4.95 s later. The reason to stop asking WMI is the
    machine-wide load, not lateness. **That margin does not generalise:**
    `frame-generation/streamline-generation-bridge.md` documents the opposite case, a 1.x
    Streamline title that reached `slInit` inside the notification window
    (`20260821_151924`, `d3d12=1` on the very first poll). Detection latency is slack for a
    game that takes seconds to reach its first swapchain and decisive for one that does not. `kMinPollIntervalMs`/`kMaxPollIntervalMs` bound the cadence and
    `tests/test_dxgi_shared_part15.cpp` pins both that and the absence of any `WITHIN` query.
  - **One side effect of the faster source, recorded so it is not rediscovered as a bug:** CE now
    enumerates a target's modules early enough to hit `ERROR_PARTIAL_COPY` (299), the result of
    reading a 64-bit process's module list while its PEB is still being built. The 0.5 s WMI latency
    had always hidden it (`20260918_162809` reads `d3d12=1` on its first probe; `20260918_221342`
    reads error=299 and `d3d12=0`). It changes no behaviour - `ShouldInjectAfterGraphicsProbe`
    ignores `d3d12Loaded` and injects immediately regardless - so what it cost was the diagnostic,
    plus a log line promising "conservative non-D3D12 injection timing" for a timing path that does
    not exist. The probe retries the transient error now and the message says what happened.
- **The kernel32 loader/process-creation hooks are installed in `DllMain`, before the graphics IAT
  work.** `InstallKernel32LoaderHooks` (`hook/main_injection.cpp`) runs twice: once from `DllMain`
  ahead of `InitializeWrapperHooks`, and once on the hook thread. The second pass is not redundant -
  IAT patching only reaches import tables that exist when it runs, so modules mapped in between need
  it repeated, and `PatchIATAllModules` is idempotent per slot. The first pass is the one that
  matters for coverage: the loader hook is the cheapest hook CE installs and the one whose value
  decays fastest, because anything mapped before it exists can never be redirected. In
  `20260918_162809` CE's DLL was live at 19:53:39.190 but the loader hooks did not go in until
  19:53:39.520 - 330 ms of DXGI/D3D10/D3D11/D3D12 patching stood in between. DllMain safety is the
  same argument the graphics IAT hooks already rely on: it resolves addresses in an already-loaded
  kernel32 and writes import slots, loading nothing, so it cannot re-enter the loader.
- **Every IAT slot write must hold `g_PatchLock` across VirtualProtect/write/restore, because page
  protection is process-wide state.** `PatchIAT` changes a page to `PAGE_READWRITE`, writes one
  pointer with `InterlockedCompareExchangePointer`, and puts the old protection back. Two of those
  sequences interleaving on the same page destroy each other: A unprotects, B finishes its own patch
  on that page and restores `PAGE_READONLY`, A's CAS then writes into a read-only page and takes
  `0xC0000005`. Until this was fixed the lock was taken *after* the unprotect and covered only the
  `g_PatchedEntries.push_back`; `RestoreIAT` and `ShutdownIATHooks` always held it across all three
  steps. CE really does patch from more than one thread - the hook thread's
  `PatchIATAllModulesFiltered` sweep, and `main_redirect.cpp:PatchLateLoadedCreateProcessImports` on
  whichever game thread mapped a module - and in Strange Brigade session `20260920_224536` both
  reached `steamclient64.dll`'s adjacent `CreateProcessA`/`CreateProcessW` thunks inside the same
  millisecond and killed the game before it drew a frame. The guard has to start *after*
  `TryGetTrackedOriginalForPatchedEntry`, which takes `g_PatchLock` itself: it is a plain
  `std::mutex`, so re-entering it parks the thread forever.
  `tests/test_iat_patch_serialization.cpp` pins the ordering, that the lock is not released inside
  the write window, that the other two writers keep their shape, and that the mutex stays
  non-recursive.
- **Vulkan late injection depends entirely on the implicit-layer registration being resident, because it cannot be repaired in-process.** The Vulkan loader composes a process's layer chain exactly once, inside `vkCreateInstance`, from `SOFTWARE\Khronos\Vulkan\ImplicitLayers` as it reads at that moment. CE's whole Vulkan present/overlay path lives in `VK_LAYER_CE_overlay.dll`, not in the injected hook DLL, so a title that started without the layer in its chain can never gain an overlay later no matter how the hook is injected. `captureengine/main_vulkan_residency.h` therefore registers at controller startup and **never unregisters**; there is deliberately no destructor, no `Unregister()`, and no `ApplyRegistrationPlan(plan, false)` on any controller teardown path (`tests/test_vulkan_layer_registration.cpp` asserts all of that). Until 0.1.6156 this was an `ScopedVulkanRegistration` RAII that unregistered on exit, which made Vulkan late injection structurally impossible: session `logs/20260818_224257` (Strange Brigade Vulkan started before CE) contains no `vulkan_layer*.log` at all because the layer DLL was never loaded, `vulkanLayerActive` never got set, and the hook fell through to the D3D path with no overlay.
- **Discovery compatibility is judged on the compiled layout, never on build identity.** Residency makes the Vulkan layer the one CE component that is routinely a *different build* from the host that later wakes it, so `ValidateDiscoveryInfo` checks `DiscoveryInfo::abiSignature == SHARED_MEMORY_ABI_SIGNATURE`; `buildNumber` is diagnostics only. Until 0.1.6162 it required exact build equality, which stranded every resident layer as soon as CaptureEngine was rebuilt or updated while a Vulkan title was running: the layer could not read the whitelist, could not reach the host, and could not even resolve `logsPath` to say why, so the session contained no `vulkan_layer*.log` at all and looked identical to "the layer was never loaded" (session `logs/20260818_231619`, reproduced deliberately with host 6157 against a resident 6156 layer). The first 16 bytes of `DiscoveryInfo` (`injectPid`/`magic`/`buildNumber`/`abiSignature`) are a cross-build contract and their offsets are asserted; nothing past them may be parsed until the signature matches. `ComputeSharedMemoryAbiSignature` therefore also covers `sizeof(DiscoveryInfo)` and the `processWhitelist`/`logsPath` offsets. **Any semantic change to what these shared fields mean must bump `SHARED_MEMORY_VERSION`** — which renames every mapping and event — because the build number no longer keeps incompatible builds apart.
- **The selected profile target is published before injection.** `DiscoveryInfo::profileTargetPid` is distinct from hook-owned `sourcePid`: the former identifies the exact PID whose profile the host selected in the pre-injection callback, while the latter proves remote `LoadLibrary` completed and the hook connected. A non-whitelisted direct child Vulkan renderer may inherit only when its parent PID equals one of those published identities and the parent executable still exactly matches the discovery whitelist. This closes split-renderer startup without a polling delay or executable-specific bridge rule. The field was added with shared-memory version 46 because the discovery layout is part of the compiled ABI.
- **The inherited-renderer claim is scoped to the process tree that published it, and only that tree stands down.** Once the Vulkan layer proves the current process is the direct child renderer of the profiled client, it publishes `CaptureState::inheritedRendererClaim` (ABI 49): the renderer PID packed together with the client PID the proof was made against. `ce::vulkan_renderer_policy::ShouldApplyProcessLocalRuntimeOverrides` then hands process-local NGX/Streamline work - the `dlss_*_dll_path` / `streamline_dll_path` preload and `LdrLoadDll` redirect, and the `ShowDlssIndicator` registry answer - to that renderer alone, and *only its own client* gives them up. Until ABI 49 the record was the renderer PID by itself, which made it a session-wide claim: only the publisher can clear it, and a renderer terminated without running `LayerIPC_Shutdown` never does, so a dead PID silenced the runtime overrides of every game launched afterwards. Session `logs/20260829_220520`: a stale `NvRemixBridge.exe` claim (Portal RTX, PID 12072, published 22:05:30) made Talos Reawakened skip `dlss_rr_dll_path` at 22:06:25 and load its own bundled `nvngx_dlssd.dll` 310.6 while CE still injected the profile's `dlss_rr_preset=f`; NGX `CreateFeature` for feature 13 then read through a null pointer inside `nvngx_dlssd.dll`. Restarting CaptureEngine recreated the mapping and the identical launch worked (`logs/20260829_221333`, where the preload and indicator both arm). The gate is also evaluated inside the `LdrLoadDll` redirect hook under the loader lock, so it must remain one shared-memory load - process enumeration there is a deadlock hazard, which is why the claim carries the client identity instead of the reader deriving it. The host sensor loop additionally reaps a claim whose renderer is provably absent from a successful process snapshot; that is hygiene for other readers of the field and never the guard.
- **Vulkan ownership and hook-to-layer DLSS FG state obey the same process-tree rule (ABI 53).** `CaptureState::vulkanLayerClaim` packs renderer/client PID; present tick, present thread, and overlay-active publications are tagged with that renderer; `DLSSState::fgPublication` coherently packs publisher PID, active state, and multiplier. Direct renderers use their PID in both claim halves, while an inherited child accepts publication from either exact half. Every DXGI/D3D/OpenGL/watchdog reader checks its own PID against the claim, and the Vulkan layer rejects FG state outside that tree. Session `20260901_174634` supplied the failure pair: Portal RTX's child left global Vulkan/DLSS state behind, Filter Tester displayed stale DLSS FG, then Talos DX12 treated the foreign Vulkan bit as authoritative and bypassed its DXGI hooks. Owner-only teardown and dead-renderer host reaping are cleanup; the scoped read is the correctness guard when a publisher is hung or terminated without shutdown.
- A resident layer that really is incompatible reports itself through `LayerReportIncompatibleDiscovery`, which writes one line to `<layer dll dir>\logs\vulkan_layer_incompatible.log` naming both builds and both layout signatures. It cannot use any normal log path, because all of them sit behind a discovery mapping it must not parse; without this the only symptom is a missing overlay and no output anywhere.
- Residency is safe because the layer is inert without a host: `PerformEarlyWhitelistCheck` finds no discovery mapping, the layer reports itself not whitelisted, every entry point stays in passthrough, and `IsLayerDebugLoggingEnabled()` is false so it writes nothing. Passthrough still registers instance/device/queue/surface/swapchain metadata (with `runtimeInitialized=false`), which is what lets a later wake late-initialize the overlay from `Capture_vkQueuePresentKHR`. `DISABLE_CE_VULKAN_LAYER=1` disables it per app and `layer_register.exe --unregister` removes it entirely. This matches how Steam, OBS, RTSS, and EOS register their own implicit layers.
- **Because it is implicit, the layer may never fail a call on account of its own bookkeeping - inertness is not a property of the whitelist check, it is a property of every entry point.** Until 0.1.6177 `Capture_vkCreateDevice` resolved the owning `VkInstance` *before* consulting `g_LayerState.whitelisted` and returned `VK_ERROR_INITIALIZATION_FAILED` when the lookup missed, so a dormant layer broke `vkCreateDevice` for every device-group application on the machine with CaptureEngine closed (Red Dead Redemption 2, session `logs/rdr2crash`; reproduced standalone). Ownership now resolves through `hook/vulkan_layer/vulkan_instance_registry.h` - enumerated handle, then the loader dispatch key (`*(void**)handle`, the same value on a `VkInstance` and all its `VkPhysicalDevice`s, and on a `VkDevice` and all its `VkQueue`s), then the sole live instance - and an unresolvable instance degrades to handing the application's own `VkDeviceCreateInfo` to the next link, never to an error. **Both `vkEnumeratePhysicalDevices` and `vkEnumeratePhysicalDeviceGroups`(`KHR`) must stay wired into `vkGetInstanceProcAddr`**: a Vulkan 1.1 multi-GPU-aware engine may use only the group entry point, and CE never sees the classic one. `tests/test_vulkan_instance_registry.cpp` holds the regression and the source-policy guard.
- `RepairOwnedRegistrations` prunes only *superseded* CE entries (a previous install directory, the wrong registry view, a manifest no longer on disk) and retains the entries this instance is about to rewrite, so the live registration is never momentarily absent for a title starting right now. `SelectStaleOwnedEntries` is the pure policy behind it and matches CE manifest file names only — a foreign implicit layer in the same key is never eligible for deletion.
- **Vulkan layer registration is decoupled from the installation tree via runtime staging.** Registering `installed/captureengine/` directly caused external non-whitelisted processes (Explorer, Chrome, Discord, `DataExchangeHost.exe`, etc.) to map `VK_LAYER_CE_overlay.dll` into their address space whenever any Vulkan enumeration occurred. This held file locks preventing replacing or rebuilding `installed/captureengine/` binaries even when CaptureEngine was closed. `BuildRegistrationPlan` and `ApplyRegistrationPlan` (`common/vulkan_layer_registration.{h,cpp}`) stage layer manifests and DLLs into `%LOCALAPPDATA%\CaptureEngine\vulkan_layers\b<build_number>\` (or `%PROGRAMDATA%` when elevated), registering the staged manifest paths in the registry. External processes lock only the staged copy in AppData, leaving `installed/captureengine/` completely unlocked. `CleanupStaleStagingDirectories` automatically prunes older `b*` staging folders during startup, gracefully retaining any directory whose DLL is still locked by a surviving external process.
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
- **An Agility factory request is early D3D12-use evidence, not renderer ownership.** The signal
  latches when the application calls `ID3D12SDKConfiguration1::CreateDeviceFactory`, before that
  runtime call begins, or when an application-routed `D3D12GetInterface` returns a device factory
  directly. Merely loading `d3d12.dll` or querying SDK/debug/tool interfaces does not qualify.
  `CheckAndInstallHooks` uses the evidence to suppress only synthetic D3D9, DX8, and OpenGL
  bootstrap while D3D12 starts. The ordinary device/queue/swapchain observations still own renderer
  selection. The factory is intercepted before it reaches the application, and
  `ID3D12DeviceFactory::CreateDevice` marks definitive device evidence before any device-vtable
  work. This prevents unrelated OpenGL entry patches from quiescing the renderer during a late
  D3D12/FSR startup without making plain runtime presence authoritative. This is still a useful
  optimization, but build 0.1.6515's `20260909_063715` second launch proved it was not sufficient:
  CE's remaining synthetic hardware bootstrap and other process-wide hook transactions still
  overlapped D3D12/FFX startup.
- **The temporary Present/ECL discovery stack is software-only and thread-local.**
  `DX12_InstallHooksViaTempSwapchain` obtains WARP with `IDXGIFactory4::EnumWarpAdapter`, passes that
  adapter to `D3D12CreateDevice`, and has no hardware-adapter fallback. It bypasses existing foreign
  factory/device entry jumps; inability to construct the bypass rejects only this synthetic route.
  The thread-local internal-probe scope excludes its device/factory/swapchain callbacks from
  application D3D12 evidence, device sampler hooks, Present-hook recursion, and queue/swapchain
  tracking. The removed process-global flag could hide a real game swapchain created concurrently.
  A standalone same-process probe found identical ECL, Present, and Present1 method addresses on
  WARP and hardware, so hook discovery does not need a vendor UMD device.
- **A thread quiescence walks this process, not the machine.** `ThreadQuiescence` used to enumerate
  peers with `CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)`. That flag ignores the process id it is
  given and always snapshots **every thread on the system**, so the cost tracked total system load
  rather than the game, and every peer thread stayed suspended for the whole of it. Measured per
  un-batched entry patch in Gothic II: 37-533 ms, 279-1249 ms per launch, a 5x spread between runs
  doing identical work, worst right after a boot when system thread count is highest
  (`installed/captureengine/logs/20260916_*`). The walk now goes through
  `ce::process_threads::WalkCurrentProcessThreads` (`hook/common/process_thread_walk.{h,cpp}`), which
  uses `NtGetNextThread` and is proportional to this process alone; the system snapshot remains the
  fallback and the tests' equivalence oracle. Its visitor is a two-pointer callable view, never
  `std::function`, because a second pass runs with peers already suspended and must not touch the
  heap. A quiescence that still takes >=8 ms logs `ThreadQuiescence: ... peer thread(s) suspended
  for ...`, bounded, with the route that produced it - previously that stall was only visible as an
  unexplained gap between the trampoline write and the entry patch.
- **The temp-swapchain Present-hook bootstrap refuses legacy-presentation processes.** Its only
  precondition used to be that `dxgi.dll` and `d3d12.dll` are *loaded*, which Windows arranges in
  plenty of processes transitively and which says nothing about presenting through either. Gothic II
  is a DirectDraw7 title logging `dx12Used=0` and still paid 650-1430 ms per launch - bimodal, so a
  variable startup stall - for a throwaway WARP device, command queue, window and swapchain it could
  never use. `ShouldSkipTempSwapchainForLegacyPresentationProcess` now refuses when ddraw/d3d8 are
  mapped with no D3D11/D3D10 module and no D3D11/D3D12 device observed. `d3d9.dll` is deliberately
  **not** a discriminator (modern DXGI titles map it transitively); a DirectDraw-to-DXGI wrapper
  (dgVoodoo2, DXVK) maps `d3d11.dll`, so those keep the bootstrap. The refusal is checked before the
  guarded route spends one of its 120 bounded attempts, and is re-evaluated every service pass.
- **Related inline entry patches share one short quiescence.** `InstallPublishedBatch` performs
  instruction decoding, trampoline allocation, RX/CFG finalization, logging, and callable-original
  publication while peers run. It then takes one stable thread snapshot, exact-range checks every
  candidate, and commits only unchanged original bytes. Unsafe candidates retry through the normal
  per-target transaction after resume. Fatal hooks, OpenGL swap exports, DXGI Present/Present1,
  DLSS registry probes, Streamline core exports per module, and NGX core exports use this path.
  NGX aliases publish every predecessor atomically before the shared entry becomes live. Existing
  foreign E9/FF25 chain rules, retained published trampolines, CFG policy, and independent fallback
  coverage are unchanged.
- **Configured runtime preloads retain their original position and semantics; continuous module
  observation precedes optional diagnostics.** `PreloadConfiguredGraphicsRuntimeDlls` explicitly
  notifies the feature hooks. The `LdrLoadDll` observer is armed immediately afterwards and before
  fatal-dump entry hooks, so FFX/Streamline loads on another thread cannot escape during that slower
  diagnostic phase. Session `20260908_201724` showed the old late-injected path enter its first real
  ECL and load the official FFX module while CE was still patching unrelated OpenGL exports.
- Target-profile resolution is cached by normalized process name inside the serialized publication
  state. Injection detection, hook-source handoff, overlay toggles, and later launches therefore
  reuse the same immutable resolved config instead of repeating the many Win32 INI reads. The
  normal `SetPublicationBaseConfig` startup/reload path clears the cache, so explicit config reload
  remains authoritative.
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
     (no CreateFeature parameter carries it; `DLSSG.MultiFrameCount`/
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
- **Vulkan ownership is also a creation-time pass-through invariant.** NVIDIA's Vulkan ICD implements WSI with private DXGI swapchains. Hooks installed before renderer classification must therefore forward every `CreateSwapChain*` call with the original object identities and descriptors and perform no CE tracking, wrapping, queue hooking, shared-vtable mutation, or recovery. The resident `VK_LAYER_CE_overlay` module suppresses DllMain wrapper setup and the hook thread's speculative early factory hooks before IPC is ready; `DXGIShared::ShouldBypassSwapchainCreateForVulkan` is the fallback for hooks retained from an earlier renderer state. Portal RTX session `20260825_190436` showed the previous violation directly: the call stack ran from Remix through CE `Capture_vkCreateSwapchainKHR`, NVIDIA WSI, CE's global/inline/deep DXGI detours, and finally `InstallPresentInlineHooks`. Module presence here means the CE layer module itself, not plain `vulkan-1.dll`, so the RoboCop evidence rule remains intact. The sole opt-in exception is the `vsync=fifo` synchronization-argument path documented in `graphics-overrides-and-frame-pacing.md`: it observes exact real objects through system method-body hooks, leaves every COM vtable/descriptor unchanged, and runs no overlay, capture, queue, wait, or recovery policy.
- Inline-hook trampolines in a CFG-enabled x64 host begin on a `PAGE_TARGETS_INVALID` page, are built without write/execute overlap, sealed RX with `PAGE_TARGETS_NO_UPDATE`, and register only their aligned entrypoint. MinGW's Kernel32 import library lacks `SetProcessValidCallTargets`, so the exact export is resolved from already-loaded KernelBase/Kernel32 and invoked through one x64 `guard(nocf)` bootstrap wrapper. Do not turn that into a general unchecked-call helper or widen its use: the exception exists only because a normal dynamically resolved call is CFG-checked before it can register the new target. Session `20260716_013421` and a debugger reproduction proved the old indirect call fast-failed with subcode 10 in `InlineHook::FinalizeExecutableTrampoline` before hook initialization.
- Windows export suppression also makes some dynamically resolved system exports invalid guarded indirect-call targets even when their address is genuine. `IATHook::DetourGetProcAddress` therefore reaches the real API through the hook DLL's deliberately unpatched static `GetProcAddress` import, never a cached self-resolved function pointer. Supplied session `20260716_021732` and CDB sessions `20260716_022257`/`20260716_022313` put the resulting `FAST_FAIL_GUARD_ICALL_CHECK_FAILURE` in the old detour call while NVIDIA and Vulkan components initialized.
- **A missing `OriginalFirstThunk` means the loaded IAT contains resolved addresses, not name RVAs.** Old-linker modules are matched by exact resolved export address; an entry that already points at CE is accepted only through CE's tracked ownership record, while an unidentified foreign replacement is preserved. Normal name tables, descriptor/name RVAs, strings, and thunk walks are bounded to the loaded image and checked for readable memory before use. Gothic II/SystemPack sessions `20260914_151113` and `20260914_151846` exposed the old failure: `Shw32.dll` has `OriginalFirstThunk=0`, its first Kernel32 IAT value was the resolved `LoadLibraryA` address `0x766E1F70`, and CE added the module base `0x71D40000` then passed `0xE8421F72` to `strcmp`, crashing before a window existed. `IATHookImportTableTest` reconstructs that exact name-less layout and the malformed/foreign/tracked directions.
- **Pristine disk code is not executable at an arbitrary loaded image base until PE relocations are applied.** `ReadRelocatedImageBytes` maps bounded file RVAs and applies only the overlapping x86 `HIGHLOW` or x64 `DIR64` entries before deep/bypass resume comparison, decoding and copying; a moved image with missing, malformed or unsupported overlapping relocation data is refused. Gothic II follow-up `20260914_154220` proved the old failure exactly: the guarded system-DXGI bootstrap copied `A1 C0 86 0D 10` from disk into its x86 `CreateDXGIFactory1` bypass, then crashed at that trampoline reading preferred-base address `0x100D86C0`; the live operand was correctly relocated to `0x642E86C0` and the DLL relocation table names its RVA `0x38549` as `HIGHLOW`. Applied-relocation counts are logged so this path is diagnosable without another disassembly.
- Fatal-dump hook bootstrap is transactional with respect to callable originals: each surviving termination/fail-fast trampoline is atomically published before its inline target patch becomes live and before any IAT route can reach it. Fatal IAT patching is limited to application modules; modules anywhere under the Windows directory retain their original imports so forwarded system implementations cannot recurse through CE. Fallback calls use the hook DLL's own unpatched static Kernel32/ntdll imports, including a direct static `RtlExitUserProcess` fallback rather than the recursively equivalent `ExitProcess`. The bootstrap never publishes a raw dynamically resolved OS export as a callable original. Live exception-raising primitives (`RaiseException`, `RtlRaiseException`, `RtlRaiseStatus`, and `Nt`/`ZwRaiseException`) remain byte-identical to avoid a process-wide patch race; VEH plus application-import/dynamic interception retain diagnostic coverage. CDB sessions `20260716_023403`/`20260716_023432` proved the old ordering could route `OutputDebugStringA` through a suppressed exception export and fast-fail; all seven dumps in supplied session `20260716_025345` proved the old Rtl fallback recursively re-entered normal shutdown until `0xC00000FD`.
- **Hardware device-creation forensics are separate from the synthetic bootstrap.** The retained
  `ReportDeviceCreationFailure` helper can gather entry bytes/owners, Agility provenance, foreign
  modules, a null-output adapter/feature-level matrix, foreign-entry bypass comparison, and a
  conditional D3D11 cross-check without creating one hardware device per matrix cell. The WARP temp
  route deliberately never invokes that report because its hardware matrix would re-enter vendor
  initialization during the window this fix isolates.
- **Terminal WARP bootstrap failures are retried three times, then abandoned.**
  `DXGI_ERROR_UNSUPPORTED`, `DXGI_ERROR_SDK_COMPONENT_MISSING`, `E_NOINTERFACE`, and `E_INVALIDARG`
  describe a stable result. Transient failures such as `DXGI_ERROR_DEVICE_REMOVED` remain eligible
  for later recovery. There is no hardware fallback after the WARP budget.

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
- When an explicit hardware-failure call site emits a `DX12 device-creation report:` block, use its
  entry-owner and adapter evidence before attributing the failure. Absence of that optional report is
  not evidence that the WARP bootstrap attempted hardware creation.
- Do not re-add an unbounded retry around device creation. If a new failure mode genuinely needs retrying, classify its HRESULT in `IsTerminalCreationFailure` instead of widening the budget.
- The WARP bootstrap must not become *application evidence* (`MarkD3D12DeviceCreated`, queue/swapchain
  tracking, device-creation report), but it must claim the shared D3D12Core vtables. All
  `ID3D12Device` and `ID3D12CommandQueue` objects in a process share one vtable each, so
  `DX12_HookQueueVTable(pQueue)` and `DX12_HookDeviceVTable(pDevice)` on the bootstrap objects cover
  the game's already-created ones. That is the only point early enough for the sampler/root-signature
  overrides: those objects are immutable once created, and a game that resolved `D3D12CreateDevice`
  before CE attached (the normal case - the injector waits for `d3d12.dll` to be present, and
  `patchResult=0` because nothing imports the export statically) has already built them by the time
  CE discovers its device from a command queue.
- CE may add a swapchain *creation* flag only where it also rewrites the application's later calls
  that DXGI validates against it. `backbuffer_count` adds
  `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT`, and
  `dxgi!CDXGISwapChain::ValidateResizeBuffers` fails with `E_INVALIDARG` on any disagreement in that
  bit. The rule lives in `hook/common/swapchain_flag_policy.h`; the reconciliation must read the live
  `GetDesc().Flags`, never the current config. See `llm-wiki/log/recent.md` (2026-09-21).

## Open Questions / Stale-Risk
- Stale risk is medium because injection timing, resident lifecycle, and hook bootstrap are coupled to runtime behavior and can drift when wrapper or startup logic changes.
- Vulkan capture after a truly late host activation can require swapchain/device recreation if the already-created device did not enable CE's external-memory extensions. Overlay/pass-through activation does not have that restriction.
- Re-check this page after changes to `InjectionManager`, lifecycle event names/order, IPC mapping ownership, runtime handoff flags, or graphics hook bootstrap conditions.
