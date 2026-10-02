#include <gtest/gtest.h>

#include <filesystem>
#include "installer/setup.h"
#include <winioctl.h>

// Exercise the production file and shell-link implementations in a disposable
// folder. The unit-test binary otherwise does not link the installer target.
#include "installer/util.cpp"
#include "installer/files.cpp"
#include "installer/shortcuts.cpp"

namespace {

using namespace ce::setup;

class InstallerFilesTest : public testing::Test {
protected:
    void SetUp() override {
        wchar_t temporary[MAX_PATH] = {}, reserved[MAX_PATH] = {};
        ASSERT_NE(GetTempPathW(MAX_PATH, temporary), 0u);
        ASSERT_NE(GetTempFileNameW(temporary, L"cef", 0, reserved), 0u);
        ASSERT_TRUE(DeleteFileW(reserved));
        ASSERT_TRUE(CreateDirectoryW(reserved, nullptr));
        root_ = reserved;
    }

    void TearDown() override {
        for (const auto& alias : aliases_)
            RemoveDirectoryW(alias.c_str());
        std::error_code error;
        std::filesystem::remove_all(root_, error);
        EXPECT_FALSE(error) << error.message();
    }

    std::wstring Folder(const wchar_t* name) {
        const std::wstring path = JoinPath(root_, name);
        EXPECT_TRUE(CreateDirectoryW(path.c_str(), nullptr));
        return path;
    }

    void Write(const std::wstring& path, std::string_view text) {
        DWORD error = 0;
        ASSERT_TRUE(WriteFileAtomically(path, text.data(), text.size(), true, &error)) << error;
    }

    void Junction(const std::wstring& alias, const std::wstring& target) {
        wchar_t system[MAX_PATH] = {};
        ASSERT_NE(GetSystemDirectoryW(system, MAX_PATH), 0u);
        const std::wstring shell = JoinPath(system, L"cmd.exe");
        std::wstring command = L"\"" + shell + L"\" /c mklink /J \"" + alias + L"\" \"" + target + L"\"";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        ASSERT_TRUE(CreateProcessW(shell.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                                   nullptr, root_.c_str(), &startup, &process));
        Handle thread(process.hThread), child(process.hProcess);
        const DWORD waited = WaitForSingleObject(child.Get(), 10000);
        if (waited != WAIT_OBJECT_0) {
            TerminateProcess(child.Get(), ERROR_TIMEOUT);
            WaitForSingleObject(child.Get(), 10000);
        }
        ASSERT_EQ(waited, WAIT_OBJECT_0);
        DWORD code = 1;
        ASSERT_TRUE(GetExitCodeProcess(child.Get(), &code));
        ASSERT_EQ(code, 0u);
        aliases_.push_back(alias);
    }

    void RetagDirectory(const std::wstring& path, const std::wstring& target) {
        struct MountPoint {
            DWORD tag;
            WORD length, reserved, substituteOffset, substituteLength, printOffset, printLength;
            wchar_t names[1];
        };
        const std::wstring substitute = L"\\??\\" + target;
        const size_t nameBytes = (substitute.size() + target.size() + 2) * sizeof(wchar_t);
        const size_t bytes = offsetof(MountPoint, names) + nameBytes;
        std::vector<uint64_t> storage((bytes + 7) / 8);
        auto* point = reinterpret_cast<MountPoint*>(storage.data());
        point->tag = IO_REPARSE_TAG_MOUNT_POINT;
        point->length = static_cast<WORD>(bytes - 8);
        point->substituteLength = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
        point->printOffset = static_cast<WORD>(point->substituteLength + sizeof(wchar_t));
        point->printLength = static_cast<WORD>(target.size() * sizeof(wchar_t));
        std::memcpy(point->names, substitute.c_str(), point->substituteLength + sizeof(wchar_t));
        std::memcpy(reinterpret_cast<BYTE*>(point->names) + point->printOffset, target.c_str(),
                    point->printLength + sizeof(wchar_t));
        Handle directory(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                     nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                     nullptr));
        ASSERT_TRUE(directory.Valid()) << GetLastError();
        DWORD written = 0;
        ASSERT_TRUE(DeviceIoControl(directory.Get(), FSCTL_SET_REPARSE_POINT, point, static_cast<DWORD>(bytes),
                                   nullptr, 0, &written, nullptr)) << GetLastError();
        aliases_.push_back(path);
    }

    std::wstring root_;
    std::vector<std::wstring> aliases_;
};

TEST_F(InstallerFilesTest, CleanupDoesNotTraverseAnInternalJunction) {
    const auto installed = Folder(L"installed");
    const auto external = Folder(L"external");
    const auto canary = JoinPath(external, L"private.cebak");
    Write(canary, "external backup");
    Junction(JoinPath(installed, L"ffmpeg"), external);
    CleanTemporaryFiles(installed, {"ffmpeg/avcodec.dll"});
    EXPECT_TRUE(PathExists(canary));
}

TEST_F(InstallerFilesTest, CleanupStillRemovesItsOwnTemporaryFiles) {
    const auto installed = Folder(L"installed");
    const auto temporary = JoinPath(installed, L"old.dll.cenew");
    const auto userFile = JoinPath(installed, L"notes.txt");
    Write(temporary, "staging");
    Write(userFile, "user data");
    CleanTemporaryFiles(installed, {"old.dll"});
    EXPECT_FALSE(PathExists(temporary));
    EXPECT_TRUE(PathExists(userFile));
}

TEST_F(InstallerFilesTest, GuardPinsInternalDirectoriesUntilCleanupCompletes) {
    const auto installed = Folder(L"installed");
    const auto subdirectory = JoinPath(installed, L"ffmpeg");
    ASSERT_TRUE(CreateDirectoryW(subdirectory.c_str(), nullptr));
    {
        const InstallationPathGuard guard(installed, "ffmpeg/avcodec.dll");
        ASSERT_TRUE(guard.Valid()) << guard.Error();
        EXPECT_FALSE(MoveFileExW(subdirectory.c_str(), JoinPath(installed, L"moved").c_str(), 0));
        EXPECT_EQ(GetLastError(), static_cast<DWORD>(ERROR_SHARING_VIOLATION));
    }
    EXPECT_TRUE(MoveFileExW(subdirectory.c_str(), JoinPath(installed, L"moved").c_str(), 0));
}

TEST_F(InstallerFilesTest, GuardAllowsFileRenamesNeededForRollback) {
    const auto installed = Folder(L"installed");
    const auto backup = JoinPath(installed, L"app.exe.cebak");
    const auto destination = JoinPath(installed, L"app.exe");
    Write(backup, "previous binary");
    const InstallationPathGuard guard(installed, "app.exe");
    ASSERT_TRUE(guard.Valid());
    ASSERT_TRUE(RenameFileInDirectory(guard.ParentHandle(), backup, destination)) << GetLastError();
    EXPECT_FALSE(PathExists(backup));
    std::string content;
    ASSERT_TRUE(ReadWholeFile(destination, &content, 128));
    EXPECT_EQ(content, "previous binary");
}

TEST_F(InstallerFilesTest, ChangingARootAliasCannotRedirectGuardedDeletion) {
    const auto installed = Folder(L"installed");
    const auto external = Folder(L"external");
    const auto alias = JoinPath(root_, L"alias");
    Write(JoinPath(installed, L"app.exe.cenew"), "owned staging");
    Write(JoinPath(external, L"app.exe.cenew"), "external file");
    Junction(alias, installed);
    const InstallationPathGuard guard(alias, "app.exe.cenew");
    ASSERT_TRUE(guard.Valid());
    ASSERT_TRUE(RemoveDirectoryW(alias.c_str()));
    Junction(alias, external);
    EXPECT_TRUE(DeleteFileInDirectory(guard.ParentHandle(), JoinPath(alias, L"app.exe.cenew")));
    EXPECT_FALSE(PathExists(JoinPath(installed, L"app.exe.cenew")));
    EXPECT_TRUE(PathExists(JoinPath(external, L"app.exe.cenew")));
}

TEST_F(InstallerFilesTest, RetaggingAPinnedDirectoryCannotRedirectCleanup) {
    const auto installed = Folder(L"installed");
    const auto external = Folder(L"external");
    const auto canary = JoinPath(external, L"private.cebak");
    Write(canary, "external file");
    const InstallationPathGuard guard(installed, "private.cebak");
    ASSERT_TRUE(guard.Valid());
    RetagDirectory(installed, external);
    ASSERT_TRUE(IsReparsePoint(installed));
    bool deferred = false;
    EXPECT_FALSE(RemoveOrRenameAway(JoinPath(installed, L"private.cebak"), &deferred, guard.ParentHandle()));
    EXPECT_EQ(GetLastError(), static_cast<DWORD>(ERROR_CANT_RESOLVE_FILENAME));
    EXPECT_TRUE(PathExists(canary));
    EXPECT_FALSE(deferred);
}

TEST_F(InstallerFilesTest, InternalDirectoryLookupUsesTheRetainedRoot) {
    const auto installed = Folder(L"installed");
    const auto external = Folder(L"external");
    ASSERT_TRUE(CreateDirectoryW(JoinPath(external, L"ffmpeg").c_str(), nullptr));
    const InstallationPathGuard guard(installed, "app.exe");
    ASSERT_TRUE(guard.Valid());
    RetagDirectory(installed, external);
    ASSERT_TRUE(IsReparsePoint(installed));
    const Handle child = OpenChildDirectoryNoFollow(guard.RootHandle(), L"ffmpeg");
    const DWORD error = GetLastError();
    EXPECT_FALSE(child.Valid());
    EXPECT_EQ(error, static_cast<DWORD>(ERROR_CANT_RESOLVE_FILENAME));
}

TEST_F(InstallerFilesTest, AliasedUpdateKeepsTheNewFilesAndManifest) {
    const auto installed = Folder(L"installed");
    const auto alias = JoinPath(root_, L"alias");
    Junction(alias, installed);
    Write(JoinPath(installed, kAppExe), "new binary");
    Write(JoinPath(installed, kManifestFile), SerializeManifest("new", {"captureengine.exe"}));
    ASSERT_EQ(CompareInstallationDirectories(installed, alias), DirectoryComparison::Same);
    std::vector<std::wstring> warnings;
    bool reboot = false;
    RemovePreviousInstallation(installed, alias, &warnings, &reboot);
    EXPECT_TRUE(PathExists(JoinPath(alias, kAppExe)));
    EXPECT_TRUE(PathExists(JoinPath(alias, kManifestFile)));
    EXPECT_TRUE(warnings.empty());
    EXPECT_FALSE(reboot);
}

TEST_F(InstallerFilesTest, RelocationRemovesOnlyTheDistinctPreviousInstallation) {
    const auto previous = Folder(L"previous");
    const auto installed = Folder(L"installed");
    Write(JoinPath(previous, kAppExe), "old binary");
    Write(JoinPath(previous, kManifestFile), SerializeManifest("old", {"captureengine.exe"}));
    Write(JoinPath(previous, L"config.ini"), "user settings");
    Write(JoinPath(installed, kAppExe), "new binary");
    ASSERT_EQ(CompareInstallationDirectories(previous, installed), DirectoryComparison::Different);
    std::vector<std::wstring> warnings;
    bool reboot = false;
    RemovePreviousInstallation(previous, installed, &warnings, &reboot);
    EXPECT_FALSE(PathExists(JoinPath(previous, kAppExe)));
    EXPECT_TRUE(PathExists(JoinPath(previous, L"config.ini")));
    EXPECT_TRUE(PathExists(JoinPath(installed, kAppExe)));
    EXPECT_TRUE(warnings.empty());
}

TEST_F(InstallerFilesTest, UnverifiableRelocationKeepsThePreviousInstallation) {
    const auto previous = Folder(L"previous");
    Write(JoinPath(previous, kAppExe), "old binary");
    Write(JoinPath(previous, kManifestFile), SerializeManifest("old", {"captureengine.exe"}));
    std::vector<std::wstring> warnings;
    bool reboot = false;
    RemovePreviousInstallation(previous, JoinPath(root_, L"missing"), &warnings, &reboot);
    EXPECT_TRUE(PathExists(JoinPath(previous, kAppExe)));
    EXPECT_TRUE(PathExists(JoinPath(previous, kManifestFile)));
    EXPECT_EQ(warnings.size(), 1u);
}

TEST_F(InstallerFilesTest, DisabledShortcutIsRemovedAfterRelocation) {
    const auto previous = Folder(L"previous");
    const auto installed = Folder(L"installed");
    Write(JoinPath(previous, kAppExe), "old binary");
    const auto link = JoinPath(root_, L"Capture Engine.lnk");
    DWORD error = 0;
    ASSERT_TRUE(CreateAppShortcut(link, previous, &error)) << error;
    ASSERT_TRUE(OwnedShortcutExists(link, previous));
    EXPECT_TRUE(RemoveOwnedShortcut(link, installed, previous));
    EXPECT_FALSE(PathExists(link));
}

TEST_F(InstallerFilesTest, ShortcutRemovalKeepsForeignLinksAndHandlesCurrentLinks) {
    const auto previous = Folder(L"previous");
    const auto installed = Folder(L"installed");
    const auto foreign = Folder(L"foreign");
    const auto link = JoinPath(root_, L"Capture Engine.lnk");
    DWORD error = 0;
    ASSERT_TRUE(CreateAppShortcut(link, foreign, &error)) << error;
    EXPECT_TRUE(RemoveOwnedShortcut(link, installed, previous));
    EXPECT_TRUE(PathExists(link));
    ASSERT_TRUE(CreateAppShortcut(link, installed, &error)) << error;
    EXPECT_TRUE(RemoveOwnedShortcut(link, installed, previous));
    EXPECT_FALSE(PathExists(link));
}

}  // namespace
