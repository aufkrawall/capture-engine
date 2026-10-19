#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "common/capture/capture_policy/cfr_nearest_playout.h"
#include "common/capture/capture_policy/cfr_startup.h"
#include "common/capture/capture_policy/constants.h"
#include "source_fragment_reader.h"

// Inject CFR frame zero (ce::capture_policy::DecideInjectCfrStartFrame). BioShock Infinite at a 120 fps
// capture sync (20261019_173119) started its recordings on a frame 272 ms behind the content target: the
// producer had filled the inject ring before media ingested, warm-up kept that backlog, the go-live tick
// took its oldest frame, and a ~120 ms first encode held it until tick 1 superseded 12 frames to catch up.
namespace {

namespace policy = ce::capture_policy;

constexpr int64_t kFrequency = 10000000;
constexpr int64_t kInterval = kFrequency / 120;
constexpr int64_t kTarget = 171743535334;  // the session's go-live content target
const int64_t kLead = policy::GetInjectCfrSelectionLeadToleranceQpc(kInterval);

std::vector<int64_t> Grid(int64_t first, size_t count) {
    std::vector<int64_t> stamps;
    for (size_t i = 0; i < count; ++i) {
        stamps.push_back(first + static_cast<int64_t>(i) * kInterval);
    }
    return stamps;
}

TEST(InjectCfrStartFrameTest, AStaleBacklogIsDiscardedAndTheStartWaits) {
    // The session's warm-up buffer: 16 frames ending ~200 ms before the target.
    const std::vector<int64_t> backlog = Grid(kTarget - 2725714 - 15 * kInterval, 16);
    const auto decision = policy::DecideInjectCfrStartFrame(backlog.data(), backlog.size() - 1, kTarget, kLead,
                                                            kInterval, /*waitBudgetSpent=*/false);
    ASSERT_TRUE(decision.hasCandidate);
    EXPECT_FALSE(decision.fresh);
    EXPECT_FALSE(decision.ready) << "no frame near the target has arrived yet";
    EXPECT_EQ(decision.discardBefore, backlog.size() - 2) << "only the newest eligible frame is kept";
    EXPECT_GT(decision.ageQpc, 2000000);
}

TEST(InjectCfrStartFrameTest, FrameZeroIsTheFrameNearestTheTargetAndOlderFramesGo) {
    // Fresh frames around the target, one content delay of history, the last one still copying.
    const std::vector<int64_t> stamps = Grid(kTarget - 5 * kInterval + 1500, 9);
    const auto decision =
        policy::DecideInjectCfrStartFrame(stamps.data(), stamps.size() - 1, kTarget, kLead, kInterval, false);
    ASSERT_TRUE(decision.ready);
    EXPECT_TRUE(decision.fresh);
    EXPECT_EQ(decision.discardBefore, 5u);
    EXPECT_EQ(decision.ageQpc, -1500);
    EXPECT_LE(decision.ageQpc < 0 ? -decision.ageQpc : decision.ageQpc, kInterval / 2);
}

TEST(InjectCfrStartFrameTest, NeverStartsAheadOfTheTargetUnlessEveryFrameIs) {
    // 1 ms behind vs 0.3 ms ahead: inside the lead tolerance the nearer frame wins. A frame past the
    // tolerance is never chosen while an older one exists.
    const std::vector<int64_t> nearAhead = {kTarget - 10000, kTarget + 3000};
    EXPECT_EQ(policy::DecideInjectCfrStartFrame(nearAhead.data(), 2, kTarget, kLead, kInterval, false).discardBefore,
              1u);
    const std::vector<int64_t> pastTolerance = {kTarget - 30000, kTarget + kLead + 1};
    EXPECT_EQ(
        policy::DecideInjectCfrStartFrame(pastTolerance.data(), 2, kTarget, kLead, kInterval, false).discardBefore,
        0u);
    // Every frame newer than the target: the oldest, as the warm-up rule always did (causal).
    const std::vector<int64_t> allNewer = Grid(kTarget + kInterval, 4);
    const auto newest = policy::DecideInjectCfrStartFrame(allNewer.data(), 4, kTarget, kLead, kInterval, false);
    EXPECT_TRUE(newest.ready);
    EXPECT_EQ(newest.discardBefore, 0u);
    EXPECT_LT(newest.ageQpc, 0);
}

TEST(InjectCfrStartFrameTest, AStalledSourceStartsWithItsNewestFrameOnceTheBudgetIsSpent) {
    const std::vector<int64_t> stalled = Grid(kTarget - 400 * 10000, 6);
    const auto waiting = policy::DecideInjectCfrStartFrame(stalled.data(), 6, kTarget, kLead, kInterval, false);
    EXPECT_FALSE(waiting.ready);
    const auto started = policy::DecideInjectCfrStartFrame(stalled.data(), 6, kTarget, kLead, kInterval, true);
    EXPECT_TRUE(started.ready);
    EXPECT_FALSE(started.fresh);
    EXPECT_EQ(started.discardBefore, 5u);
    EXPECT_GT(policy::kInjectCfrStartFreshWaitMaxMs, 0u);
}

TEST(InjectCfrStartFrameTest, ASlowSourceIsFreshWithinItsOwnInterval) {
    // 30 fps game, 120 fps recording: the nearest frame can sit most of a source interval behind.
    const int64_t sourceInterval = kFrequency / 30;
    const std::vector<int64_t> stamps = {kTarget - sourceInterval - 200000, kTarget - 200000, kTarget + 300000};
    const auto byRecordingInterval =
        policy::DecideInjectCfrStartFrame(stamps.data(), 3, kTarget, kLead, kInterval, false);
    const auto bySourceInterval =
        policy::DecideInjectCfrStartFrame(stamps.data(), 3, kTarget, kLead, sourceInterval, false);
    EXPECT_EQ(byRecordingInterval.discardBefore, 1u);
    EXPECT_FALSE(byRecordingInterval.fresh) << "20 ms behind is more than one 120 fps interval";
    EXPECT_TRUE(bySourceInterval.ready);
}

TEST(InjectCfrStartFrameTest, NothingEligibleIsNoCandidate) {
    const std::vector<int64_t> stamps = Grid(kTarget, 2);
    EXPECT_FALSE(policy::DecideInjectCfrStartFrame(stamps.data(), 0, kTarget, kLead, kInterval, true).hasCandidate);
    EXPECT_FALSE(policy::DecideInjectCfrStartFrame(nullptr, 2, kTarget, kLead, kInterval, true).ready);
    EXPECT_FALSE(policy::DecideInjectCfrStartFrame(stamps.data(), 2, 0, kLead, kInterval, true).ready);
}

TEST(InjectCfrStartFrameTest, ReplayOfTheSessionStartLandsOnTheContentTarget) {
    // Go-live: the 16-frame backlog plus nothing fresh; each later tick one fresh frame arrives (stamped
    // with its present release, up to one interval ahead of now) and the target advances with the clock.
    std::vector<int64_t> buffered = Grid(kTarget - 2725714 - 15 * kInterval, 16);
    int64_t now = kTarget + 297870;  // the session's content delay
    int64_t nextFresh = now + 6000;
    size_t ticks = 0;
    for (; ticks < 30; ++ticks) {
        const int64_t target = now - 297870;
        const auto decision =
            policy::DecideInjectCfrStartFrame(buffered.data(), buffered.size() - 1, target, kLead, kInterval, false);
        buffered.erase(buffered.begin(), buffered.begin() + static_cast<std::ptrdiff_t>(decision.discardBefore));
        if (decision.ready) {
            EXPECT_TRUE(decision.fresh);
            EXPECT_LE(decision.ageQpc, kInterval);
            break;
        }
        now += kInterval;
        buffered.push_back(nextFresh);
        nextFresh += kInterval;
    }
    // Fresh frames reach the target one content delay (~30 ms, 4 ticks) after they start arriving.
    EXPECT_EQ(ticks, 4u);
}

std::string ReadRepoSource(const char* relative) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / relative);
}

TEST(InjectCfrStartFrameSourceTest, GoLivePicksFrameZeroByTargetAndKeepsWarmupFrames) {
    const std::string startup = ReadRepoSource("captureengine/media/media_main_encoder_06_loop_startup.cpp");
    const std::string inject = ReadRepoSource("captureengine/media/media_main_encoder_06_loop_startup_inject.cpp");
    ASSERT_FALSE(startup.empty());
    ASSERT_FALSE(inject.empty());

    const size_t pending = startup.find("if (pendingLiveActivation) {");
    const size_t select = startup.find("SelectInjectCfrStartFrame()", pending);
    const size_t commit = startup.find("TryCommitCapturePipelineLive()", select);
    ASSERT_NE(pending, std::string::npos);
    ASSERT_NE(select, std::string::npos);
    ASSERT_NE(commit, std::string::npos);
    EXPECT_LT(select, commit) << "frame zero is chosen before the pipeline goes live";

    // The warm-up reset neither flushes the freshest queued frames nor count-trims inject CFR history.
    EXPECT_NE(startup.find("if (!useScreenGrab && !injectCfrStart) {"), std::string::npos);
    EXPECT_NE(startup.find("if (!bufferedInjectFrames.empty() && !injectCfrStart) {"), std::string::npos);

    // The popped warm-up frame competes again, and the decision drives discards and readiness.
    const size_t requeue = inject.find("bufferedInjectFrames.push_front(std::move(frame));");
    const size_t decide = inject.find("DecideInjectCfrStartFrame(", requeue);
    const size_t discard = inject.find("DiscardQueuedFrame(stale);", decide);
    ASSERT_NE(requeue, std::string::npos);
    ASSERT_NE(decide, std::string::npos);
    ASSERT_NE(discard, std::string::npos);
    EXPECT_NE(inject.find("Inject CFR start frame: ageUs="), std::string::npos);
}

TEST(InjectCfrStartFrameSourceTest, InjectWarmupPrewarmsTheEncoderBeforeFrameZero) {
    const std::string startup = ReadRepoSource("captureengine/media/media_main_encoder_06_loop_startup.cpp");
    const std::string inject = ReadRepoSource("captureengine/media/media_main_encoder_06_loop_startup_inject.cpp");
    const std::string encoder = ReadRepoSource("mediaengine/video/video_encoder_framegrab.cpp");
    ASSERT_FALSE(encoder.empty());

    const size_t prewarm = startup.find("PrewarmInjectEncoder(frame);");
    const size_t warmupCommit = startup.find("CommitWarmupSync();");
    ASSERT_NE(prewarm, std::string::npos);
    ASSERT_NE(warmupCommit, std::string::npos);
    EXPECT_LT(prewarm, warmupCommit);
    EXPECT_NE(inject.find("MediaEngine_PrepareInjectVideo(warmupFrame.luidLow"), std::string::npos);
    EXPECT_NE(inject.find("injectEncoderPrewarmAttempted = true;"), std::string::npos);

    // The encoder prepares what the first inject frame would: output, header, conversion shaders.
    const size_t body = encoder.find("bool VideoEncoder::PrepareInjectVideo(");
    ASSERT_NE(body, std::string::npos);
    const size_t header = encoder.find("OpenOutputAndWriteHeader()", body);
    const size_t shaders = encoder.find("return EnsureSwapRBShader();", header);
    EXPECT_NE(header, std::string::npos);
    EXPECT_NE(shaders, std::string::npos);
    // Screen-grab prewarm compiles the same shaders.
    const size_t screenGrab = encoder.find("bool VideoEncoder::PrepareFrameD3D11(");
    ASSERT_NE(screenGrab, std::string::npos);
    EXPECT_LT(encoder.find("if (!outputWasOpen)\n        EnsureSwapRBShader();", screenGrab), body);
}

}  // namespace
