#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/config/application_profile.h"

namespace ce::process_loopback {

struct ProcessTreeEntry {
    uint32_t processId = 0;
    uint32_t parentProcessId = 0;
    std::string executableName;
    std::string executablePath = {};
};

struct ProcessNameSelection {
    uint32_t selectedProcessId = 0;
    uint32_t selectedParentProcessId = 0;
    uint32_t firstMatchProcessId = 0;
    size_t matchingProcessCount = 0;
    size_t rootCandidateCount = 0;
    size_t selectedTreeSize = 0;
    size_t selectedProcessTreeSize = 0;
};

inline bool ProcessExecutableNamesEqual(const std::string& lhs, const std::string& rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    return std::equal(lhs.begin(), lhs.end(), rhs.begin(),
                      [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); });
}

inline ProcessNameSelection SelectProcessTreeRootByName(const std::vector<ProcessTreeEntry>& processes,
                                                        const std::string& executableName,
                                                        const std::vector<WhitelistEntry>& exclusions = {}) {
    ProcessNameSelection result;
    if (executableName.empty()) {
        return result;
    }

    WhitelistEntry target;
    target.pattern = executableName;
    const auto matches = [&](const ProcessTreeEntry& process) {
        const ProcessTargetIdentity identity{process.executableName, process.executablePath};
        if (!MatchesProcessTarget(target, identity, true))
            return false;
        for (const WhitelistEntry& exclusion : exclusions) {
            if (MatchesProcessTarget(exclusion, identity))
                return false;
            // If the image path cannot be queried, a path-specific profile
            // with this filename may own the process. Never capture it twice.
            if (process.executablePath.empty() && IsProcessPathSelector(exclusion)) {
                const size_t separator = exclusion.pattern.find_last_of("\\/");
                WhitelistEntry basename;
                basename.pattern = exclusion.pattern.substr(separator + 1);
                if (MatchesProcessTarget(basename, identity, true))
                    return false;
            }
        }
        return true;
    };

    std::unordered_map<uint32_t, const ProcessTreeEntry*> byProcessId;
    byProcessId.reserve(processes.size());
    for (const auto& process : processes) {
        if (process.processId != 0) {
            byProcessId[process.processId] = &process;
        }
    }

    std::unordered_map<uint32_t, size_t> membersByRoot;
    for (const auto& process : processes) {
        if (process.processId == 0 || !matches(process)) {
            continue;
        }
        if (result.firstMatchProcessId == 0) {
            result.firstMatchProcessId = process.processId;
        }
        ++result.matchingProcessCount;

        uint32_t rootProcessId = process.processId;
        uint32_t parentProcessId = process.parentProcessId;
        std::unordered_set<uint32_t> visited;
        visited.reserve(8);
        visited.insert(rootProcessId);
        while (parentProcessId != 0) {
            const auto parentIt = byProcessId.find(parentProcessId);
            if (parentIt == byProcessId.end() || !matches(*parentIt->second) ||
                !visited.insert(parentProcessId).second) {
                break;
            }
            rootProcessId = parentProcessId;
            parentProcessId = parentIt->second->parentProcessId;
        }
        ++membersByRoot[rootProcessId];
    }

    result.rootCandidateCount = membersByRoot.size();
    size_t largestTreeSize = 0;
    uint32_t selectedProcessId = 0;
    for (const auto& [rootProcessId, treeSize] : membersByRoot) {
        if (treeSize > largestTreeSize || (treeSize == largestTreeSize && rootProcessId < selectedProcessId)) {
            selectedProcessId = rootProcessId;
            largestTreeSize = treeSize;
        }
    }

    result.selectedProcessId = selectedProcessId;
    result.selectedTreeSize = largestTreeSize;
    const auto selectedIt = byProcessId.find(selectedProcessId);
    if (selectedIt != byProcessId.end()) {
        result.selectedParentProcessId = selectedIt->second->parentProcessId;
    }

    for (const auto& process : processes) {
        uint32_t processId = process.processId;
        std::unordered_set<uint32_t> visited;
        visited.reserve(8);
        while (processId != 0 && visited.insert(processId).second) {
            if (processId == selectedProcessId) {
                ++result.selectedProcessTreeSize;
                break;
            }
            const auto processIt = byProcessId.find(processId);
            if (processIt == byProcessId.end()) {
                break;
            }
            processId = processIt->second->parentProcessId;
        }
    }
    return result;
}

inline bool ProcessBelongsToTree(const std::vector<ProcessTreeEntry>& processes, uint32_t processId,
                                 uint32_t rootProcessId) {
    if (processId == 0 || rootProcessId == 0) {
        return false;
    }

    uint32_t currentProcessId = processId;
    // This runs only on rare session-creation notifications. A bounded linear
    // walk avoids allocating another hash table on the capture thread while
    // still terminating safely on corrupt/cyclic parent data.
    for (size_t depth = 0; currentProcessId != 0 && depth <= processes.size(); ++depth) {
        if (currentProcessId == rootProcessId) {
            return true;
        }
        const auto processIt = std::find_if(processes.begin(), processes.end(),
                                            [&](const auto& process) { return process.processId == currentProcessId; });
        if (processIt == processes.end()) {
            return false;
        }
        currentProcessId = processIt->parentProcessId;
    }
    return false;
}

inline bool ShouldRecycleCaptureForSessionCreation(const std::vector<ProcessTreeEntry>& processes,
                                                   bool activationQualified, bool activationHadObservedTargetSession,
                                                   uint32_t activeRootProcessId, uint32_t sessionProcessId,
                                                   uint64_t activationGeneration, uint64_t notificationGeneration) {
    return notificationGeneration > activationGeneration &&
           ProcessBelongsToTree(processes, sessionProcessId, activeRootProcessId) &&
           (!activationQualified || !activationHadObservedTargetSession);
}

}  // namespace ce::process_loopback
