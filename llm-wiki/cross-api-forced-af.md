# Cross-API Forced Anisotropic Filtering

Last cross-checked: 2026-09-24 (D3D9 state-block snapshots and below-the-slot sampler re-arm; no hardware run)

Primary sources:
- `hook/common/sampler_override_utils.h`
- `common/mip_mapping_policy.h`
- `hook/apis/{dx9_sampler_state,legacy_d3d_sampler_state,opengl_sampler_override,opengl_texture_storage_override}.*`
- `hook/apis/{ddraw_hook,dx8_hook,dx9_hook,dx11_hook,opengl_hook}.cpp`
- `hook/wrappers/{d3d9_device_wrap,d3d10_device_wrap}.*`
- `tests/{test_mip_mapping_policy,test_sampler_override_utils,test_inject_capture_source}.cpp`
- `tests/{test_legacy_d3d7_vtable_abi,test_legacy_d3d8_vtable_abi}.cpp`
- `llm-wiki/{dx11-forced-af,dx12-forced-af}.md`

## Summary

Forced AF now follows each API's native state model instead of using one replacement strategy everywhere. D3D10,
D3D12, and Vulkan mutate immutable sampler descriptions only at creation. D3D9 and D3D6-8 reconcile mutable sampler
state when the application changes a texture or sampler state, at a configuration-version boundary, and immediately
after a state block restores physical state. OpenGL reconciles at texture/sampler-parameter, mip-storage, and cached
object-bind events. None of these paths adds a draw/dispatch hook, a per-draw resource query, a GPU wait, or a
sampler-object replacement cache.

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
  state when the override is disabled. Per-vtable Create/Begin/EndStateBlock and Capture interception gives every block
  created after injection a snapshot that Apply merges before reconciling (no getter refresh); a block created before
  injection takes one bounded getter refresh that adopts changed values as logical (see the state-block section).
- **D3D8/D3D7/D3D6:** a shared event-driven texture-stage-state owner provides the same logical/physical split and
  bounded one-time bootstrap. Returned DX6/7/8 device classes retain originals per vtable instead of assuming the
  bootstrap HAL class. D3D6/7 refresh config at EndScene and D3D8 at Present with a version fast path; D3D7/8
  state blocks follow the D3D9 snapshot model (section below). D3D7 uses slots 36/37 for
  Get/SetTextureStageState and slot 39 for ApplyStateBlock. D3D6 uses Device3 slots 39/40. Legacy MAG anisotropy is
  value 5 (`D3DTFG_ANISOTROPIC`), not MIN's value 3; using 3 as MAG selects flat-cubic filtering. D3D5 and older expose
  no anisotropic value in their pre-stage `D3DTEXTUREFILTER` render states, so there is no generic AF action to take.
  The mip override uses each API's real enum family: nearest is point MIN/MAG plus point MIP, bilinear is linear
  MIN/MAG plus point MIP, and trilinear is linear MIN/MAG plus linear MIP. It preserves `MIPFILTER=NONE` because an
  override cannot invent missing texture levels; safe mode also preserves non-material address modes. Pure DirectDraw
  2D exposes no mip sampler; DirectDraw-hosted sampler overrides are the D3D6/7 device paths.
- **OpenGL:** core bound-texture APIs, sampler objects, core DSA, and EXT DSA parameter variants share one policy.
  Texture image/compressed image/copy-image allocation, immutable storage, texture-view creation, and mip-generation
  entry points trigger reconciliation when mip availability can change. Texture/sampler binds reconcile each object
  once per context/config/object-generation; deletion advances the generation across contexts so reused names are not
  skipped. Actual level `base+1` allocation is queried before safe-mode AF is enabled. The implementation recognizes
  EXT/ARB anisotropy, clamps to `GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT`, protects compare/border/non-mip/point-filter
  objects, handles dimensional wrap relevance, and excludes rectangle, buffer, multisample, and proxy targets.
- **D3D12/Vulkan:** their existing creation-time implementations already match the no-draw-overhead requirement and
  were intentionally left unchanged. D3D12 covers dynamic and static samplers/root signatures; Vulkan requires the
  device feature, clamps to physical limits, and transactionally retries rejected modified descriptors. See
  `dx12-forced-af.md` and `graphics-overrides-and-frame-pacing.md`.

## D3D9 state blocks and slot drift (2026-09-24)

- **The "S6" defect** (audit-2 label, origin not recorded; derived from code): Apply re-read the physical state and
  reconciled from the logical values held *before* the Apply, so every tracked state the block set (address, filters,
  LOD bias, MAXMIPLEVEL) was written back to its old value. Without an override the first Apply before the first
  Present reset all samplers to the D3D defaults the shadow was seeded with. Set* calls between Begin/EndStateBlock
  (recorded, not applied by D3D9) were taken as applied.
- **Fix** - `dx9_state_block_sampler_policy.h` + `dx9_sampler_state_blocks.cpp`: every block created through CE's
  hooks carries a snapshot (logical + physical + texture metadata per covered sampler). CreateStateBlock(type)/Capture
  (state-block vtable slot 4, now hooked) copy the shadow; BeginStateBlock (device slot 60, now hooked) starts a
  recording in which Set* go into the snapshot unforced and never touch the shadow or a config reconcile. Apply
  merges the snapshot FIRST, then reconciles only covered samplers - no getter re-read. Blocks CE never saw fall back
  to the re-read and adopt changed values as logical. Inactive override: Apply does nothing. Coverage: ALL = samplers
  + textures, PIXELSTATE = samplers, VERTEXSTATE = none tracked. Snapshots are dropped at Reset; 2048 per device cap.
- **Slot drift re-arm** - `dx9_sampler_rearm_policy.h` + `dx9_hook_sampler_rearm.cpp`: a drifted SetTexture/
  GetSamplerState/SetSamplerState slot that stays on one foreign value for 120 presents gets ONE inline body hook on
  the implementation CE saved at install (only if that lies in the vtable owner's module). CE never rewrites the slot
  and never calls the foreign handler; every CE "original" of that implementation is retargeted to the trampoline
  (records, globals, later records via `TranslateD3D9SamplerOriginal`). `D3D9SamplerProcessingScope` makes nested
  body-detour hits pass straight through (no double processing, no re-entry into the shadow mutex).
- Logs: `State block %p applied from its snapshot`, `applied without a snapshot`, `sampler slot %d stayed on %p ...
  re-arming below it`, `re-armed below the foreign slot owner`, refusal/failure lines; summary counters
  `trackedStateBlockApplies`, `recordedStateBlocks`.

## D3D7/D3D8 state blocks (2026-09-24, audit 4)

- **Same defect as D3D9, plus a worse no-override case** (derived from code): Apply refreshed the physical values and
  reconciled from the pre-Apply logical shadow, undoing the block. With no override the shadow is not maintained
  (Set passes straight through) but `RegisterDevice` seeds it as initialized defaults, and after the first EndScene
  every Apply bootstraps it once - so each later Apply wrote the defaults or the previous Apply's re-read back.
- **Fix** - `legacy_d3d_state_block_policy.h` (pure; tested by a fake-device harness in
  `tests/test_legacy_d3d_state_block_policy.cpp`), `legacy_d3d_sampler_state_internal.h`,
  `legacy_d3d_sampler_state_blocks.cpp`. Inactive override: Apply writes nothing and drops the shadow. Otherwise
  Create/Capture copy the shadow (valid only while it tracks the application), Begin/EndStateBlock record Sets
  unforced, Apply merges the snapshot first and reconciles covered stages; unknown blocks re-read and adopt changed
  values. ALL/PIXELSTATE cover all tracked stage states (D3D7 has no ADDRESSW), VERTEXSTATE none. Snapshots are keyed
  by handle/token, dropped at RegisterDevice(new)/ResetDevice, capped at 2048 per device.
- **Hooks**: D3D7 slots 22/23/40/41/42 (`InstallD3D7StateBlockTrackingHooks`, ddraw_hook_detours_legacy_d3d.cpp),
  D3D8 slots 52/53/55/56/57 (`InstallD3D8StateBlockTrackingHooks`); indices pinned by the SDK-backed ABI tests.
  Begin is hooked only if End is (a Begin without End would latch "recording" and stop forcing). CE's own calls
  (`LegacyD3DInternalCallActive`, `dx8_hook_g_DX8StateHookBypassDepth`) are not tracked. D3D6 has no state blocks.
- Logs: `State block %lu applied from its snapshot`, `applied without a snapshot`, `applied with no sampler override
  active; nothing written`, `state-block tracking partial`.

## Performance and diagnostics

- D3D10, D3D12, and Vulkan have no bind/draw work after sampler creation.
- D3D9 and D3D6-8 do constant-size bookkeeping only on mutable state/config events. Driver getters are bootstrap-only
  except for a bounded Apply refresh of a block created before injection; config hashes are computed only when the shared config version
  changes; companion writes are skipped when the physical value is already correct.
- OpenGL has no draw interception. Bind interception queries an object only once per context/config/object generation;
  parameter/storage mutation events reconcile directly, and an already-correct filter or anisotropy value skips the
  redundant driver write.
- Transition, bootstrap failure, descriptor retry, and safety-decision logs are rate-limited. DX6-8 device
  registration reports the resolved AF/mip policy and maximum anisotropy. Per-stage AF logs identify allow,
  disabled-mip, point-filter, special-address, and unsupported-cap decisions even when no physical transition is
  needed. Per-stage mip logs independently report allow, disabled mip filtering, or protected addressing and the
  exact logical-to-target MIN/MAG/MIP values. The shared-memory publication summary includes both
  `sampler_override_mode` and `mip_mapping`, distinguishing a missing profile from a deliberately preserved sampler.
  Shutdown summaries report reconciliations, driver writes, bootstrap attempts, and OpenGL storage/parameter events.

## Verification

- Required incremental installed product build `0.1.5088` passed x64/x86 hooks, both Vulkan layers, CaptureEngine,
  MediaEngine, test applications, packaging, import closure, PE mitigations/architecture, and PDB checks.
- The exact-build `python build.py --no-build --run-tests --skip-updates --concise` gate passed the complete native
  suite and all five Python tool self-tests. Focused policy/config/source coverage exercises the shared filter triples,
  all four OpenGL mip MIN enums, Vulkan's independent mip/AF eligibility, legacy per-vtable/state-block wiring, and
  transactional fallback.
- The 2026-09-15 focused legacy audit passed SDK-backed D3D6/7/8 vtable and enum tests plus the mip eligibility and
  shared filter-policy suites. This proves the hook's numeric ABI assumptions and policy transforms, not a vendor
  driver's runtime behavior.
- Native runtime validation remains intentionally separate for the APIs listed below; the existing BioShock result
  establishes the D3D11 behavior, not legacy/OpenGL/Vulkan driver behavior.

## Open questions / stale-risk

- Native runtime validation is still required for D3D10, classic/Ex D3D9, D3D8, D3D7, D3D6, and representative OpenGL
  core/DSA/shared-context applications on both x86 and x64. Source and policy tests do not prove vendor-driver behavior.
- D3D6-8 state blocks still use the refresh-and-reconcile path (`legacy_d3d_sampler_state`), which reconciles from
  the pre-Apply logical state; the D3D9 snapshot design below has not been ported to them (open).
- State-block-heavy D3D9 games and a co-resident overlay that re-patches the sampler slots need a native run.
- D3D10 samplers created before a late injection cannot be enumerated or safely replaced without retaining a bind-time
  indirection. Creation-time interception is the intentional zero-steady-state-overhead tradeoff.
- OpenGL extension function pointers are driver/context supplied. Multi-ICD or unusual context migration remains a
  focused runtime-validation target even though capability state is context-local and reset after `wglMakeCurrent`.
