#include "layer_participation.h"

#include <tlhelp32.h>

#include <cstring>
#include <string>

#include "../../common/vulkan_layer_target_list.h"
#include "../common/vulkan_renderer_policy.h"

namespace ce::vulkan_layer_participation {
namespace {

bool GetCurrentParentIdentity(DWORD* parentPid, char* parentName, size_t parentNameSize) {
    if (!parentPid || !parentName || parentNameSize == 0)
        return false;
    *parentPid = 0;
    parentName[0] = '\0';

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return false;

    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    bool foundCurrent = false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == GetCurrentProcessId()) {
                *parentPid = entry.th32ParentProcessID;
                foundCurrent = *parentPid != 0;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }

    bool foundParent = false;
    entry = {};
    entry.dwSize = sizeof(entry);
    if (foundCurrent && Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == *parentPid) {
                foundParent = WideCharToMultiByte(CP_UTF8, 0, entry.szExeFile, -1, parentName,
                                                  static_cast<int>(parentNameSize), nullptr, nullptr) > 0;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return foundParent;
}

uint32_t ReadActiveSourcePid(const DiscoveryInfo* info) {
    if (!info || info->GetInjectPid() == 0)
        return 0;

    wchar_t sharedMemName[64] = {};
    GenerateSharedMemName(sharedMemName, _countof(sharedMemName), info->GetInjectPid());
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, sharedMemName);
    if (!mapping)
        return 0;

    auto* sharedMemory = static_cast<SharedMemoryLayout*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
    const uint32_t sourcePid = ValidateSharedMemory(sharedMemory) ? sharedMemory->GetSourcePid() : 0;
    if (sharedMemory)
        UnmapViewOfFile(sharedMemory);
    CloseHandle(mapping);
    return sourcePid;
}

}  // namespace

void GetCurrentProcessBaseName(char* out, size_t outSize) {
    if (!out || outSize == 0)
        return;
    char fullPath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, fullPath, sizeof(fullPath));
    const char* base = strrchr(fullPath, '\\');
    strncpy(out, base ? base + 1 : fullPath, outSize - 1);
    out[outSize - 1] = '\0';
}

bool IsProcessNameWhitelisted(const DiscoveryInfo* info, const char* processName) {
    if (!info || !processName)
        return false;

    const char* entry = info->processWhitelist;
    const char* end = entry + sizeof(info->processWhitelist);
    while (entry < end && *entry != '\0') {
        if (_stricmp(processName, entry) == 0)
            return true;
        const size_t remaining = static_cast<size_t>(end - entry);
        const size_t length = strnlen(entry, remaining);
        if (length == remaining)
            break;
        entry += length + 1;
    }
    return false;
}

bool IsProcessEligibleByDiscovery(const DiscoveryInfo* info, const char* processName, DWORD* inheritedParentPid) {
    if (inheritedParentPid)
        *inheritedParentPid = 0;
    if (!ValidateDiscoveryInfo(info))
        return false;
    if (IsProcessNameWhitelisted(info, processName))
        return true;

    DWORD parentPid = 0;
    char parentName[MAX_PATH] = {};
    const bool parentKnown = GetCurrentParentIdentity(&parentPid, parentName, sizeof(parentName));
    const uint32_t activeSourcePid = parentKnown ? ReadActiveSourcePid(info) : 0;
    const uint32_t profileTargetPid = info->GetProfileTargetPid();
    const bool parentProcessWhitelisted = parentKnown && IsProcessNameWhitelisted(info, parentName);
    const bool eligible = ce::vulkan_renderer_policy::ShouldEnableVulkanLayerForProfile(
        false, parentPid, activeSourcePid, profileTargetPid, parentProcessWhitelisted);
    if (eligible && inheritedParentPid)
        *inheritedParentPid = parentPid;
    return eligible;
}

bool IsCompatibleHostPublished() {
    HANDLE discovery = OpenFileMappingW(FILE_MAP_READ, FALSE, SHARED_MEM_DISCOVERY);
    if (!discovery)
        return false;
    auto* info = static_cast<DiscoveryInfo*>(MapViewOfFile(discovery, FILE_MAP_READ, 0, 0, sizeof(DiscoveryInfo)));
    const bool compatible = ValidateDiscoveryInfo(info);
    if (info)
        UnmapViewOfFile(info);
    CloseHandle(discovery);
    return compatible;
}

bool IsListedAsResidentTarget() {
    std::wstring exePath(32768, L'\0');
    const DWORD exeLength = GetModuleFileNameW(nullptr, exePath.data(), static_cast<DWORD>(exePath.size()));
    if (exeLength == 0 || exeLength >= exePath.size())
        return false;
    exePath.resize(exeLength);
    const size_t separator = exePath.find_last_of(L"\\/");
    const std::wstring exeName = separator == std::wstring::npos ? exePath : exePath.substr(separator + 1);

    std::wstring list;
    if (!ce::vulkan_layer_targets::ReadPersistedTargetList(&list))
        return false;
    return ce::vulkan_layer_targets::IsProcessNameListed(list, exeName);
}

bool ReadPublishedHostLogging(char* logsPath, size_t logsPathSize) {
    if (logsPath && logsPathSize > 0)
        logsPath[0] = '\0';
    HANDLE discovery = OpenFileMappingW(FILE_MAP_READ, FALSE, SHARED_MEM_DISCOVERY);
    if (!discovery)
        return false;

    bool debugLogging = false;
    auto* info = static_cast<DiscoveryInfo*>(MapViewOfFile(discovery, FILE_MAP_READ, 0, 0, sizeof(DiscoveryInfo)));
    if (ValidateDiscoveryInfo(info)) {
        if (logsPath && logsPathSize > 0) {
            strncpy(logsPath, info->logsPath, logsPathSize - 1);
            logsPath[logsPathSize - 1] = '\0';
        }
        const uint32_t hostPid = info->GetInjectPid();
        if (hostPid != 0) {
            wchar_t sharedMemName[64] = {};
            GenerateSharedMemName(sharedMemName, _countof(sharedMemName), hostPid);
            HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, sharedMemName);
            if (mapping) {
                auto* sharedMemory = static_cast<SharedMemoryLayout*>(
                    MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(SharedMemoryLayout)));
                if (ValidateSharedMemory(sharedMemory))
                    debugLogging = sharedMemory->GetDebugLogging();
                if (sharedMemory)
                    UnmapViewOfFile(sharedMemory);
                CloseHandle(mapping);
            }
        }
    }
    if (info)
        UnmapViewOfFile(info);
    CloseHandle(discovery);
    return debugLogging;
}

Decision DecideParticipation(const char* processName) {
    Decision decision;
    HANDLE discovery = OpenFileMappingW(FILE_MAP_READ, FALSE, SHARED_MEM_DISCOVERY);
    if (discovery) {
        auto* info =
            static_cast<DiscoveryInfo*>(MapViewOfFile(discovery, FILE_MAP_READ, 0, 0, sizeof(DiscoveryInfo)));
        if (ValidateDiscoveryInfo(info)) {
            decision.hostPublished = true;
            decision.eligibleByHost = IsProcessEligibleByDiscovery(info, processName, &decision.inheritedParentPid);
        }
        if (info)
            UnmapViewOfFile(info);
        CloseHandle(discovery);
    }
    if (!decision.hostPublished)
        decision.listedTarget = IsListedAsResidentTarget();
    decision.participate = ce::vulkan_layer_targets::ShouldLayerParticipate(
        decision.hostPublished, decision.eligibleByHost, decision.listedTarget);
    return decision;
}

}  // namespace ce::vulkan_layer_participation
