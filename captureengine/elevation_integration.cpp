#include "startup_control.h"
#include "elevation_client.h"
#include "../common/shared_defs.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdio>
#include <new>
#include <tlhelp32.h>

namespace ce::startup {
namespace {
using ce::elevation::Handle;
using Microsoft::WRL::ComPtr;

struct Mapping {
    Handle file;
    void* view = nullptr;
    ~Mapping() {
        if (view)
            UnmapViewOfFile(view);
    }
    bool Create(const wchar_t* name, DWORD bytes) {
        file.Reset(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, bytes, name));
        if (!file || GetLastError() == ERROR_ALREADY_EXISTS)
            return false;
        view = MapViewOfFile(file.Get(), FILE_MAP_ALL_ACCESS, 0, 0, bytes);
        return view != nullptr;
    }
};

bool Unelevated(HANDLE process) {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &raw))
        return false;
    Handle token(raw);
    TOKEN_ELEVATION elevation{};
    DWORD bytes = 0;
    return GetTokenInformation(token.Get(), TokenElevation, &elevation, sizeof(elevation), &bytes) &&
           !elevation.TokenIsElevated;
}

Handle Spawn(const std::wstring& arguments) {
    std::wstring command = ce::elevation::QuoteArgument(ce::elevation::ExecutablePath()) + L" " + arguments;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                        &child))
        return {};
    CloseHandle(child.hThread);
    return Handle(child.hProcess);
}

std::wstring ReadyName(uint32_t parent) {
    return LR"(Local\CE_ElevationIntegration_)" + std::to_wstring(parent);
}

struct Worker {
    Handle process;
    Handle shutdown;
    ~Worker() {
        if (shutdown)
            SetEvent(shutdown.Get());
        if (process && WaitForSingleObject(process.Get(), 15000) != WAIT_OBJECT_0) {
            TerminateProcess(process.Get(), ERROR_TIMEOUT);
            WaitForSingleObject(process.Get(), INFINITE);
        }
    }
};

DWORD Fixture(bool waitForLoss) {
    if (!Unelevated(GetCurrentProcess()))
        return ERROR_ELEVATION_REQUIRED;
    Mapping discovery;
    if (!discovery.Create(SHARED_MEM_DISCOVERY, sizeof(DiscoveryInfo)))
        return ERROR_ALREADY_EXISTS;
    auto* info = new (discovery.view) DiscoveryInfo;
    info->SetAbiSignature(SHARED_MEMORY_ABI_SIGNATURE);
    info->SetInjectPid(GetCurrentProcessId());
    info->SetMagic(DISCOVERY_MAGIC);
    wchar_t name[64]{};
    GenerateSharedMemName(name, std::size(name), GetCurrentProcessId());
    Mapping shared;
    if (!shared.Create(name, sizeof(SharedMemoryLayout)))
        return ERROR_ALREADY_EXISTS;
    auto* memory = new (shared.view) SharedMemoryLayout;
    memory->structSize.store(sizeof(SharedMemoryLayout));
    memory->abiSignature.store(SHARED_MEMORY_ABI_SIGNATURE);
    memory->SetSourcePid(GetCurrentProcessId());
    memory->overlayConfig.frameTimeSource = FrameTimeSource::DisplayChange;
    memory->SetMagic(SHARED_MEMORY_MAGIC);
    Worker worker;
    GenerateShutdownEventName(name, std::size(name), GetCurrentProcessId());
    worker.shutdown.Reset(CreateEventW(nullptr, TRUE, FALSE, name));
    worker.process = Spawn(L"--mode=sensors --parent-pid=" + std::to_wstring(GetCurrentProcessId()));
    if (!worker.process || !Unelevated(worker.process.Get()))
        return ERROR_ACCESS_DENIED;
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"CE_ElevationIntegration";
    RegisterClassW(&windowClass);
    HWND window =
        CreateWindowW(windowClass.lpszClassName, L"CaptureEngine service integration", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                      0, 0, 640, 480, nullptr, nullptr, windowClass.hInstance, nullptr);
    if (!window)
        return GetLastError();
    struct Window {
        HWND value;
        ~Window() {
            DestroyWindow(value);
        }
    } ownedWindow{window};
    DXGI_SWAP_CHAIN_DESC description{};
    description.BufferDesc.Width = 640;
    description.BufferDesc.Height = 480;
    description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 2;
    description.OutputWindow = window;
    description.Windowed = TRUE;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> chain;
    HRESULT result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                                   D3D11_SDK_VERSION, &description, &chain, &device, nullptr, &context);
    if (FAILED(result))
        return static_cast<DWORD>(result);
    ComPtr<ID3D11Texture2D> backBuffer;
    ComPtr<ID3D11RenderTargetView> target;
    if (FAILED(chain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) ||
        FAILED(device->CreateRenderTargetView(backBuffer.Get(), nullptr, &target)))
        return ERROR_NOT_READY;
    const Preferences enabled = ReadPreferences();
    struct RestorePreference {
        Preferences value;
        ~RestorePreference() {
            WritePreferences(value);
        }
    } restore{enabled};
    enum class Stage { Privileged, LocalFallback, Restored };
    Stage stage = Stage::Privileged;
    const uint64_t deadline = GetTickCount64() + 30000;
    while (GetTickCount64() < deadline) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        const float color[] = {0.1f, 0.2f, 0.3f, 1.0f};
        context->ClearRenderTargetView(target.Get(), color);
        if (FAILED(chain->Present(1, 0)))
            return ERROR_NOT_READY;
        if (WaitForSingleObject(worker.process.Get(), 0) != WAIT_TIMEOUT)
            return ERROR_PROCESS_ABORTED;
        if (stage == Stage::LocalFallback) {
            if (memory->systemMetrics.cpuTemperatureC.load() == 0 && memory->displayTiming.writeSequence.load() == 0) {
                if (!WritePreferences(enabled))
                    return ERROR_WRITE_FAULT;
                stage = Stage::Restored;
            }
            continue;
        }
        if (memory->systemMetrics.cpuTemperatureC.load() > 0 && memory->displayTiming.writeSequence.load() > 0) {
            if (stage == Stage::Privileged) {
                Preferences local = enabled;
                local.service = false;
                if (!WritePreferences(local))
                    return ERROR_WRITE_FAULT;
                stage = Stage::LocalFallback;
                continue;
            }
            // The parent uses this event to exercise forced controller loss.
            Handle ready(OpenEventW(EVENT_MODIFY_STATE, FALSE, ReadyName(ce::elevation::ControllerPid()).c_str()));
            if (ready)
                SetEvent(ready.Get());
            if (waitForLoss) {
                Handle parent(OpenProcess(SYNCHRONIZE, FALSE, ce::elevation::ControllerPid()));
                if (!parent)
                    return ERROR_ACCESS_DENIED;
                WaitForSingleObject(parent.Get(), INFINITE);
            }
            return ERROR_SUCCESS;
        }
    }
    return ERROR_TIMEOUT;
}

DWORD ForcedControllerLoss() {
    Handle ready(CreateEventW(nullptr, TRUE, FALSE, ReadyName(GetCurrentProcessId()).c_str()));
    Handle fixture =
        Spawn(L"--ce-service-fixture --ce-fixture-wait --parent-pid=" + std::to_wstring(GetCurrentProcessId()));
    if (!ready || !fixture)
        return ERROR_NOT_READY;
    HANDLE waits[] = {ready.Get(), fixture.Get()};
    const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, 40000);
    std::vector<Handle> workers;
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (snapshot && Process32FirstW(snapshot.Get(), &entry)) {
        do {
            if (entry.th32ParentProcessID == GetProcessId(fixture.Get())) {
                Handle child(OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, entry.th32ProcessID));
                if (child)
                    workers.push_back(std::move(child));
            }
        } while (Process32NextW(snapshot.Get(), &entry));
    }
    Handle service(OpenProcess(SYNCHRONIZE, FALSE, ce::elevation::ServiceProcessId()));
    TerminateProcess(fixture.Get(), ERROR_PROCESS_ABORTED);
    WaitForSingleObject(fixture.Get(), INFINITE);
    DWORD result = wait == WAIT_OBJECT_0 && !workers.empty() ? ERROR_SUCCESS : ERROR_NOT_READY;
    for (auto& worker : workers) {
        if (WaitForSingleObject(worker.Get(), 15000) != WAIT_OBJECT_0) {
            result = ERROR_TIMEOUT;
            TerminateProcess(worker.Get(), result);
            WaitForSingleObject(worker.Get(), INFINITE);
        }
    }
    if (service && WaitForSingleObject(service.Get(), 15000) != WAIT_OBJECT_0)
        result = ERROR_TIMEOUT;
    return result;
}
}  // namespace

DWORD RunElevationFixture(bool waitForLoss) {
    return Fixture(waitForLoss);
}

DWORD CheckServiceStopped() {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    SC_HANDLE service = manager ? OpenServiceW(manager, ce::elevation::kServiceName, SERVICE_QUERY_STATUS) : nullptr;
    if (manager)
        CloseServiceHandle(manager);
    if (!service)
        return GetLastError();
    const bool stopped = ce::elevation::WaitServiceState(service, SERVICE_STOPPED, 15000);
    CloseServiceHandle(service);
    return stopped ? ERROR_SUCCESS : GetLastError();
}

DWORD RunElevationIntegration() {
    if (!Unelevated(GetCurrentProcess()))
        return ERROR_ELEVATION_REQUIRED;
    const Preferences original = ReadPreferences();
    if (original.service || ServiceStatusText() != L"Use elevation service (not installed)")
        return ERROR_ALREADY_EXISTS;
    Preferences enabled = original;
    enabled.service = true;
    const DWORD install = RunIntegrationSetup(true, enabled);
    if (install) {
        std::printf("Elevation integration: setup failed (error=%lu)\n", install);
        std::fflush(stdout);
        return install;
    }
    DWORD result = ERROR_SUCCESS;
    {
        Handle fixture = Spawn(L"--ce-service-fixture");
        if (!fixture || WaitForSingleObject(fixture.Get(), 40000) != WAIT_OBJECT_0) {
            result = ERROR_TIMEOUT;
            if (fixture) {
                TerminateProcess(fixture.Get(), result);
                WaitForSingleObject(fixture.Get(), INFINITE);
            }
        } else
            GetExitCodeProcess(fixture.Get(), &result);
    }
    if (!result)
        result = CheckServiceStopped();
    if (!result)
        result = ForcedControllerLoss();
    if (!result)
        result = CheckServiceStopped();
    const DWORD remove = RunIntegrationSetup(false, original);
    std::printf("Elevation integration: sensors/ordinary worker/display publication=%lu cleanup=%lu\n", result, remove);
    std::fflush(stdout);
    return remove ? remove : result;
}
}  // namespace ce::startup
