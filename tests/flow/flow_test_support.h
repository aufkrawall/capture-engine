#pragma once

// Helpers shared by the FG flow scenarios.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>

#include "tests/flow/flow_host.h"

namespace ce::flow {

inline std::string CurrentTestName() {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    return std::string(info->test_suite_name()) + "." + info->name();
}

// The inject overlay's contract: on every presented frame, drawn once. CE must also account every physical
// present: one its ledger never saw would show no overlay without counting as uncovered.
// The harness enables the display-gamma pass: it must have run (or been run by the frame's own route) for the
// scenario's frames, never failed and never skipped a frame for a reason the routing cannot name. The frames it
// leaves uncorrected (FG switches, DLSS-G start-up) are reported, not forbidden: the harness exercises exactly
// the transitions where CE keeps its work off the game queue.
// Display gamma is an SDR correction: a scenario that presented only HDR (HDR10, scRGB) must show no route applying it.
inline void ExpectPostProcessAccounted(const FlowGame& game) {
    const CEFlowPostProcess postProcess = game.PostProcess();
    if (game.PresentedSdr()) {
        EXPECT_GT(postProcess.corrected + postProcess.covered + postProcess.routeApplied, 0u)
            << "the post-process pass never ran or was covered in " << postProcess.frames << " frames; logs: "
            << game.LogDirectory();
    } else {
        EXPECT_EQ(postProcess.corrected + postProcess.routeApplied, 0u)
            << "display gamma ran on HDR output; logs: " << game.LogDirectory();
    }
    EXPECT_EQ(postProcess.failed + postProcess.routeFailed, 0u) << "logs: " << game.LogDirectory();
    EXPECT_EQ(postProcess.unclassified, 0u) << "logs: " << game.LogDirectory();
    std::printf("[  FLOW  ] post-process frames=%llu corrected=%llu covered=%llu uncorrected=%llu runs=%llu "
                "left-before-decision=%llu route-corrected=%llu\n",
                static_cast<unsigned long long>(postProcess.frames),
                static_cast<unsigned long long>(postProcess.corrected),
                static_cast<unsigned long long>(postProcess.covered),
                static_cast<unsigned long long>(postProcess.uncorrected),
                static_cast<unsigned long long>(postProcess.gapRuns),
                static_cast<unsigned long long>(postProcess.leftBeforeDecision),
                static_cast<unsigned long long>(postProcess.routeApplied));
}

// Every panel CE laid out in the default frame (its "[Overlay] Panel:" line) fits it, neither reflowed nor clipped:
// the frame and the pinned panel scale (kFrameWidth, flow_host.h) keep CE's default layout. Returns how many panel
// layouts in the default frame the hook log names (a dropped log line can only make it see fewer).
inline int ExpectPanelsFitTheDefaultFrame(const FlowGame& game) {
    std::ifstream log(game.LogDirectory() + "/hook_debug.log");
    int checked = 0;
    for (std::string line; std::getline(log, line);) {
        const size_t at = line.find("[Overlay] Panel: ");
        if (at == std::string::npos)
            continue;
        int layout = 0, viewportWidth = 0, viewportHeight = 0;
        float width = 0, height = 0, x = 0, y = 0, available = 0;
        if (std::sscanf(line.c_str() + at, "[Overlay] Panel: layout=%d size=%fx%f at=%f,%f viewport=%dx%d available=%f",
                        &layout, &width, &height, &x, &y, &viewportWidth, &viewportHeight, &available) != 8) {
            ADD_FAILURE() << "unreadable panel line: " << line;
            continue;
        }
        if (viewportWidth != static_cast<int>(kFrameWidth) || viewportHeight != static_cast<int>(kFrameHeight))
            continue;
        ++checked;
        EXPECT_LE(width, available) << "the panel reflowed in the default frame: " << line;
        EXPECT_LE(x + width, static_cast<float>(viewportWidth)) << "the panel left the default frame: " << line;
        EXPECT_LE(y + height, static_cast<float>(viewportHeight)) << "the panel left the default frame: " << line;
    }
    return checked;
}

// Only cold-start scenarios may supply the measured presents before CE's first visible draw. Later gaps must
// always remain in the ledger; the default contract still requires coverage from the very first physical present.
inline void ExpectEveryPresentCoveredOnce(const FlowGame& game, uint64_t coldStartupPresents = 0) {
    const CEFlowOverlayCoverage coverage = game.Coverage();
    EXPECT_EQ(coverage.presents + coldStartupPresents, game.PhysicalPresents()) << "logs: " << game.LogDirectory();
    EXPECT_EQ(coverage.uncovered, 0u) << "of " << coverage.presents << " presents; longest uncovered streak "
                                      << coverage.longestUncoveredStreak << "; logs: " << game.LogDirectory();
    EXPECT_EQ(coverage.doubleDraws, 0u) << "logs: " << game.LogDirectory();
    EXPECT_EQ(coverage.outputFrameMismatches, 0u)
        << "of " << coverage.outputFrameChecks << " runtime outputs; logs: " << game.LogDirectory();
    EXPECT_EQ(coverage.outputOwnerViolations, 0u)
        << "of " << coverage.outputFrameChecks << " runtime outputs; logs: " << game.LogDirectory();
    ExpectPostProcessAccounted(game);
    ExpectPanelsFitTheDefaultFrame(game);
}

// Debug-layer failures ExpectNoDebugLayerErrors already reported: the teardown check (flow_test_environment.cpp)
// reports only new ones.
inline uint64_t g_reportedDebugLayerFailures = 0;

// The game runs with the D3D12 debug layer (FlowGame::CreateDeviceAndSwapchain): nothing the game, CE or a fake
// runtime does may produce a CORRUPTION or ERROR message. Every message is in logs/<Suite.Test>/d3d12_debug.log.
// `requireWatched`: the scenario created its device, so the layer's messages must have been watched.
inline void ExpectNoDebugLayerErrors(const char* where = "scenario", bool requireWatched = true) {
    const D3D12DebugMessages messages = D3D12DebugMessagesSoFar();
    if (requireWatched) {
        EXPECT_TRUE(messages.watched) << where << ": the D3D12 debug layer's messages were not watched";
    }
    const uint64_t failures = messages.corruptions + messages.errors;
    EXPECT_EQ(failures, g_reportedDebugLayerFailures)
        << where << ": " << messages.corruptions << " CORRUPTION and " << messages.errors
        << " ERROR debug-layer messages; first: " << messages.firstFailure << "; log: " << messages.logPath;
    g_reportedDebugLayerFailures = failures;
}

// What the overlay shows: "<type> <multiplier>x" or off.
inline void ExpectPublished(const FlowGame& game, int type, int multiplier, const char* where) {
    const CEFlowPublishedFG published = game.PublishedFG();
    if (multiplier < 2) {
        EXPECT_LT(published.multiplier, 2) << where << "; logs: " << game.LogDirectory();
        return;
    }
    EXPECT_EQ(published.type, type) << where << "; logs: " << game.LogDirectory();
    EXPECT_EQ(published.multiplier, multiplier) << where << "; logs: " << game.LogDirectory();
}

// CE's own account of the overlay submission on this thread (CEFlow_DescribeD3D12OverlaySubmission).
inline std::string DescribeOverlaySubmission() {
    using Describe = int (*)(char*, int);
    const auto describe = reinterpret_cast<Describe>(
        GetProcAddress(GetModuleHandleW(L"capture_hook_x64.dll"), "CEFlow_DescribeD3D12OverlaySubmission"));
    char text[512] = {};
    if (!describe || describe(text, static_cast<int>(sizeof(text))) <= 0)
        return "CEFlow_DescribeD3D12OverlaySubmission unavailable";
    return text;
}

// No-callback FSR FG: the proven final-batch route draws every output after the runtime composed it (not AMD's UI
// resource at the rendered rate).
inline void ExpectFinalBatchRouteOwnsTheOutputs(const FlowGame& game, const char* where) {
    const std::string state = DescribeOverlaySubmission();
    EXPECT_NE(state.find("granted=1"), std::string::npos) << where << ": " << state << "; logs: " << game.LogDirectory();
}

}  // namespace ce::flow
