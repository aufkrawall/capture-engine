#include "dx12_hook_internal.h"
#include "hook/runtime/hook_clock.h"
#include "dx12_hook_process_session.h"

void ProcessFrame(IDXGISwapChain* pSwapChain, bool processCapture, bool applicationSourcePresent,
                  bool frameGenerationPresentationActive,
                  ce::dx12_process_frame_diagnostics::StageTimings* diagnostics) {
    static thread_local bool s_inProcessFrame = false;
    if (s_inProcessFrame) {
        if (diagnostics) {
            diagnostics->reentrantInnerSkipped = true;
        }
            return;
    }
    s_inProcessFrame = true;
    auto reentryGuard = ce::make_scope_guard([&]() { s_inProcessFrame = false; });
    dx12_hook_g_LastProcessFrameTickMs.store(ce::hook_clock::TickCount64(), std::memory_order_release);
    CleanupDeferredPostSLQueuesIfSafe("DX12: ProcessFrame deferred PostSL cleanup");
    static bool s_firstFrame = true;
    if (s_firstFrame) {
        s_firstFrame = false;
        HookLog("DX12: ProcessFrame FIRST CALL (swapchain=%p)", (void*)pSwapChain);
        HookLogImportant(
            "DX12 focus-loss sync policy=v13 draw-every-frame + x86 solid-span text + upload-slot per-frame fence "
            "(overlay never hidden on focus; x86 text avoids CE-owned font SRV sampling; upload slot reuse remains "
            "gated on the overlay fence)");
    }
    FrameProcessSession session(pSwapChain, processCapture, applicationSourcePresent,
                               frameGenerationPresentationActive, diagnostics);
    session.Run();
}

void FrameProcessSession::Run() {
    auto metricsCompletion = ce::make_scope_guard([&] { if (metricsGuardArmed) LogFrameMetrics(); });
    auto postProcessLedger = ce::make_scope_guard([&] { NotePostProcessOutcome(); });
    ProcessFrameFlow flow = ProcessFrameFlow::kContinue;
    flow = PrepareFrame();
    if (flow == ProcessFrameFlow::kReturn) {
        return;
    }
    flow = TrackSwapchainAndSelectQueue();
    if (flow == ProcessFrameFlow::kReturn) {
        return;
    }
    // This renderer owns an independent backbuffer/queue/completion contract. Run it before InitOverlayBackend: once the
    // transition cooldown reaches zero, InitOverlayBackend skips normal-backend initialization while runtime-owned FSR
    // presentation is active (the frame continues only for capture). The route must not depend on anything InitOverlayBackend
    // or later decides, or the proven topmost owner retires and the callback baseline resumes, changing the
    // translucent overlay blend and putting foreign overlays above CE again.
    if (allowOverlayRender && !suspendOverlayRender && !dx12_hook_s_insideECL) {
        independentFSRTopmostCompositedThisPresent =
            TryCompositeOverlayBelowForeignChainForRuntimeOwnedFSR();
    }
    // The post-process pass does not need the overlay. While overlay init is deferred or backing off
    // (startup grace, resume settle, repeated init failures) it keeps correcting frames, but only in
    // states where CE work on the game queue is as safe as in a plain game.
    flow = InitOverlayBackend();
    if (flow == ProcessFrameFlow::kReturn) {
        RunPostProcessWhileOverlayUnavailable();
        return;
    }
    flow = InitOverlaySyncAndFocusHold();
    if (flow == ProcessFrameFlow::kReturn) {
        RunPostProcessWhileOverlayUnavailable();
        return;
    }
    flow = HandleOuterFGTransition();
    if (flow == ProcessFrameFlow::kReturn) {
        return;
    }
    flow = DrawOverlayFrame();
    if (flow == ProcessFrameFlow::kReturn) {
        return;
    }
    flow = PublishPostOverlayCapture();
    if (flow == ProcessFrameFlow::kReturn) {
        return;
    }
}

void FrameProcessSession::LogFrameMetrics() {
if (activeDebugSample) {
    activeDebugSample->processFrameUs = static_cast<int32_t>(PerfLogger::GetQpcUs() - processFrameStartUs);
    activeDebugSample->captureUs = perfMetrics.captureUs;
}
if (PerfLogger::Get().IsEnabled()) {
    perfMetrics.totalUs = static_cast<int32_t>((PerfLogger::GetQpcUs() - perfMetrics.qpcUs));
    perfMetrics.fpsLimitWaitUs = static_cast<int32_t>(g_SharedFpsLimiter.GetLastWaitUs());
    PerfLogger::Get().LogFrame(perfMetrics);
}
}
