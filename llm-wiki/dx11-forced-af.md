# DX11 Forced Anisotropic Filtering

Last cross-checked: 2026-10-17

Primary sources:
- `hook/d3d11/d3d11_af_engine.{h,cpp}`, `d3d11_af_engine_slow.cpp`, `d3d11_af_engine_cost.cpp` (the engine)
- `hook/d3d11/d3d11_af_metadata.{h,cpp}` (shader plan and view class, object-owned)
- `hook/d3d11/dx11_hook_af_raw.cpp`, `dx11_hook_detours.cpp`, `dx11_hook_install.cpp`, `d3d11_context_slots.h`
  (raw context hooks)
- `hook/wrappers/d3d11_devicecontext_wrap.{h,cpp}` (CE's context wrapper)
- `hook/overrides/sampler_override_utils.h`, `hook/overrides/sampler_override/*.h` (policy)
- Tests: `tests/flow/test_flow_d3d11_sampler_af.cpp` (every scenario on both paths, real WARP),
  `tests/test_d3d11_context_slots.cpp`, `tests/test_d3d11_context_wrapper_source.cpp`

## Summary

D3D11 forced AF is a pixel-shader/SRV-aware bind replacement, run by one engine
(`ce::d3d11_af`) for both ways CE reaches a context:

- **Wrapper path:** the game created its device through CE's `D3D11CreateDevice` hook and
  calls `CWrapD3D11DeviceContext`. Its state is a member; no lock (a context is
  single-threaded by contract). BioShock Infinite takes this path.
- **Raw path:** CE never wrapped the context (late injection, `D3D11CreateDeviceAndSwapChain`
  callers that fetch the context from the device, translators like DXUP, D3D11On12). Vtable
  hooks reach the context's slot (`dx11_hook_af_raw.cpp`) through a 4-entry thread-local
  cache and hold only that slot's spin flag for one state step. Heroes of the Storm takes
  this path.

Until 2026-10-17 each path had its own state machine and only one got each optimization:
the sampler-keyed choice memo landed in the raw path for HotS, while BioShock's wrapper path
kept per-slot variant lookups through sampler private data (`[D3D11 AF COST wrapper]`
estMsPerS 121.9, ~0.8 ms/frame at ~150 fps, 1.6 M binds/s, session `20261017_003122`). The
raw path in turn lacked what only the wrapper did (see invariants). The wrapper is used
whenever CE catches device creation, independent of the game: no exe or engine check exists.

## Engine shape

- `PixelState`: tracked views (+ per-slot AF class), logical samplers (game's), physical
  samplers (real context's), shader + metadata handle, dirty mask, replacement epoch, a
  128-entry sampler-keyed `SamplerChoiceCache` (holds references; LRU in a 4-probe window), and
  single-writer `Count` counters the hook thread can read without a lock.
- `Host`: binds to the real context below CE's interception, reads bound shader/view, acquires
  the device, creates a replacement without CE's CreateSamplerState override, names the path.
- `Access`: `Run(fn)` (state held for one step) and `IsClean(epoch)` (lock-free per-draw exit:
  wrapper reads its member; raw sweeps a new epoch into every state once, then reads one
  counter).
- Raw slots (since 2026-10-17, replacing one process mutex + hash lookup per bind): slots are
  never freed, only recycled with a new incarnation; the owner re-checks the cached incarnation
  once it holds the slot and looks up again on a mismatch. Cross-thread work (view drop at a
  resize/host disconnect, epoch sweep, whole drop at shutdown) takes the map mutex, then each
  slot's flag; a state step never takes the map mutex. COM releases made during a step are
  queued in a `DeferredReleaseScope` and run after the slot is free; dropped states are moved
  out and destroyed with no lock held.
- `ReleaseTrackedShaderResources11` (resize, host disconnect, any thread) drops only views
  (the only references that block a resize) for contexts used since the previous drop and
  drops idle contexts whole (a destroyed context cannot hold its device alive). Until
  2026-10-17 it dropped every state whole: a bind tracked but not yet applied on another
  thread was lost, and CE forgot which replacement each slot held (a protected texture could
  keep AF until the sampler was rebound). `ReleaseAllTrackedContextStates11` is the whole
  drop, for CE's shutdown only.
- `Reconcile`: fast resolve under the state (memoized choice per logical sampler and verdict),
  view classes outside it, slow slots snapshotted and classified outside it, binds outside it,
  then one more step only if something was not recorded yet. Templates in the header; the
  x86 hook DLL has no LTO.

## Current invariants

- Forced AF-on is decided at pixel-sampler bind/draw reconciliation, never in
  `CreateSamplerState`, because creation has no shader/SRV pairing. AF-off, mip mapping and
  bias are applied at creation by `CreateSamplerWithCreationPolicy` (both paths), which tags
  the sampler; a tagged sampler later changes only the AF decision (`ApplyD3D11BindTimeAF`),
  an untagged one (created before CE attached) gets the whole policy at bind time.
- Pixel-shader metadata is stored as a private COM interface on the shader
  (`ShaderMetadataHandle`): AF-candidate mask, texture-to-sampler masks, compact sampler-to-
  texture lists, built once at CreatePixelShader. Contexts hold the handle, not a copy.
- `sample`, `sample_b`, `sample_d` are AF-safe; `sample_l`, comparison and unrecognized
  explicit sampling are unsafe, and any unsafe use of a sampler register blocks it.
- Every texture register paired with a candidate sampler must have a bound eligible SRV.
  Missing, unsafe or single-visible-mip inputs keep the logical sampler.
- SRV class is structural and immutable (dimension, typed format, mip visibility, sampling,
  depth capability), stored on the view as private data; the wrapper classifies at creation,
  either path lazily on first need.
- Replacements live in the process cache keyed by (device, desc), cleared on a config change
  (new epoch). A new epoch marks every bound or overridden slot dirty, so a config change
  applies at the next draw even when the game binds nothing new (flow test).
- With no sampler override configured, the game's PSSetSamplers is forwarded verbatim unless
  a replacement of CE's sits in the range; reconciliation then only restores CE's slots.
- `PSGetSamplers` returns the game's own sampler on both paths (raw: slot 75 hook). CE's own
  overlay (`dx11_hook_g_InOverlayRender`) reads and restores physical state untranslated.
- `ClearState`, `ExecuteCommandList(..., FALSE)`, successful `FinishCommandList(FALSE)` and
  `SwapDeviceContextState` drop tracking on both paths (raw: slots 110, 58, 114, 131; 131
  only on a Context1 vtable). Before 2026-10-17 the raw path saw only Execute: after an
  unseen ClearState it kept the replacement as physical and swallowed the game's rebind of
  the same sampler, leaving the real slot empty (flow test `RawClearStateDoesNotSwallow...`).
- A new deferred context drops any state at its address (a destroyed context's state could
  carry over).
- Wrapper-to-real forwarding stays inside `DX11WrapperForwardingScope`, so the raw hooks pass
  through and never run a second state machine for the same call.
- Raw slot indices live in `d3d11_context_slots.h`; `D3D11ContextSlotsTest` checks each
  against the compiler's own vtable call.

## Safety and coverage policy

- Safe mode requires linear min/mag filtering. Aggressive mode may promote ordinary point
  min/mag samplers. Both reject border addressing, comparison and min/max reduction filters,
  and fixed/invalid mip ranges. Clamp and mirror-once addressing are allowed.
- Eligible views: single-sample mipmapped Texture2D, Texture2DArray, TextureCube,
  TextureCubeArray with typed filterable color/material formats (BC1-BC7, UNORM/SNORM, float).
- Typed integer, depth, typeless/problematic, unknown/non-color, multisampled, non-2D-family
  and single-visible-mip views stay blocked; a depth-stencil bind capability is a hard veto.

## Performance

- Clean draws: a config-version check, the epoch and a dirty check (raw: one counter). No
  lock, descriptor query, COM call or driver call.
- A shader/view/sampler change dirties only affected sampler bits; resolved from the memo,
  a slot costs one probe; only genuinely changed physical samplers are bound.
- Cost line: `[D3D11 AF COST]` (raw) and `[D3D11 AF COST wrapper]`, same format
  (`LogCostWindow`), every 10 s with activity. Raw steps cost a thread-local lookup and one
  uncontended exchange on the context's own flag; the wrapper takes no lock at all.
- Flow tests drive the cross-thread windows deterministically through flow-only probes in
  the raw step (`CEFlow_SetRawAFProbes`): a whole drop between lookup and slot (stale
  incarnation retried), a view drop between a bind's tracking and binding steps (fails with
  the old whole drop: the bind is lost), and the resize semantics (CE's view reference gone,
  idle context dropped whole).

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
