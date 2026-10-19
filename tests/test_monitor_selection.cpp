#include <gtest/gtest.h>

#include "common/platform/monitor_selection.h"

namespace monitor = ce::monitor_selection;

TEST(MonitorSelectionTest, ParsesDocumentedSelectorsAndPreservesStableId) {
    monitor::Selector selector;
    ASSERT_TRUE(monitor::TryParseSelector(" AUTO ", selector));
    EXPECT_EQ(selector.kind, monitor::SelectorKind::kAuto);
    EXPECT_EQ(selector.canonical, "auto");

    ASSERT_TRUE(monitor::TryParseSelector("Primary", selector));
    EXPECT_EQ(selector.kind, monitor::SelectorKind::kPrimary);
    ASSERT_TRUE(monitor::TryParseSelector("window", selector));
    EXPECT_EQ(selector.kind, monitor::SelectorKind::kWindow);
    ASSERT_TRUE(monitor::TryParseSelector("CURSOR", selector));
    EXPECT_EQ(selector.kind, monitor::SelectorKind::kCursor);

    ASSERT_TRUE(monitor::TryParseSelector("id:\\\\?\\DISPLAY#Acme123#{Monitor-Guid}", selector));
    EXPECT_EQ(selector.kind, monitor::SelectorKind::kStableId);
    EXPECT_EQ(selector.stableId, "\\\\?\\DISPLAY#Acme123#{Monitor-Guid}");
    EXPECT_EQ(selector.canonical, "id:\\\\?\\DISPLAY#Acme123#{Monitor-Guid}");
    EXPECT_TRUE(monitor::IsExplicitSelector(selector));

    EXPECT_FALSE(monitor::TryParseSelector("id:", selector));
    EXPECT_FALSE(monitor::TryParseSelector("display-2", selector));
}

TEST(MonitorSelectionTest, AutoCandidatePrecedenceIsDeterministic) {
    const HMONITOR targetWindow = reinterpret_cast<HMONITOR>(static_cast<uintptr_t>(1));
    const HMONITOR hint = reinterpret_cast<HMONITOR>(static_cast<uintptr_t>(2));
    const HMONITOR foreground = reinterpret_cast<HMONITOR>(static_cast<uintptr_t>(3));
    const HMONITOR primary = reinterpret_cast<HMONITOR>(static_cast<uintptr_t>(4));
    monitor::CandidateHandles candidates{targetWindow, hint, foreground, nullptr, primary};

    monitor::CandidateChoice choice = monitor::ChooseCandidate(monitor::SelectorKind::kAuto, candidates);
    EXPECT_EQ(choice.monitor, targetWindow);
    EXPECT_STREQ(choice.reason, "target-window");

    candidates.targetWindow = nullptr;
    choice = monitor::ChooseCandidate(monitor::SelectorKind::kAuto, candidates);
    EXPECT_EQ(choice.monitor, hint);
    candidates.hint = nullptr;
    choice = monitor::ChooseCandidate(monitor::SelectorKind::kAuto, candidates);
    EXPECT_EQ(choice.monitor, foreground);
    candidates.foregroundWindow = nullptr;
    choice = monitor::ChooseCandidate(monitor::SelectorKind::kAuto, candidates);
    EXPECT_EQ(choice.monitor, primary);
    EXPECT_STREQ(choice.reason, "primary-fallback");
}

TEST(MonitorSelectionTest, ExplicitCandidateKindsNeverUseAnotherCandidate) {
    const HMONITOR window = reinterpret_cast<HMONITOR>(static_cast<uintptr_t>(1));
    const HMONITOR cursor = reinterpret_cast<HMONITOR>(static_cast<uintptr_t>(2));
    const HMONITOR primary = reinterpret_cast<HMONITOR>(static_cast<uintptr_t>(3));
    monitor::CandidateHandles candidates{window, nullptr, nullptr, cursor, primary};

    EXPECT_EQ(monitor::ChooseCandidate(monitor::SelectorKind::kWindow, candidates).monitor, window);
    EXPECT_EQ(monitor::ChooseCandidate(monitor::SelectorKind::kCursor, candidates).monitor, cursor);
    EXPECT_EQ(monitor::ChooseCandidate(monitor::SelectorKind::kPrimary, candidates).monitor, primary);

    candidates.targetWindow = nullptr;
    EXPECT_EQ(monitor::ChooseCandidate(monitor::SelectorKind::kWindow, candidates).monitor, nullptr);
    candidates.cursor = nullptr;
    EXPECT_EQ(monitor::ChooseCandidate(monitor::SelectorKind::kCursor, candidates).monitor, nullptr);
}

TEST(MonitorSelectionTest, StableIdsMatchCaseInsensitivelyAndListAsConfigValues) {
    monitor::MonitorDescriptor first;
    first.handle = reinterpret_cast<HMONITOR>(static_cast<uintptr_t>(1));
    first.stableId = "\\\\?\\DISPLAY#Acme123#{Monitor-Guid}";
    first.deviceName = "\\\\.\\DISPLAY2";
    first.friendlyName = "Acme Panel";
    first.desktopRect = RECT{1920, 0, 4480, 1440};
    first.hasDisplayConfigIdentity = true;
    std::vector<monitor::MonitorDescriptor> monitors{first};

    const monitor::MonitorDescriptor* found =
        monitor::FindByStableId(monitors, "\\\\?\\display#acme123#{monitor-guid}");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->handle, first.handle);

    const std::string list = monitor::FormatMonitorList({monitor::ToMonitorInfo(first)});
    EXPECT_NE(list.find("Acme Panel"), std::string::npos);
    EXPECT_NE(list.find("bounds=1920,0 2560x1440"), std::string::npos);
    EXPECT_NE(list.find("monitor=id:\\\\?\\DISPLAY#Acme123#{Monitor-Guid}"), std::string::npos);
}

TEST(MonitorSelectionTest, GdiDeviceNamesMatchCaseInsensitivelyAndRejectsNulls) {
    EXPECT_TRUE(monitor::GdiDeviceNamesMatch(L"\\\\.\\DISPLAY2", L"\\\\.\\display2"));
    EXPECT_FALSE(monitor::GdiDeviceNamesMatch(L"\\\\.\\DISPLAY2", L"\\\\.\\DISPLAY3"));
    EXPECT_FALSE(monitor::GdiDeviceNamesMatch(nullptr, L"\\\\.\\DISPLAY2"));
    EXPECT_FALSE(monitor::GdiDeviceNamesMatch(L"\\\\.\\DISPLAY2", nullptr));
}

TEST(MonitorSelectionTest, SdrWhiteNitsClampToTheWindowsRange) {
    EXPECT_FLOAT_EQ(monitor::SdrWhiteNitsFromLevel(1000), 80.0f);
    EXPECT_FLOAT_EQ(monitor::SdrWhiteNitsFromLevel(2538), 203.04f);
    EXPECT_FLOAT_EQ(monitor::SdrWhiteNitsFromLevel(0), 80.0f);
    EXPECT_FLOAT_EQ(monitor::SdrWhiteNitsFromLevel(65535), 1000.0f);
}

namespace {
// A DisplayConfig-backed primary display plus a GDI-fallback display left of it with a negative origin.
std::vector<monitor::MonitorDescriptor> GoldenMonitors() {
    monitor::MonitorDescriptor primary;
    primary.stableId = "\\\\?\\DISPLAY#Acme123#{Monitor-Guid}";
    primary.deviceName = "\\\\.\\DISPLAY1";
    primary.friendlyName = "Acme Panel";
    primary.desktopRect = RECT{0, 0, 2560, 1440};
    primary.primary = true;
    primary.adapterLuid.LowPart = 0xABCD;
    primary.adapterLuid.HighPart = 0;
    primary.sourceId = 0;
    primary.targetId = 4353;
    primary.hasDisplayConfigIdentity = true;

    monitor::MonitorDescriptor fallback;
    fallback.stableId = "gdi:\\\\.\\DISPLAY2";
    fallback.deviceName = "\\\\.\\DISPLAY2";
    fallback.friendlyName = fallback.deviceName;
    fallback.desktopRect = RECT{-1920, 120, 0, 1200};
    return {primary, fallback};
}
}  // namespace

TEST(MonitorSelectionTest, ListingTextIsByteStable) {
    const std::string expected =
        "CaptureEngine active monitors: 2\r\n"
        "\r\n"
        "[1] Acme Panel (primary)\r\n"
        "  device=\\\\.\\DISPLAY1\r\n"
        "  bounds=0,0 2560x1440\r\n"
        "  monitor=id:\\\\?\\DISPLAY#Acme123#{Monitor-Guid}\r\n"
        "  adapter_luid=0:abcd source=0 target=4353\r\n"
        "\r\n"
        "[2] \\\\.\\DISPLAY2\r\n"
        "  device=\\\\.\\DISPLAY2\r\n"
        "  bounds=-1920,120 1920x1080\r\n"
        "  monitor=id:gdi:\\\\.\\DISPLAY2\r\n"
        "  identity_warning=DisplayConfig path unavailable; this GDI fallback ID may change\r\n";
    std::vector<monitor::MonitorInfo> listing;
    for (const monitor::MonitorDescriptor& descriptor : GoldenMonitors())
        listing.push_back(monitor::ToMonitorInfo(descriptor));
    EXPECT_EQ(monitor::FormatMonitorList(listing), expected);
}

TEST(MonitorSelectionTest, ListingEntryCarriesTheFieldsThePublicRecordReports) {
    const std::vector<monitor::MonitorDescriptor> descriptors = GoldenMonitors();
    const monitor::MonitorInfo primary = monitor::ToMonitorInfo(descriptors[0]);
    EXPECT_EQ(primary.id, "\\\\?\\DISPLAY#Acme123#{Monitor-Guid}");  // without the `id:` the config value adds
    EXPECT_EQ(primary.name, "Acme Panel");
    EXPECT_EQ(primary.desktopRect.right, 2560);
    EXPECT_TRUE(primary.primary);
    EXPECT_TRUE(primary.hasDisplayConfigIdentity);
    const monitor::MonitorInfo fallback = monitor::ToMonitorInfo(descriptors[1]);
    EXPECT_EQ(fallback.id, "gdi:\\\\.\\DISPLAY2");
    EXPECT_EQ(fallback.name, "\\\\.\\DISPLAY2");  // no friendly name: the GDI device name stands in
    EXPECT_EQ(fallback.desktopRect.left, -1920);
    EXPECT_FALSE(fallback.primary);
    EXPECT_FALSE(fallback.hasDisplayConfigIdentity);
    // Both ids are accepted as the `monitor=` config value and select exactly their own display.
    for (const monitor::MonitorInfo& info : {primary, fallback}) {
        monitor::Selector selector;
        ASSERT_TRUE(monitor::TryParseSelector("id:" + info.id, selector)) << info.id;
        EXPECT_EQ(selector.kind, monitor::SelectorKind::kStableId);
        EXPECT_EQ(selector.stableId, info.id);
        EXPECT_EQ(selector.canonical, "id:" + info.id);
        const monitor::MonitorDescriptor* found = monitor::FindByStableId(descriptors, selector.stableId);
        ASSERT_NE(found, nullptr) << info.id;
        EXPECT_EQ(found->stableId, info.id);
    }
}

TEST(MonitorSelectionTest, ListedMonitorsOfThisMachineResolveFromTheirOwnIds) {
    const std::vector<monitor::MonitorInfo> listing = monitor::ListMonitors();  // may be empty without a display
    size_t primaries = 0;
    for (size_t index = 0; index < listing.size(); ++index) {
        const monitor::MonitorInfo& info = listing[index];
        SCOPED_TRACE(info.id);
        EXPECT_FALSE(info.id.empty());
        EXPECT_FALSE(info.name.empty());
        EXPECT_GT(info.desktopRect.right, info.desktopRect.left);
        EXPECT_GT(info.desktopRect.bottom, info.desktopRect.top);
        primaries += info.primary ? 1 : 0;
        if (info.primary)
            EXPECT_EQ(index, 0u);  // the primary display is listed first
        for (size_t other = index + 1; other < listing.size(); ++other)
            EXPECT_NE(info.id, listing[other].id);
        monitor::ResolveRequest request;
        ASSERT_TRUE(monitor::TryParseSelector("id:" + info.id, request.selector));
        const monitor::ResolveResult resolved = monitor::Resolve(request);
        ASSERT_TRUE(static_cast<bool>(resolved)) << resolved.error;
        EXPECT_EQ(resolved.descriptor.stableId, info.id);
        EXPECT_EQ(resolved.reason, "configured-stable-id");
    }
    EXPECT_LE(primaries, 1u);
}

TEST(MonitorSelectionTest, EmptyListingStatesThatNoMonitorWasFound) {
    EXPECT_EQ(monitor::FormatMonitorList({}),
              "CaptureEngine active monitors: 0\r\nNo active monitors were found.\r\n");
}
