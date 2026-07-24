#include "process_audio_session_monitor.h"

#include <mmdeviceapi.h>
#include <objidl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <exception>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

#include "../common/logging.h"

namespace ce::process_loopback {

namespace {

template <typename T>
void ReleaseInterface(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

DWORD GetAudioSessionProcessId(IAudioSessionControl* session) {
    if (!session) {
        return 0;
    }
    IAudioSessionControl2* session2 = nullptr;
    DWORD processId = 0;
    const HRESULT queryResult =
        session->QueryInterface(__uuidof(IAudioSessionControl2), reinterpret_cast<void**>(&session2));
    if (SUCCEEDED(queryResult) && session2) {
        session2->GetProcessId(&processId);
    }
    ReleaseInterface(session2);
    return processId;
}

}  // namespace

struct ProcessAudioSessionMonitor::SharedState {
    SharedState() : activityEvent(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {}
    ~SharedState() {
        if (activityEvent) {
            CloseHandle(activityEvent);
        }
    }

    HANDLE activityEvent = nullptr;
    std::atomic<bool> acceptingNotifications{false};
    std::atomic<uint64_t> generation{0};
    std::atomic<uint64_t> droppedNotifications{0};
    std::mutex pendingMutex;
    std::array<AudioSessionCreation, ProcessAudioSessionMonitor::kMaxPendingNotifications> pendingCreations{};
    size_t pendingHead = 0;
    size_t pendingCount = 0;
    std::array<DWORD, ProcessAudioSessionMonitor::kMaxObservedSessionProcesses> observedProcessIds{};
    size_t observedProcessCount = 0;

    void RememberProcessIdLocked(DWORD processId) noexcept {
        if (processId == 0) {
            return;
        }
        for (size_t index = 0; index < observedProcessCount; ++index) {
            if (observedProcessIds[index] == processId) {
                return;
            }
        }
        if (observedProcessCount < observedProcessIds.size()) {
            observedProcessIds[observedProcessCount++] = processId;
        }
    }

    void RememberProcessId(DWORD processId) noexcept {
        try {
            std::lock_guard<std::mutex> lock(pendingMutex);
            RememberProcessIdLocked(processId);
        } catch (...) {
            // Runs on a WASAPI session-notification callback, so the exception must
            // not escape. Losing the process id means a newly started app-audio
            // session can be missed entirely, which previously looked like the app
            // simply never produced audio.
            static std::atomic<uint64_t> s_rememberFailures{0};
            const uint64_t failures = s_rememberFailures.fetch_add(1, std::memory_order_relaxed) + 1;
            if (failures <= 3 || (failures % 100ull) == 0ull) {
                LogWarn("[AppAudio] Failed to record notified session pid=%lu (occurrence %llu)", processId,
                        static_cast<unsigned long long>(failures));
            }
        }
    }
};

class ProcessAudioSessionMonitor::NotificationHandler final : public IAudioSessionNotification {
public:
    explicit NotificationHandler(std::shared_ptr<SharedState> state) : state_(std::move(state)) {}

    STDMETHODIMP QueryInterface(REFIID interfaceId, void** object) override {
        if (!object) {
            return E_POINTER;
        }
        if (interfaceId == __uuidof(IUnknown) || interfaceId == __uuidof(IAudioSessionNotification) ||
            interfaceId == IID_IAgileObject) {
            *object = static_cast<IAudioSessionNotification*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        return refs_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    STDMETHODIMP_(ULONG) Release() override {
        const ULONG refs = refs_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (refs == 0) {
            delete this;
        }
        return refs;
    }

    STDMETHODIMP OnSessionCreated(IAudioSessionControl* newSession) override {
        if (!newSession || !state_ || !state_->acceptingNotifications.load(std::memory_order_acquire)) {
            return S_OK;
        }

        const DWORD processId = GetAudioSessionProcessId(newSession);
        if (processId == 0) {
            return S_OK;
        }

        try {
            std::lock_guard<std::mutex> lock(state_->pendingMutex);
            const uint64_t generation = state_->generation.fetch_add(1, std::memory_order_acq_rel) + 1;
            state_->RememberProcessIdLocked(processId);
            if (state_->pendingCount == ProcessAudioSessionMonitor::kMaxPendingNotifications) {
                state_->pendingHead = (state_->pendingHead + 1) % ProcessAudioSessionMonitor::kMaxPendingNotifications;
                --state_->pendingCount;
                state_->droppedNotifications.fetch_add(1, std::memory_order_relaxed);
            }
            const size_t tail =
                (state_->pendingHead + state_->pendingCount) % ProcessAudioSessionMonitor::kMaxPendingNotifications;
            state_->pendingCreations[tail] = {generation, processId};
            ++state_->pendingCount;
        } catch (...) {
            state_->droppedNotifications.fetch_add(1, std::memory_order_relaxed);
            return S_OK;
        }
        if (state_->activityEvent) {
            SetEvent(state_->activityEvent);
        }
        return S_OK;
    }

private:
    std::atomic<ULONG> refs_{1};
    std::shared_ptr<SharedState> state_;
};

ProcessAudioSessionMonitor::ProcessAudioSessionMonitor()
    : readyEvent_(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
      stopEvent_(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
      state_(std::make_shared<SharedState>()) {}

ProcessAudioSessionMonitor::~ProcessAudioSessionMonitor() {
    Stop();
    if (stopEvent_) {
        CloseHandle(stopEvent_);
    }
    if (readyEvent_) {
        CloseHandle(readyEvent_);
    }
}

bool ProcessAudioSessionMonitor::Start(HANDLE cancellationEvent) {
    Stop();
    if (!readyEvent_ || !stopEvent_ || !state_ || !state_->activityEvent) {
        startupResult_.store(HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY), std::memory_order_release);
        return false;
    }

    ResetEvent(readyEvent_);
    ResetEvent(stopEvent_);
    ResetEvent(state_->activityEvent);
    state_->acceptingNotifications.store(false, std::memory_order_release);
    state_->generation.store(0, std::memory_order_release);
    state_->droppedNotifications.store(0, std::memory_order_relaxed);
    activeEndpointCount_.store(0, std::memory_order_release);
    registeredEndpointCount_.store(0, std::memory_order_release);
    registrationFailureCount_.store(0, std::memory_order_release);
    existingSessionCount_.store(0, std::memory_order_release);
    firstRegistrationFailure_.store(S_OK, std::memory_order_release);
    startupResult_.store(E_PENDING, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(state_->pendingMutex);
        state_->pendingHead = 0;
        state_->pendingCount = 0;
        state_->observedProcessCount = 0;
    }

    try {
        thread_ = std::thread(&ProcessAudioSessionMonitor::ThreadMain, this);
    } catch (const std::exception&) {
        startupResult_.store(HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY), std::memory_order_release);
        return false;
    }

    HANDLE waitHandles[] = {readyEvent_, cancellationEvent};
    const DWORD waitHandleCount = cancellationEvent ? 2 : 1;
    const DWORD wait = WaitForMultipleObjects(waitHandleCount, waitHandles, FALSE, 5000);
    if (wait != WAIT_OBJECT_0) {
        const HRESULT waitResult = waitHandleCount == 2 && wait == WAIT_OBJECT_0 + 1
                                       ? HRESULT_FROM_WIN32(ERROR_CANCELLED)
                                   : wait == WAIT_FAILED ? HRESULT_FROM_WIN32(GetLastError())
                                                         : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        startupResult_.store(waitResult, std::memory_order_release);
        SetEvent(stopEvent_);
        thread_.join();
        return false;
    }
    return running_.load(std::memory_order_acquire);
}

void ProcessAudioSessionMonitor::Stop() {
    if (state_) {
        state_->acceptingNotifications.store(false, std::memory_order_release);
    }
    if (!thread_.joinable()) {
        running_.store(false, std::memory_order_release);
        return;
    }
    if (stopEvent_) {
        SetEvent(stopEvent_);
    }
    thread_.join();
    running_.store(false, std::memory_order_release);
}

bool ProcessAudioSessionMonitor::IsRunning() const {
    return running_.load(std::memory_order_acquire);
}

uint64_t ProcessAudioSessionMonitor::Generation() const {
    return state_ ? state_->generation.load(std::memory_order_acquire) : 0;
}

uint64_t ProcessAudioSessionMonitor::DroppedNotificationCount() const {
    return state_ ? state_->droppedNotifications.load(std::memory_order_relaxed) : 0;
}

HANDLE ProcessAudioSessionMonitor::GetActivityEvent() const {
    return state_ ? state_->activityEvent : nullptr;
}

size_t ProcessAudioSessionMonitor::TakeSessionCreations(AudioSessionCreation* output, size_t capacity) noexcept {
    if (!state_ || !output || capacity == 0) {
        return 0;
    }

    size_t copied = 0;
    bool morePending = false;
    try {
        std::lock_guard<std::mutex> lock(state_->pendingMutex);
        copied = std::min(capacity, state_->pendingCount);
        for (size_t index = 0; index < copied; ++index) {
            output[index] = state_->pendingCreations[(state_->pendingHead + index) % kMaxPendingNotifications];
        }
        state_->pendingHead = (state_->pendingHead + copied) % kMaxPendingNotifications;
        state_->pendingCount -= copied;
        morePending = state_->pendingCount != 0;
    } catch (...) {
        return 0;
    }
    if (morePending && state_->activityEvent) {
        SetEvent(state_->activityEvent);
    }
    return copied;
}

uint64_t ProcessAudioSessionMonitor::SnapshotGenerationAndObservedProcessIds(DWORD* output, size_t capacity,
                                                                             size_t* outputCount) noexcept {
    if (outputCount) {
        *outputCount = 0;
    }
    if (!state_ || !output || capacity == 0 || !outputCount) {
        return Generation();
    }
    try {
        std::lock_guard<std::mutex> lock(state_->pendingMutex);
        const size_t copied = std::min(capacity, state_->observedProcessCount);
        for (size_t index = 0; index < copied; ++index) {
            output[index] = state_->observedProcessIds[index];
        }
        *outputCount = copied;
        return state_->generation.load(std::memory_order_acquire);
    } catch (...) {
        return Generation();
    }
}

void ProcessAudioSessionMonitor::ThreadMain() {
    const HRESULT coInitResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(coInitResult)) {
        startupResult_.store(coInitResult, std::memory_order_release);
        SetEvent(readyEvent_);
        return;
    }

    IMMDeviceEnumerator* deviceEnumerator = nullptr;
    IMMDeviceCollection* endpoints = nullptr;
    NotificationHandler* notification = new (std::nothrow) NotificationHandler(state_);
    std::vector<IAudioSessionManager2*> managers;
    HRESULT firstFailure = notification ? S_OK : E_OUTOFMEMORY;
    HRESULT result = firstFailure;

    if (notification) {
        result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&deviceEnumerator));
        if (FAILED(result)) {
            firstFailure = result;
        }
    }
    if (SUCCEEDED(result)) {
        result = deviceEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &endpoints);
        if (FAILED(result)) {
            firstFailure = result;
        }
    }

    UINT endpointCount = 0;
    if (endpoints) {
        const HRESULT countResult = endpoints->GetCount(&endpointCount);
        if (FAILED(countResult) && SUCCEEDED(firstFailure)) {
            firstFailure = countResult;
        }
    }
    activeEndpointCount_.store(endpointCount, std::memory_order_release);
    bool managerStorageReady = true;
    try {
        managers.reserve(endpointCount);
    } catch (...) {
        managerStorageReady = false;
        if (SUCCEEDED(firstFailure)) {
            firstFailure = E_OUTOFMEMORY;
        }
    }
    size_t initialSessionCount = 0;
    size_t registrationFailures = managerStorageReady ? 0 : endpointCount;
    if (notification && managerStorageReady) {
        state_->acceptingNotifications.store(true, std::memory_order_release);
    }
    for (UINT index = 0; managerStorageReady && index < endpointCount; ++index) {
        IMMDevice* endpoint = nullptr;
        IAudioSessionManager2* manager = nullptr;
        IAudioSessionEnumerator* sessionEnumerator = nullptr;
        bool notificationRegistered = false;
        HRESULT endpointResult = endpoints->Item(index, &endpoint);
        if (SUCCEEDED(endpointResult)) {
            endpointResult = endpoint->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                                reinterpret_cast<void**>(&manager));
        }
        if (SUCCEEDED(endpointResult)) {
            endpointResult = manager->RegisterSessionNotification(notification);
            notificationRegistered = SUCCEEDED(endpointResult);
        }
        if (SUCCEEDED(endpointResult)) {
            // Windows requires an initial enumeration after registration before
            // OnSessionCreated notifications are delivered for this manager.
            endpointResult = manager->GetSessionEnumerator(&sessionEnumerator);
        }
        if (SUCCEEDED(endpointResult) && sessionEnumerator) {
            int sessionCount = 0;
            endpointResult = sessionEnumerator->GetCount(&sessionCount);
            if (SUCCEEDED(endpointResult) && sessionCount > 0) {
                initialSessionCount += static_cast<size_t>(sessionCount);
                for (int sessionIndex = 0; sessionIndex < sessionCount; ++sessionIndex) {
                    IAudioSessionControl* session = nullptr;
                    if (SUCCEEDED(sessionEnumerator->GetSession(sessionIndex, &session)) && session) {
                        state_->RememberProcessId(GetAudioSessionProcessId(session));
                    }
                    ReleaseInterface(session);
                }
            }
        }
        if (SUCCEEDED(endpointResult) && sessionEnumerator) {
            managers.push_back(manager);
            manager = nullptr;
        } else {
            ++registrationFailures;
            if (SUCCEEDED(firstFailure)) {
                firstFailure = endpointResult;
            }
        }
        if (manager && notification && notificationRegistered) {
            manager->UnregisterSessionNotification(notification);
        }
        ReleaseInterface(sessionEnumerator);
        ReleaseInterface(manager);
        ReleaseInterface(endpoint);
    }

    registeredEndpointCount_.store(managers.size(), std::memory_order_release);
    registrationFailureCount_.store(registrationFailures, std::memory_order_release);
    existingSessionCount_.store(initialSessionCount, std::memory_order_release);
    firstRegistrationFailure_.store(FAILED(firstFailure) ? firstFailure : S_OK, std::memory_order_release);
    startupResult_.store(
        managers.empty() ? (FAILED(firstFailure) ? firstFailure : HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) : S_OK,
        std::memory_order_release);
    running_.store(!managers.empty(), std::memory_order_release);
    if (managers.empty()) {
        state_->acceptingNotifications.store(false, std::memory_order_release);
    }
    SetEvent(readyEvent_);

    if (!managers.empty()) {
        WaitForSingleObject(stopEvent_, INFINITE);
    }
    state_->acceptingNotifications.store(false, std::memory_order_release);
    running_.store(false, std::memory_order_release);
    for (auto it = managers.rbegin(); it != managers.rend(); ++it) {
        (*it)->UnregisterSessionNotification(notification);
        (*it)->Release();
    }
    if (notification) {
        notification->Release();
    }
    ReleaseInterface(endpoints);
    ReleaseInterface(deviceEnumerator);
    if (SUCCEEDED(coInitResult)) {
        CoUninitialize();
    }
}

}  // namespace ce::process_loopback
