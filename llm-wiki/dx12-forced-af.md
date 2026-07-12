# DX12 Forced Anisotropic Filtering

Last cross-checked: 2026-07-12

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

- Only ordinary non-comparison/non-reduction samplers with linear min/mag
  filtering, a usable mip range, and wrap/mirror addressing can be promoted.
- Comparison, minimum/maximum reduction, border, clamp/mirror-once, fixed/no-mip,
  point-min/mag, and invalid descriptors retain their game semantics.
- `ComparisonFunc` does not classify a normal filter as a comparison sampler.
- AF-off and mip-bias changes use the same safety boundary. Mip-bias application
  is independent from AF configuration, but never changes protected sampler
  classes.
- Dynamic and static samplers share the same policy. Static sampler coverage
  includes serializer descriptors and precompiled root-signature blobs for v1.0
  and v1.1.
- Raw `D3D12CreateDevice` interception returns the requested unwrapped interface,
  then installs `CreateSampler` and `CreateRootSignature` hooks on the actual
  device before it reaches the game. Originals are retained per validated vtable.
- D3D12 device creation and both root-signature serializers are covered through
  IAT imports and dynamic `GetProcAddress` resolution. Third-party overlay and
  Streamline modules retain the existing bypass rules.

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
