#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "../hook/common/ddraw_present_policy.h"
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

// Every DirectDraw lock CE performs is a ->Lock( call site somewhere under
// hook/ - overlay composite, capture, and the D3D7 font atlas alike - so the
// scan is the whole tree, not two remembered files. Each unit is read exactly
// once: the logical reader re-emits sibling units for every stem and fragment
// files for every wrapper, which would double-count call sites in an
// exhaustive walk (ReadFile from the same header normalizes line endings the
// same way).
std::vector<std::filesystem::path> EnumerateHookSources(const std::filesystem::path& hookRoot) {
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(hookRoot)) {
        if (!entry.is_regular_file())
            continue;
        const std::string extension = entry.path().extension().string();
        if (extension == ".cpp" || extension == ".h" || extension == ".hpp" || extension == ".c" ||
            extension == ".inl") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

// The ->Lock( argument list up to its closing paren, so a flag expression
// wrapped over several lines is judged as one call.
std::string LockCallArguments(const std::string& contents, size_t call) {
    const size_t open = contents.find('(', call);
    if (open == std::string::npos)
        return {};
    size_t depth = 0;
    for (size_t at = open; at < contents.size(); ++at) {
        if (contents[at] == '(') {
            ++depth;
        } else if (contents[at] == ')' && --depth == 0) {
            return contents.substr(open, at - open + 1);
        }
    }
    return {};
}

// Everything between the end of the previous function and the call: where an
// indirect call site's flags have to be sourced to be judged at all.
std::string TextSinceLastFunctionBoundary(const std::string& contents, size_t at) {
    const size_t boundary = contents.rfind("\n}", at);
    const size_t begin = boundary == std::string::npos ? 0 : boundary + 2;
    return contents.substr(begin, at - begin);
}

size_t CountOccurrences(const std::string& contents, const char* needle) {
    size_t count = 0;
    for (size_t at = contents.find(needle); at != std::string::npos; at = contents.find(needle, at + 1))
        ++count;
    return count;
}

}  // namespace

TEST(DDrawLockFlagsTest, EveryDirectDrawLockCallSiteUnderHookKeepsNoSysLock) {
    const std::filesystem::path hookRoot = std::filesystem::current_path() / "hook";
    size_t directDrawSites = 0;
    size_t totalSites = 0;
    for (const std::filesystem::path& file : EnumerateHookSources(hookRoot)) {
        const std::string contents = ce::test_source::ReadFile(file);
        if (contents.empty())
            continue;
        for (size_t at = contents.find("->Lock("); at != std::string::npos;
             at = contents.find("->Lock(", at + 1)) {
            ++totalSites;

            // Prose naming a lock is not a lock.
            const size_t previousNewline = contents.rfind('\n', at);
            const size_t lineBegin = previousNewline == std::string::npos ? 0 : previousNewline + 1;
            if (contents.substr(lineBegin, at - lineBegin).find("//") != std::string::npos)
                continue;

            const std::string call = LockCallArguments(contents, at);
            if (call.find("DDLOCK_") != std::string::npos) {
                // The flags are on the call itself (possibly wrapped): the call
                // text is the expression NOSYSLOCK has to appear in.
                ++directDrawSites;
                EXPECT_NE(call.find("DDLOCK_NOSYSLOCK"), std::string::npos)
                    << file.string() << " has a DirectDraw ->Lock( without DDLOCK_NOSYSLOCK: " << call;
                continue;
            }
            if (call.find("D3DLOCK_") != std::string::npos)
                continue;  // D3D8/DX9 vertex or index buffer lock, not DirectDraw

            // Flags arrive through a named constant or loop variable
            // (LockDirectDrawSurfaceForRead's kLockAttempts): the function text
            // above the call has to show where its NOSYSLOCK comes from.
            ++directDrawSites;
            EXPECT_NE(TextSinceLastFunctionBoundary(contents, at).find("DDLOCK_NOSYSLOCK"), std::string::npos)
                << file.string() << " has a ->Lock( whose indirect flags never show DDLOCK_NOSYSLOCK";
        }
    }
    EXPECT_GE(directDrawSites, 5u) << "the hook tree no longer holds the DirectDraw locks this rule covers";
    EXPECT_GE(totalSites, 9u) << "the ->Lock( scan silently found almost nothing";
}

TEST(DDrawLockFlagsTest, EveryWaitLockFlagExpressionUnderHookKeepsNoSysLock) {
    // The flag sets themselves, wherever they live: a DDLOCK_WAIT expression
    // written on one line (every flag set CE writes is written on one line, so
    // that whole line is the expression NOSYSLOCK has to appear in) must carry
    // DDLOCK_NOSYSLOCK, so a named constant a ->Lock( call site merely refers
    // to cannot smuggle a Win16-lock-taking lock past the call-site rule.
    const std::filesystem::path hookRoot = std::filesystem::current_path() / "hook";
    size_t flagSets = 0;
    for (const std::filesystem::path& file : EnumerateHookSources(hookRoot)) {
        const std::string contents = ce::test_source::ReadFile(file);
        size_t searchFrom = 0;
        while (true) {
            const size_t match = contents.find("DDLOCK_WAIT", searchFrom);
            if (match == std::string::npos)
                break;
            searchFrom = match + 1;

            // Prose naming the flags - the comments above each lock - is not a
            // flag set.
            const size_t previousNewline = contents.rfind('\n', match);
            const size_t lineBegin = previousNewline == std::string::npos ? 0 : previousNewline + 1;
            const size_t lineEnd = contents.find('\n', match);
            const std::string line = contents.substr(lineBegin, lineEnd - lineBegin);
            if (line.find("//") != std::string::npos)
                continue;

            ++flagSets;
            EXPECT_NE(line.find("DDLOCK_NOSYSLOCK"), std::string::npos)
                << file.string() << " has a DDLOCK_WAIT flag set without DDLOCK_NOSYSLOCK: " << line;
        }
    }
    EXPECT_GE(flagSets, 5u) << "the hook tree no longer holds the flag sets this rule covers";
}

// One failed Unlock used to leak lock-tracking depth: the Unlock detours ran
// CompleteDirectDrawSurfaceLock only on SUCCEEDED(hr), `--depth != 0` then
// deferred every later DirectScanout presentation, and both the overlay and
// the freeze-watchdog heartbeat went silent for the session while the game ran
// on. An application Lock/Unlock imbalance on a driver that accepted
// overlapping locks stuck the same way. DirectDraw permits one lock at a time,
// so every Unlock attempt resolves the surface's whole track.
TEST(DDrawLockFlagsTest, AnUnlockAttemptAlwaysResolvesTheLockTracking) {
    namespace policy = ce::ddraw_present_policy;
    policy::SurfaceLockTrack track;

    // The failed Unlock is the conservative Unknown and leaves no depth.
    policy::BeginSurfaceLockTrack(track, true);
    EXPECT_EQ(policy::CompleteSurfaceLockTrack(track, false), policy::SurfaceLockAccess::Unknown);
    EXPECT_EQ(track.depth, 0u);

    // The next pair completes normally instead of staying deferred forever.
    policy::BeginSurfaceLockTrack(track, true);
    EXPECT_EQ(policy::CompleteSurfaceLockTrack(track, true), policy::SurfaceLockAccess::Writable);

    // An unbalanced pair - two successful locks, one unlock - cannot leave
    // depth behind either; the single Unlock resolves both holders.
    policy::BeginSurfaceLockTrack(track, false);
    policy::BeginSurfaceLockTrack(track, true);
    EXPECT_EQ(policy::CompleteSurfaceLockTrack(track, true), policy::SurfaceLockAccess::Writable);
    EXPECT_EQ(track.depth, 0u);

    // A read-only holder reports ReadOnly; an Unlock with nothing tracked is
    // the conservative Unknown.
    policy::BeginSurfaceLockTrack(track, false);
    EXPECT_EQ(policy::CompleteSurfaceLockTrack(track, true), policy::SurfaceLockAccess::ReadOnly);
    EXPECT_EQ(policy::CompleteSurfaceLockTrack(track, true), policy::SurfaceLockAccess::Unknown);

    // The bookkeeping only helps while every Unlock detour feeds it on every
    // attempt, failed or not - that unconditionality is the whole fix.
    const std::filesystem::path source =
        std::filesystem::current_path() / "hook/apis" / "ddraw_hook_detours_surface_access.cpp";
    const std::string contents = ce::test_source::ReadFile(source);
    ASSERT_FALSE(contents.empty()) << source.string();
    EXPECT_EQ(CountOccurrences(contents, "CompleteDirectDrawSurfaceLock(surface, SUCCEEDED(hr))"), 3u)
        << "an Unlock detour stopped resolving the lock tracking unconditionally";
    EXPECT_EQ(contents.find("SUCCEEDED(hr) ? CompleteDirectDrawSurfaceLock"), std::string::npos);
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
    // The whole DirectDraw hook family, not one unit: the Surface4 generation
    // lives in its own translation unit and its guards count the same.
    const std::filesystem::path source = std::filesystem::current_path() / "hook/apis" / "ddraw_hook.cpp";
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

    size_t answers = 0;
    for (size_t at = contents.find("AnswerReenteredPresentation(surface"); at != std::string::npos;
         at = contents.find("AnswerReenteredPresentation(surface", at + 1)) {
        ++answers;
    }
    EXPECT_EQ(answers, 9u) << "a re-entry guard stopped answering the nested presentation";

    // Session 20260916_013230: answering the cycle by calling another function -
    // even one captured before CE patched the slot and validated as ddraw-owned -
    // did not end it, because that address is where the foreign entry patch sits.
    // The answer is the implementation reached *past* that patch, never the
    // recorded entry point and never CE's own saved original.
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

// The answer to a nested presentation may never be CE's own saved original -
// that pointer is what leads back into the injector that re-entered CE - but it
// must still present. Session 20260916_021049: every nested Flip was on the
// primary surface and every one was dropped, so the screen kept the last image
// it had while the game ran on.
TEST(DDrawLockFlagsTest, ANestedPresentationRunsTheRealImplementationThroughABypass) {
    const std::filesystem::path source = std::filesystem::current_path() / "hook/apis" / "ddraw_hook.cpp";
    const std::string contents = ce::test_source::ReadLogicalSource(source);
    ASSERT_FALSE(contents.empty()) << source.string();

    // The bypass is built from the module's own on-disk bytes, past the foreign
    // entry patch, exactly as DXGIShared does for a patched dxgi!Present.
    EXPECT_NE(contents.find("InlineHook::CreateBypassTrampoline"), std::string::npos);
    EXPECT_NE(contents.find("EntryLooksInlinePatched"), std::string::npos);

    // And it is still never the saved original that gets called back.
    EXPECT_EQ(contents.find("AcquireDirectDrawPresentCycleEscape"), std::string::npos)
        << "a re-entry guard is calling the recorded entry point again";

    // The answer itself is shared by all three DirectDraw generations, so it
    // lives in the classification header rather than in any one unit.
    const std::filesystem::path shared =
        std::filesystem::current_path() / "hook/apis" / "ddraw_hook_blit_classification.h";
    const std::string sharedContents = ce::test_source::ReadLogicalSource(shared);
    ASSERT_FALSE(sharedContents.empty()) << shared.string();
    EXPECT_NE(sharedContents.find("NestedPresentationMayRunRealImplementation"), std::string::npos);
    EXPECT_NE(sharedContents.find("AcquireDirectDrawPresentEntryBypass"), std::string::npos);
}
