#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

#include "../hook/common/dx12_sampler_policy.h"
#include "../hook/common/sampler_override_utils.h"
#include "source_fragment_reader.h"

namespace {

D3D12_SAMPLER_DESC MaterialSampler(D3D12_FILTER filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR) {
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
    D3D12_SAMPLER_DESC desc = {};
    desc.Filter = filter;
    desc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    desc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    desc.MaxAnisotropy = 1;
    desc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.MinLOD = 0.0f;
    desc.MaxLOD = D3D12_FLOAT32_MAX;
    return desc;
}

GraphicsConfig ForcedAf(const char* value = "16x") {
    GraphicsConfig gfx;
    gfx.anisotropicFiltering = value;
    gfx.mipBias = "default";
    return gfx;
}

std::string ReadTextFile(const std::filesystem::path& path) {
    return ce::test_source::ReadLogicalSource(path);
}

void SetFloatBits(float& value, uint32_t bits) {
    auto* bytes = reinterpret_cast<volatile uint8_t*>(&value);
    bytes[0] = static_cast<uint8_t>(bits);
    bytes[1] = static_cast<uint8_t>(bits >> 8);
    bytes[2] = static_cast<uint8_t>(bits >> 16);
    bytes[3] = static_cast<uint8_t>(bits >> 24);
}

TEST(DX12SamplerPolicyTest, PromotesOnlyMaterialStyleLinearSampler) {
    D3D12_SAMPLER_DESC desc = MaterialSampler();
    const auto result = ce::dx12_sampler_policy::Apply(desc, ForcedAf());

    EXPECT_TRUE(result.Modified());
    EXPECT_TRUE(result.anisotropyModified);
    EXPECT_EQ(result.decision, ce::dx12_sampler_policy::Decision::Allow);
    EXPECT_EQ(desc.Filter, D3D12_FILTER_ANISOTROPIC);
    EXPECT_EQ(desc.MaxAnisotropy, 16u);
}

TEST(DX12SamplerPolicyTest, NormalFilterIgnoresComparisonFunc) {
    D3D12_SAMPLER_DESC desc = MaterialSampler();
    desc.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;

    const auto result = ce::dx12_sampler_policy::Apply(desc, ForcedAf());

    EXPECT_TRUE(result.Modified());
    EXPECT_EQ(desc.Filter, D3D12_FILTER_ANISOTROPIC);
}

TEST(DX12SamplerPolicyTest, PreservesComparisonAndReductionFilters) {
    for (D3D12_FILTER filter : {D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR, D3D12_FILTER_COMPARISON_ANISOTROPIC,
                                D3D12_FILTER_MINIMUM_MIN_MAG_MIP_LINEAR, D3D12_FILTER_MAXIMUM_MIN_MAG_MIP_LINEAR}) {
        D3D12_SAMPLER_DESC desc = MaterialSampler(filter);
        const D3D12_SAMPLER_DESC original = desc;
        const auto result = ce::dx12_sampler_policy::Apply(desc, ForcedAf());

        EXPECT_FALSE(result.Modified()) << static_cast<unsigned>(filter);
        EXPECT_EQ(desc.Filter, original.Filter);
        EXPECT_EQ(desc.MaxAnisotropy, original.MaxAnisotropy);
        if (ce::sampler_override::IsD3D12ComparisonFilter(filter)) {
            EXPECT_EQ(result.decision, ce::dx12_sampler_policy::Decision::ComparisonFilter);
        } else {
            EXPECT_EQ(result.decision, ce::dx12_sampler_policy::Decision::ReductionFilter);
        }
    }
}

TEST(DX12SamplerPolicyTest, PreservesNonMaterialAddressModes) {
    for (D3D12_TEXTURE_ADDRESS_MODE mode : {D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_BORDER,
                                            D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE}) {
        D3D12_SAMPLER_DESC desc = MaterialSampler();
        desc.AddressU = mode;
        const auto result = ce::dx12_sampler_policy::Apply(desc, ForcedAf());

        EXPECT_FALSE(result.Modified()) << static_cast<unsigned>(mode);
        EXPECT_EQ(desc.Filter, D3D12_FILTER_MIN_MAG_MIP_LINEAR);
    }
}

TEST(DX12SamplerPolicyTest, PreservesFixedLodAndPointMinMagSamplers) {
    D3D12_SAMPLER_DESC fixed = MaterialSampler();
    fixed.MaxLOD = 0.0f;
    const auto fixedResult = ce::dx12_sampler_policy::Apply(fixed, ForcedAf());
    EXPECT_EQ(fixedResult.decision, ce::dx12_sampler_policy::Decision::FixedLod);
    EXPECT_FALSE(fixedResult.Modified());

    D3D12_SAMPLER_DESC point = MaterialSampler(D3D12_FILTER_MIN_MAG_MIP_POINT);
    const auto pointResult = ce::dx12_sampler_policy::Apply(point, ForcedAf());
    EXPECT_EQ(pointResult.decision, ce::dx12_sampler_policy::Decision::PointMinMag);
    EXPECT_FALSE(pointResult.Modified());
}

TEST(DX12SamplerPolicyTest, RejectsInvalidLodRangesWithoutChangingBytes) {
    for (const auto [minLod, maxLod] : {std::pair{1.0f, 1.0f}, std::pair{2.0f, 1.0f}}) {
        D3D12_SAMPLER_DESC desc = MaterialSampler();
        desc.MinLOD = minLod;
        desc.MaxLOD = maxLod;
        const D3D12_SAMPLER_DESC original = desc;

        const auto result = ce::dx12_sampler_policy::Apply(desc, ForcedAf());

        EXPECT_FALSE(result.Modified());
        // NOLINTNEXTLINE(bugprone-suspicious-memory-comparison) - byte-identical preservation is the contract under test
        EXPECT_EQ(0, std::memcmp(&desc, &original, sizeof(desc)));
    }

    for (const auto [bits, writeMinLod] : {std::pair{0x7FC00000u, true}, std::pair{0x7F800000u, false}}) {
        D3D12_SAMPLER_DESC desc = MaterialSampler();
        SetFloatBits(writeMinLod ? desc.MinLOD : desc.MaxLOD, bits);
        const D3D12_SAMPLER_DESC original = desc;

        const auto result = ce::dx12_sampler_policy::Apply(desc, ForcedAf());

        EXPECT_EQ(result.decision, ce::dx12_sampler_policy::Decision::InvalidDescriptor);
        EXPECT_FALSE(result.Modified());
        // NOLINTNEXTLINE(bugprone-suspicious-memory-comparison) - byte-identical preservation is the contract under test
        EXPECT_EQ(0, std::memcmp(&desc, &original, sizeof(desc)));
    }
}

TEST(DX12SamplerPolicyTest, DisablesOnlySafeOrdinaryAnisotropicSampler) {
    D3D12_SAMPLER_DESC ordinary = MaterialSampler(D3D12_FILTER_ANISOTROPIC);
    ordinary.MaxAnisotropy = 16;
    const auto ordinaryResult = ce::dx12_sampler_policy::Apply(ordinary, ForcedAf("off"));
    EXPECT_TRUE(ordinaryResult.Modified());
    EXPECT_EQ(ordinary.Filter, D3D12_FILTER_MIN_MAG_MIP_LINEAR);
    EXPECT_EQ(ordinary.MaxAnisotropy, 1u);

    D3D12_SAMPLER_DESC comparison = MaterialSampler(D3D12_FILTER_COMPARISON_ANISOTROPIC);
    comparison.MaxAnisotropy = 16;
    const auto comparisonResult = ce::dx12_sampler_policy::Apply(comparison, ForcedAf("off"));
    EXPECT_FALSE(comparisonResult.Modified());
    EXPECT_EQ(comparison.Filter, D3D12_FILTER_COMPARISON_ANISOTROPIC);
}

TEST(DX12SamplerPolicyTest, AppliesMipBiasOnlyToSafeMaterialSampler) {
    GraphicsConfig gfx = ForcedAf("default");
    gfx.mipBias = "-1.25";

    D3D12_SAMPLER_DESC material = MaterialSampler();
    const auto materialResult = ce::dx12_sampler_policy::Apply(material, gfx);
    EXPECT_TRUE(materialResult.mipBiasModified);
    EXPECT_FLOAT_EQ(material.MipLODBias, -1.25f);

    D3D12_SAMPLER_DESC shadow = MaterialSampler(D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR);
    const auto shadowResult = ce::dx12_sampler_policy::Apply(shadow, gfx);
    EXPECT_FALSE(shadowResult.Modified());
    EXPECT_FLOAT_EQ(shadow.MipLODBias, 0.0f);
}

TEST(DX12SamplerPolicyTest, AppliesMipMappingWithoutMisreportingAnisotropy) {
    GraphicsConfig gfx = ForcedAf("default");
    gfx.mipMapping = "bilinear";
    D3D12_SAMPLER_DESC desc = MaterialSampler();

    const auto result = ce::dx12_sampler_policy::Apply(desc, gfx);

    EXPECT_TRUE(result.Modified());
    EXPECT_TRUE(result.mipMappingModified);
    EXPECT_FALSE(result.anisotropyModified);
    EXPECT_EQ(desc.Filter, D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT);

    gfx.mipMapping = "trilinear";
    const auto trilinear = ce::dx12_sampler_policy::Apply(desc, gfx);
    EXPECT_TRUE(trilinear.mipMappingModified);
    EXPECT_EQ(desc.Filter, D3D12_FILTER_MIN_MAG_MIP_LINEAR);
}

TEST(DX12SamplerPolicyTest, AggressiveModeIncludesOrdinaryPointAndClampSamplers) {
    GraphicsConfig gfx = ForcedAf();
    gfx.samplerOverrideMode = "aggressive";
    D3D12_SAMPLER_DESC desc = MaterialSampler(D3D12_FILTER_MIN_MAG_MIP_POINT);
    desc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;

    const auto result = ce::dx12_sampler_policy::Apply(desc, gfx);

    EXPECT_TRUE(result.Modified());
    EXPECT_EQ(desc.Filter, D3D12_FILTER_ANISOTROPIC);
    EXPECT_EQ(desc.MaxAnisotropy, 16u);
}

TEST(DX12SamplerPolicyTest, AggressiveModeStillProtectsBorderSamplers) {
    GraphicsConfig gfx = ForcedAf();
    gfx.samplerOverrideMode = "aggressive";
    D3D12_SAMPLER_DESC desc = MaterialSampler();
    desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    const D3D12_SAMPLER_DESC original = desc;

    const auto result = ce::dx12_sampler_policy::Apply(desc, gfx);

    EXPECT_FALSE(result.Modified());
    EXPECT_EQ(result.decision, ce::dx12_sampler_policy::Decision::BorderAddress);
    // NOLINTNEXTLINE(bugprone-suspicious-memory-comparison) - byte-identical preservation is the contract under test
    EXPECT_EQ(0, std::memcmp(&desc, &original, sizeof(desc)));
}

TEST(DX12SamplerPolicyTest, MalformedMipBiasIsIgnored) {
    for (const char* value : {"1suffix", "nan", "inf"}) {
        GraphicsConfig gfx = ForcedAf("default");
        gfx.mipBias = value;
        D3D12_SAMPLER_DESC desc = MaterialSampler();

        const auto result = ce::dx12_sampler_policy::Apply(desc, gfx);

        EXPECT_FALSE(result.Modified()) << value;
        EXPECT_FLOAT_EQ(desc.MipLODBias, 0.0f);
    }
}

TEST(DX12SamplerPolicyTest, IsIdempotentAndReportsAlreadyCompliant) {
    D3D12_SAMPLER_DESC desc = MaterialSampler();
    EXPECT_TRUE(ce::dx12_sampler_policy::Apply(desc, ForcedAf()).Modified());

    const auto second = ce::dx12_sampler_policy::Apply(desc, ForcedAf());
    EXPECT_FALSE(second.Modified());
    EXPECT_EQ(second.decision, ce::dx12_sampler_policy::Decision::AlreadyCompliant);
}

TEST(DX12SamplerPolicyTest, AppliesSamePolicyToStaticSamplerWithoutChangingBindings) {
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
    D3D12_STATIC_SAMPLER_DESC desc = {};
    desc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    desc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    desc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    desc.MaxAnisotropy = 1;
    desc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.MinLOD = 0.0f;
    desc.MaxLOD = D3D12_FLOAT32_MAX;
    desc.ShaderRegister = 7;
    desc.RegisterSpace = 3;
    desc.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    const auto result = ce::dx12_sampler_policy::Apply(desc, ForcedAf());

    EXPECT_TRUE(result.Modified());
    EXPECT_EQ(desc.Filter, D3D12_FILTER_ANISOTROPIC);
    EXPECT_EQ(desc.MaxAnisotropy, 16u);
    EXPECT_EQ(desc.ShaderRegister, 7u);
    EXPECT_EQ(desc.RegisterSpace, 3u);
    EXPECT_EQ(desc.ShaderVisibility, D3D12_SHADER_VISIBILITY_PIXEL);
}

TEST(DX12SamplerPolicyTest, RuntimeCoverageIncludesDynamicExportsAndPrecompiledRootSignatures) {
    const auto root = std::filesystem::current_path();
    // The IAT engine is split between the patching primitives and the per-API
    // installation that registers the dynamic exports; assert over both.
    const std::string iat = ReadTextFile(root / "hook" / "wrappers" / "iat_hook.cpp") + "\n" +
                            ReadTextFile(root / "hook" / "wrappers" / "iat_hook_init.cpp");
    const std::string hooks = ReadTextFile(root / "hook" / "apis" / "dx12_sampler_hooks.cpp");

    EXPECT_NE(iat.find("RegisterDynamicHook(\"D3D12CreateDevice\""), std::string::npos);
    EXPECT_NE(iat.find("RegisterDynamicHook(\"D3D12SerializeVersionedRootSignature\""), std::string::npos);
    EXPECT_NE(iat.find("FARPROC proc = ::GetProcAddress(hModule, lpProcName);"), std::string::npos);
    EXPECT_EQ(iat.find("oGetProcAddress"), std::string::npos);
    EXPECT_NE(hooks.find("D3D12CreateVersionedRootSignatureDeserializer"), std::string::npos);
    EXPECT_NE(hooks.find("D3D_ROOT_SIGNATURE_VERSION_1_0"), std::string::npos);
    EXPECT_NE(hooks.find("D3D_ROOT_SIGNATURE_VERSION_1_1"), std::string::npos);
    EXPECT_NE(hooks.find("precompiled-v1.2"), std::string::npos);
    EXPECT_NE(hooks.find("DetourCreateSampler2"), std::string::npos);
    EXPECT_NE(hooks.find("DetourFactoryCreateDevice"), std::string::npos);
    EXPECT_NE(hooks.find("std::unordered_map<void**, DeviceOriginals>"), std::string::npos);
    EXPECT_NE(hooks.find("CreateRootSignature is the single mutation boundary"), std::string::npos);
    EXPECT_EQ(hooks.find("ModifyStaticSamplers(modified, samplers, \"serialize-v1.0\")"), std::string::npos);
}

}  // namespace
