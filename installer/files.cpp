// File operations shared by setup (updates, moves) and the uninstaller.
//
// Nothing here deletes recursively. An installation is removed by walking the
// manifest it wrote, so a file that was never ours - or a folder the user put
// something into - survives every update and uninstall.

#include "setup.h"
#include "filesystem_guard.h"

#include <set>
#include <winternl.h>

namespace ce::setup {
namespace {

constexpr uint64_t kManifestLimit = 4u << 20;

using OpenFileFn = decltype(&NtCreateFile);
using SetFileFn = decltype(&NtSetInformationFile);
using StatusErrorFn = decltype(&RtlNtStatusToDosError);

const auto kOpenFile = reinterpret_cast<OpenFileFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateFile"));
const auto kSetFile = reinterpret_cast<SetFileFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetInformationFile"));
const auto kStatusError =
    reinterpret_cast<StatusErrorFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlNtStatusToDosError"));

bool NativeResult(NTSTATUS status) {
    if (status >= 0)
        return true;
    SetLastError(kStatusError ? kStatusError(status) : ERROR_GEN_FAILURE);
    return false;
}

Handle OpenDirectoryEntry(HANDLE directory, const std::wstring& leaf, ACCESS_MASK access,
                          ULONG options, ULONG sharing) {
    if (!kOpenFile) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return {};
    }
    UNICODE_STRING name{};
    name.Buffer = const_cast<PWSTR>(leaf.c_str());
    name.Length = static_cast<USHORT>(leaf.size() * sizeof(wchar_t));
    name.MaximumLength = name.Length;
    OBJECT_ATTRIBUTES attributes{};
    attributes.Length = sizeof(attributes);
    attributes.RootDirectory = directory;
    attributes.ObjectName = &name;
    attributes.Attributes = OBJ_CASE_INSENSITIVE;
    IO_STATUS_BLOCK io{};
    HANDLE raw = nullptr;
    // Synchronous, no-follow, single-leaf access relative to the retained object.
    const NTSTATUS status = kOpenFile(&raw, access | SYNCHRONIZE, &attributes, &io, nullptr, FILE_ATTRIBUTE_NORMAL,
                                     sharing, FILE_OPEN,
                                     FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT | options,
                                     nullptr, 0);
    if (!NativeResult(status))
        return {};
    return Handle(raw);
}

Handle OpenDirectoryLeaf(HANDLE directory, const std::wstring& path, ACCESS_MASK access) {
    return OpenDirectoryEntry(directory, LeafOf(path), access, FILE_NON_DIRECTORY_FILE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
}

DWORD DirectoryLeafAttributes(HANDLE directory, const std::wstring& path) {
    if (!directory)
        return GetFileAttributesW(path.c_str());
    Handle file = OpenDirectoryLeaf(directory, path, FILE_READ_ATTRIBUTES);
    BY_HANDLE_FILE_INFORMATION information{};
    if (!file.Valid() || !GetFileInformationByHandle(file.Get(), &information))
        return INVALID_FILE_ATTRIBUTES;
    return information.dwFileAttributes;
}

void NormalizeLeafAttributes(HANDLE directory, const std::wstring& path) {
    if (!directory) {
        SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
        return;
    }
    Handle file = OpenDirectoryLeaf(directory, path, FILE_WRITE_ATTRIBUTES);
    FILE_BASIC_INFO information{};
    information.FileAttributes = FILE_ATTRIBUTE_NORMAL;
    if (file.Valid())
        SetFileInformationByHandle(file.Get(), FileBasicInfo, &information, sizeof(information));
}

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

Handle OpenChildDirectoryNoFollow(HANDLE parentDirectory, const std::wstring& name) {
    return OpenDirectoryEntry(parentDirectory, name, GENERIC_READ, FILE_DIRECTORY_FILE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE);
}

bool DeleteFileInDirectory(HANDLE parentDirectory, const std::wstring& path) {
    if (!parentDirectory)
        return DeleteFileW(path.c_str()) != FALSE;
    Handle file = OpenDirectoryLeaf(parentDirectory, path, DELETE);
    if (!file.Valid())
        return false;
    FILE_DISPOSITION_INFO disposition{TRUE};
    return SetFileInformationByHandle(file.Get(), FileDispositionInfo, &disposition, sizeof(disposition)) != FALSE;
}

bool RenameFileInDirectory(HANDLE parentDirectory, const std::wstring& path, const std::wstring& destination) {
    if (!parentDirectory)
        return MoveFileExW(path.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!kSetFile || DirectoryOf(path) != DirectoryOf(destination)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }
    Handle file = OpenDirectoryLeaf(parentDirectory, path, DELETE);
    if (!file.Valid())
        return false;
    const std::wstring leaf = LeafOf(destination);
    const size_t bytes = offsetof(FILE_RENAME_INFORMATION, FileName) + leaf.size() * sizeof(wchar_t);
    std::vector<uint64_t> storage((bytes + sizeof(uint64_t) - 1) / sizeof(uint64_t));
    auto* rename = reinterpret_cast<FILE_RENAME_INFORMATION*>(storage.data());
    rename->RootDirectory = parentDirectory;
    rename->FileNameLength = static_cast<ULONG>(leaf.size() * sizeof(wchar_t));
    std::memcpy(rename->FileName, leaf.data(), rename->FileNameLength);
    IO_STATUS_BLOCK io{};
    return NativeResult(kSetFile(file.Get(), &io, rename, static_cast<ULONG>(bytes), FileRenameInformation));
}

bool RemoveOrRenameAway(const std::wstring& path, bool* deferredToReboot, HANDLE parentDirectory) {
    const DWORD attributes = DirectoryLeafAttributes(parentDirectory, path);
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return IsMissingError(GetLastError());
    if (attributes & FILE_ATTRIBUTE_DIRECTORY)
        return false;
    if (attributes & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_SYSTEM))
        NormalizeLeafAttributes(parentDirectory, path);
    if (DeleteFileInDirectory(parentDirectory, path) || IsMissingError(GetLastError()))
        return true;
    const DWORD deleteError = GetLastError();
    // In use: a running image cannot be deleted but can be renamed. Moving it
    // aside frees the name for the new file immediately.
    std::wstring aside = path + kBackupSuffixWide;
    for (int attempt = 0; PathExists(aside) && attempt < 100; ++attempt) {
        if (DeleteFileInDirectory(parentDirectory, aside))
            break;
        aside = path + L"." + std::to_wstring(GetTickCount64() + static_cast<ULONGLONG>(attempt)) + kBackupSuffixWide;
    }
    if (!RenameFileInDirectory(parentDirectory, path, aside)) {
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
        const InstallationPathGuard guard(directory, relative);
        if (!guard.Valid()) {
            if (IsMissingError(guard.Error()))
                continue;
            Log("files: skipping %s (unsafe or unavailable folder, error %lu)", relative.c_str(), guard.Error());
            if (warnings)
                warnings->push_back(L"Left " + Widen(relative) + L" in place because its folder could not be safely opened.");
            continue;
        }
        const std::wstring path = JoinPath(guard.Directory(), Widen(relative));
        bool deferred = false;
        if (!RemoveOrRenameAway(path, &deferred, guard.ParentHandle())) {
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
        const InstallationPathGuard guard(directory, relative.empty() ? "x" : relative + "/x");
        if (!guard.Valid()) {
            if (!IsMissingError(guard.Error()))
                Log("cleanup: skipping %s (unsafe or unavailable folder, error %lu)", relative.c_str(), guard.Error());
            continue;
        }
        const std::wstring folder = relative.empty() ? guard.Directory() : JoinPath(guard.Directory(), Widen(relative));
        WIN32_FIND_DATAW data{};
        HANDLE search = FindFirstFileW(JoinPath(folder, L"*").c_str(), &data);
        if (search == INVALID_HANDLE_VALUE)
            continue;
        do {
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;
            if (IsInstallerTemporaryName(Narrow(data.cFileName))) {
                bool ignored = false;
                RemoveOrRenameAway(JoinPath(folder, data.cFileName), &ignored, guard.ParentHandle());
            }
        } while (FindNextFileW(search, &data));
        FindClose(search);
    }
}

void RemovePreviousInstallation(const std::wstring& previousDirectory, const std::wstring& directory,
                                std::vector<std::wstring>* warnings, bool* rebootRecommended) {
    InstallationPathGuard previousRoot(previousDirectory, "x");
    const InstallationPathGuard currentRoot(directory, "x");
    const DirectoryComparison comparison = previousRoot.Valid() && currentRoot.Valid()
                                               ? CompareInstallationDirectories(previousRoot.RootHandle(), currentRoot.RootHandle())
                                               : DirectoryComparison::Unknown;
    if (comparison == DirectoryComparison::Same) {
        Log("install: previous and selected paths identify the same folder; keeping the updated files");
        return;
    }
    Manifest old;
    if (comparison == DirectoryComparison::Unknown || !LoadManifest(previousRoot.Directory(), &old)) {
        Log("install: previous folder left in place (identity or manifest could not be verified)");
        if (warnings)
            warnings->push_back(L"The previous installation in " + previousDirectory +
                                L" was left in place because its folder identity or file list could not be verified.");
        return;
    }
    const std::wstring previous = previousRoot.Directory();
    RemoveManifestFiles(previous, old.files, warnings, rebootRecommended);
    RemoveEmptyDirectories(previous, old.files);
    RemoveOrRenameAway(JoinPath(previous, kManifestFile), nullptr, previousRoot.RootHandle());
    previousRoot.Release();
    RemoveDirectoryW(previous.c_str());
}

}  // namespace ce::setup
