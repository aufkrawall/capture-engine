#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <iterator>
#include <string>

#include "common/platform/build_identity.h"
#include "common/platform/build_identity_check.h"
#include "tests/source_fragment_reader.h"

namespace bid = ce::build_identity;

namespace {

bid::Record Sample() {
    bid::Record record = bid::Request();
    record.buildNumber = 7277;
    std::strcpy(record.version, "0.1.7277");
    std::strcpy(record.timestamp, "2026-10-19 21:40:42");
    return record;
}

int g_exportCalls = 0;
bid::Record g_exportAnswer;
bool g_exportAccepts = true;

bool FakeExport(bid::Record* out) {
    ++g_exportCalls;
    if (!g_exportAccepts)
        return false;
    *out = g_exportAnswer;
    return true;
}

}  // namespace

TEST(BuildIdentityCheckTest, CurrentCarriesTheGeneratedIdentityOfThisBinary) {
    const bid::Record current = bid::Current();
    EXPECT_EQ(current.size, sizeof(bid::Record));
    EXPECT_EQ(current.buildNumber, GetCurrentBuildNumber());
    EXPECT_STREQ(current.version, GetCaptureVersion());
    EXPECT_STREQ(current.timestamp, GetBuildTimestamp());
    EXPECT_EQ(bid::Compare(current, current), bid::Verdict::Match);
}

TEST(BuildIdentityCheckTest, PublishAnswersOnlyTheRecordLayoutItKnows) {
    bid::Record record = bid::Request();
    ASSERT_TRUE(bid::Publish(&record));
    EXPECT_EQ(bid::Compare(bid::Current(), record), bid::Verdict::Match);

    EXPECT_FALSE(bid::Publish(nullptr));
    bid::Record wrongSize = bid::Request();
    wrongSize.size = sizeof(bid::Record) - 1;
    wrongSize.buildNumber = 99;
    EXPECT_FALSE(bid::Publish(&wrongSize));
    EXPECT_EQ(wrongSize.buildNumber, 99u) << "a refused request is never written to";
    wrongSize.size = 0;
    EXPECT_FALSE(bid::Publish(&wrongSize));
}

TEST(BuildIdentityCheckTest, CompareNamesTheFirstFieldThatDiffers) {
    const bid::Record host = Sample();
    EXPECT_EQ(bid::Compare(host, Sample()), bid::Verdict::Match);

    bid::Record other = Sample();
    other.size = sizeof(bid::Record) + 8;
    EXPECT_EQ(bid::Compare(host, other), bid::Verdict::SizeMismatch);

    other = Sample();
    other.buildNumber = 7276;
    EXPECT_EQ(bid::Compare(host, other), bid::Verdict::BuildNumberMismatch);

    other = Sample();
    std::strcpy(other.version, "0.1.7276");
    EXPECT_EQ(bid::Compare(host, other), bid::Verdict::VersionMismatch);

    other = Sample();
    std::strcpy(other.timestamp, "2026-10-19 21:40:43");
    EXPECT_EQ(bid::Compare(host, other), bid::Verdict::TimestampMismatch);

    // A shorter text that is a prefix of the host's is still another text.
    other = Sample();
    std::strcpy(other.version, "0.1.727");
    EXPECT_EQ(bid::Compare(host, other), bid::Verdict::VersionMismatch);
}

TEST(BuildIdentityCheckTest, TextWithoutTerminatorNeverMatches) {
    bid::Record host = Sample();
    bid::Record module = Sample();
    std::memset(host.version, 'v', sizeof(host.version));
    std::memset(module.version, 'v', sizeof(module.version));
    EXPECT_EQ(bid::Compare(host, module), bid::Verdict::VersionMismatch);

    host = Sample();
    module = Sample();
    std::memset(host.timestamp, 't', sizeof(host.timestamp));
    std::memset(module.timestamp, 't', sizeof(module.timestamp));
    EXPECT_EQ(bid::Compare(host, module), bid::Verdict::TimestampMismatch);
}

TEST(BuildIdentityCheckTest, BytesAfterTheTerminatorDoNotMatter) {
    bid::Record module = Sample();
    std::memset(module.version + 10, 'x', sizeof(module.version) - 10);
    EXPECT_EQ(bid::Compare(Sample(), module), bid::Verdict::Match);
}

TEST(BuildIdentityCheckTest, CheckExportRefusesAMissingOrRejectingExport) {
    g_exportCalls = 0;
    g_exportAnswer = Sample();
    g_exportAccepts = false;
    EXPECT_EQ(bid::CheckExport(nullptr, Sample()), bid::Verdict::ExportMissing);
    EXPECT_EQ(g_exportCalls, 0);
    EXPECT_EQ(bid::CheckExport(&FakeExport, Sample()), bid::Verdict::ExportRejected);
    EXPECT_EQ(g_exportCalls, 1);
}

TEST(BuildIdentityCheckTest, CheckExportComparesTheAnswerAndReportsIt) {
    g_exportAccepts = true;
    g_exportAnswer = Sample();
    bid::Record reported{};
    EXPECT_EQ(bid::CheckExport(&FakeExport, Sample(), &reported), bid::Verdict::Match);
    EXPECT_EQ(reported.buildNumber, 7277u);

    g_exportAnswer.buildNumber = 7000;
    std::strcpy(g_exportAnswer.version, "0.1.7000");
    EXPECT_EQ(bid::CheckExport(&FakeExport, Sample(), &reported), bid::Verdict::BuildNumberMismatch);
    EXPECT_EQ(reported.buildNumber, 7000u) << "the mismatching identity is available for the log line";

    g_exportAnswer = Sample();
    g_exportAnswer.size = 8;
    EXPECT_EQ(bid::CheckExport(&FakeExport, Sample()), bid::Verdict::SizeMismatch);
}

// Both executables that load mediaengine.dll (the media role through MediaEngine_Load, the process-loopback
// worker host directly) compare identities before they call into it. The worker host is product-only.
TEST(BuildIdentityCheckTest, TheProcessLoopbackWorkerHostChecksBeforeCallingIntoTheModule) {
    const std::string host = ce::test_source::ReadFile(std::filesystem::current_path() / "captureengine" / "media" /
                                                       "process_loopback_worker_host.cpp");
    ASSERT_FALSE(host.empty());
    const size_t load = host.find("LoadLibraryFromSecurePath(executableDir");
    const size_t check = host.find("bid::CheckExport(", load);
    const size_t entry = host.find("GetProcAddress(mediaEngine, \"MediaEngine_RunProcessLoopbackWorker\")", load);
    ASSERT_NE(load, std::string::npos);
    ASSERT_NE(check, std::string::npos);
    ASSERT_NE(entry, std::string::npos);
    EXPECT_LT(check, entry);
    EXPECT_NE(host.find("ERROR_REVISION_MISMATCH", check), std::string::npos);
}

TEST(BuildIdentityCheckTest, DescribeAndVerdictNamesAreBoundedAndDistinct) {
    EXPECT_EQ(bid::Describe(Sample()), "0.1.7277 (build 7277, built 2026-10-19 21:40:42)");

    bid::Record garbage{};
    std::memset(garbage.version, 'v', sizeof(garbage.version));
    std::memset(garbage.timestamp, 't', sizeof(garbage.timestamp));
    EXPECT_EQ(bid::Describe(garbage).size(), bid::kTextChars * 2 + std::string(" (build 0, built )").size());

    const bid::Verdict all[] = {bid::Verdict::Match,           bid::Verdict::ExportMissing,
                                bid::Verdict::ExportRejected,  bid::Verdict::SizeMismatch,
                                bid::Verdict::BuildNumberMismatch, bid::Verdict::VersionMismatch,
                                bid::Verdict::TimestampMismatch};
    for (size_t i = 0; i < std::size(all); ++i)
        for (size_t j = i + 1; j < std::size(all); ++j)
            EXPECT_STRNE(bid::VerdictName(all[i]), bid::VerdictName(all[j]));
}
