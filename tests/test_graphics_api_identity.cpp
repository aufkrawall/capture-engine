#include <gtest/gtest.h>

#include "../hook/common/graphics_api_identity.h"

namespace {
using ce::graphics_api_identity::D3D10Label;
using ce::graphics_api_identity::D3D11Label;
using ce::graphics_api_identity::D3D9Label;
using ce::graphics_api_identity::DirectDrawLabel;
using ce::graphics_api_identity::DirectDrawVersion;
using ce::graphics_api_identity::FormatOpenGLLabel;
using ce::graphics_api_identity::LabelsDiffer;
using ce::graphics_api_identity::LegacyDirectXLabel;
using ce::graphics_api_identity::MergeD3D11Minor;
using ce::graphics_api_identity::OpenGLProfile;
using ce::graphics_api_identity::ResolveOpenGLIdentity;
using ce::graphics_api_identity::ScopedIdentityRegistry;

TEST(GraphicsApiIdentityTest, FormatsEveryDirectDrawGeneration) {
    EXPECT_STREQ(DirectDrawLabel(DirectDrawVersion::DirectDraw), "DirectDraw");
    EXPECT_STREQ(DirectDrawLabel(DirectDrawVersion::DirectDraw2), "DirectDraw2");
    EXPECT_STREQ(DirectDrawLabel(DirectDrawVersion::DirectDraw3), "DirectDraw3");
    EXPECT_STREQ(DirectDrawLabel(DirectDrawVersion::DirectDraw4), "DirectDraw4");
    EXPECT_STREQ(DirectDrawLabel(DirectDrawVersion::DirectDraw7), "DirectDraw7");
}

TEST(GraphicsApiIdentityTest, Direct3DUseOutranksDirectDrawTransport) {
    EXPECT_STREQ(LegacyDirectXLabel(DirectDrawVersion::DirectDraw4, 6), "DX6");
    EXPECT_STREQ(LegacyDirectXLabel(DirectDrawVersion::DirectDraw7, 7), "DX7");
    EXPECT_STREQ(LegacyDirectXLabel(DirectDrawVersion::DirectDraw7, 0), "DirectDraw7");
}

TEST(GraphicsApiIdentityTest, FormatsClassicAndExD3D9WithTranslationSuffix) {
    EXPECT_STREQ(D3D9Label(false, false), "DX9");
    EXPECT_STREQ(D3D9Label(true, false), "DX9Ex");
    EXPECT_STREQ(D3D9Label(false, true), "DX9 (DXVK)");
    EXPECT_STREQ(D3D9Label(true, true), "DX9Ex (DXVK)");
}

TEST(GraphicsApiIdentityTest, FormatsD3D10AndD3D101WithTranslationSuffix) {
    EXPECT_STREQ(D3D10Label(false, false), "DX10");
    EXPECT_STREQ(D3D10Label(true, false), "DX10.1");
    EXPECT_STREQ(D3D10Label(false, true), "DX10 (DXVK)");
    EXPECT_STREQ(D3D10Label(true, true), "DX10.1 (DXVK)");
}

TEST(GraphicsApiIdentityTest, D3D11MinorUseIsMonotonicAndBounded) {
    EXPECT_EQ(MergeD3D11Minor(0, 1), 1u);
    EXPECT_EQ(MergeD3D11Minor(3, 1), 3u);
    EXPECT_EQ(MergeD3D11Minor(3, 9), 4u);
    EXPECT_STREQ(D3D11Label(0, false), "DX11");
    EXPECT_STREQ(D3D11Label(1, false), "DX11.1");
    EXPECT_STREQ(D3D11Label(2, false), "DX11.2");
    EXPECT_STREQ(D3D11Label(3, false), "DX11.3");
    EXPECT_STREQ(D3D11Label(4, false), "DX11.4");
    EXPECT_STREQ(D3D11Label(4, true), "DX11.4 (DXVK)");
}

TEST(GraphicsApiIdentityTest, ScopedIdentityRegistryIsolatesDevicesAndNoOpsUnchangedEvidence) {
    ScopedIdentityRegistry<unsigned> registry;
    int deviceA = 0;
    int deviceB = 0;
    EXPECT_TRUE(registry.Set(&deviceA, 0u));
    EXPECT_FALSE(registry.Set(&deviceA, 0u));
    EXPECT_TRUE(registry.Promote(&deviceA, 3u));
    EXPECT_FALSE(registry.Promote(&deviceA, 1u));
    EXPECT_TRUE(registry.Set(&deviceB, 1u));

    unsigned identity = 0;
    ASSERT_TRUE(registry.TryGet(&deviceA, &identity));
    EXPECT_EQ(identity, 3u);
    ASSERT_TRUE(registry.TryGet(&deviceB, &identity));
    EXPECT_EQ(identity, 1u);
    EXPECT_TRUE(registry.Set(&deviceA, 0u));
    ASSERT_TRUE(registry.TryGet(&deviceA, &identity));
    EXPECT_EQ(identity, 0u);
    registry.Erase(&deviceA);
    EXPECT_FALSE(registry.TryGet(&deviceA, &identity));
}

TEST(GraphicsApiIdentityTest, ParsesOpenGLVersionAndProfiles) {
    const auto legacy = ResolveOpenGLIdentity("2.1.2 NVIDIA 999", 0);
    EXPECT_TRUE(legacy.valid);
    EXPECT_EQ(legacy.major, 2);
    EXPECT_EQ(legacy.minor, 1);
    EXPECT_EQ(legacy.profile, OpenGLProfile::Unknown);
    EXPECT_EQ(FormatOpenGLLabel(legacy), "OpenGL 2.1");

    const auto core = ResolveOpenGLIdentity("4.6.0 NVIDIA", 0x00000001u);
    EXPECT_EQ(core.profile, OpenGLProfile::Core);
    EXPECT_EQ(FormatOpenGLLabel(core), "OpenGL 4.6 Core");

    const auto compatibility = ResolveOpenGLIdentity("3.3 Mesa", 0x00000002u);
    EXPECT_EQ(compatibility.profile, OpenGLProfile::Compatibility);
    EXPECT_EQ(FormatOpenGLLabel(compatibility), "OpenGL 3.3 Compat");

    const auto es = ResolveOpenGLIdentity("OpenGL ES 3.2 vendor", 0x00000001u);
    EXPECT_EQ(FormatOpenGLLabel(es), "OpenGL 3.2 Core");
}

TEST(GraphicsApiIdentityTest, InvalidOpenGLVersionFallsBackWithoutFalseProfile) {
    const auto identity = ResolveOpenGLIdentity("vendor string", 0x00000001u);
    EXPECT_FALSE(identity.valid);
    EXPECT_EQ(identity.profile, OpenGLProfile::Unknown);
    EXPECT_EQ(FormatOpenGLLabel(identity), "OpenGL");
}

TEST(GraphicsApiIdentityTest, OpenGLContextIdentitiesRemainIsolatedAcrossSwitches) {
    ScopedIdentityRegistry<std::string> contexts;
    int legacyContext = 0;
    int coreContext = 0;
    EXPECT_TRUE(contexts.Set(&legacyContext, FormatOpenGLLabel(ResolveOpenGLIdentity("2.1 vendor", 0))));
    EXPECT_TRUE(contexts.Set(&coreContext, FormatOpenGLLabel(ResolveOpenGLIdentity("4.6 vendor", 0x1))));

    std::string label;
    ASSERT_TRUE(contexts.TryGet(&legacyContext, &label));
    EXPECT_EQ(label, "OpenGL 2.1");
    ASSERT_TRUE(contexts.TryGet(&coreContext, &label));
    EXPECT_EQ(label, "OpenGL 4.6 Core");
}

TEST(GraphicsApiIdentityTest, DetectsOnlyRealLabelTransitions) {
    EXPECT_FALSE(LabelsDiffer("DX11.3", "DX11.3"));
    EXPECT_TRUE(LabelsDiffer("DX11", "DX11.1"));
    EXPECT_FALSE(LabelsDiffer(nullptr, ""));
}
}  // namespace
