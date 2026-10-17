# DX11 Forced Anisotropic Filtering

Last cross-checked: 2026-10-17 (second round: x86 counters, per-thread shader memo, sampler-bind deferral; raw contexts dispatched through CE tables)

Primary sources:
- `hook/d3d11/d3d11_af_engine.{h,cpp}`, `d3d11_af_engine_slow.cpp`, `d3d11_af_engine_cost.cpp` (the engine)
- `hook/d3d11/d3d11_af_creation.{h,cpp}` (game sampler creation: policy, tag, per-thread memo)
- `hook/d3d11/d3d11_af_thread.{h,cpp}` (per-thread blocks: creation memo, shader memo, claim/reclaim)
- `hook/d3d11/d3d11_af_metadata.{h,cpp}` (shader plan and view class, object-owned)
- `hook/d3d11/d3d11_af_replacements.{h,cpp}` (replacement registry, lifetime bound to the game sampler)
- `hook/d3d11/dx11_hook_af_raw.cpp`, `dx11_hook_detours.cpp`, `dx11_hook_install.cpp`, `d3d11_context_slots.h`
  (raw context hooks), `d3d11_context_dispatch.{h,cpp}` (CE tables for runtime-owned context tables,
  see `d3d11-context-dispatch.md`)
- `hook/wrappers/d3d11_devicecontext_wrap.{h,cpp}` (CE's context wrapper)
- `hook/overrides/sampler_override_utils.h`, `hook/overrides/sampler_override/*.h` (policy)
- Tests: `tests/flow/test_flow_d3d11_sampler_af.cpp` (decisions) and `test_flow_d3d11_sampler_af_cost.cpp`
  (work: sampling, memos, ranged binds) and `test_flow_d3d11_sampler_af_deferral.cpp` (pending binds at
  every boundary), every scenario on both paths (deferral: also raw on slot hooks) on real WARP,
  sharing `tests/flow/d3d11_af_fixture.h`; `tests/test_d3d11_context_slots.cpp`,
  `tests/test_d3d11_context_wrapper_source.cpp`

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
  this path. d3d11.dll's own contexts dispatch through a CE table (their function table lives in
  the object and the runtime rewrites it); static tables (translator frontends) keep slot hooks
  (`d3d11-context-dispatch.md`).

Until 2026-10-17 each path had its own state machine and only one got each optimization:
the sampler-keyed choice memo landed in the raw path for HotS, while BioShock's wrapper path
kept per-slot variant lookups through sampler private data (`[D3D11 AF COST wrapper]`
estMsPerS 121.9, ~0.8 ms/frame at ~150 fps, 1.6 M binds/s, session `20261017_003122`). The
raw path in turn lacked what only the wrapper did (see invariants). The wrapper is used
whenever CE catches device creation, independent of the game: no exe or engine check exists.

## Engine shape

- `PixelState`: tracked views (+ per-slot AF class), logical samplers (game's), physical
  samplers (real context's) with incrementally kept `trackedMask`/`overriddenMask`, shader +
  metadata handle (borrowed from the held shader's private data), dirty mask, replacement epoch,
  a 128-entry sampler-keyed `SamplerChoiceCache` (holds references; LRU in a 4-probe window), the
  deferral-evidence fields, and single-writer `Count` counters the hook thread can read without a
  lock (pointer-width: 32 bits in the x86 hook, see Cost model).
- Shader binds (`OnShader`): the tracked shader again, a null shader or one the calling thread's
  shader memo holds is tracked in the first state step without any lookup (`TrackKnownShader`);
  only a shader new to the thread reads its private data (`AcquireShaderPlanOwner`, outside the
  state) and takes a second step. Only shaders with metadata are memoized. Until 2026-10-17 every
  PSSetShader read the private data first; the first memo (64 per context, holding shader
  references) still missed half of BioShock's switches (`20261017_215604`: misses == evictions).
- Per-thread blocks (`d3d11_af_thread.h`): one per thread that creates samplers or switches
  shaders, at most 32, never freed, reclaimed from exited threads through a `SYNCHRONIZE` handle.
  Each holds the creation memo (128 entries), a 1024-entry 8-probe shader memo and the creation
  counters. Neither memo holds a reference: an entry names an object only while it carries a CE
  private-data interface whose final release clears it from every block before the address can be
  reused - the registry sentinel for samplers, the metadata handle for shaders (its only owner is
  the shader; every other reference is taken and dropped while the shader is held).
- `Host`: binds to the real context below CE's interception, reads bound shader/view, acquires
  the device, creates a replacement without CE's CreateSamplerState override, names the path.
- `Access`: `Run(fn)` (state held for one step), `IsClean(epoch)` (lock-free per-draw exit:
  wrapper reads its member; raw sweeps a new epoch into every state once, then reads one
  counter) and `DefersSamplerBinds()` (wrapper true, raw while CE-dispatched; see Deferring).
- Raw slots (since 2026-10-17, replacing one process mutex + hash lookup per bind): slots are
  never freed, only recycled with a new incarnation; the owner re-checks the cached incarnation
  once it holds the slot and looks up again on a mismatch. Cross-thread work (view drop at a
  resize/host disconnect, epoch sweep, whole drop at shutdown) takes the map mutex, then each
  slot's flag; a state step never takes the map mutex. COM releases made during a step are
  queued in a `DeferredReleaseScope` and run after the slot is free; dropped states are moved
  out and destroyed with no lock held.
- Replacement registry (since 2026-10-17, replacing a process vector scanned linearly and
  released only at a config change): game sampler -> {epoch, replacement per AF verdict}, one
  mutex, slow path only. A reference-free sentinel in the game sampler's private data
  (`SetPrivateDataInterface`) erases the entry when the runtime destroys that sampler, so an
  entry never names a reused address; a new epoch (`AdvanceSamplerPolicyEpoch11Unlocked`) drops
  every entry. Cycle-free: within one epoch the policy is idempotent (a replacement needs no
  replacement), and stale epochs hold nothing. CE-created replacements carry a private-data mark
  with their epoch (`IsReplacementInEpoch`), checked only for samplers without a creation tag.
- `ReleaseTrackedShaderResources11` (resize, host disconnect, any thread) drops only views
  (the only references that block a resize) for contexts used since the previous drop and
  drops idle contexts whole (a destroyed context cannot hold its device alive). Until
  2026-10-17 it dropped every state whole: a bind tracked but not yet applied on another
  thread was lost, and CE forgot which replacement each slot held (a protected texture could
  keep AF until the sampler was rebound). `ReleaseAllTrackedContextStates11` is the whole
  drop, for CE's shutdown only.
- `Reconcile`: fast resolve under the state (memoized choice per logical sampler and verdict),
  view classes outside it, slow slots snapshotted and classified outside it, binds outside it
  (each run of adjacent slots as one ranged `PSSetSamplers`, since 2026-10-17), then one more
  step only if something was not recorded yet or the pass is sampled. Templates in the header;
  the x86 hook DLL has no LTO.
- Game sampler creation (`d3d11_af_creation.cpp`): the thread block's creation memo is keyed by
  policy hash and application descriptor (32-bit two-lane hash). An entry keeps the policy's
  result (`DX11Hook_ApplySamplerOverrides` is a pure function of `HashSamplerOverrideConfig` and
  the descriptor) and `tagged`, the object last confirmed to carry exactly that tag. A repeat
  creation skips the policy work and the `GetPrivateData` tag check when the runtime returns that
  object, and counts once (`kFullHits`; the snapshot derives creates/descHits/tagsKept/tagSkips).
  Creation reads `GetActiveSamplerConfigCached()`, not `AcquireSamplerPolicy11()`: the epoch and its
  freshness check belong to binds and draws. Threads without a block take the old path with shared
  atomics.

## Current invariants

- Forced AF-on is decided at pixel-sampler bind/draw reconciliation, never in
  `CreateSamplerState`, because creation has no shader/SRV pairing. AF-off, mip mapping and
  bias are applied at creation by `CreateSamplerWithCreationPolicy` (both paths), which tags
  the sampler with the policy hash and the application's original descriptor. The same policy
  later changes only AF (`ApplyD3D11BindTimeAF`); a changed policy starts from that original
  descriptor and applies the whole policy once (`ResolveD3D11SamplerBindDesc`). Untagged
  objects (created before CE attached, or an old hash-only tag) retain the whole-policy fallback.
  Comparing the final descriptor to the logical object's descriptor also covers removal of
  a setting when the new policy does not itself modify the original. The logical object is
  never rewritten. Rebase diagnostics are change-gated by replacement epoch.
  `FlowD3D11SamplerAF.*AFChangesDoNotReapplyCreationBias` covers 16x -> 4x -> off -> 16x
  plus changed/removed/restored offsets on both paths without a game rebind; both failed before
  the fix (bias 1 became 2 after an AF-only change). All 13 AF flow scenarios pass.
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
- A replacement lives as long as the game sampler it stands in for (registry entry), plus CE's
  per-context memo (128) and the bound slot; CE's share of the device's 4096 unique sampler
  objects follows the game's live samplers. BioShock Infinite creates a sampler per call for each
  streaming texture (MinLOD clamp, fade bias) and releases it; the old process list grew by one
  replacement per descriptor and crashed the game (session `20261017_135632`,
  `FlowD3D11SamplerAF.*PerDrawSamplersDoNotExhaustTheDevice` failed at sampler 3965 before).
  A new epoch marks every bound or overridden slot dirty, so a config change applies at the next
  draw even when the game binds nothing new (flow test).
- Runtime facts the design depends on (verified on WARP, same d3d11.dll layer as hardware): the
  runtime hands out the existing object for an identical descriptor; it zeroes `BorderColor`
  without border addressing and `MaxAnisotropy` without an anisotropic filter, so no unused
  descriptor field can mark a replacement; a released state object stops counting toward 4096
  only at the next flush (a game's Present), so the flow test flushes every 64 draws.
- Game sampler creation (`CreateSamplerWithCreationPolicy`, both paths, per call in BioShock)
  reads the thread's cached policy (`AcquireSamplerPolicy11`: config, epoch, hash) and skips the
  tag write when the object already carries the identical tag. Before 2026-10-17 it called
  `GetActiveGraphicsConfig()`, a process-wide mutex plus a by-value copy of the whole config.
- Creation memo (2026-10-17): an entry may vouch for an address only while that object carries
  the replacement registry's sentinel (`AttachSamplerSentinel`). The sentinel's final release (the
  runtime destroying the sampler) runs `ForgetCreatedSampler` before the address can be reused,
  so a new object at the same address is tagged again, never mistaken for the old one (untagged,
  it would get the whole creation policy again at bind time: the mip bias twice). A tag write
  bumps `g_tagWrites` and clears the object from every block; a confirmation that overlapped any
  tag write is not memoized (seq_cst load, tag, store, load: Dekker pairing with the invalidator's
  scan). Two descriptors folded into one object keep last-writer-wins tags, as before the memo.
  Flow tests `*DestroyedSamplersLeaveTheMemo` (WARP reused every address: destroyed=64/64) and
  `*RetagByAnotherThreadClearsTheMemo` fail with the respective invalidation removed.
- Slot masks: every write of `logical[]`/`physical[]` goes through `SyncSlotMasks`; flow builds
  recompute both masks at every read (`CheckSlotMasks`) and the AF fixture asserts no mismatch at
  the end of each scenario (27 of 31 failed with one update removed).
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
- Every raw detour reaches the runtime through `ResolveContextOriginal11`, which for a CE-dispatched
  context returns whatever the runtime's own table holds at that call (an `*_Amortized` variant
  included), never a pointer captured at install. `FlowD3D11RawContextHooks.*` cover draws after
  every `Flush` for the six drawable entry points, `SetMultithreadProtected` (binds, read-back,
  ClearState), `SwapDeviceContextState`, `ExecuteCommandList`, deferred contexts and a foreign copy
  of CE's table; all four failed with adoption disabled.

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
  (`LogCostWindow`), every 10 s with activity. `[D3D11 AF REPLACEMENTS]` (created, reused,
  failures, current/peak `entries`) and `[D3D11 SAMPLER CREATES]` (game creations, tag writes
  vs kept) follow in the same window when they changed; `entries` climbing toward 4096 means a
  lifetime regression. Raw steps cost a thread-local lookup and one
  uncontended exchange on the context's own flag; the wrapper takes no lock at all.

### Cost model (2026-10-17)

Baseline, session `20261017_193612` (build 0.1.7214, BioShock Infinite x86, wrapper path,
~200-255 fps): wrapper `estMsPerS` 73-173 (passes 22-61, binds 51-112) at 118-259k passes/s,
binds/s srv 340-720k, sampler 320-690k, shader 80-180k, `bindUs` 0.07, `passUs` 0.18-0.27
(~0.35-0.7 ms/frame); creates 1,574,523 per 10 s, tagKept 99.9%. Two costs were outside that
estimate: the shader-metadata lookup ran before the bind's sample started, and creations were
not sampled at all.

Second round, measured in `20261017_215604` (0.1.7216, same game, 200-255 fps): wrapper
`estMsPerS` 71-151 (passes 30-67, binds 25-63, shaders 10-21), `bindUs` 0.04, `passUs` 0.23-0.37,
`shaderUs` 0.11-0.14; `shaderSwitches` repeat ~70% of shader binds but memo misses == evictions
(hit rate ~56%); `deferral(savable)` 19-44 ms/s; `slotsPerCall` 1.13-1.18; creates 57-158k/s with
~100% memo hits, `ceUs` 0.15-0.21 against `runtimeUs` 0.13-0.23. That led to:

- **x86 counters.** The x86 hook targets i686 without SSE2: a 64-bit `std::atomic` load+store
  (every `Count` increment) is an x87 round trip through the stack with a store-forwarding stall,
  40 cycles against 0.7 for 32 bits (i686 micro benchmark, 2026-10-17); plain 64-bit atomic loads
  cost the same as 32-bit ones (4.5 cycles). `Count` now holds `CountValue` (pointer width) and
  every window goes through `CountDelta` (modulo the width). This also explained `passUs` rising
  with the deferral counters (three more increments per pass).
- **Creation memo hit**: one count instead of four, a two-lane 32-bit descriptor hash (52 -> 24
  cycles on i686), no epoch/freshness check, and `RefreshedThreadConfig` no longer reads the clock
  when shared config exists. x64 flow benchmark (create+release, WARP): raw 265 -> 215 cycles,
  wrapper 284 -> 223.
- **Shader memo** per thread, 1024 entries, no references (Engine shape).
- **Deferral** on wrapper contexts (below); since the dispatch fix also on CE-dispatched raw ones.

Where the game-thread time goes now, and the field that shows it:

| Work | Per call | Field |
| --- | --- | --- |
| SRV/sampler bind tracking | one state step; masks are field reads; wrapper: no pass | `bindUs`, `binds=`, `deferral(deferred/s)` |
| Shader bind | repeat: one compare; memo hit: up to 8 probes; new: private-data read + second step | `shaderUs`, `shaders=`, `shaderSwitches(repeat memo lookup evicted)` |
| Reconcile pass | memoized slot: 1 probe; adjacent binds: one ranged call | `passUs`, `passes=`, `rebinds(slotsPerCall)` |
| Game sampler creation | memo hit: hash + up to 4 probes, one count, no policy, no `GetPrivateData` | `[D3D11 SAMPLER CREATES] ceUs runtimeUs estMsPerS memo(desc tag raced threads)` |
| Draw (clean) | config check, epoch, dirty check, one thread-local increment (deferral evidence) | not sampled |

Sampling is 1 call in 32 per thread and stream (SRV+sampler binds; shader binds; passes;
creations); each timed interval carries about one QPC read of bias. Per-frame CE cost =
(`estMsPerS` of the cost line + `estMsPerS` of the creates line) / fps.

### Deferring sampler-bind reconciliation (wrapper and CE-dispatched raw contexts, since 2026-10-17)

A sampler bind used to reconcile its slots at once against a shader and views the game may still
change before the draw, which then reconciled again: 19-44 ms/s in BioShock (`deferral(savable)`).
Where the access says so (`DefersSamplerBinds()`) `OnSamplers` only tracks and marks the range
dirty; the next draw reconciles it. Wrapper contexts always defer (every wrapper draw entry point
calls `PreparePixelSamplersForDraw`); raw contexts defer while they dispatch through a CE table
(`RuntimeTable(context)`, one range check per bind), because then every draw made through the
context reaches CE's detours. Boundaries, each covered on all three paths (wrapper, raw, raw on
slot hooks) by `test_flow_d3d11_sampler_af_deferral.cpp`:
- `PSGetSamplers`: `TranslateToLogical` maps whatever the real slot holds of CE's last bind to the
  logical sampler, a pending unbind (logical null) included; before deferral it skipped null
  logical slots.
- `SwapDeviceContextState`: the wrapper and the raw detour (`RawAF_FlushPending`) flush pending
  binds (`FlushPending`, `PassKind::kFlush`, binds empty slots like a sampler-bind pass) before
  forwarding, so the saved state holds them; the scenario fails without the flush (raw mutation
  checked 2026-10-17).
- `ClearState`, `ExecuteCommandList(FALSE)`, `FinishCommandList(FALSE)` drop pending work with the
  tracking; `FinishCommandList(TRUE)` and `ExecuteCommandList(TRUE)` leave it pending for the next
  draw (no draw used it). CE's overlay restores physical state and leaves pending work pending.
- Raw contexts on slot hooks stay eager: in a table the runtime rewrites (adoption unavailable) the
  draw hooks are gone after the next flush (`d3d11-context-dispatch.md`), and static translator
  tables (DXUP, Heroes of the Storm) have no flow fixture proving their draw coverage yet. A missed
  draw costs an eager context a stale AF decision; a deferring one would lose the game's bind.
- Residual risk on CE-dispatched contexts: draws made through function pointers the game captured
  before CE adopted the context (late injection) bypass CE, and a deferred bind would then not
  reach the context. No such engine is known; nothing detects it.

Flow tests drive the raw path's cross-thread windows deterministically through flow-only probes
in the raw step (`CEFlow_SetRawAFProbes`): a whole drop between lookup and slot (stale incarnation
retried), a view drop between a bind's tracking and binding steps (fails with the old whole drop:
the bind is lost), and the resize semantics (CE's view reference gone, idle context dropped whole).

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

## Rejected approaches

- A process-wide replacement list kept until the config changed: unbounded with per-draw game
  samplers (the BioShock crash above), and every lookup scanned it.
- Marking replacements through an unused descriptor field (border color): the runtime zeroes it,
  so replacements still deduplicate into game samplers (`stamped=0` on WARP).
- A strong private-data link from the game sampler to its replacement: with deduplication a
  game's AF16 and AF4 samplers can each be the other's replacement across a 4x/16x toggle, and
  a stale link from the older epoch then closes a reference cycle that never dies.
- A shader memo holding shader references (2026-10-17, first round, 64 per context): bounded
  retention of released shaders and too small for BioShock; a larger referencing memo would keep
  more released shaders alive. The metadata handle's destructor makes references unnecessary.
- Deferring sampler binds on raw contexts whose draw hooks the runtime can remove (slot hooks in a
  runtime-owned table).
- A creation memo holding sampler references (2026-10-17): every held sampler counts toward the
  device's 4096, and a thread-local memo's references outlive its thread (no TLS destructor in
  an injected DLL) and keep the device alive. Invalidating every entry on any sampler death (a
  global generation) instead of the sentinel would clear the memo about once per frame in
  BioShock (~157 tag writes/deaths per second).

## Open questions / stale-risk

- **Raw draw hooks lost after the first flush: fixed 2026-10-17, hardware validation pending.**
  d3d11.dll rewrote all seven draw entries after every `Flush`/`ExecuteCommandList`, and with
  `SetMultithreadProtected`/`SwapDeviceContextState` also the bind, read-back and reset hooks; CE
  now dispatches such contexts through its own table (`d3d11-context-dispatch.md`, evidence there).
  Rejected: body hooks on the runtime's draw functions (the slot holds one of several variants per
  threading mode, and the defaults change when multithread protection does) and re-patching the
  slot (the runtime rewrites it during calls CE does not see, and a captured `*_Amortized` or
  default "original" skips or repeats the runtime's pending work). Expect `[D3D11 DISPATCH] adopted`
  in a late-injected native D3D11 game's hook log.
- Hardware validation pending (2026-10-17, second round): BioShock Infinite (wrapper) against
  `20261017_215604`. Expect `shaderSwitches` lookups far below `shader=` (1024-entry thread memo),
  `deferral(deferred/s)` near the sampler-bind rate with `eager/s` 0 and `passes/s` falling by about
  the old `savable/s`, `passUs` back near or below the 0.18-0.27 of `193612` (x86 counters), creates
  `ceUs` well below `runtimeUs`, and the perf CSV's new DX11 columns (`overlay_us`, `capture_us`,
  `present_call_us`, `pre/post_present_us`, `overlay_gpu_us`) separating CE's present-path work from
  the driver's. The old CSV's DX11 `total_us` (median 304 -> 417 us between `193612` and `215604`)
  was the real Present call in both sessions (the wrapper's own row was dropped by the row dedup), so
  that rise is the driver/compositor's, not CE's present-path work; both sessions recorded
  `av1_nvenc` at 120 fps. Heroes of the Storm (raw, eager) not run since the first round. The
  creation policy's diagnostics (`mipBias`, `mipOverride` in the shutdown summary) count memo
  misses only.
- Hardware validation pending (2026-10-17): BioShock Infinite with AF 16x through a save load -
  no crash, `[D3D11 AF REPLACEMENTS] entries` bounded, texture streaming at normal pace,
  `[D3D11 AF COST wrapper]` estMsPerS, and no Blackwell corruption (classification unchanged).

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
