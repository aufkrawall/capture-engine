#pragma once

#include <windows.h>

#include <mutex>
#include <unordered_map>

// Handle validation cache: tracks handles that have previously failed D3D11 OpenShared*
// calls, so we don't repeatedly trigger SEH exceptions from invalid handles.
// D3D11 throws SEH exceptions for invalid handles, and MinGW's catch(...) cannot
// catch SEH exceptions. Pre-validation is the only reliable protection.
//
// The cache stores (handle_value, failure_count) pairs. Handles that fail >3 times
// are permanently skipped. Each VideoEncoder owns one and clears it when it is initialized.
struct HandleFailureCache {
    std::mutex mutex;
    std::unordered_map<HANDLE, int> fenceFailures;
    std::unordered_map<HANDLE, int> textureFailures;

    bool ShouldSkipFence(HANDLE h) {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = fenceFailures.find(h);
        return it != fenceFailures.end() && it->second >= 3;
    }

    bool ShouldSkipTexture(HANDLE h) {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = textureFailures.find(h);
        return it != textureFailures.end() && it->second >= 3;
    }

    void RecordFenceFailure(HANDLE h) {
        std::lock_guard<std::mutex> lock(mutex);
        fenceFailures[h]++;
    }

    void RecordTextureFailure(HANDLE h) {
        std::lock_guard<std::mutex> lock(mutex);
        textureFailures[h]++;
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(mutex);
        fenceFailures.clear();
        textureFailures.clear();
    }
};
