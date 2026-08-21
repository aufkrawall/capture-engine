#include <gtest/gtest.h>

#include <cstdint>
#include <set>
#include <string>

#include "../hook/apis/streamline_bridge_policy.h"

namespace {

namespace bridge = ce::streamline_bridge;

using ce::streamline_api::Generation;

bridge::ActivationInputs UpgradeableProcess() {
    bridge::ActivationInputs inputs;
    inputs.upgradeEnabled = true;
    inputs.runtimePathConfigured = true;
    inputs.processGeneration = Generation::V1;
    inputs.runtimeGeneration = Generation::V2;
    inputs.gameAlreadyInitializedStreamline = false;
    inputs.gameAlreadyCreatedDeviceOrFactory = false;
    return inputs;
}

// ---------------------------------------------------------------------------
// Feature translation
// ---------------------------------------------------------------------------

TEST(StreamlineBridgePolicyTest, TranslatesFrameGenerationOffItsCollidingValue) {
    // The whole reason this table exists. 1.x eFeatureDLSS_G is 5; 2.x reserves 5 for
    // kFeatureDeepDVC and puts DLSS-G at 1000. Passing 5 through unchanged would silently
    // drive an unrelated feature instead of frame generation.
    uint32_t mapped = 0;
    ASSERT_TRUE(bridge::TranslateV1FeatureToV2(bridge::kV1FeatureDLSS_G, &mapped));
    EXPECT_EQ(mapped, bridge::kV2FeatureDLSS_G);
    EXPECT_EQ(mapped, 1000u);
    EXPECT_NE(mapped, bridge::kV1FeatureDLSS_G) << "an identity mapping here selects DeepDVC";
    EXPECT_NE(mapped, bridge::kV2FeatureDeepDVC);
}

TEST(StreamlineBridgePolicyTest, TranslatesTheFeaturesThatKeptTheirValues) {
    for (const auto& [v1, v2] : {std::pair<uint32_t, uint32_t>{bridge::kV1FeatureDLSS, bridge::kV2FeatureDLSS},
                                 {bridge::kV1FeatureNIS, bridge::kV2FeatureNIS},
                                 {bridge::kV1FeatureReflex, bridge::kV2FeatureReflex},
                                 {bridge::kV1FeatureCommon, bridge::kV2FeatureCommon}}) {
        uint32_t mapped = 0xFFFFFFFEu;
        EXPECT_TRUE(bridge::TranslateV1FeatureToV2(v1, &mapped)) << "1.x feature " << v1;
        EXPECT_EQ(mapped, v2) << "1.x feature " << v1;
    }
}

TEST(StreamlineBridgePolicyTest, RefusesFeaturesWithNoFaithful2xMapping) {
    uint32_t mapped = 0xFFFFFFFEu;

    // eFeatureDebug is 4 in 1.x, which is kFeaturePCL in 2.x - a collision in the opposite
    // direction from DLSS-G, and 1.x's debug feature has no 2.x counterpart at all.
    EXPECT_FALSE(bridge::TranslateV1FeatureToV2(bridge::kV1FeatureDebug, &mapped));
    EXPECT_EQ(bridge::kV1FeatureDebug, bridge::kV2FeaturePCL)
        << "the collision this refusal exists for";

    // NRD was removed from Streamline entirely; 2.x spells the slot kFeatureNRD_INVALID.
    EXPECT_FALSE(bridge::TranslateV1FeatureToV2(bridge::kV1FeatureNRD, &mapped));

    // Nothing outside the 1.5.6 table is guessed at.
    EXPECT_FALSE(bridge::TranslateV1FeatureToV2(6, &mapped));
    EXPECT_FALSE(bridge::TranslateV1FeatureToV2(1000, &mapped));
    EXPECT_EQ(mapped, 0xFFFFFFFEu) << "a refused translation must not write an output";
}

TEST(StreamlineBridgePolicyTest, FeatureTranslationNeverCollapsesTwoFeaturesOntoOne) {
    std::set<uint32_t> produced;
    for (uint32_t v1 = 0; v1 <= 8; ++v1) {
        uint32_t mapped = 0;
        if (bridge::TranslateV1FeatureToV2(v1, &mapped)) {
            EXPECT_TRUE(produced.insert(mapped).second)
                << "1.x feature " << v1 << " maps onto an already-used 2.x value " << mapped;
        }
    }
}

// ---------------------------------------------------------------------------
// Buffer type translation
// ---------------------------------------------------------------------------

TEST(StreamlineBridgePolicyTest, BufferTypesAreIdentityAcrossTheWholeKnown1xTable) {
    // sl.common 1.5.6's own name table holds exactly 38 entries and every one lands on the
    // same value in the 2.11.1 headers, so the bridge range-checks instead of mapping.
    for (uint32_t type = 0; type <= bridge::kV1BufferTypeMaxKnown; ++type) {
        uint32_t mapped = 0xFFFFFFFEu;
        EXPECT_TRUE(bridge::TranslateV1BufferTypeToV2(type, &mapped)) << "buffer type " << type;
        EXPECT_EQ(mapped, type) << "buffer type " << type;
    }
    EXPECT_EQ(bridge::kV1BufferTypeMaxKnown, 37u)
        << "the 1.x table is Depth(0)..TransparencyAndCompositionMaskHint(37)";
}

TEST(StreamlineBridgePolicyTest, BufferTypeUiColorAndAlphaKeepsTheValueCeAlreadyUses) {
    uint32_t mapped = 0;
    ASSERT_TRUE(bridge::TranslateV1BufferTypeToV2(ce::streamline_api::kV1BufferTypeUIColorAndAlpha, &mapped));
    EXPECT_EQ(mapped, 23u);
}

TEST(StreamlineBridgePolicyTest, RefusesBufferTypesOutsideTheVerified1xTable) {
    uint32_t mapped = 0xFFFFFFFEu;
    EXPECT_FALSE(bridge::TranslateV1BufferTypeToV2(bridge::kV1BufferTypeMaxKnown + 1, &mapped));
    EXPECT_FALSE(bridge::TranslateV1BufferTypeToV2(0xFFFFFFFFu, &mapped));
    EXPECT_EQ(mapped, 0xFFFFFFFEu) << "a refused translation must not write an output";
}

// ---------------------------------------------------------------------------
// Import surface
// ---------------------------------------------------------------------------

TEST(StreamlineBridgePolicyTest, CoversExactlyTheImportsTheReferenceTitleUses) {
    // The 15 symbols The Witcher 3 imports from sl.interposer.dll, read off its import
    // table. Missing one leaves a slot pointing at the 1.x runtime in a bridged process.
    const char* imported[] = {
        "slInit", "slShutdown", "slIsFeatureSupported", "slSetTag", "slSetConstants",
        "slSetFeatureConstants", "slGetFeatureSettings", "slEvaluateFeature",
        "CreateDXGIFactory", "CreateDXGIFactory1", "CreateDXGIFactory2", "DXGIGetDebugInterface1",
        "D3D12CreateDevice", "D3D12GetDebugInterface", "D3D12SerializeVersionedRootSignature",
    };
    for (const char* name : imported) {
        EXPECT_TRUE(bridge::IsBridgedExport(name)) << name << " is imported but not bridged";
    }
    EXPECT_EQ(bridge::kTranslatedV1ExportCount + bridge::kPassThroughExportCount,
              sizeof(imported) / sizeof(imported[0]));
}

TEST(StreamlineBridgePolicyTest, SeparatesTranslatedCallsFromPassThroughs) {
    // Streamline's own API needs translation; the DXGI/D3D12 entry points the interposer
    // re-exports are Microsoft's signatures and identical in both generations.
    EXPECT_TRUE(bridge::IsTranslatedV1Export("slEvaluateFeature"));
    EXPECT_FALSE(bridge::IsPassThroughExport("slEvaluateFeature"));

    EXPECT_TRUE(bridge::IsPassThroughExport("D3D12CreateDevice"));
    EXPECT_FALSE(bridge::IsTranslatedV1Export("D3D12CreateDevice"));

    for (size_t i = 0; i < bridge::kTranslatedV1ExportCount; ++i) {
        EXPECT_FALSE(bridge::IsPassThroughExport(bridge::kTranslatedV1Exports[i]))
            << bridge::kTranslatedV1Exports[i] << " cannot be both";
    }
}

TEST(StreamlineBridgePolicyTest, DoesNotClaimExportsItHasNoPlanFor) {
    // Present in the 1.x interposer but not imported by the reference title. Claiming a
    // slot CE cannot serve is worse than leaving it with the game's own runtime.
    for (const char* name : {"slAllocateResources", "slFreeResources", "slGetFeatureConfiguration",
                             "slUpgradeInterface", "slGetHooks", "slGetNumHooks",
                             "slSetFeatureEnabled", "slIsFeatureEnabled"}) {
        EXPECT_FALSE(bridge::IsBridgedExport(name)) << name;
    }
    EXPECT_FALSE(bridge::IsBridgedExport(""));
    EXPECT_FALSE(bridge::IsBridgedExport(nullptr));
    // Matching is exact, never by prefix.
    EXPECT_FALSE(bridge::IsBridgedExport("slInitEx"));
    EXPECT_FALSE(bridge::IsBridgedExport("slIni"));
}

// ---------------------------------------------------------------------------
// Preferences
// ---------------------------------------------------------------------------

TEST(StreamlineBridgePolicyTest, PinsTheStagedRuntimeByTurningOtaOff) {
    const uint64_t flags = bridge::BridgePreferenceFlags();
    EXPECT_EQ(flags & bridge::kPrefAllowOTA, 0u)
        << "a driver-side OTA update must not choose which plugins the bridge runs";
    EXPECT_EQ(flags & bridge::kPrefLoadDownloadedPlugins, 0u);
    // Both are on in the SDK default, so this is a real departure and not a no-op.
    EXPECT_NE(bridge::kPrefSdkDefault & bridge::kPrefAllowOTA, 0u);
    EXPECT_NE(bridge::kPrefSdkDefault & bridge::kPrefLoadDownloadedPlugins, 0u);
}

TEST(StreamlineBridgePolicyTest, AsksStreamlineForAFactoryProxyBecauseCeIsInjected) {
    // Streamline documents this flag for overlays that operate by injection rather than
    // integration; without it Streamline mutates the base interface v-table underneath
    // CE's own DXGI hooks.
    EXPECT_NE(bridge::BridgePreferenceFlags() & bridge::kPrefUseDXGIFactoryProxy, 0u);
    EXPECT_EQ(bridge::kPrefSdkDefault & bridge::kPrefUseDXGIFactoryProxy, 0u);
}

TEST(StreamlineBridgePolicyTest, KeepsTheSdkDefaultsItHasNoReasonToChange) {
    EXPECT_NE(bridge::BridgePreferenceFlags() & bridge::kPrefDisableCLStateTracking, 0u);
}

// ---------------------------------------------------------------------------
// Activation
// ---------------------------------------------------------------------------

TEST(StreamlineBridgePolicyTest, ActivatesOnlyForAnOptedInOneToTwoUpgrade) {
    EXPECT_EQ(bridge::DecideActivation(UpgradeableProcess()), bridge::ActivationDecision::Activate);
    EXPECT_TRUE(bridge::ShouldActivate(UpgradeableProcess()));
}

TEST(StreamlineBridgePolicyTest, StaysOffUnlessExplicitlyEnabled) {
    // Default off: the bridge is more invasive than the path overrides and carries the same
    // anti-cheat warning, so an existing streamline_dll_path must never activate it.
    auto inputs = UpgradeableProcess();
    inputs.upgradeEnabled = false;
    EXPECT_EQ(bridge::DecideActivation(inputs), bridge::ActivationDecision::DeclinedNotEnabled);

    inputs = UpgradeableProcess();
    inputs.runtimePathConfigured = false;
    EXPECT_EQ(bridge::DecideActivation(inputs), bridge::ActivationDecision::DeclinedNoRuntimePath);
}

TEST(StreamlineBridgePolicyTest, RefusesEveryGenerationPairingThatIsNotAnUpgrade) {
    const Generation generations[] = {Generation::Unknown, Generation::V1, Generation::V2};
    for (Generation process : generations) {
        for (Generation runtime : generations) {
            auto inputs = UpgradeableProcess();
            inputs.processGeneration = process;
            inputs.runtimeGeneration = runtime;
            const bool isUpgrade = (process == Generation::V1 && runtime == Generation::V2);
            EXPECT_EQ(bridge::ShouldActivate(inputs), isUpgrade)
                << "process=" << static_cast<int>(process) << " runtime=" << static_cast<int>(runtime);
        }
    }
}

TEST(StreamlineBridgePolicyTest, RefusesOnceTheGameHasDrivenItsOwnRuntime) {
    // Taking the imports over after the game has initialised 1.x, or created its device
    // through the 1.x interposer, leaves half the process talking to each generation. The
    // bridge is all-or-nothing, so lateness declines instead of half-switching.
    auto inputs = UpgradeableProcess();
    inputs.gameAlreadyInitializedStreamline = true;
    EXPECT_EQ(bridge::DecideActivation(inputs), bridge::ActivationDecision::DeclinedTooLate);

    inputs = UpgradeableProcess();
    inputs.gameAlreadyCreatedDeviceOrFactory = true;
    EXPECT_EQ(bridge::DecideActivation(inputs), bridge::ActivationDecision::DeclinedTooLate);
}

TEST(StreamlineBridgePolicyTest, ADefaultConstructedProcessNeverActivates) {
    EXPECT_FALSE(bridge::ShouldActivate(bridge::ActivationInputs{}));
}

TEST(StreamlineBridgePolicyTest, EveryDecisionExplainsItself) {
    for (auto decision : {bridge::ActivationDecision::Activate, bridge::ActivationDecision::DeclinedNotEnabled,
                          bridge::ActivationDecision::DeclinedNoRuntimePath,
                          bridge::ActivationDecision::DeclinedNotAnUpgrade,
                          bridge::ActivationDecision::DeclinedTooLate}) {
        const char* text = bridge::Describe(decision);
        ASSERT_NE(text, nullptr);
        EXPECT_GT(std::string(text).size(), 8u);
    }
}

TEST(StreamlineBridgePolicyTest, AnActiveBridgeStandsTheOrdinaryRedirectDown) {
    // Both mechanisms want the same configured folder for opposite purposes. Letting the
    // path substitution also fire would rewrite the game's own 1.x plugin loads into the
    // 2.x folder - the version mixing the redirect guards were written for.
    EXPECT_TRUE(bridge::StreamlineRedirectSuppressedByBridge(/*bridgeActive=*/true));
    EXPECT_FALSE(bridge::StreamlineRedirectSuppressedByBridge(/*bridgeActive=*/false));
}

}  // namespace
