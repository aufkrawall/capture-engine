#include <gtest/gtest.h>

#include <string>

#include "../common/log_privacy.h"

namespace privacy = ce::privacy;

namespace {

std::string Redact(const std::string& text) {
    return privacy::RedactUserAccountComponents(text);
}

}  // namespace

TEST(LogPrivacyTest, RedactsAccountInUserProfilePath) {
    // Regression: shared logs exposed the Windows account name through
    // install-directory, DLL, and session paths like these.
    EXPECT_EQ(Redact("C:\\Users\\Jdoe\\Programme\\build\\captureproject\\installed\\captureengine\\logs"),
              "C:\\Users\\****\\Programme\\build\\captureproject\\installed\\captureengine\\logs");
}

TEST(LogPrivacyTest, RedactsEveryAccountOccurrenceInOneLine) {
    EXPECT_EQ(Redact("baseDir=C:\\Users\\alice\\ce manifest=C:\\Users\\alice\\ce\\VK_LAYER_CE_overlay.json ok"),
              "baseDir=C:\\Users\\*****\\ce manifest=C:\\Users\\*****\\ce\\VK_LAYER_CE_overlay.json ok");
}

TEST(LogPrivacyTest, RedactionIsCaseInsensitiveForUsersComponent) {
    // The "users" component matches case-insensitively while its original
    // spelling is preserved verbatim; only the account token is masked.
    EXPECT_EQ(Redact("c:\\users\\Bob\\file.dll"), "c:\\users\\***\\file.dll");
    EXPECT_EQ(Redact("C:\\USERS\\Bob\\file.dll"), "C:\\USERS\\***\\file.dll");
    EXPECT_NE(Redact("C:/Users/Bob/file.dll"), "C:/Users/Bob/file.dll");
    EXPECT_EQ(Redact("C:/Users/Bob/file.dll"), "C:/Users/***/file.dll");
}

TEST(LogPrivacyTest, AccountAtEndOfStringWithoutTrailingSeparatorIsRedacted) {
    EXPECT_EQ(Redact("logsPath=C:\\Users\\jdoe"), "logsPath=C:\\Users\\****");
}

TEST(LogPrivacyTest, RedactionPreservesMessageLength) {
    // Log funnels format into fixed-capacity buffers, so redaction must never
    // grow (or shrink) a formatted message.
    const std::string original = "C:\\Users\\ab\\x";
    EXPECT_EQ(Redact(original).size(), original.size());
    const std::string longAccount = "C:\\Users\\a-very-long-account-name\\x";
    EXPECT_EQ(Redact(longAccount).size(), longAccount.size());
}

TEST(LogPrivacyTest, NonUserPathsAreUntouched) {
    EXPECT_EQ(Redact("C:\\WINDOWS\\SYSTEM32\\dxgi.dll"), "C:\\WINDOWS\\SYSTEM32\\dxgi.dll");
    EXPECT_EQ(Redact("\\\\nas\\recordings\\20260823\\file.mkv"), "\\\\nas\\recordings\\20260823\\file.mkv");
    EXPECT_EQ(Redact("H:\\captures\\capture_stage.mkv"), "H:\\captures\\capture_stage.mkv");
    EXPECT_EQ(Redact("no paths here"), "no paths here");
}

TEST(LogPrivacyTest, WordsContainingUsersAreNotRedacted) {
    EXPECT_EQ(Redact("C:\\data\\myusers\\bob\\x"), "C:\\data\\myusers\\bob\\x");
    EXPECT_EQ(Redact("C:\\users.txt"), "C:\\users.txt");
    EXPECT_EQ(Redact("the users folder"), "the users folder");
}

TEST(LogPrivacyTest, InPlaceVariantMatchesCopyVariantAndTerminates) {
    const std::string original = "[Inject] Using DLL: C:\\Users\\secret.user\\ce\\capture_hook_x64.dll";
    char buffer[128];
    snprintf(buffer, sizeof(buffer), "%s", original.c_str());
    const size_t newLen = privacy::RedactUserAccountComponents(buffer);
    EXPECT_EQ(newLen, strlen(buffer));
    EXPECT_STREQ(buffer, Redact(original).c_str());
}

TEST(LogPrivacyTest, LengthBoundedInputNeverReadsPastLength) {
    // The funnel variants pass (buffer, capacity); a truncated write must not
    // redact bytes that were never part of the formatted message.
    char buffer[] = "C:\\Users\\alice\\app";
    const size_t len = strlen(buffer);
    const size_t outLen = privacy::RedactUserAccountComponents(buffer, len);
    EXPECT_EQ(outLen, len);
    buffer[outLen] = '\0';
    EXPECT_STREQ(buffer, "C:\\Users\\*****\\app");
}

TEST(LogPrivacyTest, EmptyAndShortInputsAreSafe) {
    EXPECT_EQ(privacy::RedactUserAccountComponents(static_cast<char*>(nullptr)), 0u);
    char empty[] = "";
    EXPECT_EQ(privacy::RedactUserAccountComponents(empty, 0), 0u);
    EXPECT_EQ(Redact(""), "");
    EXPECT_EQ(Redact("short"), "short");
}

TEST(LogPrivacyTest, CollapseKeepsRootPrefixAndLeafOnly) {
    EXPECT_EQ(privacy::CollapsePathForLog("H:\\captures\\capture_stage_20260823T120000000Z_p1234_s1.mkv"),
              "H:\\...\\capture_stage_20260823T120000000Z_p1234_s1.mkv");
    EXPECT_EQ(privacy::CollapsePathForLog("C:\\Users\\jdoe\\Programme\\ce\\captures\\screenshot_1.png"),
              "C:\\...\\screenshot_1.png");
}

TEST(LogPrivacyTest, CollapseHandlesUncRelativeAndBarePaths) {
    EXPECT_EQ(privacy::CollapsePathForLog("\\\\nas\\media\\clips\\capture.mkv"), "\\\\...\\capture.mkv");
    EXPECT_EQ(privacy::CollapsePathForLog("relative\\dir\\file.png"), "...\\file.png");
    EXPECT_EQ(privacy::CollapsePathForLog("file.png"), "file.png");
    EXPECT_EQ(privacy::CollapsePathForLog("Z:\\Captures"), "Z:\\...");
    EXPECT_EQ(privacy::CollapsePathForLog(""), "");
}

TEST(LogPrivacyTest, CollapseStripsExtendedLengthPrefix) {
    EXPECT_EQ(privacy::CollapsePathForLog("\\\\?\\D:\\deeply\\nested\\out.mp4"), "\\\\?\\D:\\...\\out.mp4");
}

TEST(LogPrivacyTest, CollapsePreservesForwardSlashStyleLeafJoiner) {
    // POSIX-style absolute paths have no Windows root; they still collapse to
    // the leaf with their original separator style.
    EXPECT_EQ(privacy::CollapsePathForLog("/var/tmp/capture.mkv"), ".../capture.mkv");
    EXPECT_EQ(privacy::CollapsePathForLog("https://example.com/a/b.png"), ".../b.png");
}
