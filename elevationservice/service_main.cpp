#include "service_internal.h"
#include "../captureengine/sensor_bridge_host.h"
#include "../common/logging.h"
#include <algorithm>
#include <vector>
#include "../common/elevation_lifetime.h"

namespace ce::elevation {
namespace {
SERVICE_STATUS_HANDLE statusHandle = nullptr;
Handle stopEvent;
Handle clientsChanged;
ClientLifetime clientLifetime;
std::atomic<DWORD> serviceState{SERVICE_START_PENDING};

void PublishStatus(DWORD state, DWORD error = ERROR_SUCCESS) {
    SERVICE_STATUS status{};
    status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState = state;
    status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    status.dwWin32ExitCode = error;
    status.dwWaitHint = state == SERVICE_STOP_PENDING ? 15000 : 0;
    serviceState.store(state);
    SetServiceStatus(statusHandle, &status);
}

DWORD WINAPI Control(DWORD control, DWORD, void*, void*) {
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
        PublishStatus(SERVICE_STOP_PENDING);
        SetEvent(stopEvent.Get());
    }
    return ERROR_SUCCESS;
}

void ServeClient(Handle pipe, std::wstring owner, TraceManager& traces) {
    Handle controller;
    std::unique_ptr<ce::hardware_sensors::LibreHardwareMonitorPlugin> sensors;
    struct TraceLease {
        TraceManager& manager;
        bool acquired = false;
        ~TraceLease() {
            if (acquired)
                manager.Release();
        }
    } trace{traces};
    uint32_t sequence = 0;
    bool authenticated = false;
    while (true) {
        Header request;
        if (!Transfer(pipe.Get(), &request, sizeof(request), false, stopEvent.Get(), controller.Get(),
                      authenticated ? INFINITE : 5000) ||
            !ValidateHeader(request, false) || request.sequence <= sequence)
            break;
        std::array<unsigned char, kMaximumPayload> input{};
        if (request.size && !Transfer(pipe.Get(), input.data(), request.size, false, stopEvent.Get(), controller.Get()))
            break;
        if (!ValidateRequest(request, input.data(), request.size))
            break;
        Header reply = request;
        reply.size = 0;
        sequence = request.sequence;
        SensorSample sample;
        Capabilities capabilities;
        capabilities.serverPid = GetCurrentProcessId();
        const void* output = &sample;
        if (!authenticated) {
            Hello hello{};
            if (request.operation != Operation::Hello)
                break;
            std::memcpy(&hello, input.data(), sizeof(hello));
            if (!AuthenticateClient(pipe.Get(), hello, owner, controller))
                break;
            authenticated = true;
            capabilities.controllerPid = hello.controllerPid;
            reply.size = sizeof(capabilities);
            output = &capabilities;
        } else {
            switch (request.operation) {
                case Operation::SubscribeSensors: {
                    SensorRequest sensorRequest;
                    std::memcpy(&sensorRequest, input.data(), sizeof(sensorRequest));
                    sensors = std::make_unique<ce::hardware_sensors::LibreHardwareMonitorPlugin>(
                        MakeSensorConfig(sensorRequest));
                    if (!sensors->Start()) {
                        sensors.reset();
                        reply.error = ERROR_NOT_READY;
                    }
                    break;
                }
                case Operation::Status:
                    capabilities.controllerPid = GetProcessId(controller.Get());
                    reply.size = sizeof(capabilities);
                    output = &capabilities;
                    break;
                case Operation::SampleSensors:
                    if (!sensors) {
                        reply.error = ERROR_NOT_READY;
                        break;
                    }
                    sensors->Poll();
                    if (!sensors->Start()) {
                        reply.error = ERROR_NOT_READY;
                        break;
                    }
                    sample = MakeSensorSample(sensors->GetSnapshot());
                    reply.size = sizeof(sample);
                    break;
                case Operation::AcquireTrace:
                    if (!trace.acquired) {
                        reply.error = traces.Acquire();
                        trace.acquired = reply.error == ERROR_SUCCESS;
                    }
                    break;
                case Operation::ReleaseTrace:
                    if (trace.acquired) {
                        traces.Release();
                        trace.acquired = false;
                    }
                    break;
                default:
                    reply.error = ERROR_INVALID_FUNCTION;
                    break;
            }
        }
        if (!Transfer(pipe.Get(), &reply, sizeof(reply), true, stopEvent.Get(), controller.Get()) ||
            (reply.size &&
             !Transfer(pipe.Get(), const_cast<void*>(output), reply.size, true, stopEvent.Get(), controller.Get())))
            break;
    }
    sensors.reset();
    DisconnectNamedPipe(pipe.Get());
}

void WINAPI ServiceMain(DWORD, wchar_t**) {
    stopEvent.Reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    clientsChanged.Reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    statusHandle = RegisterServiceCtrlHandlerExW(kServiceName, Control, nullptr);
    if (!statusHandle)
        return;
    const std::wstring owner = InstalledOwner();
    if (!stopEvent || !clientsChanged || owner.empty()) {
        PublishStatus(SERVICE_STOPPED, ERROR_INVALID_DATA);
        return;
    }
    Security security(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x12019b;;;" + owner + L")");
    if (!security.Get()) {
        PublishStatus(SERVICE_STOPPED, ERROR_INVALID_SECURITY_DESCR);
        return;
    }
    // Ordinary clients observe service death without receiving process-control rights.
    Security processSecurity(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x101000;;;" + owner + L")");
    Handle process(OpenProcess(WRITE_DAC, FALSE, GetCurrentProcessId()));
    if (!process || !processSecurity.Descriptor() ||
        !SetKernelObjectSecurity(process.Get(), DACL_SECURITY_INFORMATION, processSecurity.Descriptor())) {
        PublishStatus(SERVICE_STOPPED, ERROR_ACCESS_DENIED);
        return;
    }
    process.Reset();
    DWORD error = ERROR_SUCCESS;
    {
        TraceManager traces(owner);
        struct ClientThread {
            std::thread thread;
            std::shared_ptr<std::atomic<bool>> done;
        };
        std::vector<ClientThread> clients;
        clients.reserve(16);
        bool accepted = false;
        bool firstPipe = true;
        const uint64_t firstClientDeadline = GetTickCount64() + 15000;
        while (WaitForSingleObject(stopEvent.Get(), 0) == WAIT_TIMEOUT) {
            for (auto it = clients.begin(); it != clients.end();) {
                if (it->done->load(std::memory_order_acquire)) {
                    it->thread.join();
                    it = clients.erase(it);
                } else
                    ++it;
            }
            if (clientLifetime.Count() >= 16) {
                HANDLE capacity[] = {stopEvent.Get(), clientsChanged.Get()};
                if (WaitForMultipleObjects(2, capacity, FALSE, INFINITE) == WAIT_OBJECT_0)
                    break;
                continue;
            }
            Handle pipe(CreateNamedPipeW(
                kPipeName, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (firstPipe ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 16, 8192, 8192, 5000, security.Get()));
            if (!pipe) {
                error = GetLastError();
                break;
            }
            firstPipe = false;
            if (serviceState.load() == SERVICE_START_PENDING)
                PublishStatus(SERVICE_RUNNING);
            Handle connected(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            OVERLAPPED operation{};
            operation.hEvent = connected.Get();
            const BOOL result = ConnectNamedPipe(pipe.Get(), &operation);
            const DWORD connectError = result ? ERROR_SUCCESS : GetLastError();
            if (connectError == ERROR_PIPE_CONNECTED)
                SetEvent(connected.Get());
            else if (connectError != ERROR_IO_PENDING && !result) {
                error = connectError;
                break;
            }
            HANDLE waits[] = {stopEvent.Get(), connected.Get(), clientsChanged.Get()};
            bool connectedClient = false;
            while (true) {
                const uint64_t now = GetTickCount64();
                const DWORD timeout = accepted                    ? INFINITE
                                      : now < firstClientDeadline ? static_cast<DWORD>(firstClientDeadline - now)
                                                                  : 0;
                const DWORD wait = WaitForMultipleObjects(3, waits, FALSE, timeout);
                if (wait == WAIT_OBJECT_0 + 1) {
                    connectedClient = true;
                    break;
                }
                if (wait == WAIT_OBJECT_0 + 2 && clientLifetime.Count() != 0)
                    continue;
                SetEvent(stopEvent.Get());
                break;
            }
            if (!connectedClient) {
                CancelIoEx(pipe.Get(), &operation);
                DWORD ignored = 0;
                GetOverlappedResult(pipe.Get(), &operation, &ignored, TRUE);
                break;
            }
            accepted = true;
            if (!clientLifetime.Admit())
                break;
            try {
                auto done = std::make_shared<std::atomic<bool>>(false);
                clients.push_back({std::thread([client = std::move(pipe), owner, &traces, done]() mutable {
                                       try {
                                           ServeClient(std::move(client), owner, traces);
                                       } catch (...) {
                                           LogError(
                                               "[ElevationService] Client resources released after worker failure");
                                       }
                                       if (clientLifetime.Release())
                                           SetEvent(stopEvent.Get());
                                       done->store(true, std::memory_order_release);
                                       SetEvent(clientsChanged.Get());
                                   }),
                                   done});
            } catch (...) {
                if (clientLifetime.Release())
                    SetEvent(stopEvent.Get());
                error = ERROR_NOT_ENOUGH_MEMORY;
                break;
            }
        }
        PublishStatus(SERVICE_STOP_PENDING);
        SetEvent(stopEvent.Get());
        for (auto& client : clients)
            if (client.thread.joinable())
                client.thread.join();
    }  // Destroy trace manager and revoke ETW access before publishing STOPPED.
    PublishStatus(SERVICE_STOPPED, error);
}
}  // namespace
}  // namespace ce::elevation

int WINAPI wWinMain(HINSTANCE, HINSTANCE, wchar_t*, int) {
    if (const auto result = ce::hardware_sensors::TryRunSensorBridgeHost())
        return *result;
    SERVICE_TABLE_ENTRYW table[] = {{const_cast<wchar_t*>(ce::elevation::kServiceName), ce::elevation::ServiceMain},
                                    {nullptr, nullptr}};
    return StartServiceCtrlDispatcherW(table) ? 0 : static_cast<int>(GetLastError());
}
