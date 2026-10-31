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
    size_t excludedProcessCount = 0;
    size_t excludedTreeOverlapCount = 0;
    size_t unresolvedPathProcessCount = 0;
    uint32_t firstUnresolvedPathProcessId = 0;
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
                                                        const std::vector<WhitelistEntry>& exclusions = {},
                                                        uint32_t activeRootProcessId = 0) {
    ProcessNameSelection result;
    if (executableName.empty()) {
        return result;
    }

    WhitelistEntry target;
    target.pattern = executableName;
    const auto basenameSelector = [](const WhitelistEntry& entry) {
        WhitelistEntry basename = entry;
        const size_t separator = basename.pattern.find_last_of("\\/");
        if (separator != std::string::npos)
            basename.pattern.erase(0, separator + 1);
        return basename;
    };
    const WhitelistEntry targetBasename = basenameSelector(target);
    const auto matchesTarget = [&](const ProcessTreeEntry& process) {
        return MatchesProcessTarget(target, ProcessTargetIdentity{process.executableName, process.executablePath},
                                    true);
    };

    std::unordered_map<uint32_t, const ProcessTreeEntry*> byProcessId;
    byProcessId.reserve(processes.size());
    for (const auto& process : processes) {
        if (process.processId != 0) {
            byProcessId[process.processId] = &process;
        }
    }

    std::unordered_set<uint32_t> directlyExcluded;
    directlyExcluded.reserve(processes.size());
    for (const auto& process : processes) {
        if (process.processId == 0)
            continue;
        const ProcessTargetIdentity identity{process.executableName, process.executablePath};
        const bool targetMatched = matchesTarget(process);
        bool unresolvedPath = process.executablePath.empty() && IsProcessPathSelector(target) &&
                              MatchesProcessTarget(targetBasename, identity, true);
        bool excluded = false;
        for (const WhitelistEntry& exclusion : exclusions) {
            if (MatchesProcessTarget(exclusion, identity)) {
                excluded = true;
                break;
            }
            // If the image path cannot be queried, a path-specific profile
            // with this filename may own the process. Never capture it twice.
            if (process.executablePath.empty() && IsProcessPathSelector(exclusion) &&
                MatchesProcessTarget(basenameSelector(exclusion), identity, true)) {
                excluded = true;
                if (targetMatched || MatchesProcessTarget(targetBasename, identity, true))
                    unresolvedPath = true;
                break;
            }
        }
        if (unresolvedPath) {
            ++result.unresolvedPathProcessCount;
            if (result.firstUnresolvedPathProcessId == 0)
                result.firstUnresolvedPathProcessId = process.processId;
        }
        if (excluded) {
            directlyExcluded.insert(process.processId);
            if (targetMatched && !unresolvedPath)
                ++result.excludedProcessCount;
        }
    }

    // Any ancestor of an excluded process contains that excluded process in its
    // Windows process-loopback tree (PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE).
    // Conversely, any descendant of an excluded process (or of a target-matching
    // ancestor that contains an excluded descendant) belongs to that excluded
    // installation tree and must not be captured a second time by a generic profile.
    std::unordered_set<uint32_t> hasExcludedDescendant;
    hasExcludedDescendant.reserve(directlyExcluded.size() * 4);
    for (const uint32_t excludedPid : directlyExcluded) {
        uint32_t currentPid = excludedPid;
        std::unordered_set<uint32_t> visited;
        visited.reserve(8);
        while (currentPid != 0 && visited.insert(currentPid).second) {
            hasExcludedDescendant.insert(currentPid);
            const auto it = byProcessId.find(currentPid);
            if (it == byProcessId.end())
                break;
            currentPid = it->second->parentProcessId;
        }
    }

    std::unordered_set<uint32_t> eligibleProcessIds;
    eligibleProcessIds.reserve(processes.size());
    for (const auto& process : processes) {
        if (process.processId == 0 || !matchesTarget(process))
            continue;
        if (directlyExcluded.find(process.processId) != directlyExcluded.end())
            continue;
        bool treeOverlap = hasExcludedDescendant.find(process.processId) != hasExcludedDescendant.end();
        if (!treeOverlap) {
            uint32_t parentPid = process.parentProcessId;
            std::unordered_set<uint32_t> visited;
            visited.reserve(8);
            visited.insert(process.processId);
            while (parentPid != 0 && visited.insert(parentPid).second) {
                const auto parentIt = byProcessId.find(parentPid);
                if (parentIt == byProcessId.end())
                    break;
                if (directlyExcluded.find(parentPid) != directlyExcluded.end() ||
                    (hasExcludedDescendant.find(parentPid) != hasExcludedDescendant.end() &&
                     matchesTarget(*parentIt->second))) {
                    treeOverlap = true;
                    break;
                }
                parentPid = parentIt->second->parentProcessId;
            }
        }
        if (treeOverlap) {
            ++result.excludedTreeOverlapCount;
            continue;
        }
        eligibleProcessIds.insert(process.processId);
    }

    const auto matches = [&](const ProcessTreeEntry& process) {
        return eligibleProcessIds.find(process.processId) != eligibleProcessIds.end();
    };

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
    // Live validation retains a still-eligible binding. An unrelated larger
    // installation starting must not interrupt the stream already being captured.
    const auto activeRoot = membersByRoot.find(activeRootProcessId);
    if (activeRoot != membersByRoot.end()) {
        selectedProcessId = activeRoot->first;
        largestTreeSize = activeRoot->second;
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
