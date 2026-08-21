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

TEST(StreamlineBridgePolicyTest, FrameGenerationKeepsTheValueTheGameActuallySends) {
    // Measured, after being inferred wrong. sl.interposer 1.5.6's name table lists DLSS_G
    // sixth, and reading position as value put it at 5; The Witcher 3 session
    // 20260821_041255 records the game passing feature 1000 to slSetFeatureConstants right
    // after the Reflex constants. 1.x already uses the same out-of-line 1000 as 2.x, so
    // DLSS-G is an identity mapping and 5 is not a 1.x feature at all.
    EXPECT_EQ(bridge::kV1FeatureDLSS_G, 1000u);
    uint32_t mapped = 0;
    ASSERT_TRUE(bridge::TranslateV1FeatureToV2(bridge::kV1FeatureDLSS_G, &mapped));
    EXPECT_EQ(mapped, bridge::kV2FeatureDLSS_G);
    EXPECT_EQ(mapped, 1000u);

    // 5 is kFeatureDeepDVC in 2.x and means nothing in 1.5.6, so it must never translate.
    uint32_t stray = 0xFFFFFFFEu;
    EXPECT_FALSE(bridge::TranslateV1FeatureToV2(bridge::kV2FeatureDeepDVC, &stray));
    EXPECT_EQ(stray, 0xFFFFFFFEu);
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
    EXPECT_FALSE(bridge::TranslateV1FeatureToV2(999, &mapped));
    EXPECT_FALSE(bridge::TranslateV1FeatureToV2(1001, &mapped));
    EXPECT_EQ(mapped, 0xFFFFFFFEu) << "a refused translation must not write an output";
}

TEST(StreamlineBridgePolicyTest, FeatureTranslationNeverCollapsesTwoFeaturesOntoOne) {
    std::set<uint32_t> produced;
    for (uint32_t v1 : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 1000u, UINT32_MAX}) {
        uint32_t mapped = 0;
        if (bridge::TranslateV1FeatureToV2(v1, &mapped)) {
            EXPECT_TRUE(produced.insert(mapped).second)
                << "1.x feature " << v1 << " maps onto an already-used 2.x value " << mapped;
        }
    }
}

TEST(StreamlineBridgePolicyTest, NamesEach1xFeatureDistinctlyForDiagnostics) {
    // These strings identify which call a refusal or a recorded payload belongs to, so the
    // two colliding values must not describe themselves as each other.
    EXPECT_STREQ(bridge::DescribeV1Feature(bridge::kV1FeatureDLSS_G), "DLSS-G");
    EXPECT_STREQ(bridge::DescribeV1Feature(bridge::kV1FeatureDebug), "Debug");
    EXPECT_STREQ(bridge::DescribeV1Feature(bridge::kV1FeatureDLSS), "DLSS");
    EXPECT_STREQ(bridge::DescribeV1Feature(bridge::kV1FeatureCommon), "Common");

    std::set<std::string> names;
    for (uint32_t f : {bridge::kV1FeatureDLSS, bridge::kV1FeatureNRD, bridge::kV1FeatureNIS,
                       bridge::kV1FeatureReflex, bridge::kV1FeatureDebug, bridge::kV1FeatureDLSS_G,
                       bridge::kV1FeatureCommon}) {
        EXPECT_TRUE(names.insert(bridge::DescribeV1Feature(f)).second) << "duplicate name for feature " << f;
    }
    // An unmapped value must be reported as unknown, never folded into a neighbour.
    EXPECT_STREQ(bridge::DescribeV1Feature(77), "an unrecognized 1.x feature");
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

TEST(StreamlineBridgePolicyTest, DerivesTheSdkVersionFromWhicheverRuntimeIsStaged) {
    // The staged runtime is expected to be updated, so no version may be hard-coded: CE
    // declares itself as the SDK it actually found. Hard-coding one would mis-declare CE to
    // every other folder the user points it at.
    EXPECT_EQ(bridge::StreamlineSdkVersion(2, 11, 1),
              (2ull << 48) | (11ull << 32) | (1ull << 16) | bridge::kStreamlineSdkVersionMagic);
    EXPECT_EQ(bridge::StreamlineSdkVersion(2, 12, 0),
              (2ull << 48) | (12ull << 32) | (0ull << 16) | bridge::kStreamlineSdkVersionMagic);
    EXPECT_NE(bridge::StreamlineSdkVersion(2, 12, 0), bridge::StreamlineSdkVersion(2, 11, 1));

    // A version nobody has seen yet must still produce a well-formed value, with no table
    // anywhere to keep updated.
    EXPECT_EQ(bridge::StreamlineSdkVersion(2, 20, 3) & 0xffffull, bridge::kStreamlineSdkVersionMagic);
    EXPECT_EQ(bridge::StreamlineSdkVersion(2, 20, 3) >> 48, 2ull);
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

// ---------------------------------------------------------------------------
// Which generation answers for the whole process
// ---------------------------------------------------------------------------

TEST(StreamlineBridgePolicyTest, TheBridgedRuntimeAnswersForTheProcess) {
    // The 1.x interposer is loaded from process start, so it is almost always classified
    // first - but once bridged, every Streamline call the game makes reaches CE's thunks and
    // then the 2.x runtime. First-seen must not win.
    EXPECT_EQ(bridge::AuthoritativeProcessGeneration(/*bridgeActive=*/true, Generation::V1), Generation::V2);
    EXPECT_EQ(bridge::AuthoritativeProcessGeneration(true, Generation::Unknown), Generation::V2);
    EXPECT_EQ(bridge::AuthoritativeProcessGeneration(true, Generation::V2), Generation::V2);
}

TEST(StreamlineBridgePolicyTest, WithoutABridgeTheFirstSeenGenerationStillAnswers) {
    // Unbridged processes must behave exactly as before: one distribution, one answer.
    for (Generation g : {Generation::Unknown, Generation::V1, Generation::V2}) {
        EXPECT_EQ(bridge::AuthoritativeProcessGeneration(/*bridgeActive=*/false, g), g);
    }
}

TEST(StreamlineBridgePolicyTest, TheProcessWideAnswerMustNeverDecideAPerModuleAbiHook) {
    // This is why ClassifyModuleGeneration exists and why the install path stopped caching
    // one generation for every module. In a bridged process the authoritative answer is V2,
    // and applying it to the still-resident 1.x interposer would authorise precisely the
    // 2.x-shaped hooks on a 1.x module that truncate the caller's command-list pointer -
    // the Witcher 3 crash the generation gate was written for.
    const Generation processWide = bridge::AuthoritativeProcessGeneration(/*bridgeActive=*/true, Generation::V1);
    EXPECT_TRUE(ce::streamline_api::MayInstallAbiSensitiveHook(processWide, Generation::V2))
        << "the process-wide answer would authorise a 2.x hook";
    EXPECT_FALSE(ce::streamline_api::MayInstallAbiSensitiveHook(Generation::V1, Generation::V2))
        << "the 1.x module's own generation is the answer that must be used";
}

TEST(StreamlineBridgePolicyTest, AnActiveBridgeStandsTheOrdinaryRedirectDown) {
    // Both mechanisms want the same configured folder for opposite purposes. Letting the
    // path substitution also fire would rewrite the game's own 1.x plugin loads into the
    // 2.x folder - the version mixing the redirect guards were written for.
    EXPECT_TRUE(bridge::StreamlineRedirectSuppressedByBridge(/*bridgeActive=*/true));
    EXPECT_FALSE(bridge::StreamlineRedirectSuppressedByBridge(/*bridgeActive=*/false));
}

}  // namespace
