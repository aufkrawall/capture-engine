#include "inject_config_publication.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <mutex>

#include "../common/config.h"
#include "../common/inject_overlay_policy.h"
#include "../common/logging.h"
#include "../common/process_identity.h"
#include "../common/shared_defs.h"
#include "inject_config.h"

namespace {

// Every shared config publication is serialized here. The overlay-config
// seqlock permits one writer, while injection workers, IPC commands, and hook
// source transitions can all publish from different threads.
struct PublicationState {
    std::mutex mutex;
    std::string configPath;
    AppConfig baseConfig;
    std::string targetProcess;
    OverlayVisibilityOverride overlayVisibility;
};

PublicationState& Publication() {
    static PublicationState state;
    return state;
}

AppConfig ResolveActiveConfigLocked(SharedMemoryLayout* sharedMemory, std::string& targetProcessOut) {
    std::string hookSourceProcess;
    if (sharedMemory) {
        const uint32_t sourcePid = sharedMemory->GetSourcePid();
        if (sourcePid != 0)
            hookSourceProcess = GetProcessNameFromPID(sourcePid);
    }

    const PublicationState& publication = Publication();
    targetProcessOut = ResolveActiveTargetProcessName(publication.targetProcess, hookSourceProcess);
    return ResolveTargetConfig(publication.configPath, publication.baseConfig, targetProcessOut);
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

void PopulateWhitelistCache(DiscoveryInfo* discovery, const AppConfig& config) {
    if (!discovery)
        return;
    memset(discovery->processWhitelist, 0, sizeof(discovery->processWhitelist));

    char* output = discovery->processWhitelist;
    char* end = output + sizeof(discovery->processWhitelist) - 2;
    auto addName = [&](const std::string& name) {
        if (name.empty())
            return;
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
    LogInfo("[Inject] Whitelist cache prepared: games=%zu overlayTargets=%zu traceExtras=%d", gameCount,
            overlayCount, IsTraceLoggingEnabled(config.logLevel) ? 1 : 0);
}
