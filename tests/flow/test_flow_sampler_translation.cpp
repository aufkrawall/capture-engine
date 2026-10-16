#include "tests/flow/flow_test_support.h"

#include "common/ipc/shared_defs.h"

namespace {

TEST(FlowSamplerTranslation, FrontendOwnsAFSafetyAndBiasWithoutChangingNativeSamplers) {
    ce::flow::FlowGame game(ce::flow::CurrentTestName());
    auto* memory = game.CaptureMemory();
    ASSERT_NE(memory, nullptr);
    strcpy_s(memory->graphicsConfig.anisotropicFiltering, "16x");
    strcpy_s(memory->graphicsConfig.mipBias, "-0.5");
    strcpy_s(memory->graphicsConfig.mipBiasMode, "offset");
    memory->configVersion.fetch_add(1, std::memory_order_release);
    using Apply = BOOL (*)(D3D12_SAMPLER_DESC*, bool);
    const auto apply = reinterpret_cast<Apply>(GetProcAddress(GetModuleHandleW(L"capture_hook_x64.dll"),
                                                            "CEFlow_ApplyDynamicSamplerPolicy"));
    ASSERT_NE(apply, nullptr);
    D3D12_SAMPLER_DESC original = {};
    original.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    original.AddressU = original.AddressV = original.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    original.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    original.MaxAnisotropy = 1;
    original.MaxLOD = D3D12_FLOAT32_MAX;
    original.MipLODBias = -0.75f;
    auto native = original;
    EXPECT_TRUE(apply(&native, false));
    EXPECT_EQ(native.Filter, D3D12_FILTER_ANISOTROPIC);
    EXPECT_FLOAT_EQ(native.MipLODBias, -1.25f);

    // The frontend has applied its bias but deliberately kept a resource's
    // sampler linear. Native translation must preserve both decisions.
    auto translated = original;
    translated.MipLODBias = -1.25f;
    EXPECT_FALSE(apply(&translated, true));
    EXPECT_EQ(translated.Filter, original.Filter);
    EXPECT_FLOAT_EQ(translated.MipLODBias, -1.25f);
    native = original;
    EXPECT_TRUE(apply(&native, false));
    EXPECT_EQ(native.Filter, D3D12_FILTER_ANISOTROPIC);
    EXPECT_FLOAT_EQ(native.MipLODBias, -1.25f);
}

}  // namespace
