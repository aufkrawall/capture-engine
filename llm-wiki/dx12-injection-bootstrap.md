# DX12 Injection Bootstrap

Last cross-checked: 2026-08-09

Primary sources:
- `captureengine/injection.cpp`
- `captureengine/injection_policy.h`
- `captureengine/inject_main.cpp`
- `captureengine/media_main.cpp`
- `captureengine/pseudo_overlay.cpp`
- `common/inject_overlay_policy.cpp`
- `common/shared_defs.h`
- `hook/main.cpp`
- `hook/wrappers/inline_hook.cpp`
- `tests/test_crash_handler.cpp`
- `tests/test_shared_runtime_state.cpp`
- `tests/test_capture_coordinator_source.cpp`

## Scope
This page describes how DX12 injection and overlay bootstrap currently work, with emphasis on how to make inject and overlay behavior work optimally for DX12 games without turning the wiki into a substitute for the code.

## Facts
- Host-side injection currently uses a delayed-injection thread instead of injecting blindly at process start.
- The delayed-injection thread polls for graphics API readiness and tries to detect `d3d12.dll` with `EnumProcessModules()`.
- If `d3d12.dll` is detected, the current logic waits for a short additional settle window before attempting injection.
- If module enumeration fails, the injector logs the failure and falls back to conservative non-D3D12 timing instead of aborting the target entirely.
- Pending startup-scan injection is processed while `InjectionManager::Update()` owns the injection mutex. Any already-injected / recently-failed checks from that loop must use the locked variants instead of public helpers that re-lock the same mutex. `ShouldLaunchPendingInjection(whitelisted, alreadyInjected, recentlyFailed)` captures the intended gate: launch only for a live whitelisted process that is not already injected and not in the recent-failure window. Healthy logs include `Launching deferred injection thread for ...` shortly after the startup scan sees a whitelisted target.
- `inject_main.cpp` sets shared runtime flags so the controller-side pseudo-overlay suppresses itself while the injected overlay handoff is pending or active.
- Fresh application routing comes from named `[Profile.*]` sections. `video_capture=inject` implies the injector's full video/overlay/graphics route without a second setting; WGC, DXGI, and no-video sources normally remain non-injected. Optional `dll_injection=always` selects overlay/graphics-only injection for those non-injected sources, while `never` blocks injection and `when_needed` remains accepted as the redundant default spelling. Legacy `injection_mode`, `injection`, `whitelist`, and `overlay_whitelist` remain parser inputs with their historical behavior.
- Hook-side video publication is a separate runtime decision. The raw `captureRequested` bit remains the recording/session signal used by REC UI and capture-synced policy, while `kCaptureRuntimeFlagInjectVideoCaptureRequested` is set only while the active video path consumes injected frames. The injector clears this path flag before publishing every new start request and again on stop, so an interrupted prior media session cannot leak stale inject publication into a screen-grab start. WGC/DXGI recordings therefore keep injection features without paying for unused injected texture copies.
- With global `capture_method=auto`, an explicit `video_capture=inherit` profile normally resolves to inject and resolves to WGC when `dll_injection=never` is set. Explicit per-profile WGC/DXGI/none routes override that global choice. New DLL-only profiles that omit `video_capture` have no video route; compatibility injection keys retain their historical implicit inherited route. During a live inject-to-WGC fallback, inject publication remains enabled until WGC first-frame proof; the media coordinator clears the inject-video flag only after committing the WGC path and before stopping the inject capture pipeline.
- In current wrapper builds, DX12 hook bootstrap is for state tracking and `ExecuteCommandLists` tracking. Present and `ResizeBuffers` interception comes from wrappers rather than DXGI vtable hooks.
- In wrapper builds, DX12 hook init is deferred until real D3D12 device creation is observed. In no-wrapper builds, the hook instance is initialized more eagerly so late injection does not miss the recovery path.
- **The DXGI present/resize "Vulkan pass-through" decision is evidence-based, not module-presence-based.** `hook/common/vulkan_renderer_policy.h` owns the decision: `HasD3DUsageEvidence` (D3D12/11 device creation, d3d12.dll/d3d11.dll presence, legacy D3D modules; under DXVK only a real D3D12 device counts) and `ShouldTreatVulkanAsActiveRenderer` (Vulkan layer ownership, or vulkan-1.dll loaded without D3D evidence). `CheckAndInstallHooks` publishes the result through `DXGIShared::SetVulkanActiveForDXGIPresentPath`, and `DXGIShared::IsVulkanActive()` (consulted by the DX12 present routing, Present1, and ResizeBuffers) reads that flag. A DX12 UE5 title that merely loads vulkan-1.dll as a transitive dependency (RoboCop: Rogue City, session `logs/20260809_134642`) must keep full DXGI processing and the overlay; the old one-shot `GetModuleHandleW(L"vulkan-1.dll")` latch permanently bypassed `HandleDX12ProcessFrame`, so the original game queue was never captured and PostSL activation never completed. The inner guards in `DX12Hook::Init` / `DX11Hook::Init` use the same flag, so late-loaded vulkan-1.dll cannot suppress hook installation in a D3D process.
- Inline-hook trampolines in a CFG-enabled x64 host begin on a `PAGE_TARGETS_INVALID` page, are built without write/execute overlap, sealed RX with `PAGE_TARGETS_NO_UPDATE`, and register only their aligned entrypoint. MinGW's Kernel32 import library lacks `SetProcessValidCallTargets`, so the exact export is resolved from already-loaded KernelBase/Kernel32 and invoked through one x64 `guard(nocf)` bootstrap wrapper. Do not turn that into a general unchecked-call helper or widen its use: the exception exists only because a normal dynamically resolved call is CFG-checked before it can register the new target. Session `20260716_013421` and a debugger reproduction proved the old indirect call fast-failed with subcode 10 in `InlineHook::FinalizeExecutableTrampoline` before hook initialization.
- Windows export suppression also makes some dynamically resolved system exports invalid guarded indirect-call targets even when their address is genuine. `IATHook::DetourGetProcAddress` therefore reaches the real API through the hook DLL's deliberately unpatched static `GetProcAddress` import, never a cached self-resolved function pointer. Supplied session `20260716_021732` and CDB sessions `20260716_022257`/`20260716_022313` put the resulting `FAST_FAIL_GUARD_ICALL_CHECK_FAILURE` in the old detour call while NVIDIA and Vulkan components initialized.
- Fatal-dump hook bootstrap is transactional with respect to callable originals: each surviving termination/fail-fast trampoline is atomically published before its inline target patch becomes live and before any IAT route can reach it. Fatal IAT patching is limited to application modules; modules anywhere under the Windows directory retain their original imports so forwarded system implementations cannot recurse through CE. Fallback calls use the hook DLL's own unpatched static Kernel32/ntdll imports, including a direct static `RtlExitUserProcess` fallback rather than the recursively equivalent `ExitProcess`. The bootstrap never publishes a raw dynamically resolved OS export as a callable original. Live exception-raising primitives (`RaiseException`, `RtlRaiseException`, `RtlRaiseStatus`, and `Nt`/`ZwRaiseException`) remain byte-identical to avoid a process-wide patch race; VEH plus application-import/dynamic interception retain diagnostic coverage. CDB sessions `20260716_023403`/`20260716_023432` proved the old ordering could route `OutputDebugStringA` through a suppressed exception export and fast-fail; all seven dumps in supplied session `20260716_025345` proved the old Rtl fallback recursively re-entered normal shutdown until `0xC00000FD`.

## Working Guidance
- For DX12 games, prefer bootstrap-aware injection over eager process-start injection.
- Base injection and hook-init decisions on observed runtime state such as module load, device creation, and shared runtime flags instead of executable names.
- Keep the pseudo-overlay and injected overlay handoff explicit. The current tree already has runtime flags for this; extending those flags is safer than adding guesswork in UI code.
- Keep recording/session state separate from the active video producer. New capture methods must not disable injection features, and screen-grab paths must not leave unused inject texture publication active.
- Log each bootstrap phase with enough context to reconstruct failures later: wait-loop start, D3D12 detection, fallback path, wait-loop exit, hook-init deferral, and overlay handoff state.
- For capture-method/injection separation diagnostics, look for `[Inject] Video method ... keeps injection active` and `[Media] Inject video publication enabled|disabled (...)`.
- When changing pending-injection scan logic, keep lock ownership explicit. Avoid calling public query helpers from code that already holds `injectMutex`; add or use locked helpers and policy tests instead.
- Preserve the one-call CFG registration bootstrap boundary when changing trampoline allocation or API resolution. The host CFG policy and the hook DLL's own CFG instrumentation must remain enabled; never solve a bootstrap failure by disabling x64 test-app CFG or marking whole trampoline pages valid.
- Preserve static-import fallbacks, publish every callable trampoline before activating its inline patch, and only then patch application-module imports to its hook. Do not patch Windows-directory module imports, translate the Rtl exit fallback into `ExitProcess`, cache dynamically resolved Kernel32/ntdll exports for guarded indirect calls, or inline-patch live exception-dispatch primitives during process-wide bootstrap.
- Do not treat the current polling sleeps as permission to add more timing bandaids. Existing polling is part of bootstrap orchestration; it is not a general-purpose fix for overlay or FG correctness bugs.

## Open Questions / Stale-Risk
- Stale risk is medium because injection timing and hook bootstrap are coupled to runtime behavior and can drift when wrapper or startup logic changes.
- Re-check this page after changes to `InjectionManager`, runtime handoff flags, or DX12 hook bootstrap conditions.
