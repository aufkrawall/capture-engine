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

// Gothic II session 20260916_011148: CE and Steam's gameoverlayrenderer each
// hooked IDirectDrawSurface7's Flip slot, in an order that left each holding
// the other's detour as "the original". Calling it recursed - three frames
// repeating down eight megabytes of the render thread's stack, each level
// running a full 4K overlay composite. From outside that looked like a game
// presenting 3,600 times at ~270/s for thirteen seconds; not one flip ever
// completed, which is why nothing reached the screen.
//
// Every presentation detour must therefore refuse to re-enter itself on one
// thread, and must break out through an entry point that provably belongs to
// DirectDraw rather than through the saved original that leads back.
TEST(DDrawLockFlagsTest, EveryPresentationDetourRefusesToReenterItself) {
    const std::filesystem::path source =
        std::filesystem::current_path() / "hook/apis" / "ddraw_hook_detours.cpp";
    const std::string contents = ce::test_source::ReadLogicalSource(source);
    ASSERT_FALSE(contents.empty()) << source.string();

    // Three Flip detours (legacy/4/7) and six blit presentation detours.
    size_t scopes = 0;
    for (size_t at = contents.find("DirectDrawPresentDetourScope presentDetourScope");
         at != std::string::npos;
         at = contents.find("DirectDrawPresentDetourScope presentDetourScope", at + 1)) {
        ++scopes;
    }
    EXPECT_EQ(scopes, 9u) << "a presentation detour lost its re-entry guard";

    size_t refusals = 0;
    for (size_t at = contents.find("RefuseReenteredPresentation(surface"); at != std::string::npos;
         at = contents.find("RefuseReenteredPresentation(surface", at + 1)) {
        ++refusals;
    }
    EXPECT_EQ(refusals, 9u) << "a re-entry guard stopped refusing the nested presentation";

    // Session 20260916_013230: answering the cycle by calling another function -
    // even one captured before CE patched the slot and validated as ddraw-owned -
    // did not end it. Returning without calling anything is the only response
    // that cannot recurse, so no guard may call through a function pointer.
    EXPECT_EQ(contents.find("AcquireDirectDrawPresentCycleEscape"), std::string::npos)
        << "a re-entry guard is calling through a pointer again";
}

// The escape is only an escape if it belongs to DirectDraw. Another overlay's
// detour is exactly what must not be re-entered, so the recorded entry points
// are validated against ddraw.dll's module range before they are kept.
TEST(DDrawLockFlagsTest, TheCycleEscapeOnlyAcceptsDirectDrawOwnedCode) {
    const std::filesystem::path source =
        std::filesystem::current_path() / "hook/apis" / "ddraw_hook_present_reentry.cpp";
    const std::string contents = ce::test_source::ReadLogicalSource(source);
    ASSERT_FALSE(contents.empty()) << source.string();

    EXPECT_NE(contents.find("IsDirectDrawOwnedCode"), std::string::npos);
    EXPECT_NE(contents.find("GetModuleHandleA(\"ddraw.dll\")"), std::string::npos);
    EXPECT_NE(contents.find("GetModuleInformation"), std::string::npos);
    // The recursion cannot be attributed from the fact of it alone, so the
    // report has to name the module that re-entered CE.
    EXPECT_NE(contents.find("DescribeCodeAddress"), std::string::npos);
    // The snapshot has to happen before CE patches the slot, or it records CE.
    const std::string installer =
        ce::test_source::ReadLogicalSource(std::filesystem::current_path() / "hook/apis" / "ddraw_hook_install.cpp");
    ASSERT_FALSE(installer.empty());
    const size_t record = installer.find("RecordDirectDrawPresentEntryPoints(surfaceVTable)");
    const size_t patch = installer.find("VTableHook::Create", record);
    EXPECT_NE(record, std::string::npos);
    EXPECT_NE(patch, std::string::npos);
    EXPECT_LT(record, patch);
}
