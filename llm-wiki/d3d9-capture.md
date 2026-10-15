# Native D3D9 Capture

Last cross-checked: 2026-10-15 (classic Ex-gate unlock; hardware run pending)

Primary sources:
- `hook/d3d9/dx9_hook.cpp`
- `hook/d3d9/d3d9_capture_policy.*`
- `hook/d3d9/{d3d9_ex_flag_layout,dx9_classic_share_unlock}.*`, `hook/d3d9/dx9_hook_capture_direct_ring.cpp`
- `hook/present/vulkan_renderer_policy.h`
- `hook/vulkan_layer/{layer_ipc,vulkan_layer_present}.cpp`
- `captureengine/injection/{inject_main.cpp,inject_config_publication.cpp}, captureengine/media/media_main_threads_inject.cpp`
- `common/capture/inject_frame_source_policy.h`
- `common/ipc/shared_defs_detail/abi_constants_and_config.h`
- `hook/wrappers/{d3d9_wrap,wrapper_hooks}.cpp`
- `tests/{test_d3d9_capture_policy,test_d3d9_ex_flag_layout,test_inject_capture_source,test_vulkan_renderer_policy,test_inject_frame_source_policy}.cpp`

## Summary

Classic `Direct3DCreate9` applications keep a classic `IDirect3D9` factory and device. Capture must never promote them to D3D9Ex: Ex changes managed-resource, lost-device, reset, presentation, COM-identity, and potentially undocumented runtime-layout behavior.

The Windows Vista D3D9 documentation describes shared resources through `pSharedHandle`, including opening them through D3D9 or D3D9Ex. This is not a portable guarantee for a classic device in current drivers. Local NVIDIA x64 and x86 probes returned `D3DERR_INVALIDCALL` when a classic device created a shared texture and when it opened an Ex-helper texture, despite the caps bit and format checks succeeding. Inject zero-copy is therefore opportunistic for classic D3D9, not an invariant; the classic Ex-gate unlock below is how it is now obtained.

Zero-copy here means no GPU-to-CPU readback or CPU re-upload. A shareable inject path still requires one asynchronous GPU `StretchRect`. GPU-based WGC is the reliable no-CPU-readback alternative when a classic runtime rejects sharing, while overlay-only injection can remain active independently.

Without the Ex-gate unlock (multithreaded device, layout not located, proof failed) capture performance for classic D3D9 is much worse than for D3D9Ex. When sharing fails, injected capture falls back to the D3D11 staging path (GPU `StretchRect`, deferred GPU→CPU readback after Present, `LockRect`, and D3D11 upload); the readback is deferred so Present is not blocked, but the per-frame CPU/GPU work remains. Observed 4K/120 staging capture is really slow and severely affects game performance. For old classic-D3D9 games, running them through DXVK is recommended: D3D9-under-DXVK is captured through the Vulkan layer's GPU-resident transport, while native D3D9Ex already gets the fast GPU-only path without DXVK.

## Shared-ring architecture

- The actual game device is probed first for every candidate format. A classic-device `D3DERR_INVALIDCALL` skips helper construction immediately; advertised caps are not trusted.
- Only after that probe succeeds may a private D3D9Ex helper on the same adapter own single-level, non-MSAA, default-pool render-target textures. The helper factory/device never escapes to the application.
- The native game device must successfully open each helper-owned texture through the matching `CreateTexture` shared handle and perform the backbuffer blit itself.
- D3D11 must successfully import a probe handle before the ring is published to the media process.
- `D3DCAPS2_CANSHARERESOURCE` is an Ex-only advertised capability and is diagnostic only for classic devices. Actual create/open/import probes are authoritative.
- X8R8G8B8 sources use an A8R8G8B8 shared resource; `StretchRect` performs the GPU conversion. A8R8G8B8 and A2B10G10R10 retain their compatible transport formats.

## Classic Ex-gate unlock

The classic refusal is the Windows runtime's own API validation, not the driver: shared create/open reads
`device->factory` and rejects unless the factory's internal "created by Direct3DCreate9Ex" BOOL is set (the same
check opens `IDirect3DDevice9Ex::CheckResourceResidency`, vtable slot 125). OBS game capture sets that BOOL around
its shared `CreateTexture` (`get-graphics-offsets/d3d9-offsets.cpp`, `graphics-hook/d3d9-capture.cpp`); its older
per-build code-byte patch table ended at Windows 10 1607. CE does the data flip only, with three layers that must all
pass before anything is written:

1. Decode (`ce::d3d9_ex_flag::FindCandidates`): in the first 256 bytes of slot 125 of CE's own Ex helper device,
   a pointer load `mov r,[base+disp32]`, then `cmp dword [r+disp32], reg|0` (opcodes 39/3B/83 /7 imm 0), then
   `jz/jnz` short or near. No fixed signatures; SIB, REX.W compares, base mismatch, misaligned or >0x20000 offsets
   are rejected. The function must lie inside the loaded system-directory `d3d9.dll`.
2. Data (`SelectLayout`/`CheckObjects`): the reference factory is what the runtime returns from the device's own
   `GetDirect3D` (slot 6, must lie in d3d9.dll; the factory's vtable too), never a handle CE holds. On the Ex helper
   the field must hold that factory and the dword must be exactly 1; on the classic target the field must hold its
   `GetDirect3D()` result and the dword exactly 0. Reads use `ReadProcessMemory` on the own process.
   Session `20261015_123230` (Mirror's Edge x86, Steam overlay loaded) failed the first version, which compared
   against CE's `Direct3DCreate9Ex` handle: `candidates=1 rejected=1 lastReject=factory-mismatch`. Static disassembly
   of SysWOW64 d3d9.dll 10.0.26100.9549 shows the decode was right (`mov eax,[ebx+2B3Ch]; cmp [eax+4F44h],edi; jne`)
   and that pristine `CEnum::CreateDeviceEx` stores its own `this` at `+2B3C`, which `GetDirect3D` returns, so the
   held handle differed from the runtime's object in-game. Session `20261015_125417` named the owner: the Steam
   overlay wraps `Direct3DCreate9Ex` factories (`held=... vtbl=gameoverlayrenderer.dll reported=stored vtbl=d3d9.dll`),
   so a caller-held Ex factory is never trustworthy as the runtime object.
3. Function: the game-device probe is first run without the unlock (rejected), then with it; only a pass sets
   `directSharedClassicUnlock`, after which every shared create/open on a classic device of that ring (native
   producer, game open of helper textures, legacy helper, Reset retirement owner) runs inside
   `DX9ClassicShareUnlockScope`. The scope sets the BOOL with `InterlockedCompareExchange(0->1)` and restores 1->0.

Invariants:
- The device is never promoted and nothing outside the scoped call sees the flag set.
- The flag is factory-wide and not serialized against the runtime's device lock, so a game factory is unlocked
  only for devices without `D3DCREATE_MULTITHREADED` (the app may not call the device while CE runs inside its
  Present/Reset). CE-private factories (legacy helper) are always allowed. Residual, not handled: a second
  single-threaded device on the same factory driven concurrently from another thread.
- The located layout is cached per process (first locate result, success or failure).

## GDI interop fallback

Chosen after the shared ring and the legacy shared surface fail. It is not near-free: for devices without
`D3DCREATE_MULTITHREADED` the D3D9 `GetDC` + `BitBlt` consume runs on the render thread. Until 2026-10-15
`CaptureFrame` re-asked the stateful cadence gate after the Present/EndScene caller had already admitted the frame;
the second consult saw elapsed ~0 and skipped every admitted frame (`Pacing capture skip ... until=~minSpacing`),
so recordings never received a first frame (session `20261015_123230`, 29200 of 29210 recording frames with
`capture_us=0`). Cadence is gated once, by the caller, for every DX9 capture path.

## Synchronization and lifetime invariants

- Issue a D3D9 event query after every capture blit and publish only after a later nonblocking `GetData` reports completion.
- Never spin, sleep, or poll-wait for query completion. A flush flag may submit final work, but completion remains event-driven.
- A ring slot cannot be overwritten while the consumer lease is outstanding.
- Helper ownership keeps a published generation alive while game-device views are released before `Reset`; rebuild views only after a successful reset.
- D3D9 shared-resource identifiers are resource-owned KMT values, not closeable NT handles.
- If any sharing, format, or D3D11-import probe fails, retain the native game device. Inject capture may use its existing staging path; GPU-based WGC is preferred when no-CPU-readback capture is required. Do not promote the device as fallback.

## Diagnostics and failure modes

Expected initialization evidence includes the original device type/flags, adapter LUID or ordinal fallback, source/shared formats, Ex-only share-cap state, conversion result, native probe result, any helper producer and game-device open results, D3D11 import result, and final `D3D9-SHARED-DIRECT` or explicit fallback mode. Ex-gate unlock lines: `Classic share layout located (d3d9 <ver> ts=<link stamp> candidates= rejected= factoryOff= exFlagOff=)` or `not located (...)`, `shared-texture probe ... unlock=0|1 hr=`, `Classic share unlock proof passed|failed`, `Classic share unlock engaged for <label> (count=)` (first 4, then every 64th), `declined for <label> (<reason>)` change-gated, and `zero-copy path active (<producer> classicUnlock=0|1)`. A failed proof with a located layout means a further runtime or driver refusal.

DXVK remains on the Vulkan transport because its D3D9 shared values are not native Windows handles. True D3D9Ex applications retain their native Ex creation path.

## Translated D3D9 ownership

- A non-system `d3d9.dll` is not safe to probe like the Windows runtime. `DX9Hook::Init` discovers vtables by creating a synthetic factory/device; a translation runtime may treat that as a second renderer initialization rather than an isolated probe. Use the real-object IAT wrapper when it is the only available D3D9 path, and use the Vulkan layer for final presentation when that layer owns the process.
- Once the Vulkan layer owns presentation, it also owns overlay rendering, injected capture, screenshots, and FPS pacing for D3D9 translation. Do not keep a parallel DX9 Present path merely because a D3D9 front-end is loaded: final Vulkan WSI is the authoritative displayed-output boundary.
- Split 32-bit-to-64-bit translation stacks may keep the configured client and final Vulkan presentation in different processes. Portal RTX/RTX Remix sessions `20260825_190436`, `20260825_202150`, and `20260825_203627` prove this topology: `hl2.exe` is the 32-bit D3D9 client, while `NvRemixBridge.exe` owns the CE Vulkan layer, Vulkan device, and WSI swapchain. An exact bridge profile remains valid, but a non-whitelisted direct child renderer also inherits Vulkan-layer eligibility when its live parent PID is either the host's active source PID or the exact profile target published before injection, and that parent executable still matches the published whitelist. The pre-injection identity is required because the bridge can negotiate Vulkan before remote `LoadLibrary` publishes `sourcePid`. This preserves the configured parent profile without broadening it to unrelated helpers or relying on an executable-specific bridge rule.
- Inject capture in that topology is written by the layer inside the renderer, so every frame-ring slot carries the renderer PID (the handles live in its handle table) while `sourcePid` stays with the client. Media admits such slots only through `common/capture/inject_frame_source_policy.h`: `vulkanLayerClaim` must be {slot PID, session source} and a Toolhelp check (cached per pair) must show the renderer is the source's live direct child. Before this, media's source-PID pin dropped every NvRemixBridge.exe frame and Portal RTX recordings never left preparation (session `20260929_031827`). The cursor window is resolved from the session source, since the renderer owns no window.
- In that session CE's hook worker called `GetD3D9PresentAddresses` inside Remix's `d3d9.dll`, which negotiated a second Vulkan layer instance/device concurrently with the real renderer. The Remix dump then faulted with a null read inside `d3d9!remixapi_InitializeLibrary`. The generic prevention is policy-based: no synthetic D3D9 bootstrap for Vulkan-owned or non-system D3D9 runtimes.

## Open questions / stale-risk

- Ex-gate unlock VALIDATED 2026-10-15 (session `20261015_125417`, Mirror's Edge x86, 4K, classic flags 0x142,
  Steam overlay, NVIDIA, d3d9 10.0.26100.9549): layout `0x2b3c/0x4f44` located, proof passed, three game-device
  opens of helper-Ex textures, `D3D9-SHARED-DIRECT ... classicUnlock=1`, 2235 live frames, recording healthy.
  Hook CPU time per Present rose ~23 us on average while recording (perf CSV; the capture_us column does not time
  the shared-ring path). Still unvalidated: x64 classic games, Reset/alt-tab retention under the unlock, AMD/Intel.
  Multithreaded classic devices still decline (factory-wide flag race) and fall back.
- The 2026-07-12 NVIDIA 0x2f04 runtime rejected classic sharing in both x64 and x86. Fresh AMD and Intel probes remain required; even a successful driver must still pass fullscreen reset and third-party-overlay validation.
- A D3D9-to-D3D9Ex wrapper is not a generic compatibility solution. A complete compatibility layer would have to emulate managed resources, lost/reset behavior, presentation results, object identity, and every child-resource edge case; the removed partial managed-pool interception corrupted rendering.
- The currently supported transport formats are A8R8G8B8, X8R8G8B8 via GPU conversion, and A2B10G10R10. Other legacy backbuffer formats fail loudly rather than guessing a byte layout.
- `dx9_hook.cpp` remains over the preferred file-size limit; future D3D9 work should extract the shared transport and hook/bootstrap responsibilities without changing the invariants above.
