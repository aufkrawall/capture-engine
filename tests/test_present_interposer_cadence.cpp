// Regression tests for present-interposer output cadence
// (hook/common/present_interposer_cadence.h).
//
// Root cause these guard against: NVIDIA Smooth Motion status was inferred from command-list
// work populations and Present gap pairing measured on whatever present stream CE happened to
// process. The July 2026 validation runs only saw a 2x pattern because CE was processing
// NvPresent64's PRIVATE output chain — the same chain whose back buffers CE was compositing
// into, which removed the D3D12 device with DXGI_ERROR_ACCESS_DENIED in Strange Brigade DX12
// (session 20260914_102700). Once CE correctly moved onto the application's chain, that stream
// is 1x by construction and the heuristics can never fire again.
//
// The replacement is structural: the interposer presents its private chain once per frame it
// puts on screen, the application presents its proxy once per rendered frame, and the ratio IS
// the generation factor. Session 20260914_105853 shows exactly two private-chain presents per
// application present.

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "../hook/common/dx12_overlay_policy.h"
#include "../hook/common/fg_runtime_state.h"
#include "../hook/common/present_interposer_cadence.h"
#include "source_fragment_reader.h"

namespace {

using namespace ce::present_interposer;

// The overlay belongs ON the interposer's output chain — below every overlay that patched the dxgi
// entry, so CE draws last and is topmost, and after the driver generated the frame, so CE's overlay
// is not interpolated. What killed Strange Brigade DX12 was the QUEUE, not the chain: CE submitted
// the overlay on the application's queue while drawing into buffers owned by the interposer's
// queue. ffx_routing.h already records that HRESULT (DXGI_ERROR_ACCESS_DENIED, 0x887A002B) as the
// signature of cross-queue backbuffer access, from the late-inject DLSS-G case.
TEST(PresentInterposerQueuePolicyTest, OverlayGoesOnTheChainsOwnQueueOrNowhere) {
    using ce::dx12_overlay_policy::ShouldUsePresentInterposerOutputQueue;
    EXPECT_TRUE(ShouldUsePresentInterposerOutputQueue(true, true));

    // No observed queue: there is no safe route onto this chain, so CE must not draw here at all
    // rather than fall back to the application's queue.
    EXPECT_FALSE(ShouldUsePresentInterposerOutputQueue(true, false));
    // Not an interposer chain: normal routing owns the decision.
    EXPECT_FALSE(ShouldUsePresentInterposerOutputQueue(false, true));
    EXPECT_FALSE(ShouldUsePresentInterposerOutputQueue(false, false));
}

// Bypassing a runtime's own ExecuteCommandLists hook makes it treat CE's overlay as an unexpected
// submission on its queue and remove the device. That is established for native FSR FG; an
// interposer's output queue is the same situation.
TEST(PresentInterposerQueuePolicyTest, OverlayEntersTheQueuesLiveECLChain) {
    using ce::dx12_overlay_policy::ShouldSubmitOverlayThroughHookedECLChain;
    EXPECT_TRUE(ShouldSubmitOverlayThroughHookedECLChain(true, false));
    EXPECT_TRUE(ShouldSubmitOverlayThroughHookedECLChain(false, true));
    EXPECT_TRUE(ShouldSubmitOverlayThroughHookedECLChain(true, true));
    // The game's own queue keeps the raw D3D12 entry, which is what the normal overlay path uses.
    EXPECT_FALSE(ShouldSubmitOverlayThroughHookedECLChain(false, false));
}

TEST(PresentInterposerCadenceTest, TwoOutputPresentsPerApplicationPresentIsSmoothMotion2x) {
    const CadenceVerdict verdict = ClassifyInterposerCadence(120, 240, 1000000);
    EXPECT_TRUE(verdict.generating);
    EXPECT_EQ(verdict.multiplier, 2);
    EXPECT_NEAR(verdict.applicationFps, 120.0f, 0.5f);
    EXPECT_NEAR(verdict.outputFps, 240.0f, 0.5f);
}

// The interposer is in the chain whenever the driver feature is installed. Forwarding one output
// per application frame is Smooth Motion loaded and NOT engaged, and must read as no frame
// generation rather than as a 1x generator.
TEST(PresentInterposerCadenceTest, OneToOneForwardingIsNotFrameGeneration) {
    const CadenceVerdict verdict = ClassifyInterposerCadence(120, 120, 1000000);
    EXPECT_FALSE(verdict.generating);
    EXPECT_EQ(verdict.multiplier, 1);
    // The measured rates are still published: they are what the overlay shows as the base rate.
    EXPECT_NEAR(verdict.applicationFps, 120.0f, 0.5f);
}

// A window that straddles a present burst or a dropped output must not publish a factor.
TEST(PresentInterposerCadenceTest, ShortWindowsProduceNoVerdict) {
    const CadenceVerdict verdict = ClassifyInterposerCadence(kMinApplicationPresentsForVerdict - 1, 200, 1000000);
    EXPECT_FALSE(verdict.generating);
    EXPECT_EQ(verdict.multiplier, 1);
    EXPECT_EQ(verdict.outputFps, 0.0f);
}

TEST(PresentInterposerCadenceTest, RatioRoundsToTheNearestWholeFactorAndIsClamped) {
    // 1.5x is the lowest ratio that counts as generating, and it rounds up to 2x: a generator
    // that drops some of its output is still a generator.
    EXPECT_TRUE(IsInterposerGeneratingFrames(100, 150));
    EXPECT_FALSE(IsInterposerGeneratingFrames(100, 149));
    EXPECT_EQ(ResolveInterposerMultiplier(100, 150), 2);
    EXPECT_EQ(ResolveInterposerMultiplier(100, 240), 2);
    EXPECT_EQ(ResolveInterposerMultiplier(100, 260), 3);
    EXPECT_EQ(ResolveInterposerMultiplier(100, 400), 4);
    // A miscounted window must not publish an absurd factor into the overlay.
    EXPECT_EQ(ResolveInterposerMultiplier(100, 5000), kMaxInterposerMultiplier);
}

TEST(PresentInterposerCadenceTest, TrackerClosesAWindowOnTheApplicationPresentThatCrossesIt) {
    CadenceTracker tracker;
    CadenceVerdict verdict;

    // The first application present only opens the window.
    EXPECT_FALSE(tracker.NoteApplicationPresent(0, &verdict));

    // One application present, two interposer output presents, for a full second.
    for (int i = 0; i < 200; ++i) {
        tracker.NoteOutputPresent();
        tracker.NoteOutputPresent();
        const int64_t nowUs = static_cast<int64_t>(i + 1) * 8333;
        if (tracker.NoteApplicationPresent(nowUs, &verdict)) {
            EXPECT_TRUE(verdict.generating);
            EXPECT_EQ(verdict.multiplier, 2);
            return;
        }
    }
    FAIL() << "a one-second window never closed";
}

TEST(PresentInterposerCadenceTest, ResetDiscardsAPartialWindow) {
    CadenceTracker tracker;
    CadenceVerdict verdict;
    EXPECT_FALSE(tracker.NoteApplicationPresent(0, &verdict));
    for (int i = 0; i < 60; ++i) {
        tracker.NoteOutputPresent();
        tracker.NoteOutputPresent();
        EXPECT_FALSE(tracker.NoteApplicationPresent(static_cast<int64_t>(i + 1) * 8333, &verdict));
    }

    // A retired swapchain leaves counts that no longer describe anything.
    tracker.Reset();
    EXPECT_FALSE(tracker.NoteApplicationPresent(2000000, &verdict));
    // Only presents recorded after the reset may close the next window.
    for (int i = 0; i < kMinApplicationPresentsForVerdict - 2; ++i) {
        EXPECT_FALSE(tracker.NoteApplicationPresent(2000000 + static_cast<int64_t>(i + 1) * 8333, &verdict));
    }
    const bool closed = tracker.NoteApplicationPresent(3100000, &verdict);
    EXPECT_TRUE(closed);
    EXPECT_FALSE(verdict.generating) << "no output presents were recorded after the reset";
}

// Strange Brigade DX12 + NVIDIA Smooth Motion (session 20260914_102700). NvPresent64 hands the
// application a proxy swapchain and keeps its own output chain, on its own command queue, for the
// frames it puts on screen. CE's overlay belongs on that output chain — it is below every overlay
// that patched the dxgi entry, so CE draws last and is topmost, and the frame is already generated,
// so the overlay is not interpolated. What killed the game was the QUEUE: CE submitted the overlay
// on the GAME's queue while drawing into the interposer's buffers, and the first
// ExecuteCommandLists removed the device (GetDeviceRemovedReason == DXGI_ERROR_ACCESS_DENIED,
// 0x887A002B), which ffx_routing.h already records as the signature of cross-queue backbuffer
// access. Present then returned DXGI_ERROR_DEVICE_REMOVED and the game null-dereferenced 1.1 s later.
TEST(PresentInterposerSourceTest, OutputChainIsCompositedOnItsOwnQueue) {
    namespace fs = std::filesystem;
    const std::string create = ce::test_source::ReadFile(
        fs::current_path() / "hook" / "apis" / "dx12_hook_swapchain_create.cpp");
    ASSERT_FALSE(create.empty());
    // Every create site must classify the interposer's private chain before any CE side effect,
    // including the third-party-overlay branch that would otherwise capture its queue.
    for (const char* context : {"CreateSwapChainForHwnd INLINE", "DetourCreateSwapChainGlobal",
                                "DetourCreateSwapChainForHwndGlobal"}) {
        const std::string note =
            std::string("NotePresentInterposerPrivateSwapchainCreate(\"") + context + "\"";
        const size_t noteAt = create.find(note);
        ASSERT_NE(noteAt, std::string::npos) << context;
        const size_t markAt = create.find("MarkThirdPartyOverlaySwapchain(", noteAt);
        ASSERT_NE(markAt, std::string::npos) << context;
        EXPECT_LT(noteAt, markAt) << context;
    }

    // The create must record the queue: that association is the only thing that makes the chain
    // compositable, and its absence is what forces the application-facing fallback.
    const std::string wrapPolicy = ce::test_source::ReadFile(
        fs::current_path() / "hook" / "apis" / "dx12_hook_swapchain_wrap_policy.cpp");
    ASSERT_FALSE(wrapPolicy.empty());
    EXPECT_NE(wrapPolicy.find("DX12_RegisterPresentInterposerPrivateSwapchain(pSwapChain, outputQueue)"),
              std::string::npos);
    EXPECT_NE(wrapPolicy.find("ShouldTreatCreatedSwapchainAsPresentInterposerPrivateChain("), std::string::npos);

    // Both present entries resolve the route for EVERY api, before any per-api branch.
    //
    // This used to assert only that the D3D12 branch passed a queueless chain through. The
    // decision sat inside `if (ctx.api == APIType::D3D12)`, so DX11 composited on an interposer's
    // private chain with no policy at all and NvPresent64 terminated Witcher 3 from its own
    // std::terminate (session 20260919_154534). The chain's ownership is not an api property, so
    // the classification has to happen before the api is branched on.
    for (const char* presentUnit : {"dxgi_shared_present_core.cpp", "dxgi_shared_present1.cpp"}) {
        const std::string present =
            ce::test_source::ReadFile(fs::current_path() / "hook" / "common" / presentUnit);
        ASSERT_FALSE(present.empty()) << presentUnit;
        const size_t guard = present.find("DX12_IsPresentInterposerPrivateSwapchain(pSwapChain)");
        ASSERT_NE(guard, std::string::npos) << presentUnit;
        // The route, not a hand-rolled queue test, decides.
        const size_t route = present.find("ResolvePresentInterposerCompositeRoute(", guard);
        ASSERT_NE(route, std::string::npos)
            << presentUnit << ": the route policy must decide, so every api is covered by one rule";
        EXPECT_NE(present.find("DXGIShared::DX12_GetPresentInterposerOutputQueue(pSwapChain) != nullptr", route),
                  std::string::npos)
            << presentUnit << ": the D3D12 pass-through must still be conditional on having no queue";
        // Before the api branch, so DX11/DX10 cannot miss it.
        const size_t apiBranch = present.find("api == APIType::D3D12");
        ASSERT_NE(apiBranch, std::string::npos) << presentUnit;
        EXPECT_LT(guard, present.rfind("api == APIType::D3D12", present.find("DX12_IsThirdPartyOverlaySwapchain")))
            << presentUnit << ": the interposer route must be resolved before the D3D12-only branch";
        const size_t overlayGuard = present.find("DX12_IsThirdPartyOverlaySwapchain(pSwapChain)");
        ASSERT_NE(overlayGuard, std::string::npos) << presentUnit;
        EXPECT_LT(guard, overlayGuard) << presentUnit;
        // And the per-Present scope has to reach the DX11/DX10 overlay.
        EXPECT_NE(present.find("SetPresentInterposerPrivateOutputChainScope("), std::string::npos) << presentUnit;
    }

    // The DX11 overlay must consult that scope, and must neither retain nor borrow on such a chain.
    const std::string dx11Overlay =
        ce::test_source::ReadFile(fs::current_path() / "hook" / "apis" / "dx11_hook_overlay.cpp");
    ASSERT_FALSE(dx11Overlay.empty());
    EXPECT_NE(dx11Overlay.find("IsPresentOnPresentInterposerPrivateOutputChain()"), std::string::npos);
    EXPECT_NE(dx11Overlay.find("MayAdoptBoundRenderTargetAsOverlayTarget("), std::string::npos)
        << "the bound render target must not be adopted on an interposer's private chain";
    EXPECT_NE(dx11Overlay.find("const bool retainRenderTargetView = !interposerPrivateOutputChain;"),
              std::string::npos)
        << "nothing CE creates on an interposer's private chain may outlive the Present";

    // The overlay queue for that chain is the chain's own queue, never the application's.
    const std::string phase2 = ce::test_source::ReadFile(
        fs::current_path() / "hook" / "apis" / "dx12_hook_process_session_phase2.cpp");
    ASSERT_FALSE(phase2.empty());
    const size_t interposerRoute = phase2.find("ShouldUsePresentInterposerOutputQueue(");
    ASSERT_NE(interposerRoute, std::string::npos);
    EXPECT_NE(phase2.find("gameQueue = interposerOutputQueue;", interposerRoute), std::string::npos);
    // It must outrank the generic routing, which would otherwise pick the application's queue.
    const size_t genericRouting = phase2.find("DecideSwapchainOverlayRouting(");
    ASSERT_NE(genericRouting, std::string::npos);
    EXPECT_LT(interposerRoute, genericRouting);

    // ...and it enters that queue's live ECL chain rather than the raw D3D12 entry.
    const std::string drawTail = ce::test_source::ReadFile(
        fs::current_path() / "hook" / "apis" / "dx12_hook_process_session_draw_tail.cpp");
    ASSERT_FALSE(drawTail.empty());
    EXPECT_NE(drawTail.find("ShouldSubmitOverlayThroughHookedECLChain("), std::string::npos);

    // The wrapper stays a pure pass-through while the detour composites on the output chain, so the
    // frame is composited exactly once and never on the application's interpolated copy.
    const std::string wrapInternal = ce::test_source::ReadFile(
        fs::current_path() / "hook" / "wrappers" / "dxgi_swapchain_wrap_internal.h");
    ASSERT_FALSE(wrapInternal.empty());
    const size_t delegateForInterposer = wrapInternal.find("HasCompositablePresentInterposerOutputChain()");
    ASSERT_NE(delegateForInterposer, std::string::npos);
    const size_t invisibleRefusal = wrapInternal.find("presentInvisibleToDetourHook) {", delegateForInterposer);
    ASSERT_NE(invisibleRefusal, std::string::npos)
        << "the no-queue refusal must come after the compositable-chain delegation";

    // The object handed to the application is never a private chain, whatever the create looked
    // like — an interposer that merely forwards the app's create must not cost CE its overlay.
    const std::string factory = ce::test_source::ReadFile(
        fs::current_path() / "hook" / "wrappers" / "dxgi_factory_wrap.cpp");
    ASSERT_FALSE(factory.empty());
    const size_t assign = factory.find("void AssignCreatedSwapchain(");
    ASSERT_NE(assign, std::string::npos);
    const size_t unregister =
        factory.find("DX12_UnregisterPresentInterposerPrivateSwapchain(", assign);
    const size_t preserve =
        factory.find("ShouldPreserveDX12SwapchainIdentityBelowForeignPresentChain(", assign);
    ASSERT_NE(unregister, std::string::npos);
    ASSERT_NE(preserve, std::string::npos);
    EXPECT_LT(unregister, preserve);
    // ...and the identity preservation must be conditioned on the deep body hook actually covering
    // that object's Present.
    EXPECT_NE(factory.find("IsSwapchainPresentCoveredByDeepBodyHook(", assign), std::string::npos);
}

// Forced FIFO under a present interposer. The 2026-09-13 Portal RTX result is the precedent and it
// cuts the other way from the obvious reading: what unpaced a generated group was forcing the
// present MODE above the generator, not stating the interval on its final flip. CE touches nothing
// above NvPresent64 — the pair arrives at DXGI already spread by the driver's own metering, which is
// exactly what its SyncInterval=0 + ALLOW_TEARING output present IS — so quantizing that flip onto
// vertical blanks is vertical-blank synchronization.
TEST(PresentInterposerSourceTest, ForcedFifoIsStatedOnTheInterposersOwnFlip) {
    namespace fs = std::filesystem;
    const std::string pacing = ce::test_source::ReadFile(
        fs::current_path() / "hook" / "common" / "dxgi_shared_present_pacing.cpp");
    ASSERT_FALSE(pacing.empty());
    const size_t interposerBranch = pacing.find("DX12_IsPresentInterposerPrivateSwapchain(pSwapChain)");
    ASSERT_NE(interposerBranch, std::string::npos);

    // The final-flip contract is the shared one, not a second spelling of it.
    EXPECT_NE(pacing.find("ApplyFinalDxgiFifoParameters(", interposerBranch), std::string::npos);
    // ...and the generic input-side override must not also run for that present.
    const size_t genericOverride = pacing.find("ProcessVSyncOverride(syncInterval, flags);");
    ASSERT_NE(genericOverride, std::string::npos);
    EXPECT_LT(interposerBranch, genericOverride);
    EXPECT_NE(pacing.find("return;", interposerBranch), std::string::npos);

    // Only a vertical-blank request applies: off/mailbox are not one, and the interposer's own
    // parameters already are those.
    EXPECT_NE(pacing.find("useMailbox", interposerBranch), std::string::npos);

    // The swapchain has to reach the decision at all.
    const std::string header = ce::test_source::ReadFile(
        fs::current_path() / "hook" / "common" / "dxgi_shared.h");
    ASSERT_FALSE(header.empty());
    EXPECT_NE(header.find("ProcessPresentVSyncOverride(UINT& syncInterval, UINT& flags, IDXGISwapChain* pSwapChain"),
              std::string::npos);
}


// Witcher 3 DX11 + Smooth Motion, session 20260919_160555: 6962 presents in 40.8 s, in 3231 groups
// of exactly two. Every one of them counted as an application frame, because
// RecordPresentForNvidiaSmoothMotion recorded a constant 1 - DX11 has no command-list population
// to classify by - so the overlay reported the interposer's OUTPUT rate (~171) as the game's frame
// rate (~85). The application's own immediate context supplies the missing half.
TEST(PresentInterposerSourceClassificationTest, ApplicationWorkSeparatesTheTwoStreams) {
    using ce::fg_runtime::IsApplicationSourcedInterposerPresent;

    // A generated frame is produced entirely inside the interposer: the game submitted nothing
    // across it.
    EXPECT_FALSE(IsApplicationSourcedInterposerPresent(0));
    // One submission is enough. There is no threshold to tune and no timing involved, so a very
    // light application frame is still an application frame.
    EXPECT_TRUE(IsApplicationSourcedInterposerPresent(1));
    EXPECT_TRUE(IsApplicationSourcedInterposerPresent(4231));
}

// The two streams must produce the same ratio DX12 measures: the interposer's output chain carries
// BOTH the forwarded real frame and the generated one, so output/application is the factor.
TEST(PresentInterposerSourceClassificationTest, TheMeasuredRatioIsTheGenerationFactor) {
    namespace pi = ce::present_interposer;

    // One second of the measured session: ~85 application presents, ~171 output presents.
    const pi::CadenceVerdict verdict = pi::ClassifyInterposerCadence(85, 171, 1000000);
    EXPECT_TRUE(verdict.generating);
    EXPECT_EQ(verdict.multiplier, 2);
    EXPECT_NEAR(verdict.applicationFps, 85.0f, 0.5f);
    EXPECT_NEAR(verdict.outputFps, 171.0f, 0.5f);

    // Smooth Motion loaded but not engaged: one output per application frame is NOT a 1x
    // generator, and the base rate must not be halved.
    const pi::CadenceVerdict idle = pi::ClassifyInterposerCadence(85, 85, 1000000);
    EXPECT_FALSE(idle.generating);
    EXPECT_EQ(idle.multiplier, 1);
}

// Source-policy: the DX11 present path must actually feed both streams, or the tracker never
// closes a window and the getters silently fall back to the unclassified rate.
TEST(PresentInterposerSourceTest, DX11FeedsBothCadenceStreams) {
    namespace fs = std::filesystem;
    const std::string dx11Device =
        ce::test_source::ReadFile(fs::current_path() / "hook" / "apis" / "dx11_hook_device.cpp");
    ASSERT_FALSE(dx11Device.empty());
    EXPECT_NE(dx11Device.find("NoteApplicationPresentUnderPresentInterposer()"), std::string::npos)
        << "the application stream has to be fed, not just the output stream";
    EXPECT_NE(dx11Device.find("IsPresentOnPresentInterposerPrivateOutputChain()"), std::string::npos);

    // The classifier's evidence comes from the game's own context, and counting must stay off
    // until an interposer exists so the ordinary draw path is untouched.
    const std::string fgDetection =
        ce::test_source::ReadFile(fs::current_path() / "hook" / "common" / "fg_detection.cpp");
    ASSERT_FALSE(fgDetection.empty());
    EXPECT_NE(fgDetection.find("IsApplicationSourcedInterposerPresent("), std::string::npos);
    // The classifier must gate the constant-1 record, not sit beside it: an unconditional
    // RecordFrame(1) is exactly what reported the interposer's output rate as the game's fps.
    const size_t classifier = fgDetection.find("IsApplicationSourcedInterposerPresent(");
    const size_t neutralRecord = fgDetection.find("RecordFrame(1);");
    ASSERT_NE(neutralRecord, std::string::npos);
    EXPECT_NE(fgDetection.find("IsApplicationSubmissionCountingEnabled()"), std::string::npos)
        << "the neutral constant-1 sample must be reachable only while no interposer is registered";
    EXPECT_LT(neutralRecord, classifier)
        << "the neutral sample is the early-out; the classified record is what follows it";

    const std::string tracking =
        ce::test_source::ReadFile(fs::current_path() / "hook" / "common" / "present_interposer_tracking.cpp");
    ASSERT_FALSE(tracking.empty());
    EXPECT_NE(tracking.find("SetApplicationSubmissionCountingEnabled(true)"), std::string::npos);
}

}  // namespace
