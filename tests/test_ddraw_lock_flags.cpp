#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "source_fragment_reader.h"

// DDLOCK_NOSYSLOCK is not an optimization. Without it a DDLOCK_WAIT lock takes
// the Win16 lock, and every DirectDraw lock CE performs runs on the
// application's render thread, inside its present, in a process that also hosts
// a third-party overlay and a message pump. Gothic II session 20260916_005504
// is that shape: CE's composite lock started returning E_FAIL, CE retried
// without the flag, and the render thread never came back out of the
// presentation a second later.
//
// These paths used to "progressively" drop the flag for drivers that reject it.
// A lock CE cannot take without the system lock is a frame CE does not
// composite and does not capture; both call sites already handle failure that
// way. This test exists so the fallback cannot come back as a compatibility fix.
namespace {

void ExpectEveryDirectDrawLockKeepsNoSysLock(const char* directory, const char* fileName) {
    const std::filesystem::path source = std::filesystem::current_path() / directory / fileName;
    const std::string contents = ce::test_source::ReadLogicalSource(source);
    ASSERT_FALSE(contents.empty()) << source.string();

    size_t searchFrom = 0;
    size_t found = 0;
    while (true) {
        const size_t match = contents.find("DDLOCK_WAIT", searchFrom);
        if (match == std::string::npos) {
            break;
        }
        searchFrom = match + 1;

        // Every flag set in these units is written on one line, so that whole
        // line is the expression NOSYSLOCK has to appear in. Prose naming the
        // flags - the comments above each lock - is not a lock.
        const size_t previousNewline = contents.rfind('\n', match);
        const size_t lineBegin = previousNewline == std::string::npos ? 0 : previousNewline + 1;
        const size_t lineEnd = contents.find('\n', match);
        const std::string line = contents.substr(lineBegin, lineEnd - lineBegin);
        if (line.find("//") != std::string::npos || line.find("DDLOCK_SURFACEMEMORYPTR") == std::string::npos) {
            continue;
        }

        EXPECT_NE(line.find("DDLOCK_NOSYSLOCK"), std::string::npos)
            << source.string() << " has a DDLOCK_WAIT lock without DDLOCK_NOSYSLOCK: " << line;
        ++found;
    }
    EXPECT_GT(found, 0u) << source.string() << " no longer contains a DirectDraw lock";
}

}  // namespace

TEST(DDrawLockFlagsTest, TheOverlayCompositeNeverTakesTheWin16Lock) {
    ExpectEveryDirectDrawLockKeepsNoSysLock("hook/apis", "ddraw_hook_overlay_composite.cpp");
}

TEST(DDrawLockFlagsTest, CaptureNeverTakesTheWin16Lock) {
    ExpectEveryDirectDrawLockKeepsNoSysLock("hook/apis", "ddraw_hook_capture_frame.cpp");
}
