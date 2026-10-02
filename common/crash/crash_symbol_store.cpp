#include "crash_symbol_store.h"

#include <windows.h>

#include <system_error>

namespace fs = std::filesystem;

namespace ce::crash_symbols {

namespace {

bool StoreCopyIsCurrent(const fs::path& source, const fs::path& storeFile) {
    std::error_code ec;
    if (!fs::is_regular_file(storeFile, ec) || ec)
        return false;
    const auto sourceSize = fs::file_size(source, ec);
    if (ec)
        return false;
    const auto storeSize = fs::file_size(storeFile, ec);
    if (ec || storeSize != sourceSize)
        return false;
    const auto sourceTime = fs::last_write_time(source, ec);
    if (ec)
        return false;
    const auto storeTime = fs::last_write_time(storeFile, ec);
    return !ec && storeTime == sourceTime;
}

// Refreshes the store copy through a temporary file and a replacing rename, so
// the replaced store file keeps its identity for every session still linking it.
bool RefreshStoreCopy(const fs::path& source, const fs::path& storeFile) {
    if (StoreCopyIsCurrent(source, storeFile))
        return true;
    std::error_code ec;
    fs::create_directories(storeFile.parent_path(), ec);
    const fs::path temporary = storeFile.wstring() + L".partial";
    fs::remove(temporary, ec);
    if (!CopyFileW(source.c_str(), temporary.c_str(), FALSE)) {
        fs::remove(temporary, ec);
        return false;
    }
    // CopyFileW carries the source's last-write time, which is what marks the
    // store copy current for the next session.
    if (!MoveFileExW(temporary.c_str(), storeFile.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        fs::remove(temporary, ec);
        return false;
    }
    return true;
}

}  // namespace

unsigned long QueryLinkCount(const fs::path& file) {
    HANDLE handle = CreateFileW(file.c_str(), FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return 0;
    BY_HANDLE_FILE_INFORMATION information = {};
    const bool ok = GetFileInformationByHandle(handle, &information) != FALSE;
    CloseHandle(handle);
    return ok ? information.nNumberOfLinks : 0;
}

PlaceResult PlaceArtifact(const fs::path& source, const fs::path& destination, const fs::path& storeDir) {
    PlaceResult result;
    std::error_code ec;
    if (fs::exists(destination, ec)) {
        result.placed = true;
        result.alreadyPresent = true;
        return result;
    }
    fs::create_directories(destination.parent_path(), ec);

    const fs::path storeFile = storeDir / source.filename();
    if (!storeDir.empty() && RefreshStoreCopy(source, storeFile) &&
        CreateHardLinkW(destination.c_str(), storeFile.c_str(), nullptr)) {
        result.placed = true;
        result.linked = true;
        return result;
    }

    // No store or no hard links on this volume: the pre-store behavior.
    ec.clear();
    fs::copy_file(source, destination, fs::copy_options::none, ec);
    result.placed = !ec;
    return result;
}

size_t PruneUnreferencedStoreFiles(const fs::path& storeDir) {
    size_t removed = 0;
    std::error_code ec;
    if (!fs::is_directory(storeDir, ec))
        return 0;
    for (const auto& entry : fs::directory_iterator(storeDir, ec)) {
        std::error_code entryError;
        if (!entry.is_regular_file(entryError) || entryError)
            continue;
        const fs::path& file = entry.path();
        if (file.extension() == L".partial") {
            // A refresh interrupted before its rename; nothing links it.
            if (fs::remove(file, entryError))
                ++removed;
            continue;
        }
        // A link count of one means only the store names this file. Another CE
        // instance linking it right now loses nothing: its CreateHardLinkW fails
        // and it falls back to a copy.
        if (QueryLinkCount(file) == 1 && fs::remove(file, entryError))
            ++removed;
    }
    return removed;
}

}  // namespace ce::crash_symbols
