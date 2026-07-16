# Cross-API Forced Anisotropic Filtering

Last cross-checked: 2026-07-16

Primary sources:
- `hook/common/sampler_override_utils.h`
- `hook/apis/{dx9_sampler_state,legacy_d3d_sampler_state,opengl_sampler_override,opengl_texture_storage_override}.*`
- `hook/apis/{ddraw_hook,dx8_hook,dx9_hook,dx11_hook,opengl_hook}.cpp`
- `hook/wrappers/{d3d9_device_wrap,d3d10_device_wrap}.*`
- `tests/{test_sampler_override_utils,test_inject_capture_source}.cpp`
- `llm-wiki/{dx11-forced-af,dx12-forced-af}.md`

## Summary

Forced AF now follows each API's native state model instead of using one replacement strategy everywhere. D3D10,
D3D12, and Vulkan mutate immutable sampler descriptions only at creation. D3D9 and D3D6-8 reconcile mutable sampler
state only when the application changes a texture or sampler state, plus a configuration-version boundary. OpenGL
reconciles at texture/sampler-parameter and mip-storage events. None of these paths adds a draw/dispatch hook, a
per-draw resource query, a GPU wait, or a sampler-object replacement cache.

D3D11 remains the exceptional shader/resource-aware implementation documented in `dx11-forced-af.md`. Runtime session
`installed/captureengine/logs/20260716_001012` used build `0.1.4878` in 32-bit BioShock Infinite. The trace sustained
roughly 25,000 Presents with ordinary 3-5 ms heartbeat gaps and no device-removal/stall signature; the user confirmed
that material textures received the intended AF effect, performance was good, and no visual corruption was visible.

## API invariants

- **D3D10:** sampler descriptors are modified once in `CreateSamplerState`; the old PS/VS/GS `SetSamplers` replacement
  hooks, global replacement cache, locks, and one-time rebinding walk are removed. Safe mode preserves comparison,
  border, fixed-LOD, and point-min/mag samplers while allowing ordinary clamp/wrap/mirror material samplers. A rejected
  modified descriptor is discarded and the exact original descriptor is retried transactionally.
- **D3D9:** one raw-device owner intercepts `SetTexture`, `SetSamplerState`, and logical `GetSamplerState`. Per-vtable
  original callbacks keep classic and Ex devices safe when their vtables differ. State is split into the application's
  logical values and the physical forced values; only changed companion states are written. Eligibility requires a
  bound texture, more than one visible mip, a mip filter, supported MIN/MAG anisotropy caps, and non-special addressing.
  2D/cube textures ignore irrelevant W addressing; volume textures include it. Autogen-mipmap resources derive their
  effective chain from level-zero dimensions instead of being mistaken for single-level textures.
- **D3D9 late injection:** getter/resource bootstrap is attempted once per sampler, never on every draw or repeatedly
  after a pure-device getter failure. New/reset devices start from documented defaults. Present performs only a cached
  configuration-version check after initialization; a real config change reconciles tracked state and restores logical
  state when the override is disabled.
- **D3D8/D3D7/D3D6:** a shared event-driven texture-stage-state owner provides the same logical/physical split and
  bounded one-time bootstrap. D3D8 also refreshes config state at Present with a version fast path. D3D7 uses the actual
  vtable slots 36/37 for Get/SetTextureStageState and slot 20 for SetRenderState. D3D6 uses Device3 slots 39/40. Legacy
  MAG anisotropy is value 5 (`D3DTFG_ANISOTROPIC`), not MIN's value 3; using 3 as MAG selects flat-cubic filtering.
  D3D5 and older expose no anisotropic value in their pre-stage `D3DTEXTUREFILTER` render states, so there is no generic
  AF action to take there.
- **OpenGL:** core bound-texture APIs, sampler objects, core DSA, and EXT DSA parameter variants share one policy.
  Texture image/compressed image/copy-image allocation, immutable storage, texture-view creation, and mip-generation
  entry points trigger reconciliation only when mip availability can change. Actual level `base+1` allocation is queried before safe-mode AF
  is enabled. The implementation recognizes EXT/ARB anisotropy, clamps to `GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT`, protects
  compare/border/non-mip/point-filter objects, handles dimensional wrap relevance, and excludes rectangle, buffer,
  multisample, and proxy targets.
- **D3D12/Vulkan:** their existing creation-time implementations already match the no-draw-overhead requirement and
  were intentionally left unchanged. D3D12 covers dynamic and static samplers/root signatures; Vulkan requires the
  device feature, clamps to physical limits, and transactionally retries rejected modified descriptors. See
  `dx12-forced-af.md` and `graphics-overrides-and-frame-pacing.md`.

## Performance and diagnostics

- D3D10, D3D12, and Vulkan have no bind/draw work after sampler creation.
- D3D9 and D3D6-8 do constant-size bookkeeping only on mutable state events. Driver getters are bootstrap-only;
  config hashes are computed only when the shared config version changes; companion writes are skipped when the
  physical value is already correct.
- OpenGL does no `glBindTexture` or draw interception. Additional GL queries/writes occur only at parameter or storage
  mutation boundaries, not while sampling; an already-correct anisotropy value skips the redundant driver write.
- Transition, bootstrap failure, descriptor retry, and safety-decision logs are rate-limited. Shutdown summaries report
  reconciliations, driver writes, bootstrap attempts, and OpenGL storage/parameter events.

## Verification

- Required `python build.py --skip-updates` passed end to end as installed build `0.1.4892`, including x64/x86 hooks,
  Vulkan layers, packaging, import closure, PE mitigations/architecture, and PDB checks.
- Canonical `python build.py --no-build --run-tests --skip-updates` passed all 1,546 native tests in 111 suites plus
  all Python tool self-tests at metadata `0.1.4893`.
- Native runtime validation remains intentionally separate for the APIs listed below; the existing BioShock result
  establishes the D3D11 behavior, not legacy/OpenGL driver behavior.

## Open questions / stale-risk

- Native runtime validation is still required for D3D10, classic/Ex D3D9, D3D8, D3D7, D3D6, and representative OpenGL
  core/DSA/shared-context applications on both x86 and x64. Source and policy tests do not prove vendor-driver behavior.
- A state block created before late injection can restore sampler state without calling the public mutable-state setter.
  The current design deliberately has no draw-time fallback; validate state-block-heavy legacy games before considering
  state-block metadata interception, because getter sweeps at every Apply would violate the performance invariant.
- D3D10 samplers created before a late injection cannot be enumerated or safely replaced without retaining a bind-time
  indirection. Creation-time interception is the intentional zero-steady-state-overhead tradeoff.
- OpenGL extension function pointers are driver/context supplied. Multi-ICD or unusual context migration remains a
  focused runtime-validation target even though capability state is context-local and reset after `wglMakeCurrent`.
