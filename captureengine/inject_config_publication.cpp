#include "inject_config_publication.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../common/config.h"
#include "../common/inject_overlay_policy.h"
#include "../common/logging.h"
#include "../common/process_identity.h"
#include "../common/shared_defs.h"
#include "../common/vulkan_layer_target_list.h"
#include "inject_config.h"

namespace {

// A resolved config plus the target name it was resolved for. The map is keyed
// by the lowercased name; the original spelling is kept so a reload can redo the
// resolve with exactly the string the first resolve used.
struct ResolvedTargetConfig {
    std::string targetProcess;
    AppConfig config;
};

// Every shared config publication is serialized here. The overlay-config
// seqlock permits one writer, while injection workers, IPC commands, and hook
// source transitions can all publish from different threads.
struct PublicationState {
    std::mutex mutex;
    std::string configPath;
    AppConfig baseConfig;
    std::string targetProcess;
    OverlayVisibilityOverride overlayVisibility;
    std::unordered_map<std::string, ResolvedTargetConfig> resolvedTargetConfigs;

    // Prewarming state. Bumped by every SetPublicationBaseConfig so a resolve
    // that finishes after a newer base config arrived is discarded instead of
    // seeding the cache with a stale profile.
    uint64_t configGeneration = 0;
    std::deque<std::string> warmQueue;
    std::condition_variable warmSignal;
    std::thread warmWorker;
    bool warmStop = false;

    ~PublicationState() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!warmWorker.joinable()) {
                warmStop = true;
                return;
            }
            warmStop = true;
            warmQueue.clear();
        }
        warmSignal.notify_all();
        warmWorker.join();
    }
};

PublicationState& Publication() {
    static PublicationState state;
    return state;
}

std::string NormalizeTargetProcessName(std::string processName) {
    std::transform(processName.begin(), processName.end(), processName.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return processName;
}

AppConfig ResolveActiveConfigLocked(SharedMemoryLayout* sharedMemory, std::string& targetProcessOut) {
    std::string hookSourceProcess;
    if (sharedMemory) {
        const uint32_t sourcePid = sharedMemory->GetSourcePid();
        if (sourcePid != 0)
            hookSourceProcess = GetProcessNameFromPID(sourcePid);
    }

    PublicationState& publication = Publication();
    targetProcessOut = ResolveActiveTargetProcessName(publication.targetProcess, hookSourceProcess);
    if (targetProcessOut.empty()) {
        return publication.baseConfig;
    }

    const std::string cacheKey = NormalizeTargetProcessName(targetProcessOut);
    const auto cached = publication.resolvedTargetConfigs.find(cacheKey);
    if (cached != publication.resolvedTargetConfigs.end()) {
        LogDebug("[Inject] Reusing resolved target config: target=%s", targetProcessOut.c_str());
        return cached->second.config;
    }

    AppConfig resolved = ResolveTargetConfig(publication.configPath, publication.baseConfig, targetProcessOut);
    publication.resolvedTargetConfigs.emplace(cacheKey, ResolvedTargetConfig{targetProcessOut, resolved});
    LogDebug("[Inject] Cached resolved target config: target=%s", targetProcessOut.c_str());
    return resolved;
}

void PublishConfigLocked(SharedMemoryLayout* sharedMemory, const AppConfig& resolved,
                         const std::string& targetProcess, const char* reason) {
    LogDebug("[Inject] Publishing config: target=%s overlayOverride=%d overlayVisible=%d source=%s",
             targetProcess.empty() ? "<none>" : targetProcess.c_str(),
             Publication().overlayVisibility.active ? 1 : 0, resolved.overlay.showOverlay ? 1 : 0,
             reason ? reason : "unknown");
    UpdateSharedMemoryFromConfig(sharedMemory, resolved);
    if (sharedMemory) {
        strncpy(sharedMemory->benchmark.profileName, targetProcess.c_str(),
                sizeof(sharedMemory->benchmark.profileName) - 1);
        sharedMemory->benchmark.profileName[sizeof(sharedMemory->benchmark.profileName) - 1] = '\0';
    }
}

void PublishResolvedConfigLocked(SharedMemoryLayout* sharedMemory, const char* reason) {
    std::string targetProcess;
    AppConfig resolved = ResolveActiveConfigLocked(sharedMemory, targetProcess);
    ApplyOverlayVisibility(Publication().overlayVisibility, resolved);
    PublishConfigLocked(sharedMemory, resolved, targetProcess, reason);
}

// Prewarms the resolved-config cache for every whitelisted target, so the
// injector's pre-LoadLibrary publish (which carries ngx_ota mode) is a cache hit
// and adds no discovery latency.
//
// This runs on its own thread because each entry is a full ReadLiteralIniValue
// pass over config.ini. Doing all of them inline used to cost ~4.9 s for 27
// whitelisted games, and SetPublicationBaseConfig is called from the inject
// child's ReloadConfig handler, which the controller waits on for at most 1 s
// before declaring the channel broken and respawning this process. A sharpen-only
// config save therefore tore the host out from under a running game's hook for
// ~7 s (session 20260920_192913). The warm-up is an optimization, never a
// correctness requirement: ResolveActiveConfigLocked still resolves and caches
// any target the sweep has not reached yet.
void PublicationWarmupLoop() {
    PublicationState& publication = Publication();
    std::unique_lock<std::mutex> lock(publication.mutex);
    for (;;) {
        publication.warmSignal.wait(lock,
                                    [&] { return publication.warmStop || !publication.warmQueue.empty(); });
        if (publication.warmStop)
            return;

        const uint64_t generation = publication.configGeneration;
        const std::string target = std::move(publication.warmQueue.front());
        publication.warmQueue.pop_front();
        const std::string cacheKey = NormalizeTargetProcessName(target);
        if (target.empty() || publication.resolvedTargetConfigs.find(cacheKey) != publication.resolvedTargetConfigs.end())
            continue;

        const std::string configPath = publication.configPath;
        const AppConfig baseConfig = publication.baseConfig;

        // Never hold the publication mutex across a resolve: an injection or
        // hotkey publish would then queue behind the whole sweep.
        lock.unlock();
        AppConfig resolved;
        bool resolveSucceeded = false;
        try {
            resolved = ResolveTargetConfig(configPath, baseConfig, target);
            resolveSucceeded = true;
        } catch (const std::exception& error) {
            LogWarn("[Inject] Failed to prewarm config for target %s: %s", target.c_str(), error.what());
        } catch (...) {
            LogWarn("[Inject] Unknown exception while prewarming config for target %s", target.c_str());
        }
        lock.lock();

        if (!resolveSucceeded)
            continue;

        // A newer base config queued its own sweep; this result describes the
        // previous file.
        if (publication.configGeneration != generation)
            continue;
        publication.resolvedTargetConfigs.emplace(cacheKey, ResolvedTargetConfig{target, std::move(resolved)});
    }
}

void QueueWarmTargetLocked(PublicationState& publication, const std::string& target) {
    if (target.empty())
        return;
    publication.warmQueue.push_back(target);
}

}  // namespace

std::string GetProcessNameFromPID(DWORD pid) {
    const ce::process::ProcessIdentityResult identity = ce::process::QueryProcessIdentity(pid);
    if (!identity) {
        static std::atomic<uint32_t> failureLogs{0};
        if (failureLogs.fetch_add(1, std::memory_order_relaxed) < 16) {
            LogDebug("[Identity] Limited process-name query failed (pid=%lu error=%lu)",
                     static_cast<unsigned long>(pid), identity.error);
        }
    }
    return identity.imageName;
}

void ClearPublicationTarget() {
    PublicationState& publication = Publication();
    std::lock_guard<std::mutex> lock(publication.mutex);
    publication.targetProcess.clear();
}

void SetPublicationBaseConfig(const std::string& configPath, const AppConfig& baseConfig) {
    PublicationState& publication = Publication();
    std::lock_guard<std::mutex> lock(publication.mutex);
    publication.configPath = configPath;
    publication.baseConfig = baseConfig;
    publication.overlayVisibility = {};
    ++publication.configGeneration;

    // Every cached resolve describes the previous file, so none of them survive.
    // Requeueing them ahead of the rest of the whitelist puts the targets this
    // process is actually publishing for at the front of the sweep.
    std::deque<std::string> warmQueue;
    for (auto& entry : publication.resolvedTargetConfigs) {
        if (!entry.second.targetProcess.empty())
            warmQueue.push_back(std::move(entry.second.targetProcess));
    }
    publication.resolvedTargetConfigs.clear();
    publication.warmQueue = std::move(warmQueue);

    if (configPath.empty() || publication.warmStop) {
        publication.warmQueue.clear();
        return;
    }

    for (const auto& entry : baseConfig.gameWhitelist)
        QueueWarmTargetLocked(publication, entry.pattern);
    for (const auto& entry : baseConfig.overlayWhitelist)
        QueueWarmTargetLocked(publication, entry.pattern);

    if (!publication.warmWorker.joinable())
        publication.warmWorker = std::thread(PublicationWarmupLoop);
    publication.warmSignal.notify_all();
}

void StopPublicationWarmup() {
    PublicationState& publication = Publication();
    {
        std::lock_guard<std::mutex> lock(publication.mutex);
        if (!publication.warmWorker.joinable()) {
            publication.warmStop = true;
            return;
        }
        publication.warmStop = true;
        publication.warmQueue.clear();
    }
    publication.warmSignal.notify_all();
    publication.warmWorker.join();
}

void PublishResolvedConfig(SharedMemoryLayout* sharedMemory, const char* reason) {
    std::lock_guard<std::mutex> lock(Publication().mutex);
    PublishResolvedConfigLocked(sharedMemory, reason);
}

void PublishResolvedConfigForTarget(SharedMemoryLayout* sharedMemory, const std::string& targetProcessName,
                                    const char* reason) {
    PublicationState& publication = Publication();
    std::lock_guard<std::mutex> lock(publication.mutex);
    if (!targetProcessName.empty())
        publication.targetProcess = targetProcessName;
    PublishResolvedConfigLocked(sharedMemory, reason);
}

bool TogglePublishedOverlayVisibility(SharedMemoryLayout* sharedMemory) {
    PublicationState& publication = Publication();
    std::lock_guard<std::mutex> lock(publication.mutex);
    std::string targetProcess;
    AppConfig resolved = ResolveActiveConfigLocked(sharedMemory, targetProcess);
    // Flip the resolved target state so a profile-level override cannot turn
    // the first press into a no-op or disappear on the next publication.
    publication.overlayVisibility =
        ToggleOverlayVisibility(publication.overlayVisibility, resolved.overlay.showOverlay);
    ApplyOverlayVisibility(publication.overlayVisibility, resolved);
    PublishConfigLocked(sharedMemory, resolved, targetProcess, "hotkey:toggle-overlay");
    return publication.overlayVisibility.showOverlay;
}

// Persists the injection whitelist in CE's per-user registry key, for the
// Vulkan layer's negotiation-time decision while no host is running
// (vulkan_layer_target_list.h). Written only when the contents change.
static void PersistVulkanLayerTargetList(const std::vector<std::string>& names) {
    std::vector<std::wstring> wideNames;
    wideNames.reserve(names.size());
    for (const std::string& name : names) {
        const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(),
                                               static_cast<int>(name.size()), nullptr, 0);
        if (length <= 0) {
            LogWarn("[Inject] Vulkan layer target list skips an executable name that is not valid UTF-8");
            continue;
        }
        std::wstring wide(static_cast<size_t>(length), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(), static_cast<int>(name.size()), wide.data(),
                            length);
        wideNames.push_back(std::move(wide));
    }
    const std::wstring contents = ce::vulkan_layer_targets::SerializeTargetList(wideNames);

    std::wstring current;
    if (ce::vulkan_layer_targets::ReadPersistedTargetList(&current) && current == contents)
        return;
    const LONG status = ce::vulkan_layer_targets::WritePersistedTargetList(contents);
    if (status != ERROR_SUCCESS) {
        LogWarn("[Inject] Vulkan layer target list not written to HKCU\\%ls\\%ls (error=%ld); Vulkan titles "
                "started before CaptureEngine keep the previous list",
                ce::vulkan_layer_targets::kRegistryKey, ce::vulkan_layer_targets::kRegistryValue, status);
        return;
    }
    LogInfo("[Inject] Vulkan layer target list updated in HKCU\\%ls\\%ls (%zu executable(s))",
            ce::vulkan_layer_targets::kRegistryKey, ce::vulkan_layer_targets::kRegistryValue, wideNames.size());
}

void PopulateWhitelistCache(DiscoveryInfo* discovery, const AppConfig& config) {
    if (!discovery)
        return;
    memset(discovery->processWhitelist, 0, sizeof(discovery->processWhitelist));

    char* output = discovery->processWhitelist;
    char* end = output + sizeof(discovery->processWhitelist) - 2;
    std::vector<std::string> persistedNames;
    auto addName = [&](const std::string& name) {
        if (name.empty())
            return;
        persistedNames.push_back(name);
        const size_t length = name.length();
        if (output + length + 1 < end) {
            std::string lower = name;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            memcpy(output, lower.c_str(), length);
            output += length;
            *output++ = '\0';
        }
    };

    size_t gameCount = 0;
    size_t overlayCount = 0;
    for (const auto& entry : config.gameWhitelist) {
        addName(entry.pattern);
        ++gameCount;
        if (IsTraceLoggingEnabled(config.logLevel))
            LogInfo("[Inject] Added game to whitelist cache: %s", entry.pattern.c_str());
    }
    for (const auto& entry : config.overlayWhitelist) {
        addName(entry.pattern);
        ++overlayCount;
        if (IsTraceLoggingEnabled(config.logLevel))
            LogInfo("[Inject] Added overlay target to whitelist cache: %s", entry.pattern.c_str());
    }

    if (IsTraceLoggingEnabled(config.logLevel)) {
        addName("dx12_test.exe");
        addName("dx11_test.exe");
        addName("vulkan_test.exe");
    }
    *output = '\0';
    PersistVulkanLayerTargetList(persistedNames);
    LogInfo("[Inject] Whitelist cache prepared: games=%zu overlayTargets=%zu traceExtras=%d", gameCount,
            overlayCount, IsTraceLoggingEnabled(config.logLevel) ? 1 : 0);
}
