// Win32 mapping for the session-scoped render-endpoint latency channel. The cross-process layout
// and the pure table logic live in av_sync_latency_channel.h.

#include "av_sync_latency_channel.h"

#include <windows.h>

#include <mutex>

#include "logging.h"

namespace ce::av_sync {
namespace {

std::mutex g_SessionMutex;
HANDLE g_SessionMapping = nullptr;
LatencyChannelBlock* g_SessionBlock = nullptr;
bool g_SessionCreateAttempted = false;

// Anonymous (unnamed) so nothing outside our own inherited children can open it, and inheritable
// so SpawnChildProcess can hand it to a disposable media child.
bool CreateSessionChannelLocked() {
    if (g_SessionCreateAttempted) {
        return g_SessionMapping != nullptr;
    }
    g_SessionCreateAttempted = true;

    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    security.lpSecurityDescriptor = nullptr;

    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, &security, PAGE_READWRITE, 0,
                                        static_cast<DWORD>(sizeof(LatencyChannelBlock)), nullptr);
    if (!mapping) {
        LogWarn("[AVSyncChannel] Session latency channel unavailable (CreateFileMapping error=%lu); every media "
                "child will re-probe",
                static_cast<unsigned long>(GetLastError()));
        return false;
    }

    void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(LatencyChannelBlock));
    if (!view) {
        LogWarn("[AVSyncChannel] Session latency channel unavailable (MapViewOfFile error=%lu); every media child "
                "will re-probe",
                static_cast<unsigned long>(GetLastError()));
        CloseHandle(mapping);
        return false;
    }

    g_SessionMapping = mapping;
    g_SessionBlock = static_cast<LatencyChannelBlock*>(view);
    InitLatencyChannel(*g_SessionBlock);
    LogInfo("[AVSyncChannel] Session latency channel created (entries=%zu keyCapacity=%zu); the render->loopback "
            "probe now runs once per CE session instead of once per recording",
            kLatencyChannelMaxEntries, kLatencyChannelKeyCapacity);
    return true;
}

}  // namespace

void* GetSessionLatencyChannelChildHandle() {
    std::lock_guard<std::mutex> lock(g_SessionMutex);
    if (!CreateSessionChannelLocked()) {
        return nullptr;
    }
    return g_SessionMapping;
}

void ReleaseSessionLatencyChannel() {
    std::lock_guard<std::mutex> lock(g_SessionMutex);
    if (g_SessionBlock) {
        UnmapViewOfFile(g_SessionBlock);
        g_SessionBlock = nullptr;
    }
    if (g_SessionMapping) {
        CloseHandle(g_SessionMapping);
        g_SessionMapping = nullptr;
    }
    g_SessionCreateAttempted = false;
}

LatencyChannelBlock* MapInheritedLatencyChannel(void* inheritedHandle) {
    if (!inheritedHandle || inheritedHandle == INVALID_HANDLE_VALUE) {
        return nullptr;
    }
    // The child owns its inherited copy of the handle. The view outlives the handle, so close it
    // as soon as the mapping exists rather than leaking it for the life of the process.
    void* view = MapViewOfFile(static_cast<HANDLE>(inheritedHandle), FILE_MAP_ALL_ACCESS, 0, 0,
                               sizeof(LatencyChannelBlock));
    const DWORD mapError = view ? ERROR_SUCCESS : GetLastError();
    CloseHandle(static_cast<HANDLE>(inheritedHandle));
    if (!view) {
        LogWarn("[AVSyncChannel] Inherited latency channel could not be mapped (error=%lu); probing instead",
                static_cast<unsigned long>(mapError));
        return nullptr;
    }

    auto* block = static_cast<LatencyChannelBlock*>(view);
    if (!IsLatencyChannelCompatible(*block)) {
        LogWarn("[AVSyncChannel] Inherited latency channel rejected: magic=0x%08X version=%u size=%u count=%u "
                "expected=(0x%08X,%u,%zu,<=%zu)",
                block->magic, block->version, block->structSize, block->entryCount, kLatencyChannelMagic,
                kLatencyChannelVersion, sizeof(LatencyChannelBlock), kLatencyChannelMaxEntries);
        UnmapViewOfFile(view);
        return nullptr;
    }
    LogInfo("[AVSyncChannel] Inherited latency channel attached (entries=%u)", block->entryCount);
    return block;
}

void UnmapInheritedLatencyChannel(LatencyChannelBlock* block) {
    if (block) {
        UnmapViewOfFile(block);
    }
}

}  // namespace ce::av_sync
