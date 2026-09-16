#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <limits>
#include <string>

#include "../hook/common/ddraw_present_policy.h"
#include "source_fragment_reader.h"

namespace policy = ce::ddraw_present_policy;

namespace {

size_t CountOccurrences(const std::string& text, const std::string& needle) {
    size_t count = 0;
    for (size_t offset = 0; (offset = text.find(needle, offset)) != std::string::npos;
         offset += needle.size()) {
        ++count;
    }
    return count;
}

}  // namespace

TEST(DDrawPresentOverridePolicyTest, ParsesTheSharedVsyncVocabularyExactly) {
    EXPECT_EQ(policy::ParseVsyncRequest("fifo"), policy::VsyncRequest::Fifo);
    EXPECT_EQ(policy::ParseVsyncRequest("adaptive"), policy::VsyncRequest::Fifo);
    EXPECT_EQ(policy::ParseVsyncRequest("off"), policy::VsyncRequest::Immediate);
    EXPECT_EQ(policy::ParseVsyncRequest("mailbox"), policy::VsyncRequest::Immediate);
    EXPECT_EQ(policy::ParseVsyncRequest("default"), policy::VsyncRequest::ApplicationControlled);
    EXPECT_EQ(policy::ParseVsyncRequest("FIFO"), policy::VsyncRequest::ApplicationControlled);
}

TEST(DDrawPresentOverridePolicyTest, FifoFlipSelectsOneReliableSynchronizedInterval) {
    constexpr uint32_t applicationBit = 0x00001000u;
    const uint32_t original = applicationBit | policy::kFlipNoVsync | policy::kFlipDoNotWait | 0x03000000u;
    const uint32_t rewritten =
        policy::ApplyVsyncFlags(original, policy::PresentOperation::Flip, policy::VsyncRequest::Fifo);
    EXPECT_EQ(rewritten & policy::kFlipNoVsync, 0u);
    EXPECT_EQ(rewritten & policy::kFlipDoNotWait, 0u);
    EXPECT_EQ(rewritten & policy::kFlipIntervalMask, 0u);
    EXPECT_NE(rewritten & policy::kFlipWait, 0u);
    EXPECT_NE(rewritten & applicationBit, 0u);
}

TEST(DDrawPresentOverridePolicyTest, ImmediateFlipPreservesTheApplicationsBusyQueuePolicy) {
    const uint32_t original = policy::kFlipWait | policy::kFlipDoNotWait | 0x02000000u;
    const uint32_t rewritten =
        policy::ApplyVsyncFlags(original, policy::PresentOperation::Flip, policy::VsyncRequest::Immediate);
    EXPECT_NE(rewritten & policy::kFlipNoVsync, 0u);
    EXPECT_NE(rewritten & policy::kFlipWait, 0u);
    EXPECT_NE(rewritten & policy::kFlipDoNotWait, 0u);
    EXPECT_EQ(rewritten & policy::kFlipIntervalMask, 0u);
}

TEST(DDrawPresentOverridePolicyTest, FifoBltCannotBeRejectedByAnAlreadyBusyFifo) {
    constexpr uint32_t presentationBit = 0x10000000u;
    const uint32_t original = presentationBit | policy::kBltAsync | policy::kBltDoNotWait;
    const uint32_t rewritten =
        policy::ApplyVsyncFlags(original, policy::PresentOperation::Blt, policy::VsyncRequest::Fifo);
    EXPECT_EQ(rewritten & policy::kBltAsync, 0u);
    EXPECT_EQ(rewritten & policy::kBltDoNotWait, 0u);
    EXPECT_NE(rewritten & policy::kBltWait, 0u);
    EXPECT_NE(rewritten & presentationBit, 0u);
}

TEST(DDrawPresentOverridePolicyTest, FifoBltFastWaitsForSubmissionWithoutChangingPixels) {
    constexpr uint32_t sourceColorKey = 0x00000001u;
    const uint32_t original = sourceColorKey | policy::kBltFastDoNotWait;
    const uint32_t rewritten =
        policy::ApplyVsyncFlags(original, policy::PresentOperation::BltFast, policy::VsyncRequest::Fifo);
    EXPECT_EQ(rewritten & policy::kBltFastDoNotWait, 0u);
    EXPECT_NE(rewritten & policy::kBltFastWait, 0u);
    EXPECT_NE(rewritten & sourceColorKey, 0u);
}

TEST(DDrawPresentOverridePolicyTest, NonFifoBlitsPreserveTheApplicationsSubmissionContract) {
    const uint32_t bltFlags = policy::kBltAsync | policy::kBltDoNotWait;
    EXPECT_EQ(policy::ApplyVsyncFlags(bltFlags, policy::PresentOperation::Blt,
                                     policy::VsyncRequest::Immediate),
              bltFlags);
    EXPECT_EQ(policy::ApplyVsyncFlags(bltFlags, policy::PresentOperation::Blt,
                                     policy::VsyncRequest::ApplicationControlled),
              bltFlags);
}

TEST(DDrawPresentOverridePolicyTest, OnlyFifoBlitsNeedAnExplicitVerticalBlankBoundary) {
    EXPECT_TRUE(policy::NeedsExplicitVblankWait(policy::VsyncRequest::Fifo,
                                                policy::PresentOperation::Blt, false));
    EXPECT_TRUE(policy::NeedsExplicitVblankWait(policy::VsyncRequest::Fifo,
                                                policy::PresentOperation::BltFast, false));
    EXPECT_FALSE(policy::NeedsExplicitVblankWait(policy::VsyncRequest::Fifo,
                                                 policy::PresentOperation::Flip, false));
    EXPECT_FALSE(policy::NeedsExplicitVblankWait(policy::VsyncRequest::Immediate,
                                                 policy::PresentOperation::Blt, false));
    EXPECT_FALSE(policy::NeedsExplicitVblankWait(policy::VsyncRequest::Fifo,
                                                 policy::PresentOperation::Blt, true));
}

TEST(DDrawPresentOverridePolicyTest, ReusesOnlySynchronousApplicationVblankBeginWaits) {
    EXPECT_TRUE(policy::IsReusableVblankBeginWait(policy::kWaitVblankBlockBegin));
    EXPECT_FALSE(policy::IsReusableVblankBeginWait(policy::kWaitVblankBlockEnd));
    EXPECT_FALSE(policy::IsReusableVblankBeginWait(policy::kWaitVblankBlockBeginEvent));
    EXPECT_FALSE(policy::IsReusableVblankBeginWait(policy::kWaitVblankBlockBegin |
                                                   policy::kWaitVblankBlockBeginEvent));
    EXPECT_FALSE(policy::IsReusableVblankBeginWait(policy::kWaitVblankBlockBegin |
                                                   policy::kWaitVblankBlockEnd));
    EXPECT_FALSE(policy::IsReusableVblankBeginWait(0));
}

TEST(DDrawPresentOverridePolicyTest, AcceptsOnlyTheConfiguredIntegerQueueDepthContract) {
    EXPECT_EQ(policy::ResolvePrerenderQueueDepth(-1.0f), -1);
    EXPECT_EQ(policy::ResolvePrerenderQueueDepth(0.0f), 0);
    EXPECT_EQ(policy::ResolvePrerenderQueueDepth(1.0f), 1);
    EXPECT_EQ(policy::ResolvePrerenderQueueDepth(6.0f), 6);
    EXPECT_EQ(policy::ResolvePrerenderQueueDepth(0.5f), -1);
    EXPECT_EQ(policy::ResolvePrerenderQueueDepth(-0.5f), -1);
    EXPECT_EQ(policy::ResolvePrerenderQueueDepth(7.0f), -1);
    EXPECT_EQ(policy::ResolvePrerenderQueueDepth(std::nanf("")), -1);
    EXPECT_EQ(policy::ResolvePrerenderQueueDepth(std::numeric_limits<float>::infinity()), -1);
}

TEST(DDrawPresentOverridePolicyTest, EveryHookedFlipAndFullSurfaceBlitUsesTheSharedOwner) {
    const std::filesystem::path root = std::filesystem::current_path();
    // The whole DirectDraw hook family: the Surface4 generation has its own
    // translation unit, and its three presentations use the same owner.
    const std::string detours =
        ce::test_source::ReadLogicalSource(root / "hook" / "apis" / "ddraw_hook.cpp");
    const std::string overrides = ce::test_source::ReadFile(
        root / "hook" / "apis" / "ddraw_hook_present_overrides.cpp");
    const std::string install =
        ce::test_source::ReadFile(root / "hook" / "apis" / "ddraw_hook_install.cpp");
    ASSERT_FALSE(detours.empty());
    ASSERT_FALSE(overrides.empty());
    ASSERT_FALSE(install.empty());

    EXPECT_EQ(CountOccurrences(detours, "DirectDrawPresentationOverrideScope presentationOverride"), 9u);
    EXPECT_EQ(CountOccurrences(detours, "presentationOverride.PrepareForCall()"), 9u);
    EXPECT_EQ(CountOccurrences(detours, "presentationOverride.Complete(hr)"), 9u);
    EXPECT_EQ(detours.find("ApplyPrerenderLimitDDraw"), std::string::npos);
    EXPECT_NE(overrides.find("GetFlipStatus(DDGFS_ISFLIPDONE)"), std::string::npos);
    EXPECT_NE(overrides.find("GetBltStatus(DDGBS_ISBLTDONE)"), std::string::npos);
    EXPECT_NE(overrides.find("IndexedDirectDrawWaitForVerticalBlankDetour"), std::string::npos);
    EXPECT_NE(overrides.find("kWaitDetours[recordIndex]"), std::string::npos);
    EXPECT_EQ(overrides.find("ResolveWaitOriginal"), std::string::npos);
    EXPECT_EQ(CountOccurrences(install, "InstallDirectDrawWaitForVerticalBlankHook("), 3u);
}
