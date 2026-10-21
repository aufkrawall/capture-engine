#include "test_config_shared.h"

#include "common/ipc/elevation_windows.h"
#include "common/platform/user_editable_access.h"

#include <aclapi.h>

#include <filesystem>
#include <string>

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
