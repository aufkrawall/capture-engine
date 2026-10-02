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

TEST(StreamlineBridgePolicyTest, AnAlreadyInitialised1xRuntimeIsTakenOverAndShutDown) {
    // The regression this pins: refusing here made the feature unreachable. CE reaches a 1.x
    // DX12 title through WMI notification, a config reload and a remote-thread LoadLibrary,
    // and in a title whose executable imports D3D12/DXGI FROM sl.interposer.dll, `d3d12.dll`
    // only enters the process when sl.common.dll drags it in from inside slInit - so CE's
    // arrival signal and this deadline are the same event. Sessions 20260821_151738 and
    // 20260821_151924 both refused for exactly this reason.
    //
    // slInit is recoverable: CE takes the imports over and shuts that runtime back down.
    auto inputs = UpgradeableProcess();
    inputs.gameAlreadyInitializedStreamline = true;
    EXPECT_EQ(bridge::DecideActivation(inputs), bridge::ActivationDecision::ActivateAndQuiesce);
    EXPECT_TRUE(bridge::ShouldActivate(inputs));
    EXPECT_TRUE(bridge::RequiresLegacyQuiesce(bridge::DecideActivation(inputs)));
}

TEST(StreamlineBridgePolicyTest, RefusesOnceTheGameOwnsItsDevice) {
    // Device creation is the one step no in-memory takeover undoes: a 2.x runtime can only
    // drive a device its own interposer created. This one stays a refusal.
    auto inputs = UpgradeableProcess();
    inputs.gameAlreadyCreatedDeviceOrFactory = true;
    EXPECT_EQ(bridge::DecideActivation(inputs), bridge::ActivationDecision::DeclinedGameOwnsItsDevice);
    EXPECT_FALSE(bridge::ShouldActivate(inputs));

    // And it outranks the recoverable one, so a process that is past both never activates.
    inputs.gameAlreadyInitializedStreamline = true;
    EXPECT_EQ(bridge::DecideActivation(inputs), bridge::ActivationDecision::DeclinedGameOwnsItsDevice);
}

TEST(StreamlineBridgePolicyTest, ShutsTheLegacyRuntimeDownOnlyWhenOneIsActuallyUp) {
    // An early takeover must not call slShutdown on a runtime that never initialised: that
    // is a state change made for no reason, on a runtime the game may still be about to use
    // if the bridge later falls back to it.
    EXPECT_FALSE(bridge::RequiresLegacyQuiesce(bridge::ActivationDecision::Activate));
    EXPECT_TRUE(bridge::RequiresLegacyQuiesce(bridge::ActivationDecision::ActivateAndQuiesce));
    EXPECT_FALSE(bridge::RequiresLegacyQuiesce(bridge::ActivationDecision::DeclinedGameOwnsItsDevice));
    EXPECT_FALSE(bridge::RequiresLegacyQuiesce(bridge::ActivationDecision::DeclinedNotEnabled));
    EXPECT_FALSE(bridge::RequiresLegacyQuiesce(bridge::ActivationDecision::DeclinedNoRuntimePath));
    EXPECT_FALSE(bridge::RequiresLegacyQuiesce(bridge::ActivationDecision::DeclinedNotAnUpgrade));
}

TEST(StreamlineBridgePolicyTest, LatenessNeverOverridesTheGatesInFrontOfIt) {
    // Being late is not a reason to activate something that was never eligible: the opt-in,
    // the configured runtime and the generation pairing all still have to hold first.
    auto inputs = UpgradeableProcess();
    inputs.gameAlreadyInitializedStreamline = true;
    inputs.upgradeEnabled = false;
    EXPECT_EQ(bridge::DecideActivation(inputs), bridge::ActivationDecision::DeclinedNotEnabled);

    inputs = UpgradeableProcess();
    inputs.gameAlreadyInitializedStreamline = true;
    inputs.runtimePathConfigured = false;
    EXPECT_EQ(bridge::DecideActivation(inputs), bridge::ActivationDecision::DeclinedNoRuntimePath);

    inputs = UpgradeableProcess();
    inputs.gameAlreadyInitializedStreamline = true;
    inputs.processGeneration = Generation::V2;
    EXPECT_EQ(bridge::DecideActivation(inputs), bridge::ActivationDecision::DeclinedNotAnUpgrade);
}

TEST(StreamlineBridgePolicyTest, ADefaultConstructedProcessNeverActivates) {
    EXPECT_FALSE(bridge::ShouldActivate(bridge::ActivationInputs{}));
}

TEST(StreamlineBridgePolicyTest, EveryDecisionExplainsItself) {
    const bridge::ActivationDecision decisions[] = {
        bridge::ActivationDecision::Activate,          bridge::ActivationDecision::ActivateAndQuiesce,
        bridge::ActivationDecision::DeclinedNotEnabled, bridge::ActivationDecision::DeclinedNoRuntimePath,
        bridge::ActivationDecision::DeclinedNotAnUpgrade,
        bridge::ActivationDecision::DeclinedGameOwnsItsDevice};
    std::set<std::string> seen;
    for (auto decision : decisions) {
        const char* text = bridge::Describe(decision);
        ASSERT_NE(text, nullptr);
        EXPECT_GT(std::string(text).size(), 8u);
        // Two decisions that read the same in the log are two decisions nobody can tell
        // apart from a session - which is precisely what made the first refusals unreadable.
        EXPECT_TRUE(seen.insert(text).second) << "duplicate decision text: " << text;
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

TEST(StreamlineBridgePolicyTest, ABridgedAwayOneXModuleGetsNoHooks) {
    // Session `20260821_155250`: CE hooked the game's 1.x interposer - which the bridge had
    // already routed every one of its fifteen import slots away from - and then refused the
    // hook on the 2.x runtime that was actually being called, because its single forward
    // pointer per symbol was already taken:
    //     Refusing to retarget slSetTag from <1.x addr> to <2.x addr> - the installed
    //     target is still mapped
    // CE was left watching a module nothing calls, so dlss_fg_factor, dlss_fg_preset and the
    // overlay's FG state machine saw nothing.
    EXPECT_TRUE(bridge::StreamlineModuleSupersededByBridge(/*bridgeActive=*/true, Generation::V1));

    // The CE-owned 2.x runtime is the one being called, so it must keep its hooks.
    EXPECT_FALSE(bridge::StreamlineModuleSupersededByBridge(true, Generation::V2));
    // An unclassified module is never assumed to be the superseded one - skipping hooks on a
    // module CE could not identify would silently drop coverage.
    EXPECT_FALSE(bridge::StreamlineModuleSupersededByBridge(true, Generation::Unknown));
}

TEST(StreamlineBridgePolicyTest, WithoutABridgeEveryStreamlineModuleKeepsItsHooks) {
    // The overwhelmingly common case is an unbridged 1.x game whose own runtime is the only
    // one there is. Skipping its hooks would break DLSS-G observation in every SL1 title.
    for (Generation generation : {Generation::Unknown, Generation::V1, Generation::V2}) {
        EXPECT_FALSE(bridge::StreamlineModuleSupersededByBridge(/*bridgeActive=*/false, generation))
            << "generation=" << static_cast<int>(generation);
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
