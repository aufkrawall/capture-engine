#include <gtest/gtest.h>

#include "../common/monitor_selection.h"

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

    const std::string list = monitor::FormatMonitorList(monitors);
    EXPECT_NE(list.find("Acme Panel"), std::string::npos);
    EXPECT_NE(list.find("bounds=1920,0 2560x1440"), std::string::npos);
    EXPECT_NE(list.find("monitor=id:\\\\?\\DISPLAY#Acme123#{Monitor-Guid}"), std::string::npos);
}
