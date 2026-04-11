# DX12 Injection Bootstrap

Last cross-checked: 2026-04-11

Primary sources:
- `captureengine/injection.cpp`
- `captureengine/inject_main.cpp`
- `captureengine/pseudo_overlay.cpp`
- `hook/main.cpp`

## Scope
This page describes how DX12 injection and overlay bootstrap currently work, with emphasis on how to make inject and overlay behavior work optimally for DX12 games without turning the wiki into a substitute for the code.

## Facts
- Host-side injection currently uses a delayed-injection thread instead of injecting blindly at process start.
- The delayed-injection thread polls for graphics API readiness and tries to detect `d3d12.dll` with `EnumProcessModules()`.
- If `d3d12.dll` is detected, the current logic waits for a short additional settle window before attempting injection.
- If module enumeration fails, the injector logs the failure and falls back to conservative non-D3D12 timing instead of aborting the target entirely.
- `inject_main.cpp` sets shared runtime flags so the controller-side pseudo-overlay suppresses itself while the injected overlay handoff is pending or active.
- In WGC mode, explicit overlay targets can still enable overlay-only injection when the normal game whitelist is empty.
- In current wrapper builds, DX12 hook bootstrap is for state tracking and `ExecuteCommandLists` tracking. Present and `ResizeBuffers` interception comes from wrappers rather than DXGI vtable hooks.
- In wrapper builds, DX12 hook init is deferred until real D3D12 device creation is observed. In no-wrapper builds, the hook instance is initialized more eagerly so late injection does not miss the recovery path.

## Working Guidance
- For DX12 games, prefer bootstrap-aware injection over eager process-start injection.
- Base injection and hook-init decisions on observed runtime state such as module load, device creation, and shared runtime flags instead of executable names.
- Keep the pseudo-overlay and injected overlay handoff explicit. The current tree already has runtime flags for this; extending those flags is safer than adding guesswork in UI code.
- Log each bootstrap phase with enough context to reconstruct failures later: wait-loop start, D3D12 detection, fallback path, wait-loop exit, hook-init deferral, and overlay handoff state.
- Do not treat the current polling sleeps as permission to add more timing bandaids. Existing polling is part of bootstrap orchestration; it is not a general-purpose fix for overlay or FG correctness bugs.

## Open Questions / Stale-Risk
- Stale risk is medium because injection timing and hook bootstrap are coupled to runtime behavior and can drift when wrapper or startup logic changes.
- Re-check this page after changes to `InjectionManager`, runtime handoff flags, or DX12 hook bootstrap conditions.
