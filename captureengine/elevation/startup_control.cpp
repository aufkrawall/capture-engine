#include "startup_control.h"
#include "elevation_client.h"
#include "common/setup/installer_setup_policy.h"
#include "common/logging/logging.h"
#include <shellapi.h>
#include <bcrypt.h>
#include <tlhelp32.h>
#include <filesystem>
#include <thread>
#include <atomic>

namespace ce::startup {
namespace {
using ce::elevation::Handle;
using ce::elevation::Transfer;
enum class Action : uint32_t { Launch, InstallService, RemoveService, Autostart };
struct Context {
    uint32_t magic = 0x31534343;
    Action action = Action::Launch;
    uint32_t flags = 0;
    uint32_t administrator = 0;
    std::array<wchar_t, 192> owner{};
};
bool ownerAdministrator = false;
std::atomic<bool> busy{false};
std::atomic<bool> complete{false};
std::atomic<DWORD> actionResult{ERROR_SUCCESS};
std::thread actionWorker;
constexpr wchar_t kContextPrefix[] = L"\\\\.\\pipe\\CE_StartupContext_";

uint32_t Encode(const Preferences& preferences) {
    return (preferences.service ? 1u : 0u) | (preferences.elevated ? 2u : 0u) | (preferences.autostart ? 4u : 0u);
}
Preferences Decode(uint32_t flags) {
    return {(flags & 1) != 0, (flags & 2) != 0, (flags & 4) != 0};
}

std::wstring Argument(std::wstring_view prefix) {
    int count = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    std::wstring result;
    if (arguments) {
        for (int index = 1; index < count; ++index) {
            const std::wstring_view value(arguments[index]);
            if (value.substr(0, prefix.size()) == prefix)
                result.assign(value.substr(prefix.size()));
        }
        LocalFree(arguments);
    }
    return result;
}

bool SameExecutable(HANDLE process) {
    std::wstring path(32768, L'\0');
    DWORD length = static_cast<DWORD>(path.size());
    return QueryFullProcessImageNameW(process, 0, path.data(), &length) &&
           _wcsicmp(path.substr(0, length).c_str(), ce::elevation::ExecutablePath().c_str()) == 0;
}

DWORD ReceiveContext(Context& context) {
    const std::wstring name = Argument(L"--ce-context=");
    if (name.size() != wcslen(kContextPrefix) + 32 || name.substr(0, wcslen(kContextPrefix)) != kContextPrefix)
        return ERROR_INVALID_PARAMETER;
    for (wchar_t character : name.substr(wcslen(kContextPrefix))) {
        if (!((character >= L'0' && character <= L'9') || (character >= L'a' && character <= L'f')))
            return ERROR_INVALID_PARAMETER;
    }
    Handle pipe(CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                            FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
    ULONG serverPid = 0;
    if (!pipe || !GetNamedPipeServerProcessId(pipe.Get(), &serverPid))
        return ERROR_ACCESS_DENIED;
    Handle server(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, serverPid));
    bool administrator = false;
    const std::wstring serverSid = server ? ce::elevation::ProcessUserSid(server.Get(), &administrator) : L"";
    if (!server || !SameExecutable(server.Get()) || serverSid.empty() ||
        !Transfer(pipe.Get(), &context, sizeof(context), false, nullptr, server.Get()))
        return ERROR_ACCESS_DENIED;
    const auto* terminator = std::wmemchr(context.owner.data(), 0, context.owner.size());
    if (context.magic != 0x31534343 || context.action > Action::Autostart || context.flags > 7 ||
        context.administrator > 1 || !terminator)
        return ERROR_INVALID_DATA;
    PSID parsed = nullptr;
    if (!ConvertStringSidToSidW(context.owner.data(), &parsed))
        return ERROR_INVALID_SID;
    LocalFree(parsed);
    if (serverSid != context.owner.data()) {
        HANDLE raw = nullptr;
        if (!OpenProcessToken(server.Get(), TOKEN_QUERY, &raw))
            return ERROR_ACCESS_DENIED;
        Handle token(raw);
        TOKEN_ELEVATION elevation{};
        DWORD bytes = 0;
        if (!GetTokenInformation(token.Get(), TokenElevation, &elevation, sizeof(elevation), &bytes) ||
            !elevation.TokenIsElevated)
            return ERROR_ACCESS_DENIED;
    } else
        context.administrator = administrator ? 1 : 0;
    SetOwnerSid(context.owner.data());
    ownerAdministrator = context.administrator != 0;
    DWORD ack = context.magic;
    if (!Transfer(pipe.Get(), &ack, sizeof(ack), true, nullptr, server.Get()))
        return ERROR_BROKEN_PIPE;
    return ERROR_SUCCESS;
}

std::wstring RawArguments() {
    std::wstring_view command(GetCommandLineW());
    size_t end = 0;
    if (!command.empty() && command.front() == L'"') {
        end = command.find(L'"', 1);
        if (end != std::wstring_view::npos)
            ++end;
    } else
        end = command.find_first_of(L" \t");
    return end == std::wstring_view::npos ? L"" : std::wstring(command.substr(end));
}

DWORD LaunchRole(Action action, const Preferences& desired) {
    struct Apartment {
        HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        ~Apartment() {
            if (SUCCEEDED(result))
                CoUninitialize();
        }
    } apartment;
    if (FAILED(apartment.result) && apartment.result != RPC_E_CHANGED_MODE)
        return static_cast<DWORD>(apartment.result);
    Context context;
    context.action = action;
    context.flags = Encode(desired);
    context.administrator = ownerAdministrator ? 1 : 0;
    const std::wstring owner = OwnerSid();
    if (owner.size() >= context.owner.size())
        return ERROR_INVALID_SID;
    std::copy(owner.begin(), owner.end(), context.owner.begin());
    std::array<unsigned char, 16> random{};
    if (BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) !=
        0)
        return ERROR_GEN_FAILURE;
    std::wstring name = kContextPrefix;
    constexpr wchar_t hex[] = L"0123456789abcdef";
    for (unsigned char value : random) {
        name.push_back(hex[value >> 4]);
        name.push_back(hex[value & 15]);
    }
    ce::elevation::Security security(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;" + owner + L")");
    if (!security.Get())
        return ERROR_INVALID_SECURITY_DESCR;
    Handle pipe(
        CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                         PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 5000, security.Get()));
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!pipe || !event)
        return GetLastError();
    OVERLAPPED connection{};
    connection.hEvent = event.Get();
    if (!ConnectNamedPipe(pipe.Get(), &connection) && GetLastError() != ERROR_IO_PENDING)
        return GetLastError();
    const std::wstring executable = ce::elevation::ExecutablePath();
    const std::wstring directory = std::filesystem::path(executable).parent_path().wstring();
    const std::wstring arguments = (action == Action::Launch ? L"" : L"--ce-setup ") + std::wstring(L"--ce-context=") +
                                   ce::elevation::QuoteArgument(name) +
                                   (action == Action::Launch ? L" " + RawArguments() : L"");
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    info.lpVerb = L"runas";
    info.lpFile = executable.c_str();
    info.lpParameters = arguments.c_str();
    info.lpDirectory = directory.c_str();
    info.nShow = action == Action::Launch ? SW_SHOWNORMAL : SW_HIDE;
    if (!ShellExecuteExW(&info) || !info.hProcess) {
        const DWORD error = GetLastError();
        CancelIoEx(pipe.Get(), &connection);
        DWORD ignored = 0;
        GetOverlappedResult(pipe.Get(), &connection, &ignored, TRUE);
        return error;
    }
    Handle child(info.hProcess);
    HANDLE waits[] = {event.Get(), child.Get()};
    const DWORD connectionResult = WaitForMultipleObjects(2, waits, FALSE, 10000);
    if (connectionResult != WAIT_OBJECT_0) {
        DWORD result = ERROR_TIMEOUT;
        if (connectionResult == WAIT_OBJECT_0 + 1)
            GetExitCodeProcess(child.Get(), &result);
        if (result == ERROR_SUCCESS)
            result = ERROR_BROKEN_PIPE;
        CancelIoEx(pipe.Get(), &connection);
        DWORD ignored = 0;
        GetOverlappedResult(pipe.Get(), &connection, &ignored, TRUE);
        return result;
    }
    ULONG childPid = 0;
    DWORD ack = 0;
    if (!GetNamedPipeClientProcessId(pipe.Get(), &childPid) || childPid != GetProcessId(child.Get()) ||
        !Transfer(pipe.Get(), &context, sizeof(context), true, nullptr, child.Get()) ||
        !Transfer(pipe.Get(), &ack, sizeof(ack), false, nullptr, child.Get()) || ack != context.magic)
        return ERROR_ACCESS_DENIED;
    DisconnectNamedPipe(pipe.Get());
    pipe.Reset();
    if (action == Action::Launch)
        return ERROR_SUCCESS;
    WaitForSingleObject(child.Get(), INFINITE);
    DWORD result = ERROR_GEN_FAILURE;
    GetExitCodeProcess(child.Get(), &result);
    return result;
}

DWORD Apply(Action action, const Preferences& desired) {
    const Preferences old = ReadPreferences();
    unsigned long rollbackError = ERROR_SUCCESS;
    const DWORD error = ApplyTransaction(
        [&] {
            return action == Action::InstallService  ? InstallElevationService()
                   : action == Action::RemoveService ? RemoveElevationService()
                                                     : ConfigureAutostart(desired, ownerAdministrator);
        },
        [&] { return WritePreferences(desired); },
        [&] {
            return action == Action::InstallService || action == Action::RemoveService
                       ? old.service ? InstallElevationService() : RemoveElevationService()
                       : ConfigureAutostart(old, ownerAdministrator);
        },
        ERROR_WRITE_FAULT, rollbackError);
    if (rollbackError)
        LogError("[Startup] Could not roll back setup (error=%lu)", rollbackError);
    return error;
}

// The installer runs this role elevated and waits for it. Anyone able to start an
// elevated process may already do everything it does, so what must be refused is an
// unelevated caller steering an elevated copy through a consent prompt: the parent
// has to be a live, older, elevated process.
bool ParentIsLiveElevatedProcess() {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot)
        return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    const DWORD self = GetCurrentProcessId();
    DWORD parentId = 0;
    for (BOOL more = Process32FirstW(snapshot.Get(), &entry); more; more = Process32NextW(snapshot.Get(), &entry)) {
        if (entry.th32ProcessID == self) {
            parentId = entry.th32ParentProcessID;
            break;
        }
    }
    if (!parentId)
        return false;
    Handle parent(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, parentId));
    FILETIME parentCreated{}, ownCreated{}, exited{}, kernel{}, user{};
    if (!parent || !GetProcessTimes(parent.Get(), &parentCreated, &exited, &kernel, &user) ||
        !GetProcessTimes(GetCurrentProcess(), &ownCreated, &exited, &kernel, &user))
        return false;
    // A recycled parent id must not identify a process that started later.
    if (CompareFileTime(&parentCreated, &ownCreated) >= 0 || WaitForSingleObject(parent.Get(), 0) != WAIT_TIMEOUT)
        return false;
    HANDLE raw = nullptr;
    if (!OpenProcessToken(parent.Get(), TOKEN_QUERY, &raw))
        return false;
    Handle token(raw);
    TOKEN_ELEVATION elevation{};
    DWORD bytes = 0;
    return GetTokenInformation(token.Get(), TokenElevation, &elevation, sizeof(elevation), &bytes) &&
           elevation.TokenIsElevated != 0;
}
}  // namespace

std::optional<int> TryRunInstallerSetup() {
    int count = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments || count < 2 || wcscmp(arguments[1], kInstallerSetupArgument) != 0) {
        if (arguments)
            LocalFree(reinterpret_cast<HLOCAL>(arguments));
        return std::nullopt;
    }
    const std::vector<std::wstring> rest(arguments + 2, arguments + count);
    LocalFree(reinterpret_cast<HLOCAL>(arguments));
    if (!ce::elevation::IsElevated())
        return ERROR_ELEVATION_REQUIRED;
    InstallerSetupRequest request;
    if (!ParseInstallerSetupArguments(rest, &request))
        return ERROR_INVALID_PARAMETER;
    if (!ParentIsLiveElevatedProcess())
        return ERROR_ACCESS_DENIED;
    PSID parsed = nullptr;
    if (!ConvertStringSidToSidW(request.ownerSid.c_str(), &parsed))
        return ERROR_INVALID_SID;
    LocalFree(parsed);
    SetOwnerSid(request.ownerSid);
    ownerAdministrator = request.ownerAdministrator;
    DWORD error = ERROR_SUCCESS;
    if (request.service != ServiceStep::Keep)
        error = Apply(request.service == ServiceStep::Install ? Action::InstallService : Action::RemoveService,
                      request.preferences);
    if (error == ERROR_SUCCESS && request.applyAutostart)
        error = Apply(Action::Autostart, request.preferences);
    if (error == ERROR_SUCCESS && request.service == ServiceStep::Keep && !request.applyAutostart &&
        !WritePreferences(request.preferences))
        error = ERROR_WRITE_FAULT;
    return static_cast<int>(error);
}

std::optional<int> TryRunSetup() {
    if (const auto installer = TryRunInstallerSetup())
        return installer;
    int count = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    const bool setup = arguments && count >= 2 && wcscmp(arguments[1], L"--ce-setup") == 0;
    if (arguments)
        LocalFree(arguments);
    if (!setup)
        return std::nullopt;
    if (!ce::elevation::IsElevated())
        return ERROR_ELEVATION_REQUIRED;
    Context context;
    const DWORD error = ReceiveContext(context);
    if (error != ERROR_SUCCESS || context.action == Action::Launch)
        return error ? error : ERROR_INVALID_PARAMETER;
    return static_cast<int>(Apply(context.action, Decode(context.flags)));
}

std::optional<int> Bootstrap(bool controller) {
    if (!controller)
        return std::nullopt;
    if (!Argument(L"--ce-context=").empty()) {
        Context context;
        const DWORD error = ReceiveContext(context);
        if (error != ERROR_SUCCESS || context.action != Action::Launch || !ce::elevation::IsElevated())
            return error ? static_cast<int>(error) : ERROR_ACCESS_DENIED;
    } else {
        SetOwnerSid(ce::elevation::ProcessUserSid(GetCurrentProcess(), &ownerAdministrator));
    }
    const auto directory = std::filesystem::path(ce::elevation::ExecutablePath()).parent_path().wstring();
    SetCurrentDirectoryW(directory.c_str());
    const Preferences preferences = ReadPreferences();
    if (!ShouldRequestElevation(true, ce::elevation::IsElevated(), preferences))
        return std::nullopt;
    const DWORD error = LaunchRole(Action::Launch, preferences);
    if (error != ERROR_SUCCESS && error != ERROR_CANCELLED)
        MessageBoxW(nullptr, L"CaptureEngine could not request administrator privileges. This launch was canceled.",
                    L"CaptureEngine", MB_OK | MB_ICONERROR);
    return error == ERROR_SUCCESS ? 0 : static_cast<int>(error);
}

DWORD RunIntegrationSetup(bool install, const Preferences& preferences) {
    SetOwnerSid(ce::elevation::ProcessUserSid(GetCurrentProcess(), &ownerAdministrator));
    return LaunchRole(install ? Action::InstallService : Action::RemoveService, preferences);
}

bool Busy() {
    return busy.load(std::memory_order_acquire);
}

void Toggle(Setting setting) {
    if (Busy())
        return;
    if (actionWorker.joinable())
        actionWorker.join();
    const Preferences old = ReadPreferences();
    Preferences desired = old;
    const Preferences displayed = DisplayPreferences();
    if (setting == Setting::Service)
        desired.service = !displayed.service;
    if (setting == Setting::Elevation)
        desired.elevated = !desired.elevated;
    if (setting == Setting::Autostart)
        desired.autostart = !displayed.autostart;
    busy.store(true, std::memory_order_release);
    complete.store(false, std::memory_order_release);
    try {
        actionWorker = std::thread([old, desired, setting] {
            DWORD result = ERROR_SUCCESS;
            try {
                const Action action = setting == Setting::Service
                                          ? desired.service ? Action::InstallService : Action::RemoveService
                                          : Action::Autostart;
                if (setting == Setting::Elevation && !old.autostart) {
                    if (!WritePreferences(desired))
                        result = ERROR_WRITE_FAULT;
                } else {
                    const bool elevatedAction =
                        setting == Setting::Service ||
                        SelectRegistration(old, ownerAdministrator) == Registration::ElevatedTask ||
                        SelectRegistration(desired, ownerAdministrator) == Registration::ElevatedTask;
                    result = elevatedAction && !ce::elevation::IsElevated() ? LaunchRole(action, desired)
                                                                            : Apply(action, desired);
                }
            } catch (...) {
                result = ERROR_NOT_ENOUGH_MEMORY;
            }
            actionResult.store(result, std::memory_order_relaxed);
            complete.store(true, std::memory_order_release);
        });
    } catch (...) {
        actionResult.store(ERROR_NOT_ENOUGH_MEMORY);
        complete.store(true, std::memory_order_release);
    }
}

void Pump() {
    if (!complete.exchange(false, std::memory_order_acq_rel))
        return;
    if (actionWorker.joinable())
        actionWorker.join();
    busy.store(false, std::memory_order_release);
    const DWORD result = actionResult.load();
    if (result == ERROR_SUCCESS)
        LogInfo("[Startup] Tray preference applied; process elevation changes on the next launch");
    else if (result == ERROR_CANCELLED)
        LogInfo("[Startup] Setup canceled; previous preferences retained");
    else {
        LogWarn("[Startup] Setup failed (error=%lu); previous preferences retained", result);
        const std::wstring message = L"The setting could not be applied (Windows error " + std::to_wstring(result) +
                                     L"). The previous preference was retained.";
        MessageBoxW(nullptr, message.c_str(), L"CaptureEngine", MB_OK | MB_ICONERROR);
    }
}

void Shutdown() {
    if (actionWorker.joinable())
        actionWorker.join();
    complete.store(false);
    busy.store(false);
}

Preferences DisplayPreferences() {
    Preferences preferences = ReadPreferences();
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    SC_HANDLE service = manager ? OpenServiceW(manager, ce::elevation::kServiceName, SERVICE_QUERY_STATUS) : nullptr;
    preferences.service = preferences.service && service != nullptr;
    if (service)
        CloseServiceHandle(service);
    if (manager)
        CloseServiceHandle(manager);
    preferences.autostart = preferences.autostart && AutostartMatches(preferences, ownerAdministrator);
    return preferences;
}

std::wstring ServiceStatusText() {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    SC_HANDLE service = manager ? OpenServiceW(manager, ce::elevation::kServiceName, SERVICE_QUERY_STATUS) : nullptr;
    SERVICE_STATUS_PROCESS status{};
    DWORD bytes = 0;
    const bool exists = service && QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                                                        reinterpret_cast<BYTE*>(&status), sizeof(status), &bytes);
    if (service)
        CloseServiceHandle(service);
    if (manager)
        CloseServiceHandle(manager);
    if (!exists)
        return L"Use elevation service (not installed)";
    return status.dwCurrentState == SERVICE_RUNNING ? L"Use elevation service (running)"
                                                    : L"Use elevation service (installed, stopped)";
}
}  // namespace ce::startup
