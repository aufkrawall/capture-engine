# Transaction and retirement invariants

Last verified: 2026-10-12 against production sources and focused native regressions.
Stale-risk: hardware FG transitions and live audio routes require separate scenario validation.

## Present ownership and pacing

`hook/present/dxgi_shared_detail/present_pacing_scope.h` shares one wait claim across
wrapper, vtable, inline and Present1 fallback calls for one receiver. TEST, shutdown
and delegated frontend forwarding suppress the claim. It gates CE's configured
pacing override; it does not change the application's native presentation contract.
`present_vtable_dispatch.h` detaches the exact private vtable adapters to their
retained predecessors and preserves a later foreign follower.

`translated_swapchain_owner.cpp` recognizes proven system DXGI vtables without
private-data/device probes. A covered frontend decision is read before discovery,
so nested hits do not repeat forwarded private-data calls. Native DX12 negative
results cache only when the frontend class is already covered for a translated frontend. An
uncovered native proxy re-probes each Present (two COM calls) rather than installing a Release hook
on a DXGI vtable shared by every swapchain of that class (flow tests
`NativeApiProxiesAndUnforwardedDataKeepTheirOwnership` and `NativeAndTranslatedViewsOfOneOutputKeepIndependentDecisions`).
Frontend reuse and foreign Release replacement still invalidate lifetime evidence.
`dxgi_swapchain_wrap_present.cpp` retains the real receiver under `resourceMutex`
before discovery, using the same lease for the rest of the call.

Tests: `test_present_vtable_dispatch.cpp`, `test_postsl_submission_resources.cpp`,
`flow/test_flow_translation_layers.cpp` (device, private-data read/write and output counts).

## CPU and GPU lifetimes are separate

`PostSLSubmissionResources::RenderTransaction` holds `dx12_hook_g_OverlayMutex`
through admission, recording and submission. Cleanup shares that mutex; a callback
waiting for it rejects a retired generation. Teardown cancels admission first.
`dx12_hook_postsl_queue.cpp` snapshots GPU evidence before clearing selected or
pinned queues, including a pending signal on its exact submitting queue.

`overlay_resource_retirement.h` adopts old allocator/list/fence/queue/view references
and completion-event handles. Cleanup and sync reinitialization move them out of
active state without waiting for a timeout. Collection releases them only after
all recorded submission fences complete, including the separate FG completion
fence. View evidence survives replacement of synchronization objects. A failed
signal has no trustworthy completion value; later numeric completion cannot prove
it safe (`RetainedCount()` / `UnprovableCount()` track retained generations and log
escalation in `[OverlayRetirement]`). Device removal terminates that work.
`dx12_hook_g_OverlayCompletionFence` is released in `DX12Hook::Shutdown()` after
`CleanupRTVs()` retires the final generation. Outstanding references survive
loader-lock destruction rather than calling a partially torn-down driver.
Normal frames add no COM references or retirement allocations when nothing retired.
`postsl_queue_owner.h` also preserves incomplete references through explicit shutdown.

`overlay_draw_transaction.h` closes every abandoned recording once, including
record/submit early returns; priming closes once already. A failed abandon close
invalidates synchronization before reuse.

Tests: `test_overlay_resource_retirement.cpp`, `test_overlay_retirement_source.cpp`,
`test_postsl_queue_owner.cpp`, `test_dx12_draw_transaction.cpp`.
Diagnostics: `[OverlayRetirement]` and `[PostSLLifecycle]`, transition-gated.

## Audio and accepted video time

`audio_sync/packet_boundary_fade.h` owns continuous raised-cosine progress separately
for ingestion and emitted output in a packed atomic 64-bit state. `AudioLoop` edits
`ingestBoundaryFade`; pull and silence transitions edit `outputBoundaryFade`. Ingest
fades never smooth PCM already queued for output, and splitting a packet into several
output chunks does not restart its envelope. Ordered recording/epoch resets atomically
clear both owners via `Arm(0)`. `ResolveFirstPacketStartupPlacement` includes a packet
straddling recording start in shared startup alignment after trimming it to position
zero; a wholly old packet cannot choose the shared first-packet anchor, records
app-startup evidence (`sawSyncPendingPackets`), and emits a rate-limited discard log.
Rate conversion uses actual source rate.

`submission_timing.h` commits the VFR engine origin only with an accepted first frame
and clears `pendingOrigin_` across both sources once the first output commits so a
pre-commit candidate on a non-winning source cannot overwrite `startSourceQpc` later;
a rejected candidate cannot change the later accepted duration. Mux PTS already
rebases to zero independently. `RefreshCaptureContentDelay` recomputes delay from
raw configured latency after a warmup backend change, never from an already reduced
value. `present_callback_association.cpp` uses atomic payloads, a claimed writer
sequence and generation invalidation, including Reset.

Tests: `test_audio_fade_policy.cpp`, `test_audio_startup_first_packet_gap.cpp`,
`test_submission_timing.cpp`, `test_present_callback_association.cpp`,
`test_inject_capture_source_part2.cpp`.

## Post-processing and lost transport

`vulkan_post_process_settings.h` publishes both gamma curves and native intent on
connect and every live refresh; its default disables gamma and verified native
correction prevents double application. D3D12 post-processing resolves typeless
storage to supported typed SRV/RTV/PSO formats in normal and borrowed-list rendering.
Every forwarded ECL branch notifies submitted post-process ownership. Failed recording
already publishes no pending slot; proven abandonment remains explicit, without
speculatively reclaiming a command list that might still be submitted.

`RemovedTransport` latches fence/source/generation identity after a removed shared
capture fence. The frame is terminal rather than indefinitely deferred, so output
failure monitoring runs. Reconfiguration or a new transport can probe afresh.
Packet timeline diagnostics subtract only residual Opus discard after shortened tail
packet duration, matching the existing mux rule instead of double-subtracting.

Tests: `test_vulkan_post_process_settings.cpp`, `test_gamma_d3d12.cpp` (WARP pixels,
alpha, failed recording, abandonment/reuse), `test_frame_fence_failure.cpp`,
`test_mux_invariants.cpp`.

## Runtime override and environment ownership

The mapped custom Streamline module identifies its image for recursion/exclusion;
`RuntimePublication::Query` controls ready runtime redirection. The review's claimed
capture of takeover thunks as Streamline originals was not supported by the actual
export/trampoline baselines and IAT rebind path. Dynamic `slInit` OTA routing does
need to forward through an armed pending takeover; its atomic original is retargeted
without rewriting the raw dynamic registry baseline.

NGX diagnostic logging remains independent of CE `log_level`. Its environment
transaction records each previous value and restores only values CE still owns,
including path removal and deactivation. Native PEB locking makes the compare and
restore indivisible with Windows environment writes. Launcher and blacklisted roles
do not apply the game logging transaction.
Tests: `test_ngx_ota_policy.cpp`, `test_ngx_log_environment.cpp` (real environment),
`common/platform/owned_environment_value.h` ownership tests.

## Bounded PE access and crash records

`common/platform/loaded_pe_image.h` validates readable regions in the same allocation,
DOS/NT offsets, optional-header format, declared image/header sizes, section bounds,
export arrays and strings before callers read or patch. Used by cached pointer
retargeting, EAT handling and runtime diagnostic/unload ranges. The startup-import
fuzzer exercises synthetic mapped PE input as well as the existing import parser.
`fault_neighborhood_policy.h` saturates range/window arithmetic and rejects wrapped
relative references, including register values at the address-space maximum.

First-chance clear and context copy claim the slot before touching non-atomic payloads;
a reused slot is checked against the original thread identity after the claim.
A stale record surviving handled SEH unwind does not prove an ordinary non-zero exit
is fault-associated. That requires live exception dispatch; crash-like exit codes
have their separate gate. Verified fault-associated termination selects bounded
fatal/assert helper behavior. Tests: `test_loaded_pe_image.cpp`,
`test_crash_first_chance.cpp`, `test_crash_dump_policy.cpp`, `test_fault_neighborhood.cpp`.
