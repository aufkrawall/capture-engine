#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "../hook/common/present_hook_target_memo.h"
#include "source_fragment_reader.h"

// GTA session 20260925_233000: DetectSLPresentHook resolved the module of a foreign JMP target on dxgi!Present
// (an overlay thunk outside any module) on every Present of the game thread and AMD's presenter thread -
// 15,000 loader lookups in five minutes. The verdict is now remembered until the module set changes.

namespace {

using ce::present_hook_target::RejectedTargetMemo;

const void* const kThunk = reinterpret_cast<const void*>(0x7FFC102F0298ull);
const void* const kOtherThunk = reinterpret_cast<const void*>(0x7FFC20000000ull);

TEST(PresentHookTargetMemo, RemembersTheRejectedTargetForTheSameModuleSet) {
    RejectedTargetMemo memo;
    EXPECT_FALSE(memo.IsKnownRejected(kThunk, 10));
    memo.RememberRejected(kThunk, 10);
    EXPECT_TRUE(memo.IsKnownRejected(kThunk, 10));
}

TEST(PresentHookTargetMemo, AModuleLoadOrUnloadForcesReevaluation) {
    RejectedTargetMemo memo;
    memo.RememberRejected(kThunk, 10);
    // Streamline may have loaded and taken the entry since.
    EXPECT_FALSE(memo.IsKnownRejected(kThunk, 11));
}

TEST(PresentHookTargetMemo, ADifferentJumpTargetIsReevaluated) {
    RejectedTargetMemo memo;
    memo.RememberRejected(kThunk, 10);
    EXPECT_FALSE(memo.IsKnownRejected(kOtherThunk, 10));
}

TEST(PresentHookTargetMemo, ANullTargetIsNeverRemembered) {
    RejectedTargetMemo memo;
    memo.RememberRejected(nullptr, 10);
    EXPECT_FALSE(memo.IsKnownRejected(nullptr, 10));
}

std::string ReadSource(const char* relativePath) {
    return ce::test_source::ReadFile(std::filesystem::current_path() / relativePath);
}

TEST(PresentHookTargetMemoSource, MemoIsConsultedBeforeTheModuleLookupAndOnlyWithLoaderNotifications) {
    const std::string routing = ReadSource("hook/common/dxgi_shared_steam_routing.cpp");
    ASSERT_FALSE(routing.empty());
    const size_t fn = routing.find("void DetectSLPresentHook()");
    ASSERT_NE(fn, std::string::npos);
    const size_t memoCheck = routing.find("s_rejectedTarget.IsKnownRejected(hookTarget, moduleSetGeneration)", fn);
    const size_t lookup = routing.find("TryGetModulePathFromCodeAddress(hookTarget", fn);
    const size_t remember = routing.find("s_rejectedTarget.RememberRejected(hookTarget, moduleSetGeneration)", fn);
    ASSERT_NE(memoCheck, std::string::npos);
    ASSERT_NE(lookup, std::string::npos);
    ASSERT_NE(remember, std::string::npos);
    EXPECT_LT(memoCheck, lookup);
    EXPECT_GT(remember, lookup);
    EXPECT_NE(routing.find("moduleSetObservable && s_rejectedTarget.IsKnownRejected(", fn), std::string::npos);
}

TEST(PresentHookTargetMemoSource, StreamlineModuleSnapshotRerunsOnlyOnAModuleSetChange) {
    const std::string init = ReadSource("hook/apis/streamline_hook.cpp");
    ASSERT_FALSE(init.empty());
    const size_t fn = init.find("void Init() {");
    ASSERT_NE(fn, std::string::npos);
    const size_t gate = init.find("s_scannedModuleSet == moduleSetGeneration", fn);
    const size_t featureOnly = init.find("ResolveStreamlineFeatureHooks(/*pinFeatureResolution=*/true)", fn);
    const size_t fullScan = init.find("ScanLoadedStreamlineModules(/*pinFeatureResolution=*/true, &snapshotCompleted)", fn);
    ASSERT_NE(gate, std::string::npos);
    ASSERT_NE(featureOnly, std::string::npos);
    ASSERT_NE(fullScan, std::string::npos);
    EXPECT_LT(gate, featureOnly);
    EXPECT_NE(init.find("s_scanCommitted = snapshotCompleted;", fn), std::string::npos)
        << "a failed snapshot must not suppress the next scan";
}

}  // namespace
