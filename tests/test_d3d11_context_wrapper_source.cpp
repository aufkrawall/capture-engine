#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "source_fragment_reader.h"

namespace {

std::string ReadTextFile(const std::filesystem::path& path) {
    return ce::test_source::ReadLogicalSource(path);
}

// The device-context wrapper is implemented across one internal header and four
// translation units. Assertions below describe the wrapper as a whole, so they
// run over the concatenation in declaration-then-definition order.
std::string ReadContextWrapperSources() {
    const auto dir = std::filesystem::current_path() / "hook" / "wrappers";
    std::string combined;
    for (const char* part : {"d3d11_devicecontext_wrap_internal.h", "d3d11_context_forced_af.cpp",
                             "d3d11_devicecontext_wrap.cpp", "d3d11_devicecontext_forward.cpp",
                             "d3d11_devicecontext_get.cpp"}) {
        const std::filesystem::path path = dir / part;
        EXPECT_TRUE(std::filesystem::exists(path)) << part;
        combined += ReadTextFile(path);
        combined += '\n';
    }
    return combined;
}

}  // namespace

TEST(D3D11ContextWrapperSourceTest, PromotesInheritedContextInterfacesIndependently) {
    const std::string text = ReadContextWrapperSources();
    ASSERT_FALSE(text.empty());

    EXPECT_NE(text.find("m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal1))"), std::string::npos);
    EXPECT_NE(text.find("m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal2))"), std::string::npos);
    EXPECT_NE(text.find("m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal3))"), std::string::npos);
    EXPECT_NE(text.find("m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal4))"), std::string::npos);
    EXPECT_EQ(text.find("else if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal1))))"), std::string::npos);
    EXPECT_EQ(text.find("IID_ID3D11DeviceContext1 && m_Version >= 1"), std::string::npos);
    EXPECT_NE(text.find("IID_ID3D11DeviceContext1 && m_pReal1"), std::string::npos);
    EXPECT_NE(text.find("ClearView requested without real Context1"), std::string::npos);
}

TEST(D3D11ContextWrapperSourceTest, ForcedAFHotPathUsesObjectOwnedCachesAndDirtyState) {
    const auto root = std::filesystem::current_path();
    const std::string context = ReadContextWrapperSources();
    const std::string device = ReadTextFile(root / "hook" / "wrappers" / "d3d11_device_wrap.cpp");
    const std::string rawHook = ReadTextFile(root / "hook" / "d3d11" / "dx11_hook.cpp");

    ASSERT_FALSE(context.empty());
    ASSERT_FALSE(device.empty());
    ASSERT_FALSE(rawHook.empty());

    EXPECT_NE(context.find("kWrapperPixelShaderAFMetadataGuid"), std::string::npos);
    EXPECT_NE(context.find("kWrapperForcedAFViewCacheGuid"), std::string::npos);
    EXPECT_NE(context.find("SetPrivateDataInterface(variantGuid, replacement)"), std::string::npos);
    const size_t eligibilityCheck = context.find("WrapperSamplerAllowsForcedAF(desc, gfx)");
    const size_t variantLookup = context.find("original->GetPrivateData(variantGuid");
    ASSERT_NE(eligibilityCheck, std::string::npos);
    ASSERT_NE(variantLookup, std::string::npos);
    EXPECT_LT(eligibilityCheck, variantLookup);
    EXPECT_NE(context.find("if (dirtyMask == 0)"), std::string::npos);
    EXPECT_EQ(context.find("g_WrapperSamplerCacheMutex"), std::string::npos);
    EXPECT_EQ(context.find("MarkForcedAFResourceMutation"), std::string::npos);
    EXPECT_EQ(context.find("PendingStreamingQuiet"), std::string::npos);

    EXPECT_NE(device.find("RegisterWrapperForcedAFViewMetadata(*ppSRView)"), std::string::npos);
    EXPECT_NE(context.find("if (!RestoreContextState)"), std::string::npos);
    EXPECT_NE(context.find("if (SUCCEEDED(hr) && !RestoreDeferredContextState)"), std::string::npos);
    EXPECT_NE(context.find("ClearForcedAFTracking();"), std::string::npos);
    EXPECT_NE(context.find("ppSamplers[i] != logical"), std::string::npos);

    EXPECT_NE(rawHook.find("g_D3D11DirtyContextCount.load(std::memory_order_acquire) == 0"), std::string::npos);
    EXPECT_NE(rawHook.find("ClearTrackedContextState11(context);"), std::string::npos);
    const size_t rawStateErase = rawHook.find("g_D3D11ContextStates.erase(it);");
    const size_t rawStateRelease = rawHook.find("ReleaseTrackedContextState11(retiredState);");
    ASSERT_NE(rawStateErase, std::string::npos);
    ASSERT_NE(rawStateRelease, std::string::npos);
    EXPECT_LT(rawStateErase, rawStateRelease);
    EXPECT_EQ(rawHook.find("AF draw stats draws="), std::string::npos);
}

TEST(D3D11ContextWrapperSourceTest, RawContextUsesCompactShaderDependencies) {
    const auto root = std::filesystem::current_path();
    const std::string state = ReadTextFile(root / "hook" / "d3d11" / "dx11_hook_sampler_state.cpp");
    const std::string overrides = ReadTextFile(root / "hook" / "d3d11" / "dx11_hook_sampler_override.cpp");
    EXPECT_NE(state.find("GetWrapperPixelShaderAFPlan("), std::string::npos);
    EXPECT_NE(state.find("GetTrackedPixelShaderSamplerPlan11("), std::string::npos);
    EXPECT_EQ(overrides.find("GetTrackedPixelShaderMetadata11("), std::string::npos);
    EXPECT_NE(overrides.find("textures.TakeFirst()"), std::string::npos);
    EXPECT_EQ(overrides.find("textureSlot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT"), std::string::npos);
    EXPECT_NE(state.find("pixelShaderAFPlan.ResourceDirtyMask(slot, trackedMask)"), std::string::npos);
    EXPECT_NE(state.find("pixelShaderAFPlan.ShaderDirtyMask("), std::string::npos);
    EXPECT_NE(overrides.find("g_D3D11SamplerOverridesActive.load("), std::string::npos);
    EXPECT_NE(overrides.find("HasCurrentD3D11SamplerCreationPolicy(original, gfx)"), std::string::npos);
    EXPECT_NE(overrides.find("ApplyD3D11BindTimeAF(desc, gfx, allowAnisotropicOverride)"), std::string::npos);
}

TEST(D3D11ContextWrapperSourceTest, TranslatedSamplersKeepTheFrontendPolicyAtEveryCreationBoundary) {
    const auto root = std::filesystem::current_path();
    for (const char* path : {"hook/d3d11/dx11_hook_present.cpp", "hook/d3d11/dx11_hook_sampler_override.cpp",
                             "hook/wrappers/d3d11_device_wrap.cpp", "hook/wrappers/d3d11_context_forced_af.cpp"}) {
        const std::string source = ReadTextFile(root / path);
        EXPECT_NE(source.find("D3D11SamplerCreationScope translationScope;"), std::string::npos) << path;
    }
    const std::string native = ReadTextFile(root / "hook/d3d12/dx12_sampler_hooks.cpp");
    const size_t begin = native.find("Result ApplyDynamicSampler(");
    const size_t end = native.find("Result ApplyStaticSampler(", begin);
    ASSERT_NE(begin, std::string::npos);
    ASSERT_NE(end, std::string::npos);
    const std::string apply = native.substr(begin, end - begin);
    const size_t ownership = apply.find("D3D11OwnsTranslatedSampler()");
    ASSERT_NE(ownership, std::string::npos);
    EXPECT_LT(ownership, apply.find("dx12_sampler_policy::Apply(desc, gfx)"));
    EXPECT_NE(apply.find("return {};", ownership), std::string::npos);
}
