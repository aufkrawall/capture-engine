#include <gtest/gtest.h>

#include "common/capture/inflight_journal.h"
#include "common/capture/reserved_capture_output.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

namespace {
using ce::capture_output::InFlightJournal;
using ce::capture_output::OutputNameSeed;
using ce::capture_output::ReservedCaptureOutput;

class InFlightJournalTest : public ::testing::Test {
protected:
    void SetUp() override {
        static std::atomic<uint64_t> s_Counter{0};
        root_ = std::filesystem::temp_directory_path() /
                (L"inflight_journal_test_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
                 std::to_wstring(GetTickCount64()) + L"_" + std::to_wstring(++s_Counter));
        std::filesystem::create_directories(root_ / L"out");
    }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
    }

    InFlightJournal Journal() const { return InFlightJournal(root_ / L"journal"); }
    std::filesystem::path Out() const { return root_ / L"out"; }

    static bool Lists(const InFlightJournal& journal, const std::filesystem::path& path) {
        const std::vector<std::filesystem::path> entries = journal.Entries();
        const std::filesystem::path absolute = std::filesystem::absolute(path);
        return std::any_of(entries.begin(), entries.end(),
                           [&](const std::filesystem::path& entry) { return entry == absolute; });
    }

    static OutputNameSeed Seed(uint64_t sequence) {
        OutputNameSeed seed;
        seed.utcMilliseconds = 1700000000000ull;
        seed.processId = 4242;
        seed.sequence = sequence;
        return seed;
    }

    std::filesystem::path root_;
};

TEST_F(InFlightJournalTest, AnEntryRoundTripsItsPathIncludingNonAsciiCharacters) {
    const InFlightJournal journal = Journal();
    const std::filesystem::path staging = Out() / L"capture_stage_ä中.mkv";
    ASSERT_TRUE(journal.Add(staging));
    const std::vector<std::filesystem::path> entries = journal.Entries();
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0], std::filesystem::absolute(staging));
    journal.Remove(staging);
    EXPECT_TRUE(journal.Entries().empty());
}

TEST_F(InFlightJournalTest, TheEntryNameIgnoresTheCaseOfThePath) {
    const InFlightJournal journal = Journal();
    EXPECT_EQ(journal.EntryPath(Out() / L"Capture_Stage_1.mkv"), journal.EntryPath(Out() / L"capture_stage_1.MKV"));
}

TEST_F(InFlightJournalTest, RemovingAnUnknownEntryIsHarmless) {
    Journal().Remove(Out() / L"never_added.mkv");
    EXPECT_TRUE(Journal().Entries().empty());
}

TEST_F(InFlightJournalTest, AJournalWithoutADirectoryRecordsNothing) {
    const InFlightJournal journal{std::filesystem::path()};
    EXPECT_FALSE(static_cast<bool>(journal));
    EXPECT_FALSE(journal.Add(Out() / L"x.mkv"));
    EXPECT_TRUE(journal.Entries().empty());
}

TEST_F(InFlightJournalTest, TheUserJournalOfUnitTestsIsTheTestNamespace) {
    const InFlightJournal journal = InFlightJournal::ForThisUser();
    ASSERT_TRUE(static_cast<bool>(journal));
    EXPECT_NE(journal.Directory().wstring().find(L"CaptureEngine_UT"), std::wstring::npos);
}

TEST_F(InFlightJournalTest, ATrackedStagingFileLeavesTheJournalWhenPublishedUnderItsFinalName) {
    const InFlightJournal journal = Journal();
    ReservedCaptureOutput staging = ReservedCaptureOutput::ReserveForTesting(Out(), L"capture_stage", L"mkv", Seed(1));
    ASSERT_TRUE(staging);
    ASSERT_TRUE(staging.TrackInFlight(journal));
    const std::filesystem::path stagingPath = staging.Path();
    EXPECT_TRUE(Lists(journal, stagingPath));
    ASSERT_TRUE(staging.PublishToNewPathForTesting(Out(), L"capture", L"mkv", Seed(2)));
    EXPECT_TRUE(journal.Entries().empty());
}

TEST_F(InFlightJournalTest, AStagingFileKeptUnderItsStagingNameLeavesTheJournal) {
    const InFlightJournal journal = Journal();
    ReservedCaptureOutput staging = ReservedCaptureOutput::ReserveForTesting(Out(), L"capture_stage", L"mka", Seed(3));
    ASSERT_TRUE(staging.TrackInFlight(journal));
    staging.Publish();
    EXPECT_TRUE(journal.Entries().empty());
}

TEST_F(InFlightJournalTest, ADiscardedStagingFileLeavesTheJournal) {
    const InFlightJournal journal = Journal();
    ReservedCaptureOutput staging = ReservedCaptureOutput::ReserveForTesting(Out(), L"capture_stage", L"mkv", Seed(4));
    ASSERT_TRUE(staging.TrackInFlight(journal));
    ASSERT_TRUE(staging.CleanupOwnedFile());
    EXPECT_TRUE(journal.Entries().empty());
}

TEST_F(InFlightJournalTest, AnUnpublishedReservationThatGoesAwayLeavesTheJournal) {
    const InFlightJournal journal = Journal();
    {
        ReservedCaptureOutput staging =
            ReservedCaptureOutput::ReserveForTesting(Out(), L"capture_stage", L"mkv", Seed(5));
        ASSERT_TRUE(staging.TrackInFlight(journal));
        ReservedCaptureOutput moved = std::move(staging);  // the entry follows the reservation
        EXPECT_FALSE(journal.Entries().empty());
    }
    EXPECT_TRUE(journal.Entries().empty());
}

// The case the journal exists for: the writer died before publishing, so nothing removed the entry Add wrote.
TEST_F(InFlightJournalTest, AnEntryWhoseWriterVanishedStaysForTheRecovery) {
    const InFlightJournal journal = Journal();
    const std::filesystem::path orphan = Out() / L"capture_stage_1700000000000_p1_s1.mkv";
    ASSERT_TRUE(journal.Add(orphan));
    EXPECT_TRUE(Lists(journal, orphan));
}

}  // namespace
