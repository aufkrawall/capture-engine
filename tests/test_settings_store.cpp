#include "test_config_shared.h"

#include "common/config/config_ini_reader.h"
#include "common/ipc/elevation_windows.h"
#include "common/logging/logging.h"
#include "runtime/settings/settings_store.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// SettingsStore: transactional commits and external-edit polling over a temporary config.ini. Time comes from an
// injected clock, so the debounce is driven by NextPollDelayMs instead of sleeps. Every file lives in a directory
// under the temp path; the repository config.ini is never touched.

namespace {
using ce::runtime::CommitResult;
using ce::runtime::CommitStatus;
using ce::runtime::SettingsEdit;
using ce::runtime::SettingsStore;

uint64_t gNowMs = 0;
uint64_t FakeNowMs() noexcept {
    return gNowMs;
}

std::string ReadAll(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

void WriteAll(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << bytes;
}

SettingsEdit Set(const char* section, const char* key, const char* value) {
    return SettingsEdit{section, key, std::optional<std::string>(value)};
}

SettingsEdit Remove(const char* section, const char* key) {
    return SettingsEdit{section, key, std::nullopt};
}

size_t CountErrors(const ConfigDiagnostics& diagnostics) {
    size_t count = 0;
    for (const ConfigDiagnostic& entry : diagnostics.entries) {
        if (entry.severity == ConfigDiagnosticSeverity::kError)
            ++count;
    }
    return count;
}

class SettingsStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        gNowMs = 1000;
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        directory_ = std::filesystem::temp_directory_path() /
                     ("settings_store." + std::to_string(GetCurrentProcessId()) + "." + info->name());
        std::filesystem::remove_all(directory_);
        std::filesystem::create_directories(directory_);
        path_ = directory_ / "config.ini";
    }
    void TearDown() override {
        std::error_code ignored;
        SetFileAttributesW(path_.c_str(), FILE_ATTRIBUTE_NORMAL);
        std::filesystem::remove_all(directory_, ignored);
    }

    std::unique_ptr<SettingsStore> Open(const std::string& text) {
        WriteAll(path_, text);
        return std::make_unique<SettingsStore>(path_.wstring(), FakeNowMs);
    }

    // Advances the clock to the next due check, then polls: the change is seen on the first check and published
    // on the second, once it is stable (ConfigurationState's rule).
    bool PollWhenDue(SettingsStore& store) {
        gNowMs += store.NextPollDelayMs();
        return store.Poll();
    }

    // The administrator-only folder an unelevated process meets under Program Files: it lists and reads, takes no
    // new file. The owner keeps WRITE_DAC, which the fake elevation service below uses.
    void MakeDirectoryReadOnly() {
        const std::wstring sid = ce::elevation::ProcessUserSid(GetCurrentProcess());
        ASSERT_FALSE(sid.empty());
        ce::elevation::Security readOnly(L"D:P(A;OICI;0x1200a9;;;" + sid + L")");
        ASSERT_NE(readOnly.Get(), nullptr);
        ASSERT_TRUE(std::filesystem::remove(directory_));
        ASSERT_TRUE(CreateDirectoryW(directory_.c_str(), readOnly.Get()));
    }

    // What the first two due Polls log: the first reports the construction's first-run outcome.
    std::string FirstPollsLog(SettingsStore& store) {
        const std::filesystem::path logFile =
            std::filesystem::temp_directory_path() /
            ("settings_store_creation." + std::to_string(GetCurrentProcessId()) + ".log");
        std::filesystem::remove(logFile);
        Log_Init(logFile.string(), LogLevel::Debug);
        PollWhenDue(store);
        PollWhenDue(store);
        Log_Shutdown();
        const std::string log = ReadAll(logFile);
        std::filesystem::remove(logFile);
        return log;
    }

    std::filesystem::path directory_;
    std::filesystem::path path_;
};

TEST_F(SettingsStoreTest, CommitPublishesTheNewSnapshotRevisionAndValue) {
    auto store = Open("[Video]\r\nfps=60\r\n");
    const uint64_t base = store->Revision();
    const auto before = store->Current();
    ASSERT_EQ(before->video.fps, 60);
    EXPECT_EQ(store->Value("Video", "fps"), "60");
    EXPECT_EQ(store->Value("Video", "missing"), std::nullopt);

    const CommitResult result = store->Commit({Set("Video", "fps", "90")}, base);
    EXPECT_EQ(result.status, CommitStatus::Committed);
    EXPECT_EQ(result.revision, base + 1);
    EXPECT_EQ(store->Revision(), base + 1);
    EXPECT_EQ(store->Current()->video.fps, 90);
    EXPECT_EQ(store->Value("Video", "fps"), "90");
    EXPECT_EQ(before->video.fps, 60) << "a snapshot never changes after it was handed out";
    EXPECT_EQ(ReadAll(path_), "[Video]\r\nfps=90\r\n");
}

TEST_F(SettingsStoreTest, CommentsOrderAndUnknownLinesSurviveAnEditByteForByte) {
    auto store = Open("; leading comment\r\n[Video]\r\n; keep me\r\nfps = 60 ; inline\r\n\r\ncodec=hevc\r\n"
                      "[Audio]\r\nunknown line without equals\r\nvolume=50\nlast=1");
    const CommitResult result = store->Commit({Set("Video", "fps", "90")}, store->Revision());
    ASSERT_EQ(result.status, CommitStatus::Committed);
    EXPECT_EQ(ReadAll(path_), "; leading comment\r\n[Video]\r\n; keep me\r\nfps = 90\r\n\r\ncodec=hevc\r\n"
                              "[Audio]\r\nunknown line without equals\r\nvolume=50\nlast=1");
}

TEST_F(SettingsStoreTest, ByteOrderMarkIsKeptWithAndWithoutIt) {
    auto withBom = Open("\xEF\xBB\xBF[Video]\nfps=60\n");
    ASSERT_EQ(withBom->Commit({Set("Video", "fps", "90")}, withBom->Revision()).status, CommitStatus::Committed);
    EXPECT_EQ(ReadAll(path_), "\xEF\xBB\xBF[Video]\nfps=90\n");
    EXPECT_EQ(withBom->Current()->video.fps, 90);

    auto plain = Open("[Video]\nfps=60\n");
    ASSERT_EQ(plain->Commit({Set("Video", "fps", "90")}, plain->Revision()).status, CommitStatus::Committed);
    EXPECT_EQ(ReadAll(path_), "[Video]\nfps=90\n");
}

TEST_F(SettingsStoreTest, InvalidValueIsRefusedAndNothingIsPublished) {
    auto store = Open("[Capture]\r\ncapture_method=wgc\r\n");
    const auto before = store->Current();
    const uint64_t base = store->Revision();

    const CommitResult result = store->Commit({Set("Capture", "capture_method", "bogus_method")}, base);
    EXPECT_EQ(result.status, CommitStatus::Invalid);
    EXPECT_GT(CountErrors(result.diagnostics), 0u);
    bool namesTheKey = false;
    for (const ConfigDiagnostic& entry : result.diagnostics.entries)
        namesTheKey = namesTheKey || entry.key == "capture_method";
    EXPECT_TRUE(namesTheKey);
    EXPECT_EQ(result.revision, base);
    EXPECT_EQ(store->Revision(), base);
    EXPECT_EQ(store->Current(), before);
    EXPECT_EQ(ReadAll(path_), "[Capture]\r\ncapture_method=wgc\r\n");
}

TEST_F(SettingsStoreTest, ExternalEditBetweenBaseAndCommitIsAConflict) {
    auto store = Open("[Video]\r\nfps=60\r\n");
    const uint64_t base = store->Revision();
    WriteAll(path_, "[Video]\r\nfps=75\r\nextra=1\r\n");

    const CommitResult result = store->Commit({Set("Video", "fps", "90")}, base);
    EXPECT_EQ(result.status, CommitStatus::Conflict);
    EXPECT_EQ(store->Revision(), base);
    EXPECT_EQ(ReadAll(path_), "[Video]\r\nfps=75\r\nextra=1\r\n") << "a conflict never writes";
}

TEST_F(SettingsStoreTest, ExternalEditWithTheSameSizeAndWriteTimeIsStillAConflict) {
    auto store = Open("[Video]\r\nfps=60\r\n");
    const uint64_t base = store->Revision();
    const auto stamp = std::filesystem::last_write_time(path_);
    WriteAll(path_, "[Video]\r\nfps=61\r\n");
    std::filesystem::last_write_time(path_, stamp);

    const CommitResult result = store->Commit({Set("Video", "fps", "90")}, base);
    EXPECT_EQ(result.status, CommitStatus::Conflict) << "the content hash catches what size and time cannot";
    EXPECT_EQ(ReadAll(path_), "[Video]\r\nfps=61\r\n");
}

TEST_F(SettingsStoreTest, SelfWriteIsNotPublishedAgainByPoll) {
    auto store = Open("[Video]\r\nfps=60\r\n");
    const uint64_t base = store->Revision();
    ASSERT_EQ(store->Commit({Set("Video", "fps", "90")}, base).status, CommitStatus::Committed);
    EXPECT_FALSE(PollWhenDue(*store));
    EXPECT_FALSE(PollWhenDue(*store));
    EXPECT_EQ(store->Revision(), base + 1);
    EXPECT_EQ(store->Current()->video.fps, 90);
}

TEST_F(SettingsStoreTest, StableExternalEditIsPublishedByPollAfterTheDebounce) {
    auto store = Open("[Video]\r\nfps=60\r\n");
    const uint64_t base = store->Revision();
    WriteAll(path_, "[Video]\r\nfps=75\r\n");

    EXPECT_FALSE(PollWhenDue(*store)) << "the first check only notices the change";
    EXPECT_TRUE(PollWhenDue(*store)) << "the second check sees it stable";
    EXPECT_EQ(store->Revision(), base + 1);
    EXPECT_EQ(store->Current()->video.fps, 75);
    EXPECT_EQ(store->Value("Video", "fps"), "75");
}

TEST_F(SettingsStoreTest, ExternalInvalidEditKeepsThePublishedSnapshot) {
    auto store = Open("[Video]\r\nfps=60\r\n");
    const auto before = store->Current();
    const uint64_t base = store->Revision();
    WriteAll(path_, "[Capture]\r\ncapture_method=bogus_method\r\n");

    EXPECT_FALSE(PollWhenDue(*store));
    EXPECT_FALSE(PollWhenDue(*store)) << "a stable invalid candidate is refused";
    EXPECT_EQ(store->Current(), before);
    EXPECT_EQ(store->Revision(), base);

    EXPECT_FALSE(PollWhenDue(*store)) << "the refused identity is settled, not re-validated on every check";
    EXPECT_EQ(store->Revision(), base);

    WriteAll(path_, "[Video]\r\nfps=80\r\n");
    EXPECT_FALSE(PollWhenDue(*store));
    EXPECT_TRUE(PollWhenDue(*store));
    EXPECT_EQ(store->Current()->video.fps, 80);
}

TEST_F(SettingsStoreTest, ARefusedExternalEditIsReportedOnceAndAPublishedOneIsNot) {
    auto store = Open("[Video]\r\nfps=60\r\n");
    EXPECT_EQ(store->TakeRejection(), std::nullopt) << "nothing has been refused yet";

    WriteAll(path_, "[Capture]\r\ncapture_method=bogus_method\r\n");
    EXPECT_FALSE(PollWhenDue(*store));
    EXPECT_FALSE(PollWhenDue(*store));
    const std::optional<std::string> refusal = store->TakeRejection();
    ASSERT_TRUE(refusal.has_value());
    if (!refusal)
        return;
    EXPECT_NE(refusal.value().find("[Capture]"), std::string::npos) << refusal.value();
    EXPECT_EQ(store->TakeRejection(), std::nullopt) << "a refusal is handed out once";

    WriteAll(path_, "[Video]\r\nfps=80\r\n");
    EXPECT_FALSE(PollWhenDue(*store));
    EXPECT_TRUE(PollWhenDue(*store));
    EXPECT_EQ(store->TakeRejection(), std::nullopt) << "a published edit is not a refusal";
}

TEST_F(SettingsStoreTest, CheckValidatesLikeCommitAndWritesNothing) {
    auto store = Open("[Video]\r\nfps=60\r\n");
    const uint64_t base = store->Revision();

    const CommitResult valid = store->Check({Set("Video", "fps", "90")}, base);
    EXPECT_EQ(valid.status, CommitStatus::Committed);
    EXPECT_EQ(valid.revision, base);
    EXPECT_EQ(ReadAll(path_), "[Video]\r\nfps=60\r\n") << "a check never writes";
    EXPECT_EQ(store->Revision(), base);
    EXPECT_EQ(store->Current()->video.fps, 60);

    const CommitResult invalid = store->Check({Set("Capture", "capture_method", "bogus_method")}, base);
    EXPECT_EQ(invalid.status, CommitStatus::Invalid);
    EXPECT_GT(CountErrors(invalid.diagnostics), 0u);

    EXPECT_EQ(store->Check({Set("Video", "fps", "90")}, base + 5).status, CommitStatus::Conflict);

    EXPECT_EQ(store->Commit({Set("Video", "fps", "90")}, base).status, CommitStatus::Committed)
        << "the commit that the checks predicted still goes through";
}

TEST_F(SettingsStoreTest, CheckReportsAnExternalWriteAsAConflictLikeCommit) {
    auto store = Open("[Video]\r\nfps=60\r\n");
    const uint64_t base = store->Revision();
    WriteAll(path_, "[Video]\r\nfps=70\r\n");
    EXPECT_EQ(store->Check({Set("Video", "fps", "90")}, base).status, CommitStatus::Conflict);
}

TEST_F(SettingsStoreTest, RemovingAKeyDeletesOnlyItsLine) {
    auto store = Open("[Video]\r\nfps=60\r\ncodec=hevc\r\n");
    ASSERT_EQ(store->Commit({Remove("Video", "fps")}, store->Revision()).status, CommitStatus::Committed);
    EXPECT_EQ(store->Value("Video", "fps"), std::nullopt);
    EXPECT_EQ(ReadAll(path_), "[Video]\r\ncodec=hevc\r\n");
}

TEST_F(SettingsStoreTest, MissingSectionIsCreatedAtTheEnd) {
    auto store = Open("[Video]\r\nfps=60\r\n");
    ASSERT_EQ(store->Commit({Set("Audio", "volume", "50")}, store->Revision()).status, CommitStatus::Committed);
    EXPECT_EQ(ReadAll(path_), "[Video]\r\nfps=60\r\n\r\n[Audio]\r\nvolume=50\r\n");
    EXPECT_EQ(store->Value("Audio", "volume"), "50");
}

TEST_F(SettingsStoreTest, MultiLineListKeysAreNeverEditedLineWise) {
    const std::string text = "[Injection]\r\nwhitelist=(\r\n  game.exe\r\n)\r\n";
    auto store = Open(text);

    const CommitResult edited = store->Commit({Set("Injection", "whitelist", "x.exe")}, store->Revision());
    EXPECT_EQ(edited.status, CommitStatus::Invalid);
    EXPECT_GT(CountErrors(edited.diagnostics), 0u);
    EXPECT_EQ(store->Commit({Remove("Injection", "whitelist")}, store->Revision()).status, CommitStatus::Invalid);
    // A new key inserted after the list would end it and orphan its entries.
    EXPECT_EQ(store->Commit({Set("Injection", "dll_injection", "never")}, store->Revision()).status,
              CommitStatus::Invalid);
    EXPECT_EQ(ReadAll(path_), text);
    EXPECT_EQ(store->Revision(), 1u);

    // A key that follows the list is fine: the insertion point is after it.
    auto followed = Open("[Injection]\r\nwhitelist=(\r\n  game.exe\r\n)\r\nfps=60\r\n");
    EXPECT_EQ(followed->Commit({Set("Injection", "dll_injection", "never")}, followed->Revision()).status,
              CommitStatus::Committed);
}

TEST_F(SettingsStoreTest, NonUtf8FileIsNeverWrittenTo) {
    const std::string ansi = "[Video]\r\nfps=60\r\n; caf\xE9\r\n";
    auto store = Open(ansi);
    const CommitResult result = store->Commit({Set("Video", "fps", "90")}, store->Revision());
    EXPECT_EQ(result.status, CommitStatus::Invalid);
    EXPECT_EQ(ReadAll(path_), ansi);
}

TEST_F(SettingsStoreTest, CommitThatChangesNothingWritesNothing) {
    auto store = Open("[Video]\r\nfps=60\r\n");
    const auto stamp = std::filesystem::last_write_time(path_);
    const CommitResult result = store->Commit({Set("Video", "fps", "60")}, store->Revision());
    EXPECT_EQ(result.status, CommitStatus::Committed);
    EXPECT_EQ(result.revision, 1u);
    EXPECT_EQ(store->Revision(), 1u);
    EXPECT_EQ(std::filesystem::last_write_time(path_), stamp);
}

TEST_F(SettingsStoreTest, StaleBaseRevisionIsAConflictAfterAnotherCommit) {
    auto store = Open("[Video]\r\nfps=60\r\n");
    const uint64_t base = store->Revision();
    ASSERT_EQ(store->Commit({Set("Video", "fps", "90")}, base).status, CommitStatus::Committed);

    const CommitResult stale = store->Commit({Set("Video", "fps", "120")}, base);
    EXPECT_EQ(stale.status, CommitStatus::Conflict);
    EXPECT_EQ(store->Revision(), base + 1);
    EXPECT_EQ(store->Current()->video.fps, 90);
}

TEST_F(SettingsStoreTest, FailedReplaceIsAnIoErrorAndLeavesNoTemporaryFile) {
    auto store = Open("[Video]\r\nfps=60\r\n");
    ASSERT_TRUE(SetFileAttributesW(path_.c_str(), FILE_ATTRIBUTE_READONLY));
    const CommitResult result = store->Commit({Set("Video", "fps", "90")}, store->Revision());
    EXPECT_EQ(result.status, CommitStatus::IoError);
    EXPECT_EQ(store->Revision(), 1u);
    EXPECT_EQ(ReadAll(path_), "[Video]\r\nfps=60\r\n");
    for (const auto& entry : std::filesystem::directory_iterator(directory_))
        EXPECT_EQ(entry.path().filename().string().find("config.ini.tmp-"), std::string::npos);
}

TEST_F(SettingsStoreTest, MissingFileGetsTheDefaultTemplate) {
    ASSERT_FALSE(std::filesystem::exists(path_));
    SettingsStore store(path_.wstring(), FakeNowMs);
    EXPECT_TRUE(std::filesystem::exists(path_));
    EXPECT_EQ(store.Revision(), 1u);
    EXPECT_TRUE(store.Value("Video", "fps").has_value());
    ExpectFreshInstallSettings(*store.Current());
}

// An administrator-only folder (Program Files) after the user deleted config.ini: nothing unelevated can put the file
// back, so the store publishes the embedded template's settings - the bare defaults bound no hotkey, so recording and
// screenshots were dead (2026-10-24) - and its first Poll - the log does not exist at construction - says why.
TEST_F(SettingsStoreTest, MissingFileThatCannotBeCreatedPublishesTheTemplateAndIsReportedOnce) {
    MakeDirectoryReadOnly();
    SettingsStore store(path_.wstring(), FakeNowMs);
    EXPECT_FALSE(std::filesystem::exists(path_));
    EXPECT_EQ(store.Revision(), 1u);
    ExpectFreshInstallSettings(*store.Current());
    EXPECT_FALSE(store.Value("Video", "fps").has_value()) << "no file: no stored text";

    const std::string log = FirstPollsLog(store);
    EXPECT_EQ(store.Revision(), 1u);
    const std::string line = "[Settings] config.ini is missing and could not be created (error=5,";
    const size_t first = log.find(line);
    EXPECT_NE(first, std::string::npos) << log;
    EXPECT_EQ(log.find(line, first + 1), std::string::npos) << log;
    EXPECT_NE(log.find("service=not asked", first), std::string::npos) << log;
    EXPECT_NE(log.find("the built-in first-run template applies until it exists", first), std::string::npos) << log;
}

// The same folder with the elevation service at hand (here a fake that does what the service does: it creates the file
// with rights this process lacks). The restored file is what the store publishes, from the start.
TEST_F(SettingsStoreTest, AFolderThatRefusesTheFileAsksTheRestorerAndPublishesItsFile) {
    MakeDirectoryReadOnly();
    int calls = 0;
    const auto restore = [&]() -> unsigned long {
        ++calls;
        const std::wstring sid = ce::elevation::ProcessUserSid(GetCurrentProcess());
        ce::elevation::Security writable(L"D:P(A;OICI;FA;;;" + sid + L")");
        if (SetFileSecurityW(directory_.c_str(), DACL_SECURITY_INFORMATION, writable.Descriptor()) == FALSE)
            return GetLastError();
        WriteAll(path_, "[Video]\r\nfps=77\r\n");
        return ERROR_SUCCESS;
    };
    SettingsStore store(path_.wstring(), FakeNowMs, restore);
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(store.Current()->video.fps, 77);
    EXPECT_EQ(store.Value("Video", "fps"), "77");

    const std::string log = FirstPollsLog(store);
    EXPECT_NE(log.find("[Settings] config.ini was missing; the elevation service restored it (result=0"),
              std::string::npos)
        << log;
    EXPECT_EQ(log.find("could not be created"), std::string::npos) << log;
}

// No service (off, absent or too old): the template applies, and the warning names what the service answered.
TEST_F(SettingsStoreTest, ARestorerThatFailsLeavesTheTemplateAndNamesItsAnswer) {
    MakeDirectoryReadOnly();
    SettingsStore store(path_.wstring(), FakeNowMs, [] { return static_cast<unsigned long>(ERROR_SERVICE_NOT_ACTIVE); });
    EXPECT_FALSE(std::filesystem::exists(path_));
    ExpectFreshInstallSettings(*store.Current());
    const std::string log = FirstPollsLog(store);
    EXPECT_NE(log.find("could not be created (error=5,"), std::string::npos) << log;
    EXPECT_NE(log.find("service=1062)"), std::string::npos) << log;
}

// The service restores its own installation's file: a success that leaves no file at this path is no restore.
TEST_F(SettingsStoreTest, ARestoreThatLeavesNoFileHereStillReportsTheMissingFile) {
    MakeDirectoryReadOnly();
    SettingsStore store(path_.wstring(), FakeNowMs, [] { return static_cast<unsigned long>(ERROR_SUCCESS); });
    ExpectFreshInstallSettings(*store.Current());
    const std::string log = FirstPollsLog(store);
    EXPECT_NE(log.find("could not be created (error=5,"), std::string::npos) << log;
    EXPECT_NE(log.find("service=0)"), std::string::npos) << log;
    EXPECT_EQ(log.find("restored it"), std::string::npos) << log;
}

// A folder the process may write never involves the service.
TEST_F(SettingsStoreTest, AWritableFolderNeverAsksTheRestorer) {
    SettingsStore store(path_.wstring(), FakeNowMs, []() -> unsigned long {
        ADD_FAILURE() << "the restorer is only for a folder that refuses the file";
        return ERROR_SUCCESS;
    });
    EXPECT_TRUE(std::filesystem::exists(path_));
    ExpectFreshInstallSettings(*store.Current());
}

// An empty config.ini never becomes coherent (a writer truncates first), so the store keeps what it published at
// construction: the template, not the bare defaults.
TEST_F(SettingsStoreTest, EmptyFileAtStartupPublishesTheTemplate) {
    auto store = Open("");
    ExpectFreshInstallSettings(*store->Current());
}

TEST_F(SettingsStoreTest, ValidateAcceptsTheShippedTemplate) {
    const std::string templateText = ReadTextFile(DefaultTemplatePath());
    ASSERT_FALSE(templateText.empty());
    const ConfigDiagnostics diagnostics = SettingsStore::Validate(ce::config_text::ParseUtf8Ini(templateText));
    ASSERT_EQ(CountErrors(diagnostics), 0u);
}

TEST_F(SettingsStoreTest, ValidateReportsRejectedValuesAsErrorsAndUnknownKeysAsWarnings) {
    const ConfigDiagnostics rejected =
        SettingsStore::Validate(ce::config_text::ParseUtf8Ini("[Capture]\r\ncapture_method=bogus_method\r\n"));
    EXPECT_GE(CountErrors(rejected), 1u);

    const ConfigDiagnostics unknown =
        SettingsStore::Validate(ce::config_text::ParseUtf8Ini("[Video]\r\nfps=60\r\nretired_key=1\r\n"));
    EXPECT_EQ(CountErrors(unknown), 0u);
    EXPECT_GT(unknown.Count(ConfigDiagnosticKind::kUnknownKey), 0u);
}

}  // namespace
