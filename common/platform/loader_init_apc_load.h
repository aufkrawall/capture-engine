#pragma once

#include "loader_init_apc.h"

#include <string>

namespace ce::loader_init_apc {

enum class LoadState { Pending, Loaded, Failed };

struct LoadResult {
    LoadState state = LoadState::Pending;
    uint64_t module = 0;
    DWORD error = ERROR_SUCCESS;
};

// Owns an acknowledged native x64 APC load. The completion signal is emitted
// by a tail call into ntdll, after execution has permanently left the remote
// code allocation, so completion also proves that allocation can be freed.
class LoadRequest {
public:
    LoadRequest() = default;
    ~LoadRequest();
    LoadRequest(const LoadRequest&) = delete;
    LoadRequest& operator=(const LoadRequest&) = delete;

    Result Queue(HANDLE process, DWORD pid, const std::wstring& path, void* loadLibrary);
    LoadResult ReadResult() const;
    HANDLE CompletionEvent() const { return completion_; }

private:
    void Reset();
    HANDLE process_ = nullptr;
    HANDLE completion_ = nullptr;
    HANDLE remoteEvent_ = nullptr;
    void* remoteData_ = nullptr;
    void* remoteCode_ = nullptr;
    bool queued_ = false;
};

}  // namespace ce::loader_init_apc
