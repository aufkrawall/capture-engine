#include <gtest/gtest.h>
#include "source_fragment_reader.h"

TEST(OverlayRetirementSourceTest, EveryCleanupAndReinitializationRetiresTheGpuGeneration) {
    const auto overlay = ce::test_source::ReadFile("hook/d3d12/dx12_hook_overlay.cpp");
    const auto state = ce::test_source::ReadFile("hook/d3d12/dx12_hook_types_impl.cpp");
    ASSERT_FALSE(overlay.empty());
    ASSERT_FALSE(state.empty());
    EXPECT_NE(overlay.find("RetireOverlayGpuResources(true, false)"), std::string::npos);
    EXPECT_NE(overlay.find("RetireOverlayGpuResources(false, true)"), std::string::npos);
    EXPECT_NE(state.find("RetireOverlayGpuResources(true, true)"), std::string::npos);
    EXPECT_EQ(overlay.find("WaitForSingleObject(waitEvent, 200)"), std::string::npos);
    EXPECT_EQ(overlay.find("WaitForSingleObject(dx12_hook_g_State.fenceEvent, 200)"), std::string::npos);
}

TEST(OverlayRetirementSourceTest, ShutdownCancelsAdmissionBeforeSharedResourcesAndRetainsGpuProof) {
    const auto source = ce::test_source::ReadFile("hook/d3d12/dx12_hook_main.cpp");
    const auto retire = ce::test_source::ReadFile("hook/d3d12/dx12_hook_overlay_retirement.cpp");
    const auto start = source.find("void DX12Hook::Shutdown()");
    ASSERT_NE(start, std::string::npos);
    EXPECT_EQ(source.find("dx12_hook_g_OverlayCompletionFence.exchange(nullptr", 0),
              source.find("dx12_hook_g_OverlayCompletionFence.exchange(nullptr", start));
    const auto cancel = source.find("g_PostSLLifecycle.InvalidateGeneration()", start);
    const auto lock = source.find("resources(dx12_hook_g_OverlayMutex)", start);
    const auto cleanup = source.find("CleanupResources()", start);
    const auto cleanupRtvs = source.find("CleanupRTVs()", start);
    const auto releaseCompletionFence = source.find("dx12_hook_g_OverlayCompletionFence.exchange(nullptr", start);
    EXPECT_LT(cancel, lock);
    EXPECT_LT(lock, cleanup);
    EXPECT_LT(cleanupRtvs, releaseCompletionFence);
    EXPECT_NE(retire.find("queue->Signal(state.fence, value)"), std::string::npos);
    EXPECT_NE(retire.find("state.gpuCompletionUnprovable = true"), std::string::npos);
    EXPECT_NE(retire.find("state.overlayCompletionValue"), std::string::npos);
    EXPECT_NE(retire.find("retired.RetainedCount()"), std::string::npos);
    EXPECT_NE(retire.find("retired.UnprovableCount()"), std::string::npos);
}
