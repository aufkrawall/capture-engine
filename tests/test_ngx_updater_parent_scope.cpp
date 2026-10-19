#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "common/graphics/ngx_ota_policy.h"
#include "source_fragment_reader.h"

// Regression: with ngx_ota=off the injector terminated every nvngx_update.exe
// on the machine by image name - on process start, at every target launch
// sweep, and on the startup scan - so a game CE never injected lost its NGX
// updates while CaptureEngine ran, and the off/on answer came from whichever
// profile was published last, even after that game had exited. The host kill
// is a backstop for CE's own targets only; the in-process refusal stays the
// mechanism inside them.
TEST(NgxUpdaterParentScope, HostTerminatesOnlyUpdatersOfTargets) {
    EXPECT_TRUE(ce::ngx_ota::ShouldHostTerminateUpdater(kNgxOtaModeOff, /*parentIsTarget=*/true));
    EXPECT_FALSE(ce::ngx_ota::ShouldHostTerminateUpdater(kNgxOtaModeOff, /*parentIsTarget=*/false))
        << "an updater of an application CE does not target is not CE's to stop";
    EXPECT_FALSE(ce::ngx_ota::ShouldHostTerminateUpdater(kNgxOtaModeOn, true));
    EXPECT_FALSE(ce::ngx_ota::ShouldHostTerminateUpdater(kNgxOtaModeDefault, true));
}

TEST(NgxUpdaterParentScope, TerminationIsDecidedOnTheUpdatersOwnParent) {
    const std::string watchdog = ce::test_source::ReadLogicalSource(
        std::filesystem::current_path() / "captureengine" / "injection" / "injection_ota_watchdog.cpp");
    ASSERT_FALSE(watchdog.empty());

    const size_t open = watchdog.find("OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION");
    const size_t parent = watchdog.find("QueryUpdaterParent(hProcess)");
    const size_t gate = watchdog.find("ShouldHostTerminateUpdater(");
    const size_t terminate = watchdog.find("TerminateProcess(hProcess");
    ASSERT_NE(open, std::string::npos);
    ASSERT_NE(parent, std::string::npos);
    ASSERT_NE(gate, std::string::npos);
    ASSERT_NE(terminate, std::string::npos);
    EXPECT_LT(open, parent);
    EXPECT_LT(parent, gate);
    EXPECT_LT(gate, terminate) << "nothing is terminated before its parent was proven to be a target";

    // A reused parent PID names a different process; it must not decide.
    EXPECT_NE(watchdog.find("ParentPredatesChild("), std::string::npos);
    EXPECT_NE(watchdog.find("IsWhitelisted(parent.name)"), std::string::npos);
    EXPECT_NE(watchdog.find("is not a CaptureEngine target"), std::string::npos) << "a spared updater is logged";
}
