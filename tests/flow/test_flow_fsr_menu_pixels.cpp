// Pixel observations through DLSS -> retained FSR proxy transitions, menu suspension,
// callback/direct FSR topologies, panel single-blend alpha, text updates, and graph motion.
#include <algorithm>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "tests/flow/fakes/output_pixel_probe.h"
#include "tests/flow/graph_motion_oracle.h"
#include "tests/flow/flow_test_support.h"

namespace {
using namespace ce::flow;
using ce::flow::fake::OutputPanelObservation;
namespace fake = ce::flow::fake;
namespace graph_motion = ce::flow::graph_motion;

// The panel's pixels are judged here: it is drawn at 150% with CE's default font in a 640x360 frame, where the graph
// moves ~2.7 px per output and clears the Present-chain stand-in's 16 px corner. The history point lies inside the
// game's HUD element in the frame's bottom-right corner (fsrTestHudWindow, flow_host_fidelityfx.cpp), the region over
// the panel's side of the frame.
constexpr UINT kPixelFrameWidth = 640;
constexpr UINT kPixelFrameHeight = 360;
constexpr UINT kHudPointX = kPixelFrameWidth - 10;
constexpr UINT kHudPointY = kPixelFrameHeight - 10;
constexpr UINT kPanelRegion[4] = {15, 15, 450, 340};

void UsePixelOracleGeometry(GameOptions& options) {
    options.width = kPixelFrameWidth;
    options.height = kPixelFrameHeight;
    options.overlayDpiScale = 1.5f;
    options.overlayFontSize = 0.0f;
    options.fullPostProcess = true;  // the scene and HUD thresholds are those of the corrected image
}

struct PixelOracleReader {
    std::function<uint64_t(uint64_t, OutputPanelObservation*)> readObs;

    std::vector<OutputPanelObservation> Collect(uint64_t begin, uint64_t end) const {
        std::vector<OutputPanelObservation> out;
        for (uint64_t i = begin; i < end; ++i) {
            OutputPanelObservation obs{};
            const uint64_t total = readObs(i, &obs);
            EXPECT_GE(total, end);
            out.push_back(obs);
        }
        return out;
    }
};

// The graph's motion between consecutive outputs of a phase (graph_motion_oracle.h).
std::vector<graph_motion::Shift> GraphSteps(const std::vector<OutputPanelObservation>& frames) {
    std::vector<graph_motion::Shift> steps;
    for (size_t i = 1; i < frames.size(); ++i)
        steps.push_back(graph_motion::EstimateShift(frames[i - 1].graphProfile, frames[i].graphProfile,
                                                    fake::kGraphProfileColumns, 16));
    return steps;
}

// `handoverOutputs`: an enable begins with the UI baseline drawing the overlay once per game frame, onto the copy of
// the UI resource AMD composes into both of that frame's outputs, until the final-batch route proved itself and draws
// each output (make before break, ffx_output_frames.h). Within that many outputs, two outputs of one game frame may show
// the same graph; nowhere else.
void VerifyPhasePixels(const char* phaseName, const std::vector<OutputPanelObservation>& frames, bool expectHud,
                       bool checkTextChanges, bool checkGraphMotion, size_t handoverOutputs = 0) {
    SCOPED_TRACE(phaseName);
    ASSERT_FALSE(frames.empty());
    std::set<uint64_t> textHashes;
    for (size_t i = 0; i < frames.size(); ++i) {
        const auto& obs = frames[i];
        SCOPED_TRACE(i);
        ASSERT_EQ(obs.hasPanel, 1u) << "missing overlay panel on frame " << obs.frame << " corner=0x" << std::hex
                                    << obs.sceneCorner << " bg=0x" << obs.bgPixel << std::dec;
        ASSERT_GE(obs.panelWidth, 160u) << "collapsed panel width on frame " << obs.frame;
        ASSERT_GE(obs.panelHeight, 55u) << "collapsed panel height on frame " << obs.frame;
        ASSERT_EQ(obs.bgDoubleBlended, 0u)
            << "double-blended panel alpha on frame " << obs.frame << " bg=0x" << std::hex << obs.bgPixel
            << " corner=0x" << obs.sceneCorner << std::dec;
        if (expectHud)
            ASSERT_GE(obs.refPixel & 255u, 125u) << "missing game HUD on frame " << obs.frame;
        else
            ASSERT_LT(obs.refPixel & 255u, 55u) << "stale game HUD on frame " << obs.frame;
        textHashes.insert(obs.textHash);
    }
    if (checkTextChanges)
        EXPECT_GE(textHashes.size(), 2u) << phaseName << " froze the overlay text across update intervals";
    if (!checkGraphMotion)
        return;
    auto steps = GraphSteps(frames);
    size_t sharedBaselineOutputs = 0;
    for (size_t i = 0; i < steps.size() && i < handoverOutputs; ++i) {
        if (frames[i].frame == frames[i + 1].frame && steps[i].measured && steps[i].pixels < 0.25) {
            steps[i].measured = false;
            ++sharedBaselineOutputs;
        }
    }
    EXPECT_LE(sharedBaselineOutputs, handoverOutputs / 2) << phaseName;
    EXPECT_GT(frames.back().graphColumns, 100u) << phaseName << ": no frame-time graph";
    const auto verdict = graph_motion::JudgeSteps(steps, {}, 0.35);
    // A step is measurable once a frame-time spike is in view (a flat line matches itself at every shift); from then
    // on nearly every step must be.
    const auto firstMeasured = std::find_if(steps.begin(), steps.end(), [](const auto& step) { return step.measured; });
    const size_t inView = static_cast<size_t>(steps.end() - firstMeasured);
    EXPECT_GE(verdict.measured, (std::max)(size_t{30}, inView * 9 / 10))
        << phaseName << ": graph motion measurable on too few of " << inView << " outputs with a spike in view";
    // A rescale redraws every column; one per text update at most, never a steady state.
    EXPECT_LE(verdict.rescaled, steps.size() / 20 + 2) << phaseName << ": graph redrawn instead of scrolled";
    EXPECT_EQ(verdict.backwards + verdict.holds + verdict.jumps, 0u)
        << phaseName << ": uneven graph motion (" << verdict.holds << " holds, " << verdict.jumps << " jumps, "
        << verdict.backwards << " backwards of " << verdict.measured << " measured, median "
        << verdict.medianPixels << " px); first " << verdict.firstViolation << "; steps " << [&] {
               std::string list;
               for (size_t index : verdict.violations)
                   list += std::to_string(index) + "(f" + std::to_string(frames[index + 1].frame) + ") ";
               return list;
           }();
}

TEST(FlowFSRMenuPixels, NestedPresenterKeepsOverlayAndHudAcrossSuspension) {
    SetEnvironmentVariableA("CE_FLOW_FFX_COMPOSE_UI", "1");
    SetEnvironmentVariableA("CE_FLOW_MAGENTA_GRAPH", "1");
    FlowGame game(CurrentTestName());
    GameOptions options;
    UsePixelOracleGeometry(options);
    options.streamline = true;
    options.swapchain = SwapchainKind::kNative;
    options.fsrNestedPresenter = true;
    options.fsrUi = FSRUiResource::kFullSizeWithSuspendedPlaceholder;
    options.fsrUiBuffering = FSRUiBuffering::kSwapchainCopy;
    options.fsrTestHudWindow = true;
    options.frameIntervalSpikePeriod = 70;
    options.frameIntervalSpikeDuration = 3;
    options.frameIntervalSpikeMicroseconds = 22000;
    options.presentChainOverlays = {{OverlayStandIn::kReShade, 0}};
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(options)) << game.Error();

    ASSERT_TRUE(game.RenderFrames(30)) << game.Error();
    ASSERT_TRUE(game.UseSwapchain(SwapchainKind::kStreamline)) << game.Error();
    ASSERT_TRUE(game.SetDLSSFrameGeneration(true)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(90)) << game.Error();
    ExpectPublished(game, 1, 2, "DLSS FG on");
    ASSERT_TRUE(game.SetDLSSFrameGeneration(false)) << game.Error();
    ASSERT_TRUE(game.UnloadDLSSG()) << game.Error();
    ASSERT_TRUE(game.UseSwapchain(SwapchainKind::kFidelityFX)) << game.Error();

    const HMODULE plugin = GetModuleHandleW(L"sl.dlss_g.dll");
    ASSERT_NE(plugin, nullptr);
    const auto lookup = reinterpret_cast<void* (*)(const char*)>(GetProcAddress(plugin, "slGetPluginFunction"));
    ASSERT_NE(lookup, nullptr);
    const auto enable = reinterpret_cast<void (*)(bool)>(lookup("flowSampleNestedOutput"));
    const auto setPoint = reinterpret_cast<void (*)(UINT, UINT)>(lookup("flowSetNestedHistoryPoint"));
    const auto setRegion = reinterpret_cast<void (*)(UINT, UINT, UINT, UINT)>(lookup("flowSetNestedHistoryRegion"));
    const auto readObs = reinterpret_cast<uint64_t (*)(uint64_t, OutputPanelObservation*)>(
        lookup("flowReadNestedObservation"));
    ASSERT_NE(enable, nullptr);
    ASSERT_NE(setPoint, nullptr);
    ASSERT_NE(setRegion, nullptr);
    ASSERT_NE(readObs, nullptr);
    setPoint(kHudPointX, kHudPointY);
    setRegion(kPanelRegion[0], kPanelRegion[1], kPanelRegion[2], kPanelRegion[3]);
    enable(true);

    PixelOracleReader oracle{readObs};
    uint64_t cursor = 0;

    // 1. Initial protected FSR startup (configured disabled).
    ASSERT_TRUE(game.RenderFrames(12)) << game.Error();
    ASSERT_TRUE(game.WaitForFSRPresents()) << game.Error();
    OutputPanelObservation dummy{};
    uint64_t end = readObs(0, &dummy);
    VerifyPhasePixels("fsr-startup-disabled", oracle.Collect(cursor, end), false, false, false);
    cursor = end;

    // 2. Active FSR FG.
    ASSERT_TRUE(game.SetFSRFrameGeneration(true, false)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(30)) << game.Error();
    ASSERT_TRUE(game.WaitForFSRPresents()) << game.Error();
    ExpectPublished(game, 2, 2, "FSR FG active before menu");
    end = readObs(0, &dummy);
    VerifyPhasePixels("fsr-active-1", oracle.Collect(cursor, end), true, false, true, 12);
    cursor = end;

    // 3. Retained proxy menu suspension (>= 160 frames crosses multiple 500ms text intervals & 3-buffer wraps;
    //    mix Present and Present1).
    ASSERT_TRUE(game.SetFSRFrameGeneration(false, false)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(85, FlowGame::PresentMethod::kPresent1)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(80, FlowGame::PresentMethod::kPresent)) << game.Error();
    ASSERT_TRUE(game.WaitForFSRPresents()) << game.Error();
    ExpectPublished(game, 0, 1, "FSR FG suspended in menu");
    end = readObs(0, &dummy);
    VerifyPhasePixels("fsr-menu-suspend-1", oracle.Collect(cursor, end), false, true, true);
    cursor = end;

    // 4. Resume FSR FG on the same proxy.
    ASSERT_TRUE(game.SetFSRFrameGeneration(true, false)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(85)) << game.Error();
    ASSERT_TRUE(game.WaitForFSRPresents()) << game.Error();
    ExpectPublished(game, 2, 2, "FSR FG resumed");
    end = readObs(0, &dummy);
    VerifyPhasePixels("fsr-resume-1", oracle.Collect(cursor, end), true, true, true, 12);
    cursor = end;

    // 5. Second suspend -> resume cycle + draining after disable.
    ASSERT_TRUE(game.SetFSRFrameGeneration(false, false)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(85, FlowGame::PresentMethod::kPresent1)) << game.Error();
    ASSERT_TRUE(game.WaitForFSRPresents()) << game.Error();
    end = readObs(0, &dummy);
    VerifyPhasePixels("fsr-menu-suspend-2", oracle.Collect(cursor, end), false, false, true);
    cursor = end;

    ASSERT_TRUE(game.SetFSRFrameGeneration(true, false)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(50)) << game.Error();
    ASSERT_TRUE(game.SetFSRFrameGeneration(false, false)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(30)) << game.Error();
    ASSERT_TRUE(game.WaitForFSRPresents()) << game.Error();
    end = readObs(0, &dummy);
    ASSERT_GT(end, cursor);
    VerifyPhasePixels("fsr-drain-off", oracle.Collect(end - 20, end), false, false, false);

    enable(false);
    ASSERT_TRUE(game.LoadDLSSG()) << game.Error();
    ASSERT_TRUE(game.UseSwapchain(SwapchainKind::kStreamline)) << game.Error();
    ASSERT_TRUE(game.SetDLSSFrameGeneration(true)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(90)) << game.Error();
    ExpectPublished(game, 1, 2, "DLSS FG resumed");
    ASSERT_TRUE(game.SetDLSSFrameGeneration(false)) << game.Error();
    ASSERT_TRUE(game.UseSwapchain(SwapchainKind::kNative)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(30)) << game.Error();
    ExpectPublished(game, 0, 1, "all FG off");
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

TEST(FlowFSRMenuPixels, DirectAndCallbackFSRKeepFreshOverlayAcrossSuspension) {
    SetEnvironmentVariableA("CE_FLOW_FFX_COMPOSE_UI", "1");
    SetEnvironmentVariableA("CE_FLOW_MAGENTA_GRAPH", "1");
    FlowGame game(CurrentTestName());
    GameOptions options;
    UsePixelOracleGeometry(options);
    options.swapchain = SwapchainKind::kFidelityFX;
    options.fsrNestedPresenter = false;
    options.probeRuntimeOutput = true;
    options.fsrUi = FSRUiResource::kFullSizeWithSuspendedPlaceholder;
    options.fsrUiBuffering = FSRUiBuffering::kSwapchainCopy;
    options.fsrTestHudWindow = true;
    options.frameIntervalSpikePeriod = 70;
    options.frameIntervalSpikeDuration = 3;
    options.frameIntervalSpikeMicroseconds = 22000;
    options.presentChainOverlays = {{OverlayStandIn::kReShade, 0}};
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(options)) << game.Error();

    auto& probe = RuntimeOutputProbe();
    const auto readObs = [&probe](uint64_t ordinal, OutputPanelObservation* out) {
        return probe.ReadObservation(ordinal, out);
    };
    probe.SetHistoryPoint(kHudPointX, kHudPointY);
    probe.SetHistoryRegion(kPanelRegion[0], kPanelRegion[1], kPanelRegion[2], kPanelRegion[3]);
    probe.Enable(true);

    PixelOracleReader oracle{readObs};
    OutputPanelObservation dummy{};
    uint64_t cursor = 0;

    ASSERT_TRUE(game.RenderFrames(12)) << game.Error();
    ASSERT_TRUE(game.WaitForFSRPresents()) << game.Error();
    uint64_t end = readObs(0, &dummy);
    VerifyPhasePixels("direct-fsr-startup", oracle.Collect(cursor, end), false, false, false);
    cursor = end;

    ASSERT_TRUE(game.SetFSRFrameGeneration(true, false)) << game.Error();
    // Past the scenario's first frame-time spike (frame 70): a flat graph shows no motion.
    ASSERT_TRUE(game.RenderFrames(100)) << game.Error();
    ASSERT_TRUE(game.WaitForFSRPresents()) << game.Error();
    ExpectPublished(game, 2, 2, "Direct FSR FG active");
    end = readObs(0, &dummy);
    VerifyPhasePixels("direct-fsr-active", oracle.Collect(cursor, end), true, false, true, 12);
    cursor = end;

    ASSERT_TRUE(game.SetFSRFrameGeneration(false, false)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(160, FlowGame::PresentMethod::kPresent1)) << game.Error();
    ASSERT_TRUE(game.WaitForFSRPresents()) << game.Error();
    ExpectPublished(game, 0, 1, "Direct FSR FG suspended");
    end = readObs(0, &dummy);
    VerifyPhasePixels("direct-fsr-suspend", oracle.Collect(cursor, end), false, true, true);
    cursor = end;

    ASSERT_TRUE(game.SetFSRFrameGeneration(true, true)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(40)) << game.Error();
    ASSERT_TRUE(game.WaitForFSRPresents()) << game.Error();
    ASSERT_GT(game.FSRAppPresentCallbackCalls(), 0u);
    ExpectPublished(game, 2, 2, "Callback FSR FG active");
    end = readObs(0, &dummy);
    // PresentCallback copies backbuffer without fake UI composition, so only check panel & single-blend.
    for (uint64_t i = cursor; i < end; ++i) {
        OutputPanelObservation obs{};
        readObs(i, &obs);
        ASSERT_EQ(obs.hasPanel, 1u) << "missing overlay panel on callback FSR frame " << obs.frame << " corner=0x"
                                    << std::hex << obs.sceneCorner << " bg=0x" << obs.bgPixel << std::dec
                                    << " changed=" << obs.regionChanged;
        ASSERT_EQ(obs.bgDoubleBlended, 0u) << "double-blend on callback FSR frame " << obs.frame;
    }

    probe.Enable(false);
    ASSERT_TRUE(game.SetFSRFrameGeneration(false, false)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(25)) << game.Error();
    ASSERT_TRUE(game.WaitForFSRPresents()) << game.Error();
    ExpectPublished(game, 0, 1, "Direct FSR off");
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}
}  // namespace
