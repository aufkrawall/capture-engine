// When a display episode gets its raw trace (display_stall_trace_policy.h) and how the sealed file reaches the session
// folder (display_stall_trace_sealer.h). GTA V Enhanced 20261110_222057: 37 s of presents that never reached the
// screen, 3 composed frames at the very end - the case the trace exists for. 20261111_013758: the same stall for
// 1.71 s, which the expiry monitor only noticed after 1.5 s and counted as 531 ms, so nothing was sealed.

#include <gtest/gtest.h>

#include <windows.h>

#include <string>
#include <vector>

#include "captureengine/display_timing/display_stall_trace_policy.h"
#include "captureengine/display_timing/display_stall_trace_sealer.h"

namespace {

using display_stall_trace::Policy;
using display_stall_trace::Prune;
using display_stall_trace::SealReason;

constexpr int64_t kFirstUnshownMs = 4'649'600;  // QPC milliseconds of the first present that never reached the screen

int64_t At(int64_t sinceFirstUnshownMs) {
    return (kFirstUnshownMs + sinceFirstUnshownMs) * 1000;
}

// The expiry monitor starts an episode once the first unshown present is older than the completion bound.
Prune Start(int64_t sinceFirstUnshownMs, uint64_t shownTotal = 0) {
    Prune prune;
    prune.started = true;
    prune.expiring = true;
    prune.nowUs = At(sinceFirstUnshownMs);
    prune.firstUnshownUs = At(0);
    prune.shownTotal = shownTotal;
    prune.processId = 6612;
    return prune;
}

Prune Expiring(int64_t sinceFirstUnshownMs, uint64_t shownTotal = 0) {
    Prune prune;
    prune.expiring = true;
    prune.nowUs = At(sinceFirstUnshownMs);
    prune.shownTotal = shownTotal;
    return prune;
}

Prune Stop(int64_t sinceFirstUnshownMs, uint64_t shownTotal = 0) {
    Prune prune;
    prune.stopped = true;
    prune.nowUs = At(sinceFirstUnshownMs);
    prune.shownTotal = shownTotal;
    return prune;
}

TEST(DisplayStallTracePolicyTest, ALongSilentEpisodeIsSealedOnceWhileItLastsAndAgainWhenItEnds) {
    Policy policy;
    EXPECT_FALSE(policy.Observe(Start(1'024, 100)));
    EXPECT_FALSE(policy.Observe(Expiring(9'999, 100)));
    const auto ongoing = policy.Observe(Expiring(10'000, 100));
    ASSERT_TRUE(ongoing);
    EXPECT_EQ(ongoing.reason, SealReason::kOngoing);
    EXPECT_EQ(ongoing.episode, 1u);
    EXPECT_EQ(ongoing.processId, 6612u);
    EXPECT_EQ(ongoing.silenceMs, 10'000u);
    EXPECT_EQ(ongoing.silenceStartUs, At(0));
    EXPECT_FALSE(policy.Observe(Expiring(20'000, 100)));
    // Recovery: the compositor shows three frames, then the game's own flips resume.
    EXPECT_FALSE(policy.Observe(Expiring(37'047, 103)));
    const auto ended = policy.Observe(Stop(37'300, 103));
    ASSERT_TRUE(ended);
    EXPECT_EQ(ended.reason, SealReason::kEnded);
    EXPECT_EQ(ended.silenceMs, 37'047u);
    EXPECT_EQ(ended.shown, 3u);
    EXPECT_EQ(policy.seals(), 2u);
}

TEST(DisplayStallTracePolicyTest, TheMonitorsLateStartDoesNotHideAShortStall) {
    // 20261111_013758: last screen change, then 156 presents; the monitor started 1525 ms after the first unshown one,
    // the compositor showed 3 frames at 1785 ms and the own flips resumed at 2049 ms.
    Policy policy;
    EXPECT_FALSE(policy.Observe(Start(1'525)));
    EXPECT_FALSE(policy.Observe(Expiring(1'785, 3)));
    const auto ended = policy.Observe(Stop(2'049, 3));
    ASSERT_TRUE(ended);
    EXPECT_EQ(ended.reason, SealReason::kEnded);
    EXPECT_EQ(ended.silenceMs, 1'785u);
    EXPECT_EQ(ended.shown, 3u);
}

TEST(DisplayStallTracePolicyTest, AComposedEpisodeThatShowedTheGameIsNoStall) {
    // Windowed with an overlay above: the compositor shows the game from the first prune of the episode on.
    Policy policy;
    EXPECT_FALSE(policy.Observe(Start(1'024)));
    EXPECT_FALSE(policy.Observe(Expiring(1'040, 2)));
    EXPECT_FALSE(policy.Observe(Expiring(12'000, 1'500)));
    EXPECT_FALSE(policy.Observe(Stop(15'000, 1'900)));
    EXPECT_EQ(policy.seals(), 0u);
}

TEST(DisplayStallTracePolicyTest, AShortHandoffGapIsNotSealed) {
    Policy policy;
    policy.Observe(Start(1'024));
    EXPECT_FALSE(policy.Observe(Stop(1'499)));
    policy.Observe(Start(1'024));
    EXPECT_TRUE(policy.Observe(Stop(1'500)));
}

TEST(DisplayStallTracePolicyTest, WithoutTheFirstUnshownPresentTheSilenceStartsAtTheEpisode) {
    Policy policy;
    Prune start = Start(1'024);
    start.firstUnshownUs = 0;
    policy.Observe(start);
    EXPECT_FALSE(policy.Observe(Stop(2'000)));  // 976 ms from the episode's start
    start.firstUnshownUs = At(5'000);           // later than the prune itself: not believable
    policy.Observe(start);
    const auto ended = policy.Observe(Stop(2'600));
    ASSERT_TRUE(ended);
    EXPECT_EQ(ended.silenceStartUs, At(1'024));
}

TEST(DisplayStallTracePolicyTest, EveryEpisodeGetsItsOwnNumberAndTheSessionIsCapped) {
    Policy policy;
    uint32_t lastEpisode = 0;
    for (uint32_t i = 0; i < display_stall_trace::kMaximumSealsPerSession + 3; ++i) {
        policy.Observe(Start(1'024));
        const auto ended = policy.Observe(Stop(2'000));
        if (i < display_stall_trace::kMaximumSealsPerSession) {
            ASSERT_TRUE(ended);
            EXPECT_GT(ended.episode, lastEpisode);
            lastEpisode = ended.episode;
        } else {
            EXPECT_FALSE(ended);
        }
    }
    EXPECT_EQ(policy.seals(), display_stall_trace::kMaximumSealsPerSession);
    EXPECT_EQ(policy.refusedForLimit(), 3u);
}

TEST(DisplayStallTracePolicyTest, AStopOutsideAnEpisodeSealsNothing) {
    Policy policy;
    EXPECT_FALSE(policy.Observe(Stop(50'000)));
    policy.Observe(Start(1'024));
    EXPECT_TRUE(policy.Observe(Stop(3'000)));
    EXPECT_FALSE(policy.Observe(Stop(9'000)));
    EXPECT_EQ(policy.seals(), 1u);
}

std::array<wchar_t, 260> ServicePath(const wchar_t* text) {
    std::array<wchar_t, 260> path{};
    wcsncpy_s(path.data(), path.size(), text, _TRUNCATE);
    return path;
}

display_stall_trace::SealRequest Ended(uint32_t episode) {
    display_stall_trace::SealRequest request;
    request.reason = SealReason::kEnded;
    request.episode = episode;
    request.processId = 6612;
    request.silenceMs = 37'047;
    return request;
}

TEST(DisplayStallTraceSealerTest, TheSealedFileMovesIntoTheSessionFolderUnderTheEpisodesName) {
    std::vector<std::pair<std::wstring, std::wstring>> moves;
    DisplayStallTraceSealer sealer;
    sealer.Start(
        [](ce::elevation::SealedDisplayTrace& sealed) {
            sealed.path = ServicePath(L"C:\\CE\\DisplayTraces\\display_trace_42.etl");
            sealed.bytes = 1234;
            return true;
        },
        L"C:\\logs\\20261110_222057",
        [&moves](const std::wstring& from, const std::wstring& to) {
            moves.emplace_back(from, to);
            return static_cast<unsigned long>(ERROR_SUCCESS);
        });
    sealer.Request(Ended(3));
    sealer.WaitIdle();
    sealer.Stop();
    ASSERT_EQ(moves.size(), 1u);
    EXPECT_EQ(moves[0].first, L"C:\\CE\\DisplayTraces\\display_trace_42.etl");
    EXPECT_EQ(moves[0].second, L"C:\\logs\\20261110_222057\\display_stall_6612_ep3_ended.etl");
}

TEST(DisplayStallTraceSealerTest, AFailedSealOrAForeignPathMovesNothing) {
    int moves = 0;
    int calls = 0;
    DisplayStallTraceSealer sealer;
    sealer.Start(
        [&calls](ce::elevation::SealedDisplayTrace& sealed) {
            ++calls;
            if (calls == 1) {
                sealed.status = ERROR_RETRY;
                return true;
            }
            if (calls == 2)
                return false;
            sealed.path = ServicePath(L"C:\\Windows\\System32\\display_trace_1.etl");
            return true;
        },
        L"C:\\logs\\s",
        [&moves](const std::wstring&, const std::wstring&) {
            ++moves;
            return static_cast<unsigned long>(ERROR_SUCCESS);
        });
    for (uint32_t episode = 1; episode <= 3; ++episode) {
        sealer.Request(Ended(episode));
        sealer.WaitIdle();
    }
    sealer.Stop();
    EXPECT_EQ(calls, 3);
    EXPECT_EQ(moves, 0);
}

TEST(DisplayStallTraceSealerTest, NoRequestIsTakenBeforeStartOrAfterStop) {
    int calls = 0;
    DisplayStallTraceSealer sealer;
    sealer.Request(Ended(1));
    sealer.Start(
        [&calls](ce::elevation::SealedDisplayTrace&) {
            ++calls;
            return false;
        },
        L"C:\\logs\\s");
    sealer.WaitIdle();
    sealer.Stop();
    sealer.Request(Ended(2));
    EXPECT_EQ(calls, 0);
}

}  // namespace
