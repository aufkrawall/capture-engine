# Graphics API reporting

Last verified: 2026-07-16

## Summary

The inject overlay reports the API evidenced by the active application device or context, rather than the newest
runtime interface available on the machine or an API DLL that happens to be loaded. The common formatting and
device-scope registry live in `hook/common/graphics_api_identity.h`; accepted visible-label changes flow through
`OverlayAdapter::SetGraphicsAPI` and dirty layout state only when the label actually changes.

## Labels and evidence

- DirectDraw reports `DirectDraw`, `DirectDraw2`, `DirectDraw3`, `DirectDraw4`, or `DirectDraw7` from the interface
  that created the active presentation surface. Successful application `IDirect3D3::CreateDevice` and
  `IDirect3D7::CreateDevice` evidence overrides that transport label with `DX6` or `DX7`.
- D3D8 reports `DX8` from its active device.
- D3D9 records classic versus Ex identity at successful `CreateDevice` / `CreateDeviceEx`. Registered capture and
  overlay helper devices are excluded. A one-time interface query is only a fallback for a device first discovered
  after creation. DXVK suffixes are preserved.
- D3D10 records `DX10` versus `DX10.1` at the matching creation export, including the combined device/swapchain
  entrypoints. A separately created device carries its identity into later factory swapchain presentation.
- Every D3D11 device starts at `DX11`. Successful application queries for Device1/2/3/4/5 or Context1/2/3/4
  monotonically promote only that device through `DX11.1` to `DX11.4`; CE wrapper/capture capability probes are
  explicitly scoped as internal and ignored. The active swapchain chooses which device identity is visible.
- OpenGL reparses `GL_VERSION` whenever the active `HGLRC` changes. OpenGL 3.2+ also uses
  `GL_CONTEXT_PROFILE_MASK`, producing labels such as `OpenGL 2.1`, `OpenGL 3.3 Compat`, and
  `OpenGL 4.6 Core`; parse failure reports `OpenGL`.

## Source anchors

- Formatting, parsing, monotonic upgrade, scoped registry: `hook/common/graphics_api_identity.h`
- Transition-only overlay update and diagnostics: `hook/common/overlay_adapter.cpp`
- DirectDraw 1/2/3/4/7 plus D3D6/7 evidence: `hook/apis/ddraw_hook.cpp`
- D3D9 classic/Ex and helper exclusion: `hook/apis/dx9_hook.cpp`
- D3D10/10.1 and D3D11 revision evidence: `hook/apis/dx11_hook.cpp`, `hook/wrappers/wrapper_hooks.cpp`,
  `hook/wrappers/d3d11_device_wrap.cpp`, `hook/wrappers/d3d11_devicecontext_wrap.cpp`
- OpenGL version/profile reporting: `hook/apis/opengl_hook.cpp`

## Invariants and failure modes

- Feature levels are not API revisions and never change the label.
- DirectDraw bootstrap devices, D3D9 capture helpers, D3D11 wrapper promotions, and capture synchronization probes
  cannot promote the application label.
- D3D11 revision evidence is monotonic per real device, but switching to another swapchain/device selects that
  device's independent revision.
- DirectDraw's D3D9Ex overlay bridge is an implementation detail and does not turn DirectDraw into `DX9Ex`.
- Repeated per-frame submissions of an unchanged label are no-ops and must not invalidate cached overlay layout.

## Tests

`tests/test_graphics_api_identity.cpp` covers all labels, precedence, DXVK suffixes, scoped-device isolation,
monotonic D3D11 promotion, OpenGL parsing/profiles/context isolation, and unchanged-label behavior.
`tests/test_graphics_api_reporting_source.cpp` protects creation-hook wiring and internal-evidence exclusions.

Open question / stale-risk: DirectDraw capture still relies on Windows exposing a Surface7 upgrade for old surface
interfaces. The API identity remains exact even when that capture-backend upgrade is unavailable, but capture and
overlay rendering then log a rate-limited upgrade failure.
