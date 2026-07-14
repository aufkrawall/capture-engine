#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ce::process_loopback {

struct ProcessTreeEntry {
    uint32_t processId = 0;
    uint32_t parentProcessId = 0;
    std::string executableName;
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
    return std::equal(lhs.begin(), lhs.end(), rhs.begin(), [](unsigned char a, unsigned char b) {
        return std::tolower(a) == std::tolower(b);
    });
}

inline ProcessNameSelection SelectProcessTreeRootByName(const std::vector<ProcessTreeEntry>& processes,
                                                         const std::string& executableName) {
    ProcessNameSelection result;
    if (executableName.empty()) {
        return result;
    }

    std::unordered_map<uint32_t, const ProcessTreeEntry*> byProcessId;
    byProcessId.reserve(processes.size());
    for (const auto& process : processes) {
        if (process.processId != 0) {
            byProcessId[process.processId] = &process;
        }
    }

    std::unordered_map<uint32_t, size_t> membersByRoot;
    for (const auto& process : processes) {
        if (process.processId == 0 || !ProcessExecutableNamesEqual(process.executableName, executableName)) {
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
            if (parentIt == byProcessId.end() ||
                !ProcessExecutableNamesEqual(parentIt->second->executableName, executableName) ||
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
        if (treeSize > largestTreeSize ||
            (treeSize == largestTreeSize && rootProcessId < selectedProcessId)) {
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

inline bool ShouldReactivateUnqualifiedCaptureForTreeGrowth(bool activationQualified, uint32_t activeProcessId,
                                                            size_t activatedTreeSize,
                                                            const ProcessNameSelection& observedSelection) {
    return !activationQualified && activeProcessId != 0 && observedSelection.selectedProcessId == activeProcessId &&
           observedSelection.selectedProcessTreeSize > activatedTreeSize;
}

}  // namespace ce::process_loopback
