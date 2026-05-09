# DX11 Forced Anisotropic Filtering

Last cross-checked: 2026-05-09

Primary sources:
- `hook/common/sampler_override_utils.h`
- `hook/apis/dx11_hook.cpp`
- `hook/wrappers/d3d11_device_wrap.cpp`
- `hook/wrappers/d3d11_devicecontext_wrap.cpp`
- `hook/wrappers/d3d11_devicecontext_wrap.h`
- `tests/test_sampler_override_utils.cpp`

## Summary
Forced D3D11 AF is shader-aware and draw-time reconciled. This is aimed at NVIDIA
Blackwell corruption cases where blindly forcing AF can produce green/red dot
artifacts, while game-requested AF is normally safe because games apply it only to
appropriate material texture samples.

## Current Invariants
- D3D11 `CreateSamplerState` must not enable forced AF-on because no SRV/resource
  context is available there. It may still apply AF-off, mip mapping, and mip-bias
  overrides.
- A sampler is treated as a comparison sampler only when its `D3D11_FILTER` is a
  D3D comparison filter. `ComparisonFunc` alone, such as `D3D11_COMPARISON_ALWAYS`
  on a normal linear sampler, must not block AF.
- Runtime forced AF is applied only on the pixel stage, and only when active pixel
  shader metadata proves that the sampler uses only AF-safe sample opcodes.
  `sample_l` (explicit LOD) is now treated as AF-safe because it selects the mip
  level, while AF controls filtering within that level. `sample_b` (bias) is also
  safe. The check `D3D11ShaderSamplerUsesAFSafeSample()` accepts any sampler with
  `implicit || bias || lod` (and no unsafe explicit opcodes). `sample_d` (gradient),
  `sample_c` (comparison), and `OtherExplicit` remain unsafe and block AF.
- Shader metadata pairs each sampler register with the texture registers used by
  the same sample instruction. Every texture register sampled through that sampler
  must have a currently bound SRV, and every sampled SRV/resource must pass the
  material-texture classifier before AF is forced.
- Present-time deferred AF bootstrap must also ensure D3D11 sampler/SRV vtable hooks
  are installed on the actual device/context being presented. A global original
  function pointer is not proof that this specific runtime vtable slot is patched;
  additional vtables may be patched when the slot still points at the known original.
- Sampler/SRV/shader bind hooks track logical state and mark the pixel sampler
  state dirty. They do not repeatedly rebind samplers. Draw hooks consume that dirty
  flag and reconcile pixel samplers immediately before rendering, avoiding per-bind
  state churn while still applying AF to the draw that uses the shader/SRV/sampler
  combination.
- D3D11 context vtable draw hook slots must match the SDK order exactly:
  `DrawAuto=38`, `DrawIndexedInstancedIndirect=39`, `DrawInstancedIndirect=40`.
  Slot 41 is `Dispatch` and must not be hooked with an indirect-draw signature.
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
  `DX11: Runtime AF hook ensure from Present ...`, `DX11: AF pixel-shader metadata ...`,
  `DX11: AF allow shader-paired sampler ...` (now includes `lod=` field),
  `DX11: AF reconciled sampler ...`, and
  rate-limited skip reasons including no active pixel shader, no shader metadata,
  shader-unused sampler, explicit/non-implicit sample opcode, missing sampled SRV,
  unsupported format, single visible mip, unsafe resource, comparison filter, border,
  reduction, and fixed/no mips.
- Unsafe-resource skip logs include SRV format, resolved texture format, view dimension,
  dimensions, mip/view mip counts, most-detailed mip, array/sample count, and bind/misc
  flags so Blackwell artifact avoidance can be distinguished from missed hook coverage.
- Shutdown/host-disconnect summaries include `AF_runtimeHooks`,
  `AF_shaderMeta(created=... fail=...)`, `explicitSample`, `AF_lodAllowed`,
  `drawReconcile`, and the detailed skip counters.
- Wrapper path logs mirror the important skip and reconcile decisions with
  `Wrapper: AF ...` messages.
- New `AF_lodAllowed` counter tracks how many samplers now receive AF thanks to
  the relaxed `sample_l` policy. A non-zero value confirms the fix is working for
  games that use explicit LOD sampling.

## Verification
- Focused tests: `python build.py --run-tests --tests-only --skip-updates --gtest-filter=SamplerOverrideUtilsTest.*`
  passed 17/17 on 2026-05-09.
- Full build: `python build.py --skip-updates` passed on 2026-05-09 and produced build
  `0.1.2968`, compiling both x64/x86 hook DLLs.
- Full no-rebuild unit run: `python build.py --no-build --run-tests --skip-updates`
  passed 698/698 tests on 2026-05-09.

## Open Questions / Stale-Risk
- BioShock Infinite should be rerun with AF=16x. Expected proof is
  `DX11: Runtime AF hook ensure from Present ...`, `AF pixel-shader metadata ...`,
  `AF_lodAllowed` counters > 0 (showing that `sample_l`-using samplers now get AF),
  bounded `drawReconcile`, and either `AF reconciled sampler` lines for eligible
  textures or detailed skip lines explaining why specific sampled SRVs stayed blurry.
- The `sample_l` relaxation should be verified on NVIDIA Blackwell GPUs to confirm
  no corruption arises from the broader AF coverage. The existing resource policy
  (format, bind flags, view dimension, mip count) remains in place as the safety net.
- If BioShock is still slow with the FPS limiter inactive, compare `perf_metrics_*.csv`
  frame deltas against `drawReconcile`, `AF_replaced`, and skip counters.
- A more robust future option is DXBC token parsing instead of text disassembly. The
  current parser is covered by focused tests and keeps the implementation feasible, but
  DXBC-level parsing would remove dependence on disassembler text shape.
