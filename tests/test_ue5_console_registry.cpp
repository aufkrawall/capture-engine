#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
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
