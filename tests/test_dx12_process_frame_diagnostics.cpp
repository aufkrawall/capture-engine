#include <gtest/gtest.h>

#include "../hook/common/dx12_process_frame_diagnostics.h"

namespace diagnostics = ce::dx12_process_frame_diagnostics;

TEST(DX12ProcessFrameDiagnosticsTest, StableInactiveActivityDoesNotOverlap) {
    const diagnostics::ConcurrentActivitySnapshot before{42, 0};
    const diagnostics::ConcurrentActivitySnapshot after{42, 0};

    EXPECT_FALSE(diagnostics::DidActivityOverlap(before, after));
}

TEST(DX12ProcessFrameDiagnosticsTest, DetectsActivitySpanningOrCompletingInsideCall) {
    EXPECT_TRUE(diagnostics::DidActivityOverlap({42, 1}, {42, 1}));
    EXPECT_TRUE(diagnostics::DidActivityOverlap({42, 1}, {43, 0}));
    EXPECT_TRUE(diagnostics::DidActivityOverlap({42, 0}, {44, 0}));
    EXPECT_TRUE(diagnostics::DidActivityOverlap({42, 0}, {43, 1}));
}

TEST(DX12ProcessFrameDiagnosticsTest, BreakdownSeparatesExternalInnerAndOverlayStages) {
    diagnostics::StageTimings timings;
    timings.totalUs = 14400;
    timings.innerUs = 12000;
    timings.captureUs = 100;
    timings.screenshotUs = 400;
    timings.overlayAcquireUs = 200;
    timings.overlayRecordUs = 9000;
    timings.overlaySubmitUs = 1500;
    timings.overlayPostSubmitUs = 100;

    const diagnostics::Breakdown breakdown = diagnostics::ComputeBreakdown(timings);

    EXPECT_EQ(breakdown.overlayUs, 10800);
    EXPECT_EQ(breakdown.innerOtherUs, 1100);
    EXPECT_EQ(breakdown.externalUs, 2000);
}

TEST(DX12ProcessFrameDiagnosticsTest, BreakdownClampsPartialOrOverlappingMeasurements) {
    diagnostics::StageTimings timings;
    timings.totalUs = 5000;
    timings.innerUs = 6000;
    timings.captureUs = 1000;
    timings.screenshotUs = 500;
    timings.overlayAcquireUs = -20;
    timings.overlayRecordUs = 4000;
    timings.overlaySubmitUs = 2000;
    timings.overlayPostSubmitUs = -1;

    const diagnostics::Breakdown breakdown = diagnostics::ComputeBreakdown(timings);

    EXPECT_EQ(breakdown.overlayUs, 6000);
    EXPECT_EQ(breakdown.innerOtherUs, 0);
    EXPECT_EQ(breakdown.externalUs, 0);
}
