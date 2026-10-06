# Architecture debt: current state and implementation plan

Last source audit: 2026-10-05; next-wave inventory started 2026-10-06 at `3563155d`.
Core implementation baseline: `e83eabd2` / product 0.1.6989.
The subsequent privacy workflow commit `6b8249e8` adds commit checks, not product architecture.
Canonical version: this wiki page. `temp/refactor.md` is an identical working copy of this revision.
This document supersedes the completed sections 1-10 of the old temporary refactor plan.

## Execution status (2026-10-06)

- D0 active: every first-party subsystem inventoried; bounded operation traces and repeated coupling
  evidence recorded in [architecture-inventory.md](architecture-inventory.md). Remaining full lifecycle
  audits and child replacement/finalization traces are explicit there; clean/IPC fuzz gates passed.
- D1 partial: NGX core fake and two real-hook lifecycle/OFF/ON scenarios pass focused/17-flow gates;
  all three production mutants are detected. Clean 0.1.7006 and final verification 0.1.7007 passed.
  Contracts and cold-start accounting
  finding: [NGX flow lifecycle](frame-generation/ngx-flow-lifecycle.md). Foreign interposer, two-vtable
  and SDK unload/retirement coverage remain required. D2-D12 pending; apply D9/D12 inside each slice.
- The first queue ownership slice must characterize exact-vtable original resolution, coherent cache
  publication and retirement before migrating install/forward consumers.
- Baseline no-build reuse refused the stale unit-test link manifest at 0.1.7004; no native test ran.
  D0 clean verification 0.1.7005 passed products, native/15 FG flows, Python, ASan/UBSan and lint ratchets.
  All four parser fuzz targets passed a bounded 10-second-per-target run; hardware/application checks remain pending.
- Temporary directory literally inventoried: only this working copy remains. The earlier 32-file
  cleanup is already complete; do not repeat it or treat future diagnostic files as disposable.

## Scope and evidence

The completed core refactor substantially improves ownership and behavioral protection. It does not
finish project-wide architecture. The next work should remove obligations from callers, reduce the
number of places that can break an invariant, and make production transactions directly testable.
Moving fields into a public struct, adding forwarding methods, or splitting large files alone does
not meet that goal. A deep module owns a meaningful policy and lifetime behind a small contract.

This is a source-backed audit of representative controller, IPC, media, DX12, SDK and build paths.
Other graphics APIs, configuration, service, installer and override paths need the explicit inventory
in D0 before their residual debt can be called fully assessed. Findings below are architectural
coupling evidence, not proof of a current user-visible bug or a measured performance regression.

Use [refactor-contracts.md](refactor-contracts.md) for completed ownership, exact source/test anchors,
glossary, commit history and pending verification. Use [refactor-roadmap.md](refactor-roadmap.md) for
the older layout/logging/library program; its numbered waves are historical, not this execution order.

## Completed work: preserve and extend

| Area | Implemented boundary | Remaining boundary |
| --- | --- | --- |
| Controller recording | Command-thread `RecordingSession`; start/stop/toggle/reconciliation; effects separated from policy | Child supervision, configuration and application effects outside recording |
| Inject control IPC | Scoped validated intent, notification and health operations; classified failures | Inventory remaining direct control mappings; transport leases remain specialized |
| Media submission | Separate inject/D3D11 adapters; accepted/deferred/rejected outcomes; candidate completion | Source selection, scheduling, session teardown and worker coordination |
| Media ABI/timing | Additive result exports; strict loader cleanup; explicit units and committed anchors | Broader ABI verification, audio reset/drain and mux finalization ownership |
| PostSL | Activation, confirmation epoch, callback admission, queue roles and deferred GPU retirement | Surrounding handover evidence, other SDK lifecycles and residual routing state |
| Native DX12 draw | Shared production transaction seam; exact backbuffer release; private admission; named draw/capture phases | Backend resource groups, fence/allocator ownership and large frame context |
| Verification | Native regressions; 15 real-hook WARP FG scenarios; six draw mutations detected | NGX/foreign-overlay harness coverage; real games, A/V and hardware evidence |

Production anchors include [recording_session.h](../captureengine/app/recording_session.h),
[inject_control_channel.h](../common/ipc/inject_control_channel.h),
[frame_submission.h](../captureengine/media/frame_submission.h),
[submission_transaction.h](../mediaengine/engine/submission_transaction.h),
[postsl_lifecycle.h](../hook/d3d12/postsl_lifecycle.h),
[postsl_queue_owner.h](../hook/d3d12/postsl_queue_owner.h), and
[overlay_draw_transaction.h](../hook/d3d12/overlay_draw_transaction.h).

The new DLL exports are `MediaEngine_SubmitFrameWithResultV1`,
`MediaEngine_SubmitFrameD3D11WithResultV1`, and `MediaEngine_RepeatLastFrameWithResultV1`.
Their result layout is 40 bytes, aligned to 8. Preserve legacy positional/descriptor exports,
descriptor layouts and the deferred query. Acceptance does not promise packet emission or GPU completion.

The final core gate passed x64/x86 product compilation, binary/package checks, native units and all
15 FG flow scenarios and produced the 0.1.6989 installer. Later documentation gates are recorded in
`build/verification/latest_summary.txt`; they do not extend hardware coverage. The complete core work
is 15 local commits from `7490bfc4` through `e83eabd2`. Do not reopen completed work merely to rename it.

## Measured locality and remaining coupling

Repeat the same investigations, including new owners and adapters. Tokens below are characters / 4,
not a model tokenizer; the precise file sets and computation are in refactor-contracts.md.

| Investigation | Original full source context | Current full source context | Current policy entry context |
| --- | --- | --- | --- |
| Pending stop and next start | 1,509 lines / 16,473 tokens | 1,705 / 17,755 | 254 / 2,820 |
| FG handover and stale epoch | 2,886 / 40,396 | 3,302 / 44,884 | 356 / 4,120 |
| Encode failure, leases and timeline | 2,015 / 28,517 | 2,271 / 31,369 | 149 / 1,493 |

Full implementation context grew. Caller decisions became more local; total source-reading reduction
has not been demonstrated. The next waves must measure both policy entry cost and full implementation
cost, including test adapters, rather than hiding complexity in new headers or umbrella includes.

Concrete debt observed on 2026-10-05:

- [dx12_hook_process_session.h](../hook/d3d12/dx12_hook_process_session.h) keeps dozens of phase facts,
  route/focus flags, raw resource pointers and preparation results. Privacy is improved, but the
  transaction still requires broad knowledge of preparation, SDK routing and lifetime rules.
- [dx12_hook_types.h](../hook/d3d12/dx12_hook_types.h) retains 32 writable inline reference aliases.
  They include ECL dispatch/cache state, capture generation and surrounding PostSL/coverage state.
  This count does not include state already privately owned by the new PostSL owners.
- [streamline_hook_internal.h](../hook/streamline/streamline_hook_internal.h) retains 115 writable
  inline reference aliases to grouped state. Grouping alone still lets unrelated consumers mutate it.
- [dx12_hook_helpers.cpp](../hook/d3d12/dx12_hook_helpers.cpp) initializes an untracked queue's ECL
  lookup from the first global original. Installation and recursion forwarding also use global
  fallbacks. Provenance across different vtables/wrappers needs a complete transaction and tests.
  This is an unresolved type/provenance hazard, not a reproduced crash in the existing flow scenarios.
- [media_main_internal.h](../captureengine/media/media_main_internal.h) has 63 `extern` declarations:
  session flags, queues/caches, source handover, WGC workers, privacy and health coordination remain
  externally mutable despite the completed submission adapters.
- [mediaengine_internal.h](../mediaengine/engine/mediaengine_internal.h) exposes mutable encoder,
  audio source, configuration, FFmpeg output, epoch reset, drain and synchronization state through a
  broad engine class. Submission timing ownership does not yet encapsulate those lifecycles.
- [ffx_hook_internal.h](../hook/ffx/ffx_hook_internal.h) exposes per-context routing maps, callback
  bridge state, hooks and teardown facts. Streamline and NGX retain their own broader SDK lifecycles.
- Direct quoted includes across first-party product sources: `main_internal.h` 23 files,
  `dx12_hook_internal.h` 71, `media_main_internal.h` 28, `mediaengine_internal.h` 18. Counts exclude
  tests and measure include sites by file, not transitive dependencies or interface quality.
- [build.py](../build.py) concatenates 22 ordered fragments and executes them in shared globals.
  File splitting preserves a large implicit namespace and import/monkeypatch/order dependencies.
- 142 native test source files contain selected source-reading helper names. This is a file count,
  not a test count or percentage. Many protections require spelling/layout rather than behavior;
  independent hook/security/source constraints remain useful and must not be removed wholesale.
- Some wiki topics exceed 100 KB even with modest line counts. Line ceilings alone do not make the
  architectural context economical. Historical narratives, stale terminology and duplicated rules
  still obscure the current contracts.

## Non-negotiable implementation contract

Preserve process topology and public contracts. Keep source-specific capture and scheduling behavior.
No feature disabling, game-specific workarounds, artificial delays, per-present allocation/virtual
dispatch, extra frame copies, or unnecessary overlay suspension. Keep native DX12; no D3D11On12.
Do not add extra locks, COM reference churn or dispatch lookups to hot paths merely to hide state.

Keep accepted settings, runtime presence, device identity, generation evidence, route ownership and
visible FG status separate. One application present may have several physical outputs with distinct
routes. A single universal FG mode or unconditional exclusive route is an invalid simplification.

Preserve existing lock order, cancellation-before-render-drain, reentry, callback admission, source
leases, GPU completion, capture ordering, fences, cache promotion and release points. New abstractions
must state which thread owns each operation and what evidence permits retirement. Independently read
atomics are not a transactional snapshot. Strong types encode actual units/proofs, not decorative names.

Preserve QPC/source time versus scheduled output time, inject encoded-duration commitment versus WGC
scheduled live timing, and audio 100 ns timestamps versus sample indices. CFR overload policies,
privacy-blackout behavior, resampling, all codecs/tracks and final A/V endpoint rules remain intact.

## Execution order and commit discipline

Start with D0 and the needed D1 harness coverage. D2 precedes routing/resource changes that depend on
queue provenance. D3 and D4 build on each other in small slices. D5 precedes D6. D7 consumes the SDK
coverage from D1 and the admission/provenance contracts from D2-D3. D8 can proceed after D0 independently
of graphics waves. D9 and D12 are acceptance work inside every wave. D10 starts with an audit, then
prioritizes demonstrated ownership gaps. D11 is independent after build characterization in D0.

Each wave below is a sequence of independently verified local commits, never one giant change.
Separate mechanical moves from changed policy. Choose the first complete invariant with the smallest
reader/writer/callback set; do not migrate all globals at once. Names in the proposed contracts below
are working names: settle them from the source inventory, not by creating empty facade classes.

Before each implementation commit: update CHANGELOG/wiki, inspect task ownership and diff, validate
changelog, run the applicable focused tests and closing product/package gate, review/scan staged
content and the message, commit locally, then inspect/scan the exact commit and metadata. No push.
No commit is complete while migrated aliases, temporary forwarding cycles or duplicated policy remain.

## D0 - Establish the next-wave evidence and close validation gaps

1. Inventory every module via repo-map.md: controller, inject, media, common contracts, each graphics
   backend, SDK interception, overrides, service, installer and build/test tooling. Mark areas not read.
2. For stop/restart, FG handover, encode failure, resize/device loss, child replacement and finalization,
   trace actual readers/writers, lock acquisition, callbacks, resets, release and unload paths.
3. Record authoritative state, units, request/child/channel/epoch identities, ownership, thread affinity,
   synchronous reentry and asynchronous work. Separate observed facts from inferred evidence.
4. Recompute include sites, writable aliases, obsolete forwarding layers and complete source-context
   sets. Include new owners; list dependencies instead of reporting a cosmetic file-line reduction.
5. Repeat current native and all FG flow baselines. Preserve case names and failures in durable memory.
6. Run/record outstanding shared-ABI clean verification and relevant IPC fuzz gates before declaring
   that boundary fully verified. Record any failures as defects with reproductions, not baseline debt
   to suppress. Existing game/A/V/hardware checks stay explicitly pending until observed.

Commits: evidence and characterization; then each independently discovered fix with regression tests.
Exit: a source/test-backed ownership matrix, reproducible baselines, and an exact pending-check list.
Reject: a repository-wide numeric maintainability score or a complete-audit claim based on samples.

## D1 - Expand real-hook lifecycle and coexistence coverage

Anchors: [tests/flow](../tests/flow), [flow_test_support.h](../tests/flow/flow_test_support.h),
[nvngx_hook_internal.h](../hook/ngx/nvngx_hook_internal.h),
[ngx_feature_lifecycle.h](../hook/ngx/ngx_feature_lifecycle.h).

1. Add a minimal NGX fake using the actual intercepted export/parameter/feature lifecycle contracts.
   Exercise create, evaluate, settings changes, release and reactivation through the production hook.
   Keep existing small NGX lifetime policies; the fake must invoke orchestration around them.
2. Add a controlled foreign Present interposer above/below CE. Cover supported installation orders,
   re-hook, nested presentation and removal with callbacks in flight. Use test-only identities and
   genuine forwarding chains; do not implement production rules tied to a vendor filename.
3. Add two queue implementations/vtables and wrapper/native combinations for provenance scenarios.
   Add resize/replacement/device-loss cases only where deterministic WARP orchestration can model them.
4. Assert physical-present accounting, coverage and owner identity, current visible FG status,
   capture ordering, callback/COM balance, retirement completion and debug-layer error absence.
5. Supply explicit readiness, fake clocks/barriers and bounded stop conditions. Test fixture failure
   must shut down hosts and children and report unreleased callbacks/resources. No test sleeps.

Commits: NGX fake/lifecycle scenarios; foreign interposer scenarios; queue/resource failure scenarios.
Exit: each new scenario proves the relevant defect would fail when deliberately reintroduced.
Reject: stubbing hook decisions in the flow host or counting an application present as every output.
Limit: WARP fakes cannot establish actual NGX/FFX behavior, foreign-overlay compatibility or performance
on user hardware. Retain the separate game validation matrix.

## D2 - Own queue dispatch provenance and hook retirement

Anchors: [dx12_hook_ecl_install.cpp](../hook/d3d12/dx12_hook_ecl_install.cpp),
[dx12_hook_ecl_forward.cpp](../hook/d3d12/dx12_hook_ecl_forward.cpp),
[dx12_hook_helpers.cpp](../hook/d3d12/dx12_hook_helpers.cpp),
[dx12_hook_types.h](../hook/d3d12/dx12_hook_types.h).

1. Characterize installation, duplicate discovery, vtable replacement, original resolution, recursion
   breaking, unhook and module retirement. Document which queue/wrapper a saved target can legally accept.
2. Introduce one private registry/owner for installation publication, dispatch evidence and retirement.
   Resolution returns a target with sufficient provenance, or a classified inability to establish it.
   Ordinary callers must not choose among globals, per-vtable maps and SDK heuristics themselves.
3. Replace the untracked-vtable first-global fallback with a proven resolution path derived from the
   actual interception chain. Do not blindly cast/call another queue implementation or disable overlay
   drawing on an unsupported classification. Fix missing discovery/provenance at its source.
4. Preserve the established common-path cache and dispatch cost. Cache publication must associate
   identity and target coherently; retirement must invalidate evidence before callable code disappears.
5. Migrate install/forward/render consumers and remove migrated aliases/maps from the public header.
   Keep wrapper/device identity evidence explicit; device equality alone is insufficient route proof.

Tests: two distinct vtables, cache changes, stale/replaced targets, duplicate install/uninstall,
reentrant forwarding, callback across retirement, genuine wrapper/native paths and all FG flows.
Mutation proof: wrong global original, stale cache pair and premature retirement each fail.
Commits: characterization; provenance owner/lookup; installation/retirement migration.
Exit: all dispatch targets have a stated admissible identity and no caller repeats fallback policy.
Reject: a map wrapper whose callers still select originals or an extra global fallback of unknown type.

## D3 - Own handover evidence and physical-output route decisions

Anchors: [dx12_hook_process_session.h](../hook/d3d12/dx12_hook_process_session.h),
[dx12_hook_types.h](../hook/d3d12/dx12_hook_types.h),
[postsl_lifecycle.h](../hook/d3d12/postsl_lifecycle.h), and the stage preparation/FG transition siblings.

1. Inventory coverage ledger, explicit-off/warm-resume/cooldown/startup facts, presentation ownership,
   runtime progress and accepted settings. Classify each by scope: SDK, device, swapchain, epoch or output.
2. Extend existing lifecycle owners only where the same invariant applies; introduce a separate route
   evidence owner for independent facts. Avoid a single container for all DX12/FG state.
3. Feed observations with correlation identity and timestamps. Return a decision for the particular
   output, including required proven route/queue evidence and reason, without transferring SDK policy.
4. Make activation, invalidation, output confirmation and native return complete transactions. Keep
   stale evidence rejection and uncertainty when no available wire/runtime evidence proves attribution.
5. Migrate one handover direction first, then startup/native return, then multi-output composition.
   Remove caller mutations and redundant boolean combinations after every completed slice.

Tests: off/FSR/DLSS all directions, settings without runtime, runtime without enabled FG, explicit off,
stale epoch, reordered observation, synthetic progress, late callback, repeated cleanup and several
physical outputs per app present. Assert no missing/double draw and no unnecessary route suspension.
Commits: evidence contract; startup/native return; handover and multi-output consumers.
Exit: the caller supplies observations and executes the decision; it does not reconstruct route policy.
Reject: collapsing configuration, runtime, route and visible status into one mode enumeration.

## D4 - Own native DX12 backend resources and shorten frame orchestration

Anchors: [overlay_draw_transaction.h](../hook/d3d12/overlay_draw_transaction.h),
[frame_render_admission.h](../hook/d3d12/frame_render_admission.h),
[dx12_hook_process_session_draw_resources.cpp](../hook/d3d12/dx12_hook_process_session_draw_resources.cpp),
[dx12_hook_process_session_draw_record.cpp](../hook/d3d12/dx12_hook_process_session_draw_record.cpp),
[dx12_hook_process_session_draw_submission.cpp](../hook/d3d12/dx12_hook_process_session_draw_submission.cpp).

1. Map backend initialization, descriptor heaps, per-buffer allocators, command list, fence/event,
   backbuffer acquisition, capture publication, resize/device loss and destruction. Identify resource
   groups with one compatible creation/reset/retirement policy, not just the same struct prefix.
2. Introduce private resource owners for those groups with prepare/record/submit/retire operations.
   Extend the production transaction seam with statically bound adapters; production remains direct.
3. Replace optional flag combinations with short-lived phase results that carry established resource
   validity, queue identity, backbuffer lease and required fence evidence. Do not expose SDK internals
   or a giant shared frame context through the operation contract.
4. Keep the normal backbuffer release point and destructor cleanup backstop. Carry capture ordering
   explicitly across submit/publication. Preserve independent FSR composition and runtime presentation.
5. Reduce FrameProcessSession to transaction orchestration. Remove state that no longer crosses phases;
   metrics finish at the private boundary. Do not rebuild completed named draw phases for appearance.

Tests: existing success/recovery/release tests plus initialization partial failure, each allocator/list
failure, submission exit, stale fence, resize, device loss and destruction. Existing six draw mutants
must remain detectable; add wrong capture ordering and early resource retirement mutations.
Commits: backend/resource lifetime; allocator/fence transaction; phase results/capture orchestration.
Exit: each resource has one creation/retirement owner; draw callers cannot reset backend readiness or
release a borrowed buffer. Session fields contain only facts genuinely shared between remaining phases.
Reject: a renamed large context, per-frame virtual operation interfaces, allocations or extra COM churn.

## D5 - Own media coordination without homogenizing sources

Anchors: [media_main_internal.h](../captureengine/media/media_main_internal.h),
[media_main_encoder_session.h](../captureengine/media/media_main_encoder_session.h),
[frame_submission.h](../captureengine/media/frame_submission.h),
[candidate_completion.h](../captureengine/media/candidate_completion.h), and encoder/start/worker siblings.

1. Inventory session start/stop, WGC/inject workers, queue/cache ownership, source availability,
   handover, scheduling debt, privacy, health reporting and encoder shutdown. Record lock order and
   exactly which producer retains/releases each source lease.
2. Introduce a media-session coordination owner for session lifecycle and worker admission. Keep
   source-specific producer state behind WGC and inject operations; transport/ring leases stay specialized.
3. Give source handover one transaction over availability, generation evidence, selected source and
   cache eligibility. Preserve existing priorities, startup readiness and timing policies.
4. Encapsulate scheduling state at the encoder-thread boundary. Reuse submission adapters/outcomes and
   CandidateCompletion; fresh/repeat/drain/deferred paths must not regain parallel result bookkeeping.
5. Move privacy and failure reconciliation as separate complete invariants. Preserve blackout-frame
   behavior and source retention; a failure must not promote a stale cache or commit an audio anchor.
6. Migrate workers, start/stop and status reporting; remove migrated externs and broad caller includes.
   A snapshot reports observed facts; no frontend directly repairs coordinator flags.

Tests: source startup/unavailability, switch with candidate in flight, old generation, repeated stop,
worker error/child loss, deferred retry/drop, privacy entry/exit, CFR overload/drain and exactly-once
shutdown. Use explicit barriers and fake clock/sources; add real production-path coordination seams.
Commits: session/workers; source handover/leases; scheduling/privacy/failure ownership.
Exit: external code requests/observes coordination; it cannot mutate queues, caches and lifecycle flags.
Reject: a generic frame union/virtual SubmitFrame layer or a manager forwarding to remaining globals.

## D6 - Own audio epoch, drain and finalization transactions

Anchors: [mediaengine_internal.h](../mediaengine/engine/mediaengine_internal.h),
[mediaengine_audio_pull.cpp](../mediaengine/engine/mediaengine_audio_pull.cpp),
[submission_timing.h](../mediaengine/engine/submission_timing.h),
[mux_invariants.h](../mediaengine/mux/mux_invariants.h),
[audio_time_utils.h](../mediaengine/audio/audio_time_utils.h).

1. Trace epoch reset request/ack/commit, source-stop admission, reservoir/cursors, drift correction,
   packet preservation, drain condition variables, mux locks, trailer/finalization and output reservation.
   Separate source clock, sample timeline, packet timestamp and committed video endpoint.
2. Introduce an audio-epoch operation owner that owns publication, acknowledgment and commit; extend
   existing timing helpers rather than inventing another clock. Preserve proven memory orders and
   exactly-once first-output/audio anchoring at existing successful acceptance points.
3. Encapsulate pull/reset/drain admission and lock order. Keep per-source capture/codec policy in its
   current specialized layer; coordinate operations rather than exporting mutable atomics and cursors.
4. Introduce a finalization owner for the effective common A/V endpoint, encoder flush, packet trim,
   mux/trailer completion and output release/error. Asynchronous stop remains asynchronous; controller
   acceptance and actual finalized output remain distinct.
5. Migrate one audio source/codec-neutral operation first, then all tracks/audio-only and failure paths.
   Remove public FFmpeg/output/reset/drain state only when all participating consumers use operations.

Tests: reset while pull/drain is active, delayed acknowledgment, stale epoch, first-frame failure,
retry without anchor, multi-source stop, encode/mux/trailer failure and repeated finalization. Verify
sample bounds, monotonic timestamps, declared effective endpoint, codec priming/padding and no double
packet/cache ownership. Use production orchestration with controlled encoders/mux adapters.
Commits: epoch ownership; pull/drain coordination; finalization/output lifetime.
Exit: one owner can explain the committed timeline and finalization outcome without reading external
reset/drain globals. All existing codec-specific trim and resampling protections still run.
Reject: forcing A/V equality by dropping legitimate audio, adding silence arbitrarily or delaying stop.
Limit: deterministic timing tests do not prove inaudible audio or real capture synchronization.

## D7 - Complete SDK lifecycle owners, one cluster at a time

Anchors: [streamline_hook_internal.h](../hook/streamline/streamline_hook_internal.h),
[ffx_hook_internal.h](../hook/ffx/ffx_hook_internal.h),
[nvngx_hook_internal.h](../hook/ngx/nvngx_hook_internal.h), existing NGX/PostSL lifetime helpers.

1. Inventory per SDK: module load/unload, originals/patch ownership, device and viewport/context
   identity, accepted settings, callbacks, feature creation/release, reset and retirement.
2. Streamline first: choose one complete module/device or viewport lifecycle cluster. Its owner exposes
   accept settings, observe runtime, admit operation and retire operations; counters/maps/mutexes stay
   private. Preserve accepted settings separately from runtime and visible status.
3. FFX next: per-context callback bridge registration, admission, context destruction and deferred
   release form one transaction. Keep cancellation publication ahead of drain and SDK callback ordering.
4. NGX next: reuse current feature lifecycle policies; own the surrounding module/original/feature
   transaction and identity evidence. Do not invent a generic SDK hierarchy for different contracts.
5. Remove migrated inline writable aliases and public locks. Narrow cross-SDK evidence contracts;
   SDK owners publish facts to route ownership rather than mutating each other's state.

Tests: init failure, reload/replacement, device/viewport/context reuse, explicit off, settings without
evaluation, stale generation, callback across destroy/unload, repeated cleanup and real-hook reactivation.
Commits: one complete Streamline cluster; FFX bridge/context lifecycle; NGX feature/module lifecycle.
Exit: each migrated SDK resource has one retirement owner and no unrelated writer to its state.
Reject: a giant SDK manager containing the same externally writable state or getter/setter wrappers.

## D8 - Finish controller application and configuration boundaries

Anchors: [main_internal.h](../captureengine/app/main_internal.h),
[main_controller.cpp](../captureengine/app/main_controller.cpp),
[controller_recording.h](../captureengine/app/controller_recording.h),
[libcaptureengine_controller.cpp](../captureengine/app/libcaptureengine_controller.cpp),
[process_ipc.cpp](../common/ipc/process_ipc.cpp).

1. Audit child supervision/replacement, configuration loading/publication, injection control, overlay
   settings, screenshot, benchmark and application shutdown. Preserve the completed recording owner.
2. Own child lifecycle with explicit start/readiness/exit/replacement observations and cancellation.
   A child identity must bind command/health evidence; do not attribute old observations to a new child.
3. Separate validated settings from UI presentation and process effects. Publish complete supported
   configuration operations; retain atomic/layout/generation barriers in the existing IPC contract.
4. Migrate remaining low-frequency control mapping sequences to specific validated IPC operations.
   Keep frame/ring snapshots and leases separate; no universal channel framework or mapped-pointer API.
5. Narrow controller effects contracts used by frontends/C facade; remove direct mutation and cycles.
   Preserve unsupported results for API capabilities that do not exist yet.

Tests: readiness cancellation, replacement/old health, partial startup, command failure, reconnect,
invalid configuration/layout/target, publication ordering, reentrant shutdown and exactly-once notices.
Use isolated Windows mappings and production validation. Parser/untrusted-boundary changes require
fuzz harnesses and committed safe corpus; new shared ABI/layout changes require clean verification.
Commits: child lifecycle; settings publication; remaining validated control operations/frontend cleanup.
Exit: frontends invoke domain commands and consume observations without child/IPC bookkeeping.
Reject: treating the controller-bound C facade as an independently embeddable engine DLL.

## D9 - Shrink interfaces as ownership becomes real

Apply inside D2-D8 rather than delaying all include cleanup until the end.

1. After a lifecycle cluster is owned, define a small caller contract with commands, outcomes and
   necessary identities. Keep mutable state, locks, SDK types, COM resources and phase helpers private.
2. Replace umbrella includes in migrated consumers. Avoid exposing an implementation header through
   another nominally small header or exporting mutable references to bypass the contract.
3. Add strong types only at demonstrated unit/identity/lifetime boundaries. Preserve ABI fixed layouts;
   internal types must not silently alter exported calling conventions, field offsets or descriptor sizes.
4. Check dependency cycles, include fan-out and caller decisions after migration. Remove temporary
   forwarders and duplicated input validation where the owner now guarantees it.

Tests: relevant production behavior, header/ABI build checks and existing layout assertions. No test
should merely mirror a private field or accessor. Confirm x86/x64 consumers compile where applicable.
Exit: a caller needs only the small contract to use the module correctly; private changes stay local.
Reject: opaque heap handles everywhere, excessive tiny headers or an interface per internal function.

## D10 - Audit and migrate remaining graphics/capture/override lifecycles

This is an audit-required backlog, not a claim that these modules need the same DX12 design.

1. Inventory D3D9/10/11, OpenGL, Vulkan, DXGI interposers, WGC, transport/ring and 3D overrides using
   repo-map.md. Trace init/reset/present/capture/unload and dispatch/publication boundaries for each.
2. Select actual cross-module invariant gaps: device/context lifetime, swapchain replacement, capture
   generation, hook original ownership or override restore. Document which are already properly owned.
3. Characterize one chosen path and expose its production transaction to controlled failure tests.
   Create an owner only for the demonstrated lifecycle; share existing pure policies where identical.
4. Migrate consumers, preserve source-specific ordering/performance and remove obsolete aliases.
   Add backend-specific scripted integration only when it meaningfully exercises the changed contract.

Commits: module audit; one coherent invariant per commit series, with native/flow/product gates as relevant.
Exit: every audited module has a documented ownership map and each selected gap is closed or justified.
Reject: a generic plugin/frame/backend hierarchy introduced to make unlike APIs look uniform.
Real application/backend coverage must be recorded separately from deterministic unit coverage.

## D11 - Convert build fragments into explicit modules

Anchors: [build.py](../build.py), [tools/build](../tools/build), [tools/tests](../tools/tests),
[build.py.md](build.py.md). This changes build machinery and requires clean/verify-clean gates.

1. Map fragment reads/writes, import-time effects, monkeypatch expectations, CLI flag interactions,
   process execution and environment/path/version/cache/verification ownership. Characterize normal,
   tests-only, no-build, resume, clean, verification, flow, package and failure behavior first.
2. Extract pure leaf policies into ordinary importable modules. Preserve caller behavior with narrowly
   scoped compatibility exports in the entry point while migrating tests to patch actual dependencies.
3. Introduce explicit immutable build options and owned mutable run state for identity, stage results,
   cache validation and resume. Keep tool/path resolution and execution dependencies explicit; do not
   replace shared globals with an untyped dictionary passed to every function.
4. Migrate dependency/toolchain, compilation/test and packaging/privacy operations in dependency order.
   Maintain content-validated reuse, compile databases, stage parallelism, failure propagation, resume
   eligibility, installer freshness, symbols, hardening and release asset privacy.
5. Make build.py a small parser/composition/CLI entry. Remove concatenation/exec only after every
   fragment dependency is an explicit import or parameter and compatibility users have migrated.

Tests: existing Python policy suites plus CLI composition, cache invalidation, failed stage/resume,
tests-only product identity, clean rebuild, x86/x64, flow host cleanup, packaging and privacy failures.
Run focused Python tests in development; close machinery commits with clean product/package tests
and the required verify-clean policy. Never weaken a gate to simplify modularization.
Commits: pure leaves; options/run state; build/test orchestration; packaging/entry-point convergence.
Exit: no executable-source concatenation or hidden cross-fragment mutable namespace remains.
Reject: imports that install every module's names into shared globals or a giant universal BuildContext.

## D12 - Replace brittle protection and retire obsolete documentation/code

Apply throughout; finish with a cross-module audit after the ownership waves.

1. Classify source-reading tests: behavior proxy, hook/patch/security invariant, ABI/layout protection,
   packaging policy or source organization rule. Keep the latter categories where source is the fact.
2. For behavior proxies, invoke the same production orchestration through private static adapters.
   Demonstrate historical-defect sensitivity by deliberate mutants before retiring old assertions.
   Preserve independent hook/security protection even when the happy path has a behavior test.
3. Recompute unused entities with compiler/source evidence; the historical 84 candidates are a review
   queue, not permission to delete. Cover conditional compilation, other TUs, exports and tests.
4. Move incident narratives from source to dated wiki evidence; code retains the current invariant.
   Split oversized topic pages by reusable contract/evidence boundaries; do not merely wrap fewer lines.
5. Reconcile terminology, source/test anchors, index routing, backlinks, pending validation and backlog.
   Meter remaining repetitive transition logs with ChangeGate and capped summaries where useful.
   Vendor log verbosity needs supported controls and evidence, not an unsupported silent suppression.
6. Repeat the original and new investigations using fixed entry questions. Report decisions removed
   from callers, private mutation sites, include reach and full implementation context including adapters.

Commits: one tested protection replacement/removal cluster; documentation/logging cleanup; final comparison.
Exit: no duplicate migrated policy/adapter, no falsely completed validation claim and no stale ownership
documentation. Log and test budgets must be measured, not justified by introducing unconditional noise.
Reject: deleting source tests wholesale, packing files to satisfy line limits or trusting an old unused list.

## Verification matrix and commands

| Boundary | Required evidence before accepting the slice |
| --- | --- |
| Controller/child | Pending cancellation, rejection/unknown ack, immediate restart, stale observations, readiness/media/child failure, video/audio-only and exactly-once effects |
| IPC/config | Open/map/discovery failure, ABI/identity/generation mismatch, replacement/reconnect, ordering, cleanup and relevant parser fuzz |
| Media coordination | Fresh/repeat/deferred/rejected, retention/release, cache eligibility, source handover, privacy, overload/drain and worker teardown |
| Audio/finalization | Reset/ack/commit, sample/packet units, first-output failure, drain races, common effective endpoint and codec/multi-track invariants |
| SDK/FG | Settings/runtime/status distinction, stale epochs, context/device replacement, callback across retirement and deferred GPU release |
| DX12/queue | Proven dispatch identity, reentry, success without recovery, controlled resource/list/submit failures, exact release and capture ordering |
| Test seam | Actual production orchestration executes; deliberate historical defects are detected |
| ABI/build | Legacy and new exports/layouts, rejected loader pointer cleanup, applicable x86/x64, clean shared-ABI/machinery verification |
| Flow harness | Every physical output covered exactly once, correct owner/status, no debug-layer corruption/errors and confirmed process cleanup |

Focused development loop:

```powershell
python build.py --incremental --tests-only --run-tests --gtest-filter="<focused-suite-or-test>" --skip-updates --concise
python tools/refactor/syntax_check.py --changed
python build.py --incremental --tests-only --flow-tests --run-tests --gtest-filter="Flow*" --skip-updates --concise
```

Before each completed implementation commit:

```powershell
python tools/manage_changelog.py --validate
git diff --check
python build.py --incremental --run-tests --gtest-filter="*" --skip-updates --concise
```

Explicit `*` selects native units and all FG scenarios under the inspected runner; it does not select
Python self-tests. Never skip packaging. Read recorded gate summaries instead of repeating a gate
for output. Resume only an immediately preceding failed top-level build. Keep started processes bounded
and verify cleanup on every failure. Follow [secret-leak-prevention.md](secret-leak-prevention.md).

Additional checks are chosen by the actual changed boundary, not omitted to keep the loop short:
clean product build for machinery/dependency/stale-artifact changes; `--verify --verify-clean` for
machinery/shared ABI/hardening/gate policy; relevant fuzz for parser/deserialization/untrusted boundary
changes. Lint and sanitizer/fuzz/runtime gates follow build.py.md. Record required-but-unrun checks
explicitly. Do not turn this document into blanket authorization to skip mandatory project gates.

## User hardware/application validation

Use the fresh installer produced by the accepted gate; record version, capture source, GPU/driver,
API, FG settings, codec/track configuration and application scenario. Start diagnostics with log_digest.

1. Talos/GTA and other established workloads: switch every off/FSR/DLSS direction, repeat switching,
   focus changes, resize and shutdown. Evidence: continuous overlay coverage, correct visible FG status,
   no crash/hang/device loss and correlated route/epoch transitions. Investigate relevant dumps.
2. Repeat supported Steam/Rockstar/EOS overlay combinations and installation order. Evidence: correct
   forwarding/re-hook behavior and physical-output coverage without extra suspension or duplicate draw.
3. WGC and inject recording: startup, immediate stop/restart, FG switching, focus, source loss/privacy
   and encoder overload. Evidence: stable CFR schedule, correct candidate release/retry and recovery.
4. ALAC/AAC/FLAC/OPUS/PCM, application/system/microphone sources, mixing and multiple tracks: verify
   effective A/V endpoints, sample/packet timelines, startup/stop sync and no audible artifacts.
   Use deterministic media probes plus listening and application recording; container duration alone
   does not establish equal effective audio/video length or perceptual sync.
5. Compare representative hardware timing/capture runs before/after under the same settings and scene.
   Evidence: frame-time distributions, relevant CPU/GPU timing, allocation/copy behavior and overload
   recovery. Report measured deltas and conditions; unit/WARP success proves none of these results.

Do not reinterpret the older GTA ResizeBuffers/FFX-reference hypothesis as a proven CE root cause.
Collect a current bounded reproduction, ownership evidence and relevant dump before deciding its fix.

## Independent features remain separate

An independent engine DLL, event subscriptions, preview/packet output, dynamic reconfiguration and
generic plugin/frame hierarchies are not prerequisites for removing the debt above. They need their
own user-facing requirements, API/ABI/threading/versioning design and integration tests. Keep the
controller-bound C facade honest about unsupported capabilities. Revisit feature proposals after
the relevant ownership contracts stabilize; do not expand this plan into a simultaneous engine rewrite.

## Completion and delivery

Architecture debt in an audited boundary is closed when one owner maintains its invariant, all callers
use commands/observations, mutation/lifetime aliases are removed, production failures are behaviorally
protected, documentation agrees with code, and applicable gates have passed. A smaller file or a new
class is insufficient. Any residual debt has a concrete owner, reason, source anchor and follow-up.

For every wave deliver local commit sequence, fresh installer, focused/native/flow and applicable
additional verification, mutation evidence, ownership/locality comparison, updated contracts and pending
user validation. State audit limits and unresolved findings. No unsupported completion percentage,
calendar estimate, performance claim or whole-project maintainability certification.

The 32 other files in temp were one-off migration scripts, a layout probe and an old commit message.
They are obsolete after the completed implementation and are safe to purge following literal inventory.
Keep this refreshed refactor.md working copy; reusable refactor tooling remains under tools/refactor.
Do not generalize that decision to future logs, captures, dumps or unrelated temporary work.
