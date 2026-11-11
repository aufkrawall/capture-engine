// When a display episode gets its raw trace (display_stall_trace_policy.h) and how the sealed file reaches the session
// folder (display_stall_trace_sealer.h). GTA V Enhanced 20261110_222057: 37 s of presents that never reached the
// screen, 3 composed frames at the very end - the case the trace exists for.

#include <gtest/gtest.h>

#include <windows.h>

#include <string>
#include <vector>

#include "captureengine/display_timing/display_stall_trace_policy.h"
#include "captureengine/display_timing/display_stall_trace_sealer.h"

namespace {

using display_stall_trace::Policy;
using display_stall_trace::SealReason;

struct Prune {
    bool started = false;
    bool stopped = false;
    bool expiring = false;
    uint64_t episodeMs = 0;
    uint64_t shownTotal = 0;
};

display_stall_trace::SealRequest Feed(Policy& policy, const Prune& prune, uint32_t pid = 6612) {
    return policy.Observe(prune.started, prune.stopped, prune.expiring, prune.episodeMs, prune.shownTotal, pid);
}

TEST(DisplayStallTracePolicyTest, ALongSilentEpisodeIsSealedOnceWhileItLastsAndAgainWhenItEnds) {
    Policy policy;
    EXPECT_FALSE(Feed(policy, {true, false, true, 0, 100}));
    EXPECT_FALSE(Feed(policy, {false, false, true, 9'999, 100}));
    const auto ongoing = Feed(policy, {false, false, true, 10'000, 100});
    ASSERT_TRUE(ongoing);
    EXPECT_EQ(ongoing.reason, SealReason::kOngoing);
    EXPECT_EQ(ongoing.episode, 1u);
    EXPECT_EQ(ongoing.processId, 6612u);
    EXPECT_FALSE(Feed(policy, {false, false, true, 20'000, 100}));
    // Recovery: three composed frames over 37 s still count as nothing on screen.
    const auto ended = Feed(policy, {false, true, false, 37'047, 103});
    ASSERT_TRUE(ended);
    EXPECT_EQ(ended.reason, SealReason::kEnded);
    EXPECT_EQ(ended.shown, 3u);
    EXPECT_EQ(policy.seals(), 2u);
}

TEST(DisplayStallTracePolicyTest, AComposedEpisodeThatShowedTheGameIsNoStall) {
    Policy policy;
    Feed(policy, {true, false, true, 0, 0});
    EXPECT_FALSE(Feed(policy, {false, false, true, 12'000, 1'500}));
    EXPECT_FALSE(Feed(policy, {false, true, false, 15'000, 1'900}));
    EXPECT_EQ(policy.seals(), 0u);
}

TEST(DisplayStallTracePolicyTest, AShortHandoffGapIsNotSealed) {
    Policy policy;
    Feed(policy, {true, false, true, 0, 0});
    EXPECT_FALSE(Feed(policy, {false, true, false, 1'499, 0}));
    Feed(policy, {true, false, true, 0, 0});
    EXPECT_TRUE(Feed(policy, {false, true, false, 1'500, 0}));
}

TEST(DisplayStallTracePolicyTest, EveryEpisodeGetsItsOwnNumberAndTheSessionIsCapped) {
    Policy policy;
    uint32_t lastEpisode = 0;
    for (uint32_t i = 0; i < display_stall_trace::kMaximumSealsPerSession + 3; ++i) {
        Feed(policy, {true, false, true, 0, 0});
        const auto ended = Feed(policy, {false, true, false, 2'000, 0});
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

TEST(DisplayStallTracePolicyTest, AStopWithoutAStartSealsNothing) {
    Policy policy;
    EXPECT_FALSE(Feed(policy, {false, true, false, 50'000, 0}));
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
    request.episodeMs = 37'047;
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
