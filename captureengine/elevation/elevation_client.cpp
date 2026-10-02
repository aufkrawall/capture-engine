#include "elevation_client.h"
#include "startup_preferences.h"
#include "common/logging/logging.h"
#include <shellapi.h>
#include <atomic>

namespace ce::elevation {

bool ServiceEnabled() {
    return ce::startup::ReadPreferences().service;
}

uint32_t ServiceProcessId() {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    SC_HANDLE service = manager ? OpenServiceW(manager, kServiceName, SERVICE_QUERY_STATUS) : nullptr;
    SERVICE_STATUS_PROCESS status{};
    DWORD size = 0;
    const bool running = service &&
                         QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&status),
                                              sizeof(status), &size) &&
                         status.dwCurrentState == SERVICE_RUNNING;
    if (service)
        CloseServiceHandle(service);
    if (manager)
        CloseServiceHandle(manager);
    return running ? status.dwProcessId : 0;
}

uint32_t ControllerPid() {
    int count = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    uint32_t result = GetCurrentProcessId();
    if (arguments) {
        for (int index = 1; index < count; ++index) {
            const std::wstring_view argument(arguments[index]);
            constexpr std::wstring_view prefix = L"--parent-pid=";
            if (argument.substr(0, prefix.size()) == prefix) {
                uint64_t number = 0;
                const auto value = argument.substr(prefix.size());
                for (wchar_t character : value) {
                    if (character < L'0' || character > L'9' || number > UINT32_MAX / 10) {
                        number = 0;
                        break;
                    }
                    number = number * 10 + static_cast<unsigned>(character - L'0');
                }
                if (number && number <= UINT32_MAX)
                    result = static_cast<uint32_t>(number);
            }
        }
        LocalFree(arguments);
    }
    return result;
}

bool WaitServiceState(SC_HANDLE service, DWORD state, DWORD timeoutMs) {
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event)
        return false;
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    SC_HANDLE observed = manager ? OpenServiceW(manager, kServiceName, SERVICE_QUERY_STATUS) : nullptr;
    if (manager)
        CloseServiceHandle(manager);
    if (!observed)
        return false;
    SERVICE_NOTIFYW notification{};
    const auto observe = [&]() {
        const uint64_t deadline = GetTickCount64() + timeoutMs;
        while (GetTickCount64() < deadline) {
            SERVICE_STATUS_PROCESS status{};
            DWORD bytes = 0;
            if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&status), sizeof(status),
                                      &bytes))
                return false;
            if (status.dwCurrentState == state)
                return true;
            if (state == SERVICE_RUNNING && status.dwCurrentState == SERVICE_STOPPED) {
                SetLastError(status.dwWin32ExitCode ? status.dwWin32ExitCode : ERROR_SERVICE_NOT_ACTIVE);
                return false;
            }
            notification = {};
            notification.dwVersion = SERVICE_NOTIFY_STATUS_CHANGE;
            notification.pContext = event.Get();
            notification.pfnNotifyCallback = [](void* context) {
                auto* notified = static_cast<SERVICE_NOTIFYW*>(context);
                SetEvent(static_cast<HANDLE>(notified->pContext));
            };
            const DWORD mask =
                state == SERVICE_RUNNING ? SERVICE_NOTIFY_RUNNING | SERVICE_NOTIFY_STOPPED : SERVICE_NOTIFY_STOPPED;
            ResetEvent(event.Get());
            const DWORD registered = NotifyServiceStatusChangeW(observed, mask, &notification);
            if (registered != ERROR_SUCCESS) {
                SetLastError(registered);
                return false;
            }
            // Notifications are APCs on this thread. Keep notification storage alive until delivered.
            DWORD wait = WAIT_IO_COMPLETION;
            while (wait == WAIT_IO_COMPLETION) {
                const uint64_t now = GetTickCount64();
                wait =
                    WaitForSingleObjectEx(event.Get(), now < deadline ? static_cast<DWORD>(deadline - now) : 0, TRUE);
            }
            if (wait != WAIT_OBJECT_0) {
                // Closing the registration handle cancels any still-pending notification.
                // Use a dedicated handle, so callers retain their management handle.
                SetLastError(ERROR_TIMEOUT);
                return false;
            }
            if (notification.pszServiceNames)
                LocalFree(notification.pszServiceNames);
            if (notification.dwNotificationStatus != ERROR_SUCCESS) {
                SetLastError(notification.dwNotificationStatus);
                return false;
            }
        }
        SetLastError(ERROR_TIMEOUT);
        return false;
    };
    const bool result = observe();
    const DWORD error = result ? ERROR_SUCCESS : GetLastError();
    CloseServiceHandle(observed);
    while (WaitForSingleObjectEx(event.Get(), 0, TRUE) == WAIT_IO_COMPLETION) {}
    SetLastError(error);
    return result;
}

bool Client::Connect(uint32_t controllerPid) {
    if (Connected())
        return true;
    auto open = [&] {
        return CreateFileW(kPipeName, FILE_READ_DATA | FILE_WRITE_DATA | SYNCHRONIZE, 0, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
    };
    pipe_.Reset(open());
    if (!pipe_) {
        SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!manager)
            return false;
        SC_HANDLE service = OpenServiceW(manager, kServiceName, SERVICE_START | SERVICE_QUERY_STATUS);
        CloseServiceHandle(manager);
        if (!service)
            return false;
        bool started = StartServiceW(service, 0, nullptr) != FALSE;
        if (!started && GetLastError() == ERROR_SERVICE_ALREADY_RUNNING)
            started = true;
        if (started)
            started = WaitServiceState(service, SERVICE_RUNNING, 5000);
        CloseServiceHandle(service);
        if (!started)
            return false;
        pipe_.Reset(open());
    }
    if (!pipe_)
        return false;
    // A spoofed endpoint must not receive requests. SCM supplies the genuine server PID.
    ULONG serverPid = 0;
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    SC_HANDLE service = manager ? OpenServiceW(manager, kServiceName, SERVICE_QUERY_STATUS) : nullptr;
    SERVICE_STATUS_PROCESS status{};
    DWORD bytes = 0;
    const bool genuine = service && GetNamedPipeServerProcessId(pipe_.Get(), &serverPid) &&
                         QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&status),
                                              sizeof(status), &bytes) &&
                         status.dwCurrentState == SERVICE_RUNNING && status.dwProcessId == serverPid;
    if (service)
        CloseServiceHandle(service);
    if (manager)
        CloseServiceHandle(manager);
    if (!genuine) {
        Disconnect();
        return false;
    }
    serverProcess_.Reset(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, serverPid));
    if (!serverProcess_) {
        Disconnect();
        return false;
    }
    Hello hello{controllerPid, 0};
    DWORD sessionId = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &sessionId);
    hello.sessionId = sessionId;
    Capabilities capabilities;
    if (!Exchange(Operation::Hello, &hello, sizeof(hello), &capabilities, sizeof(capabilities)) ||
        capabilities.protocolVersion != kProtocolVersion || capabilities.serverPid != serverPid ||
        capabilities.controllerPid != controllerPid ||
        (capabilities.features & (kSensorCapability | kDisplayTraceCapability)) !=
            (kSensorCapability | kDisplayTraceCapability)) {
        Disconnect();
        return false;
    }
    return true;
}

bool Client::Exchange(Operation operation, const void* input, uint32_t inputSize, void* output, uint32_t outputSize) {
    if (!pipe_)
        return false;
    Header request;
    request.operation = operation;
    request.size = inputSize;
    if (++sequence_ == 0)
        ++sequence_;
    request.sequence = sequence_;
    Header reply;
    const bool transferred = Transfer(pipe_.Get(), &request, sizeof(request), true) &&
                             (!inputSize || Transfer(pipe_.Get(), const_cast<void*>(input), inputSize, true)) &&
                             Transfer(pipe_.Get(), &reply, sizeof(reply), false);
    if (!transferred || !ValidateHeader(reply, true) || reply.operation != operation ||
        reply.sequence != request.sequence || reply.error != ERROR_SUCCESS || reply.size != outputSize ||
        (outputSize && !Transfer(pipe_.Get(), output, outputSize, false))) {
        const DWORD error = !transferred                        ? GetLastError()
                            : reply.error                       ? reply.error
                            : reply.version != kProtocolVersion ? ERROR_REVISION_MISMATCH
                                                                : ERROR_INVALID_DATA;
        static std::atomic<uint64_t> nextDiagnostic{0};
        const uint64_t now = GetTickCount64();
        uint64_t next = nextDiagnostic.load(std::memory_order_relaxed);
        if (now >= next && nextDiagnostic.compare_exchange_strong(next, now + 30000))
            LogWarn("[ElevationService] Request failed (operation=%u error=%lu protocol=%u expected=%u)",
                    static_cast<unsigned>(operation), error, reply.version, kProtocolVersion);
        Disconnect();
        return false;
    }
    return true;
}

bool Client::Subscribe(const SensorRequest& request) {
    return Exchange(Operation::SubscribeSensors, &request, sizeof(request), nullptr, 0);
}
bool Client::Sample(SensorSample& sample) {
    return Exchange(Operation::SampleSensors, nullptr, 0, &sample, sizeof(sample));
}
bool Client::AcquireTrace() {
    if (trace_)
        return true;
    trace_ = Exchange(Operation::AcquireTrace, nullptr, 0, nullptr, 0);
    return trace_;
}
void Client::ReleaseTrace() {
    if (trace_)
        Exchange(Operation::ReleaseTrace, nullptr, 0, nullptr, 0);
    trace_ = false;
}
}  // namespace ce::elevation
