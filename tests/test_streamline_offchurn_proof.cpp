#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>

#include "../hook/common/streamline_runtime_policy.h"
#include "source_fragment_reader.h"

namespace {

using ce::streamline_runtime_policy::GetStartupProtectedOffChurnActiveProof;
using ce::streamline_runtime_policy::GetStartupProtectedOffChurnActiveProofUpdateThreshold;
using ce::streamline_runtime_policy::HasStartupProtectedOffChurnActiveProof;
using ce::streamline_runtime_policy::ShouldCountTitleFrameAsStartupProtectedActiveProof;
using ce::streamline_runtime_policy::ShouldKeepStartupProtectedOffChurnDeferredUntilActiveProof;

// Either clock alone satisfies the proof; they never add up (a GetState-polling title advances both
// once per frame, so summing would halve its protection).
TEST(StreamlineOffChurnProofTest, ActiveProofIsTheFurtherOfTheTwoClocks) {
    EXPECT_EQ(GetStartupProtectedOffChurnActiveProof(0, 0), 0u);
    EXPECT_EQ(GetStartupProtectedOffChurnActiveProof(1, 0), 1u);
    EXPECT_EQ(GetStartupProtectedOffChurnActiveProof(1, 3), 3u);
    EXPECT_EQ(GetStartupProtectedOffChurnActiveProof(3, 1), 3u);
    EXPECT_EQ(GetStartupProtectedOffChurnActiveProof(2, 2), 2u);
    EXPECT_FALSE(HasStartupProtectedOffChurnActiveProof(GetStartupProtectedOffChurnActiveProof(2, 2)));
}

TEST(StreamlineOffChurnProofTest, TitleFrameCountsOnlyWhileGeneratedThroughAndOncePerFrame) {
    EXPECT_TRUE(ShouldCountTitleFrameAsStartupProtectedActiveProof(true, true, true, true));
    // Nothing pending -> nothing to prove.
    EXPECT_FALSE(ShouldCountTitleFrameAsStartupProtectedActiveProof(false, true, true, true));
    // A re-marked present reuses its frame token.
    EXPECT_FALSE(ShouldCountTitleFrameAsStartupProtectedActiveProof(true, /*newTitleFrame=*/false, true, true));
    // FG not running, or PostSL not confirmed: the frame is no evidence that FG runs stably.
    EXPECT_FALSE(ShouldCountTitleFrameAsStartupProtectedActiveProof(true, true, /*streamlineFGRunning=*/false, true));
    EXPECT_FALSE(
        ShouldCountTitleFrameAsStartupProtectedActiveProof(true, true, true, /*postSLConfirmedRendering=*/false));
}

// Witcher 3 through the 2.x bridge, session 20261002_051703: the title calls SetOptions only on its
// ON/OFF edges and never polls GetState. An OFF 0.4 s after an ON was suppressed as startup churn;
// the next ON gave 1 of 3 active updates, and every later genuine OFF (the game menu) was suppressed
// and re-armed the latch, so DLSS-G stayed on in menus for the rest of the session.
TEST(StreamlineOffChurnProofTest, EdgeOnlyTitleReachesProofThroughItsFrames) {
    const uint32_t threshold = GetStartupProtectedOffChurnActiveProofUpdateThreshold();
    const bool churnObserved = true;
    const bool comebackProof = true;  // explicit SetOptions(ON) for this comeback
    const bool confirmed = true;
    const bool settling = false;

    // Before: one active update per comeback, no other clock.
    const uint32_t updatesOnly = GetStartupProtectedOffChurnActiveProof(1, 0);
    EXPECT_TRUE(ShouldKeepStartupProtectedOffChurnDeferredUntilActiveProof(churnObserved, updatesOnly, comebackProof,
                                                                          confirmed, settling));

    // Now: the title's frames while DLSS-G generates satisfy the proof within a few frames.
    uint32_t frames = 0;
    for (uint32_t i = 0; i < threshold; ++i) {
        if (ShouldCountTitleFrameAsStartupProtectedActiveProof(churnObserved, true, true, confirmed)) {
            ++frames;
        }
    }
    const uint32_t proof = GetStartupProtectedOffChurnActiveProof(1, frames);
    EXPECT_TRUE(HasStartupProtectedOffChurnActiveProof(proof));
    EXPECT_FALSE(ShouldKeepStartupProtectedOffChurnDeferredUntilActiveProof(churnObserved, proof, comebackProof,
                                                                           confirmed, settling));
}

// A GetState-polling title (the GTA startup-churn family) keeps the same pace: one stale OFF per frame
// resets both clocks, so neither reaches the threshold while the churn lasts.
TEST(StreamlineOffChurnProofTest, PerFrameChurnStillNeverReachesProof) {
    uint32_t updates = 0;
    uint32_t frames = 0;
    for (int frame = 0; frame < 100; ++frame) {
        ++updates;  // GetState(active)
        ++frames;   // present-start marker
        if (frame % 2 == 1) {
            updates = 0;  // stale GetState(OFF) -> MarkStartupProtectedOffChurnObserved
            frames = 0;
        }
        EXPECT_FALSE(HasStartupProtectedOffChurnActiveProof(GetStartupProtectedOffChurnActiveProof(updates, frames)));
    }
}

TEST(StreamlineOffChurnProofSourceTest, PresentStartMarkerFeedsTheProofAndEveryResetClearsBothClocks) {
    namespace fs = std::filesystem;
    const fs::path pcl = fs::current_path() / "hook" / "apis" / "streamline_hook_pcl.cpp";
    const fs::path startup = fs::current_path() / "hook" / "apis" / "streamline_hook_startup.cpp";
    ASSERT_TRUE(fs::exists(pcl));
    ASSERT_TRUE(fs::exists(startup));
    const std::string pclText = ce::test_source::ReadLogicalSource(pcl);
    const std::string startupText = ce::test_source::ReadLogicalSource(startup);

    const size_t hook = pclText.find("sl::Result Hooked_slPCLSetMarker(");
    ASSERT_NE(hook, std::string::npos);
    const size_t forward = pclText.find("original(marker, frame)", hook);
    const size_t presentStart = pclText.find("kPresentStartMarker &&", forward);
    const size_t ceIssued = pclText.find("!StreamlineHook::CeIssuedFrameMarkerScope::Active()", presentStart);
    const size_t sequence = pclText.find("streamline_hook_g_TitleFrameMarkerSequence.fetch_add(", presentStart);
    const size_t note = pclText.find("NoteStartupProtectedActiveTitleFrame(frameId)", forward);
    const size_t replay = pclText.find("StreamlineHook::ServiceHeldSetOptionsOffOnTitleFrame()", forward);
    ASSERT_NE(forward, std::string::npos);
    ASSERT_NE(presentStart, std::string::npos);
    ASSERT_NE(ceIssued, std::string::npos);
    ASSERT_NE(sequence, std::string::npos);
    ASSERT_NE(note, std::string::npos);
    ASSERT_NE(replay, std::string::npos);
    EXPECT_LT(presentStart, note) << "only the present-start marker is the per-frame clock";
    EXPECT_LT(ceIssued, note) << "a marker CE issues itself is not the title's frame";
    EXPECT_LT(note, replay);

    for (const char* fn : {"void ResetStartupProtectedOffChurnActiveProof(", "void MarkStartupProtectedOffChurnObserved("}) {
        const size_t begin = startupText.find(fn);
        ASSERT_NE(begin, std::string::npos) << fn;
        const size_t end = startupText.find("\n}\n", begin);
        ASSERT_NE(end, std::string::npos) << fn;
        const std::string body = startupText.substr(begin, end - begin);
        EXPECT_NE(body.find("streamline_hook_g_StartupProtectedOffChurnActiveProofCount.exchange(0"), std::string::npos)
            << fn;
        EXPECT_NE(body.find("streamline_hook_g_StartupProtectedOffChurnActiveFrameCount.exchange(0"), std::string::npos)
            << fn;
    }
}

TEST(StreamlineOffChurnProofTest, HeldOffGoesToTheTitleThreadOnceTheTitleMarkedAFrame) {
    using ce::streamline_runtime_policy::ShouldReplayHeldOffOnTitleThread;
    // The title marked a frame since the hold: its next marker replays it on its own thread.
    EXPECT_TRUE(ShouldReplayHeldOffOnTitleThread(true, /*sequence=*/8, /*atHold=*/7));
    // No marker since the hold (a title without PCL markers): the Present path forwards it.
    EXPECT_FALSE(ShouldReplayHeldOffOnTitleThread(true, 7, 7));
    // Nothing held.
    EXPECT_FALSE(ShouldReplayHeldOffOnTitleThread(false, 8, 7));
}

// GTA session 20260421_213224: the user had just enabled DLSS-G, a startup OFF was held, and GTA kept
// polling slDLSSGGetState with ON options - that OFF was churn. Witcher 3 through the bridge sends
// neither SetOptions(ON) nor GetState after its menu OFF - that OFF is its latest request.
TEST(StreamlineOffChurnProofTest, TitleOptionsReportedOnThroughGetStateSupersedeAHeldOff) {
    using ce::streamline_runtime_policy::ShouldTitleOptionsSupersedeHeldOff;
    EXPECT_TRUE(ShouldTitleOptionsSupersedeHeldOff(true, true, true, /*optionsModeEnabled=*/true));
    // The title's options say OFF: the held OFF stands.
    EXPECT_FALSE(ShouldTitleOptionsSupersedeHeldOff(true, true, true, false));
    // No options passed, or the call failed: no statement of intent.
    EXPECT_FALSE(ShouldTitleOptionsSupersedeHeldOff(true, true, /*optionsProvided=*/false, true));
    EXPECT_FALSE(ShouldTitleOptionsSupersedeHeldOff(true, /*getStateSucceeded=*/false, true, true));
    // Nothing held.
    EXPECT_FALSE(ShouldTitleOptionsSupersedeHeldOff(false, true, true, true));
}

// Witcher 3 opening its menu inside the 3 s startup window: the held OFF used to be dropped at expiry
// as "stale" once DLSS-G ran stably, leaving DLSS-G on in the menu. A held OFF is now cleared only by
// the title reporting ON again (SetOptions(ON) or GetState options); otherwise it is replayed.
TEST(StreamlineOffChurnProofSourceTest, HeldOffIsNeverDiscardedAndReplaysThroughCEsHandlerUnlocked) {
    namespace fs = std::filesystem;
    const fs::path apis = fs::current_path() / "hook" / "apis";
    for (const char* name : {"streamline_hook.cpp", "streamline_hook_dlssg.cpp", "streamline_hook_startup.cpp",
                             "streamline_hook_state.cpp"}) {
        const std::string text = ce::test_source::ReadLogicalSource(apis / name);
        ASSERT_FALSE(text.empty()) << name;
        EXPECT_EQ(text.find("ShouldDropSuppressedOffChurn"), std::string::npos) << name;
        EXPECT_EQ(text.find("Dropping stale suppressed"), std::string::npos) << name;
    }

    const std::string hook = ce::test_source::ReadLogicalSource(apis / "streamline_hook.cpp");
    const size_t service = hook.find("void ServiceHeldSetOptionsOffOnTitleFrame()");
    ASSERT_NE(service, std::string::npos);
    const size_t lock = hook.find("std::lock_guard<std::mutex> offLock(streamline_hook_g_SuppressedOffMutex);", service);
    const size_t deferral = hook.find("EvaluateHeldOffDeferral().keepDeferred", service);
    const size_t release = hook.find("streamline_hook_g_SuppressedSetOptionsOffDuringStartup = false;\n    }", service);
    const size_t replay = hook.find("Hooked_slDLSSGSetOptions(viewport, options);", service);
    ASSERT_NE(lock, std::string::npos);
    ASSERT_NE(deferral, std::string::npos);
    ASSERT_NE(release, std::string::npos) << "the held OFF is taken and the lock scope closes";
    ASSERT_NE(replay, std::string::npos);
    EXPECT_LT(lock, deferral);
    EXPECT_LT(deferral, release);
    EXPECT_LT(release, replay) << "Hooked_slDLSSGSetOptions takes the same mutex";

    const size_t flush = hook.find("void FlushSuppressedSetOptionsOffIfNeeded()");
    const size_t handoff = hook.find("ShouldReplayHeldOffOnTitleThread(", flush);
    const size_t rawForward = hook.find("originalSetOptions(streamline_hook_g_SuppressedOffViewport", flush);
    ASSERT_NE(handoff, std::string::npos);
    ASSERT_NE(rawForward, std::string::npos);
    EXPECT_LT(handoff, rawForward) << "the Present path forwards only when no title frame clock exists";

    const std::string dlssg = ce::test_source::ReadLogicalSource(apis / "streamline_hook_dlssg.cpp");
    const size_t hold = dlssg.find("streamline_hook_g_SuppressedOffOptions = adjustedOptions;");
    ASSERT_NE(hold, std::string::npos);
    const size_t clearNext = dlssg.find("streamline_hook_g_SuppressedOffOptions.next = nullptr;", hold);
    const size_t holdSequence = dlssg.find("streamline_hook_g_SuppressedOffTitleFrameSequence =", hold);
    EXPECT_NE(clearNext, std::string::npos) << "the title's extension chain does not outlive its call";
    EXPECT_NE(holdSequence, std::string::npos);

    // The GetState hook applies the title's options before it may flush the held OFF.
    const size_t getState = dlssg.find("slResult Hooked_slDLSSGGetState(");
    ASSERT_NE(getState, std::string::npos);
    const size_t supersede = dlssg.find("ShouldTitleOptionsSupersedeHeldOff(", getState);
    const size_t getStateFlush = dlssg.find("Forwarding suppressed slDLSSGSetOptions(OFF) via GetState", getState);
    ASSERT_NE(supersede, std::string::npos);
    ASSERT_NE(getStateFlush, std::string::npos);
    EXPECT_LT(supersede, getStateFlush);

    const std::string bridge = ce::test_source::ReadLogicalSource(apis / "streamline_bridge_reflex.cpp");
    const size_t synth = bridge.find("bool SynthesizePresentMarkers(");
    ASSERT_NE(synth, std::string::npos);
    const size_t scope = bridge.find("StreamlineHook::CeIssuedFrameMarkerScope", synth);
    const size_t firstMarker = bridge.find("g_slPCLSetMarker(sl::PCLMarker::ePresentStart", synth);
    ASSERT_NE(scope, std::string::npos);
    ASSERT_NE(firstMarker, std::string::npos);
    EXPECT_LT(scope, firstMarker) << "the bridge re-marks from inside sl.dlss_g's Present hook";
}

}  // namespace
