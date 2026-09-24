#include <gtest/gtest.h>

#include <windows.h>

#include <cstring>
#include <filesystem>
#include <string>

#include "../common/crash_dump_policy.h"
#include "../hook/common/freeze_watchdog.h"
#include "../hook/common/window_text_safe.h"
#include "source_fragment_reader.h"

// A title that reports its own fatal error in a modal box on the render thread
// and then quits with exit code 0 used to leave no dump at all. Gothic II
// session 20260924_233030: `Error-Message` on the render thread, dismissed
// after under three seconds, ExitProcess(0) - the dialog dump waits five
// seconds for a stale heartbeat and the pre-termination dump skipped code 0.
namespace watchdog_policy = ce::freeze_watchdog_policy;
namespace dump_policy = ce::crash_dump_policy;

TEST(RenderThreadDialogExitTest, OnlyAnExitWithNoPresentationSinceTheDialogQualifies) {
    constexpr uint64_t kHeartbeatAtDialog = 1'000'000;
    EXPECT_TRUE(watchdog_policy::TerminationFollowsRenderThreadDialog(true, kHeartbeatAtDialog, kHeartbeatAtDialog));
    EXPECT_FALSE(watchdog_policy::TerminationFollowsRenderThreadDialog(true, kHeartbeatAtDialog,
                                                                       kHeartbeatAtDialog + 16'667))
        << "the game dismissed the box and rendered on; a later quit is a quit";
    EXPECT_FALSE(watchdog_policy::TerminationFollowsRenderThreadDialog(false, 0, 0))
        << "no dialog seen - a title that never presented is not an error report";
}

TEST(RenderThreadDialogExitTest, AnErrorReportDumpsEvenWithExitCodeZero) {
    constexpr auto kOrigin = dump_policy::TerminationOrigin::kPrimaryModule;
    EXPECT_TRUE(dump_policy::ShouldCapturePreTerminationDump(true, 0, false, false, kOrigin, false, true));
    EXPECT_FALSE(dump_policy::ShouldCapturePreTerminationDump(true, 0, false, false, kOrigin, false, false))
        << "an ordinary clean quit still writes nothing";
    EXPECT_FALSE(dump_policy::ShouldCapturePreTerminationDump(false, 0, false, false, kOrigin, false, true))
        << "terminating another process";
    EXPECT_FALSE(dump_policy::ShouldCapturePreTerminationDump(true, 0, true, false, kOrigin, false, true))
        << "one pre-termination dump per process";
    EXPECT_FALSE(dump_policy::ShouldCapturePreTerminationDump(true, dump_policy::kProcessIsTerminatingExitCode,
                                                              false, false, kOrigin, false, true));
}

TEST(RenderThreadDialogExitTest, DialogTextBecomesOneBoundedLogLine) {
    char buffer[40] = {};
    ce::window_text::AppendDialogTextFragment(buffer, sizeof(buffer), "D3D: Primary\r\nsurface failed");
    EXPECT_STREQ(buffer, "D3D: Primary surface failed");
    ce::window_text::AppendDialogTextFragment(buffer, sizeof(buffer), "");
    EXPECT_STREQ(buffer, "D3D: Primary surface failed") << "an empty control adds no separator";
    ce::window_text::AppendDialogTextFragment(buffer, sizeof(buffer), "Retry later");
    EXPECT_EQ(std::strlen(buffer), sizeof(buffer) - 1) << "truncated at the buffer end";
    EXPECT_EQ(std::string(buffer).rfind("D3D: Primary surface failed | Re", 0), 0u);

    char tiny[1] = {'x'};
    ce::window_text::AppendDialogTextFragment(tiny, sizeof(tiny), "text");
    EXPECT_EQ(tiny[0], '\0') << "a buffer with no room for text is still terminated";
}

TEST(RenderThreadDialogExitTest, WatchdogAndFatalExitAreWiredTogether) {
    const auto root = std::filesystem::current_path();
    const std::string watchdog = ce::test_source::ReadFile(root / "hook/common/freeze_watchdog.cpp");
    const size_t note = watchdog.find("NoteRenderThreadDialog(dialogInfo.hwnd");
    ASSERT_NE(note, std::string::npos) << "the watchdog no longer records render-thread dialogs";
    const size_t guard = watchdog.rfind("FreezeIsExplainedByApplicationDialog(", note);
    ASSERT_NE(guard, std::string::npos);
    EXPECT_LT(note - guard, 300u) << "only a dialog the monitored render thread owns may qualify";

    const std::string fatal = ce::test_source::ReadFile(root / "hook/main_fatal_dump.cpp");
    const size_t query = fatal.find("g_RenderWatchdog.TerminationFollowsRenderThreadDialog()");
    const size_t decision = fatal.find("followsUnresolvedFault, followsRenderThreadDialog)");
    ASSERT_NE(query, std::string::npos);
    ASSERT_NE(decision, std::string::npos) << "the pre-termination policy never sees the dialog evidence";
    EXPECT_LT(query, decision);
}
