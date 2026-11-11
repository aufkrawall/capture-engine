// When a stretch with nothing on screen gets its raw trace (display_stall_trace_policy.h) and how the sealed file
// reaches the session folder (display_stall_trace_sealer.h). GTA V Enhanced loading into the game with DLSS-G on:
// the screen stood still for 38.06 s (20261110_222057), 1.70 s (20261111_013758) and 1.38 s (20261111_015744) while
// the game kept presenting; every other FG switch stayed under 0.66 s.

#include <gtest/gtest.h>

#include <windows.h>

#include <string>
#include <vector>

#include "captureengine/display_timing/display_stall_trace_policy.h"
#include "captureengine/display_timing/display_stall_trace_sealer.h"

namespace {

using display_stall_trace::Policy;
using display_stall_trace::SealReason;
using display_stall_trace::SealRequest;

constexpr uint32_t kGame = 1220;
constexpr uint32_t kOther = 4242;

int64_t Ms(int64_t ms) {
    return 5'827'000'000 + ms * 1000;  // QPC microseconds, as the display timing service hands them over
}

// The game presents every `intervalMs` in [fromMs, toMs); with `shown`, every present reaches the screen 7 ms later.
void Present(Policy& policy, int64_t fromMs, int64_t toMs, int64_t intervalMs = 7, bool shown = false,
             uint32_t pid = kGame) {
    for (int64_t ms = fromMs; ms < toMs; ms += intervalMs) {
        policy.OnPresent(pid, Ms(ms));
        if (shown)
            policy.OnScreenChange(pid, Ms(ms + 7));
    }
}

// Prunes every 250 ms over [fromMs, toMs] and collects what they seal.
std::vector<SealRequest> Prune(Policy& policy, int64_t fromMs, int64_t toMs, uint32_t foreground = kGame) {
    std::vector<SealRequest> seals;
    for (int64_t ms = fromMs; ms <= toMs; ms += 250) {
        if (const SealRequest request = policy.Observe(Ms(ms), foreground))
            seals.push_back(request);
    }
    return seals;
}

TEST(DisplayStallTracePolicyTest, AShortBlackScreenIsSealedWhenTheScreenChangesAgain) {
    // 20261111_015744: the old chain's last frame reached the screen at 0 ms, the DLSS-G chain presented ~121 times
    // and its first own flip showed at 1372 ms.
    Policy policy;
    Present(policy, -1'000, 0, 7, true);
    Present(policy, 207, 1'365, 9);
    EXPECT_TRUE(Prune(policy, 0, 1'250).empty());
    policy.OnScreenChange(kGame, Ms(1'372));
    const auto seals = Prune(policy, 1'500, 1'500);
    ASSERT_EQ(seals.size(), 1u);
    EXPECT_EQ(seals[0].reason, SealReason::kEnded);
    EXPECT_EQ(seals[0].episode, 1u);
    EXPECT_EQ(seals[0].processId, kGame);
    EXPECT_EQ(seals[0].silenceStartUs, Ms(207));
    EXPECT_EQ(seals[0].silenceMs, 1'165u);
    EXPECT_EQ(seals[0].presents, 129u);
}

TEST(DisplayStallTracePolicyTest, ALongBlackScreenIsSealedWhileItLastsAndAgainWhenItEnds) {
    // 20261110_222057: ~131 presents a second for 38 s, nothing on screen; composed frames ended it.
    Policy policy;
    Present(policy, -1'000, 0, 7, true);
    Present(policy, 10, 38'060, 8);
    const auto running = Prune(policy, 0, 38'000);
    ASSERT_EQ(running.size(), 1u);
    EXPECT_EQ(running[0].reason, SealReason::kOngoing);
    EXPECT_GE(running[0].silenceMs, 10'000u);
    EXPECT_LT(running[0].silenceMs, 10'250u);
    policy.OnScreenChange(kGame, Ms(38'060));
    const auto ended = Prune(policy, 38'250, 38'250);
    ASSERT_EQ(ended.size(), 1u);
    EXPECT_EQ(ended[0].reason, SealReason::kEnded);
    EXPECT_EQ(ended[0].episode, running[0].episode);
    EXPECT_EQ(ended[0].silenceMs, 38'050u);
    EXPECT_EQ(policy.seals(), 2u);
}

TEST(DisplayStallTracePolicyTest, AnOrdinarySwapchainHandoffIsNoStall) {
    // Every other FG switch in those sessions: the new chain reached the screen within 0.66 s.
    Policy policy;
    Present(policy, -1'000, 0, 7, true);
    Present(policy, 300, 950, 7);
    policy.OnScreenChange(kGame, Ms(960));
    Present(policy, 960, 3'000, 7, true);
    EXPECT_TRUE(Prune(policy, 0, 3'000).empty());
}

TEST(DisplayStallTracePolicyTest, ABackgroundGameIsNoStall) {
    // Alt-tabbed: the game presents, nothing it presents shows, and another process owns the foreground.
    Policy policy;
    Present(policy, -1'000, 0, 7, true);
    Present(policy, 10, 3'000, 7);
    EXPECT_TRUE(Prune(policy, 0, 2'000, kOther).empty());
    EXPECT_TRUE(Prune(policy, 2'250, 2'750).empty());  // back in front: this silence stays disqualified
    policy.OnScreenChange(kGame, Ms(3'000));
    EXPECT_TRUE(Prune(policy, 3'000, 3'250).empty());
    // The next silence in the foreground counts again.
    Present(policy, 3'010, 4'500, 7);
    policy.OnScreenChange(kGame, Ms(4'500));
    EXPECT_EQ(Prune(policy, 4'750, 4'750).size(), 1u);
}

TEST(DisplayStallTracePolicyTest, AGameThatStopsPresentingIsNoStall) {
    // A loading hitch: presents pause for longer than the gap bound, so the silence starts over after it.
    Policy policy;
    Present(policy, -1'000, 0, 7, true);
    Present(policy, 10, 700, 7);
    Present(policy, 1'300, 2'000, 7);
    policy.OnScreenChange(kGame, Ms(2'000));
    EXPECT_TRUE(Prune(policy, 0, 2'250).empty());
}

TEST(DisplayStallTracePolicyTest, NothingIsMeasuredBeforeTheGameFirstReachesTheScreen) {
    Policy policy;
    Present(policy, 0, 5'000, 7);
    policy.OnScreenChange(kGame, Ms(5'000));
    EXPECT_TRUE(Prune(policy, 0, 15'000).empty());
}

TEST(DisplayStallTracePolicyTest, AFewSlowPresentsAreNoStall) {
    Policy policy;
    Present(policy, -1'000, 0, 7, true);
    Present(policy, 10, 1'500, 250);
    policy.OnScreenChange(kGame, Ms(1'500));
    EXPECT_TRUE(Prune(policy, 0, 1'750).empty());
}

TEST(DisplayStallTracePolicyTest, AChangePublishedLateStillEndsTheSilenceItShowed) {
    // Changes are published after the reorder window: a present is counted first, then the change that showed it.
    Policy policy;
    Present(policy, -1'000, 0, 7, true);
    Present(policy, 10, 1'200, 7);
    policy.OnScreenChange(kGame, Ms(15));
    Present(policy, 1'200, 2'000, 7, true);
    EXPECT_TRUE(Prune(policy, 0, 2'250).empty());
}

TEST(DisplayStallTracePolicyTest, EveryEpisodeGetsItsOwnNumberAndTheSessionIsCapped) {
    Policy policy;
    Present(policy, -1'000, 0, 7, true);
    uint32_t lastEpisode = 0;
    for (uint32_t i = 0; i < display_stall_trace::kMaximumSealsPerSession + 3; ++i) {
        const int64_t base = 10 + static_cast<int64_t>(i) * 3'000;
        Present(policy, base, base + 1'500, 7);
        policy.OnScreenChange(kGame, Ms(base + 1'500));
        const auto seals = Prune(policy, base + 1'750, base + 1'750);
        if (i < display_stall_trace::kMaximumSealsPerSession) {
            ASSERT_EQ(seals.size(), 1u);
            EXPECT_GT(seals[0].episode, lastEpisode);
            lastEpisode = seals[0].episode;
        } else {
            EXPECT_TRUE(seals.empty());
        }
    }
    EXPECT_EQ(policy.seals(), display_stall_trace::kMaximumSealsPerSession);
    EXPECT_EQ(policy.refusedForLimit(), 3u);
}

TEST(DisplayStallTracePolicyTest, ANewProcessTakesTheLeastRecentSlot) {
    Policy policy;
    for (uint32_t pid = 100; pid < 100 + display_stall_trace::kMaximumProcesses; ++pid)
        Present(policy, -1'000 + pid, 0, 7, true, pid);
    Present(policy, -1'000, 0, 7, true);
    Present(policy, 10, 1'500, 7);
    policy.OnScreenChange(kGame, Ms(1'500));
    const auto seals = Prune(policy, 1'750, 1'750);
    ASSERT_EQ(seals.size(), 1u);
    EXPECT_EQ(seals[0].processId, kGame);
}

constexpr uint64_t kLoadingChain = 0x1455aca26b0;
constexpr uint64_t kDlssgChain = 0x1498745fb50;

// Presents on one swapchain every `intervalMs` in [fromMs, toMs); with `shown`, each reaches the screen 7 ms later.
void PresentOn(Policy& policy, uint64_t chain, int64_t fromMs, int64_t toMs, int64_t intervalMs = 7,
               bool shown = false, uint32_t pid = kGame) {
    for (int64_t ms = fromMs; ms < toMs; ms += intervalMs) {
        policy.OnPresent(pid, Ms(ms), chain);
        if (shown)
            policy.OnScreenChange(pid, Ms(ms + 7));
    }
}

TEST(DisplayStallTracePolicyTest, AQuickSwapchainSwitchIsSealedAsAReference) {
    // 20261111_145243 (overlay observer modes): the DLSS-G chain's first frame showed 130 ms after its first present.
    Policy policy;
    PresentOn(policy, kLoadingChain, -1'000, 0, 16, true);
    PresentOn(policy, kDlssgChain, 207, 337, 22);
    EXPECT_TRUE(Prune(policy, 0, 250).empty());
    policy.OnScreenChange(kGame, Ms(337));
    const auto seals = Prune(policy, 500, 500);
    ASSERT_EQ(seals.size(), 1u);
    EXPECT_EQ(seals[0].reason, SealReason::kSwitch);
    EXPECT_EQ(seals[0].processId, kGame);
    EXPECT_EQ(seals[0].silenceStartUs, Ms(207));
    EXPECT_EQ(seals[0].silenceMs, 130u);
    EXPECT_EQ(seals[0].presents, 6u);
    EXPECT_EQ(DisplayStallTraceSealer::DestinationName(seals[0]), L"display_stall_1220_ep1_switch.etl");
    // The chain keeps showing: nothing more.
    PresentOn(policy, kDlssgChain, 340, 3'000, 7, true);
    EXPECT_TRUE(Prune(policy, 750, 3'000).empty());
    EXPECT_EQ(policy.switchSeals(), 1u);
    EXPECT_EQ(policy.seals(), 0u);
}

TEST(DisplayStallTracePolicyTest, ASlowSwitchIsSealedAsTheStallOnly) {
    // 20261111_024650: the DLSS-G chain's first frame after 1.42 s and 152 presents.
    Policy policy;
    PresentOn(policy, kLoadingChain, -1'000, 0, 16, true);
    PresentOn(policy, kDlssgChain, 0, 1'420, 9);
    policy.OnScreenChange(kGame, Ms(1'420));
    const auto seals = Prune(policy, 1'500, 1'750);
    ASSERT_EQ(seals.size(), 1u);
    EXPECT_EQ(seals[0].reason, SealReason::kEnded);
    EXPECT_EQ(policy.switchSeals(), 0u);
}

TEST(DisplayStallTracePolicyTest, TheFirstSwapchainAndUnknownPointersAreNoSwitch) {
    Policy policy;
    PresentOn(policy, kLoadingChain, 0, 300, 16);  // the game's first chain: nothing shown before it
    policy.OnScreenChange(kGame, Ms(310));
    PresentOn(policy, 0, 320, 600, 16, true);  // the runtime's pointer unread
    PresentOn(policy, kLoadingChain, 600, 900, 16, true);
    EXPECT_TRUE(Prune(policy, 0, 1'000).empty());
}

TEST(DisplayStallTracePolicyTest, ReturningToARecentSwapchainIsNoSwitch) {
    Policy policy;
    PresentOn(policy, kLoadingChain, -1'000, 0, 16, true);
    PresentOn(policy, kDlssgChain, 10, 200, 16);
    policy.OnScreenChange(kGame, Ms(200));
    ASSERT_EQ(Prune(policy, 250, 250).size(), 1u);
    PresentOn(policy, kDlssgChain, 200, 400, 16, true);
    PresentOn(policy, kLoadingChain, 410, 500, 16);  // back to the earlier chain (FG off again)
    policy.OnScreenChange(kGame, Ms(500));
    EXPECT_TRUE(Prune(policy, 750, 750).empty());
}

TEST(DisplayStallTracePolicyTest, SwitchReferencesHaveTheirOwnLimit) {
    Policy policy;
    PresentOn(policy, 0x1000, -1'000, 0, 16, true);
    for (uint64_t i = 1; i <= display_stall_trace::kMaximumSwitchSealsPerSession + 2; ++i) {
        const int64_t base = static_cast<int64_t>(i) * 1'000;
        PresentOn(policy, 0x1000 + i * 0x10, base, base + 100, 16);
        policy.OnScreenChange(kGame, Ms(base + 100));
        PresentOn(policy, 0x1000 + i * 0x10, base + 100, base + 500, 16, true);
        Prune(policy, base + 750, base + 750);
    }
    EXPECT_EQ(policy.switchSeals(), display_stall_trace::kMaximumSwitchSealsPerSession);
    EXPECT_EQ(policy.refusedForLimit(), 2u);
    // The stall seals are untouched.
    PresentOn(policy, 0x1000 + 0x10, 9'000, 10'500, 7);
    policy.OnScreenChange(kGame, Ms(10'500));
    const auto seals = Prune(policy, 10'750, 10'750);
    ASSERT_EQ(seals.size(), 1u);
    EXPECT_EQ(seals[0].reason, SealReason::kEnded);
}

TEST(DisplayStallTracePolicyTest, ABackgroundSwitchIsNoReference) {
    Policy policy;
    PresentOn(policy, kLoadingChain, -1'000, 0, 16, true);
    PresentOn(policy, kDlssgChain, 10, 400, 16);
    EXPECT_TRUE(Prune(policy, 250, 250, kOther).empty());
    policy.OnScreenChange(kGame, Ms(400));
    EXPECT_TRUE(Prune(policy, 500, 750).empty());
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
    request.silenceMs = 38'050;
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
