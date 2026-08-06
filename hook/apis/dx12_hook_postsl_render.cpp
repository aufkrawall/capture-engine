#include "dx12_hook_internal.h"
#include "dx12_hook_postsl_session.h"

std::atomic<int> s_postSLRenders{0};
std::atomic<int> s_postSLSkipFence{0};
int s_reactivationEpoch{0};
int s_callsSinceReactivation{0};
int s_postSLProbeFrames{0};

void PostSLOverlayRender(IDXGISwapChain* pSwapChain) {
    PostSLRenderSession session(pSwapChain);
    session.Run();
}

void PostSLRenderSession::Run() {
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
}
