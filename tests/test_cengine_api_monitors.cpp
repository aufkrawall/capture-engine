// The rules of ce_enumerate_monitors (llm-wiki/library/03 "Misc"): argument checks, the capacity and count contract,
// and the id and name rules. Driven through ce::capi::EnumerateMonitors with a fixed listing, so no display is queried.
#include "runtime/api/monitor_list.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {
using ce::monitor_selection::MonitorInfo;

constexpr uint32_t kItemSize = sizeof(ce_monitor_info_t);

MonitorInfo Monitor(std::string id, std::string name, LONG left, LONG top, LONG right, LONG bottom, bool primary) {
    MonitorInfo info;
    info.id = std::move(id);
    info.name = std::move(name);
    info.desktopRect = RECT{left, top, right, bottom};
    info.primary = primary;
    return info;
}

// A listing that stands in for the OS. `queried` records whether the rules asked for it at all.
ce::capi::MonitorListing Listing(const std::vector<MonitorInfo>& monitors, bool* queried = nullptr) {
    // An init-capture: a copy of a const reference would be a const member, whose closure moves by copying.
    return [monitors = std::vector<MonitorInfo>(monitors), queried]() {
        if (queried)
            *queried = true;
        return monitors;
    };
}

// Every byte set, so a field the call wrote is distinguishable from one it left alone.
ce_monitor_info_t Sentinel() {
    ce_monitor_info_t info{};
    std::memset(&info, 0xCD, sizeof(info));
    return info;
}

TEST(CengineApiMonitorsTest, ArgumentsAreCheckedBeforeTheListingRuns) {
    bool queried = false;
    const ce::capi::MonitorListing listing = Listing({Monitor("a", "A", 0, 0, 1, 1, true)}, &queried);
    ce_monitor_info_t items[2];
    uint32_t count = 0;
    EXPECT_EQ(ce::capi::EnumerateMonitors(listing, items, kItemSize - 1, 2, &count), CE_E_INVALID_ARGUMENT);
    EXPECT_EQ(ce::capi::EnumerateMonitors(listing, items, kItemSize + 4, 2, &count), CE_E_INVALID_ARGUMENT);
    EXPECT_EQ(ce::capi::EnumerateMonitors(listing, items, kItemSize, 2, nullptr), CE_E_INVALID_ARGUMENT);
    EXPECT_EQ(ce::capi::EnumerateMonitors(listing, nullptr, kItemSize, 1, &count), CE_E_INVALID_ARGUMENT);
    EXPECT_FALSE(queried) << "a refused call asks the OS for nothing";
}

TEST(CengineApiMonitorsTest, TheCountIsTheTotalAndTheFirstCapacityItemsAreFilledInListingOrder) {
    const ce::capi::MonitorListing listing =
        Listing({Monitor("one", "First", 0, 0, 1920, 1080, true), Monitor("two", "Second", 1920, 0, 3840, 1080, false),
                 Monitor("three", "Third", -1280, 0, 0, 1024, false)});
    ce_monitor_info_t items[3] = {Sentinel(), Sentinel(), Sentinel()};
    uint32_t count = 0;
    ASSERT_EQ(ce::capi::EnumerateMonitors(listing, items, kItemSize, 2, &count), CE_OK);
    EXPECT_EQ(count, 3u) << "the total, even though only two fit";
    EXPECT_STREQ(items[0].id, "one");
    EXPECT_STREQ(items[0].name, "First");
    EXPECT_EQ(items[0].left, 0);
    EXPECT_EQ(items[0].right, 1920);
    EXPECT_EQ(items[0].bottom, 1080);
    EXPECT_EQ(items[0].primary, 1u);
    EXPECT_STREQ(items[1].id, "two");
    EXPECT_EQ(items[1].left, 1920);
    EXPECT_EQ(items[1].primary, 0u);
    EXPECT_EQ(items[2].id[0], static_cast<char>(0xCD)) << "nothing is written beyond capacity";
}

TEST(CengineApiMonitorsTest, ACapacityOfZeroIsACountQueryAndNoMonitorsIsZero) {
    const ce::capi::MonitorListing two = Listing({Monitor("one", "First", 0, 0, 1, 1, true), Monitor("two", "Second", 0, 0, 1, 1, false)});
    uint32_t count = 99;
    ASSERT_EQ(ce::capi::EnumerateMonitors(two, nullptr, kItemSize, 0, &count), CE_OK);
    EXPECT_EQ(count, 2u);

    const ce::capi::MonitorListing none = Listing({});
    count = 99;
    ASSERT_EQ(ce::capi::EnumerateMonitors(none, nullptr, kItemSize, 0, &count), CE_OK);
    EXPECT_EQ(count, 0u);
}

TEST(CengineApiMonitorsTest, AnIdThatFitsIsKeptWholeAndOneThatDoesNotFitIsSkippedNotCut) {
    const std::string fits(255, 'x');     // id[256]: 255 characters and the terminator
    const std::string tooLong(256, 'y');  // one character too many: skipped, never returned as a prefix
    const ce::capi::MonitorListing listing =
        Listing({Monitor(tooLong, "Skipped", 0, 0, 1, 1, false), Monitor(fits, "Kept", 0, 0, 1, 1, true),
                 Monitor("after", "Also kept", 0, 0, 1, 1, false)});
    ce_monitor_info_t items[3] = {Sentinel(), Sentinel(), Sentinel()};
    uint32_t count = 0;
    ASSERT_EQ(ce::capi::EnumerateMonitors(listing, items, kItemSize, 3, &count), CE_OK);
    ASSERT_EQ(count, 2u) << "the skipped monitor is not counted";
    EXPECT_EQ(std::string(items[0].id), fits);
    EXPECT_STREQ(items[1].id, "after");
    EXPECT_STREQ(items[1].name, "Also kept");
}

TEST(CengineApiMonitorsTest, ANameLongerThanItsFieldIsCutAtACharacterBoundary) {
    // name[128] holds 127 bytes and the terminator. Two-byte characters (U+00E9) cut at 127 bytes would leave half a
    // character, so the cut backs up to 126 bytes.
    const std::string ascii(200, 'n');
    std::string twoByte;
    for (int i = 0; i < 100; ++i)
        twoByte += "\xC3\xA9";
    const ce::capi::MonitorListing listing =
        Listing({Monitor("a", ascii, 0, 0, 1, 1, true), Monitor("b", twoByte, 0, 0, 1, 1, false)});
    ce_monitor_info_t items[2] = {Sentinel(), Sentinel()};
    uint32_t count = 0;
    ASSERT_EQ(ce::capi::EnumerateMonitors(listing, items, kItemSize, 2, &count), CE_OK);
    ASSERT_EQ(count, 2u);
    EXPECT_EQ(std::strlen(items[0].name), 127u);
    EXPECT_EQ(std::strlen(items[1].name), 126u);
    EXPECT_EQ(std::string(items[1].name), twoByte.substr(0, 126));
}

}  // namespace
