#include <gtest/gtest.h>

#include <array>
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

constexpr uint32_t kPid = 4242;

// Shorthands: a token of the default surface created / changing state at a time in microseconds.
void Create(CompositionTokenTally& tally, uint64_t present, int64_t timeUs = 0, uint64_t luid = kLuid,
            uint64_t bind = kBind, uint32_t pid = kPid) {
    tally.ObserveCreated(pid, luid, present, bind, timeUs);
}

CompositionTokenTally::Match State(CompositionTokenTally& tally, uint32_t present, uint32_t state,
                                   bool independentFlip = false, int64_t timeUs = 0, uint64_t luid = kLuid,
                                   uint64_t bind = kBind) {
    return tally.ObserveState(luid, present, bind, state, independentFlip, timeUs);
}

// A composed present: InFrame, Confirmed, Retired, then the release.
void Compose(CompositionTokenTally& tally, uint32_t present, int64_t timeUs = 0, uint64_t luid = kLuid,
             uint64_t bind = kBind) {
    State(tally, present, kStateInFrame, false, timeUs, luid, bind);
    State(tally, present, kStateConfirmed, false, timeUs, luid, bind);
    State(tally, present, kStateRetired, false, timeUs, luid, bind);
    State(tally, present, kStateDiscarded, false, timeUs, luid, bind);
}

TEST(DisplayTimingTokens, ATrackedTokenIsCountedWhenTheCompositorRetiresIt) {
    CompositionTokenTally tally;
    Create(tally, 100);
    ASSERT_TRUE(tally.HasTrackedTokens());
    EXPECT_EQ(State(tally, 100, kStateInFrame), CompositionTokenTally::Match::kIgnored);
    EXPECT_EQ(State(tally, 100, kStateConfirmed), CompositionTokenTally::Match::kCounted);
    EXPECT_EQ(State(tally, 100, kStateRetired), CompositionTokenTally::Match::kCounted);
    // The release of a retired present follows at once (GTA trace 20261111_153313): not a second end.
    EXPECT_EQ(State(tally, 100, kStateDiscarded), CompositionTokenTally::Match::kIgnored);
    EXPECT_EQ(tally.total().confirmed, 1u);
    EXPECT_EQ(tally.total().retired, 1u);
    EXPECT_EQ(tally.total().discarded, 0u);
    EXPECT_EQ(tally.unmatched(), 0u);
    EXPECT_FALSE(tally.HasTrackedTokens());
    EXPECT_EQ(State(tally, 100, kStateRetired), CompositionTokenTally::Match::kUnmatched);
    EXPECT_EQ(tally.total().retired, 1u);
}

TEST(DisplayTimingTokens, AnIndependentFlipEndIsShownNotDiscarded) {
    // Steady independent flip: Completed -> InFrame -> Discarded with IndependentFlip=1, no retirement.
    CompositionTokenTally tally;
    Create(tally, 8);
    State(tally, 8, kStateCompleted, true);
    State(tally, 8, kStateInFrame, true);
    EXPECT_EQ(State(tally, 8, kStateDiscarded, true), CompositionTokenTally::Match::kCounted);
    EXPECT_EQ(tally.total().flipped, 1u);
    EXPECT_EQ(tally.total().discarded, 0u);
    EXPECT_FALSE(tally.HasTrackedTokens());
}

TEST(DisplayTimingTokens, AnUnshownTokenIsToldApartFromShownOnes) {
    CompositionTokenTally tally;
    for (uint64_t present = 1; present <= 6; ++present) {
        Create(tally, present);
    }
    Compose(tally, 1);
    State(tally, 2, kStateDiscarded, true);
    // The cold DLSS-G black screen: confirmed in a compositor frame, then discarded without being shown.
    for (uint32_t present = 3; present <= 6; ++present) {
        State(tally, present, kStateInFrame);
        State(tally, present, kStateConfirmed);
        State(tally, present, kStateDiscarded);
    }
    EXPECT_EQ(tally.total().retired, 1u);
    EXPECT_EQ(tally.total().flipped, 1u);
    EXPECT_EQ(tally.total().discarded, 4u);
    EXPECT_EQ(tally.total().confirmed, 5u);
    EXPECT_EQ(tally.created(), 6u);
}

TEST(DisplayTimingTokens, StateChangesOfOtherProcessesAreNotCounted) {
    CompositionTokenTally tally;
    EXPECT_EQ(State(tally, 1, kStateRetired), CompositionTokenTally::Match::kUnmatched);
    Create(tally, 7);
    // Another surface, another present, another bind: all someone else's.
    EXPECT_EQ(State(tally, 7, kStateRetired, false, 0, kLuid + 1), CompositionTokenTally::Match::kUnmatched);
    EXPECT_EQ(State(tally, 8, kStateRetired), CompositionTokenTally::Match::kUnmatched);
    EXPECT_EQ(State(tally, 7, kStateRetired, false, 0, kLuid, kBind + 1), CompositionTokenTally::Match::kUnmatched);
    EXPECT_EQ(tally.unmatched(), 4u);
    EXPECT_EQ(tally.total().retired, 0u);
    EXPECT_TRUE(tally.HasTrackedTokens());
}

TEST(DisplayTimingTokens, ThePresentCountMatchesOnItsLowHalf) {
    // TokenCompositionSurfaceObject carries a 64-bit PresentCount, TokenStateChanged a 32-bit one.
    CompositionTokenTally tally;
    Create(tally, 0x1'0000'0005ull);
    EXPECT_EQ(State(tally, 5, kStateRetired), CompositionTokenTally::Match::kCounted);
}

TEST(DisplayTimingTokens, EpisodeCountsRestartWhileTheTotalsDoNot) {
    CompositionTokenTally tally;
    Create(tally, 1);
    Create(tally, 2);
    State(tally, 1, kStateRetired);
    EXPECT_EQ(tally.episode().retired, 1u);
    tally.BeginEpisode();
    EXPECT_EQ(tally.episode().retired, 0u);
    State(tally, 2, kStateDiscarded);
    EXPECT_EQ(tally.episode().discarded, 1u);
    EXPECT_EQ(tally.episode().retired, 0u);
    EXPECT_EQ(tally.total().retired, 1u);
    EXPECT_EQ(tally.total().discarded, 1u);
}

TEST(DisplayTimingTokens, ANeverEndingProducerIsBoundedAndTheOldestTokenGoesFirst) {
    CompositionTokenTally tally;
    for (uint64_t present = 1; present <= CompositionTokenTally::kMaxTracked + 10; ++present) {
        Create(tally, present);
    }
    EXPECT_EQ(tally.tracked(), CompositionTokenTally::kMaxTracked);
    EXPECT_EQ(tally.evicted(), 10u);
    EXPECT_EQ(State(tally, 1, kStateRetired), CompositionTokenTally::Match::kUnmatched);
    EXPECT_EQ(State(tally, 11, kStateRetired), CompositionTokenTally::Match::kCounted);
}

TEST(DisplayTimingTokens, AHealthySwitchShowsItsNewSurfaceWithinItsFirstPresents) {
    // 20261111_153313: the DLSS-G chain's first present at 0, composed and retired 25 ms later.
    CompositionTokenTally tally;
    Create(tally, 1, 0);
    EXPECT_FALSE(tally.TakeOnset().has_value());
    Compose(tally, 1, 25'000);
    const auto onset = tally.TakeOnset();
    ASSERT_TRUE(onset.has_value());
    EXPECT_EQ(onset->processId, kPid);
    EXPECT_EQ(onset->surfaceLuid, kLuid);
    EXPECT_EQ(onset->bindId, kBind);
    EXPECT_EQ(onset->firstPresentUs, 0);
    EXPECT_EQ(onset->shownUs, 25'000);
    EXPECT_FALSE(onset->shownByIndependentFlip);
    EXPECT_EQ(onset->presents, 1u);
    EXPECT_EQ(onset->discardedBefore, 0u);
    // Later presents of a surface already shown start nothing.
    Create(tally, 2, 30'000);
    Compose(tally, 2, 40'000);
    EXPECT_FALSE(tally.TakeOnset().has_value());
}

TEST(DisplayTimingTokens, ABlackSwitchReportsTheUnshownPresentsAndTheDelay) {
    // 20261111_024650: ~150 presents confirmed and discarded before DWM bound the new surface at ~1.36 s.
    CompositionTokenTally tally;
    const int64_t start = 8'802'354'064;
    for (uint32_t present = 1; present <= 150; ++present) {
        Create(tally, present, start + present * 9'000);
        State(tally, present, kStateInFrame);
        State(tally, present, kStateConfirmed);
        State(tally, present, kStateDiscarded);
        EXPECT_FALSE(tally.TakeOnset().has_value());
    }
    Create(tally, 151, start + 1'359'000);
    Compose(tally, 151, start + 1'384'000);
    const auto onset = tally.TakeOnset();
    ASSERT_TRUE(onset.has_value());
    EXPECT_EQ(onset->shownUs - onset->firstPresentUs, 1'384'000 - 9'000);
    EXPECT_EQ(onset->presents, 151u);
    EXPECT_EQ(onset->discardedBefore, 150u);
}

TEST(DisplayTimingTokens, AnOnsetCanEndByIndependentFlip) {
    CompositionTokenTally tally;
    Create(tally, 1, 100);
    State(tally, 1, kStateDiscarded, true, 4'100);
    const auto onset = tally.TakeOnset();
    ASSERT_TRUE(onset.has_value());
    EXPECT_TRUE(onset->shownByIndependentFlip);
    EXPECT_EQ(onset->shownUs, 4'100);
}

TEST(DisplayTimingTokens, ANewBindingOfTheSameSurfaceIsANewOnsetAndOldOnesAreRemembered) {
    CompositionTokenTally tally;
    Create(tally, 1, 0);
    Compose(tally, 1, 1'000);
    ASSERT_TRUE(tally.TakeOnset().has_value());
    // ResizeBuffers rebinds the chain: same surface, next binding.
    Create(tally, 2, 2'000, kLuid, kBind + 1);
    Compose(tally, 2, 3'000, kLuid, kBind + 1);
    const auto rebound = tally.TakeOnset();
    ASSERT_TRUE(rebound.has_value());
    EXPECT_EQ(rebound->bindId, kBind + 1);
    // Back on the first binding: remembered, no onset.
    Create(tally, 3, 4'000);
    Compose(tally, 3, 5'000);
    EXPECT_FALSE(tally.TakeOnset().has_value());
}

TEST(DisplayTimingTokens, ASurfaceLeftBeforeItWasShownIsReportedUnshown) {
    CompositionTokenTally tally;
    Create(tally, 1, 0);
    State(tally, 1, kStateDiscarded);
    Create(tally, 2, 10'000);
    State(tally, 2, kStateDiscarded);
    // The process moves to another surface: the first onset ends without a shown frame.
    Create(tally, 1, 20'000, kLuid + 1);
    const auto left = tally.TakeOnset();
    ASSERT_TRUE(left.has_value());
    EXPECT_EQ(left->surfaceLuid, kLuid);
    EXPECT_EQ(left->shownUs, 0);
    EXPECT_EQ(left->presents, 2u);
    EXPECT_EQ(left->discardedBefore, 2u);
    // Another process's new surface does not end it.
    CompositionTokenTally other;
    Create(other, 1, 0);
    Create(other, 1, 0, kLuid + 5, kBind, kPid + 1);
    EXPECT_FALSE(other.TakeOnset().has_value());
}

TEST(DisplayTimingTokens, FinishedOnsetsAreBoundedAndTheLossIsCounted) {
    CompositionTokenTally tally;
    const std::size_t surfaces = CompositionTokenTally::kMaxOnsets + 3;
    for (std::size_t i = 0; i < surfaces; ++i) {
        const uint64_t luid = kLuid + i;
        Create(tally, 1, 0, luid);
        Compose(tally, 1, 1'000, luid);
    }
    EXPECT_EQ(tally.onsetsDropped(), 3u);
    std::size_t taken = 0;
    while (tally.TakeOnset().has_value()) {
        ++taken;
    }
    EXPECT_EQ(taken, CompositionTokenTally::kMaxOnsets);
}

TEST(DisplayTimingTokens, VersionOnePayloadsFromWindowsBuild26300Decode) {
    using namespace display_timing_etw;
    // Raw user data of GTA's token events in 20261111_153313 (56 bytes each, 64-bit process, version 1).
    const std::array<uint8_t, 56> created = {
        0x28, 0xca, 0xb3, 0xca, 0x00, 0xc8, 0xff, 0xff, 0x60, 0x91, 0x22, 0xe5, 0x00, 0xc8, 0xff, 0xff,
        0x02, 0x00, 0x00, 0x00, 0x48, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0x56, 0x58, 0x06,
        0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x0f, 0x00, 0x00, 0x70, 0x08, 0x00, 0x00};
    const std::array<uint8_t, 56> state = {
        0x60, 0x91, 0x22, 0xe5, 0x00, 0xc8, 0xff, 0xff, 0x01, 0x00, 0x00, 0x00, 0x62, 0x06, 0x00, 0x00,
        0x62, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x07, 0x56, 0x58, 0x06, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    Win32kTokenCreated token;
    ASSERT_TRUE(DecodeWin32kTokenCreated(created.data(), created.size(), true, token));
    EXPECT_EQ(token.presentCount, 0x648u);
    EXPECT_EQ(token.surfaceLuid, 106452487u);
    EXPECT_EQ(token.bindId, 2u);
    Win32kTokenState changed;
    ASSERT_TRUE(DecodeWin32kTokenState(state.data(), state.size(), true, changed));
    EXPECT_EQ(changed.presentCount, 0x662u);
    EXPECT_EQ(changed.newState, kStateCompleted);
    EXPECT_TRUE(changed.independentFlip);
    EXPECT_EQ(changed.surfaceLuid, 106452487u);
    EXPECT_EQ(changed.bindId, 2u);
}

TEST(DisplayTimingTokens, OnlyLayoutVersionsWhoseFieldsAreKnownAreDecoded) {
    // Version 1 appends fields to version 0; a later version may move them and is refused, not misread.
    EXPECT_TRUE(display_timing_etw::IsDecodableWin32kTokenVersion(0));
    EXPECT_TRUE(display_timing_etw::IsDecodableWin32kTokenVersion(1));
    EXPECT_FALSE(display_timing_etw::IsDecodableWin32kTokenVersion(2));
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
