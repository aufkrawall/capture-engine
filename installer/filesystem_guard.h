#pragma once

#include "setup.h"

#include <cstring>

namespace ce::setup {

// Pin checked directory objects without delete sharing. File operations use
// the retained parent handle so neither an alias change nor a later reparse tag
// can redirect cleanup. Write sharing permits renames within the directory.
class InstallationPathGuard {
public:
    InstallationPathGuard(const std::wstring& directory, const std::string& relative) {
        if (!IsSafeRelativePath(relative)) {
            error_ = ERROR_INVALID_NAME;
            return;
        }
        Handle root(CreateFileW(directory.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        if (!Lock(std::move(root), false))
            return;
        const DWORD length = GetFinalPathNameByHandleW(directories_.front().Get(), nullptr, 0, FILE_NAME_NORMALIZED);
        if (!length) {
            error_ = GetLastError();
            return;
        }
        directory_.resize(length);
        const DWORD copied = GetFinalPathNameByHandleW(directories_.front().Get(), directory_.data(), length,
                                                       FILE_NAME_NORMALIZED);
        if (!copied || copied >= length) {
            error_ = copied >= length ? ERROR_INSUFFICIENT_BUFFER : GetLastError();
            return;
        }
        directory_.resize(copied);
        for (size_t start = 0;;) {
            const size_t slash = relative.find('/', start);
            if (slash == std::string::npos)
                break;
            Handle child = OpenChildDirectoryNoFollow(directories_.back().Get(),
                                                       Widen(relative.substr(start, slash - start)));
            if (!Lock(std::move(child), true))
                return;
            start = slash + 1;
        }
        valid_ = true;
    }

    bool Valid() const { return valid_; }
    DWORD Error() const { return error_; }
    const std::wstring& Directory() const { return directory_; }
    void Release() { directories_.clear(); }
    HANDLE ParentHandle() const { return directories_.empty() ? nullptr : directories_.back().Get(); }
    HANDLE RootHandle() const { return directories_.empty() ? nullptr : directories_.front().Get(); }

private:
    bool Lock(Handle directory, bool refuseLink) {
        BY_HANDLE_FILE_INFORMATION information{};
        if (!directory.Valid() || !GetFileInformationByHandle(directory.Get(), &information)) {
            error_ = GetLastError();
            return false;
        }
        if (!(information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (refuseLink && (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))) {
            error_ = ERROR_CANT_ACCESS_FILE;
            return false;
        }
        directories_.push_back(std::move(directory));
        return true;
    }

    std::vector<Handle> directories_;
    std::wstring directory_;
    DWORD error_ = ERROR_SUCCESS;
    bool valid_ = false;
};

enum class DirectoryComparison { Same, Different, Unknown };

// Follow aliases and compare filesystem identity, not drive letters or spelling.
// Unknown must never authorize deleting the previous installation.
inline DirectoryComparison CompareInstallationDirectories(HANDLE first, HANDLE second) {
    FILE_ID_INFO firstId{}, secondId{};
    if (!first || !second ||
        !GetFileInformationByHandleEx(first, FileIdInfo, &firstId, sizeof(firstId)) ||
        !GetFileInformationByHandleEx(second, FileIdInfo, &secondId, sizeof(secondId)))
        return DirectoryComparison::Unknown;
    return firstId.VolumeSerialNumber == secondId.VolumeSerialNumber &&
                   std::memcmp(firstId.FileId.Identifier, secondId.FileId.Identifier, sizeof(firstId.FileId.Identifier)) == 0
               ? DirectoryComparison::Same
               : DirectoryComparison::Different;
}

inline DirectoryComparison CompareInstallationDirectories(const std::wstring& left, const std::wstring& right) {
    const InstallationPathGuard first(left, "x"), second(right, "x");
    return first.Valid() && second.Valid() ? CompareInstallationDirectories(first.RootHandle(), second.RootHandle())
                                          : DirectoryComparison::Unknown;
}

}  // namespace ce::setup
