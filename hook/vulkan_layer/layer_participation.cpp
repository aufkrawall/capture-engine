#include "layer_participation.h"

#include <tlhelp32.h>

#include <cstring>
#include <string>
#include <string_view>

#include "common/graphics/vulkan_layer_target_list.h"
#include "common/platform/process_parent.h"
#include "hook/present/vulkan_renderer_policy.h"

namespace ce::vulkan_layer_participation {
namespace {

// The fallback when ntdll does not answer: a machine-wide process snapshot,
// which is what every lookup used to cost.
bool FindParentInSnapshot(DWORD* parentPid, wchar_t* parentName, size_t parentNameSize) {
    if (!parentPid || !parentName || parentNameSize == 0)
        return false;
    *parentPid = 0;
    parentName[0] = L'\0';

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
                // PROCESSENTRY32W reports UTF-16 already; matching is UTF-16
                // end to end, so no conversion happens here.
                wcsncpy(parentName, entry.szExeFile, parentNameSize - 1);
                parentName[parentNameSize - 1] = L'\0';
                foundParent = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return foundParent;
}

// This process's parent PID from its own kernel record: no other process is
// opened and nothing machine-wide is enumerated.
bool QueryCurrentParentPid(DWORD* parentPid) {
    return ce::process_parent::QueryParentProcessId(GetCurrentProcess(), parentPid);
}

enum class ParentNameLookup { Resolved, PidReused, Unavailable };

// The parent's executable base name, read from that one process. A parent that
// cannot be opened is Unavailable (the caller falls back to the snapshot); one
// created after this process holds a reused PID and proves nothing.
ParentNameLookup QueryParentImageName(DWORD parentPid, wchar_t* parentName, size_t parentNameSize) {
    parentName[0] = 0;
    HANDLE parent = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parentPid);
    if (!parent)
        return ParentNameLookup::Unavailable;
    ParentNameLookup result = ParentNameLookup::Unavailable;
    if (!ce::process_parent::ParentPredatesChild(ce::process_parent::CreationTime(parent),
                                                 ce::process_parent::CreationTime(GetCurrentProcess()))) {
        result = ParentNameLookup::PidReused;
    } else {
        const std::wstring name = ce::process_parent::ImageBaseName(parent);
        if (!name.empty()) {
            wcsncpy(parentName, name.c_str(), parentNameSize - 1);
            parentName[parentNameSize - 1] = 0;
            result = ParentNameLookup::Resolved;
        }
    }
    CloseHandle(parent);
    return result;
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

void GetCurrentProcessBaseNameWide(wchar_t* out, size_t outSize) {
    if (!out || outSize == 0)
        return;
    out[0] = L'\0';
    // A truncated GetModuleFileNameW result would yield a wrong base name, so
    // the fetch buffer is the long-path size the persisted path always used.
    std::wstring exePath(32768, L'\0');
    const DWORD exeLength = GetModuleFileNameW(nullptr, exePath.data(), static_cast<DWORD>(exePath.size()));
    if (exeLength == 0 || exeLength >= exePath.size())
        return;
    exePath.resize(exeLength);
    const size_t separator = exePath.find_last_of(L"\\/");
    const std::wstring exeName = separator == std::wstring::npos ? exePath : exePath.substr(separator + 1);
    wcsncpy(out, exeName.c_str(), outSize - 1);
    out[outSize - 1] = L'\0';
}

void GetCurrentProcessBaseName(char* out, size_t outSize) {
    if (!out || outSize == 0)
        return;
    out[0] = '\0';
    wchar_t wideName[MAX_PATH] = {};
    GetCurrentProcessBaseNameWide(wideName, _countof(wideName));
    WideCharToMultiByte(CP_UTF8, 0, wideName, -1, out, static_cast<int>(outSize), nullptr, nullptr);
    out[outSize - 1] = '\0';
}

bool IsProcessNameWhitelisted(const DiscoveryInfo* info, const wchar_t* processName) {
    if (!info || !processName)
        return false;
    // The published whitelist is UTF-8; IsProcessNameListedUtf8 converts its
    // entries through the one conversion and matches in UTF-16.
    return ce::vulkan_layer_targets::IsProcessNameListedUtf8(
        std::string_view(info->processWhitelist, sizeof(info->processWhitelist)), processName);
}

bool IsProcessEligibleByDiscovery(const DiscoveryInfo* info, const char* processName, DWORD* inheritedParentPid) {
    if (!processName) {
        if (inheritedParentPid)
            *inheritedParentPid = 0;
        return false;
    }
    // The UTF-8 spelling makes one round trip through the same conversion the
    // published entries use, so nothing is lost for non-ASCII names.
    const std::wstring wideName = ce::vulkan_layer_targets::Utf8ToWide(std::string_view(processName));
    return IsProcessEligibleByDiscovery(info, wideName.c_str(), inheritedParentPid);
}

bool IsProcessEligibleByDiscovery(const DiscoveryInfo* info, const wchar_t* processName, DWORD* inheritedParentPid) {
    if (inheritedParentPid)
        *inheritedParentPid = 0;
    if (!ValidateDiscoveryInfo(info))
        return false;
    if (IsProcessNameWhitelisted(info, processName))
        return true;

    // Only a parent whose PID is a published identity can make this process
    // eligible. Every other Vulkan process on the machine stops here, having
    // read its own parent PID and nothing else.
    DWORD parentPid = 0;
    wchar_t parentName[MAX_PATH] = {};
    bool parentNameKnown = false;
    if (!QueryCurrentParentPid(&parentPid))
        parentNameKnown = FindParentInSnapshot(&parentPid, parentName, _countof(parentName));
    const uint32_t profileTargetPid = info->GetProfileTargetPid();
    const uint32_t activeSourcePid =
        parentPid != 0 && parentPid != profileTargetPid ? ReadActiveSourcePid(info) : 0;
    if (!ce::vulkan_renderer_policy::IsParentAPublishedTarget(parentPid, activeSourcePid, profileTargetPid))
        return false;

    if (!parentNameKnown) {
        const ParentNameLookup lookup = QueryParentImageName(parentPid, parentName, _countof(parentName));
        if (lookup == ParentNameLookup::PidReused)
            return false;
        if (lookup == ParentNameLookup::Unavailable) {
            DWORD snapshotParentPid = 0;
            parentNameKnown = FindParentInSnapshot(&snapshotParentPid, parentName, _countof(parentName)) &&
                              snapshotParentPid == parentPid;
        } else {
            parentNameKnown = true;
        }
    }
    const bool parentProcessWhitelisted = parentNameKnown && IsProcessNameWhitelisted(info, parentName);
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
    wchar_t exeName[MAX_PATH] = {};
    GetCurrentProcessBaseNameWide(exeName, _countof(exeName));

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

Decision DecideParticipation(const wchar_t* processName) {
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
    // The persisted list is consulted even when a host is published and has not
    // made this process eligible yet; a host must never mask it (see
    // ShouldLayerParticipate). Host-eligible processes skip the registry read.
    if (!decision.eligibleByHost)
        decision.listedTarget = IsListedAsResidentTarget();
    decision.participate = ce::vulkan_layer_targets::ShouldLayerParticipate(
        decision.hostPublished, decision.eligibleByHost, decision.listedTarget);
    return decision;
}

}  // namespace ce::vulkan_layer_participation
