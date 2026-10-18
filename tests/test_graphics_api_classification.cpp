#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "hook/present/dxgi_shared_detail/types_and_state.h"
#include "source_fragment_reader.h"

// Which API the overlay names and which overlay/capture path a swapchain takes: DXVK D3D11
// swapchains were classified D3D10 (found 2026-10-18).
namespace {
using DXGIShared::APIType;
using DXGIShared::SelectPrimarySwapChainAPIType;

constexpr unsigned kFL10_0 = 0xa000;
constexpr unsigned kFL10_1 = 0xa100;
constexpr unsigned kFL11_0 = 0xb000;
constexpr unsigned kFL11_1 = 0xb100;
constexpr unsigned kFL12_1 = 0xc100;

// DXVK's D3D11DXGIDevice::QueryInterface answers ID3D10Device/ID3D10Device1 on every device, and its
// swapchain's GetDevice forwards there: a DXVK D3D11 swapchain answers both, like D3D10 on D3D11.
TEST(GraphicsApiClassificationTest, DxvkD3D11SwapchainsStayD3D11) {
    for (unsigned level : {kFL11_0, kFL11_1, kFL12_1}) {
        EXPECT_EQ(SelectPrimarySwapChainAPIType(false, true, true, false, level), APIType::D3D11) << level;
        // A D3D10 runtime some other module loaded does not make an 11-level device D3D10.
        EXPECT_EQ(SelectPrimarySwapChainAPIType(false, true, true, true, level), APIType::D3D11) << level;
    }
    // A D3D10-level D3D11 device without any D3D10 runtime is still D3D11.
    EXPECT_EQ(SelectPrimarySwapChainAPIType(false, true, true, false, kFL10_0), APIType::D3D11);
}

TEST(GraphicsApiClassificationTest, NativeAndDxvkD3D10SwapchainsStayD3D10) {
    // Windows' D3D10 on D3D11 and DXVK's d3d10core: a D3D10 runtime and a 10-level device.
    EXPECT_EQ(SelectPrimarySwapChainAPIType(false, true, true, true, kFL10_0), APIType::D3D10);
    EXPECT_EQ(SelectPrimarySwapChainAPIType(false, true, true, true, kFL10_1), APIType::D3D10);
    EXPECT_EQ(SelectPrimarySwapChainAPIType(false, false, true, false, 0), APIType::D3D10);
    EXPECT_EQ(SelectPrimarySwapChainAPIType(true, true, true, true, kFL10_1), APIType::D3D12);
}

TEST(GraphicsApiClassificationSourceTest, EveryLabelAndClassifierTakesTheSharedDecision) {
    namespace fs = std::filesystem;
    const fs::path root = fs::current_path() / "hook";
    const auto read = [&](const fs::path& relative) { return ce::test_source::ReadFile(root / relative); };
    // Both swapchain classifiers.
    EXPECT_NE(read("present/dxgi_shared.cpp").find("ClassifySwapChainAPI(pSwapChain)"), std::string::npos);
    EXPECT_NE(read("d3d11/dx11_hook_helpers.cpp").find("DXGIShared::ClassifySwapChainAPI(swapChain)"),
              std::string::npos);
}

}  // namespace
