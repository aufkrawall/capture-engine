#pragma once

#include <windows.h>
#include <cstdint>

enum class RecordingStartIntent : uint8_t;
enum class OverlayNotificationType : uint32_t;

namespace ce::ipc {

enum class ControlStatus { Ok, Unavailable, MappingFailed, InvalidDiscovery, InvalidSharedMemory, StaleTarget };

struct ControlResult {
    ControlStatus status = ControlStatus::Unavailable;
    uint32_t injectPid = 0;
    explicit operator bool() const { return status == ControlStatus::Ok; }
};

// Independently published health fields. No coherent multi-field snapshot is promised.
struct RecordingHealthObservation {
    bool live = false;
    bool audioOnly = false;
    int64_t liveSince = 0;
    uint32_t failure = 0;
};

// Scoped low-frequency controller transactions, not the per-frame transport.
// expectedProcess is borrowed for each synchronous operation; a dead/replaced child is rejected.
// discoveryName is an internal test seam for isolated named mappings, not a wire/API extension.
class InjectControlChannel {
public:
    explicit InjectControlChannel(HANDLE expectedProcess = nullptr, const wchar_t* discoveryName = nullptr)
        : expectedProcess_(expectedProcess), discoveryName_(discoveryName) {}
    ControlResult PublishRecordingIntent(RecordingStartIntent intent) const;
    ControlResult PublishNotification(OverlayNotificationType type, uint64_t expiry) const;
    ControlResult ReadRecordingHealth(RecordingHealthObservation& observation) const;
    ControlResult ConsumeRecordingFailure(uint32_t expectedFailure) const;
    ControlResult ClearDeadMediaState() const;

private:
    HANDLE expectedProcess_;
    const wchar_t* discoveryName_;
};

}  // namespace ce::ipc
