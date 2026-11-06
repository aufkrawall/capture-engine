// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include <gtest/gtest.h>

// Exercise the codec's actual policy with fake allocation effects. Prebuilt
// FFmpeg hosts do not have the private source-built implementation.
#if __has_include("../ffmpeg_build/working/ffmpeg/libavcodec/aacenc_nmr_budget.h")
extern "C" {
#include "../ffmpeg_build/working/ffmpeg/libavcodec/aacenc_nmr_budget.h"
}
#include <vector>

namespace {
struct Allocation {
    int bits = 16000;
    int coarsestBits = 13000;
    int coarsenCalls = 0;
    int removalCalls = 0;
    std::vector<int> afterRemoval{13040, 11000};

    AACNMRBudgetOps Ops() { return {this, Measure, Coarsen, Remove}; }
    static int Measure(void* context) { return static_cast<Allocation*>(context)->bits; }
    static void Coarsen(void* context) {
        auto& allocation = *static_cast<Allocation*>(context);
        ++allocation.coarsenCalls;
        allocation.bits = allocation.coarsestBits;
    }
    static int Remove(void* context) {
        auto& allocation = *static_cast<Allocation*>(context);
        if (allocation.removalCalls >= static_cast<int>(allocation.afterRemoval.size()))
            return 0;
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
    EXPECT_EQ(result.coarsened, 0);
    EXPECT_EQ(allocation.coarsenCalls, 0);
    EXPECT_EQ(allocation.removalCalls, 0);
}

TEST(AacNmrBudgetTest, CoarsestGridCannotFitAndBandRemovalMustMakeProgress) {
    Allocation allocation;
    const auto ops = allocation.Ops();
    const auto result = ff_aac_nmr_fit_budget(&ops, 11600, 256);
    EXPECT_EQ(result.bits, 11000);
    EXPECT_EQ(result.coarsened, 1);
    EXPECT_EQ(result.removed, 2);
    // Repairing a scalefactor edge can initially increase the cost.
    EXPECT_EQ(allocation.coarsenCalls, 1);
    EXPECT_EQ(allocation.removalCalls, 2);
}

TEST(AacNmrBudgetTest, CoarseningAlonePreservesEveryBand) {
    Allocation allocation;
    allocation.coarsestBits = 10000;
    const auto ops = allocation.Ops();
    const auto result = ff_aac_nmr_fit_budget(&ops, 11600, 256);
    EXPECT_EQ(result.bits, 10000);
    EXPECT_EQ(result.removed, 0);
    EXPECT_EQ(allocation.removalCalls, 0);
}

TEST(AacNmrBudgetTest, ExhaustedBandsReturnTheRemainingCostWithoutLooping) {
    Allocation allocation;
    allocation.afterRemoval.clear();
    const auto ops = allocation.Ops();
    const auto result = ff_aac_nmr_fit_budget(&ops, 512, 256);
    EXPECT_EQ(result.bits, 13000);
    EXPECT_EQ(result.removed, 0);
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
#else
TEST(AacNmrBudgetTest, RequiresSourceBuiltCodec) {
    GTEST_SKIP() << "Private NMR allocation policy requires the source-built FFmpeg tree";
}
#endif
