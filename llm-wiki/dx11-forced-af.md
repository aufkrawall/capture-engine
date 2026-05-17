# DX11 Forced Anisotropic Filtering

Last cross-checked: 2026-05-17

Primary sources:
- `hook/common/sampler_override_utils.h`
- `hook/apis/dx11_hook.cpp`
- `hook/wrappers/d3d11_device_wrap.cpp`
- `hook/wrappers/d3d11_devicecontext_wrap.cpp`
- `hook/wrappers/d3d11_devicecontext_wrap.h`
- `hook/wrappers/wrapper_hooks.cpp`
- `tests/test_sampler_override_utils.cpp`

## Summary
Forced D3D11 AF is shader-aware and draw-time reconciled. This is aimed at NVIDIA
Blackwell corruption cases where blindly forcing AF can produce green/red dot
artifacts, while game-requested AF is normally safe because games apply it only to
appropriate material texture samples. UE3/BioShock-style games may create multiple
temporary D3D11 devices/contexts before the real game swapchain; the forced-AF
bootstrap therefore must be per-context and must keep both the vtable-hook path and
the returned wrapper-context path able to see shader metadata, sampler binds, SRVs,
and draws. When a wrapped context forwards calls to the real context, the raw vtable
hook must bypass duplicate AF tracking for that forwarded call; otherwise the
wrapper and raw state machines can fight over the same real sampler slots. The
current stability policy treats active material-texture streaming conservatively for
new or unstable SRVs, allows already-stable safe SRVs to keep their forced sampler
through unrelated streaming noise, and lets shader sampler slots recover from mixed
safe/unsafe history after bounded probation. BioShock load-scene/traversal crashes
correlated with texture streaming and high sampler replacement churn, so the wrapper
also keeps fast disabled-AF exits and resource-write negative caches to avoid paying
forced-AF bookkeeping when it cannot affect the draw.

## Current Invariants
- D3D11 `CreateSamplerState` must not enable forced AF-on because no SRV/resource
  context is available there. It may still apply AF-off, mip mapping, and mip-bias
  overrides.
- A sampler is treated as a comparison sampler only when its `D3D11_FILTER` is a
  D3D comparison filter. `ComparisonFunc` alone, such as `D3D11_COMPARISON_ALWAYS`
  on a normal linear sampler, must not block AF.
- Runtime forced AF is applied only on the pixel stage, and only when active pixel
  shader metadata proves that the sampler uses implicit `sample` only and every
  sampled SRV/resource is safe and stable. `sample_b` and `sample_l` are tracked
  but are not forced anymore; mixed implicit+LOD, bias-only, and LOD-only samplers
  stay blurry by design until runtime validation proves a broader policy is safe
  on Blackwell. `sample_d` (gradient), `sample_c` (comparison), and
  `OtherExplicit` remain unsafe and block AF.
- Shader metadata pairs each sampler register with the texture registers used by
  the same sample instruction. Every texture register sampled through that sampler
  must have a currently bound SRV, and every sampled SRV/resource must pass the
  material-texture classifier before AF is forced.
- Otherwise-eligible SRVs are not forced immediately after first observation.
  The wrapper classifies them as `pending-stable-observation` until they have been
  seen repeatedly and are old enough in draw-count terms. Non-color/problematic
  resources still block immediately.
- The wrapper has a global material-streaming quiet gate. Runtime forced AF is
  suspended for new or not-yet-stable AF-candidate material SRVs while recently seen
  candidates or candidate writes show active streaming. Already-stable allowed SRVs
  bypass this global gate so unrelated streaming does not make crisp textures flip
  back to blurry originals. When the global quiet state changes, the wrapper dirties
  current candidate/forced pixel samplers; if the reopen draw has no candidate mask,
  a delayed dirty is armed and consumed as soon as candidate samplers appear again.
- Resource-write mutation tracking is marker-based and registry-assisted. A resource
  only receives the forced-AF mutation marker after it has been observed through an
  AF-candidate SRV; later writes to unmarked buffers/render targets/non-candidate
  resources are skipped without descriptor reclassification. A small per-thread
  negative cache avoids repeated shared-map/private-data lookups for hot
  non-candidate resources during traversal. This keeps candidate material textures
  protected by bind-time warm-up and write invalidation without turning every buffer
  or render-target update into forced-AF overhead.
- Shader sampler slots are treated as semantic roles with bounded probation. Unsafe
  current resources still block immediately, but a shader-slot role that previously
  saw mixed safe/unsafe resources can recover after stable allowed observations on
  the current safe resources. This avoids permanent blur when UE3-style material
  systems reuse a slot for different roles across shaders or passes, while still
  preventing rapid AF/original flipping on an actively unsafe current binding.
- Present-time deferred AF bootstrap must also ensure D3D11 sampler/SRV vtable hooks
  are installed on the actual device/context being presented. A global original
  function pointer is not proof that this specific runtime vtable slot is patched;
  additional vtables may be patched when the slot still points at the known original.
  The deferred bootstrap is per immediate context, not a process-wide one-shot,
  because temporary devices can otherwise consume the bootstrap and leave the final
  game context blurry.
- Sampler/SRV/shader bind hooks track logical state and mark the pixel sampler
  state dirty. They do not repeatedly rebind samplers. Draw hooks consume that dirty
  flag and reconcile pixel samplers immediately before rendering, avoiding per-bind
  state churn while still applying AF to the draw that uses the shader/SRV/sampler
  combination.
- `Wrapped_D3D11CreateDevice` now returns a wrapped immediate context while still
  returning a wrapped device; `Wrapped_D3D11CreateDeviceAndSwapChain` keeps the raw
  device/swapchain compatibility path but returns a wrapped immediate context. This
  gives UE3-style games a COM-wrapper interception path even when cached vtable draw
  pointers bypass the patched runtime vtable slots.
- Pixel-shader metadata must be registered into both the vtable-hook metadata cache
  and the wrapper metadata cache. A raw-device plus wrapped-context path can
  otherwise track draws and sampler binds but still skip AF due to missing wrapper
  shader metadata.
- D3D11 context vtable draw hook slots must match the SDK order exactly:
  `DrawAuto=38`, `DrawIndexedInstancedIndirect=39`, `DrawInstancedIndirect=40`.
  Slot 41 is `Dispatch` and must not be hooked with an indirect-draw signature.
- Replacement sampler caches are keyed by device plus final sampler descriptor, not
  by original sampler pointer, so one original sampler can be rebound differently as
  the paired SRV changes.
- Wrapper D3D11 context code tracks logical original samplers and SRVs just like the
  vtable hook path; replacement arrays passed to `*SetSamplers(StartSlot, Num, ...)`
  must be contiguous from index 0, not indexed by absolute slot.
- Wrapper-to-real context forwarding for `*SetSamplers`, `*SetShaderResources`,
  `PSSetShader`, and draw calls uses a thread-local guard exported by
  `dx11_hook.cpp`. Raw vtable hooks must forward straight to the original function
  under that guard and must not update raw AF state. Seeing both `Wrapper: AF allow`
  and `DX11: AF allow` for the same wrapped context in one frame is a regression
  signal for duplicate state tracking.

## Resource Policy
The current safe resource classifier allows only shader-sampled, mipmapped
`Texture2D` resources that are single-sample, non-array, non-cube, not depth/stencil,
not render-target or UAV-bound, not integer/depth/typeless/problematic format, and
supported by `D3D11_FORMAT_SUPPORT_SHADER_SAMPLE`.

The allowed color set includes common mipmapped material formats such as sRGB BC1/2/3/7
and non-sRGB UNORM BC1/2/3/7 plus standard `R8G8B8A8`, `B8G8R8A8`, and `B8G8R8X8`
UNORM textures. BC4/BC5/BC6H, integer, depth, typeless, RT/UAV, array, cube,
multi-sample, and single-visible-mip resources stay blocked.

This is intentionally conservative. It may skip dynamic material-like textures that
also carry render-target bind flags, but it avoids the known Blackwell artifact class
on postprocess, shadow, single-mip, depth, normal-map, and other non-material
resources.
In the wrapper path, an otherwise-allowed SRV starts as
`pending-stable-observation` and becomes force-AF eligible only after the warm-up
threshold is met and the global material-streaming window is quiet. The current
threshold is four observations and 120000 draw calls since first seeing the SRV,
with a longer 600000-draw streaming-age target and an accelerated path for high
confidence large material textures. Once a resource is stable and allowed, global
streaming noise from other resources no longer demotes it to
`pending-streaming-quiet`; only direct candidate writes or unsafe current bindings
should make it back off.

## Diagnostics
- Vtable path logs include `DX11: Deferred AF bootstrap ...`,
  `DX11: Runtime AF hook ensure from Present ...`, `DX11: AF pixel-shader metadata ...`,
  `DX11: AF allow shader-paired sampler ...` (includes `lod=` field),
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
  `drawReconcile`, `AF_bootstrap(complete=... retry=... disabled=...)`, and the
  detailed skip counters. Under the current implicit-only rule, `AF_lodAllowed`
  should normally remain zero; a non-zero value means the policy changed again.
- Wrapper path logs mirror the important skip and reconcile decisions with
  `Wrapper: AF ...` messages. `Wrapper: AF draw hook hit ...`,
  `Wrapper: AF sampler bind tracked ...`, `Wrapper: AF sampler bind effective ...`,
  `Wrapper: AF draw stats ...`, `Wrapper: AF allow ...`, and
  `Wrapper: AF reconciled ...` are the quickest proof that the returned wrapper
  context path is actually participating in a UE3 title. New BioShock logs should
  no longer show raw `DX11: AF allow ...` lines that are sourced by wrapper-forwarded
  calls on the same real context.
- Wrapper draw stats now also include real sampler call counts, deferred unchanged
  dirty marks, streamed-resource warm-up skips, global streaming quiet skips and
  transitions, quiet-reopen delayed dirty events, candidate resource registry hits,
  misses, negative-cache hits, stable-resource global-gate bypasses, role
  probation/recovery counts, unmarked write skips, mixed-role shader-slot
  skip/block counts, and SRV cache hit/miss/write counters. Useful skip strings
  include `pending-stable-observation`, `pending-streaming-quiet`, `shader slot
  mixed safe/unsafe resource role`, and `shader-slot role blocked after unsafe
  resource`.
- When the app's forced-AF override is disabled but other graphics overrides are
  active, D3D11 sampler creation and wrapper bind/draw/resource-write paths should
  log pass-through/disabled-AF diagnostics only at low rate. A game-owned 16x AF
  control run should not produce wrapper AF draw stats or millions of registry
  misses; that indicates forced-AF bookkeeping leaked into a disabled configuration.

## Verification
- Focused shader/parser coverage still lives in `SamplerOverrideUtilsTest.*`.
- Full build: `python build.py --skip-updates` passed on 2026-05-14 and produced
  build `0.1.3115`, compiling both x64/x86 hook DLLs.
- Focused no-rebuild unit run:
  `python build.py --no-build --run-tests --skip-updates --gtest-filter=SamplerOverrideUtilsTest.*`
  passed 17/17 tests on 2026-05-14. The command bumped displayed metadata to
  `0.1.3116`.
- Full build: `python build.py --skip-updates` passed on 2026-05-15 and produced
  build `0.1.3173`, compiling both x64/x86 hook DLLs.
- Focused no-rebuild unit run:
  `python build.py --no-build --run-tests --skip-updates --gtest-filter=SamplerOverrideUtilsTest.*`
  passed 17/17 tests on 2026-05-15. The command bumped displayed metadata to
  `0.1.3174`.
- Full build: `python build.py --skip-updates` passed on 2026-05-17 and produced
  build `0.1.3246`, compiling both x64/x86 hook DLLs.
- Focused no-rebuild unit run:
  `python build.py --no-build --run-tests --skip-updates --gtest-filter=SamplerOverrideUtilsTest.*`
  passed 21/21 tests on 2026-05-17. The command bumped displayed metadata to
  `0.1.3247`.

## Open Questions / Stale-Risk
- BioShock Infinite should be rerun with AF=16x on build `0.1.3246` or later.
  Expected proof is `Wrapped_D3D11CreateDevice: Returned wrapped immediate context`,
  `Wrapper: AF draw hook hit`, `Wrapper: AF sampler bind tracked`, lower wrapper
  reconcile/bind churn, stable resources continuing to report `stableGlobalBypass`
  instead of flipping back to `pending-streaming-quiet`, and either `Wrapper: AF
  allow` / `Wrapper: AF reconciled` lines for eligible material textures or detailed
  skip lines explaining why specific sampled SRVs stayed blurry. A game-owned 16x AF
  control run should show real anisotropic sampler descriptors but no wrapper AF
  draw stats when the app AF override is default/off.
- BioShock logs at `installed/captureengine/logs/20260517_085737_lowslow` showed
  blurry/crisp oscillation and traversal overhead. The likely generic root causes
  addressed here were stable resources being demoted by unrelated global streaming
  quiet noise and forced-AF tracking staying active when only other graphics
  overrides were enabled. Validate that the next run lowers `registryMiss`,
  exposes `registryNegHit` for hot non-candidates, and no longer alternates stable
  material SRVs back to originals unless they are directly invalidated.
- Bias/LOD-only material textures may remain blurry under the current conservative
  rule. Re-expanding to `sample_b` or `sample_l` requires fresh Blackwell validation
  without artifacts and should keep the wrapper/vtable forwarding guard intact.
- If BioShock is still slow with the FPS limiter inactive, compare `perf_metrics_*.csv`
  frame deltas against `drawReconcile`, `AF_replaced`, and skip counters.
- A more robust future option is DXBC token parsing instead of text disassembly. The
  current parser is covered by focused tests and keeps the implementation feasible, but
  DXBC-level parsing would remove dependence on disassembler text shape.
