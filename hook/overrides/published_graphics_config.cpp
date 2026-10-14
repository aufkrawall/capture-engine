#include "published_graphics_config.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>

#include "hook/runtime/hook_common.h"

namespace ce::published_config {

namespace {

// Plain arrays rather than std::string: the early snapshot is filled from
// DllMain, which is reachable under the loader lock and must not allocate.
const RuntimeOverridePaths g_EmptyPaths{};
RuntimeOverridePaths g_EarlyPaths{};
std::atomic<const RuntimeOverridePaths*> g_CurrentPaths{&g_EmptyPaths};
std::atomic<bool> g_Resolved{false};
std::atomic<bool> g_LocalPublished{false};
std::mutex g_LocalPublishMutex;

bool CopyCodePagePath(char (&destination)[RuntimeOverridePaths::kCapacity], const char* source,
                      size_t sourceCapacity, const char* key) {
    destination[0] = '\0';
    if (!source || !source[0]) {
        return true;
    }
    const size_t length = strnlen(source, sourceCapacity);
    char bounded[MAX_PATH * 2] = {};
    if (length >= sizeof(bounded)) {
        HookLogImportant("Runtime override path %s ignored: %zu bytes exceed the %zu-byte limit", key, length,
                         sizeof(bounded) - 1);
        return false;
    }
    memcpy(bounded, source, length);
    if (CodePagePathToUtf8(bounded, CP_ACP, destination, RuntimeOverridePaths::kCapacity)) {
        return true;
    }
    HookLogImportant("Runtime override path %s ignored: its config text could not be converted to UTF-8 (%zu bytes)",
                     key, length);
    return false;
}

bool SamePaths(const RuntimeOverridePaths& left, const RuntimeOverridePaths& right) {
    return strcmp(left.dlssSr, right.dlssSr) == 0 && strcmp(left.dlssRr, right.dlssRr) == 0 &&
           strcmp(left.dlssFg, right.dlssFg) == 0 && strcmp(left.streamline, right.streamline) == 0;
}

}  // namespace

bool CodePagePathToUtf8(const char* source, unsigned codePage, char* destination, size_t capacity) {
    if (!destination || capacity == 0) {
        return false;
    }
    destination[0] = '\0';
    if (!source || !source[0]) {
        return true;
    }
    wchar_t wide[MAX_PATH * 2] = {};
    const int wideLength = MultiByteToWideChar(codePage, 0, source, -1, wide, static_cast<int>(MAX_PATH * 2));
    if (wideLength <= 0) {
        return false;
    }
    const int utf8Length =
        WideCharToMultiByte(CP_UTF8, 0, wide, -1, destination, static_cast<int>(capacity), nullptr, nullptr);
    if (utf8Length <= 0) {
        destination[0] = '\0';
        return false;
    }
    return true;
}

bool ReadPublishedGraphicsConfig(SharedGraphicsConfig& out) {
    bool answered = false;

    HANDLE discovery = OpenHostDiscoveryMapping();
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
    if (g_Resolved.load(std::memory_order_acquire) || g_LocalPublished.load(std::memory_order_acquire)) {
        return;
    }
    SharedGraphicsConfig published{};
    if (!ReadPublishedGraphicsConfig(published)) {
        return;  // No host answered; the hook thread's config load will take over.
    }

    // The early snapshot is written exactly once, before it is published.
    if (g_Resolved.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    CopyCodePagePath(g_EarlyPaths.dlssSr, published.dlssSrDllPath, sizeof(published.dlssSrDllPath),
                     "dlss_sr_dll_path");
    CopyCodePagePath(g_EarlyPaths.dlssRr, published.dlssRrDllPath, sizeof(published.dlssRrDllPath),
                     "dlss_rr_dll_path");
    CopyCodePagePath(g_EarlyPaths.dlssFg, published.dlssFgDllPath, sizeof(published.dlssFgDllPath),
                     "dlss_fg_dll_path");
    CopyCodePagePath(g_EarlyPaths.streamline, published.streamlineDllPath, sizeof(published.streamlineDllPath),
                     "streamline_dll_path");
    const RuntimeOverridePaths* expected = &g_EmptyPaths;
    g_CurrentPaths.compare_exchange_strong(expected, &g_EarlyPaths, std::memory_order_acq_rel);

    if (g_EarlyPaths.dlssSr[0] || g_EarlyPaths.dlssRr[0] || g_EarlyPaths.dlssFg[0] || g_EarlyPaths.streamline[0]) {
        HookLogImportant(
            "Loader redirect: armed from the injector's published config in DllMain (sr=%d rr=%d fg=%d sl=%d). "
            "The hook thread's config load is ~400 ms later, and a one-shot load in that window - sl.common is "
            "one - would otherwise pass through unredirected",
            g_EarlyPaths.dlssSr[0] ? 1 : 0, g_EarlyPaths.dlssRr[0] ? 1 : 0, g_EarlyPaths.dlssFg[0] ? 1 : 0,
            g_EarlyPaths.streamline[0] ? 1 : 0);
    }
}

void PublishLocalRuntimeOverridePaths(const char* dlssSr, const char* dlssRr, const char* dlssFg,
                                      const char* streamline, const char* source) {
    auto next = std::make_unique<RuntimeOverridePaths>();
    CopyCodePagePath(next->dlssSr, dlssSr, RuntimeOverridePaths::kCapacity, "dlss_sr_dll_path");
    CopyCodePagePath(next->dlssRr, dlssRr, RuntimeOverridePaths::kCapacity, "dlss_rr_dll_path");
    CopyCodePagePath(next->dlssFg, dlssFg, RuntimeOverridePaths::kCapacity, "dlss_fg_dll_path");
    CopyCodePagePath(next->streamline, streamline, RuntimeOverridePaths::kCapacity, "streamline_dll_path");

    std::lock_guard<std::mutex> lock(g_LocalPublishMutex);
    const RuntimeOverridePaths* current = g_CurrentPaths.load(std::memory_order_acquire);
    const bool firstLocal = !g_LocalPublished.exchange(true, std::memory_order_acq_rel);
    if (SamePaths(*current, *next)) {
        return;
    }
    // The early snapshot is whatever profile the host published last. A
    // difference means loads before this point may have used another set.
    if (firstLocal && current == &g_EarlyPaths) {
        HookLogImportant(
            "Loader redirect: this process's profile differs from the injector's early-published override paths "
            "(differs: sr=%d rr=%d fg=%d sl=%d); loads before this point used the published set",
            strcmp(current->dlssSr, next->dlssSr) != 0 ? 1 : 0, strcmp(current->dlssRr, next->dlssRr) != 0 ? 1 : 0,
            strcmp(current->dlssFg, next->dlssFg) != 0 ? 1 : 0,
            strcmp(current->streamline, next->streamline) != 0 ? 1 : 0);
    }
    // Readers hold references without a lock, so a superseded snapshot is
    // deliberately never freed. Republication happens only on a real change.
    g_CurrentPaths.store(next.release(), std::memory_order_release);
    HookLog("Loader redirect: override paths published from %s", source ? source : "unspecified");
}

const RuntimeOverridePaths& CurrentRuntimeOverridePaths() {
    return *g_CurrentPaths.load(std::memory_order_acquire);
}

bool EarlyRuntimeOverridePathsResolved() {
    return g_Resolved.load(std::memory_order_acquire);
}

}  // namespace ce::published_config
