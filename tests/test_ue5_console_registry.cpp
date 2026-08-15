#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "../hook/common/ue5_console_registry.h"
#include "../hook/common/ue5_redirect_plan.h"

namespace {

// A UE FString payload: UTF-16LE code units including the terminating null.
std::vector<uint8_t> Utf16(const std::string& text) {
    std::vector<uint8_t> bytes;
    bytes.reserve((text.size() + 1) * 2);
    for (char character : text) {
        bytes.push_back(static_cast<uint8_t>(character));
        bytes.push_back(0);
    }
    bytes.push_back(0);
    bytes.push_back(0);
    return bytes;
}

ce::ue5_registry::StringHeader HeaderFor(const std::string& text) {
    ce::ue5_registry::StringHeader header;
    header.data = 0x2000;  // any non-null even address
    header.num = static_cast<int32_t>(text.size()) + 1;
    header.max = header.num;
    return header;
}

// Chunk stride of the anchor sweep in hook/main_ue5_registry.cpp: consecutive
// chunks overlap so an element straddling a boundary is still seen whole.
constexpr std::size_t kChunkBytes = 64 * 1024;
constexpr std::size_t kChunkOverlap = 64;

// One time-boxed pass over a region, mirroring the sweep loop. `chunkLimit`
// stands in for the wall-clock budget: the pass parks on the chunk it did not
// read, exactly as the real loop does.
std::vector<std::size_t> SweepPass(const ce::ue5_registry::RegionSpan& region, uintptr_t& cursor,
                                   std::size_t chunkLimit) {
    std::vector<std::size_t> visited;
    if (ce::ue5_registry::RegionAlreadySwept(region, cursor))
        return visited;
    for (std::size_t offset = ce::ue5_registry::SweepResumeOffset(region, cursor);
         offset < region.size; offset += kChunkBytes - kChunkOverlap) {
        if (visited.size() == chunkLimit) {
            cursor = region.base + offset;
            return visited;
        }
        visited.push_back(offset);
    }
    cursor = region.base + region.size;
    return visited;
}

}  // namespace

TEST(UE5ConsoleRegistryTest, AcceptsRealisticConsoleNameHeaders) {
    for (const char* name : {"ShowFlag.Vignette", "r.Tonemapper.Sharpen",
                             "r.Lumen.ScreenProbeGather.Temporal.MaxFramesAccumulated"}) {
        const auto header = HeaderFor(name);
        EXPECT_TRUE(ce::ue5_registry::IsPlausibleStringHeader(header)) << name;
    }
}

TEST(UE5ConsoleRegistryTest, RejectsImplausibleStringHeaders) {
    auto header = HeaderFor("ShowFlag.Vignette");

    auto empty = header;
    empty.num = 1;  // just the terminator: never a CVar name
    EXPECT_FALSE(ce::ue5_registry::IsPlausibleStringHeader(empty));

    auto negative = header;
    negative.num = -4;
    EXPECT_FALSE(ce::ue5_registry::IsPlausibleStringHeader(negative));

    auto shrunk = header;
    shrunk.max = shrunk.num - 1;  // ArrayMax must cover ArrayNum
    EXPECT_FALSE(ce::ue5_registry::IsPlausibleStringHeader(shrunk));

    auto huge = header;
    huge.num = ce::ue5_registry::kMaxNameLength + 1;
    huge.max = huge.num;
    EXPECT_FALSE(ce::ue5_registry::IsPlausibleStringHeader(huge));

    auto nullData = header;
    nullData.data = 0;
    EXPECT_FALSE(ce::ue5_registry::IsPlausibleStringHeader(nullData));

    auto misaligned = header;
    misaligned.data = 0x2001;  // TCHAR data is never odd-aligned
    EXPECT_FALSE(ce::ue5_registry::IsPlausibleStringHeader(misaligned));
}

TEST(UE5ConsoleRegistryTest, MatchesComposedShowFlagNameCaseInsensitively) {
    const std::string name = "ShowFlag.Vignette";
    const auto header = HeaderFor(name);
    const std::vector<uint8_t> bytes = Utf16(name);
    EXPECT_TRUE(ce::ue5_registry::MatchesName(bytes.data(), bytes.size(), header, "ShowFlag.Vignette"));
    EXPECT_TRUE(ce::ue5_registry::MatchesName(bytes.data(), bytes.size(), header, "showflag.vignette"));
    EXPECT_FALSE(ce::ue5_registry::MatchesName(bytes.data(), bytes.size(), header, "ShowFlag.Grain"));
}

TEST(UE5ConsoleRegistryTest, RejectsPrefixAndTruncatedNameMatches) {
    const std::string stored = "ShowFlag.VignetteExtra";
    const auto header = HeaderFor(stored);
    const std::vector<uint8_t> bytes = Utf16(stored);
    // A longer stored name must not satisfy a shorter request: FString carries
    // its length, so the count is checked before the characters.
    EXPECT_FALSE(ce::ue5_registry::MatchesName(bytes.data(), bytes.size(), header, "ShowFlag.Vignette"));

    const std::string exact = "ShowFlag.Vignette";
    const auto exactHeader = HeaderFor(exact);
    const std::vector<uint8_t> exactBytes = Utf16(exact);
    // A buffer that stops short of the terminator is not a match either.
    EXPECT_FALSE(ce::ue5_registry::MatchesName(exactBytes.data(), exactBytes.size() - 2, exactHeader,
                                               "ShowFlag.Vignette"));
}

TEST(UE5ConsoleRegistryTest, RejectsNonAsciiHighByteInStoredName) {
    const std::string name = "ShowFlag.Vignette";
    const auto header = HeaderFor(name);
    std::vector<uint8_t> bytes = Utf16(name);
    bytes[3] = 0x01;  // high byte of the second code unit
    EXPECT_FALSE(ce::ue5_registry::MatchesName(bytes.data(), bytes.size(), header, name.c_str()));
}

TEST(UE5ConsoleRegistryTest, ValueOffsetAndObjectPlausibility) {
    // The tuple layout UE uses puts the object right after the 16-byte FString.
    EXPECT_TRUE(ce::ue5_registry::IsAcceptableValueOffset(16));
    EXPECT_FALSE(ce::ue5_registry::IsAcceptableValueOffset(12));
    EXPECT_FALSE(ce::ue5_registry::IsAcceptableValueOffset(0));

    EXPECT_TRUE(ce::ue5_registry::IsPlausibleConsoleObject(0x0000027B803C6CA8ull & ~uintptr_t{7}));
    EXPECT_FALSE(ce::ue5_registry::IsPlausibleConsoleObject(0));
    EXPECT_FALSE(ce::ue5_registry::IsPlausibleConsoleObject(0x400));      // first page
    EXPECT_FALSE(ce::ue5_registry::IsPlausibleConsoleObject(0x10004));    // unaligned
    EXPECT_FALSE(ce::ue5_registry::IsPlausibleConsoleObject(uintptr_t{1} << 48));
}

TEST(UE5RegistrySweepTest, ResumingAfterAPauseCoversExactlyTheSameChunks) {
    // The regression: Industria 2 (20260815_214219) covered 218 MB of heap in
    // the 400 ms budget, stopped mid-heap with 31 of 34 anchors placed, and the
    // partial result was then frozen and reused for the rest of the session.
    // Splitting a pass must cost nothing but latency, so a paused-and-resumed
    // sweep has to visit the very same chunk offsets a single pass would.
    const ce::ue5_registry::RegionSpan region{0x7FF000000000ull, kChunkBytes * 10 + 1234};

    uintptr_t wholeCursor = 0;
    const std::vector<std::size_t> whole =
        SweepPass(region, wholeCursor, std::numeric_limits<std::size_t>::max());
    ASSERT_FALSE(whole.empty());
    EXPECT_EQ(wholeCursor, region.base + region.size);

    uintptr_t splitCursor = 0;
    std::vector<std::size_t> split;
    for (int pass = 0; pass < 32 && !ce::ue5_registry::RegionAlreadySwept(region, splitCursor); ++pass) {
        const std::vector<std::size_t> chunk = SweepPass(region, splitCursor, 3);
        split.insert(split.end(), chunk.begin(), chunk.end());
    }
    EXPECT_EQ(split, whole);
    EXPECT_EQ(splitCursor, wholeCursor);
}

TEST(UE5RegistrySweepTest, ResumeOffsetParksOnTheUnreadChunkNotThePreviousOne) {
    const ce::ue5_registry::RegionSpan region{0x20000, kChunkBytes * 4};
    const std::size_t stride = kChunkBytes - kChunkOverlap;

    uintptr_t cursor = 0;
    const std::vector<std::size_t> first = SweepPass(region, cursor, 2);
    ASSERT_EQ(first.size(), 2u);
    EXPECT_EQ(first[0], 0u);
    EXPECT_EQ(first[1], stride);
    // Parking on the chunk that was read would skip it on resume; the whole
    // point of the overlap is that the next pass re-enters at the unread one.
    EXPECT_EQ(cursor, region.base + stride * 2);
    EXPECT_EQ(ce::ue5_registry::SweepResumeOffset(region, cursor), stride * 2);
}

TEST(UE5RegistrySweepTest, RegionSkippingAndResumeArithmeticStayInBounds) {
    const ce::ue5_registry::RegionSpan region{0x100000, 0x10000};

    EXPECT_FALSE(ce::ue5_registry::RegionAlreadySwept(region, 0));
    EXPECT_FALSE(ce::ue5_registry::RegionAlreadySwept(region, region.base));
    EXPECT_FALSE(ce::ue5_registry::RegionAlreadySwept(region, region.base + region.size - 1));
    EXPECT_TRUE(ce::ue5_registry::RegionAlreadySwept(region, region.base + region.size));
    EXPECT_TRUE(ce::ue5_registry::RegionAlreadySwept(region, region.base + region.size + 0x1000));

    EXPECT_EQ(ce::ue5_registry::SweepResumeOffset(region, 0), 0u);
    EXPECT_EQ(ce::ue5_registry::SweepResumeOffset(region, region.base), 0u);
    EXPECT_EQ(ce::ue5_registry::SweepResumeOffset(region, region.base + 0x40), 0x40u);
    // Clamped, never past the end, so the caller's loop cannot read outside it.
    EXPECT_EQ(ce::ue5_registry::SweepResumeOffset(region, region.base + region.size + 0x100),
              region.size);

    // A region whose end would overflow must never wrap into a bogus "already
    // swept": that would silently skip memory the sweep never read, which is
    // exactly the coverage claim the closing verdict rests on.
    const ce::ue5_registry::RegionSpan high{std::numeric_limits<uintptr_t>::max() - 0xFFF, 0x1000};
    EXPECT_FALSE(ce::ue5_registry::RegionAlreadySwept(high, high.base));
    // The last byte of the region is still pending, so the cursor sitting on it
    // is not past the region.
    EXPECT_FALSE(ce::ue5_registry::RegionAlreadySwept(high, std::numeric_limits<uintptr_t>::max()));
    EXPECT_EQ(ce::ue5_registry::SweepResumeOffset(high, std::numeric_limits<uintptr_t>::max()), 0xFFFu);
    // base + size wraps to 0 for this region; a wrapped cursor must read as
    // "below every region", never as "past this one".
    EXPECT_FALSE(ce::ue5_registry::RegionAlreadySwept(high, 0));
    EXPECT_EQ(ce::ue5_registry::SweepResumeOffset(high, 0), 0u);
}

TEST(UE5RegistrySweepTest, OnlyAFinishedSweepMayCallANameUnregistered) {
    // The second half of the same regression: with the walk stopped early, the
    // summary still reported the leftovers as "never registered by the engine",
    // which is a conclusion the coverage did not support.
    ce::ue5_registry::SweepProgress paused;
    paused.cursor = 0x7FF000000000ull;
    paused.sweptBytes = 218ull << 20;
    paused.passes = 1;
    EXPECT_FALSE(ce::ue5_registry::SweepProvesAbsence(paused));

    ce::ue5_registry::SweepProgress finished = paused;
    finished.complete = true;
    EXPECT_TRUE(ce::ue5_registry::SweepProvesAbsence(finished));

    ce::ue5_registry::SweepProgress abandoned = paused;
    abandoned.budgetExhausted = true;
    EXPECT_FALSE(ce::ue5_registry::SweepProvesAbsence(abandoned));
}

TEST(UE5RegistrySweepTest, SweepContinuesUntilFinishedOrBounded) {
    constexpr uint32_t kMaxPasses = 32;
    constexpr uint64_t kMaxBytes = 8ull << 30;

    ce::ue5_registry::SweepProgress progress;
    EXPECT_TRUE(ce::ue5_registry::SweepCanContinue(progress, kMaxPasses, kMaxBytes));

    // A pass that found the map but ran out of time must still continue: the
    // old code stopped searching the moment the first element was located.
    progress.passes = 1;
    progress.sweptBytes = 218ull << 20;
    EXPECT_TRUE(ce::ue5_registry::SweepCanContinue(progress, kMaxPasses, kMaxBytes));

    ce::ue5_registry::SweepProgress done = progress;
    done.complete = true;
    EXPECT_FALSE(ce::ue5_registry::SweepCanContinue(done, kMaxPasses, kMaxBytes));

    ce::ue5_registry::SweepProgress outOfPasses = progress;
    outOfPasses.passes = kMaxPasses;
    EXPECT_FALSE(ce::ue5_registry::SweepCanContinue(outOfPasses, kMaxPasses, kMaxBytes));

    ce::ue5_registry::SweepProgress outOfBytes = progress;
    outOfBytes.sweptBytes = kMaxBytes;
    EXPECT_FALSE(ce::ue5_registry::SweepCanContinue(outOfBytes, kMaxPasses, kMaxBytes));

    ce::ue5_registry::SweepProgress gaveUp = progress;
    gaveUp.budgetExhausted = true;
    EXPECT_FALSE(ce::ue5_registry::SweepCanContinue(gaveUp, kMaxPasses, kMaxBytes));
}

TEST(UE5RedirectPlanTest, RecordsTheGamePointerSoRestoreNeverWritesNull) {
    int32_t gameStorage = 7;
    ce::ue5_redirect::Observed observed{};
    observed.dataPointer = &gameStorage;
    observed.pointedValue = 7;
    observed.localGame = 7;
    observed.localRender = 0x3F666666;
    observed.pointedStorageWritable = true;

    const ce::ue5_redirect::Plan plan = ce::ue5_redirect::MakePlan(observed);
    ASSERT_TRUE(ce::ue5_redirect::CanInstall(plan));
    // The regression: a redirect used to be installed while the pointer to put
    // back was left default-initialised, so restoring stored null into the live
    // console object and the engine dereferenced it on the next read.
    EXPECT_NE(plan.restorePointer, nullptr);
    EXPECT_EQ(plan.restorePointer, &gameStorage);
    EXPECT_EQ(plan.restorePointedValue, 7u);
    EXPECT_EQ(plan.restoreLocalGame, 7u);
    EXPECT_EQ(plan.restoreLocalRender, 0x3F666666u);
    EXPECT_TRUE(plan.writeThrough);
}

TEST(UE5RedirectPlanTest, RefusesToInstallWhenTheOriginalPointerIsUnknown) {
    ce::ue5_redirect::Observed observed{};
    observed.pointedValue = 3;
    observed.pointedStorageWritable = true;

    const ce::ue5_redirect::Plan plan = ce::ue5_redirect::MakePlan(observed);
    EXPECT_FALSE(plan.valid);
    EXPECT_FALSE(ce::ue5_redirect::CanInstall(plan));
    EXPECT_EQ(plan.restorePointer, nullptr);
}

TEST(UE5ReferenceRedirectPlanTest, MirrorsAndRecordsBothSlotsOfTheShadowPair) {
    // The regression this encodes: repointing a TAutoConsoleVariable's Ref used
    // to be the whole install (writeThrough=0 for every Ref-redirect CVar in
    // Industria 2 and Talos). That only reaches readers which go back through
    // the wrapper - UE's renderer typically caches the TConsoleVariableData<T>*
    // itself and reads the pair directly, so it kept seeing the game's value
    // while CE's verification happily read CE's own shadow and agreed.
    const uint32_t pairStorage[2] = {1, 1};  // r.SceneColorFringeQuality as shipped
    ce::ue5_redirect::ObservedReference observed{};
    observed.pair = pairStorage;
    observed.gameBits = pairStorage[0];
    observed.renderBits = pairStorage[1];
    observed.pairWritable = true;

    const ce::ue5_redirect::ReferencePlan plan = ce::ue5_redirect::MakeReferencePlan(observed);
    ASSERT_TRUE(ce::ue5_redirect::CanMirror(plan));
    EXPECT_EQ(plan.restorePair, pairStorage);
    // Both slots, not just the game one: the render thread reads the second.
    EXPECT_EQ(plan.restoreGameBits, 1u);
    EXPECT_EQ(plan.restoreRenderBits, 1u);
}

TEST(UE5ReferenceRedirectPlanTest, RefusesToMirrorWhatItCannotHandBack) {
    const uint32_t pairStorage[2] = {4, 4};
    ce::ue5_redirect::ObservedReference readOnly{};
    readOnly.pair = pairStorage;
    readOnly.gameBits = 4;
    readOnly.renderBits = 4;
    readOnly.pairWritable = false;
    const ce::ue5_redirect::ReferencePlan readOnlyPlan = ce::ue5_redirect::MakeReferencePlan(readOnly);
    // Still a valid record of the game's values, just not mirrorable. The
    // redirect itself may proceed; only the mirroring is withheld.
    EXPECT_TRUE(readOnlyPlan.valid);
    EXPECT_FALSE(ce::ue5_redirect::CanMirror(readOnlyPlan));
    EXPECT_EQ(readOnlyPlan.restoreGameBits, 4u);

    ce::ue5_redirect::ObservedReference unknown{};
    unknown.pairWritable = true;
    const ce::ue5_redirect::ReferencePlan unknownPlan = ce::ue5_redirect::MakeReferencePlan(unknown);
    EXPECT_FALSE(unknownPlan.valid);
    EXPECT_FALSE(ce::ue5_redirect::CanMirror(unknownPlan));
    EXPECT_EQ(unknownPlan.restorePair, nullptr);
}

TEST(UE5RedirectPlanTest, SkipsWriteThroughWhenTheGameStorageIsReadOnly) {
    const int32_t constantStorage = 2;
    ce::ue5_redirect::Observed observed{};
    observed.dataPointer = &constantStorage;
    observed.pointedValue = 2;
    observed.pointedStorageWritable = false;

    const ce::ue5_redirect::Plan plan = ce::ue5_redirect::MakePlan(observed);
    ASSERT_TRUE(ce::ue5_redirect::CanInstall(plan));
    // Still fully restorable, just without mirroring into the game's storage.
    EXPECT_FALSE(plan.writeThrough);
    EXPECT_EQ(plan.restorePointer, &constantStorage);
}
