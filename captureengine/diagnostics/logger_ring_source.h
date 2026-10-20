#pragma once

#include <windows.h>

#include <cstdint>

#include "common/ipc/shared_defs.h"

// The shared rings the logger drains: the discovery record that names the injected PID and the session
// logs directory it writes to, and one log ring per PID, written by the hooks and layers. The logger
// consumes each ring alone (one consumer per ring). The production source opens the named mappings; tests
// inject in-memory rings through this interface.

struct LoggerDiscovery {
    uint32_t injectPid = 0;
    char logsPath[sizeof(DiscoveryInfo::logsPath)] = {};
};

// One mapped ring. Both members are null when the ring is not open.
struct LoggerRing {
    SharedMemoryLayout* shm = nullptr;
    HANDLE handle = nullptr;  // the mapping, released by CloseRing
};

class LoggerRingSource {
public:
    virtual ~LoggerRingSource() = default;
    // False when no valid discovery record is published.
    virtual bool ReadDiscovery(LoggerDiscovery& discovery) = 0;
    // The ring of `pid`, mapped but not validated (the caller checks ABI and version). Null when absent.
    virtual LoggerRing OpenRing(uint32_t pid) = 0;
    // Releases a ring from OpenRing; a null ring is a no-op.
    virtual void CloseRing(LoggerRing& ring) = 0;
};

// The process-wide named mappings (SHARED_MEM_DISCOVERY and the per-PID ring names). The discovery mapping is
// opened and closed per call; a ring stays mapped until CloseRing.
class SharedLoggerRings final : public LoggerRingSource {
public:
    bool ReadDiscovery(LoggerDiscovery& discovery) override;
    LoggerRing OpenRing(uint32_t pid) override;
    void CloseRing(LoggerRing& ring) override;
};
