# Core refactor contracts

Last verified: 2026-10-05; current native suite and all 15 isolated FG flow scenarios pass the
product/package closing gate. Baseline: fe805f58 and package 0.1.6974 (14 FG scenarios).
Current run/artifact authority: build/verification/latest_summary.txt and latest_manifest.json.

This page records source-backed ownership contracts for the core refactor. It is not
an independent engine/library specification. Source and tests outrank this page.
Remaining architecture work: [architecture-debt-plan.md](architecture-debt-plan.md); the temporary
refactor plan is now an identical working copy of that canonical plan.

## Baseline and investigation method

Repeat these investigations before/after each relevant slice using the same entry points:

| Investigation | Initial implementation context | Distributed decisions |
| --- | --- | --- |
| Stop a pending recording, then start again | main_internal.h, main_recording.cpp, libcaptureengine_controller.cpp: 1,509 lines, approximately 16,473 tokens | requested flag, pending intent/tick, tray state, transport stop acceptance, process release |
| Explain PostSL route retirement and stale render cancellation | dx12_hook_types.h, postsl_route/queue/render_entry/render_submit.cpp: 2,886 lines, approximately 40,396 tokens | activation/confirmation, cancellation epoch, render lock, callback/GPU drain, queue references |
| Explain encode failure, repeat and timeline commitment | media_main_encoder_08_loop_encode.cpp, _07_loop_emit.cpp, mediaengine_frame.cpp: 2,015 lines, approximately 28,517 tokens | boolean/deferred side channel, candidate/cache promotion, leases, source timing and output commitment |

Paths in the table use module-unique basenames; see repo-map.md. Approximate tokens
are UTF-8-decoded character counts divided by four, not model tokenizer measurements.
These are fixed investigation scopes, not repository-wide architecture measurements.
Completion requires fewer caller decisions, not simply smaller files or headers.

## Recording and IPC

- Controller intent is not media phase. Start command acknowledgement does not prove live output;
  pending acknowledgement does not prove no file exists. Media finalization is asynchronous.
- Explicit stop clears requested ownership, pending start and presentation before child stop.
  Media gets the first request; inject is fallback. Media self-exits after finalization and the
  controller releases its active endpoint/handle so the next start gets a fresh child.
- Video needs inject and media; audio-only also supports direct media acceptance. Sensor readiness
  failure is nonfatal for video. A media integrity failure disables automatic recording.
- Existing CapturePipelinePhase/lifecycle helpers remain the media authority. Do not add another
  media state machine to the controller. Ordinary frontend code consumes commands/observations.
- Discovery/shared mappings require exact ABI validation. Release/acquire and existing generation
  protocols remain unchanged. Independently read health atomics are observations, not a coherent
  multi-field snapshot. No reader-only lock can coordinate a different process's writer.
- Session commands are controller-thread operations. Test transport/presentation adapters are
  internal; no test interfaces or C++ objects enter the external C ABI.

Sources: captureengine/app/main_recording.cpp, main_internal.h, libcaptureengine_controller.cpp;
common/capture/recording_lifecycle.h; common/ipc/process_ipc.h and shared_defs_detail/capture_state.h.
Coverage: test_libcaptureengine.cpp, test_process_ipc.cpp, test_recording_start_feedback.cpp,
test_shared_runtime_state.cpp. Boundary fake-backend tests alone do not test session bookkeeping.

## Media outcomes and time

- Source leases remain caller-owned. Deferred candidates retain retry ownership. A fallback repeat
  does not promote the rejected fresh candidate into the cached last frame.
- First-video/audio anchors commit only after the encoder accepts the first video output. No
  failure/defer may commit pixels that did not become output. Acceptance is not packet/GPU completion.
- Source QPC, scheduled output QPC, microseconds, audio 100 ns timestamps and frame indices are
  different quantities. Inject encoded-duration commitment and WGC scheduled live timing differ.
- Preserve shared handles, fences, adapter/process identity, cursor/capture origin, generations,
  ring-slot lifetimes and privacy behavior. No extra copies or virtual interfaces per frame.
- Existing positional ProcessFrame and descriptor SubmitFrame DLL exports keep their contracts.
  Rich results require additive versioned exports, not altered signatures under old names.

Sources: mediaengine/engine/mediaengine_frame.cpp and mediaengine.h;
captureengine/media/media_main_encoder_08_loop_encode.cpp and _07_loop_emit.cpp;
common/capture/inject_frame_ring_lease.h and common/ipc/inject_transport_snapshot.h.
Coverage: test_mediaengine_frame_abi.cpp, test_inject_frame_ring_lease.cpp,
test_cfr_rational_grid.cpp, test_frame_timing_utils.cpp, audio sync/finalization tests.

## PostSL and DX12

- Publish callback cancellation/epoch invalidation before waiting for render-lock ownership.
  Already-entered callbacks check their entry epoch before GPU submission.
- CPU callback lifetime and GPU completion are separate proofs. Retain existing queue/resource
  references and release points, including deferred retirement. No blanket COM retention.
- SDK presence, accepted settings, observed generation, overlay route and visible status differ.
  Queue discovery remains observational and does not create live probe queues.
- Success must not execute failed-GetBuffer or failed-reset recovery. The frame backbuffer has
  one normal release point and idempotent destructor cleanup for early exits.
- Preserve reentry, runtime-owned present/multi-output handover, independent FSR composition,
  capture-before/after-overlay, queue selection and fences. Avoid new waits/locks/allocations.

Sources: hook/d3d12/dx12_hook_postsl_route.cpp, dx12_hook_postsl_queue.cpp,
dx12_hook_postsl_render_entry.cpp, dx12_hook_postsl_render_submit.cpp;
dx12_hook_process_session.h and its draw units.
Coverage: test_dxgi_shared_part11.cpp and tests/flow. Replace historical source assertions only
after actual orchestration tests demonstrably catch the same failures.

## Design choices and validation limits

Use a recording session, validated domain IPC operations, source-specific submission adapters,
PostSL lifecycle transactions and DX12 draw transactions. Reject forwarding managers, universal
channels, generic frame unions, individual locked setters and renamed generated chunk trees.

Agent execution covers native unit and FG flow tests, with regular verified local commits and
fresh setup packages. Real-game, capture/A/V matrix and hardware performance checks belong to
the user. WARP/fake SDK runtimes exercise real hook orchestration but cannot certify vendor/game
compatibility. Do not claim performance improvement from an interface change.

Independent engine DLL/configuration/telemetry, events, preview/packet output, dynamic
reconfiguration and plugin/frame hierarchies remain deferred independent feature work.

## Recording owner (implemented)

recording_session.{h,cpp} owns requested/pending/live-observation bookkeeping and command
results. ControllerMain owns its scope; controller_recording.cpp adapts existing processes and
presentation. All frontend decisions read a value snapshot; recording globals are removed.
Readiness may pump messages: nested start is rejected and a reentrant stop cancels continuation
before any start command. Request identity is local, not a claim of remote authentication.
Tests compile the real session, including a stop within the readiness adapter. Historical
controller substring assertions are replaced by behavior tests; process/ABI/UI wiring guards stay.

## Validated controller IPC (implemented)

common/ipc/inject_control_channel owns per-operation discovery and payload views, validates both
ABIs and the expected live process handle, and rechecks discovery after payload mapping. A new
operation rediscovers the target; no borrowed view or public mapped-pointer callback escapes.
Intent, notification, independent health observations, conditional failure consumption and dead
media-state clearing preserve existing atomic orders and wire layouts. A successful CAS operation
means mapping validation succeeded; it does not promise consumption if the failure changed.
Unavailable, map failure, invalid discovery/payload ABI and stale target are distinct outcomes.
Rejection diagnostics use ChangeGate, with status and process identity. Frame rings remain specialized.

Production-path tests use isolated named mappings and a bounded child unit-test process. They cover
publication visibility, malformed ABI, target withdrawal/replacement/reconnect, short payload mapping
failure, missing discovery/payload, compare-exchange preservation and rejection handle cleanup.
The controller's stop transport helpers are private to controller_recording.cpp. Screenshot capture
still owns its original pseudo-overlay scope; only notification publication changed.

## Media submission result (implemented)

frame_submission_result.h defines the additive 40-byte, 8-byte-aligned V1 output, with fixed-width
status, output origin, candidate disposition, first-output commitment and microsecond timeline
fields. All three WithResultV1 exports require exact result size and leave invalid storage untouched.
Their bool reports boundary validity; result.status reports ingestion. No engine or inactive recording
is a valid rejected result. Existing positional, descriptor, repeat and deferred-query exports retain
signatures and adapt the same internal operations to legacy booleans. The loader requires the V1
exports and clears every legacy/new pointer on rejection/unload.

SubmitAndCommit is a statically bound production seam around Encode, local deferral classification
and the existing successful anchor/timeline/audio commit. It adds no dispatch layer, frame copy or
lock; API/mux lock ordering and cache promotion stay unchanged. Fresh accepted source becomes release
eligible, deferred fresh source remains retryable, and repeats carry no source candidate. None of these
outcomes proves GPU completion or synchronous packet emission; ring owners still enforce fences.
Inject results report committed encoded duration (or the existing VFR value). WGC fresh and WGC CFR
repeat results distinguish scheduled video timing from the encoded-duration audio pull target.

Tests: test_submission_transaction.cpp exercises that production orchestration with controlled
operations and an actual inactive VideoEncoder; test_mediaengine_frame_abi.cpp calls the real PE
exports; test_mediaengine_loader.cpp compiles the actual private resolver/unload orchestration.
The deferred flag is cleared before inactive encoder rejection so a prior attempt cannot classify
the new result. Existing failure diagnostics stay at their original encoder boundaries; loader
incompatibility logs name the missing export. Broad ABI/sanitizer verification remains outside the
agreed agent execution scope and must be recorded in final pending validation.

## Source submission adapters and candidate completion (implemented)

captureengine/media/frame_submission.h is the small caller contract. The inject implementation binds
transport generation and preserves shared handles, fence, adapter/process identity, dimensions and
cursor; the D3D11 implementation preserves capture origin and separates media timestamp from explicit
scheduled elapsed time. Privacy black frames use the same D3D11 adapter with zero capture origin.
Both borrow sources; Repeat submits no external candidate. Caller scheduling and fence/lease policies
stay in MediaEncoderSession. No production media caller uses a paired deferred query or legacy submission.

Fresh/repeat/drain/catch-up/deferred-retry/recovery/blackout paths now carry V1 outcomes. Recovery queries
the repeat cache only after failure, preserving the original successful-path call count. The statically
bound CompleteCandidate operation admits promotion only for an accepted original fresh output, retains
retry candidates and otherwise invokes source-specific discard/release. This prevents cached-black
fallback from adopting unencoded source metadata. WGC catch-up counts that output as a repeat.

Tests compile the real source adapters with controlled DLL slots and run actual ring-lease ownership
through completion, including deferred retention, accepted transfer, rejected/recovered discard and
cached-black output accepted without source adoption. Privacy gating source protections remain; only
obsolete boolean/deferred and descriptive-comment assertions changed. Invalid result-boundary feedback
uses ChangeGate; existing failure/recovery and lineage diagnostics preserve timing and correlation.

## Timing glossary and commitment owner (implemented)

| Quantity | Internal type | Authority / boundary |
| --- | --- | --- |
| Source or scheduled timestamp | QpcTicks | Source adapter or controller scheduler; never elapsed microseconds |
| Ticks per second | QpcFrequency | Trusted system QPC frequency |
| Elapsed video/audio pull value | Microseconds | Accepted submission / existing source-specific CFR policy |
| Diagnostic source/audio timestamp | Milliseconds | QPC conversion; legacy no-frequency diagnostic fallback remains |
| Audio sync anchor | AudioHundredNanoseconds | Existing RawQpcToHundredNanoseconds helper; invalid QPC/frequency maps to zero |
| Encoded CFR frame | FrameIndex | Zero-based contiguous encoder PTS; -1 means no assigned frame |
| Selection grid tick | GridTick | Legacy one-based tick: tick 1 is frame zero; distinct from FrameIndex |

SubmissionTiming privately owns first-output source/audio anchors, both sampling cursors and their
separate accepted elapsed values. Start/stop reset it as one operation. CommitFirst publishes the
anchor before invoking the existing audio callback and admits that callback once, including reentry.
SubmitAndCommit keeps failed/deferred candidates out of that operation; returned first commitment
reflects the actual owner admission. Engine recordingStartTime remains the shared steady A/V epoch
and is updated inside the admitted callback, at its original successful acceptance point.

VFR sampling may advance on a rejected candidate under the original source-time policy. That is
not an accepted video/audio commitment. Explicit WGC elapsed values remain authoritative even when
below the sampling cursor; the scheduler owns their ordering. Stop/finalization reads the original
sampling horizon, not a newly invented clock or shortened output duration. Inject commits encoded
CFR duration; WGC commits its scheduled live value and pulls audio to encoded duration. No scheduler,
frame copies, fence/lease policy, or descriptor/shared layout changed.

Sources: common/capture/time_units.h and time_grid.h; mediaengine/engine/submission_timing.h,
mediaengine_frame.cpp, mediaengine_timeline.cpp, mediaengine_recording_{start,stop}.cpp;
video_encoder_finalize.cpp; media_main_encoder_{04_loop_wgc_target,08_loop_encode}.cpp.
Tests: test_submission_timing.cpp (production owner + submission orchestration, controlled values),
existing frame-grid/encoder source guards and ABI tests. The V1 result (40 bytes, alignment 8) and
internal unit values were additionally syntax-compiled with the installed i686 cross-compiler.
FirstVideoCommit logs distinguish raw QPC, source milliseconds and the 100 ns audio anchor; one
entry per committed first output. Audio codecs/tracks, overload recordings and hardware sync remain
user validation; these unit/layout checks cannot establish those outcomes.

## PostSL admission and epoch ownership (implemented)

Verified 2026-10-05: native lifecycle/source regressions and all 15 real-hook WARP flow scenarios.
postsl_lifecycle.h privately owns callback execution admission, callback counts, lifecycle epoch,
render serialization and epoch-specific confirmation. Mutable aliases for those fields are removed.
SDK settings, runtime presence and visible status remain distinct observations. The same lifecycle
owner now also owns route activation/proof; queue/resource retirement is owned as documented below.

RenderTransaction holds the existing render mutex through every PostSL phase and early return.
Previously Chunk0's scope guard released it before Chunk1/2/3: retirement could drain the entry phase
while recording/submission still ran. The owner now invokes the same phase chain under one scope;
no virtual dispatch, frame allocation, source copy or new GPU wait is introduced. Normal-return
PublishRetirement disables/unpublishes callbacks and invalidates the epoch before FinishRetirement
acquires the render lock. The queue/resource owner below applies callback and GPU completion evidence
at the existing release boundaries. Other transition generation resets retain their original ordering.

Confirmation publishes exact swapchain proof before its release stamp. The stamp carries the entry
epoch; invalidation immediately makes it stale, even if an old store races cancellation. Failed stale
confirmation does not publish proof and is diagnosed with metered entry/current epoch and reason.
General confirmation and probe behavior remain distinct from proof for the current epoch. Atomics
read through observations are independent, not a coherent multi-field snapshot. The owner's callback
scope increments before the enable recheck and remains counted until return, including retirement.

Tests: test_postsl_lifecycle.cpp executes production owner transactions using controlled publication,
threads and latches, with no sleeps. It covers publication ordering, stale/cancelled proof, render
serialization through recording/submission, early exits, callback-spanning retirement and repeated
retirement. FlowDLSS.NativeReturnRejectsDepartedEpochConfirmationAndReactivationRemainsCovered runs
actual WARP output through DLSS ON/OFF, native swapchain replacement and reactivation; rejected old
confirmation cannot alter the new epoch. Every physical output is accounted/covered exactly once,
FG publication is checked and no debug-layer errors are accepted. Test-only evidence exports live
in flow_hook_entry.cpp, never the product DLL. Real vendor/game concurrency remains user validation.

## PostSL callback submission resources (implemented)

PostSLSubmissionResources is the private statically-bound queue-reference adapter used around the
same production phase chain. The gate retains the selected and optional wrapper queue under the
existing command-queue mutex. Selection replacement transfers those references; the enclosing render
transaction releases both after submission/early return, before unlocking render admission. The
idempotent destructor is a cleanup backstop. No extra queue references, allocation, virtual dispatch,
GPU copy or wait is introduced. Queue selection, fences and source/capture ordering remain unchanged.

The old queueReleaseGuard and slWrapperQueueReleaseGuard were local to Chunk1 and released before
Chunk2/3 used the queue pointers. Moving their lifetime is a root-cause correction justified by the
production resource-transaction tests, rather than blanket retention of swapchain backbuffers. The
backbuffer's existing normal release point is unchanged. Global selected/pinned/last-working queue
retirement is owned separately below; this callback lease cannot replace GPU completion.

Callback admission captures its epoch before checking enabled execution. That epoch is carried to
PostSLRenderSession; RenderTransaction rejects it if it differs from the current epoch after acquiring
the mutex. A callback admitted just before retirement therefore cannot start in the new generation.
Rejected admission diagnostics distinguish render-busy from retired-generation and meter correlation
by admitted/current epoch. Independent settings, route latches and visible FG state remain separate.

Tests: test_postsl_submission_resources.cpp runs the actual production resource/lifecycle orchestration
with counted queue adapters: references survive all phases even after runtime retirement, early exit
releases exactly once, replacements and alias roles balance, stale admission acquires nothing.
test_postsl_lifecycle.cpp additionally covers admission delayed until after replacement. Syntax checks
compile the real hook consumers. All FG flow scenarios and both product architectures are required
for the closing gate; real vendor/game lifetime and hardware performance remain user validation.

## PostSL queue selection and deferred retirement (implemented)

PostSLQueueOwner privately owns selected, pinned-wrapper, last-device-healthy and deferred queue
references. Identity observations are atomic borrowed values, protected for dereference by the
existing command-queue mutex or an admitted callback lease. Several observations do not form a
snapshot. ReplaceSelection retains before publication and returns a private retired-reference
lifetime, preserving the original explicit release outside the queue lock. Healthy submission and
pinning deduplicate their own reference roles; aliased roles still own separate COM references.

Retirement records fence/value evidence under the existing overlay-to-command-queue lock order.
Fence references survive replacement of overlay state. A callback outlasting the existing bounded
drain extends the retirement target at its successful signal publication; the ordinary path performs
only pending-state atomic checks, without COM retention or allocation. Timeout is not proof of GPU
completion. Selected/pinned references with incomplete evidence remain owned until callbacks drain
and every retained fence reaches its target. Selected queues entering the established deferred slot
are released on the next cleanup pass, preserving that release point. Explicit hook shutdown drains
all remaining owner roles idempotently; it retains the existing unload authority and bounded waits.

The previous six queue globals and SetPostSLLastWorkingQueue/DetachPostSLQueuesLocked helpers are
removed. Existing queue policy, specialized runtime/normal-route queue ownership, overlay resources,
settings and visible FG state remain separate. No new frame copies, ordinary-frame allocation or
virtual adapter dispatch is introduced. GPU evidence checks/retention occur during pending retirement.

Sources: hook/d3d12/postsl_queue_owner.h; dx12_hook_postsl_queue.cpp, postsl_render_gate.cpp and
postsl_render_submit.cpp (dx12_hook_ prefix); dx12_hook_main.cpp shutdown. Tests:
tests/test_postsl_queue_owner.cpp executes the production owner with counted queue/fence adapters,
covering replacement/release boundaries, aliased roles, incomplete fences, delayed final submission,
replacement fences, callback-spanning retirement, runtime reactivation and repeated cleanup/unload.
Native regressions, real-hook WARP FG flows and the product/package gate remain the automated gate;
real vendor/game transitions and hardware performance remain user validation. Last verified: 2026-10-05.

## PostSL route activation and retained proof (implemented)

The lifecycle owner privately owns route activation, synthetic activation awaiting proof and the
retained route-confirmed latch. The three public atomic aliases are removed. ActivateSyntheticProbe
admits the route without promising output. ActivateRoute consumes the existing preserve-startup
policy; SuspendRoute keeps proven ownership for make-before-break; RestartRoute revokes activation,
startup evidence and proof together. Scheduling/probe progress counters remain separate policy
observations; accepted SDK settings, runtime presence and published visible FG status remain separate.

ConfirmRender now commits current-epoch proof and retained route proof through the same operation.
It reports first route proof to the production presentation adapter, replacing its later free-standing
confirmed/synthetic flag writes. A private cancellation revision shares the atomic route word with
its flags. Explicit proof revocation and reactivation change that revision; confirmation captured
before cancellation cannot restore or overwrite replacement proof, even without an epoch change.
Current-generation proof is cleared on generation invalidation/reactivation; the retained route latch
survives only the same temporary suspension paths as before. Borrowed independent observations remain
observations, not a transactional multi-field snapshot. Ordinary route observations still use one
atomic load, with statically bound operations and no frame allocation, copies or virtual dispatch.

Sources: postsl_lifecycle.h; dx12_hook_postsl_render_{entry,submit}.cpp; observer, transition and
route consumers (hook/d3d12). Deterministic production-owner tests in test_postsl_lifecycle.cpp cover
synthetic activation without output proof, first/repeated proof, warm suspension, reactivation,
proof cancellation during publication and replacement confirmation in the same epoch. Existing
real-hook WARP scenarios cover route/callback generations. Native, FG and package gates are required;
real games and hardware performance remain user validation. Last verified: 2026-10-05.

## Executable DX12 draw transaction (implemented)

ExecuteOverlayDraw in hook/d3d12/overlay_draw_transaction.h is the production orchestration, using
private FrameProcessSession::DrawOperations and the real overlay state. Controlled unit operations
execute exactly the same reset/acquisition/recovery/record/close/submission/release branches.
Allocator/list/close failures invalidate sync only. Failed or null GetBuffer retires RTVs and
invalidates overlay initialization only; missing commands/interface and busy/priming skips preserve
initialization. Successful draws enter no recovery. SDK calls retain direct static dispatch, queue
selection and fences; the private seam adds no frame allocation, virtual adapter layer or copies.

DrawBackBuffer owns only the per-present GetBuffer reference. Normal close-failure/success paths
release at the original point after submission completion/metrics and before post-overlay capture.
Recording/submission early returns keep the frame destructor backstop. Failed acquisition returning
a non-null pointer is rejected and released before capture. All temporary frame state is private;
metrics completion runs inside Run at the old post-Run observation point, before destructor cleanup.
Normal frame render admission and the named producer operations are documented below.

Tests: tests/test_dx12_draw_transaction.cpp covers success with unchanged overlayInit/syncInit,
controlled allocator/list/GetBuffer/close outcomes, absent interface/commands, busy/priming skips,
submission/device-loss outcomes, early returns, capture ordering and exact release count/timing.
Its bounded event trace never allocates during the mock backbuffer's noexcept Release; overflow
aborts the test rather than allowing instrumentation to throw through destructor cleanup.
tools/refactor/check_draw_transaction_mutations.py temporarily mutates only the private production
header, requires actual behavioral assertion failures (compiler failure does not count), restores
it in finally and verifies the original again. On 2026-10-05 all six mutations were detected:
success entering RTV recovery, success invalidating sync, late normal release (destructor only),
missing all release, duplicate normal release, and release before recording. Both historical
source-text recovery assertions in test_dxgi_shared_part11.cpp were retired after this evidence;
independent hooking, patching and security assertions remain. Reproduce the bounded check with
`python tools/refactor/check_draw_transaction_mutations.py`; do not run it concurrently with builds
or transaction edits. Native/FG/product/package gates still verify the restored product; controlled
operations and WARP output do not prove vendor/hardware behavior. Last verified: 2026-10-05.

## Normal frame render admission (implemented)

FrameProcessSession privately owns FrameRenderAdmission from PrepareFrame through draw, capture,
metrics and resource destructor cleanup. The prior generated split released a local adopted lock
at the end of preparation, while normal-return retirement tried to unlock an unowned member lock.
The owner uses the existing nonblocking try-lock admission. Route rechecks release the overlay lock
and leave it released when the frame is delegated; otherwise they reacquire it. Retirement first
publishes cancellation, then releases overlay admission while draining PostSL under its render lock,
and reacquires before normal-route initialization. This preserves render -> overlay lock order.

Sources: frame_render_admission.h; dx12_hook_process_session.h; stage1_prepare_frame.cpp and
stage2_swapchain_queue.cpp (hook/d3d12). Tests in test_frame_render_admission.cpp use controlled
threads/latches to verify retained admission, all scoped exits, contention without blocking, route
delegation and callbacks draining outside overlay admission. No sleeps or frame allocations.
Native unit tests, all FG flows and the product/package gate verify the integration; real games
and hardware scheduling remain user validation. Last verified: 2026-10-05.

## Readable draw producers (implemented)

The generated Front/Tail/Else/pw5_c3 chain is removed (11 unused wrappers). ExecuteDrawTransaction
uses private, statically bound operations: SelectAllocatorSlot, ResetAllocatorForDraw,
ResetCommandListForDraw, PrepareDrawResources, AcquireDrawBackBuffer, RefreshDrawRenderTarget,
RecordOverlayDraw, CloseDrawCommands and SubmitOverlayDraw. Producer files are draw_resources,
draw_record and draw_submission; draw_metrics and draw_capture hold completion/publication.
The session exposes only construction and Run. The GetBuffer lease and admission owner establish
resource and lock lifetime; metrics completion remains private. Existing named preparation phases,
reentry guard, independent FSR composition, runtime-owned presentation, multi-output routing,
capture ordering, queue policy and allocator/fence discipline are preserved. PostSL producer
operations are PrepareRouteActivation, AcquireSubmissionResources, RecordOverlayDraw and
SubmitAndConfirmOutput; its full render transaction still owns callback queue resources.

Mechanical verification compared token sequences for ten actual producer functions before/after
the moves, normalizing method names and expanding the extracted metrics call: all matched.
Independent queue-method, upload-slot, runtime-owned capture, foreign-overlay and route wiring
assertions were retained with updated producer paths. Behavioral transaction/admission tests remain
authoritative for recovery and release. Native, all FG and package gates verify the resulting
product. Last verified: 2026-10-05. Hardware/game and A/V validation remain pending.

## Complete stop endpoint policy (implemented)

RecordingSession::StopChildEndpoints owns media-first acceptance, inject fallback and media endpoint
release. The effects adapter supplies only specific child commands and process release. Acceptance
from either attempted child wins; without acceptance, any unknown acknowledgement retains uncertainty;
only explicit rejection by both attempted children produces Rejected. A missing/disconnected endpoint
or transport failure is unknown, even if a response variable contains Ack or Error. The private
child_recording_stop.h operation sends the actual StopRecording command and classifies transport
and response separately. The former bool adapter lost explicit rejection and is removed.

Explicit stop clears requested/pending state before child commands. Failure reconciliation now also
clears intent under command reentry protection before stopping endpoints. StopResult is a private
presentation observation emitted once per actual stop transaction, after release and classification;
diagnostics include request, child, outcome and reason. Neither rejection nor uncertainty proves
that no output exists. Media finalization remains asynchronous. Hotkeys and C facade toggles call
ToggleControllerRecording -> RecordingSession::Toggle directly. The facade snapshot only selects
its existing public error category. Controller shutdown disarms intent; existing process shutdown
continues to own child exit/finalization, preserving topology.

Tests: test_recording_session.cpp exercises all nine endpoint combinations, exactly-once release/
result, media acceptance suppressing fallback, repeated stops, restart and rejected nested start
during stop. test_child_recording_stop.cpp exercises actual command adapter inputs, absent/disconnected
channels, transport failure with misleading response values, rejection and acknowledgement. Process
wiring and acknowledgement-before-media-finalization assertions remain in test_process_ipc.cpp.
Native, all FG and package gate required. Last verified: 2026-10-05.

## Completion and glossary

Sections 1-10 of the core implementation plan are implemented. The controller/session, validated
IPC, source submission/timing, complete PostSL admission/activation/queue retirement cluster and
covered native DX12 draw path have production owners and regression seams. Existing public ABI,
process topology, source scheduling, caller-owned source leases, fences and GPU completion remain
separate contracts. This is not a replacement for vendor/game or A/V/hardware validation.

| Term | Meaning and authority |
| --- | --- |
| Requested recording | Controller ownership of an accepted command/intention; RecordingSession. Does not prove output. |
| Live recording | Media observation, correlated with available request/timestamp/child evidence; uncertainty retained. |
| Finalized recording | Media/mux completion and publication result; command acceptance cannot assert it. |
| Source present | Application/source timing evidence, distinct from a generated/final physical output. |
| Lifecycle epoch | PostSL cancellation/admission identity; a departed callback cannot confirm the replacement route. |
| Transport generation | Source/ring resource identity; adapters bind it without changing the existing wire protocol. |
| Scheduled output time | Controller CFR/WGC output slot; distinct from the source timestamp and inject encoded duration. |
| Borrowed resource | Observation valid only under its existing owner/lock/lease lifetime; no release authority. |
| Owning lease | Explicit caller/session retirement obligation (ring candidate, backbuffer or callback queue role). |

Dependency direction is frontend -> recording session; transport adapter -> validated domain operation;
source coordinator -> inject/D3D11 submission adapter; SDK adapter -> PostSL lifecycle/queue owner;
private frame operations -> draw transaction. RecordingEffects implements process/presentation effects
without importing tray or overlay into session policy. FrameProcessSession and PostSLRenderSession
keep fields, locks, SDK types and stages private to implementation headers. Scoped admission permits
existing callback reentry and explicitly releases overlay admission for render -> overlay retirement.
There is no generic channel/frame/plugin framework or added virtual per-frame dispatch.

Diagnostic boundaries: ControllerSession carries request, child, intent/outcome and reason; validated
IPC classifies discovery/open/map/ABI/target failures; media outcome/commit diagnostics retain source
and generation evidence; PostSL cancellation/retirement/rejected confirmation carries epoch/reason;
DX12Draw carries failure operation/HRESULT/epoch and meters lock contention. Repeated hot-path
failures use ChangeGate; no unconditional new frame logging. Existing actionable PDB/package privacy
checks run in each product gate.

Sources/tests: [recording session](../captureengine/app/recording_session.cpp),
[child stop adapter](../captureengine/app/child_recording_stop.h),
[validated IPC](../common/ipc/inject_control_channel.cpp),
[submission adapters](../captureengine/media/frame_submission.h),
[candidate completion](../captureengine/media/candidate_completion.h),
[timing commitment](../mediaengine/engine/submission_transaction.h),
[PostSL lifecycle](../hook/d3d12/postsl_lifecycle.h),
[queue retirement](../hook/d3d12/postsl_queue_owner.h),
[draw transaction](../hook/d3d12/overlay_draw_transaction.h),
[frame admission](../hook/d3d12/frame_render_admission.h),
[draw regression](../tests/test_dx12_draw_transaction.cpp),
[real-hook flows](../tests/flow/flow_test_support.h).

## Repeated locality investigations (2026-10-05)

Same entry questions and initial file sets as the baseline above. Tokens mean decoded characters / 4,
not a model tokenizer. The PostSL baseline token total is corrected by recomputing fe805f58 (32 tokens
below the earlier transcription). New owners/adapters are included below rather than hidden by moves.

| Investigation | Baseline files | Same files now | Added owners/adapters | Full implementation context now |
| --- | --- | --- | --- | --- |
| Pending stop/restart | 1,509 lines / 16,473 tokens | 1,058 / 10,405 | 647 / 7,350 | 1,705 / 17,755 |
| FG handover/stale epoch | 2,886 / 40,396 | 2,816 / 39,558 | 486 / 5,326 | 3,302 / 44,884 |
| Encode failure/leases/committed timeline | 2,015 / 28,517 | 1,952 / 28,007 | 319 / 3,362 | 2,271 / 31,369 |

Added controller context: recording_session.{h,cpp}, controller_recording.{h,cpp}, child_recording_stop.h,
and inject_control_channel.{h,cpp}. PostSL: postsl_lifecycle.h, postsl_queue_owner.h,
postsl_submission_resources.h, dx12_hook_postsl_render.cpp and dx12_hook_postsl_session.h. Media:
frame_submission.h, frame_submission_internal.h, frame_submission_{inject,d3d11}.cpp,
candidate_completion.h, frame_submission_result.h, submission_timing.h, submission_transaction.h
and common/capture/time_units.h. All paths use the module-unique basenames from repo-map.md.

The full context grew; this refactor does not claim an overall source-reading reduction. The policy
entry sets are controller session/child classification (254 lines / 2,820 tokens), PostSL owners
(356 / 4,120), and candidate/timing/units owners (149 / 1,493). Adapter/SDK investigation still needs
the full sets above. New contract code and behavioral tests make formerly implicit decisions explicit.

- Pending stop: hotkeys/API no longer change requested/pending/output mode or choose child fallback.
  The session owns clear-before-stop, outcome aggregation, endpoint release, repeat/reentry and next-start
  eligibility. Presentation consumes notices. Wire publication/validation belongs to the IPC owner.
- Handover: consumers no longer mutate owned callback counts, confirmation epoch, route flags or queue
  role references. The lifecycle transaction admits/rejects callbacks and proof; queue retirement owns
  callback/GPU completion evidence. SDK settings/runtime/status stay separate observations. Frame
  retirement releases overlay admission around callback drain, then resumes the normal route.
- Encode failure: coordinator callers no longer pair a boolean with a deferred query, manufacture frame
  descriptors, bind generations or independently commit first audio anchors. Source adapters interpret
  explicit outcomes; CompleteCandidate owns retry/cache transfer/release eligibility; SubmissionTiming
  owns anchors and committed values. Source scheduling and actual lease release remain source-specific.

## Automated verification and remaining validation

Every completed implementation commit passed `python build.py --incremental --run-tests
--gtest-filter="*" --skip-updates --concise` including the fresh setup package. Focused native loops and
product syntax checks ran between edits. All 15 FG scenarios execute isolated, bounded real-hook WARP
orchestration with fake SDK runtimes, physical-present coverage, no double draw, visible FG status and
D3D12 debug-layer checks. The new departed-epoch/native-return/reactivation flow remains covered.
Six deliberate draw defects failed behavioral assertions and the original production header was restored
and passed. The IPC probe is skipped in the parent native process and executes in its isolated child;
that parent skip is expected, not omitted IPC coverage. No agent-started test/game/runtime process remains.

Required-but-unrun broader gates: shared ABI/result boundary changes normally require
`--verify --verify-clean`; IPC input-boundary work requires fuzz coverage. The agreed execution scope excludes
these broader Python/lint/sanitizer/fuzz gates. They are pending, as are real-game, capture/A/V and
hardware performance checks. x86 sanitizer runtime is unavailable in the project toolchain. x86/x64
product builds, applicable ABI/layout tests and package/PDB/privacy verification did pass. No duration,
compatibility, risk reduction or hardware performance result is inferred from unit/WARP success.

Reproducible user validation:

1. Install the final setup package. In Talos and GTA exercise native <-> FSR FG, native <-> DLSS FG,
   and FSR <-> DLSS in both directions, including OFF/ON reactivation, focus changes, resize and Steam/
   Rockstar overlays. Record every physical output/visible overlay and FG status; expect no crash,
   disappearing overlay, uncovered/double-drawn output or unexpected status. Preserve session logs/dumps
   and run `python tools/log_digest.py <session-dir>` before focused analysis.
2. Test inject and WGC recording independently with each available hardware/software video encoder,
   AAC/ALAC/FLAC/OPUS/PCM, single and multiple audio tracks, application/system/microphone audio, mixing
   and resampling. Include start-before-live cancellation, repeated stop and immediate restart. Expect
   asynchronous finalization, one completion, correct recording identity and no inherited stale command.
3. For each combination use a reproducible visual flash plus audible click source at start, middle and
   end. Inspect decoded audio for discontinuities, distortion/pitch changes and cut-outs; compare all
   tracks against video content, not container duration alone. Use packet PTS/duration and codec trim/
   priming metadata plus decoded samples to verify the exact effective endpoints and synchronization.
   Example inspection: `build/msys64/clang64/bin/ffprobe.exe -v error -show_streams -show_packets
   -of json <recording>` (large output stays local and must not be committed).
4. Exercise sustained encoder overload and recovery with the same source/audio matrix. Expect CFR
   continuity with the existing repeat/drop policy, retained/released leases at the correct points,
   no premature audio anchor and equal effective track horizons. Inspect finalization and recording
   manifests, media diagnostics and decoded output; automated contract tests do not establish this result.
5. Compare the same hardware/game/settings workload before/after using existing profiling, CPU/GPU
   frame time, allocations, queue/fence waits and capture copies. Expect the preserved performance
   requirements; no benchmark or no-regression hardware claim has been made by this implementation.

## Section 11 assessment: deferred independent work

| Candidate | Assessment after core refactor |
| --- | --- |
| Independent engine DLL | Session separation is groundwork. Runtime/config/helper ownership, capability negotiation and MSVC/clang embedding remain separate work; no DLL is implemented. |
| Events/callbacks | A real consumer still needs live/finalized/error delivery semantics, thread/reentry/shutdown guarantees, bounded buffering and late-observation attribution. No event API is added. |
| Preview/encoded packets | Requires independent lifetime, latency and backpressure contracts; current submission outcomes do not imply packet/GPU completion. |
| Dynamic reconfiguration | Needs codec/source/timeline transition requirements and actual consumers; not implemented through this refactor. |
| Generic frame/plugin hierarchy | Existing source-specific adapters remove the demonstrated duplication. No evidence currently requires a generic payload or virtual frame hierarchy. |

Remaining fake NGX/foreign-overlay runtime coverage and hardware validation in refactor-roadmap.md are
independent follow-ups. Existing hooking, patching, security and unrelated source regressions remain.

## Local implementation commit sequence

All commits below are local; nothing was pushed. Each completed slice has a passing product/native/
FG/package gate. The final documentation/diagnostics cleanup follows this sequence in git history.

| Commit | Slice |
| --- | --- |
| `7490bfc4` | docs: establish core refactor contracts and regression baseline |
| `db18d6e3` | refactor: own controller recording lifecycle |
| `23bbaf33` | refactor: encapsulate validated inject control transactions |
| `a0f21b9c` | refactor: return explicit media submission outcomes |
| `59161f40` | refactor: migrate source submission and candidate completion |
| `e1e28166` | refactor: own media timing and first-output commitment |
| `45793b70` | refactor: own PostSL callback admission and epoch confirmation |
| `38ec6032` | refactor: retain PostSL callback resources through submission |
| `0f64703c` | refactor: own PostSL queue retirement and completion evidence |
| `60cdcded` | refactor: own PostSL route activation and rendering proof |
| `be86f3d9` | refactor: execute DX12 draw recovery and resource transactions |
| `2785fc94` | fix: retain normal DX12 frame render admission |
| `86339c87` | refactor: recover named DX12 draw and capture operations |
| `67639667` | fix: own recording stop fallback and acknowledgement outcomes |
