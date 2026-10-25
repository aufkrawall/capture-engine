#pragma once

#include <windows.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace ce::logging {

// Direct-to-file log records of a module that outlives the CaptureEngine session it first logged for.
//
// The hook writes here before IPC exists, for every HookLogImportant line and whenever the shared-memory
// ring is full; the Vulkan layer writes its vulkan_layer.log here. The session directory is resolved on
// first use and cached together with one append handle per file. A resident hook or layer reactivated by a
// replacement CaptureEngine calls Rebind(): until it did, every later direct line kept appending to the
// previous session's files (session 20261025_212736: 2708 of the 2725 hook lines written after the reconnect
// were missing from the new session folder).
class DirectLogFileSink {
public:
    // Fills the directory to write into; false while none can be built (the next append retries).
    using DirectoryResolver = bool (*)(char* directory, size_t directorySize);
    // Writes a file's preamble right after this sink opened it; `emptyFile` tells a new file from one that
    // already holds another writer's lines.
    using FileOpened = void (*)(HANDLE file, bool emptyFile);

    enum class Append : uint8_t {
        kWritten,
        kBusy,    // another thread is inside the sink; TryAppend never waits for it
        kFailed,  // no directory, the file did not open, or the write failed (that handle is reopened next time)
    };

    DirectLogFileSink(DirectoryResolver resolveDirectory, FileOpened onOpened) noexcept;
    ~DirectLogFileSink();
    DirectLogFileSink(const DirectLogFileSink&) = delete;
    DirectLogFileSink& operator=(const DirectLogFileSink&) = delete;

    // Appends one complete record to `fileName` in the bound directory. A record reported lost since the
    // last written one is announced first ("[LOGGING] N line(s) dropped ..."), so a gap in a file's
    // sequence always has an explanation in that file.
    Append TryAppend(std::string_view fileName, std::string_view record);
    // As TryAppend, but waits for a concurrent writer instead of returning kBusy.
    Append Write(std::string_view fileName, std::string_view record);
    // A record that reached no file; announced by the next written record.
    void NoteLost() noexcept;
    // Forgets the bound directory and closes every cached handle; the next append resolves the directory
    // again. Returns the directory that was bound (empty when none was) for the caller's transition line.
    std::string Rebind();

private:
    Append AppendLocked(std::string_view fileName, std::string_view record);
    void CloseHandlesLocked();

    DirectoryResolver resolveDirectory_;
    FileOpened onOpened_;
    std::mutex mutex_;
    std::string directory_;
    std::unordered_map<std::string, HANDLE> handles_;
    std::atomic<uint32_t> lostRecords_{0};
};

}  // namespace ce::logging
