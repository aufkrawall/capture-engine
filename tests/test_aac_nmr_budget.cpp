// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include <gtest/gtest.h>

// Exercise the codec's actual policy with fake allocation effects. Prebuilt
// FFmpeg hosts do not have the private source-built implementation.
#if __has_include("../ffmpeg_build/working/ffmpeg/libavcodec/aacenc_nmr_budget.h")
extern "C" {
#include "../ffmpeg_build/working/ffmpeg/libavcodec/aacenc_nmr_budget.h"
}
#include <string>
#include <vector>

namespace {
// Each repair step sets the cost it achieves; the trace records the order.
struct Allocation {
    int bits = 16000;
    int currentGridBits = 14000;
    int reopenedBits = 13000;
    std::vector<int> afterRemoval{13040, 11000};
    int removalCalls = 0;
    std::string trace;

    AACNMRBudgetOps Ops() { return {this, Measure, FitCurrent, Reopen, Remove}; }
    static Allocation& Of(void* context) { return *static_cast<Allocation*>(context); }
    static int Measure(void* context) { return Of(context).bits; }
    static void FitCurrent(void* context) {
        Of(context).trace += "fit,";
        Of(context).bits = Of(context).currentGridBits;
    }
    static void Reopen(void* context) {
        Of(context).trace += "reopen,";
        Of(context).bits = Of(context).reopenedBits;
    }
    static int Remove(void* context) {
        auto& allocation = Of(context);
        if (allocation.removalCalls >= static_cast<int>(allocation.afterRemoval.size()))
            return 0;
        allocation.trace += "remove,";
        allocation.bits = allocation.afterRemoval[allocation.removalCalls++];
        return 1;
    }
};
}  // namespace

TEST(AacNmrBudgetTest, LegalAllocationKeepsItsQualityDecisions) {
    Allocation allocation;
    allocation.bits = 10000;
    const auto ops = allocation.Ops();
    const auto result = ff_aac_nmr_fit_budget(&ops, 11600, 256);
    EXPECT_EQ(result.bits, 10000);
    EXPECT_EQ(result.initial_bits, 10000);
    EXPECT_EQ(result.refit, 0);
    EXPECT_EQ(allocation.trace, "");
}

TEST(AacNmrBudgetTest, CurrentWindowFitKeepsTheSolveAndEveryBand) {
    Allocation allocation;
    allocation.currentGridBits = 11500;
    const auto ops = allocation.Ops();
    const auto result = ff_aac_nmr_fit_budget(&ops, 11600, 256);
    EXPECT_EQ(result.initial_bits, 16000);
    EXPECT_EQ(result.bits, 11500);
    EXPECT_EQ(result.refit, 1);
    EXPECT_EQ(result.reopened, 0);
    EXPECT_EQ(result.removed, 0);
    EXPECT_EQ(allocation.trace, "fit,");
}

TEST(AacNmrBudgetTest, ReopenedRangeFitsBeforeAnyBandIsRemoved) {
    Allocation allocation;
    allocation.reopenedBits = 10000;
    const auto ops = allocation.Ops();
    const auto result = ff_aac_nmr_fit_budget(&ops, 11600, 256);
    EXPECT_EQ(result.bits, 10000);
    EXPECT_EQ(result.reopened, 1);
    EXPECT_EQ(result.removed, 0);
    EXPECT_EQ(allocation.trace, "fit,reopen,");
}

TEST(AacNmrBudgetTest, RemovalStartsOnlyAfterTheMinimumCostStillExceedsTheCap) {
    Allocation allocation;
    const auto ops = allocation.Ops();
    const auto result = ff_aac_nmr_fit_budget(&ops, 11600, 256);
    EXPECT_EQ(result.bits, 11000);
    EXPECT_EQ(result.removed, 2);
    // Repairing a scalefactor edge can initially increase the cost.
    EXPECT_EQ(allocation.trace, "fit,reopen,remove,remove,");
}

TEST(AacNmrBudgetTest, ExhaustedBandsReturnTheRemainingCostWithoutLooping) {
    Allocation allocation;
    allocation.afterRemoval.clear();
    const auto ops = allocation.Ops();
    const auto result = ff_aac_nmr_fit_budget(&ops, 512, 256);
    EXPECT_EQ(result.bits, 13000);
    EXPECT_EQ(result.removed, 0);
    EXPECT_EQ(allocation.trace, "fit,reopen,");
}

TEST(AacNmrBudgetTest, WorkIsBoundedEvenIfTheAllocatorNeverLowersItsCost) {
    Allocation allocation;
    allocation.afterRemoval.assign(300, 13000);
    const auto ops = allocation.Ops();
    const auto result = ff_aac_nmr_fit_budget(&ops, 512, 256);
    EXPECT_EQ(result.bits, 13000);
    EXPECT_EQ(result.removed, 256);
    EXPECT_EQ(allocation.removalCalls, 256);
}

TEST(AacNmrFrameActionTest, LegalFrameIsAcceptedOnEveryPass) {
    float scale = 1.0f;
    EXPECT_EQ(ff_aac_nmr_frame_action(12284, 12284, 1, &scale), AAC_NMR_FRAME_ACCEPT);
    EXPECT_EQ(ff_aac_nmr_frame_action(12284, 12284, AAC_NMR_MAX_PASSES, &scale), AAC_NMR_FRAME_ACCEPT);
    EXPECT_EQ(scale, 1.0f);
}

TEST(AacNmrFrameActionTest, OverflowShrinksLambdaByAtLeastTenPercent) {
    float scale = 1.0f;
    // Barely over: the bound ratio alone would barely move the caps.
    EXPECT_EQ(ff_aac_nmr_frame_action(12300, 12284, 1, &scale), AAC_NMR_FRAME_RETRY);
    EXPECT_FLOAT_EQ(scale, 0.9f);
    // Far over: shrink in proportion to the overflow.
    EXPECT_EQ(ff_aac_nmr_frame_action(24568, 12284, 1, &scale), AAC_NMR_FRAME_RETRY);
    EXPECT_FLOAT_EQ(scale, 0.5f);
}

TEST(AacNmrFrameActionTest, StrictRateBoundRetriesLikeTheDecoderBound) {
    float scale = 1.0f;
    EXPECT_EQ(ff_aac_nmr_frame_action(4000, 2000, 3, &scale), AAC_NMR_FRAME_RETRY);
    EXPECT_FLOAT_EQ(scale, 0.5f);
}

TEST(AacNmrFrameActionTest, NonConvergenceEndsInALegalSilentFrameNotAnError) {
    float scale = 1.0f;
    EXPECT_EQ(ff_aac_nmr_frame_action(13000, 12284, AAC_NMR_MAX_PASSES - 1, &scale), AAC_NMR_FRAME_RETRY);
    EXPECT_EQ(ff_aac_nmr_frame_action(13000, 12284, AAC_NMR_MAX_PASSES, &scale), AAC_NMR_FRAME_SILENCE);
    // A shrink of at least 10% per pass reaches the 256-bit cap floor well
    // before the bound: 5800 * 0.9^(passes - 1) < 256.
    float cap = 5800.0f;
    for (int pass = 1; pass < AAC_NMR_MAX_PASSES; ++pass)
        cap *= 0.9f;
    EXPECT_LT(cap, 256.0f);
}
#else
TEST(AacNmrBudgetTest, RequiresSourceBuiltCodec) {
    GTEST_SKIP() << "Private NMR allocation policy requires the source-built FFmpeg tree";
}
#endif
