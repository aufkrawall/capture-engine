# Native D3D9 Capture

Last cross-checked: 2026-08-03

Primary sources:
- `hook/apis/dx9_hook.cpp`
- `hook/common/d3d9_capture_policy.*`
- `hook/wrappers/{d3d9_wrap,wrapper_hooks}.cpp`
- `tests/{test_d3d9_capture_policy,test_inject_capture_source}.cpp`

## Summary

Classic `Direct3DCreate9` applications keep a classic `IDirect3D9` factory and device. Capture must never promote them to D3D9Ex: Ex changes managed-resource, lost-device, reset, presentation, COM-identity, and potentially undocumented runtime-layout behavior.

The Windows Vista D3D9 documentation describes shared resources through `pSharedHandle`, including opening them through D3D9 or D3D9Ex. This is not a portable guarantee for a classic device in current drivers. Local NVIDIA x64 and x86 probes returned `D3DERR_INVALIDCALL` when a classic device created a shared texture and when it opened an Ex-helper texture, despite the caps bit and format checks succeeding. Inject zero-copy is therefore opportunistic for classic D3D9, not an invariant.

Zero-copy here means no GPU-to-CPU readback or CPU re-upload. A shareable inject path still requires one asynchronous GPU `StretchRect`. GPU-based WGC is the reliable no-CPU-readback alternative when a classic runtime rejects sharing, while overlay-only injection can remain active independently.

Capture performance for classic D3D9 is therefore usually much worse than for D3D9Ex. When sharing fails, injected capture falls back to the D3D11 staging path (GPU `StretchRect`, deferred GPU→CPU readback after Present, `LockRect`, and D3D11 upload); the readback is deferred so Present is not blocked, but the per-frame CPU/GPU work remains. Observed 4K/120 staging capture is really slow and severely affects game performance. For old classic-D3D9 games, running them through DXVK is recommended: D3D9-under-DXVK is captured through the Vulkan layer's GPU-resident transport, while native D3D9Ex already gets the fast GPU-only path without DXVK.

## Shared-ring architecture

- The actual game device is probed first for every candidate format. A classic-device `D3DERR_INVALIDCALL` skips helper construction immediately; advertised caps are not trusted.
- Only after that probe succeeds may a private D3D9Ex helper on the same adapter own single-level, non-MSAA, default-pool render-target textures. The helper factory/device never escapes to the application.
- The native game device must successfully open each helper-owned texture through the matching `CreateTexture` shared handle and perform the backbuffer blit itself.
- D3D11 must successfully import a probe handle before the ring is published to the media process.
- `D3DCAPS2_CANSHARERESOURCE` is an Ex-only advertised capability and is diagnostic only for classic devices. Actual create/open/import probes are authoritative.
- X8R8G8B8 sources use an A8R8G8B8 shared resource; `StretchRect` performs the GPU conversion. A8R8G8B8 and A2B10G10R10 retain their compatible transport formats.

## Synchronization and lifetime invariants

- Issue a D3D9 event query after every capture blit and publish only after a later nonblocking `GetData` reports completion.
- Never spin, sleep, or poll-wait for query completion. A flush flag may submit final work, but completion remains event-driven.
- A ring slot cannot be overwritten while the consumer lease is outstanding.
- Helper ownership keeps a published generation alive while game-device views are released before `Reset`; rebuild views only after a successful reset.
- D3D9 shared-resource identifiers are resource-owned KMT values, not closeable NT handles.
- If any sharing, format, or D3D11-import probe fails, retain the native game device. Inject capture may use its existing staging path; GPU-based WGC is preferred when no-CPU-readback capture is required. Do not promote the device as fallback.

## Diagnostics and failure modes

Expected initialization evidence includes the original device type/flags, adapter LUID or ordinal fallback, source/shared formats, Ex-only share-cap state, conversion result, native probe result, any helper producer and game-device open results, D3D11 import result, and final `D3D9-SHARED-DIRECT` or explicit fallback mode.

DXVK remains on the Vulkan transport because its D3D9 shared values are not native Windows handles. True D3D9Ex applications retain their native Ex creation path.

## Open questions / stale-risk

- The 2026-07-12 NVIDIA 0x2f04 runtime rejected classic sharing in both x64 and x86. Fresh AMD and Intel probes remain required; even a successful driver must still pass fullscreen reset and third-party-overlay validation.
- A D3D9-to-D3D9Ex wrapper is not a generic compatibility solution. A complete compatibility layer would have to emulate managed resources, lost/reset behavior, presentation results, object identity, and every child-resource edge case; the removed partial managed-pool interception corrupted rendering.
- The currently supported transport formats are A8R8G8B8, X8R8G8B8 via GPU conversion, and A2B10G10R10. Other legacy backbuffer formats fail loudly rather than guessing a byte layout.
- `dx9_hook.cpp` remains over the preferred file-size limit; future D3D9 work should extract the shared transport and hook/bootstrap responsibilities without changing the invariants above.
