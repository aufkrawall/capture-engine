#include <gtest/gtest.h>

#include "../mediaengine/process_tree_selection.h"

namespace policy = ce::process_loopback;

TEST(ProcessTreeSelectionTest, SelectsSameNameTreeRootInsteadOfFirstEnumeratedChild) {
    const std::vector<policy::ProcessTreeEntry> processes = {
        {2116, 7000, "Brave.exe"},
        {5264, 20928, "brave.EXE"},
        {7000, 20928, "BRAVE.exe"},
        {20928, 400, "Brave.exe"},
        {400, 4, "explorer.exe"},
    };

    const auto selection = policy::SelectProcessTreeRootByName(processes, "brave.exe");

    EXPECT_EQ(selection.firstMatchProcessId, 2116u);
    EXPECT_EQ(selection.selectedProcessId, 20928u);
    EXPECT_EQ(selection.selectedParentProcessId, 400u);
    EXPECT_EQ(selection.matchingProcessCount, 4u);
    EXPECT_EQ(selection.rootCandidateCount, 1u);
    EXPECT_EQ(selection.selectedTreeSize, 4u);
    EXPECT_EQ(selection.selectedProcessTreeSize, 4u);
}

TEST(ProcessTreeSelectionTest, SelectsLargestIndependentSameNameTreeDeterministically) {
    const std::vector<policy::ProcessTreeEntry> processes = {
        {100, 4, "browser.exe"},
        {101, 100, "browser.exe"},
        {200, 4, "browser.exe"},
        {201, 200, "browser.exe"},
        {202, 200, "browser.exe"},
        {4, 0, "System"},
    };

    const auto selection = policy::SelectProcessTreeRootByName(processes, "Browser.exe");

    EXPECT_EQ(selection.selectedProcessId, 200u);
    EXPECT_EQ(selection.matchingProcessCount, 5u);
    EXPECT_EQ(selection.rootCandidateCount, 2u);
    EXPECT_EQ(selection.selectedTreeSize, 3u);
    EXPECT_EQ(selection.selectedProcessTreeSize, 3u);
}

TEST(ProcessTreeSelectionTest, SelectedProcessTreeIncludesDifferentlyNamedDescendants) {
    const std::vector<policy::ProcessTreeEntry> processes = {
        {500, 4, "browser.exe"},      {501, 500, "browser.exe"}, {502, 500, "audio-helper.exe"},
        {503, 502, "codec-host.exe"}, {4, 0, "explorer.exe"},
    };

    const auto selection = policy::SelectProcessTreeRootByName(processes, "browser.exe");

    EXPECT_EQ(selection.selectedProcessId, 500u);
    EXPECT_EQ(selection.selectedTreeSize, 2u);
    EXPECT_EQ(selection.selectedProcessTreeSize, 4u);
}

TEST(ProcessTreeSelectionTest, HandlesMissingParentsCyclesAndNoMatch) {
    const std::vector<policy::ProcessTreeEntry> processes = {
        {300, 301, "app.exe"},
        {301, 300, "app.exe"},
        {400, 999, "app.exe"},
    };

    const auto selection = policy::SelectProcessTreeRootByName(processes, "app.exe");
    EXPECT_NE(selection.selectedProcessId, 0u);
    EXPECT_EQ(selection.matchingProcessCount, 3u);

    const auto absent = policy::SelectProcessTreeRootByName(processes, "missing.exe");
    EXPECT_EQ(absent.selectedProcessId, 0u);
    EXPECT_EQ(absent.matchingProcessCount, 0u);
}

TEST(ProcessTreeSelectionTest, MatchesOnlyTheSelectedRootAndItsDescendants) {
    const std::vector<policy::ProcessTreeEntry> processes = {
        {500, 4, "browser.exe"},   {501, 500, "audio-helper.exe"}, {502, 501, "codec-host.exe"},
        {600, 4, "other.exe"},     {601, 600, "other-audio.exe"},  {700, 701, "cycle-a.exe"},
        {701, 700, "cycle-b.exe"},
    };

    EXPECT_TRUE(policy::ProcessBelongsToTree(processes, 500, 500));
    EXPECT_TRUE(policy::ProcessBelongsToTree(processes, 501, 500));
    EXPECT_TRUE(policy::ProcessBelongsToTree(processes, 502, 500));
    EXPECT_FALSE(policy::ProcessBelongsToTree(processes, 600, 500));
    EXPECT_FALSE(policy::ProcessBelongsToTree(processes, 601, 500));
    EXPECT_FALSE(policy::ProcessBelongsToTree(processes, 700, 500));
    EXPECT_FALSE(policy::ProcessBelongsToTree(processes, 0, 500));
    EXPECT_FALSE(policy::ProcessBelongsToTree(processes, 500, 0));
    EXPECT_TRUE(policy::ProcessBelongsToTree({}, 500, 500));
    EXPECT_FALSE(policy::ProcessBelongsToTree({}, 501, 500));
}

TEST(ProcessTreeSelectionTest, RecyclesForNewTargetSessionWhenActivationWasUnqualifiedOrHadNoTargetSession) {
    const std::vector<policy::ProcessTreeEntry> processes = {
        {500, 4, "browser.exe"},
        {501, 500, "audio-helper.exe"},
        {600, 4, "other.exe"},
    };

    EXPECT_TRUE(policy::ShouldRecycleCaptureForSessionCreation(processes, false, true, 500, 501, 7, 8));
    EXPECT_TRUE(policy::ShouldRecycleCaptureForSessionCreation(processes, false, false, 500, 500, 7, 8));
    EXPECT_TRUE(policy::ShouldRecycleCaptureForSessionCreation(processes, true, false, 500, 501, 7, 8));
    EXPECT_FALSE(policy::ShouldRecycleCaptureForSessionCreation(processes, true, true, 500, 501, 7, 8));
    EXPECT_FALSE(policy::ShouldRecycleCaptureForSessionCreation(processes, false, false, 500, 501, 7, 7));
    EXPECT_FALSE(policy::ShouldRecycleCaptureForSessionCreation(processes, false, false, 500, 501, 8, 7));
    EXPECT_FALSE(policy::ShouldRecycleCaptureForSessionCreation(processes, false, false, 500, 600, 7, 8));
}
