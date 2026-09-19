#include "published_graphics_config.h"

#include <windows.h>

#include <atomic>
#include <cstring>

#include "hook_common.h"

namespace ce::published_config {

namespace {

// Resolved once, from DllMain. Plain arrays rather than std::string: this is
// reachable under the loader lock and must not allocate.
char g_DlssSrDllPath[MAX_PATH] = {};
char g_DlssRrDllPath[MAX_PATH] = {};
char g_DlssFgDllPath[MAX_PATH] = {};
char g_StreamlineDllPath[MAX_PATH] = {};
std::atomic<bool> g_Resolved{false};

void CopyPath(char (&destination)[MAX_PATH], const char* source) {
    destination[0] = '\0';
    if (!source || !source[0]) {
        return;
    }
    strncpy(destination, source, MAX_PATH - 1);
    destination[MAX_PATH - 1] = '\0';
}

}  // namespace

bool ReadPublishedGraphicsConfig(SharedGraphicsConfig& out) {
    bool answered = false;

    HANDLE discovery = OpenFileMappingW(FILE_MAP_READ, FALSE, SHARED_MEM_DISCOVERY);
    if (!discovery) {
        return false;
    }
    auto* info = static_cast<DiscoveryInfo*>(MapViewOfFile(discovery, FILE_MAP_READ, 0, 0, sizeof(DiscoveryInfo)));
    uint32_t injectPid = 0;
    if (ValidateDiscoveryInfo(info)) {
        injectPid = info->GetInjectPid();
    }
    if (info) {
        UnmapViewOfFile(info);
    }
    CloseHandle(discovery);
    if (injectPid == 0) {
        return false;
    }

    wchar_t sharedMemName[64] = {};
    GenerateSharedMemName(sharedMemName, 64, injectPid);
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, sharedMemName);
    if (!mapping) {
        return false;
    }
    auto* shared =
        static_cast<SharedMemoryLayout*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(SharedMemoryLayout)));
    if (shared) {
        // The magic is published last, after every field is constructed, so it
        // is the only safe gate on reading the payload.
        if (shared->GetMagic() == SHARED_MEMORY_MAGIC && shared->GetVersion() == SHARED_MEMORY_VERSION &&
            shared->abiSignature.load(std::memory_order_acquire) == SHARED_MEMORY_ABI_SIGNATURE) {
            out = shared->graphicsConfig;
            answered = true;
        }
        UnmapViewOfFile(shared);
    }
    CloseHandle(mapping);
    return answered;
}

void ResolveEarlyRuntimeOverridePaths() {
    if (g_Resolved.load(std::memory_order_acquire)) {
        return;
    }
    SharedGraphicsConfig published{};
    if (!ReadPublishedGraphicsConfig(published)) {
        return;  // No host answered. Nothing to cache; a later call retries.
    }

    CopyPath(g_DlssSrDllPath, published.dlssSrDllPath);
    CopyPath(g_DlssRrDllPath, published.dlssRrDllPath);
    CopyPath(g_DlssFgDllPath, published.dlssFgDllPath);
    CopyPath(g_StreamlineDllPath, published.streamlineDllPath);
    g_Resolved.store(true, std::memory_order_release);

    if (g_DlssSrDllPath[0] || g_DlssRrDllPath[0] || g_DlssFgDllPath[0] || g_StreamlineDllPath[0]) {
        HookLogImportant(
            "Loader redirect: armed from the injector's published config in DllMain (sr=%d rr=%d fg=%d sl=%d). "
            "The hook thread's config load is ~400 ms later, and a one-shot load in that window - sl.common is "
            "one - would otherwise pass through unredirected",
            g_DlssSrDllPath[0] ? 1 : 0, g_DlssRrDllPath[0] ? 1 : 0, g_DlssFgDllPath[0] ? 1 : 0,
            g_StreamlineDllPath[0] ? 1 : 0);
    }
}

const char* EarlyDlssSrDllPath() {
    return g_DlssSrDllPath;
}

const char* EarlyDlssRrDllPath() {
    return g_DlssRrDllPath;
}

const char* EarlyDlssFgDllPath() {
    return g_DlssFgDllPath;
}

const char* EarlyStreamlineDllPath() {
    return g_StreamlineDllPath;
}

bool EarlyRuntimeOverridePathsResolved() {
    return g_Resolved.load(std::memory_order_acquire);
}

}  // namespace ce::published_config
