#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

std::string ReadGraphicsApiSource(const std::filesystem::path& relativePath) {
    const auto path = std::filesystem::current_path() / relativePath;
    std::ifstream stream(path, std::ios::binary);
    if (!stream.good())
        return {};
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

}  // namespace

TEST(GraphicsApiReportingSourceTest, OverlayIgnoresUnchangedLabelsBeforeInvalidatingLayout) {
    const std::string source = ReadGraphicsApiSource("hook/common/overlay_adapter.cpp");
    ASSERT_FALSE(source.empty());
    const size_t unchanged = source.find("LabelsDiffer(graphicsAPI, api)");
    const size_t layoutDirty = source.find("layoutDirty = true", unchanged);
    ASSERT_NE(unchanged, std::string::npos);
    ASSERT_NE(layoutDirty, std::string::npos);
    EXPECT_LT(unchanged, layoutDirty);
    EXPECT_NE(source.find("[GraphicsAPI] label transition", unchanged), std::string::npos);
}

TEST(GraphicsApiReportingSourceTest, DirectDrawHooksAllFactoryGenerationsAndExcludesBootstrapEvidence) {
    const std::string source = ReadGraphicsApiSource("hook/apis/ddraw_hook.cpp");
    ASSERT_FALSE(source.empty());
    EXPECT_NE(source.find("DetourDirectDrawCreate("), std::string::npos);
    EXPECT_NE(source.find("DetourDirectDrawCreateEx("), std::string::npos);
    EXPECT_NE(source.find("IID_IDirectDraw2"), std::string::npos);
    EXPECT_NE(source.find("IID_IDirectDraw3"), std::string::npos);
    EXPECT_NE(source.find("IID_IDirectDraw4"), std::string::npos);
    EXPECT_NE(source.find("IID_IDirectDraw7"), std::string::npos);
    EXPECT_NE(source.find("g_DDrawBootstrapDepth"), std::string::npos);
    EXPECT_NE(source.find("DetourD3D3CreateDevice"), std::string::npos);
    EXPECT_NE(source.find("DetourD3D7CreateDevice"), std::string::npos);
}

TEST(GraphicsApiReportingSourceTest, D3D9CreationEvidenceKeepsClassicAndExDevicesSeparate) {
    const std::string source = ReadGraphicsApiSource("hook/apis/dx9_hook.cpp");
    ASSERT_FALSE(source.empty());
    EXPECT_NE(source.find("RegisterD3D9DeviceIdentity(*ppReturnedDeviceInterface, false"), std::string::npos);
    EXPECT_NE(source.find("RegisterD3D9DeviceIdentity(*ppReturnedDeviceInterface, true"), std::string::npos);
    EXPECT_NE(source.find("IsDX9InternalHelperDevice(device)"), std::string::npos);
    EXPECT_NE(source.find("late-device-interface-probe"), std::string::npos);
}

TEST(GraphicsApiReportingSourceTest, D3D10AndD3D11UseCreationAndExternalInterfaceEvidence) {
    // The wrapper entry points are split between the DXGI factory exports and
    // the device-creation exports; the D3D10 evidence lives in the latter.
    const std::string wrappers = ReadGraphicsApiSource("hook/wrappers/wrapper_hooks.cpp") + "\n" +
                                 ReadGraphicsApiSource("hook/wrappers/wrapper_hooks_devices.cpp");
    const std::string dx11 = ReadGraphicsApiSource("hook/apis/dx11_hook.cpp");
    ASSERT_FALSE(wrappers.empty());
    ASSERT_FALSE(dx11.empty());
    EXPECT_NE(wrappers.find("Wrapped_D3D10CreateDeviceAndSwapChain1"), std::string::npos);
    EXPECT_NE(wrappers.find("DX10Hook_RegisterDeviceIdentity(pRealDevice, true"), std::string::npos);
    EXPECT_NE(dx11.find("external D3D11 device QueryInterface"), std::string::npos);
    EXPECT_NE(dx11.find("external D3D11 context QueryInterface"), std::string::npos);
    EXPECT_NE(dx11.find("g_D3D11InternalIdentityProbeDepth"), std::string::npos);
    EXPECT_NE(dx11.find("activeIdentityDevice"), std::string::npos);
}

TEST(GraphicsApiReportingSourceTest, OpenGLReevaluatesVersionAndProfileOnContextSwitch) {
    const std::string source = ReadGraphicsApiSource("hook/apis/opengl_hook.cpp");
    ASSERT_FALSE(source.empty());
    EXPECT_NE(source.find("g_CurrentTrackedContext = NULL"), std::string::npos);
    EXPECT_NE(source.find("g_VersionChecked = false"), std::string::npos);
    EXPECT_NE(source.find("GL_CONTEXT_PROFILE_MASK"), std::string::npos);
    EXPECT_NE(source.find("ResolveOpenGLIdentity"), std::string::npos);
    EXPECT_NE(source.find("FormatOpenGLLabel"), std::string::npos);
}
