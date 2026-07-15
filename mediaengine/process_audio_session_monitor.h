#pragma once

#include <audiopolicy.h>
#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>

namespace ce::process_loopback {

struct AudioSessionCreation {
    uint64_t generation = 0;
    DWORD processId = 0;
};

// Watches every active render endpoint for newly-created Windows audio
// sessions. Process-loopback clients can remain permanently empty when they are
// activated before the target creates its first render session; the capture
// owner uses these notifications to recycle into a fresh helper generation at
// that exact topology change instead of guessing with a timeout.
class ProcessAudioSessionMonitor {
public:
    ProcessAudioSessionMonitor();
    ~ProcessAudioSessionMonitor();

    ProcessAudioSessionMonitor(const ProcessAudioSessionMonitor&) = delete;
    ProcessAudioSessionMonitor& operator=(const ProcessAudioSessionMonitor&) = delete;

    bool Start(HANDLE cancellationEvent = nullptr);
    void Stop();

    bool IsRunning() const;
    uint64_t Generation() const;
    HANDLE GetActivityEvent() const;
    size_t TakeSessionCreations(AudioSessionCreation* output, size_t capacity) noexcept;
    uint64_t SnapshotGenerationAndObservedProcessIds(DWORD* output, size_t capacity, size_t* outputCount) noexcept;

    size_t RegisteredEndpointCount() const {
        return registeredEndpointCount_.load(std::memory_order_acquire);
    }
    size_t ActiveEndpointCount() const {
        return activeEndpointCount_.load(std::memory_order_acquire);
    }
    size_t RegistrationFailureCount() const {
        return registrationFailureCount_.load(std::memory_order_acquire);
    }
    HRESULT FirstRegistrationFailure() const {
        return firstRegistrationFailure_.load(std::memory_order_acquire);
    }
    size_t ExistingSessionCount() const {
        return existingSessionCount_.load(std::memory_order_acquire);
    }
    HRESULT StartupResult() const {
        return startupResult_.load(std::memory_order_acquire);
    }
    uint64_t DroppedNotificationCount() const;

private:
    class NotificationHandler;
    struct SharedState;

    void ThreadMain();

    static constexpr size_t kMaxPendingNotifications = 1024;
    static constexpr size_t kMaxObservedSessionProcesses = 1024;

    HANDLE readyEvent_ = nullptr;
    HANDLE stopEvent_ = nullptr;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<size_t> activeEndpointCount_{0};
    std::atomic<size_t> registeredEndpointCount_{0};
    std::atomic<size_t> registrationFailureCount_{0};
    std::atomic<size_t> existingSessionCount_{0};
    std::atomic<HRESULT> firstRegistrationFailure_{S_OK};
    std::atomic<HRESULT> startupResult_{E_FAIL};
    std::shared_ptr<SharedState> state_;
};

}  // namespace ce::process_loopback
