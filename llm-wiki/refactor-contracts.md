# Core refactor contracts

Last verified: 2026-10-04 against fe805f58; native unit suite and 14 FG flow scenarios passed.
Baseline closing product/package gate: 0.1.6974.

This page records source-backed ownership contracts for the core refactor. It is not
an independent engine/library specification. Source and tests outrank this page.

## Baseline and investigation method

Repeat these investigations before/after each relevant slice using the same entry points:

| Investigation | Initial implementation context | Distributed decisions |
| --- | --- | --- |
| Stop a pending recording, then start again | main_internal.h, main_recording.cpp, libcaptureengine_controller.cpp: 1,509 lines, approximately 16,473 tokens | requested flag, pending intent/tick, tray state, transport stop acceptance, process release |
| Explain PostSL route retirement and stale render cancellation | dx12_hook_types.h, postsl_route/queue/render_entry/render_submit.cpp: 2,886 lines, approximately 40,428 tokens | activation/confirmation, cancellation epoch, render lock, callback/GPU drain, queue references |
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

## Media submission result (implemented; caller migration follows)

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
