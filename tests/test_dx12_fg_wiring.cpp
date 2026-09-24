#include <gtest/gtest.h>

#include <windows.h>

#include <cstdint>
#include <string>

#include "../hook/common/dx12_overlay_policy.h"
#include "../hook/common/streamline_runtime_policy.h"

#include "source_fragment_reader.h"

namespace {

std::string ReadSource(const std::filesystem::path& relativePath) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / relativePath);
}

// Session 20260923_233317 (Talos FSR FG -> DLSS FG): the fresh-handoff exemption escaped the
// inactive-runtime-owned init defer but the NEXT gate in the same Phase3 block keyed its defer on
// command tracking the handoff had not populated yet (and the SL-off grace it keys on is re-seeded
// by the same transition's late outer observer), so the overlay still stayed gone. Both gates must
// accept the same exemption.
TEST(Dx12FgWiring, FreshHandoffExemptionIsAcceptedByBothInitDeferralGates) {
    using ce::dx12_overlay_policy::ShouldDeferInactiveRuntimeOwnedSwapchainOverlayInit;
    using ce::dx12_overlay_policy::ShouldDeferOverlayInitUntilCommandQueueSettlesAfterRecentStreamlineTeardown;

    // The recurrence state: FG inactive, runtime owns the fresh handoff swapchain, recent Streamline
    // teardown grace active, command tracking empty.
    EXPECT_FALSE(
        ShouldDeferInactiveRuntimeOwnedSwapchainOverlayInit(false, false, true, true, false, false, false, true));
    EXPECT_FALSE(ShouldDeferOverlayInitUntilCommandQueueSettlesAfterRecentStreamlineTeardown(
        false, false, true, true, true, true, false, false, false, false,
        /*freshStreamlineHandoffOnSubmittableQueue=*/true));
    // Without the exemption both gates keep their departing-runtime settle guards.
    EXPECT_TRUE(
        ShouldDeferInactiveRuntimeOwnedSwapchainOverlayInit(false, false, true, true, false, false, false, false));
    EXPECT_TRUE(ShouldDeferOverlayInitUntilCommandQueueSettlesAfterRecentStreamlineTeardown(
        false, false, true, true, true, true, false, false, false, false,
        /*freshStreamlineHandoffOnSubmittableQueue=*/false));
}

// Source invariant: the two Phase3 init-deferral gates carry the SAME fresh-handoff exemption, the
// call site hands both the same variable, and the handoff's "submittable queue" proof is this
// swapchain's own tracked submit path — the global realECL pointer proves nothing about it and a
// handoff queue CE has never submitted on must not qualify.
TEST(Dx12FgWiring, BothInitDeferralGatesAcceptTheSameFreshHandoffExemption) {
    const std::string ownership = ReadSource("hook/common/dx12_overlay_policy/streamline_ownership.h");
    ASSERT_FALSE(ownership.empty());
    const size_t gateA = ownership.find("inline bool ShouldDeferInactiveRuntimeOwnedSwapchainOverlayInit(");
    const size_t gateB =
        ownership.find("inline bool ShouldDeferOverlayInitUntilCommandQueueSettlesAfterRecentStreamlineTeardown(");
    const size_t gateBEnd =
        ownership.find("inline bool ShouldIgnoreCommandQueueRegistrationAfterRecentStreamlineTeardown(", gateB);
    ASSERT_NE(gateA, std::string::npos);
    ASSERT_NE(gateB, std::string::npos);
    ASSERT_NE(gateBEnd, std::string::npos);
    ASSERT_LT(gateA, gateB);
    EXPECT_NE(ownership.substr(gateA, gateB - gateA).find("if (freshStreamlineHandoffOnSubmittableQueue)"),
              std::string::npos)
        << "the inactive-runtime-owned init defer must accept the fresh-handoff exemption";
    EXPECT_NE(ownership.substr(gateB, gateBEnd - gateB).find("if (freshStreamlineHandoffOnSubmittableQueue)"),
              std::string::npos)
        << "the queue-settle defer must accept the same fresh-handoff exemption";

    const std::string phase3 = ReadSource("hook/apis/dx12_hook_process_session_phase3.cpp");
    ASSERT_FALSE(phase3.empty());
    EXPECT_NE(phase3.find("deferInactiveRuntimeOwnedInit(freshStreamlineHandoffOnSubmittableQueue)"),
              std::string::npos);
    const size_t gateBCall = phase3.find("ShouldDeferOverlayInitUntilCommandQueueSettlesAfterRecentStreamlineTeardown(");
    ASSERT_NE(gateBCall, std::string::npos);
    EXPECT_NE(phase3.find("commandQueueMatchesPrimaryGameQueue, freshStreamlineHandoffOnSubmittableQueue))", gateBCall),
              std::string::npos)
        << "the queue-settle call site must pass the same exemption variable as gate A";

    const size_t evidence = phase3.find("swapchainQueueSubmittable = currentSwapchainQueue != nullptr &&");
    ASSERT_NE(evidence, std::string::npos);
    const size_t evidenceEnd = phase3.find(';', evidence);
    ASSERT_NE(evidenceEnd, std::string::npos);
    const std::string evidenceStmt = phase3.substr(evidence, evidenceEnd - evidence);
    EXPECT_NE(evidenceStmt.find("HasTrackedExecuteCommandListsOriginal(currentSwapchainQueue)"), std::string::npos);
    EXPECT_EQ(evidenceStmt.find("RealD3D12ECL"), std::string::npos)
        << "the global realECL pointer must not qualify THIS swapchain's queue as submittable";
}

// Source invariant (drift-prevention for the triplicated phase5 exception lists): the outer SL-FG-OFF
// cooldown, GPU-drain and reinit gates must all reference the one shared keep-live predicate, and that
// predicate must contain every keep-live exception. The DLSS->FSR no-callback takeover used to be
// listed at the cooldown and reinit gates but missing from the drain gate, so every such switch ran a
// blocking WaitForSingleObject(drainEvent, 200) on the Present thread for an overlay state kept live.
TEST(Dx12FgWiring, OuterSLFGOffKeepLiveExceptionsAreSharedByAllThreeGates) {
    const std::string phase5 = ReadSource("hook/apis/dx12_hook_process_session_phase5.cpp");
    ASSERT_FALSE(phase5.empty());

    const size_t sharedDef = phase5.find("const bool keepOverlayLiveAcrossOuterOff =");
    ASSERT_NE(sharedDef, std::string::npos);
    const size_t sharedDefEnd = phase5.find(';', sharedDef);
    ASSERT_NE(sharedDefEnd, std::string::npos);
    const std::string sharedBody = phase5.substr(sharedDef, sharedDefEnd - sharedDef);
    EXPECT_NE(sharedBody.find("keepOverlayLiveAcrossDLSSToFSRNoCallbackTakeover"), std::string::npos);
    EXPECT_NE(sharedBody.find("keepOverlayLiveAcrossAuthoritativeDLSSOffNormalReturn"), std::string::npos);
    EXPECT_NE(sharedBody.find("keepOverlayLiveAcrossNativeFSRGameSwapchainRecovery"), std::string::npos);
    EXPECT_NE(sharedBody.find("keepOverlayLiveAcrossPrewarmedPostSLHandoffPreserve"), std::string::npos);

    const size_t drainComment = phase5.find("// Drain in-flight GPU work");
    ASSERT_NE(drainComment, std::string::npos);
    const size_t drainGate = phase5.find("if (dx12_hook_g_State.fence &&", drainComment);
    ASSERT_NE(drainGate, std::string::npos);
    const size_t drainGateEnd = phase5.find('{', drainGate);
    ASSERT_NE(drainGateEnd, std::string::npos);
    const std::string drainCondition = phase5.substr(drainGate, drainGateEnd - drainGate);
    EXPECT_NE(drainCondition.find("!keepOverlayLiveAcrossOuterOff"), std::string::npos)
        << "the GPU-drain gate must honor the shared keep-live set";
    EXPECT_NE(drainCondition.find("!preserveConfirmedPostSLProxyResourcesAcrossOuterOff"), std::string::npos);

    const size_t drainWait = phase5.find("WaitForSingleObject(drainEvent, 200)", drainComment);
    ASSERT_NE(drainWait, std::string::npos);
    EXPECT_LT(drainGate, drainWait) << "the blocking drain must sit behind the keep-live gate";

    const size_t reinitGate = phase5.find("if (dx12_hook_g_State.overlayInit &&");
    ASSERT_NE(reinitGate, std::string::npos);
    const size_t reinitGateEnd = phase5.find('{', reinitGate);
    ASSERT_NE(reinitGateEnd, std::string::npos);
    const std::string reinitCondition = phase5.substr(reinitGate, reinitGateEnd - reinitGate);
    EXPECT_NE(reinitCondition.find("!keepOverlayLiveAcrossOuterOff"), std::string::npos)
        << "the reinit gate must honor the shared keep-live set";

    const size_t cooldownGate =
        phase5.find("bypassPureStreamlineOffCooldown || bypassConfirmedPostSLSuspensionCooldown ||");
    ASSERT_NE(cooldownGate, std::string::npos);
    const size_t cooldownGateEnd = phase5.find('{', cooldownGate);
    ASSERT_NE(cooldownGateEnd, std::string::npos);
    EXPECT_NE(phase5.substr(cooldownGate, cooldownGateEnd - cooldownGate).find("keepOverlayLiveAcrossOuterOff"),
              std::string::npos)
        << "the cooldown gate must honor the shared keep-live set";
}

// The persistent GetState-only reactivation block must stay armed against an ON-but-not-interpolating
// runtime: a frozen DLSSGState fence/presented count (session 20260702_094955: optionsMode=on,
// presented==1, no fps gain) is exactly the case where suppressing the activation is correct.
TEST(Dx12FgWiring, GetStateOnlyBlockKeepsSuppressingWhenGenerationEvidenceIsFrozen) {
    using ce::streamline_runtime_policy::IsDLSSGGenerationEvidenceAdvancing;
    using ce::streamline_runtime_policy::ShouldRetireGetStateOnlyReactivationBlockForSustainedGeneration;
    using ce::streamline_runtime_policy::UpdateGetStateOnlyBlockGenerationEvidenceStreak;

    // Frozen fence value and frozen presented count: not advancing.
    EXPECT_FALSE(IsDLSSGGenerationEvidenceAdvancing(true, 1200, 1200, 1, 1));
    // First sample only seeds the tracker; there is nothing to compare against yet.
    EXPECT_FALSE(IsDLSSGGenerationEvidenceAdvancing(false, 0, 1200, 0, 2));

    uint32_t streak = 0;
    streak = UpdateGetStateOnlyBlockGenerationEvidenceStreak(
        IsDLSSGGenerationEvidenceAdvancing(true, 1200, 1200, 1, 1), streak);
    EXPECT_EQ(streak, 0u);
    // A frozen sample resets accumulated progress: the retire must reflect current generation.
    streak = UpdateGetStateOnlyBlockGenerationEvidenceStreak(true, 7);
    EXPECT_EQ(streak, 8u);
    streak = UpdateGetStateOnlyBlockGenerationEvidenceStreak(false, streak);
    EXPECT_EQ(streak, 0u);

    // Frozen evidence never retires the block.
    EXPECT_FALSE(ShouldRetireGetStateOnlyReactivationBlockForSustainedGeneration(
        /*blockArmed=*/true, /*callSucceeded=*/true, /*optionsRequestActive=*/true, /*hasRuntimeFenceEvidence=*/true,
        /*advancingSampleStreak=*/0, /*requiredAdvancingSamples=*/3));
    EXPECT_FALSE(ShouldRetireGetStateOnlyReactivationBlockForSustainedGeneration(true, true, true, true, 2, 3));
}

// A game that re-enables DLSS-G through GetState-visible state only (options passed to
// slDLSSGGetState, no explicit slDLSSGSetOptions enable) generates real frames while the persistent
// block keeps FG status lying and the limiter at base cadence. Sustained advancing generation
// evidence must retire the block — bounded like every other latch in this machine.
TEST(Dx12FgWiring, GetStateOnlyBlockRetiresOnSustainedAdvancingGenerationEvidence) {
    using ce::streamline_runtime_policy::IsDLSSGGenerationEvidenceAdvancing;
    using ce::streamline_runtime_policy::ShouldRetireGetStateOnlyReactivationBlockForSustainedGeneration;
    using ce::streamline_runtime_policy::UpdateGetStateOnlyBlockGenerationEvidenceStreak;

    // Advancing fence value, or advancing presented count, is real generation evidence.
    EXPECT_TRUE(IsDLSSGGenerationEvidenceAdvancing(true, 1200, 1201, 1, 1));
    EXPECT_TRUE(IsDLSSGGenerationEvidenceAdvancing(true, 1200, 1200, 1, 2));

    uint32_t streak = 0;
    for (int sample = 0; sample < 3; ++sample) {
        streak = UpdateGetStateOnlyBlockGenerationEvidenceStreak(
            IsDLSSGGenerationEvidenceAdvancing(true, 1200 + sample, 1201 + sample, 1, 1), streak);
        const bool retire = ShouldRetireGetStateOnlyReactivationBlockForSustainedGeneration(
            /*blockArmed=*/true, /*callSucceeded=*/true, /*optionsRequestActive=*/true,
            /*hasRuntimeFenceEvidence=*/true, streak, /*requiredAdvancingSamples=*/3);
        EXPECT_EQ(retire, sample == 2) << "retire only after the sustained streak, sample=" << sample;
    }

    // One advancing sample alone is a blip, not sustained generation.
    EXPECT_FALSE(ShouldRetireGetStateOnlyReactivationBlockForSustainedGeneration(true, true, true, true, 1, 3));
    // Preconditions: a disarmed block, a failed call, no ON request, or no runtime fence evidence
    // leaves the block armed.
    EXPECT_FALSE(ShouldRetireGetStateOnlyReactivationBlockForSustainedGeneration(
        /*blockArmed=*/false, true, true, true, 3, 3));
    EXPECT_FALSE(ShouldRetireGetStateOnlyReactivationBlockForSustainedGeneration(
        true, /*callSucceeded=*/false, true, true, 3, 3));
    EXPECT_FALSE(ShouldRetireGetStateOnlyReactivationBlockForSustainedGeneration(
        true, true, /*optionsRequestActive=*/false, true, 3, 3));
    EXPECT_FALSE(ShouldRetireGetStateOnlyReactivationBlockForSustainedGeneration(
        true, true, true, /*hasRuntimeFenceEvidence=*/false, 3, 3));
}

// Source invariant: the GetState hook must retire the persistent suppression BEFORE evaluating it,
// so a sustained-generation retire takes effect on the same poll instead of one frame late.
TEST(Dx12FgWiring, GetStateHookRetiresTheSuppressionBeforeEvaluatingIt) {
    const std::string dlssg = ReadSource("hook/apis/streamline_hook_dlssg.cpp");
    ASSERT_FALSE(dlssg.empty());
    const size_t retire = dlssg.find("MaybeRetireGetStateOnlyReactivationBlockForSustainedGeneration(");
    const size_t evaluate = dlssg.find("ShouldSuppressNewGetStateActivation();");
    ASSERT_NE(retire, std::string::npos);
    ASSERT_NE(evaluate, std::string::npos);
    EXPECT_LT(retire, evaluate);
}

// Deferred-signal flush failure accounting: currentFenceValue is what every later overlay wait
// blocks on. A failed Signal never reaches the fence, so its value must never be committed — that
// would leave the accounting permanently ahead of the fence and every wait would run to its
// liveness timeout. Stale/out-of-order values must not rewind it either.
TEST(Dx12FgWiring, DeferredFenceSignalAccountingNeverCommitsAValueTheFenceCannotReach) {
    using ce::dx12_overlay_policy::ShouldCommitDeferredOverlayFenceSignalValue;

    EXPECT_FALSE(ShouldCommitDeferredOverlayFenceSignalValue(/*signalSucceeded=*/false, 5, 4));
    EXPECT_FALSE(ShouldCommitDeferredOverlayFenceSignalValue(false, 5, 0));
    EXPECT_TRUE(ShouldCommitDeferredOverlayFenceSignalValue(/*signalSucceeded=*/true, 5, 4));
    EXPECT_FALSE(ShouldCommitDeferredOverlayFenceSignalValue(true, 4, 4));
    EXPECT_FALSE(ShouldCommitDeferredOverlayFenceSignalValue(true, 3, 4));
}

// Source invariant for the flush's fence ABA/use-after-free class: it must run under the overlay
// lock that owns the fence lifetime, pin the fence with an owning reference across the Signal call,
// and commit the accounting through the failure-accounting policy — never through the raw
// dx12_hook_g_State.fence pointer again.
TEST(Dx12FgWiring, DeferredSignalFlushPinsTheFenceUnderTheOverlayLock) {
    const std::string flushUnit = ReadSource("hook/apis/dx12_hook_streamline_fg_transition.cpp");
    ASSERT_FALSE(flushUnit.empty());
    const size_t flushFn = flushUnit.find("bool DX12_FlushDeferredSignalWithInfo(");
    ASSERT_NE(flushFn, std::string::npos);
    const size_t flushEnd = flushUnit.find("void DX12_FlushDeferredSignal()", flushFn);
    ASSERT_NE(flushEnd, std::string::npos);
    const std::string flushBody = flushUnit.substr(flushFn, flushEnd - flushFn);

    EXPECT_NE(flushBody.find("std::lock_guard<std::recursive_mutex> overlayLock(dx12_hook_g_OverlayMutex);"),
              std::string::npos)
        << "the flush must serialize against InitOverlaySync's fence release/recreate";
    EXPECT_NE(flushBody.find("pinnedFence = dx12_hook_g_State.fence;"), std::string::npos);
    EXPECT_NE(flushBody.find("q->Signal(pinnedFence.Get(), deferredVal)"), std::string::npos);
    EXPECT_NE(flushBody.find("ShouldCommitDeferredOverlayFenceSignalValue("), std::string::npos);
    EXPECT_EQ(flushBody.find("q->Signal(dx12_hook_g_State.fence"), std::string::npos)
        << "signaling the raw state pointer reintroduces the ABA/use-after-free race";
}

// NVIDIA Smooth Motion's private output chain has never run on hardware (llm-wiki/present-interposers.md);
// the founding bug of the class was a first overlay submit on the application's queue:
// device removal 0x887A002B followed by the game's null-deref crash.
TEST(Dx12FgWiring, PresentInterposerPrivateChainWithoutSafeQueueFailsClosed) {
    using ce::dx12_overlay_policy::ShouldPassThroughPresentInterposerPrivateChainWithoutOverlayDraw;
    using ce::dx12_overlay_policy::ShouldUsePresentInterposerOutputQueue;

    // No recorded create queue: no safe submit queue exists at all — pass through untouched.
    EXPECT_TRUE(ShouldPassThroughPresentInterposerPrivateChainWithoutOverlayDraw(
        /*swapchainIsInterposerOutputChain=*/true, /*hasInterposerOutputQueue=*/false));
    // A recorded queue draws on that queue; ordinary chains never take the fallback.
    EXPECT_FALSE(ShouldPassThroughPresentInterposerPrivateChainWithoutOverlayDraw(true, true));
    EXPECT_FALSE(ShouldPassThroughPresentInterposerPrivateChainWithoutOverlayDraw(false, false));
    EXPECT_FALSE(ShouldPassThroughPresentInterposerPrivateChainWithoutOverlayDraw(false, true));

    // The two rules are exactly complementary on the private-chain domain: every private chain is
    // either drawn on its own queue or passed through — never dropped into generic queue routing.
    EXPECT_TRUE(ShouldUsePresentInterposerOutputQueue(true, true));
    EXPECT_FALSE(ShouldUsePresentInterposerOutputQueue(true, false));
    EXPECT_NE(ShouldPassThroughPresentInterposerPrivateChainWithoutOverlayDraw(true, true),
              ShouldUsePresentInterposerOutputQueue(true, true));
    EXPECT_NE(ShouldPassThroughPresentInterposerPrivateChainWithoutOverlayDraw(true, false),
              ShouldUsePresentInterposerOutputQueue(true, false));
}

// Source invariant: the fail-closed guard must run before generic queue routing and skip the draw
// outright, or the first submit on the private chain's backbuffers from a foreign queue returns.
TEST(Dx12FgWiring, PresentInterposerFailClosedGuardRunsBeforeGenericQueueRouting) {
    const std::string phase2 = ReadSource("hook/apis/dx12_hook_process_session_phase2.cpp");
    ASSERT_FALSE(phase2.empty());
    const size_t failClosed = phase2.find("ShouldPassThroughPresentInterposerPrivateChainWithoutOverlayDraw(");
    const size_t routing = phase2.find("DecideSwapchainOverlayRouting(");
    ASSERT_NE(failClosed, std::string::npos);
    ASSERT_NE(routing, std::string::npos);
    EXPECT_LT(failClosed, routing);

    const size_t branchEnd = phase2.find("} else if (protectedOfficialFFXStartupOverlayOnly)", failClosed);
    ASSERT_NE(branchEnd, std::string::npos);
    EXPECT_NE(phase2.substr(failClosed, branchEnd - failClosed).find("return ProcessFrameFlow::kReturn;"),
              std::string::npos)
        << "the fail-closed path must skip the overlay draw, not fall through to queue routing";
}

}  // namespace
