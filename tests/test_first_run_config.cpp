#include "test_config_shared.h"

#include "common/ipc/elevation_windows.h"
#include "common/platform/user_editable_access.h"

#include <aclapi.h>
#include <winioctl.h>

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

// The first-run config.ini and the BUILTIN\Users entry that keeps it editable without administrator rights
// (common/platform/user_editable_access.h). Every scratch folder gets a protected DACL that names only the current
// user, so a Users entry appears exactly when the code under test put it there.

namespace {

enum class UsersEntry { None, Explicit, Inherited };

// The Users entry on `path` that allows writing, if any.
UsersEntry UsersWriteEntry(const std::filesystem::path& path) {
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl,
                              nullptr, &descriptor) != ERROR_SUCCESS)
        return UsersEntry::None;
    UsersEntry entry = UsersEntry::None;
    for (DWORD index = 0; dacl && index < dacl->AceCount && entry == UsersEntry::None; ++index) {
        ACE_HEADER* header = nullptr;
        if (!GetAce(dacl, index, reinterpret_cast<void**>(&header)) || header->AceType != ACCESS_ALLOWED_ACE_TYPE)
            continue;
        auto* allowed = reinterpret_cast<ACCESS_ALLOWED_ACE*>(header);
        if (IsWellKnownSid(&allowed->SidStart, WinBuiltinUsersSid) &&
            (allowed->Mask & FILE_GENERIC_WRITE) == FILE_GENERIC_WRITE)
            entry = (header->AceFlags & INHERITED_ACE) ? UsersEntry::Inherited : UsersEntry::Explicit;
    }
    LocalFree(descriptor);
    return entry;
}

// A folder only the current user can reach. `rights` is the SDDL access of that user's entry.
bool CreatePrivateFolder(const std::filesystem::path& path, const wchar_t* rights) {
    const std::wstring sid = ce::elevation::ProcessUserSid(GetCurrentProcess());
    if (sid.empty())
        return false;
    ce::elevation::Security security(L"D:P(A;OICI;" + std::wstring(rights) + L";;;" + sid + L")");
    return security.Get() && CreateDirectoryW(path.c_str(), security.Get());
}

// Turns the empty folder `path` into a junction (mount point) to `target`; no privilege needed.
bool MakeJunction(const std::filesystem::path& path, const std::filesystem::path& target) {
    struct MountPoint {
        DWORD tag;
        WORD length, reserved, substituteOffset, substituteLength, printOffset, printLength;
        wchar_t names[1];
    };
    const std::wstring printName = target.wstring();
    const std::wstring substitute = L"\\??\\" + printName;
    const size_t bytes = offsetof(MountPoint, names) + (substitute.size() + printName.size() + 2) * sizeof(wchar_t);
    std::vector<uint64_t> storage((bytes + 7) / 8);
    auto* point = reinterpret_cast<MountPoint*>(storage.data());
    point->tag = IO_REPARSE_TAG_MOUNT_POINT;
    point->length = static_cast<WORD>(bytes - 8);
    point->substituteLength = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    point->printOffset = static_cast<WORD>(point->substituteLength + sizeof(wchar_t));
    point->printLength = static_cast<WORD>(printName.size() * sizeof(wchar_t));
    std::memcpy(point->names, substitute.c_str(), point->substituteLength + sizeof(wchar_t));
    std::memcpy(reinterpret_cast<BYTE*>(point->names) + point->printOffset, printName.c_str(),
                point->printLength + sizeof(wchar_t));
    ce::elevation::Handle directory(CreateFileW(path.c_str(), GENERIC_WRITE,
                                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                                nullptr));
    DWORD written = 0;
    return directory && DeviceIoControl(directory.Get(), FSCTL_SET_REPARSE_POINT, point, static_cast<DWORD>(bytes),
                                        nullptr, 0, &written, nullptr);
}

class FirstRunConfigTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        directory_ = std::filesystem::temp_directory_path() /
                     ("first_run_config." + std::to_string(GetCurrentProcessId()) + "." + info->name());
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
        path_ = directory_ / "config.ini";
    }
    void TearDown() override {
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
    }

    std::filesystem::path directory_;
    std::filesystem::path path_;
};

TEST_F(FirstRunConfigTest, AnElevatedFirstRunOpensTheFileToUsers) {
    ASSERT_TRUE(CreatePrivateFolder(directory_, L"FA"));
    EXPECT_EQ(CreateDefaultConfig(path_.string(), true), static_cast<DWORD>(ERROR_SUCCESS));
    EXPECT_EQ(ReadTextFile(path_.string()), ReadTextFile(DefaultTemplatePath()));
    EXPECT_EQ(UsersWriteEntry(path_), UsersEntry::Explicit);
}

TEST_F(FirstRunConfigTest, AnUnelevatedFirstRunKeepsTheFolderInheritance) {
    ASSERT_TRUE(CreatePrivateFolder(directory_, L"FA"));
    EXPECT_EQ(CreateDefaultConfig(path_.string(), false), static_cast<DWORD>(ERROR_SUCCESS));
    EXPECT_EQ(ReadTextFile(path_.string()), ReadTextFile(DefaultTemplatePath()));
    EXPECT_EQ(UsersWriteEntry(path_), UsersEntry::None);
}

TEST_F(FirstRunConfigTest, AnExistingFileIsKeptAsItIs) {
    ASSERT_TRUE(CreatePrivateFolder(directory_, L"FA"));
    WriteTextFile(path_.string(), "[Mine]\r\n");
    EXPECT_EQ(CreateDefaultConfig(path_.string(), true), static_cast<DWORD>(ERROR_FILE_EXISTS));
    EXPECT_EQ(ReadTextFile(path_.string()), "[Mine]\r\n");
    EXPECT_EQ(UsersWriteEntry(path_), UsersEntry::None);
}

// The Program Files case for an unelevated process: the folder lists and reads, but takes no new file.
TEST_F(FirstRunConfigTest, AFolderThatTakesNoNewFileReportsTheErrorAndLeavesNothing) {
    ASSERT_TRUE(CreatePrivateFolder(directory_, L"0x1200a9"));
    EXPECT_EQ(CreateDefaultConfig(path_.string(), false), static_cast<DWORD>(ERROR_ACCESS_DENIED));
    EXPECT_FALSE(std::filesystem::exists(path_));
}

// The same folder through the loader every role uses (media, inject, the overlay policy): the template still applies.
// The parser's per-key fallbacks bound no screenshot hotkey, and the controller ran on the template's settings.
TEST_F(FirstRunConfigTest, LoadConfigInAFolderThatTakesNoNewFileAppliesTheTemplate) {
    ASSERT_TRUE(CreatePrivateFolder(directory_, L"0x1200a9"));
    AppConfig config;
    LoadConfig(path_.string(), config);
    ExpectFreshInstallSettings(config);
    EXPECT_FALSE(std::filesystem::exists(path_));
}

TEST_F(FirstRunConfigTest, LoadConfigInAMissingFolderAppliesTheTemplate) {
    AppConfig config;
    LoadConfig(path_.string(), config);
    ExpectFreshInstallSettings(config);
    EXPECT_FALSE(std::filesystem::exists(directory_));
}

// The shipped template itself: loads with no warning at all (an unknown, misspelled or rejected value is one) and
// writes nothing.
TEST_F(FirstRunConfigTest, TheEmbeddedTemplateLoadsCleanlyWithoutTouchingAnyFile) {
    AppConfig config;
    ConfigDiagnostics diagnostics;
    ASSERT_TRUE(LoadDefaultConfigTemplate(config, "", &diagnostics));
    ExpectFreshInstallSettings(config);
    for (const ConfigDiagnostic& entry : diagnostics.entries)
        ADD_FAILURE() << "[" << entry.section << "] " << entry.key << ": " << entry.message;
    EXPECT_FALSE(std::filesystem::exists(directory_));
}

// The elevation service's restore (CreateDefaultConfigInFolder): the template, opened to users, never over a file.
TEST_F(FirstRunConfigTest, TheServiceRestoreWritesTheTemplateOpenToUsers) {
    ASSERT_TRUE(CreatePrivateFolder(directory_, L"FA"));
    EXPECT_EQ(CreateDefaultConfigInFolder(directory_.wstring()), static_cast<DWORD>(ERROR_SUCCESS));
    EXPECT_EQ(ReadTextFile(path_.string()), ReadTextFile(DefaultTemplatePath()));
    EXPECT_EQ(UsersWriteEntry(path_), UsersEntry::Explicit);
    WriteTextFile(path_.string(), "[Mine]\r\n");
    EXPECT_EQ(CreateDefaultConfigInFolder(directory_.wstring()), static_cast<DWORD>(ERROR_FILE_EXISTS));
    EXPECT_EQ(ReadTextFile(path_.string()), "[Mine]\r\n");
}

TEST_F(FirstRunConfigTest, TheServiceRestoreRefusesAMissingFolder) {
    const DWORD status = CreateDefaultConfigInFolder(directory_.wstring());
    EXPECT_TRUE(status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) << status;
    EXPECT_FALSE(std::filesystem::exists(directory_));
}

// A privileged writer must not be steered by a link: the folder as a junction is refused and nothing lands in its
// target.
TEST_F(FirstRunConfigTest, TheServiceRestoreRefusesAJunctionedFolder) {
    ASSERT_TRUE(CreatePrivateFolder(directory_, L"FA"));
    const std::filesystem::path target = directory_ / "target";
    const std::filesystem::path junction = directory_ / "junction";
    ASSERT_TRUE(CreateDirectoryW(target.c_str(), nullptr));
    ASSERT_TRUE(CreateDirectoryW(junction.c_str(), nullptr));
    ASSERT_TRUE(MakeJunction(junction, target));
    EXPECT_EQ(CreateDefaultConfigInFolder(junction.wstring()), static_cast<DWORD>(ERROR_DIRECTORY));
    EXPECT_FALSE(std::filesystem::exists(target / "config.ini"));
    RemoveDirectoryW(junction.c_str());
}

// A link planted at the name is never followed, by the elevated first run or by the service: CREATE_NEW meets the
// link itself, and its target stays uncreated.
TEST_F(FirstRunConfigTest, ALinkAtTheNameIsNeverFollowed) {
    ASSERT_TRUE(CreatePrivateFolder(directory_, L"FA"));
    const std::filesystem::path target = directory_ / "elsewhere.ini";
    if (!CreateSymbolicLinkW(path_.c_str(), target.c_str(), SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE))
        GTEST_SKIP() << "symbolic links need developer mode or the privilege here (error " << GetLastError() << ")";
    EXPECT_EQ(CreateDefaultConfig(path_.string(), true), static_cast<DWORD>(ERROR_FILE_EXISTS));
    EXPECT_EQ(CreateDefaultConfigInFolder(directory_.wstring()), static_cast<DWORD>(ERROR_FILE_EXISTS));
    EXPECT_FALSE(std::filesystem::exists(target));
    DeleteFileW(path_.c_str());
}

// Setup's `logs`/`captures` case: the folder's entry reaches what the program writes into it later.
TEST_F(FirstRunConfigTest, AFolderEntryReachesFilesCreatedLater) {
    ASSERT_TRUE(CreatePrivateFolder(directory_, L"FA"));
    {
        ce::elevation::Handle folder(CreateFileW(directory_.c_str(), READ_CONTROL | WRITE_DAC,
                                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                                 OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        ASSERT_TRUE(folder);
        EXPECT_EQ(ce::user_access::GrantUsersModify(folder.Get(), true), static_cast<DWORD>(ERROR_SUCCESS));
    }
    WriteTextFile((directory_ / "session.log").string(), "log");
    EXPECT_EQ(UsersWriteEntry(directory_), UsersEntry::Explicit);
    EXPECT_EQ(UsersWriteEntry(directory_ / "session.log"), UsersEntry::Inherited);
}

}  // namespace
