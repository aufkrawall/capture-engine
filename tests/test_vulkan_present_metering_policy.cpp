#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>

#include "../hook/vulkan_layer/vulkan_present_metering_policy.h"
#include "source_fragment_reader.h"

// Regression coverage for `vsync_mode=fifo` behaving like mailbox in Portal RTX
// (RTX Remix), session installed/captureengine/logs/20260829_022419.
//
// CE forced VK_PRESENT_MODE_FIFO_KHR and the driver accepted it, but Remix's
// DLSS 4 multi-frame generation chains VK_NV_present_metering's
// VkSetPresentConfigNV onto every present with numFramesPerBatch=4. The driver
// then paces the batch across one *rendered* frame interval instead of waiting
// for vertical blanks, so the presented rate ran at 165-172/s on a 143 Hz
// display - the tell being that CE's DXGI interception saw the driver switch
// from SyncInterval=1 to SyncInterval=0 plus DXGI_PRESENT_ALLOW_TEARING exactly
// when the frame-generation swapchain went live.

namespace {

using ce::vulkan_present_metering_policy::ChainScan;
using ce::vulkan_present_metering_policy::Decide;
using ce::vulkan_present_metering_policy::Decision;
using ce::vulkan_present_metering_policy::Input;
using ce::vulkan_present_metering_policy::IsVblankPacedPresentMode;
using ce::vulkan_present_metering_policy::kMaxScannedChainNodes;
using ce::vulkan_present_metering_policy::kStructureTypeSetPresentConfigNV;
using ce::vulkan_present_metering_policy::RequestsVblankPacedPresentation;
using ce::vulkan_present_metering_policy::PresentModeOverrideInput;
using ce::vulkan_present_metering_policy::ScanPresentChain;
using ce::vulkan_present_metering_policy::ShouldSkipPresentModeOverride;
using ce::vulkan_present_metering_policy::SetPresentConfigNV;

Input MakeInput(VkPresentModeKHR presentMode, uint32_t framesPerBatch) {
    Input input = {};
    input.vblankPacedPresentationRequested = true;
    input.swapchainPresentModeKnown = true;
    input.swapchainPresentMode = presentMode;
    input.swapchainCount = 1;
    input.meteredFramesPerBatch = framesPerBatch;
    return input;
}

SetPresentConfigNV MakeMeteringNode(uint32_t framesPerBatch, const void* next = nullptr) {
    SetPresentConfigNV node = {};
    node.sType = kStructureTypeSetPresentConfigNV;
    node.pNext = next;
    node.numFramesPerBatch = framesPerBatch;
    node.presentConfigFeedback = 0;
    return node;
}

TEST(VulkanPresentMeteringPolicy, OnlyFifoAndAdaptiveAskForVblankPacing) {
    EXPECT_TRUE(RequestsVblankPacedPresentation("fifo"));
    EXPECT_TRUE(RequestsVblankPacedPresentation("adaptive"));
    EXPECT_FALSE(RequestsVblankPacedPresentation("mailbox"));
    EXPECT_FALSE(RequestsVblankPacedPresentation("off"));
    EXPECT_FALSE(RequestsVblankPacedPresentation("default"));
    EXPECT_FALSE(RequestsVblankPacedPresentation(""));
    // The config layer does not case-normalize vsync_mode, and every existing
    // consumer compares the exact lowercase spellings. Staying identical to
    // them is what keeps the layer's present-mode override and this policy from
    // disagreeing about the same setting.
    EXPECT_FALSE(RequestsVblankPacedPresentation("FIFO"));
}

TEST(VulkanPresentMeteringPolicy, OnlyFifoModesCarryARateContract) {
    EXPECT_TRUE(IsVblankPacedPresentMode(VK_PRESENT_MODE_FIFO_KHR));
    EXPECT_TRUE(IsVblankPacedPresentMode(VK_PRESENT_MODE_FIFO_RELAXED_KHR));
    EXPECT_FALSE(IsVblankPacedPresentMode(VK_PRESENT_MODE_MAILBOX_KHR));
    EXPECT_FALSE(IsVblankPacedPresentMode(VK_PRESENT_MODE_IMMEDIATE_KHR));
}

TEST(VulkanPresentMeteringPolicy, PreservesMeteringOnAConfiguredFifoSwapchain) {
    // The Portal RTX case: 4x MFG keeps its content-spacing signal while
    // native relative timing supplies the separate display-rate ceiling.
    const Decision decision = Decide(MakeInput(VK_PRESENT_MODE_FIFO_KHR, 4));
    EXPECT_TRUE(decision.preserveMetering);
}

TEST(VulkanPresentMeteringPolicy, PreservesMeteringUnderAdaptiveFifo) {
    const Decision decision = Decide(MakeInput(VK_PRESENT_MODE_FIFO_RELAXED_KHR, 3));
    EXPECT_TRUE(decision.preserveMetering);
}

TEST(VulkanPresentMeteringPolicy, LeavesMeteringAloneWithoutAFifoSwapchain) {
    EXPECT_FALSE(Decide(MakeInput(VK_PRESENT_MODE_IMMEDIATE_KHR, 4)).preserveMetering);
    EXPECT_FALSE(Decide(MakeInput(VK_PRESENT_MODE_MAILBOX_KHR, 4)).preserveMetering);
}

TEST(VulkanPresentMeteringPolicy, LeavesMeteringAloneWhenTheProfileDidNotAskForFifo) {
    Input input = MakeInput(VK_PRESENT_MODE_FIFO_KHR, 4);
    input.vblankPacedPresentationRequested = false;
    EXPECT_FALSE(Decide(input).preserveMetering);
}

TEST(VulkanPresentMeteringPolicy, LeavesMeteringAloneOnAnUntrackedSwapchain) {
    // Without the swapchain the layer created there is no proof of its pacing
    // contract, and assuming one would be a guess about someone else's chain.
    Input input = MakeInput(VK_PRESENT_MODE_FIFO_KHR, 4);
    input.swapchainPresentModeKnown = false;
    EXPECT_FALSE(Decide(input).preserveMetering);
}

TEST(VulkanPresentMeteringPolicy, SingleFrameBatchIsNotAConflict) {
    // numFramesPerBatch of 0 or 1 carries no generated group to diagnose.
    EXPECT_FALSE(Decide(MakeInput(VK_PRESENT_MODE_FIFO_KHR, 0)).preserveMetering);
    EXPECT_FALSE(Decide(MakeInput(VK_PRESENT_MODE_FIFO_KHR, 1)).preserveMetering);
}

TEST(VulkanPresentMeteringPolicy, MultiSwapchainPresentIsLeftAlone) {
    Input input = MakeInput(VK_PRESENT_MODE_FIFO_KHR, 4);
    input.swapchainCount = 2;
    EXPECT_FALSE(Decide(input).preserveMetering);
}

TEST(VulkanPresentMeteringPolicyChainScan, EmptyChainFindsNothing) {
    const ChainScan scan = ScanPresentChain(nullptr);
    EXPECT_FALSE(scan.found);
    EXPECT_EQ(scan.nodeCount, 0u);
    EXPECT_EQ(scan.chainWithoutMetering, nullptr);
    EXPECT_FALSE(scan.truncated);
}

TEST(VulkanPresentMeteringPolicyChainScan, FindsAHeadMeteringNodeWithoutModifyingIt) {
    VkPresentIdKHR presentId = {};
    presentId.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR;
    presentId.pNext = nullptr;
    const SetPresentConfigNV metering = MakeMeteringNode(4, &presentId);

    const ChainScan scan = ScanPresentChain(&metering);
    EXPECT_TRUE(scan.found);
    EXPECT_TRUE(scan.isChainHead);
    EXPECT_EQ(scan.framesPerBatch, 4u);
    EXPECT_EQ(scan.nodeCount, 2u);
    EXPECT_EQ(scan.chainWithoutMetering, static_cast<const void*>(&presentId));
}

TEST(VulkanPresentMeteringPolicyChainScan, ChainWithoutMeteringIsUnchanged) {
    VkPresentIdKHR presentId = {};
    presentId.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR;

    const ChainScan scan = ScanPresentChain(&presentId);
    EXPECT_FALSE(scan.found);
    EXPECT_EQ(scan.nodeCount, 1u);
    EXPECT_EQ(scan.chainWithoutMetering, static_cast<const void*>(&presentId));
}

TEST(VulkanPresentMeteringPolicyChainScan, ADeeperMeteringNodeIsFoundButNotUnlinked) {
    const SetPresentConfigNV metering = MakeMeteringNode(3);
    VkPresentIdKHR presentId = {};
    presentId.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR;
    presentId.pNext = &metering;

    const ChainScan scan = ScanPresentChain(&presentId);
    EXPECT_TRUE(scan.found);
    EXPECT_FALSE(scan.isChainHead);
    EXPECT_EQ(scan.framesPerBatch, 3u);
    // The head is preserved: unlinking a deeper node would mean writing to the
    // application's own const chain.
    EXPECT_EQ(scan.chainWithoutMetering, static_cast<const void*>(&presentId));
}

TEST(VulkanPresentMeteringPolicyChainScan, ASelfReferentialChainTerminates) {
    // A malformed or hostile chain must not spin the present hook.
    VkPresentIdKHR presentId = {};
    presentId.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR;
    presentId.pNext = &presentId;

    const ChainScan scan = ScanPresentChain(&presentId);
    EXPECT_TRUE(scan.truncated);
    EXPECT_EQ(scan.nodeCount, kMaxScannedChainNodes);
    EXPECT_FALSE(scan.found);
}

// Portal RTX session 20260913_184745 crossed a live `vsync_mode` change inside
// one running game: with `default` the metered 3x batch reached the screen at a
// 0.43 ms frame-time stddev and a ~110 fps 1% low, and with `fifo` - the only
// difference being CE's present-mode override plus its VK_EXT_present_timing
// request - the identical presents landed bunched, 6.91 ms stddev and a ~14 fps
// 1% low. `20260913_190555` reproduces it. CE must therefore never ask for a
// present schedule of its own again: not the swapchain flag, not a target time,
// and not the device/instance capabilities that only existed to reach them.
TEST(VulkanPresentTimingRetirement, TheLayerRequestsNoPresentScheduleOfItsOwn) {
    namespace fs = std::filesystem;
    const fs::path layer = fs::current_path() / "hook" / "vulkan_layer";
    for (const char* unit : {"vulkan_layer_present.cpp", "vulkan_layer_swapchain.cpp", "vulkan_layer_hooks.cpp",
                             "vulkan_layer_capabilities.cpp"}) {
        const std::string source = ce::test_source::ReadLogicalSource(layer / unit);
        ASSERT_FALSE(source.empty()) << unit;
        for (const char* forbidden : {"VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT",
                                      "VkPresentTimingsInfoEXT",
                                      "VkPresentTimingInfoEXT",
                                      "VK_PRESENT_TIMING_INFO_PRESENT_AT_RELATIVE_TIME_BIT_EXT",
                                      "VK_PRESENT_TIMING_INFO_PRESENT_AT_NEAREST_REFRESH_CYCLE_BIT_EXT",
                                      "vkGetSwapchainTimingPropertiesEXT",
                                      "VK_EXT_PRESENT_TIMING_EXTENSION_NAME"}) {
            EXPECT_EQ(source.find(forbidden), std::string::npos) << unit << " must not use " << forbidden;
        }
    }
    EXPECT_FALSE(fs::exists(layer / "vulkan_present_timing.cpp"));
    EXPECT_FALSE(fs::exists(layer / "vulkan_present_timing_policy.h"));
}

// The other half of the same rule, and the older half: the generated-frame
// spacing signal is the runtime's, so CE observes it and changes nothing.
TEST(VulkanPresentTimingRetirement, GeneratedFrameSpacingStaysWithTheRuntime) {
    namespace fs = std::filesystem;
    const std::string present =
        ce::test_source::ReadLogicalSource(fs::current_path() / "hook" / "vulkan_layer" / "vulkan_layer_present.cpp");
    const std::string capabilities = ce::test_source::ReadLogicalSource(
        fs::current_path() / "hook" / "vulkan_layer" / "vulkan_layer_capabilities.cpp");
    const std::string remix =
        ce::test_source::ReadLogicalSource(fs::current_path() / "hook" / "apis" / "remix_hook.cpp");
    ASSERT_FALSE(present.empty());
    ASSERT_FALSE(capabilities.empty());
    ASSERT_FALSE(remix.empty());

    EXPECT_NE(present.find("preserved for generated-frame spacing"), std::string::npos);
    EXPECT_EQ(present.find("suppressPresentMetering"), std::string::npos);
    EXPECT_EQ(capabilities.find("CopyWithoutPresentMetering"), std::string::npos)
        << "device extension enumeration must preserve NVIDIA's metering capability";
    EXPECT_EQ(remix.find("enablePresentMetering"), std::string::npos)
        << "CE must not force Remix away from hardware generated-frame spacing";
}

// The 2026-09-13 follow-up: removing CE's own present schedule was not enough,
// because the present-mode override alone still collapses the batch. The
// driver states it itself in sensors.log's [DisplayTiming] line - the announced
// flip lead is 6842 us with vsync_mode=default and 141 us with fifo forced, and
// the published screen intervals go from ~7 ms to p50 2100 us / p99 18500 us.
TEST(VulkanPresentModeOverride, StandsDownForAMeteredFrameGenerator) {
    PresentModeOverrideInput input = {};
    input.vblankPacedPresentationRequested = true;
    input.deviceEnabledPresentMetering = true;
    EXPECT_TRUE(ShouldSkipPresentModeOverride(input));
}

TEST(VulkanPresentModeOverride, AnOrdinaryDeviceStillGetsForcedFifo) {
    PresentModeOverrideInput input = {};
    input.vblankPacedPresentationRequested = true;
    input.deviceEnabledPresentMetering = false;
    EXPECT_FALSE(ShouldSkipPresentModeOverride(input));
}

// `off` and `mailbox` are not a vertical-blank contract, carry none of the
// measurement above, and must keep working on a metering-capable device.
TEST(VulkanPresentModeOverride, OnlyVblankPacedRequestsStandDown) {
    PresentModeOverrideInput input = {};
    input.vblankPacedPresentationRequested = false;
    input.deviceEnabledPresentMetering = true;
    EXPECT_FALSE(ShouldSkipPresentModeOverride(input));
}

// Both override sites must consult it: the layer's own swapchain creation and
// the upstream sl.interposer hook that runs above the layer, which reaches the
// same answer through the resident layer's export.
TEST(VulkanPresentModeOverride, BothOverrideSitesConsultTheMeteringGate) {
    namespace fs = std::filesystem;
    const std::string swapchain = ce::test_source::ReadLogicalSource(
        fs::current_path() / "hook" / "vulkan_layer" / "vulkan_layer_swapchain.cpp");
    const std::string streamline = ce::test_source::ReadLogicalSource(
        fs::current_path() / "hook" / "apis" / "streamline_hook_install.cpp");
    const std::string bridge = ce::test_source::ReadLogicalSource(
        fs::current_path() / "hook" / "vulkan_layer" / "layer_wsi_surface_bridge.cpp");
    const std::string bridgeHeader = ce::test_source::ReadFile(
        fs::current_path() / "hook" / "vulkan_layer" / "layer_wsi_surface_bridge.h");
    ASSERT_FALSE(swapchain.empty());
    ASSERT_FALSE(streamline.empty());
    ASSERT_FALSE(bridge.empty());
    ASSERT_FALSE(bridgeHeader.empty());

    EXPECT_NE(swapchain.find("ShouldSkipPresentModeOverride"), std::string::npos);
    const std::string meteringBridge = ce::test_source::ReadFile(
        fs::current_path() / "hook" / "common" / "vulkan_layer_metering_bridge.h");
    ASSERT_FALSE(meteringBridge.empty());
    EXPECT_NE(streamline.find("MeteredGeneratorOwnsPresentPlacement"), std::string::npos);
    EXPECT_NE(meteringBridge.find("CEVulkanLayerDeviceEnabledPresentMetering"), std::string::npos);
    EXPECT_NE(bridge.find("CEVulkanLayerDeviceEnabledPresentMetering"), std::string::npos);
    // The layer has no .def file in any link command: __declspec(dllexport) is
    // the whole export mechanism, and a name without it resolves to null in the
    // hook DLL and fails closed in silence. tools/verify_vulkan_layer_exports.py
    // checks the shipped DLL; this only keeps the attribute on the declaration.
    EXPECT_NE(bridgeHeader.find("extern \"C\" __declspec(dllexport) BOOL "
                                "CEVulkanLayerDeviceEnabledPresentMetering(void);"),
              std::string::npos)
        << "the hook DLL resolves this export by name; it must stay dllexport";
}

// Portal RTX session 20260914_114142: the same running game walked 3x/4x/2x/3x/
// 4x on a 144 Hz panel with vsync_mode=fifo. At 3x the generator's own target
// (48 base x 3) landed on the panel exactly and the screen series was clean
// (fps=144.1 stddev=253us, nvFlipSchedule avgDelayUs=6127). At 4x the target
// became 41 x 4 = 164 and the driver stopped scheduling flips at all - the
// announced lead over a pure 4x window averaged under 10 us and the published
// screen intervals collapsed to p50=2300us/p99=18400us at 163/s. The ceiling
// CE took over when it stopped forcing FIFO is what re-fits that batch.
TEST(VulkanPresentMeteringPolicyTest, MeteredGeneratorTakesTheDisplayCeilingOnTheRenderedRate) {
    using ce::vulkan_present_metering_policy::DisplayCeilingInput;
    using ce::vulkan_present_metering_policy::ResolveVblankCeilingOutputFps;

    DisplayCeilingInput input = {};
    input.vblankPacedPresentationRequested = true;
    input.deviceEnabledPresentMetering = true;
    input.createdPresentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    input.displayRefreshFps = 144;
    EXPECT_EQ(ResolveVblankCeilingOutputFps(input), 144)
        << "standing down from the present-mode override is what makes this bound CE's to state";

    // The ceiling is an OUTPUT rate. Dividing by the live multiplier belongs to
    // the limiter, because the multiplier changes inside a running game.
    DisplayCeilingInput fifoRelaxed = input;
    fifoRelaxed.createdPresentMode = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    EXPECT_EQ(ResolveVblankCeilingOutputFps(fifoRelaxed), 0)
        << "a vertical-blank-paced swapchain already gets its wait from the WSI";
    DisplayCeilingInput fifo = input;
    fifo.createdPresentMode = VK_PRESENT_MODE_FIFO_KHR;
    EXPECT_EQ(ResolveVblankCeilingOutputFps(fifo), 0);

    DisplayCeilingInput notMetered = input;
    notMetered.deviceEnabledPresentMetering = false;
    EXPECT_EQ(ResolveVblankCeilingOutputFps(notMetered), 0)
        << "CE forced the present mode there, so the WSI still owns the bound";

    DisplayCeilingInput notRequested = input;
    notRequested.vblankPacedPresentationRequested = false;
    EXPECT_EQ(ResolveVblankCeilingOutputFps(notRequested), 0)
        << "off/mailbox/default never asked for a displayed-rate bound";
}

TEST(VulkanPresentMeteringPolicyTest, DisplayCeilingRejectsImplausibleRefreshReports) {
    using ce::vulkan_present_metering_policy::DisplayCeilingInput;
    using ce::vulkan_present_metering_policy::ResolveVblankCeilingOutputFps;
    using ce::vulkan_present_metering_policy::kMaxCeilingRefreshFps;
    using ce::vulkan_present_metering_policy::kMinCeilingRefreshFps;

    DisplayCeilingInput input = {};
    input.vblankPacedPresentationRequested = true;
    input.deviceEnabledPresentMetering = true;
    input.createdPresentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;

    // EnumDisplaySettings reports 0 and 1 for "default"/"hardware default"
    // adapter modes, and an unreadable monitor reports nothing at all. None of
    // those is a refresh rate, and a ceiling invented from one would cap a game
    // at a rate no display asked for.
    for (const int unusable : {0, 1, -1, kMinCeilingRefreshFps - 1, kMaxCeilingRefreshFps + 1}) {
        input.displayRefreshFps = unusable;
        EXPECT_EQ(ResolveVblankCeilingOutputFps(input), 0) << "refresh report " << unusable;
    }
    input.displayRefreshFps = kMinCeilingRefreshFps;
    EXPECT_EQ(ResolveVblankCeilingOutputFps(input), kMinCeilingRefreshFps);
    input.displayRefreshFps = kMaxCeilingRefreshFps;
    EXPECT_EQ(ResolveVblankCeilingOutputFps(input), kMaxCeilingRefreshFps);
}

}  // namespace
