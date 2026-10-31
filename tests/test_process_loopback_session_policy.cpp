#include <gtest/gtest.h>

#include <array>

#include "mediaengine/audio/process_loopback_session_policy.h"

namespace {
namespace policy = ce::process_loopback;

const policy::CaptureSessionBinding kHealthyBinding{100, 7, true, true};
const std::array<policy::AudioSessionCreation, 1> kChildSession{{{8, 101}}};

WhitelistEntry Exclusion() {
    WhitelistEntry exclusion;
    exclusion.pattern = R"(Modded\game.exe)";
    return exclusion;
}

std::vector<policy::ProcessTreeEntry> ParentTree() {
    return {{100, 4, "game.exe", R"(D:\Games\Default\game.exe)"}};
}

TEST(ProcessLoopbackSessionPolicyTest, ANewExcludedChildRevokesAnAlreadyQualifiedParentBinding) {
    auto processes = ParentTree();
    const std::array<policy::AudioSessionCreation, 1> unrelated{{{8, 500}}};
    EXPECT_EQ(policy::ResolveSessionActivity(processes, unrelated, "game.exe", {Exclusion()}, kHealthyBinding).action,
              policy::SessionActivityAction::Continue);
    processes.push_back({101, 100, "game.exe", R"(D:\Games\Modded\game.exe)"});
    EXPECT_FALSE(policy::ShouldRecycleCaptureForSessionCreation(processes, true, true, 100, 101, 7, 8))
        << "qualification alone used to keep capturing the newly excluded descendant";
    const auto changed = policy::ResolveSessionActivity(processes, kChildSession, "game.exe", {Exclusion()},
                                                       kHealthyBinding);
    EXPECT_EQ(changed.action, policy::SessionActivityAction::ReselectProcess);
    EXPECT_EQ(changed.selection.selectedProcessId, 0u);
    EXPECT_EQ(changed.selection.excludedProcessCount, 1u);
    EXPECT_EQ(changed.selection.excludedTreeOverlapCount, 1u);
    processes.pop_back();
    EXPECT_EQ(policy::ResolveSessionActivity(processes, unrelated, "game.exe", {Exclusion()}, kHealthyBinding).action,
              policy::SessionActivityAction::Continue);
}

TEST(ProcessLoopbackSessionPolicyTest, ARevokedBindingCanSelectAnIndependentEligibleInstallation) {
    auto processes = ParentTree();
    processes.push_back({101, 100, "game.exe", R"(D:\Games\Modded\game.exe)"});
    processes.push_back({200, 4, "game.exe", R"(C:\Other\game.exe)"});
    const auto changed = policy::ResolveSessionActivity(processes, kChildSession, "game.exe", {Exclusion()},
                                                       kHealthyBinding);
    EXPECT_EQ(changed.action, policy::SessionActivityAction::ReselectProcess);
    EXPECT_EQ(changed.selection.selectedProcessId, 200u);
}

TEST(ProcessLoopbackSessionPolicyTest, AnUnrelatedLargerTreeNeverDisplacesAHealthyBinding) {
    auto processes = ParentTree();
    processes.push_back({200, 4, "game.exe", R"(C:\Other\game.exe)"});
    processes.push_back({201, 200, "game.exe", R"(C:\Other\game.exe)"});
    ASSERT_EQ(policy::SelectProcessTreeRootByName(processes, "game.exe", {Exclusion()}).selectedProcessId, 200u);
    const auto unchanged = policy::ResolveSessionActivity(processes, kChildSession, "game.exe", {Exclusion()},
                                                         kHealthyBinding);
    EXPECT_EQ(unchanged.action, policy::SessionActivityAction::Continue);
}

TEST(ProcessLoopbackSessionPolicyTest, AnUnresolvedDescendantPathDefersAmbiguousCapture) {
    auto processes = ParentTree();
    processes.push_back({101, 100, "game.exe", {}});
    const auto changed = policy::ResolveSessionActivity(processes, kChildSession, "game.exe", {Exclusion()},
                                                       kHealthyBinding);
    EXPECT_EQ(changed.action, policy::SessionActivityAction::ReselectProcess);
    EXPECT_EQ(changed.selection.unresolvedPathProcessCount, 1u);
}

TEST(ProcessLoopbackSessionPolicyTest, AnIndependentExcludedTreeDoesNotInterruptTheActiveRoot) {
    auto processes = ParentTree();
    processes.push_back({101, 4, "game.exe", R"(D:\Games\Modded\game.exe)"});
    EXPECT_EQ(policy::ResolveSessionActivity(processes, kChildSession, "game.exe", {Exclusion()}, kHealthyBinding).action,
              policy::SessionActivityAction::Continue);
}

TEST(ProcessLoopbackSessionPolicyTest, AMissingProcessSnapshotCannotRevokeAnExistingBinding) {
    EXPECT_EQ(policy::ResolveSessionActivity({}, kChildSession, "game.exe", {Exclusion()}, kHealthyBinding).action,
              policy::SessionActivityAction::Continue);
}

TEST(ProcessLoopbackSessionPolicyTest, PreSessionRecoveryAndPidCaptureKeepTheirExistingContract) {
    auto processes = ParentTree();
    processes.push_back({101, 100, "helper.exe", {}});
    const policy::CaptureSessionBinding unqualified{100, 7, false, false};
    for (const std::string& selector : {std::string{}, std::string{"game.exe"}}) {
        EXPECT_EQ(policy::ResolveSessionActivity(processes, kChildSession, selector, {}, unqualified).action,
                  policy::SessionActivityAction::RebindSession);
        EXPECT_EQ(policy::ResolveSessionActivity(processes, kChildSession, selector, {}, kHealthyBinding).action,
                  policy::SessionActivityAction::Continue);
        const std::array<policy::AudioSessionCreation, 1> oldSession{{{7, 101}}};
        EXPECT_EQ(policy::ResolveSessionActivity(processes, oldSession, selector, {}, unqualified).action,
                  policy::SessionActivityAction::Continue);
        EXPECT_EQ(policy::ResolveSessionActivity(processes, {}, selector, {}, unqualified).action,
                  policy::SessionActivityAction::Continue);
    }
}

}  // namespace
