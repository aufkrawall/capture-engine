#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <string>

#include "captureengine/display_timing/display_timing_composed.h"
#include "captureengine/display_timing/display_timing_tokens.h"
#include "common/platform/display_timing_etw.h"
#include "source_fragment_reader.h"

namespace {

using display_timing_tokens::CompositionTokenTally;
using display_timing_tokens::kStateCompleted;
using display_timing_tokens::kStateConfirmed;
using display_timing_tokens::kStateDiscarded;
using display_timing_tokens::kStateInFrame;
using display_timing_tokens::kStateRetired;

constexpr uint64_t kLuid = 0x1234'5678'9ABC;
constexpr uint64_t kBind = 0x77;

TEST(DisplayTimingTokens, ATrackedTokenIsCountedWhenTheCompositorRetiresIt) {
    CompositionTokenTally tally;
    tally.ObserveCreated(kLuid, 100, kBind);
    ASSERT_TRUE(tally.HasTrackedTokens());
    EXPECT_EQ(tally.ObserveState(kLuid, 100, kBind, kStateInFrame), CompositionTokenTally::Match::kIgnored);
    EXPECT_EQ(tally.ObserveState(kLuid, 100, kBind, kStateConfirmed), CompositionTokenTally::Match::kCounted);
    EXPECT_EQ(tally.ObserveState(kLuid, 100, kBind, kStateRetired), CompositionTokenTally::Match::kCounted);
    EXPECT_EQ(tally.total().confirmed, 1u);
    EXPECT_EQ(tally.total().retired, 1u);
    EXPECT_EQ(tally.total().discarded, 0u);
    // An end state ends the token: the same present is not counted twice.
    EXPECT_FALSE(tally.HasTrackedTokens());
    EXPECT_EQ(tally.ObserveState(kLuid, 100, kBind, kStateRetired), CompositionTokenTally::Match::kUnmatched);
    EXPECT_EQ(tally.total().retired, 1u);
}

TEST(DisplayTimingTokens, ADiscardedTokenIsToldApartFromARetiredOne) {
    CompositionTokenTally tally;
    for (uint64_t present = 1; present <= 5; ++present) {
        tally.ObserveCreated(kLuid, present, kBind);
    }
    tally.ObserveState(kLuid, 1, kBind, kStateRetired);
    for (uint32_t present = 2; present <= 5; ++present) {
        tally.ObserveState(kLuid, present, kBind, kStateDiscarded);
    }
    EXPECT_EQ(tally.total().retired, 1u);
    EXPECT_EQ(tally.total().discarded, 4u);
    EXPECT_EQ(tally.created(), 5u);
}

TEST(DisplayTimingTokens, StateChangesOfOtherProcessesAreNotCounted) {
    CompositionTokenTally tally;
    EXPECT_EQ(tally.ObserveState(kLuid, 1, kBind, kStateRetired), CompositionTokenTally::Match::kUnmatched);
    tally.ObserveCreated(kLuid, 7, kBind);
    // Another surface, another present, another bind: all someone else's.
    EXPECT_EQ(tally.ObserveState(kLuid + 1, 7, kBind, kStateRetired), CompositionTokenTally::Match::kUnmatched);
    EXPECT_EQ(tally.ObserveState(kLuid, 8, kBind, kStateRetired), CompositionTokenTally::Match::kUnmatched);
    EXPECT_EQ(tally.ObserveState(kLuid, 7, kBind + 1, kStateRetired), CompositionTokenTally::Match::kUnmatched);
    EXPECT_EQ(tally.unmatched(), 4u);
    EXPECT_EQ(tally.total().retired, 0u);
    EXPECT_TRUE(tally.HasTrackedTokens());
}

TEST(DisplayTimingTokens, ThePresentCountMatchesOnItsLowHalf) {
    // TokenCompositionSurfaceObject carries a 64-bit PresentCount, TokenStateChanged a 32-bit one.
    CompositionTokenTally tally;
    tally.ObserveCreated(kLuid, 0x1'0000'0005ull, kBind);
    EXPECT_EQ(tally.ObserveState(kLuid, 5, kBind, kStateRetired), CompositionTokenTally::Match::kCounted);
}

TEST(DisplayTimingTokens, EpisodeCountsRestartWhileTheTotalsDoNot) {
    CompositionTokenTally tally;
    tally.ObserveCreated(kLuid, 1, kBind);
    tally.ObserveCreated(kLuid, 2, kBind);
    tally.ObserveState(kLuid, 1, kBind, kStateRetired);
    EXPECT_EQ(tally.episode().retired, 1u);
    tally.BeginEpisode();
    EXPECT_EQ(tally.episode().retired, 0u);
    tally.ObserveState(kLuid, 2, kBind, kStateDiscarded);
    EXPECT_EQ(tally.episode().discarded, 1u);
    EXPECT_EQ(tally.episode().retired, 0u);
    EXPECT_EQ(tally.total().retired, 1u);
    EXPECT_EQ(tally.total().discarded, 1u);
}

TEST(DisplayTimingTokens, ANeverEndingProducerIsBoundedAndTheOldestTokenGoesFirst) {
    CompositionTokenTally tally;
    for (uint64_t present = 1; present <= CompositionTokenTally::kMaxTracked + 10; ++present) {
        tally.ObserveCreated(kLuid, present, kBind);
    }
    EXPECT_EQ(tally.tracked(), CompositionTokenTally::kMaxTracked);
    EXPECT_EQ(tally.evicted(), 10u);
    EXPECT_EQ(tally.ObserveState(kLuid, 1, kBind, kStateRetired), CompositionTokenTally::Match::kUnmatched);
    EXPECT_EQ(tally.ObserveState(kLuid, 11, kBind, kStateRetired), CompositionTokenTally::Match::kCounted);
}

TEST(DisplayTimingTokens, TheEventPayloadsAreDecodedAsPresentMonLaysThemOut) {
    using namespace display_timing_etw;
    // PresentMon's structs (Microsoft_Windows_Win32k.h), packed.
    static_assert(sizeof(Win32kTokenCreatedPayload<uint64_t>) == 8 + 8 + 4 + 8 + 8 + 8);
    static_assert(sizeof(Win32kTokenStatePayload<uint64_t>) == 8 + 4 + 4 + 8 + 4 + 4 + 4 + 8 + 8);
    static_assert(sizeof(Win32kTokenCreatedPayload<uint32_t>) == 4 + 4 + 4 + 8 + 8 + 8);
    static_assert(sizeof(Win32kTokenStatePayload<uint32_t>) == 4 + 4 + 4 + 8 + 4 + 4 + 4 + 8 + 8);

    {
        Win32kTokenStatePayload<uint64_t> payload = {};
        payload.presentCount = 42;
        payload.newState = kStateDiscarded;
        payload.surfaceLuid = kLuid;
        payload.bindId = kBind;
        Win32kTokenState state;
        ASSERT_TRUE(DecodeWin32kTokenState(&payload, sizeof(payload), true, state));
        EXPECT_EQ(state.surfaceLuid, kLuid);
        EXPECT_EQ(state.presentCount, 42u);
        EXPECT_EQ(state.bindId, kBind);
        EXPECT_EQ(state.newState, kStateDiscarded);
    }
    {
        // A 32-bit process: pointers are four bytes wide and every later field moves.
        Win32kTokenStatePayload<uint32_t> payload = {};
        payload.presentCount = 9;
        payload.newState = kStateRetired;
        payload.surfaceLuid = kLuid;
        payload.bindId = kBind;
        Win32kTokenState state;
        ASSERT_TRUE(DecodeWin32kTokenState(&payload, sizeof(payload), false, state));
        EXPECT_EQ(state.surfaceLuid, kLuid);
        EXPECT_EQ(state.presentCount, 9u);
        EXPECT_EQ(state.newState, kStateRetired);
    }
    {
        Win32kTokenCreatedPayload<uint64_t> payload = {};
        payload.presentCount = 0x1'0000'0007ull;
        payload.surfaceLuid = kLuid;
        payload.bindId = kBind;
        Win32kTokenCreated created;
        ASSERT_TRUE(DecodeWin32kTokenCreated(&payload, sizeof(payload), true, created));
        EXPECT_EQ(created.presentCount, 0x1'0000'0007ull);
        EXPECT_EQ(created.surfaceLuid, kLuid);
        EXPECT_EQ(created.bindId, kBind);
    }
}

TEST(DisplayTimingTokens, APayloadShorterThanItsLayoutIsRefused) {
    using namespace display_timing_etw;
    unsigned char bytes[sizeof(Win32kTokenStatePayload<uint64_t>)] = {};
    Win32kTokenState state;
    EXPECT_TRUE(DecodeWin32kTokenState(bytes, sizeof(bytes), true, state));
    EXPECT_FALSE(DecodeWin32kTokenState(bytes, sizeof(bytes) - 1, true, state));
    EXPECT_FALSE(DecodeWin32kTokenState(nullptr, sizeof(bytes), true, state));
    Win32kTokenCreated created;
    EXPECT_FALSE(DecodeWin32kTokenCreated(bytes, 8, true, created));
}

TEST(DisplayTimingTokens, TheSharedProviderSetEnablesTheTokenEventsWithPresentMonsIdsAndKeywords) {
    // Event ids and keywords from PresentMon's Microsoft_Windows_Win32k.h descriptors.
    EXPECT_EQ(display_timing_etw::kWin32kTokenCreated, 0x00c9);
    EXPECT_EQ(display_timing_etw::kWin32kTokenStateChanged, 0x012d);
    EXPECT_EQ(display_timing_etw::kWin32kTokenKeyword & 0x8000000000001000ull, 0x8000000000001000ull);
    EXPECT_EQ(display_timing_etw::kWin32kTokenKeyword & 0x8000000400001000ull, 0x8000000400001000ull);
    // Both the CE-owned and the elevation-service-owned session enable the one set (DisplayTimingSessionsShareOneProviderSet).
    const std::string etw = ce::test_source::ReadFile(std::filesystem::current_path() / "common" / "platform" /
                                                      "display_timing_etw.h");
    const size_t providerSet = etw.find("inline DisplayTimingProviderStatus EnableDisplayTimingProviders(");
    ASSERT_NE(providerSet, std::string::npos);
    EXPECT_NE(etw.find("kWin32kInputKeyword | kWin32kTokenKeyword", providerSet), std::string::npos);
    EXPECT_NE(etw.find("kWin32kTokenCreated,", providerSet), std::string::npos);
    EXPECT_NE(etw.find("kWin32kTokenStateChanged}", providerSet), std::string::npos);
}

TEST(DisplayTimingComposedCompositorActivity, CountsRestartAtBeginAndSurviveTheEndOfTheEpisode) {
    ComposedPresentation composed;
    composed.Begin(100, 4);
    EXPECT_EQ(composed.compositorPresents(), 0u);
    for (int i = 0; i < 9; ++i) {
        composed.NoteCompositorPresent();
    }
    for (int i = 0; i < 3; ++i) {
        composed.NoteCompositorFlip();
    }
    composed.End();
    // The line that ends the episode reads them after End.
    EXPECT_EQ(composed.compositorPresents(), 9u);
    EXPECT_EQ(composed.compositorFlips(), 3u);
    composed.Begin(100, 4);
    EXPECT_EQ(composed.compositorPresents(), 0u);
    EXPECT_EQ(composed.compositorFlips(), 0u);
}

}  // namespace
