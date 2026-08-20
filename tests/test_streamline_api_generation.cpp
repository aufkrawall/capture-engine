#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "../hook/common/streamline_api_generation.h"

#include "source_fragment_reader.h"

namespace {

namespace api = ce::streamline_api;

using api::Generation;

std::string ReadSource(const std::filesystem::path& relativePath) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / relativePath);
}

TEST(StreamlineApiGenerationTest, ClassifiesOnlyUnambiguousExportSets) {
    EXPECT_EQ(api::Classify(/*anyV2OnlyExport=*/true, /*anyV1OnlyExport=*/false), Generation::V2);
    EXPECT_EQ(api::Classify(/*anyV2OnlyExport=*/false, /*anyV1OnlyExport=*/true), Generation::V1);

    // Neither set present, or both: CE knows nothing it can act on. A repack that exposes
    // both must be treated as unknown rather than guessed into one of them.
    EXPECT_EQ(api::Classify(false, false), Generation::Unknown);
    EXPECT_EQ(api::Classify(true, true), Generation::Unknown);
}

TEST(StreamlineApiGenerationTest, TheMarkerSetsAreDisjointAndNonEmpty) {
    ASSERT_GT(api::kV1OnlyExportCount, 0u);
    ASSERT_GT(api::kV2OnlyExportCount, 0u);
    for (size_t i = 0; i < api::kV1OnlyExportCount; ++i) {
        ASSERT_NE(api::kV1OnlyExports[i], nullptr);
        for (size_t j = 0; j < api::kV2OnlyExportCount; ++j) {
            EXPECT_STRNE(api::kV1OnlyExports[i], api::kV2OnlyExports[j])
                << "a marker present in both generations cannot classify anything";
        }
    }
    // The exports The Witcher 3's sl.interposer 1.5.6 actually has, and the ones it does
    // not. Getting either list wrong reinstalls the crash.
    const auto has = [](const char* const* list, size_t count, const char* name) {
        for (size_t i = 0; i < count; ++i) {
            if (std::string(list[i]) == name) {
                return true;
            }
        }
        return false;
    };
    EXPECT_TRUE(has(api::kV1OnlyExports, api::kV1OnlyExportCount, "slSetFeatureConstants"));
    EXPECT_TRUE(has(api::kV1OnlyExports, api::kV1OnlyExportCount, "slGetFeatureSettings"));
    EXPECT_TRUE(has(api::kV2OnlyExports, api::kV2OnlyExportCount, "slSetTagForFrame"));
    EXPECT_TRUE(has(api::kV2OnlyExports, api::kV2OnlyExportCount, "slGetNewFrameToken"));
    // slSetTag and slEvaluateFeature exist in BOTH generations with different signatures.
    // They are the reason this header exists and must never be used to classify.
    EXPECT_FALSE(has(api::kV1OnlyExports, api::kV1OnlyExportCount, "slSetTag"));
    EXPECT_FALSE(has(api::kV2OnlyExports, api::kV2OnlyExportCount, "slSetTag"));
    EXPECT_FALSE(has(api::kV1OnlyExports, api::kV1OnlyExportCount, "slEvaluateFeature"));
    EXPECT_FALSE(has(api::kV2OnlyExports, api::kV2OnlyExportCount, "slEvaluateFeature"));
}

TEST(StreamlineApiGenerationTest, AnAbiSensitiveHookOnlyGoesOnItsOwnGeneration) {
    EXPECT_TRUE(api::MayInstallAbiSensitiveHook(Generation::V2, Generation::V2));
    EXPECT_TRUE(api::MayInstallAbiSensitiveHook(Generation::V1, Generation::V1));

    // The Witcher 3 crash: the 2.x hook on a 1.x interposer took the caller's command list
    // out of RCX through a uint32_t parameter and handed back the truncated half.
    EXPECT_FALSE(api::MayInstallAbiSensitiveHook(Generation::V1, Generation::V2));
    EXPECT_FALSE(api::MayInstallAbiSensitiveHook(Generation::V2, Generation::V1));

    // Unknown on either side installs nothing. CE never guesses a foreign calling convention.
    EXPECT_FALSE(api::MayInstallAbiSensitiveHook(Generation::Unknown, Generation::V1));
    EXPECT_FALSE(api::MayInstallAbiSensitiveHook(Generation::Unknown, Generation::V2));
    EXPECT_FALSE(api::MayInstallAbiSensitiveHook(Generation::V1, Generation::Unknown));
    EXPECT_FALSE(api::MayInstallAbiSensitiveHook(Generation::Unknown, Generation::Unknown));
}

TEST(StreamlineApiGenerationTest, DllSubstitutionNeverCrossesGenerations) {
    EXPECT_TRUE(api::MayRedirectStreamlineModuleAcrossGenerations(Generation::V2, Generation::V2));
    EXPECT_TRUE(api::MayRedirectStreamlineModuleAcrossGenerations(Generation::V1, Generation::V1));

    // streamline_dll_path pointing a 1.x game at a 2.x distribution: the 1.x-only imports
    // do not exist there, so the loader kills the process before its first frame.
    EXPECT_FALSE(api::MayRedirectStreamlineModuleAcrossGenerations(Generation::V1, Generation::V2));
    EXPECT_FALSE(api::MayRedirectStreamlineModuleAcrossGenerations(Generation::V2, Generation::V1));

    // Nothing provable: keep the historical behavior instead of breaking a working setup on
    // a version resource CE could not read.
    EXPECT_TRUE(api::MayRedirectStreamlineModuleAcrossGenerations(Generation::Unknown, Generation::V2));
    EXPECT_TRUE(api::MayRedirectStreamlineModuleAcrossGenerations(Generation::V1, Generation::Unknown));
}

TEST(StreamlineApiGenerationTest, GenerationComesFromTheFileVersionMajorForPluginsToo) {
    EXPECT_EQ(api::GenerationFromMajorVersion(1), Generation::V1);  // sl.interposer 1.5.6
    EXPECT_EQ(api::GenerationFromMajorVersion(2), Generation::V2);
    EXPECT_EQ(api::GenerationFromMajorVersion(0), Generation::Unknown);
    EXPECT_EQ(api::GenerationFromMajorVersion(3), Generation::Unknown);
}

TEST(StreamlineApiGenerationTest, OnlyAPlausibleResourceStateSurvivesTheV1LayoutCheck) {
    // COMMON/PRESENT is zero and must be accepted.
    EXPECT_TRUE(api::IsPlausibleV1ResourceState(0));
    EXPECT_TRUE(api::IsPlausibleV1ResourceState(0x4));   // RENDER_TARGET
    EXPECT_TRUE(api::IsPlausibleV1ResourceState(0xC0));  // NON_PIXEL | PIXEL shader resource
    // A misread field is overwhelmingly likely to carry bits no resource state defines. A
    // wrong before-state would emit a wrong barrier, which is a GPU fault, not a bad log.
    EXPECT_FALSE(api::IsPlausibleV1ResourceState(0xDEADBEEF));
    EXPECT_FALSE(api::IsPlausibleV1ResourceState(0x10000000));

    alignas(8) uint8_t object[16] = {};
    void* aligned = object;
    EXPECT_TRUE(api::LooksLikeV1UiResource(api::kV1ResourceTypeTex2d, aligned, 0x4));
    // Anything but a 2D texture, a null native, an unaligned native, or an impossible state
    // means the offsets did not land on a real sl::Resource.
    EXPECT_FALSE(api::LooksLikeV1UiResource(1, aligned, 0x4));
    EXPECT_FALSE(api::LooksLikeV1UiResource(api::kV1ResourceTypeTex2d, nullptr, 0x4));
    EXPECT_FALSE(api::LooksLikeV1UiResource(api::kV1ResourceTypeTex2d,
                                            reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(aligned) + 1), 0x4));
    EXPECT_FALSE(api::LooksLikeV1UiResource(api::kV1ResourceTypeTex2d, aligned, 0xDEADBEEF));
}

TEST(StreamlineApiGenerationTest, TheUiBufferTypeAgreesWithTheTwoDotXConstant) {
    // sl.common 1.5.6's own buffer-type name table puts eBufferTypeUIColorAndAlpha at index
    // 23, which is also 2.x's kBufferTypeUIColorAndAlpha. The 1.x hook and the 2.x hook must
    // therefore agree on the number.
    EXPECT_EQ(api::kV1BufferTypeUIColorAndAlpha, 23u);
    const std::string internals = ReadSource("hook/apis/streamline_hook_internal.h");
    ASSERT_FALSE(internals.empty());
    EXPECT_NE(internals.find("streamline_hook_kSLBufferTypeUIColorAndAlpha = 23"), std::string::npos);
}

TEST(StreamlineApiGenerationTest, InstallerGatesBothAbiSensitiveHooksOnTheDetectedGeneration) {
    const std::string source = ReadSource("hook/apis/streamline_hook_install.cpp");
    ASSERT_FALSE(source.empty());

    // The generation has to be resolved before anything is patched.
    const size_t resolve = source.find("ResolveStreamlineGeneration(module, moduleBaseName)");
    ASSERT_NE(resolve, std::string::npos);
    const size_t mayHook = source.find("mayHookAbiSensitive", resolve);
    ASSERT_NE(mayHook, std::string::npos);

    // Both inline installs and both IAT patches must be gated; an ungated one reinstates
    // the truncating hook on a 1.x interposer.
    EXPECT_NE(source.find("originalSetTag && mayHookAbiSensitive"), std::string::npos);
    EXPECT_NE(source.find("originalEvaluateFeature && mayHookAbiSensitive"), std::string::npos);
    EXPECT_NE(source.find("if (originalSetTag && mayHookAbiSensitive) {"), std::string::npos);
    EXPECT_NE(source.find("if (originalEvaluateFeature && mayHookAbiSensitive) {"), std::string::npos);

    // The unconditional dynamic-hook registration for the two exports must be gone: it ran
    // before any module was inspected, so it could not know the generation.
    const size_t registerOnce = source.find("void RegisterDynamicHooksOnce()");
    const size_t registerOnceEnd = source.find("bool InstallHooksForModule", registerOnce);
    ASSERT_NE(registerOnce, std::string::npos);
    ASSERT_NE(registerOnceEnd, std::string::npos);
    const std::string registerBody = source.substr(registerOnce, registerOnceEnd - registerOnce);
    EXPECT_EQ(registerBody.find("RegisterDynamicHookFiltered(\"slSetTag\""), std::string::npos);
    EXPECT_EQ(registerBody.find("RegisterDynamicHookFiltered(\"slEvaluateFeature\""), std::string::npos);
}

TEST(StreamlineApiGenerationTest, TheV1HooksKeepTheOneDotXCallingConvention) {
    const std::string header = ReadSource("hook/apis/streamline_hook_v1.h");
    ASSERT_FALSE(header.empty());

    // 1.x returns bool in AL, takes the command buffer FIRST in slEvaluateFeature, and takes
    // the resource pointer first in slSetTag. Every one of those differs from 2.x.
    EXPECT_NE(header.find("bool Hooked_slSetTagV1(const void* resource, uint32_t bufferType, uint32_t id, "
                          "const void* extent)"),
              std::string::npos);
    EXPECT_NE(header.find("bool Hooked_slEvaluateFeatureV1(void* commandBuffer, uint32_t feature, "
                          "uint32_t frameIndex, uint32_t id)"),
              std::string::npos);

    const std::string source = ReadSource("hook/apis/streamline_hook_v1.cpp");
    ASSERT_FALSE(source.empty());
    // Arguments are forwarded verbatim - the whole bug was a re-pack that truncated one.
    EXPECT_NE(source.find("original(resource, bufferType, id, extent)"), std::string::npos);
    EXPECT_NE(source.find("original(commandBuffer, feature, frameIndex, id)"), std::string::npos);
    // The 1.x resource layout is proven per resource before it is used, never assumed.
    const size_t looksLike = source.find("LooksLikeV1UiResource");
    const size_t comCheck = source.find("LooksCallableAsCom(native)", looksLike);
    const size_t queryInterface = source.find("QueryInterface(IID_PPV_ARGS(&texture))", comCheck);
    ASSERT_NE(looksLike, std::string::npos);
    ASSERT_NE(comCheck, std::string::npos);
    ASSERT_NE(queryInterface, std::string::npos);
    EXPECT_LT(looksLike, comCheck);
    EXPECT_LT(comCheck, queryInterface);
}

TEST(StreamlineApiGenerationTest, TheV1HooksAreFreeWhileTheUiRouteIsDormant) {
    const std::string source = ReadSource("hook/apis/streamline_hook_v1.cpp");
    ASSERT_FALSE(source.empty());

    // slSetTag runs several times per frame and slEvaluateFeature at least once. Neither may
    // pay for a VirtualQuery, a QueryInterface or a held reference while the official-UI
    // bootstrap is dormant, which is the whole time DLSS-G is not arming.
    const size_t tagGate = source.find("IsFrameTagTrackingActive()");
    const size_t tagProbe = source.find("TryReadV1UiResource(resource, &tag)", tagGate);
    ASSERT_NE(tagGate, std::string::npos);
    ASSERT_NE(tagProbe, std::string::npos);
    EXPECT_LT(tagGate, tagProbe);

    const size_t evaluateGate = source.find("const bool needsCommandList =");
    const size_t evaluateProbe = source.find("LooksCallableAsCom(commandBuffer)", evaluateGate);
    ASSERT_NE(evaluateGate, std::string::npos);
    ASSERT_NE(evaluateProbe, std::string::npos);
    EXPECT_LT(evaluateGate, evaluateProbe);

    // The remembered texture is adopted from the QueryInterface that proved it, never
    // released and re-taken: a gap there is a window for the game to free it underneath CE.
    EXPECT_NE(source.find("Adopts `next`'s reference"), std::string::npos);
    EXPECT_EQ(source.find("g_pending.resource->AddRef()"), std::string::npos);
}

TEST(StreamlineApiGenerationTest, TheDllOverrideRefusesACrossGenerationSubstitution) {
    const std::string source = ReadSource("hook/main_redirect.cpp");
    ASSERT_FALSE(source.empty());

    // Both redirect routes - the plain sl.* name match and the NGX model repository - must
    // consult the generation, and only after the duplicate-instance check they already had.
    const size_t modelGuard = source.find("StreamlineOverrideGenerationMatches(modelFinal, modelDllName)");
    const size_t nameGuard = source.find("StreamlineOverrideGenerationMatches(finalPath, filename.c_str())");
    ASSERT_NE(modelGuard, std::string::npos);
    ASSERT_NE(nameGuard, std::string::npos);
    EXPECT_NE(source.find("MayRedirectStreamlineModuleAcrossGenerations"), std::string::npos);
    // The refusal has to say what to do instead, or it is just a broken feature.
    EXPECT_NE(source.find("dlss_sr_dll_path / dlss_fg_dll_path"), std::string::npos);
}

}  // namespace
