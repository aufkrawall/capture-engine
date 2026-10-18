#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "hook/present/dxgi_shared_detail/streamline_present_routing.h"
#include "hook/present/dxgi_shared_detail/types_and_state.h"
#include "hook/runtime/graphics_api_identity.h"
#include "source_fragment_reader.h"

// Which API the overlay names and which overlay/capture path a swapchain takes. Three
// misclassifications found 2026-10-18: DXVK D3D11 games as D3D10, D3D11 games as "DX11.1"-"DX11.4"
// from interface queries the game never made, and vkd3d-proton as plain "DX12".
namespace {
using DXGIShared::APIType;
using DXGIShared::SelectPrimarySwapChainAPIType;
using ce::graphics_api_identity::ApiEvidenceCaller;
using ce::graphics_api_identity::ClassifyApiEvidenceCaller;

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

TEST(GraphicsApiClassificationTest, OnlyTheApplicationsOwnQueriesShowAD3D11MinorVersion) {
    EXPECT_EQ(ClassifyApiEvidenceCaller(true, false, false, false, false), ApiEvidenceCaller::kApplication);
    // CE's own prerender fence path asks for ID3D11Device5 / ID3D11DeviceContext4.
    EXPECT_EQ(ClassifyApiEvidenceCaller(true, true, false, false, false), ApiEvidenceCaller::kCaptureEngine);
    // Windows' or DXVK's d3d11.dll asking its own device.
    EXPECT_EQ(ClassifyApiEvidenceCaller(true, false, true, false, false), ApiEvidenceCaller::kDeviceRuntime);
    EXPECT_EQ(ClassifyApiEvidenceCaller(true, false, false, true, false), ApiEvidenceCaller::kWindows);
    EXPECT_EQ(ClassifyApiEvidenceCaller(true, false, false, false, true), ApiEvidenceCaller::kThirdPartyOverlay);
    EXPECT_EQ(ClassifyApiEvidenceCaller(false, false, false, false, false), ApiEvidenceCaller::kUnresolved);
    // CE outranks the rest: its module is never the application.
    EXPECT_EQ(ClassifyApiEvidenceCaller(true, true, true, true, true), ApiEvidenceCaller::kCaptureEngine);
}

TEST(GraphicsApiClassificationTest, WindowsModulesAreThoseUnderTheWindowsDirectory) {
    using ce::graphics_api_identity::IsPathUnderDirectory;
    EXPECT_TRUE(IsPathUnderDirectory("C:\\Windows\\System32\\dxgi.dll", "C:\\Windows"));
    EXPECT_TRUE(IsPathUnderDirectory("c:\\windows\\SysWOW64\\d3d11.dll", "C:\\Windows"));
    EXPECT_FALSE(IsPathUnderDirectory("C:\\WindowsApps\\game.exe", "C:\\Windows"));
    EXPECT_FALSE(IsPathUnderDirectory("D:\\Games\\X\\d3d11.dll", "C:\\Windows"));
    EXPECT_FALSE(IsPathUnderDirectory("C:\\Windows\\System32\\dxgi.dll", ""));
}

TEST(GraphicsApiClassificationTest, D3D12LabelNamesTheTranslator) {
    using ce::graphics_api_identity::D3D12Label;
    EXPECT_STREQ(D3D12Label(false, false), "DX12");
    EXPECT_STREQ(D3D12Label(true, false), "DX12 (VKD3D-Proton)");
    EXPECT_STREQ(D3D12Label(false, true), "DX12 (VKD3D)");
    // The same name as the Vulkan layer's label for vkd3d-proton.
    EXPECT_STREQ(DXGIShared::SelectTranslatedGraphicsAPIName(false, false, true, false), D3D12Label(true, false));
}

TEST(GraphicsApiClassificationSourceTest, EveryLabelAndClassifierTakesTheSharedDecision) {
    namespace fs = std::filesystem;
    const fs::path root = fs::current_path() / "hook";
    const auto read = [&](const fs::path& relative) { return ce::test_source::ReadFile(root / relative); };
    // Both swapchain classifiers.
    EXPECT_NE(read("present/dxgi_shared.cpp").find("ClassifySwapChainAPI(pSwapChain)"), std::string::npos);
    EXPECT_NE(read("d3d11/dx11_hook_helpers.cpp").find("DXGIShared::ClassifySwapChainAPI(swapChain)"),
              std::string::npos);
    // No D3D12 overlay path names its API by itself.
    for (const char* file : {"d3d12/dx12_hook_ffx.cpp", "d3d12/dx12_hook_ffx_ui_composite.cpp",
                             "d3d12/dx12_streamline_ui_overlay.cpp", "d3d12/dx12_hook_overlay_render.cpp",
                             "d3d12/dx12_hook_postsl_render_route.cpp",
                             "d3d12/dx12_hook_process_session_draw_record.cpp"}) {
        SCOPED_TRACE(file);
        const std::string source = read(file);
        ASSERT_FALSE(source.empty());
        EXPECT_NE(source.find("Dx12OverlayApiLabel()"), std::string::npos);
        EXPECT_EQ(source.find("SetGraphicsAPI(\"DX12\")"), std::string::npos);
        EXPECT_EQ(source.find("const char* api = \"DX12\""), std::string::npos);
        EXPECT_EQ(source.find("s_isVKD3D"), std::string::npos);
    }
    // Every D3D11 minor-version report carries its caller.
    for (const char* file : {"d3d11/dx11_hook_helpers.cpp", "wrappers/d3d11_device_wrap.cpp",
                             "wrappers/d3d11_devicecontext_forward.cpp"}) {
        SCOPED_TRACE(file);
        const std::string source = read(file);
        EXPECT_NE(source.find("CE_API_EVIDENCE_CALLER()"), std::string::npos);
        EXPECT_EQ(source.find(" QI\");"), std::string::npos) << "a report without its caller";
    }
}

}  // namespace
