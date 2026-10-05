#include "dx12_hook_internal.h"
#include "dx12_hook_postsl_session.h"
#include "common/logging/log_meter.h"

std::atomic<int> s_postSLRenders{0};
std::atomic<int> s_postSLSkipFence{0};
std::atomic<int> s_postSLSkipLock{0};
int s_reactivationEpoch{0};
int s_callsSinceReactivation{0};
int s_postSLProbeFrames{0};

void PostSLOverlayRender(IDXGISwapChain* pSwapChain, uint32_t admissionEpoch) {
    PostSLRenderSession session(pSwapChain, admissionEpoch);
    session.Run();
}

void PostSLRenderSession::Run() {
    if (!submissionResources.RenderTransaction(g_PostSLLifecycle, entryLifecycleEpoch, [&](uint32_t epoch) {
        entryLifecycleEpoch = epoch;
        PostSLFlow flow = PostSLFlow::kContinue;
        flow = PrepareRouteActivation();
        if (flow == PostSLFlow::kReturn) {
            return;
        }
        flow = AcquireSubmissionResources();
        if (flow == PostSLFlow::kReturn) {
            return;
        }
        flow = RecordOverlayDraw();
        if (flow == PostSLFlow::kReturn) {
            return;
        }
        flow = SubmitAndConfirmOutput();
        if (flow == PostSLFlow::kReturn) {
            return;
        }
    })) {
        DX12_NoteSkippedStreamlineFinalOutput();
        s_postSLSkipLock.fetch_add(1, std::memory_order_relaxed);
        NoteDX12OverlayCoverageGate("postsl-render-lock");
        const auto currentEpoch = g_PostSLLifecycle.Epoch();
        const bool retired = currentEpoch != entryLifecycleEpoch;
        static ce::log_meter::ChangeGate admissionFailureGate;
        const auto verdict = admissionFailureGate.Observe(ce::log_meter::FieldKey(entryLifecycleEpoch, currentEpoch, retired));
        if (verdict) {
            HookLogImportant("[PostSLLifecycle] admission=rejected reason=%s entryEpoch=%u epoch=%u tid=0x%04X%s",
                retired ? "retired-generation" : "render-busy", entryLifecycleEpoch, currentEpoch, GetCurrentThreadId(),
                ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
        }
    }
}
