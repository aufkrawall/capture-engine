// File operations shared by setup (updates, moves) and the uninstaller.
//
// Nothing here deletes recursively. An installation is removed by walking the
// manifest it wrote, so a file that was never ours - or a folder the user put
// something into - survives every update and uninstall.

#include "setup.h"

#include <set>

namespace ce::setup {
namespace {

constexpr uint64_t kManifestLimit = 4u << 20;

bool IsMissingError(DWORD error) {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

}  // namespace

bool IsReparsePoint(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

bool HasReparseAncestor(const std::wstring& directory, const std::string& relative) {
    std::wstring current = directory;
    size_t start = 0;
    while (true) {
        const size_t slash = relative.find('/', start);
        const std::string component = relative.substr(start, slash == std::string::npos ? slash : slash - start);
        current = JoinPath(current, Widen(component));
        // The last component is the file itself: a link there is deleted as a
        // link by DeleteFileW and never followed.
        if (slash == std::string::npos)
            return false;
        if (IsReparsePoint(current))
            return true;
        start = slash + 1;
    }
}

bool LoadManifest(const std::wstring& directory, Manifest* manifest) {
    std::string text;
    if (!ReadWholeFile(JoinPath(directory, kManifestFile), &text, kManifestLimit))
        return false;
    *manifest = ParseManifest(text);
    return manifest->valid;
}

bool RemoveOrRenameAway(const std::wstring& path, bool* deferredToReboot) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return IsMissingError(GetLastError());
    if (attributes & FILE_ATTRIBUTE_DIRECTORY)
        return false;
    if (attributes & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_SYSTEM))
        SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (DeleteFileW(path.c_str()) || IsMissingError(GetLastError()))
        return true;
    const DWORD deleteError = GetLastError();
    // In use: a running image cannot be deleted but can be renamed. Moving it
    // aside frees the name for the new file immediately.
    std::wstring aside = path + kBackupSuffixWide;
    for (int attempt = 0; PathExists(aside) && attempt < 100; ++attempt) {
        if (DeleteFileW(aside.c_str()))
            break;
        aside = path + L"." + std::to_wstring(GetTickCount64() + static_cast<ULONGLONG>(attempt)) + kBackupSuffixWide;
    }
    if (!MoveFileExW(path.c_str(), aside.c_str(), MOVEFILE_WRITE_THROUGH)) {
        Log("files: %s is in use and cannot be moved aside (delete error %lu, rename error %lu)",
            Narrow(path).c_str(), static_cast<unsigned long>(deleteError), GetLastError());
        return false;
    }
    if (!MoveFileExW(aside.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT))
        Log("files: could not schedule %s for deletion (error %lu)", Narrow(aside).c_str(), GetLastError());
    Log("files: %s was in use; moved aside and scheduled for deletion at restart", Narrow(path).c_str());
    if (deferredToReboot)
        *deferredToReboot = true;
    return true;
}

void RemoveManifestFiles(const std::wstring& directory, const std::vector<std::string>& files,
                         std::vector<std::wstring>* warnings, bool* rebootRecommended) {
    for (const std::string& relative : files) {
        // The manifest is data on disk; user data is never removable through it.
        if (!IsSafeRelativePath(relative) || IsUserDataPath(relative))
            continue;
        if (HasReparseAncestor(directory, relative)) {
            Log("files: skipping %s (a folder on its path is a link)", relative.c_str());
            if (warnings)
                warnings->push_back(L"Left " + Widen(relative) + L" in place because its folder is a link.");
            continue;
        }
        const std::wstring path = JoinPath(directory, Widen(relative));
        bool deferred = false;
        if (!RemoveOrRenameAway(path, &deferred)) {
            Log("files: could not remove %s", relative.c_str());
            if (warnings)
                warnings->push_back(L"Could not remove " + Widen(relative) + L" (in use?).");
        } else if (deferred && rebootRecommended) {
            *rebootRecommended = true;
        }
    }
}

void RemoveEmptyDirectories(const std::wstring& directory, const std::vector<std::string>& files) {
    for (const std::string& relative : ParentDirectories(files)) {
        if (IsUserDataPath(relative) || HasReparseAncestor(directory, relative + "/x"))
            continue;
        // RemoveDirectory refuses a folder that still holds anything.
        RemoveDirectoryW(JoinPath(directory, Widen(relative)).c_str());
    }
}

void CleanTemporaryFiles(const std::wstring& directory, const std::vector<std::string>& files) {
    std::vector<std::string> folders = ParentDirectories(files);
    folders.push_back(std::string());
    for (const std::string& relative : folders) {
        const std::wstring folder = relative.empty() ? directory : JoinPath(directory, Widen(relative));
        WIN32_FIND_DATAW data{};
        HANDLE search = FindFirstFileW(JoinPath(folder, L"*").c_str(), &data);
        if (search == INVALID_HANDLE_VALUE)
            continue;
        do {
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;
            if (IsInstallerTemporaryName(Narrow(data.cFileName))) {
                bool ignored = false;
                RemoveOrRenameAway(JoinPath(folder, data.cFileName), &ignored);
            }
        } while (FindNextFileW(search, &data));
        FindClose(search);
    }
}

}  // namespace ce::setup
