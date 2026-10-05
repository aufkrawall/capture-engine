#include "dx12_hook_internal.h"
#include "dx12_hook_postsl_session.h"

std::atomic<int> s_postSLRenders{0};
std::atomic<int> s_postSLSkipFence{0};
std::atomic<int> s_postSLSkipLock{0};
int s_reactivationEpoch{0};
int s_callsSinceReactivation{0};
int s_postSLProbeFrames{0};

void PostSLOverlayRender(IDXGISwapChain* pSwapChain) {
    PostSLRenderSession session(pSwapChain);
    session.Run();
}

void PostSLRenderSession::Run() {
    if (!g_PostSLLifecycle.RenderTransaction([&](uint32_t epoch) {
        entryLifecycleEpoch = epoch;
        PostSLFlow flow = PostSLFlow::kContinue;
        flow = Chunk0();
        if (flow == PostSLFlow::kReturn) {
            return;
        }
        flow = Chunk1();
        if (flow == PostSLFlow::kReturn) {
            return;
        }
        flow = Chunk2();
        if (flow == PostSLFlow::kReturn) {
            return;
        }
        flow = Chunk3();
        if (flow == PostSLFlow::kReturn) {
            return;
        }
    })) {
        DX12_NoteSkippedStreamlineFinalOutput();
        s_postSLSkipLock.fetch_add(1, std::memory_order_relaxed);
        NoteDX12OverlayCoverageGate("postsl-render-lock");
        static std::atomic<int> s_lockSkip{0};
        if (s_lockSkip.fetch_add(1, std::memory_order_relaxed) < 10)
            HookLogImportant("DX12: PostSL SKIP — another thread already rendering (tid=0x%04X)", GetCurrentThreadId());
    }
}
