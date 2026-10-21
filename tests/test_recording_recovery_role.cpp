#include <gtest/gtest.h>

#include "captureengine/media/recording_recovery_role.h"
#include "mediaengine/mux/recording_recovery.h"

#include <windows.h>

#include <filesystem>
#include <algorithm>
#include <map>
#include <string>

// The journal walk: terminal outcomes leave the journal, a live writer or a failed attempt stays for the next run,
// and a cancel stops before the next file (libcengine 11, part 3).
namespace {
using ce::capture_output::InFlightJournal;
using ce::media::RecoveryOutcome;
using ce::role::RecoverJournalEntries;
using ce::role::RecoveryPassSummary;

class RecordingRecoveryRoleTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = std::filesystem::temp_directory_path() /
                (L"recovery_role_test_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
                 std::to_wstring(GetTickCount64()));
        std::filesystem::create_directories(root_);
    }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
    }
    InFlightJournal Journal() const { return InFlightJournal(root_ / L"journal"); }
    std::filesystem::path File(const wchar_t* name) const { return std::filesystem::absolute(root_ / name); }
    std::filesystem::path root_;
};

TEST_F(RecordingRecoveryRoleTest, TerminalOutcomesLeaveTheJournalAndRetryableOnesStay) {
    const InFlightJournal journal = Journal();
    const std::map<std::wstring, RecoveryOutcome> outcomes{
        {L"recovered.mkv", RecoveryOutcome::kRecovered},   {L"empty.mkv", RecoveryOutcome::kDiscardedEmpty},
        {L"garbage.mkv", RecoveryOutcome::kKeptIncomplete}, {L"live.mkv", RecoveryOutcome::kWriterAlive},
        {L"gone.mkv", RecoveryOutcome::kMissing},           {L"broken.mkv", RecoveryOutcome::kFailed},
    };
    for (const auto& [name, outcome] : outcomes)
        ASSERT_TRUE(journal.Add(File(name.c_str())));
    const auto recoverOne = [&](const std::filesystem::path& staging, std::wstring* output, int64_t* endUs) {
        const RecoveryOutcome outcome = outcomes.at(staging.filename().wstring());
        if (outcome == RecoveryOutcome::kRecovered) {
            *output = File(L"capture_recovered_1.mkv").wstring();
            *endUs = 2500000;
        }
        return static_cast<int>(outcome);
    };
    RecoveryPassSummary summary;
    const std::vector<ce::role::RecoveredRecording> recovered =
        RecoverJournalEntries(journal, recoverOne, nullptr, &summary);
    ASSERT_EQ(recovered.size(), 1u);
    EXPECT_EQ(recovered[0].output, File(L"capture_recovered_1.mkv").wstring());
    EXPECT_EQ(recovered[0].endUs, 2500000);
    EXPECT_EQ(summary.entries, 6u);
    EXPECT_EQ(summary.recovered, 1u);
    EXPECT_EQ(summary.discarded, 1u);
    EXPECT_EQ(summary.keptIncomplete, 1u);
    EXPECT_EQ(summary.writerAlive, 1u);
    EXPECT_EQ(summary.missing, 1u);
    EXPECT_EQ(summary.failed, 1u);
    std::vector<std::wstring> left;
    for (const std::filesystem::path& entry : journal.Entries())
        left.push_back(entry.filename().wstring());
    std::sort(left.begin(), left.end());
    EXPECT_EQ(left, (std::vector<std::wstring>{L"broken.mkv", L"live.mkv"}));
}

TEST_F(RecordingRecoveryRoleTest, ACancelStopsBeforeTheNextFileAndKeepsItsEntry) {
    const InFlightJournal journal = Journal();
    ASSERT_TRUE(journal.Add(File(L"a.mkv")));
    ASSERT_TRUE(journal.Add(File(L"b.mkv")));
    volatile long cancel = 0;
    int calls = 0;
    const auto recoverOne = [&](const std::filesystem::path&, std::wstring*, int64_t*) {
        ++calls;
        cancel = 1;  // the shutdown arrives while the first file is being repaired
        return static_cast<int>(RecoveryOutcome::kRecovered);
    };
    RecoveryPassSummary summary;
    RecoverJournalEntries(journal, recoverOne, &cancel, &summary);
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(summary.canceled, 1u);
    EXPECT_EQ(journal.Entries().size(), 1u);
}

TEST_F(RecordingRecoveryRoleTest, AnEmptyJournalCallsNothing) {
    int calls = 0;
    const auto recoverOne = [&](const std::filesystem::path&, std::wstring*, int64_t*) {
        ++calls;
        return 0;
    };
    RecoveryPassSummary summary;
    EXPECT_TRUE(RecoverJournalEntries(Journal(), recoverOne, nullptr, &summary).empty());
    EXPECT_EQ(calls, 0);
    EXPECT_EQ(summary.entries, 0u);
}

}  // namespace
