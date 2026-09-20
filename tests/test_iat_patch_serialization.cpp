#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "source_fragment_reader.h"

// Regression for the Strange Brigade crash in session 20260920_224536.
//
// CE patches import slots from at least two threads: the hook thread's
// PatchIATAllModulesFiltered sweep, and the LoadLibrary hook's late-load pass
// (main_redirect.cpp, PatchLateLoadedCreateProcessImports, which runs on
// whichever game thread mapped the module). Both call IATHook::PatchIAT.
//
// PatchIAT changes a page's protection to write one pointer and then puts the
// protection back. Page protection is process-wide state, so two of those
// sequences interleaving on one page destroy each other: A unprotects, B
// finishes and restores PAGE_READONLY, A's InterlockedCompareExchangePointer
// then writes into a read-only page. That is exactly what happened -
// steamclient64.dll's CreateProcessA and CreateProcessW thunks are adjacent
// slots in one page, the sweep and the late-load pass reached them inside the
// same millisecond, and the sweep died with 0xC0000005 writing to
// 0x7FF8B6F6B928 (MEM_IMAGE, PAGE_READONLY).
//
// The property that makes the sequence correct is that g_PatchLock is held
// across all three steps. That is structural, so it is asserted structurally:
// a race test would need timing assumptions and would pass by luck.

namespace {

std::string ReadSource(const std::filesystem::path& relativePath) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / relativePath);
}

// The body of `name`, from its definition to the first line that is exactly "}".
std::string FunctionBody(const std::string& source, const std::string& signature) {
    const size_t begin = source.find(signature);
    if (begin == std::string::npos)
        return {};
    const size_t end = source.find("\n}\n", begin);
    if (end == std::string::npos)
        return {};
    return source.substr(begin, end - begin);
}

size_t CountOccurrences(const std::string& haystack, const std::string& needle) {
    size_t count = 0;
    for (size_t at = haystack.find(needle); at != std::string::npos; at = haystack.find(needle, at + needle.size()))
        ++count;
    return count;
}

}  // namespace

class IatPatchSerializationTest : public ::testing::Test {
protected:
    void SetUp() override {
        source_ = ReadSource("hook/wrappers/iat_hook.cpp");
        ASSERT_FALSE(source_.empty()) << "hook/wrappers/iat_hook.cpp could not be read";
    }

    std::string source_;
};

TEST_F(IatPatchSerializationTest, PatchIatTakesThePatchLockBeforeItUnprotectsThePage) {
    const std::string body = FunctionBody(source_, "bool PatchIAT(HMODULE targetModule");
    ASSERT_FALSE(body.empty()) << "PatchIAT could not be located";

    const size_t lockAt = body.find("g_PatchLock");
    const size_t unprotectAt = body.find("PAGE_READWRITE");
    ASSERT_NE(lockAt, std::string::npos) << "PatchIAT must take g_PatchLock";
    ASSERT_NE(unprotectAt, std::string::npos) << "PatchIAT must unprotect the slot before writing it";

    EXPECT_LT(lockAt, unprotectAt)
        << "g_PatchLock is acquired after the page is unprotected, so a second CE thread can restore "
           "PAGE_READONLY inside this thread's write window (Strange Brigade, session 20260920_224536)";
}

TEST_F(IatPatchSerializationTest, PatchIatStillHoldsTheLockWhenItWritesAndRestores) {
    const std::string body = FunctionBody(source_, "bool PatchIAT(HMODULE targetModule");
    ASSERT_FALSE(body.empty());

    const size_t writeAt = body.find("InterlockedCompareExchangePointer");
    ASSERT_NE(writeAt, std::string::npos);

    // The restoring VirtualProtect is the one that hands `oldProtect` back.
    const size_t restoreAt = body.find("oldProtect, &oldProtect", writeAt);
    ASSERT_NE(restoreAt, std::string::npos) << "the protection must be restored after the write";

    // Nothing may release the lock between the unprotect and the restore on the
    // success path: the only unlock before the restore belongs to a failure exit.
    const size_t unprotectAt = body.find("PAGE_READWRITE");
    const std::string window = body.substr(unprotectAt, restoreAt - unprotectAt);
    const size_t unlocksInWindow = CountOccurrences(window, "patchLock.unlock()");
    EXPECT_LE(unlocksInWindow, CountOccurrences(window, "return false;"))
        << "the lock is released inside the unprotect/write window on a path that then still writes";
}

TEST_F(IatPatchSerializationTest, TheOtherTwoSlotWritersAlreadyHoldTheLockThroughout) {
    // RestoreIAT and ShutdownIATHooks were always correct; pin that, so a future
    // edit cannot quietly give them PatchIAT's old shape.
    const std::string restore = FunctionBody(source_, "bool RestoreIAT(HMODULE targetModule");
    ASSERT_FALSE(restore.empty()) << "RestoreIAT could not be located";
    const size_t restoreLock = restore.find("g_PatchLock");
    const size_t restoreUnprotect = restore.find("PAGE_READWRITE");
    ASSERT_NE(restoreLock, std::string::npos);
    ASSERT_NE(restoreUnprotect, std::string::npos);
    EXPECT_LT(restoreLock, restoreUnprotect);

    const std::string shutdown = ReadSource("hook/wrappers/iat_hook_init.cpp");
    ASSERT_FALSE(shutdown.empty());
    const std::string shutdownBody = FunctionBody(shutdown, "void ShutdownIATHooks()");
    ASSERT_FALSE(shutdownBody.empty()) << "ShutdownIATHooks could not be located";
    const size_t shutdownLock = shutdownBody.find("g_PatchLock");
    const size_t shutdownUnprotect = shutdownBody.find("PAGE_READWRITE");
    ASSERT_NE(shutdownLock, std::string::npos);
    ASSERT_NE(shutdownUnprotect, std::string::npos);
    EXPECT_LT(shutdownLock, shutdownUnprotect);
}

TEST_F(IatPatchSerializationTest, PatchIatDoesNotReacquireTheNonRecursivePatchLock) {
    // g_PatchLock is a plain std::mutex, which libc++ maps onto an SRWLOCK: a
    // second acquire on the same thread parks it forever rather than throwing.
    // TryGetTrackedOriginalForPatchedEntry takes the lock itself, so the guard in
    // PatchIAT must start after that call, never before it.
    const std::string body = FunctionBody(source_, "bool PatchIAT(HMODULE targetModule");
    ASSERT_FALSE(body.empty());

    const size_t trackedLookupAt = body.find("TryGetTrackedOriginalForPatchedEntry");
    const size_t lockAt = body.find("g_PatchLock");
    ASSERT_NE(trackedLookupAt, std::string::npos);
    ASSERT_NE(lockAt, std::string::npos);
    EXPECT_LT(trackedLookupAt, lockAt)
        << "PatchIAT holds g_PatchLock across a helper that acquires it again; std::mutex is not recursive";

    EXPECT_EQ(CountOccurrences(source_, "std::recursive_mutex g_PatchLock"), 0u)
        << "making g_PatchLock recursive hides the re-entry rather than respecting it";
}
