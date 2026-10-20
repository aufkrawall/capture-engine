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

// The owner an inject channel is bound to (DR-46). A helper process is its own owner: its process handle, whose
// PID is read from the handle. The in-process inject role is a thread of the client: its thread handle signals
// when the role ends, and `pid` is the client process that discovery reports. A null handle never matches.
struct InjectOwner {
    HANDLE waitable = nullptr;
    uint32_t pid = 0;
};

// Scoped low-frequency controller transactions, not the per-frame transport.
// The owner is borrowed for each synchronous operation; a dead/replaced owner, or a discovery PID other than the
// owner's, is rejected. A channel without an owner (nullptr process) checks nothing.
// discoveryName is an internal test seam for isolated named mappings, not a wire/API extension.
class InjectControlChannel {
public:
    explicit InjectControlChannel(HANDLE expectedProcess = nullptr, const wchar_t* discoveryName = nullptr);
    explicit InjectControlChannel(InjectOwner owner, const wchar_t* discoveryName = nullptr);
    ControlResult PublishRecordingIntent(RecordingStartIntent intent) const;
    ControlResult PublishNotification(OverlayNotificationType type, uint64_t expiry) const;
    ControlResult ReadRecordingHealth(RecordingHealthObservation& observation) const;
    ControlResult ConsumeRecordingFailure(uint32_t expectedFailure) const;
    ControlResult ClearDeadMediaState() const;

private:
    InjectOwner owner_;
    bool bound_ = false;  // false: no owner, nothing is checked
    const wchar_t* discoveryName_ = nullptr;
};

}  // namespace ce::ipc
