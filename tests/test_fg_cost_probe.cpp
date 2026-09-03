#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "../hook/common/fg_cost_probe.h"

#include "source_fragment_reader.h"

namespace {

using ce::fg_cost_probe::ParseMask;

std::string ReadSource(const std::filesystem::path& relativePath) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / relativePath);
}

TEST(FgCostProbeTest, ParsesDecimalAndHexadecimalMasks) {
    EXPECT_EQ(ParseMask("1"), 1u);
    EXPECT_EQ(ParseMask("15"), 15u);
    EXPECT_EQ(ParseMask("0x1"), 1u);
    EXPECT_EQ(ParseMask("0xF"), 15u);
    EXPECT_EQ(ParseMask("0Xf"), 15u);
    EXPECT_EQ(ParseMask(" 0x0d"), 13u);
}

// A typo must leave every production behaviour in place rather than silently
// suppressing part of the frame-generation path.
TEST(FgCostProbeTest, UnparsableTextSuppressesNothing) {
    EXPECT_EQ(ParseMask(nullptr), 0u);
    EXPECT_EQ(ParseMask(""), 0u);
    EXPECT_EQ(ParseMask("0x"), 0u);
    EXPECT_EQ(ParseMask("on"), 0u);
    EXPECT_EQ(ParseMask("-1"), 0u);
    EXPECT_EQ(ParseMask("1f"), 0u);       // hexadecimal digits without the 0x prefix
    EXPECT_EQ(ParseMask("2 "), 0u);       // trailing junk is not a partial parse
    EXPECT_EQ(ParseMask("0x100000000"), 0u);
}

TEST(FgCostProbeTest, BitsAreDistinctAndIndependentlySelectable) {
    using namespace ce::fg_cost_probe;
    const uint32_t bits[] = {kBridgeTailOff,        kBreadcrumbsOff,          kBridgeOverlayOff,
                             kBridgeMirrorOff,      kEclPassthrough,          kPresentPassthrough,
                             kFfxBridgeOff,         kEclCallerModuleLookupOff, kEclStreamlineUiHooksOff,
                             kEclStartupBlockOff,   kEclClassificationOff,    kEclDiagnosticsOff,
                             kEclQueueRegistrationOff, kQueueVTableHookOff,   kQueueDevicePublishOff,
                             kQueueAdoptionOff,     kSamplerDeviceHooksOff};
    uint32_t seen = 0;
    for (uint32_t bit : bits) {
        EXPECT_NE(bit, 0u);
        EXPECT_EQ(bit & (bit - 1), 0u) << "each probe bit must select exactly one behaviour";
        EXPECT_EQ(seen & bit, 0u) << "probe bits must not overlap";
        seen |= bit;
    }
    EXPECT_EQ(ParseMask("0x6"), kBreadcrumbsOff | kBridgeOverlayOff);
    EXPECT_EQ(ParseMask("0x70"), kEclPassthrough | kPresentPassthrough | kFfxBridgeOff);
}

// The probe is a measurement aid: every suppression site must read the mask
// rather than a hard-coded constant, or a normal run stops being normal.
TEST(FgCostProbeTest, EverySuppressionSiteIsGatedOnTheMask) {
    const std::string bridge = ReadSource("hook/apis/dx12_hook_ffx.cpp");
    EXPECT_NE(bridge.find("ce::fg_cost_probe::kBridgeTailOff"), std::string::npos);
    EXPECT_NE(bridge.find("ce::fg_cost_probe::kBridgeOverlayOff"), std::string::npos);
    EXPECT_NE(bridge.find("ce::fg_cost_probe::kBridgeMirrorOff"), std::string::npos);

    const std::string breadcrumbs = ReadSource("hook/apis/dx12_hook_overlay_breadcrumbs.cpp");
    EXPECT_NE(breadcrumbs.find("ce::fg_cost_probe::kBreadcrumbsOff"), std::string::npos);

    EXPECT_NE(ReadSource("hook/apis/dx12_hook_ecl.cpp").find("ce::fg_cost_probe::kEclPassthrough"),
              std::string::npos);
    // Both present entry points, or a game that uses Present1 measures nothing.
    EXPECT_NE(ReadSource("hook/common/dxgi_shared_present.cpp").find("ce::fg_cost_probe::kPresentPassthrough"),
              std::string::npos);
    EXPECT_NE(ReadSource("hook/common/dxgi_shared_present1.cpp").find("ce::fg_cost_probe::kPresentPassthrough"),
              std::string::npos);
    EXPECT_NE(ReadSource("hook/apis/ffx_hook_context.cpp").find("ce::fg_cost_probe::kFfxBridgeOff"),
              std::string::npos);
    const std::string route = ReadSource("hook/apis/dx12_hook_postsl_route.cpp");
    EXPECT_NE(route.find("ce::fg_cost_probe::kQueueAdoptionOff"), std::string::npos);
    EXPECT_NE(route.find("ce::fg_cost_probe::kQueueDevicePublishOff"), std::string::npos);
    EXPECT_NE(route.find("ce::fg_cost_probe::kQueueVTableHookOff"), std::string::npos);
    EXPECT_NE(ReadSource("hook/apis/dx12_sampler_hooks.cpp").find("ce::fg_cost_probe::kSamplerDeviceHooksOff"),
              std::string::npos);
    // Both the per-frame sequence bump and the per-op write must go, or the
    // "breadcrumbs off" run still pays for the buffer it no longer writes.
    EXPECT_NE(breadcrumbs.find("void BeginOverlayGpuBreadcrumbFrame"), std::string::npos);
    EXPECT_NE(breadcrumbs.find("void WriteOverlayGpuBreadcrumb"), std::string::npos);
}

}  // namespace
