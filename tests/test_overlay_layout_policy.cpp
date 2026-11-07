// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include <gtest/gtest.h>

#include "common/overlay/recording_indicator_policy.h"
#include "hook/overlay/overlay_layout_policy.h"

TEST(OverlayLayoutPolicyTest, FrameGenerationRowsAppearAndDisappearAtomicallyAcrossTransitions) {
    ce::overlay_layout::RowInputs input = {};
    input.showGPU = input.showCPU = input.showVRAM = input.showRAM = true;
    input.showFPS = input.showFPSAverages = input.showFG = true;

    const uint32_t offMask = ce::overlay_layout::BuildOverlayRowMask(input);
    EXPECT_EQ(ce::overlay_layout::CountOverlayRows(offMask), 6u);
    EXPECT_EQ(offMask & (ce::overlay_layout::kRowFGRates | ce::overlay_layout::kRowFGStatus), 0u);

    input.fgActive = true;  // OFF -> DLSS 2x; type/multiplier do not alter row presence.
    const uint32_t dlss2Mask = ce::overlay_layout::BuildOverlayRowMask(input);
    EXPECT_EQ(ce::overlay_layout::CountOverlayRows(dlss2Mask), 8u);
    EXPECT_NE(dlss2Mask & ce::overlay_layout::kRowFGRates, 0u);
    EXPECT_NE(dlss2Mask & ce::overlay_layout::kRowFGStatus, 0u);
    EXPECT_EQ(ce::overlay_layout::BuildOverlayRowMask(input), dlss2Mask);  // DLSS 4x / FSR 2x

    input.fgActive = false;
    EXPECT_EQ(ce::overlay_layout::BuildOverlayRowMask(input), offMask);
    input.reserveFGSpace = true;
    // Inactive FG must never reserve blank gap lines in the row mask
    EXPECT_EQ(ce::overlay_layout::BuildOverlayRowMask(input), offMask);

    input.recordingActive = true;
    input.showRecording = true;
    input.notificationVisible = true;
    const uint32_t expandedOffMask = ce::overlay_layout::BuildOverlayRowMask(input);
    EXPECT_EQ(ce::overlay_layout::CountOverlayRows(expandedOffMask), 8u);

    input.fgActive = true;
    const uint32_t expandedFGMask = ce::overlay_layout::BuildOverlayRowMask(input);
    EXPECT_EQ(ce::overlay_layout::CountOverlayRows(expandedFGMask), 10u);

    input.recordingActive = false;
    input.recordingStarting = true;
    EXPECT_EQ(ce::overlay_layout::BuildOverlayRowMask(input), expandedFGMask);

    input.showRecording = false;
    EXPECT_EQ(ce::overlay_layout::BuildOverlayRowMask(input) & ce::overlay_layout::kRowRecording, 0u);

    // Idle warning row
    input.recordingStarting = false;
    input.recordingActive = false;
    input.warnNotRecording = true;
    EXPECT_NE(ce::overlay_layout::BuildOverlayRowMask(input) & ce::overlay_layout::kRowRecording, 0u);

    input.warnNotRecording = false;
    EXPECT_EQ(ce::overlay_layout::BuildOverlayRowMask(input) & ce::overlay_layout::kRowRecording, 0u);
}

TEST(OverlayLayoutPolicyTest, WarningBlinkCycleMatchesTwoSecondsOnOneSecondOff) {
    EXPECT_TRUE(ce::recording_indicator::IsWarningBlinkVisible(0));
    EXPECT_TRUE(ce::recording_indicator::IsWarningBlinkVisible(1999));
    EXPECT_FALSE(ce::recording_indicator::IsWarningBlinkVisible(2000));
    EXPECT_FALSE(ce::recording_indicator::IsWarningBlinkVisible(2999));
    EXPECT_TRUE(ce::recording_indicator::IsWarningBlinkVisible(3000));
    EXPECT_TRUE(ce::recording_indicator::IsWarningBlinkVisible(5000, 1000));   // 4000ms elapsed -> 1000ms cycle -> true
    EXPECT_FALSE(ce::recording_indicator::IsWarningBlinkVisible(6500, 1000));  // 5500ms elapsed -> 2500ms cycle -> false
}

TEST(OverlayLayoutPolicyTest, SystemLatencyHasAnIndependentStableRow) {
    ce::overlay_layout::RowInputs input = {};

    EXPECT_EQ(ce::overlay_layout::BuildOverlayRowMask(input) & ce::overlay_layout::kRowSystemLatency, 0u);

    input.showSystemLatency = true;
    const uint32_t latencyMask = ce::overlay_layout::BuildOverlayRowMask(input);
    EXPECT_EQ(latencyMask, ce::overlay_layout::kRowSystemLatency);
    EXPECT_EQ(ce::overlay_layout::CountOverlayRows(latencyMask), 1u);

    input.showFPS = true;
    EXPECT_EQ(ce::overlay_layout::CountOverlayRows(ce::overlay_layout::BuildOverlayRowMask(input)), 2u);
}

TEST(OverlayLayoutPolicyTest, MemoryValuesNeverRequireFabricatedCapacity) {
    using ce::overlay_layout::MemoryValueMode;
    using ce::overlay_layout::SelectMemoryValueMode;

    EXPECT_EQ(SelectMemoryValueMode(false, 0, 0), MemoryValueMode::Unavailable);
    EXPECT_EQ(SelectMemoryValueMode(true, 0, 0), MemoryValueMode::UsedOnly);
    EXPECT_EQ(SelectMemoryValueMode(false, 8ull << 30, 0), MemoryValueMode::UsedOnly);
    EXPECT_EQ(SelectMemoryValueMode(true, 8ull << 30, 32ull << 30), MemoryValueMode::UsedAndTotal);
}
