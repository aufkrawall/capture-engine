#include "logger_ring_source.h"

#include <cstring>

bool SharedLoggerRings::ReadDiscovery(LoggerDiscovery& discovery) {
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, SHARED_MEM_DISCOVERY);
    if (!mapping)
        return false;
    bool valid = false;
    auto* info = static_cast<DiscoveryInfo*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(DiscoveryInfo)));
    if (info) {
        valid = ValidateDiscoveryInfo(info);
        if (valid) {
            discovery.injectPid = info->GetInjectPid();
            std::memcpy(discovery.logsPath, info->logsPath, sizeof(discovery.logsPath));
        }
        UnmapViewOfFile(info);
    }
    CloseHandle(mapping);
    return valid;
}

LoggerRing SharedLoggerRings::OpenRing(uint32_t pid) {
    wchar_t name[64];
    GenerateSharedMemName(name, 64, pid);
    HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!mapping)
        return {};
    auto* shm =
        static_cast<SharedMemoryLayout*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedMemoryLayout)));
    if (!shm) {
        CloseHandle(mapping);
        return {};
    }
    return LoggerRing{shm, mapping};
}

void SharedLoggerRings::CloseRing(LoggerRing& ring) {
    if (ring.shm)
        UnmapViewOfFile(ring.shm);
    if (ring.handle)
        CloseHandle(ring.handle);
    ring = {};
}
