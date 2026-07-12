# DX12 Injection Bootstrap

Last cross-checked: 2026-07-12

Primary sources:
- `captureengine/injection.cpp`
- `captureengine/injection_policy.h`
- `captureengine/inject_main.cpp`
- `captureengine/media_main.cpp`
- `captureengine/pseudo_overlay.cpp`
- `common/inject_overlay_policy.cpp`
- `common/shared_defs.h`
- `hook/main.cpp`
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
- `capture_method` selects only the video acquisition path. Explicit `wgc` and `dxgi_dup` preserve the normal `whitelist`, so the injected overlay and graphics overrides remain active for those targets; `overlay_whitelist` remains the overlay-only target list. `BuildInjectorConfigState` must not clear either list based on capture method.
- Hook-side video publication is a separate runtime decision. The raw `captureRequested` bit remains the recording/session signal used by REC UI and capture-synced policy, while `kCaptureRuntimeFlagInjectVideoCaptureRequested` is set only while the active video path consumes injected frames. The injector clears this path flag before publishing every new start request and again on stop, so an interrupted prior media session cannot leak stale inject publication into a screen-grab start. WGC/DXGI recordings therefore keep injection features without paying for unused injected texture copies.
- `auto` routing is unchanged: a normal-whitelist match selects inject video, non-matches select the existing WGC/DXGI target path, and overlay-only targets stay on screen capture. During a live inject-to-WGC fallback, inject publication remains enabled until WGC first-frame proof; the media coordinator clears the inject-video flag only after committing the WGC path and before stopping the inject capture pipeline.
- In current wrapper builds, DX12 hook bootstrap is for state tracking and `ExecuteCommandLists` tracking. Present and `ResizeBuffers` interception comes from wrappers rather than DXGI vtable hooks.
- In wrapper builds, DX12 hook init is deferred until real D3D12 device creation is observed. In no-wrapper builds, the hook instance is initialized more eagerly so late injection does not miss the recovery path.

## Working Guidance
- For DX12 games, prefer bootstrap-aware injection over eager process-start injection.
- Base injection and hook-init decisions on observed runtime state such as module load, device creation, and shared runtime flags instead of executable names.
- Keep the pseudo-overlay and injected overlay handoff explicit. The current tree already has runtime flags for this; extending those flags is safer than adding guesswork in UI code.
- Keep recording/session state separate from the active video producer. New capture methods must not disable injection features, and screen-grab paths must not leave unused inject texture publication active.
- Log each bootstrap phase with enough context to reconstruct failures later: wait-loop start, D3D12 detection, fallback path, wait-loop exit, hook-init deferral, and overlay handoff state.
- For capture-method/injection separation diagnostics, look for `[Inject] Video method ... keeps injection active` and `[Media] Inject video publication enabled|disabled (...)`.
- When changing pending-injection scan logic, keep lock ownership explicit. Avoid calling public query helpers from code that already holds `injectMutex`; add or use locked helpers and policy tests instead.
- Do not treat the current polling sleeps as permission to add more timing bandaids. Existing polling is part of bootstrap orchestration; it is not a general-purpose fix for overlay or FG correctness bugs.

## Open Questions / Stale-Risk
- Stale risk is medium because injection timing and hook bootstrap are coupled to runtime behavior and can drift when wrapper or startup logic changes.
- Re-check this page after changes to `InjectionManager`, runtime handoff flags, or DX12 hook bootstrap conditions.
