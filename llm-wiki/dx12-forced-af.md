# DX12 Forced Anisotropic Filtering

Last cross-checked: 2026-07-15

Primary sources:
- `hook/common/dx12_sampler_policy.{h,cpp}`
- `hook/apis/dx12_sampler_hooks.{h,cpp}`
- `hook/wrappers/iat_hook.cpp`
- `tests/test_dx12_sampler_policy.cpp`

## Summary

DX12 forced AF uses a conservative creation-time policy because samplers and
resources are decoupled and immutable descriptor heaps cannot be safely rewritten
at draw time without shadow heaps, synchronization, and bindless compatibility
risk. The policy is generic across vendors; Blackwell makes incorrect sampler
promotion especially visible as red/green corruption.

## Invariants

- `sampler_override_mode=safe` promotes only ordinary non-comparison/non-reduction samplers with linear min/mag
  filtering, a usable mip range, and wrap/mirror addressing. `aggressive` broadens ordinary sampler coverage while
  preserving structurally special samplers.
- Every mode preserves comparison, minimum/maximum reduction, fixed/no-mip, invalid, and border samplers. Safe mode
  additionally preserves clamp/mirror-once and point-min/mag descriptors; aggressive mode intentionally opts ordinary
  instances of those two families into the override but can never promote a border sampler.
- `ComparisonFunc` does not classify a normal filter as a comparison sampler.
- AF-off and mip-bias changes use the same safety boundary. Mip-bias application
  is independent from AF configuration, but never changes protected sampler
  classes.
- Dynamic and static samplers share the same policy, including mip mapping. Dynamic coverage includes `CreateSampler`
  and `ID3D12Device11::CreateSampler2`; static coverage includes precompiled root-signature blobs for v1.0-v1.2.
- `CreateRootSignature` is the sole static-sampler mutation boundary. Serializer detours pass through so offset/base
  mip bias is never applied twice.
- Raw `D3D12CreateDevice` interception returns the requested unwrapped interface,
  then installs `CreateSampler` and `CreateRootSignature` hooks on the actual
  device before it reaches the game. Originals are retained per validated vtable.
- D3D12 device creation, `D3D12GetInterface`/`ID3D12DeviceFactory::CreateDevice`, and both root-signature serializers
  are covered through IAT imports and dynamic `GetProcAddress` resolution. Third-party overlay and Streamline modules
  retain the existing bypass rules.

## Diagnostics

- `DX12 AF: creation-time sampler policy configured ...` records the resolved
  configuration once.
- The first bounded set of unique descriptor fingerprints records source,
  decision reason, filter, addressing, LOD range, anisotropy, and bias.
- Shutdown summaries split dynamic/static observed and modified counts and emit
  per-reason counters.
- Forced AF with no observed descriptors warns about late/bypassed interception;
  observed-but-zero-modified distinguishes protected or already-compliant sets.
- A sampler-affecting configuration change after descriptor creation warns that
  existing immutable descriptors require a restart.

## Kena / Blackwell Evidence and Stale-Risk

- Kena build `0.1.4574` on an RTX 5070 loaded the per-process 16x profile and
  displayed a red edge around a dark lighting/shadow-like region. The trace
  installed a shared-vtable `CreateSampler` hook but contained no sampler
  modification events.
- The same trace resolved `D3D12SerializeVersionedRootSignature` through
  `GetProcAddress` with `no hook registered`, proving static-sampler coverage was
  incomplete.
- Fresh RTX 5070 validation is required. The expected result is nonzero eligible
  override counters, protected shadow/post-process sampler decisions, sharper
  eligible material textures, and no red/green corruption in the supplied scene.
- The 2026-07-15 border hardening is creation-time only and adds no draw/dispatch
  overhead. Required build `0.1.4878` and all 1,537 native tests at metadata
  `0.1.4879` passed; fresh Blackwell runtime validation remains pending.
