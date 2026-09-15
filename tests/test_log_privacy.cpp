#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "../common/log_privacy.h"
#include "source_fragment_reader.h"

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

TEST(LogPrivacyTest, StreamEndpointRedactionKeepsTheHostAndDropsThePlaypath) {
    // The trailing component of an RTMP URL is the stream key - a password equivalent. The
    // host is the useful diagnostic (which service, and whether it resolved), so it stays.
    EXPECT_EQ(privacy::RedactStreamEndpointsForLog("rtmp://live.example/app/live_123_SECRET failed"),
              "rtmp://live.example/<redacted> failed");
    EXPECT_EQ(privacy::RedactStreamEndpointsForLog("Cannot open rtmps://ingest.example/live/abc123"),
              "Cannot open rtmps://ingest.example/<redacted>");
}

TEST(LogPrivacyTest, StreamEndpointRedactionIsCaseInsensitiveAndHandlesQuotesAndRepeats) {
    EXPECT_EQ(privacy::RedactStreamEndpointsForLog("RTMP://Host/App/Key"), "RTMP://Host/<redacted>");
    EXPECT_EQ(privacy::RedactStreamEndpointsForLog("url='rtmp://h/a/k' retry"), "url='rtmp://h/<redacted>' retry");
    EXPECT_EQ(privacy::RedactStreamEndpointsForLog("rtmp://h/a/k1 then rtmp://h/a/k2"),
              "rtmp://h/<redacted> then rtmp://h/<redacted>");
}

TEST(LogPrivacyTest, StreamEndpointRedactionLeavesHarmlessTextAlone) {
    // A host-only URL carries no key, and unrelated text must survive untouched - the
    // callback that uses this runs over every libav diagnostic, not just failures.
    EXPECT_EQ(privacy::RedactStreamEndpointsForLog("rtmp://live.example"), "rtmp://live.example");
    EXPECT_EQ(privacy::RedactStreamEndpointsForLog("rtmp://live.example/"), "rtmp://live.example/<redacted>");
    EXPECT_EQ(privacy::RedactStreamEndpointsForLog("Qavg: 120.000 TNS(L): 0.0%"), "Qavg: 120.000 TNS(L): 0.0%");
    EXPECT_EQ(privacy::RedactStreamEndpointsForLog("https://example.com/a/b"), "https://example.com/a/b");
    EXPECT_EQ(privacy::RedactStreamEndpointsForLog(""), "");
}

TEST(LogPrivacySourceTest, LibavDiagnosticsAreRoutedIntoTheSessionLogAndRedacted) {
    // libav writes to stderr by default, and nothing in the tree redirects stderr, so a
    // windowless CaptureEngine process discarded every encoder, muxer and RTMP diagnostic.
    // The callback is the only thing keeping them, and it has to be installed before any
    // avformat/avcodec call can run.
    namespace fs = std::filesystem;
    const std::string engine = ce::test_source::ReadLogicalSource(fs::current_path() / "mediaengine" / "mediaengine.cpp");
    ASSERT_FALSE(engine.empty());
    EXPECT_NE(engine.find("av_log_set_callback(CaptureEngineAvLogCallback);"), std::string::npos);
    EXPECT_NE(engine.find("ce::privacy::RedactStreamEndpointsForLog(message).c_str()"), std::string::npos)
        << "every libav message must be redacted before it reaches the log";

    const size_t install = engine.find("InstallAvLogCallback();");
    const size_t engineInit = engine.find("return mediaengine_g_Engine->Init(config);");
    ASSERT_NE(install, std::string::npos);
    ASSERT_NE(engineInit, std::string::npos);
    EXPECT_LT(install, engineInit) << "the callback must be installed before the engine initializes";

    // Redaction is what allows the live path to keep its diagnostics; AV_LOG_QUIET used to be
    // the only protection for the stream key and silenced the transport entirely.
    const std::string configure =
        ce::test_source::ReadLogicalSource(fs::current_path() / "mediaengine" / "video_encoder_configure.cpp");
    ASSERT_FALSE(configure.empty());
    // Match the call, not the token: the surrounding comment explains why AV_LOG_QUIET was
    // dropped and legitimately names it.
    EXPECT_EQ(configure.find("av_log_set_level(liveOutput"), std::string::npos)
        << "live output must not be silenced now that endpoints are redacted";
    EXPECT_NE(configure.find("av_log_set_level(AV_LOG_WARNING);"), std::string::npos);
}
