# DX11 Forced Anisotropic Filtering

Last cross-checked: 2026-07-16

Primary sources:
- `hook/common/sampler_override_utils.h`
- `hook/common/{hook_common.h,hook_common.cpp}`
- `hook/apis/dx11_hook.cpp`
- `hook/wrappers/{d3d11_device_wrap,d3d11_devicecontext_wrap}.{h,cpp}`
- `tests/{test_sampler_override_utils,test_d3d11_context_wrapper_source}.cpp`

## Summary

D3D11 forced AF is a pixel-shader/SRV-aware bind replacement. The 32-bit BioShock
Infinite session `20260715_223043` showed that the previous temporal safety model
was itself the performance bug: about 1.67 million draw reconciliations scanned
about 4.83 million slots and issued about 704 thousand effective sampler calls,
while streaming warm-up, role history, resource-mutation registries, configuration
copies, private-data probes, atomics, and global locks ran around the draw path.
The limiter was inactive, so this CPU submission work starved the GPU.

The current model makes suitability immutable and object-owned. Shader metadata,
SRV classification, and forced sampler variants are computed or created outside
clean draws. A draw reads the configuration version and a context-local dirty mask;
when the mask is zero it performs no lock, descriptor query, COM resource walk,
history scan, allocation, logging counter, or driver sampler-state call. Only slots
whose shader/SRV/sampler inputs changed are reconsidered, and only genuinely changed
contiguous sampler runs are submitted. This is intended to remove measurable GPU
underutilization from the override; it does not claim literally zero CPU
instructions because interception and a cached version/dirty check still exist.

## Current invariants

- Forced AF-on is decided at pixel-sampler bind/draw reconciliation, never blindly
  in `CreateSamplerState`, because creation has no shader/SRV pairing. AF-off, mip
  mapping, and bias retain the shared sampler policy.
- Pixel-shader disassembly metadata is stored as a private COM interface on the
  shader. It precomputes the AF-candidate sampler mask, texture-to-sampler masks,
  and compact sampler-to-texture lists once. Shader changes acquire one immutable
  handle instead of copying or scanning the full relation matrix on every draw.
- `sample`, `sample_b`, and `sample_d` have a derivative footprint and are AF-safe.
  `sample_l`, comparison sampling, and unrecognized explicit sampling are unsafe.
  Any unsafe use of a sampler register blocks that sampler even if the same shader
  also uses it in a safe instruction.
- Every texture register paired with a candidate sampler must have a bound eligible
  SRV. Missing, unsafe, or single-visible-mip inputs restore/use the logical game
  sampler immediately.
- SRV suitability is structural and immutable: view/resource dimension, typed
  format, mip visibility, multisampling, and depth-stencil capability. Texel writes
  do not change those facts, so Map/Unmap/copy/update/generate-mips paths perform no
  AF mutation-registry or streaming-age work.
- SRVs created through the wrapper are classified once and store compact private
  data on the view. Externally-created views use the same lazy one-time fallback.
- Forced variants are owned by the original sampler through
  `SetPrivateDataInterface`, with separate 2x/4x/8x/16x keys plus negative and
  already-compliant markers. Contexts hold acquired references. This removes the
  wrapper's process-global replacement-cache lock and pointer-reuse lifetime risk.
- Safe/aggressive eligibility is checked before an object-owned variant lookup.
  A point sampler promoted in aggressive mode therefore cannot bypass safe policy
  after a live configuration change.
- `PSGetSamplers` exposes the logical game sampler rather than CE's temporary
  replacement. Application state observation remains transparent.
- `ClearState`, `ExecuteCommandList(..., FALSE)`, successful
  `FinishCommandList(FALSE)`, and `SwapDeviceContextState` clear the relevant
  tracking. The raw-vtable fallback also clears its state after an execute-without-
  restore boundary.
- Wrapper-to-real shader/SRV/sampler/draw forwarding remains guarded so the raw
  vtable fallback cannot run a second state machine for the same call.
- Modern contexts cache each inherited `ID3D11DeviceContext1` through Context4
  independently; support for Context4 must not leave Context1 forwarding null.

## Safety and coverage policy

- Safe mode requires linear min/mag filtering. Aggressive mode may promote ordinary
  point min/mag samplers. Both modes reject border addressing, comparison and
  min/max reduction filters, and fixed/invalid mip ranges. Clamp and mirror-once
  addressing are allowed in D3D10/11 when the remaining structural checks pass.
- Eligible D3D11 views include single-sample mipmapped Texture2D, Texture2DArray,
  TextureCube, and TextureCubeArray views. Common typed filterable material formats
  include BC1-BC7 (including BC4/BC5 normal/mask data and BC6H), ordinary UNORM/
  SNORM color and channel formats, and common floating-point material formats.
- Typed integer, depth, typeless/problematic, unknown/non-color, multisampled,
  non-2D-family, and single-visible-mip views remain blocked. A depth-stencil bind
  capability is a hard veto.
- RTV/UAV capability alone is not a veto: a typed, mipmapped, shader-sampled view
  can be a real material resource even when the underlying resource has additional
  legal bind capabilities. Current shader pairing and view structure provide the
  safety boundary.

## Performance architecture

- Wrapper steady-state draws do one cached config-version check and one dirty-mask
  branch. Clean draws do not call `PSGet*`, `GetDesc`, `GetResource`, `GetDevice`,
  private-data APIs, `CreateSamplerState`, or `PSSetSamplers`.
- A sampler/SRV/shader change dirties only affected sampler bits. Reconciliation
  walks only those bits and groups adjacent real changes into the minimum number of
  `PSSetSamplers` calls.
- The raw-vtable compatibility path has a process dirty-context atomic so clean raw
  draws return before its global state mutex. It shares shader/view object metadata
  with the wrapper and has no periodic draw-stat logging.
- Configuration snapshots are thread-local and refreshed by shared-memory version;
  string-heavy `GraphicsConfig` copies and parsing are absent from clean resolves.

## Diagnostics and verification

- Shader metadata, first view classifications, replacement creation/failure, and
  first skip/reconcile decisions remain bounded, high-signal logs. Periodic hot-draw
  counter logging and temporal streaming/role diagnostics were removed with their
  state machines.
- Required `python build.py --skip-updates` passed on 2026-07-16 as installed build
  `0.1.4878`, including x64/x86 hooks, x64/x86 Vulkan layers, packaging, import
  closure, PE mitigations, architecture, and PDB verification.
- Canonical `python build.py --no-build --run-tests --skip-updates` passed all 1,537
  native tests in 109 suites plus all Python tool self-tests at metadata `0.1.4879`.

## Open questions / stale-risk

- Installed x86 build `0.1.4878` was validated in 32-bit BioShock Infinite session
  `20260716_001012`. The trace sustained roughly 25,000 Presents with ordinary
  3-5 ms heartbeat gaps and no device-removal/stall signature. The user confirmed
  visually correct AF coverage, good performance, and no visible Blackwell
  corruption. This closes the original supplied-scene regression.
- Broader Blackwell and non-BioShock validation remains useful. Any future blurry
  draw should be attributable to an explicit structural veto (for example
  `sample_l`, depth, integer, border, comparison, or one visible mip), not temporal
  warm-up.
- The parser still consumes D3D disassembly text. A token-level DXBC parser would
  reduce dependence on text shape, but current parsing has focused regression
  coverage and is not part of the clean-draw cost.
