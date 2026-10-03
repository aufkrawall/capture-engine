#include <gtest/gtest.h>

#include "hook/d3d12/dx12_overlay_policy.h"

// With DLSS frame generation on, the PostSL overlay draws onto Streamline's buffer inside the runtime's own
// Present, where D3D12 requires it in PRESENT: on the queue that presents it the overlay list transitions
// PRESENT <-> RENDER_TARGET (DecidePostSLBackbufferBarrierMode, test_dxgi_shared_part7.cpp); on any other queue
// a transition races the runtime (GTA V DEVICE_HUNG, 2026-03). The D3D12 debug layer in the FG flow tests
// reported the old UAV-only draw in PRESENT on every frame (2026-10-03).

namespace {

// PostSLFGSubmitRunsOnPresentingQueue mirrors the submit chain's target queue in its order.
TEST(Dx12PostSLBarrierModePolicyTest, PresentingQueueFollowsTheSubmitChain) {
    using ce::dx12_overlay_policy::PostSLBootstrapSubmitPath;
    using ce::dx12_overlay_policy::PostSLFGSubmitRunsOnPresentingQueue;
    constexpr auto kOriginal = PostSLBootstrapSubmitPath::kSelectedQueueOriginal;
    constexpr auto kWrapper = PostSLBootstrapSubmitPath::kWrapperOrVirtual;
    constexpr auto kReject = PostSLBootstrapSubmitPath::kReject;

    // Selected-queue paths run on the selected queue: presenting only when it is the swapchain's own.
    EXPECT_TRUE(PostSLFGSubmitRunsOnPresentingQueue(true, false, true, true, kWrapper, true));
    EXPECT_FALSE(PostSLFGSubmitRunsOnPresentingQueue(true, true, false, false, kOriginal, false));
    // The swapchain queue's virtual submit runs on the presenting queue whatever was selected.
    EXPECT_TRUE(PostSLFGSubmitRunsOnPresentingQueue(false, true, false, true, kWrapper, true));
    // The real queue behind Streamline's wrapper and the wrapper bootstrap run elsewhere.
    EXPECT_FALSE(PostSLFGSubmitRunsOnPresentingQueue(false, false, true, true, kOriginal, false));
    EXPECT_FALSE(PostSLFGSubmitRunsOnPresentingQueue(false, false, true, false, kWrapper, true));
    // Without a wrapper queue the bootstrap submits on the selected queue; a rejected bootstrap submits nothing.
    EXPECT_TRUE(PostSLFGSubmitRunsOnPresentingQueue(false, false, true, false, kWrapper, false));
    EXPECT_TRUE(PostSLFGSubmitRunsOnPresentingQueue(false, false, true, false, kOriginal, true));
    EXPECT_FALSE(PostSLFGSubmitRunsOnPresentingQueue(false, false, false, false, kOriginal, false));
    EXPECT_FALSE(PostSLFGSubmitRunsOnPresentingQueue(false, false, true, false, kReject, false));
}

}  // namespace
