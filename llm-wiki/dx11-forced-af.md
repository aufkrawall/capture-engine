# DX11 Forced Anisotropic Filtering

Last cross-checked: 2026-05-05

Primary sources:
- `hook/common/sampler_override_utils.h`
- `hook/apis/dx11_hook.cpp`
- `hook/wrappers/d3d11_device_wrap.cpp`
- `hook/wrappers/d3d11_devicecontext_wrap.cpp`
- `hook/wrappers/d3d11_devicecontext_wrap.h`
- `tests/test_sampler_override_utils.cpp`

## Summary
Forced D3D11 AF is now texture-aware instead of create-time-blind. This is aimed at
NVIDIA Blackwell corruption cases where blindly forcing AF can produce green/red dot
artifacts, while game-requested AF is normally safe because games apply it only to
appropriate material textures.

## Current Invariants
- D3D11 `CreateSamplerState` must not enable forced AF-on because no SRV/resource
  context is available there. It may still apply AF-off, mip mapping, and mip-bias
  overrides.
- A sampler is treated as a comparison sampler only when its `D3D11_FILTER` is a
  D3D comparison filter. `ComparisonFunc` alone, such as `D3D11_COMPARISON_ALWAYS`
  on a normal linear sampler, must not block AF.
- Runtime forced AF is applied only when sampler and paired SRV/resource checks both
  pass. The default pairing remains conservative: same low sampler/SRV slot on the
  same shader stage.
- Present-time deferred AF bootstrap must also ensure D3D11 sampler/SRV vtable hooks
  are installed on the actual device/context being presented. A global original
  function pointer is not proof that this specific runtime vtable slot is patched;
  additional vtables may be patched when the slot still points at the known original.
- Replacement sampler caches are keyed by device plus final sampler descriptor, not
  by original sampler pointer, so one original sampler can be rebound differently as
  the paired SRV changes.
- Wrapper D3D11 context code tracks logical original samplers and SRVs just like the
  vtable hook path; replacement arrays passed to `*SetSamplers(StartSlot, Num, ...)`
  must be contiguous from index 0, not indexed by absolute slot.

## Resource Policy
The current safe resource classifier allows only shader-sampled, mipmapped
`Texture2D` resources that are single-sample, non-array, non-cube, not depth/stencil,
not render-target or UAV-bound, not integer/depth/typeless/problematic format, and
supported by `D3D11_FORMAT_SUPPORT_SHADER_SAMPLE`.

This is intentionally conservative. It may skip dynamic material-like textures that
also carry render-target bind flags, but it avoids the known Blackwell artifact class
on postprocess, shadow, single-mip, depth, and other non-material resources.

## Diagnostics
- Vtable path logs include `DX11: Deferred AF bootstrap ...`,
  `DX11: Runtime sampler/SRV hook ensure from Present ...`,
  `DX11: AF reconciled sampler ...`, and rate-limited skip reasons including no SRV,
  unsupported format, single visible mip, unsafe resource, comparison filter, border,
  reduction, fixed/no mips, and high slot.
- Unsafe-resource skip logs include SRV format, resolved texture format, view dimension,
  dimensions, mip/view mip counts, most-detailed mip, array/sample count, and bind/misc
  flags so Blackwell artifact avoidance can be distinguished from missed hook coverage.
- Shutdown/host-disconnect summaries include `AF_runtimeHooks` and the unsafe-resource
  skip count.
- Wrapper path logs mirror the important skip and reconcile decisions with
  `Wrapper: AF ...` messages.

## Verification
- Focused tests: `python build.py --run-tests --tests-only --skip-updates --gtest-filter=SamplerOverrideUtilsTest.*`
  passed on 2026-05-05.
- Focused regression rerun on 2026-05-05 also covered
  `SamplerOverrideUtilsTest.*` together with the basic-limiter regression tests.
- Full build: `python build.py --skip-updates` passed on 2026-05-05 and compiled both
  x64/x86 hook DLLs.

## Open Questions / Stale-Risk
- BioShock Infinite should be rerun with AF=16x. Expected proof is
  `DX11: Runtime sampler/SRV hook ensure from Present ...` early in the trace, followed
  by `PSSetShaderResources` / `AF reconciled sampler` evidence for eligible textures or
  detailed resource skip lines explaining why specific SRVs stayed blurry.
- Shader reflection or shader-bytecode pairing could make sampler/SRV matching less
  conservative later, but the current low-slot exact pairing is the safe baseline.
