// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
// The runtime's process session: the cross-process session claim and the diagnostics session helpers
// (runtime/platform), formerly part of captureengine.exe's controller prelude.
#include <gtest/gtest.h>

#include "runtime/platform/diagnostics_session.h"
#include "runtime/platform/session_claim.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {
using ce::runtime::SessionClaim;

std::wstring UniqueClaimName() {
    static std::atomic<int> counter{0};
    return L"Local\\CaptureEngine_SessionClaimTest_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
           std::to_wstring(++counter);
}

class ScratchDirectory {
public:
    ScratchDirectory() {
        static std::atomic<int> counter{0};
        path_ = std::filesystem::temp_directory_path() /
                ("ce_session_test_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(++counter));
        std::filesystem::create_directories(path_);
    }
    ~ScratchDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    ScratchDirectory(const ScratchDirectory&) = delete;
    ScratchDirectory& operator=(const ScratchDirectory&) = delete;
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

void Touch(const std::filesystem::path& file) {
    std::ofstream(file) << "x";
}

TEST(SessionClaimTest, ASecondClaimIsRefusedAsHeldUntilTheFirstIsReleased) {
    const std::wstring name = UniqueClaimName();
    SessionClaim::Refusal refusal = SessionClaim::Refusal::Unavailable;
    auto first = SessionClaim::TryAcquire(name.c_str(), &refusal);
    ASSERT_TRUE(first.has_value());
    if (!first)
        return;
    EXPECT_EQ(refusal, SessionClaim::Refusal::None);

    DWORD error = 1;
    EXPECT_FALSE(SessionClaim::TryAcquire(name.c_str(), &refusal, &error).has_value());
    EXPECT_EQ(refusal, SessionClaim::Refusal::Held);
    EXPECT_EQ(error, static_cast<DWORD>(ERROR_SUCCESS)) << "a held claim is not an error";

    SessionClaim moved = std::move(first.value());
    first.reset();
    EXPECT_FALSE(SessionClaim::TryAcquire(name.c_str(), &refusal).has_value()) << "a moved claim is still held";
    {
        SessionClaim released = std::move(moved);
    }
    EXPECT_TRUE(SessionClaim::TryAcquire(name.c_str(), &refusal).has_value()) << "released when its holder ends";
}

TEST(SessionClaimTest, TheClaimNameIsTheOneEveryVersionUses) {
    EXPECT_STREQ(CE_PRODUCT_SESSION_CLAIM_NAME, L"Local\\CaptureEngine_Instance_Mutex");
    EXPECT_STREQ(ce::runtime::kSessionClaimName, CE_SESSION_CLAIM_NAME);
}

TEST(DiagnosticsSessionTest, SessionNamesSortChronologically) {
    SYSTEMTIME time{};
    time.wYear = 2026;
    time.wMonth = 3;
    time.wDay = 7;
    time.wHour = 9;
    time.wMinute = 5;
    time.wSecond = 1;
    EXPECT_EQ(ce::runtime::SessionDirectoryName(time), "20260307_090501");
}

TEST(DiagnosticsSessionTest, CrashDumpsStayBelowTheSessionDirectory) {
    const std::string session = "C:\\pkg\\logs\\20260307_090501";
    using ce::runtime::CrashDumpDirectory;
    EXPECT_EQ(CrashDumpDirectory(session, ""), session);
    EXPECT_EQ(CrashDumpDirectory(session, "dumps"), session + "\\dumps");
    EXPECT_EQ(CrashDumpDirectory(session, "a\\..\\b"), session + "\\b");
    for (const char* escaping : {".", "..\\x", "a\\..\\..\\x", "C:\\x", "\\x", "D:x"}) {
        SCOPED_TRACE(escaping);
        EXPECT_EQ(CrashDumpDirectory(session, escaping), session);
    }
}

TEST(DiagnosticsSessionTest, CleanupKeepsTheNewestSessionsAndOnlyRemovesSessionArtifacts) {
    ScratchDirectory logs;
    for (int day = 1; day <= 23; ++day) {
        char name[16];
        snprintf(name, sizeof(name), "202603%02d_120000", day);
        std::filesystem::create_directories(logs.path() / name);
    }
    std::filesystem::create_directories(logs.path() / "symbol_store");
    Touch(logs.path() / "old.log");
    Touch(logs.path() / "old.csv");
    Touch(logs.path() / "notes.txt");

    ce::runtime::CleanupOldSessionDirs(logs.path().string(), 20);

    EXPECT_FALSE(std::filesystem::exists(logs.path() / "20260301_120000"));
    EXPECT_FALSE(std::filesystem::exists(logs.path() / "20260303_120000"));
    EXPECT_TRUE(std::filesystem::exists(logs.path() / "20260304_120000")) << "the 20 newest stay";
    EXPECT_TRUE(std::filesystem::exists(logs.path() / "20260323_120000"));
    EXPECT_TRUE(std::filesystem::exists(logs.path() / "symbol_store")) << "not a session directory";
    EXPECT_FALSE(std::filesystem::exists(logs.path() / "old.log"));
    EXPECT_FALSE(std::filesystem::exists(logs.path() / "old.csv"));
    EXPECT_TRUE(std::filesystem::exists(logs.path() / "notes.txt"));
}

TEST(DiagnosticsSessionTest, TheManifestNamesTheControllerAndItsServices) {
    ScratchDirectory session;
    AppConfig config;
    ce::runtime::WriteSessionManifest(session.path().string(), config, true, false);
    std::ifstream file(session.path() / "session_manifest.txt");
    std::stringstream text;
    text << file.rdbuf();
    EXPECT_NE(text.str().find("process_mode=Controller\n"), std::string::npos);
    EXPECT_NE(text.str().find("logger_enabled=1\n"), std::string::npos);
    EXPECT_NE(text.str().find("sensor_enabled=0\n"), std::string::npos);
    EXPECT_NE(text.str().find("build_version="), std::string::npos);
}

}  // namespace
